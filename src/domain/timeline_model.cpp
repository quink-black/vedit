#include "timeline_model.h"

#include "media_library.h"
#include "util/logger.h"

#include <algorithm>
#include <cstdio>

namespace vedit {

namespace {

// Property name stamped on each cut producer to carry our stable
// ClipId across playlist reindexing. Underscore prefix keeps MLT from
// serializing it to XML (we serialize our own ids in T10).
constexpr const char* kClipIdProp = "_vedit_clip_id";
constexpr const char* kTrackIdProp = "_vedit_track_id";
constexpr const char* kMediaIdxProp = "_vedit_media_idx";

int64_t read_clip_id(Mlt::Producer& cut) {
    return cut.get_int(kClipIdProp);
}

void stamp_clip_id(Mlt::Producer& cut, int64_t id) {
    cut.set(kClipIdProp, static_cast<int>(id));
}

int read_media_index(Mlt::Producer& cut) {
    return cut.get_int(kMediaIdxProp);
}

void stamp_media_index(Mlt::Producer& cut, int idx) {
    cut.set(kMediaIdxProp, idx);
}

} // namespace

TimelineModel::TimelineModel(Mlt::Profile& profile, MediaLibrary& library)
    : profile_(profile), library_(library) {
    tractor_ = std::make_unique<Mlt::Tractor>(profile_);
    // tractor_->multitrack() returns a pointer; wrap the dereferenced
    // service so Multitrack shares the underlying mlt object with the
    // tractor (no copy).
    Mlt::Multitrack* mt = tractor_->multitrack();
    multitrack_ = std::make_unique<Mlt::Multitrack>(*mt);
    delete mt;
}

TimelineModel::~TimelineModel() = default;

TrackId TimelineModel::next_track_id() {
    return TrackId{next_track_id_++};
}

ClipId TimelineModel::next_clip_id() {
    return ClipId{next_clip_id_++};
}

TrackId TimelineModel::add_track(TrackKind kind, const std::string& name) {
    TrackState ts;
    ts.info.id = next_track_id();
    ts.info.kind = kind;
    ts.info.name = name.empty()
                       ? (kind == TrackKind::Video ? "V" : "A") +
                             std::to_string(ts.info.id.value)
                       : name;
    ts.playlist = std::make_unique<Mlt::Playlist>(profile_);
    ts.playlist->set(kTrackIdProp, static_cast<int>(ts.info.id.value));

    int index = static_cast<int>(tracks_.size());
    // Multitrack::connect wires the playlist as track `index`.
    multitrack_->connect(*ts.playlist, index);
    tractor_->set_track(*ts.playlist, index);
    tracks_.push_back(std::move(ts));
    return tracks_.back().info.id;
}

int TimelineModel::track_index(TrackId id) const {
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].info.id == id) return static_cast<int>(i);
    }
    return -1;
}

TimelineModel::TrackState* TimelineModel::find_track(TrackId id) {
    int idx = track_index(id);
    return idx < 0 ? nullptr : &tracks_[idx];
}

bool TimelineModel::remove_track(TrackId id) {
    int idx = track_index(id);
    if (idx < 0) return false;
    multitrack_->disconnect(idx);
    tractor_->remove_track(idx);
    tracks_.erase(tracks_.begin() + idx);
    // Reconnect remaining tracks to their new indices.
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        multitrack_->connect(*tracks_[i].playlist, i);
        tractor_->set_track(*tracks_[i].playlist, i);
    }
    // Drop clip entries that lived on the removed track.
    for (auto it = clips_.begin(); it != clips_.end();) {
        if (it->second.track_id == id) it = clips_.erase(it);
        else ++it;
    }
    return true;
}

int TimelineModel::track_count() const {
    return static_cast<int>(tracks_.size());
}

const TrackInfo* TimelineModel::track(TrackId id) const {
    int idx = track_index(id);
    return idx < 0 ? nullptr : &tracks_[idx].info;
}

std::vector<TrackInfo> TimelineModel::tracks() const {
    std::vector<TrackInfo> result;
    result.reserve(tracks_.size());
    for (const auto& ts : tracks_) result.push_back(ts.info);
    return result;
}

ClipId TimelineModel::append_clip(TrackId track, int media_index,
                                  int64_t in_point, int64_t out_point) {
    auto* ts = find_track(track);
    if (!ts || media_index < 0) return ClipId{0};

    // We need a cut of the library producer. MediaLibrary owns the
    // parent; we make a cut so in/out is independent per clip.
    Mlt::Producer* parent = library_.producer(media_index);
    if (!parent || !parent->is_valid()) return ClipId{0};

    Mlt::Producer* cut = parent->cut(static_cast<int>(in_point < 0 ? 0 : in_point),
                                     static_cast<int>(out_point < 0 ? -1 : out_point));
    if (!cut || !cut->is_valid()) { delete cut; return ClipId{0}; }

    ClipId id = next_clip_id();
    stamp_clip_id(*cut, id.value);
    stamp_media_index(*cut, media_index);
    int in = in_point < 0 ? 0 : static_cast<int>(in_point);
    int out = out_point < 0 ? parent->get_length() - 1 : static_cast<int>(out_point);
    // The cut already carries in/out; append uses the cut's own range.
    if (ts->playlist->append(*cut, in, out) < 0) {
        delete cut;
        return ClipId{0};
    }
    delete cut;
    rebuild_clip_view();
    return id;
}

ClipId TimelineModel::insert_clip(TrackId track, int media_index,
                                  int64_t position, int64_t in_point,
                                  int64_t out_point) {
    // For T4 we implement append-first; insert_at uses playlist indices.
    // A full position-based insert lands in T5. Here we just append and
    // let rebuild compute starts; insert-by-position is exercised in T5.
    (void)position;
    return append_clip(track, media_index, in_point, out_point);
}

bool TimelineModel::remove_clip(ClipId id) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return false;
    int tidx = track_index(found->second.track_id);
    if (tidx < 0) return false;
    // Find this clip's playlist index by scanning for our stamped id.
    auto& pl = tracks_[tidx].playlist;
    for (int i = 0; i < pl->count(); ++i) {
        Mlt::Producer* cut = pl->get_clip(i);
        if (!cut) continue;
        int64_t cid = read_clip_id(*cut);
        delete cut;
        if (cid == id.value) {
            pl->remove(i);
            rebuild_clip_view();
            return true;
        }
    }
    return false;
}

bool TimelineModel::move_clip(ClipId /*id*/, TrackId /*dest_track*/,
                              int64_t /*dest_position*/) {
    // Cross-track / position-aware move lands in T5 with the full edit
    // op set. T4 establishes the model + id mapping + append/remove.
    return false;
}

bool TimelineModel::trim_clip(ClipId id, int64_t new_in, int64_t new_out) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return false;
    int tidx = track_index(found->second.track_id);
    if (tidx < 0) return false;
    auto& pl = tracks_[tidx].playlist;
    for (int i = 0; i < pl->count(); ++i) {
        Mlt::Producer* cut = pl->get_clip(i);
        if (!cut) continue;
        int64_t cid = read_clip_id(*cut);
        delete cut;
        if (cid == id.value) {
            pl->resize_clip(i, static_cast<int>(new_in),
                            static_cast<int>(new_out));
            rebuild_clip_view();
            return true;
        }
    }
    return false;
}

ClipId TimelineModel::split_clip(ClipId id, int64_t at_timeline_position) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return ClipId{0};
    int tidx = track_index(found->second.track_id);
    if (tidx < 0) return ClipId{0};
    auto& pl = tracks_[tidx].playlist;
    for (int i = 0; i < pl->count(); ++i) {
        Mlt::Producer* cut = pl->get_clip(i);
        if (!cut) continue;
        int64_t cid = read_clip_id(*cut);
        delete cut;
        if (cid == id.value) {
            // Split at the offset within this clip.
            int64_t offset = at_timeline_position - found->second.start;
            if (offset <= 0 || offset >= found->second.length) {
                return ClipId{0};
            }
            pl->split(i, static_cast<int>(offset));
            // The new (right) clip is at index i+1; stamp a new id and
            // carry over the media index so it stays resolvable.
            Mlt::Producer* right = pl->get_clip(i + 1);
            if (!right) return ClipId{0};
            ClipId new_id = next_clip_id();
            stamp_clip_id(*right, new_id.value);
            stamp_media_index(*right, static_cast<int>(found->second.media_index));
            delete right;
            rebuild_clip_view();
            return new_id;
        }
    }
    return ClipId{0};
}

int TimelineModel::clip_count() const {
    return static_cast<int>(clips_.size());
}

const ClipInfo* TimelineModel::clip(ClipId id) const {
    auto found = clips_.find(id.value);
    return found == clips_.end() ? nullptr : &found->second;
}

int TimelineModel::media_index_of(ClipId id) const {
    auto found = clips_.find(id.value);
    return found == clips_.end() ? -1 : found->second.media_index;
}

std::vector<ClipInfo> TimelineModel::clips_on_track(TrackId track) const {
    std::vector<ClipInfo> result;
    for (const auto& [_, info] : clips_) {
        if (info.track_id == track) result.push_back(info);
    }
    std::sort(result.begin(), result.end(),
              [](const ClipInfo& a, const ClipInfo& b) { return a.start < b.start; });
    return result;
}

int64_t TimelineModel::duration() const {
    int64_t max_len = 0;
    for (const auto& ts : tracks_) {
        if (ts.playlist) {
            int64_t len = ts.playlist->get_length();
            if (len > max_len) max_len = len;
        }
    }
    return max_len;
}

void TimelineModel::rebuild_clip_view() {
    clips_.clear();
    for (int t = 0; t < static_cast<int>(tracks_.size()); ++t) {
        auto& pl = tracks_[t].playlist;
        if (!pl) continue;
        for (int i = 0; i < pl->count(); ++i) {
            Mlt::ClipInfo* ci = pl->clip_info(i);
            if (!ci) continue;
            Mlt::Producer* cut = pl->get_clip(i);
            int64_t cid = cut ? read_clip_id(*cut) : 0;
            if (cid > 0) {
                ClipInfo info;
                info.id = ClipId{cid};
                info.track_id = tracks_[t].info.id;
                info.in_point = ci->frame_in;
                info.out_point = ci->frame_out;
                info.start = ci->start;
                info.length = ci->frame_count;
                info.media_index = cut ? read_media_index(*cut) : -1;
                clips_[cid] = info;
            }
            if (cut) delete cut;
            Mlt::Playlist::delete_clip_info(ci);
        }
    }
}

} // namespace vedit
