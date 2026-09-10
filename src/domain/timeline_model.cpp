#include "timeline_model.h"

#include "media_library.h"
#include "util/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <nlohmann/json.hpp>

namespace vedit {

namespace {

// Property name stamped on each cut producer to carry our stable
// ClipId across playlist reindexing. Underscore prefix keeps MLT from
// serializing it to XML (we serialize our own ids in T10).
constexpr const char* kClipIdProp = "_vedit_clip_id";
constexpr const char* kTrackIdProp = "_vedit_track_id";
constexpr const char* kMediaIdxProp = "_vedit_media_idx";

// Playback speed stamped on a clip wrapped in a timewarp producer.
// Absent (or 1.0) means the clip is a plain cut of the source. Stored
// so a second change_clip_speed call can convert the clip's current
// in/out back to source frames instead of compounding the ratio.
constexpr const char* kSpeedProp = "_vedit_speed";

// Marker stamped on every filter vedit attaches on behalf of a user or
// agent. attached_filters() reports only marked filters, so a producer's
// own MLT-internal filters (avcolor_space, crop, resize, swscale, ...)
// are never confused with a user-attached filter of the same service
// name. Filtering by service name alone hid user-attached crop/resize.
constexpr const char* kUserFilterProp = "_vedit_user_filter";

// Human-facing transition type tag stamped on a mix tractor so the type
// requested via transition.add (luma/mix/wipe/...) survives and can be
// read back by state.get and the GUI. MLT itself only marks the tractor
// with the internal "mlt_mix" data property, which does not carry type.
constexpr const char* kTransitionTypeProp = "_vedit_transition_type";

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

double read_clip_speed(Mlt::Producer& cut) {
    // 0 is MLT's "property absent" answer for a double, and speed 0 is
    // meaningless, so treat both as 1.0 (plain, unretimed cut).
    double s = cut.get_double(kSpeedProp);
    return (s == 0.0) ? 1.0 : s;
}

void stamp_clip_speed(Mlt::Producer& cut, double speed) {
    cut.set(kSpeedProp, speed);
}

// Create a cut of a library producer with our stable id stamps.
// Returns a new'd Producer (caller owns) or nullptr. in/out are parent
// frame numbers; -1 means 0 / end-of-source.
Mlt::Producer* make_stamped_cut(MediaLibrary& lib, int media_index,
                                int64_t in_point, int64_t out_point,
                                int64_t clip_id) {
    Mlt::Producer* parent = lib.producer(media_index);
    if (!parent || !parent->is_valid()) return nullptr;
    int in = in_point < 0 ? 0 : static_cast<int>(in_point);
    int out = out_point < 0 ? -1 : static_cast<int>(out_point);
    Mlt::Producer* cut = parent->cut(in, out);
    if (!cut || !cut->is_valid()) { delete cut; return nullptr; }
    stamp_clip_id(*cut, clip_id);
    stamp_media_index(*cut, media_index);
    return cut;
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
    if (name.empty()) {
        // Number tracks per type (V1, V2, ... / A1, A2, ...) rather than
        // by the global id counter, which would yield V1 V2 A3 V4.
        int same_kind = 1;
        for (const auto& existing : tracks_) {
            if (existing.info.kind == kind) ++same_kind;
        }
        ts.info.name = (kind == TrackKind::Video ? "V" : "A") +
                       std::to_string(same_kind);
    } else {
        ts.info.name = name;
    }
    ts.playlist = std::make_unique<Mlt::Playlist>(profile_);
    ts.playlist->set(kTrackIdProp, static_cast<int>(ts.info.id.value));

    int index = static_cast<int>(tracks_.size());
    // Multitrack::connect wires the playlist as track `index`.
    multitrack_->connect(*ts.playlist, index);
    tractor_->set_track(*ts.playlist, index);
    tracks_.push_back(std::move(ts));
    replant_compositions();
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
    replant_compositions();
    return true;
}

namespace {
// MLT playlist hide property: bit 0 hides video, bit 1 hides audio.
// combine_hide_flags sets the playlist property from our flags.
void apply_track_hide(Mlt::Playlist& pl, bool muted, bool hidden) {
    int hide = 0;
    if (hidden) hide |= 1;  // hide video
    if (muted)  hide |= 2;  // hide audio
    pl.set("hide", hide);
}
} // namespace

bool TimelineModel::set_track_muted(TrackId id, bool muted) {
    int idx = track_index(id);
    if (idx < 0) return false;
    tracks_[idx].info.muted = muted;
    apply_track_hide(*tracks_[idx].playlist, muted,
                     tracks_[idx].info.hidden);
    return true;
}

bool TimelineModel::set_track_hidden(TrackId id, bool hidden) {
    int idx = track_index(id);
    if (idx < 0) return false;
    tracks_[idx].info.hidden = hidden;
    apply_track_hide(*tracks_[idx].playlist,
                     tracks_[idx].info.muted, hidden);
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

    ClipId id = next_clip_id();
    Mlt::Producer* cut = make_stamped_cut(library_, media_index,
                                          in_point, out_point, id.value);
    if (!cut) return ClipId{0};

    // Pass -1/-1 so MLT uses the cut's own in/out (the cut already
    // encodes the source range; passing parent-relative in/out again
    // would be redundant and fragile).
    if (ts->playlist->append(*cut, -1, -1) < 0) {
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
    auto* ts = find_track(track);
    if (!ts || media_index < 0 || position < 0) return ClipId{0};

    ClipId id = next_clip_id();
    Mlt::Producer* cut = make_stamped_cut(library_, media_index,
                                          in_point, out_point, id.value);
    if (!cut) return ClipId{0};

    // insert_at only honors position when there is already content at
    // it; on a short/empty playlist it appends at the end. Pad with a
    // blank first so the clip lands exactly at `position`.
    int64_t current_len = ts->playlist->get_length();
    if (position > current_len) {
        ts->playlist->blank(static_cast<int>(position - current_len));
    }
    int idx = ts->playlist->insert_at(static_cast<int>(position), *cut, 0);
    delete cut;
    if (idx < 0) return ClipId{0};
    rebuild_clip_view();
    return id;
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

bool TimelineModel::move_clip(ClipId id, TrackId dest_track,
                              int64_t dest_position) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return false;
    int src_tidx = track_index(found->second.track_id);
    int dst_tidx = track_index(dest_track);
    if (src_tidx < 0 || dst_tidx < 0 || dest_position < 0) return false;

    // Locate the clip's playlist index on the source track.
    auto& src_pl = tracks_[src_tidx].playlist;
    int src_idx = -1;
    for (int i = 0; i < src_pl->count(); ++i) {
        Mlt::Producer* cut = src_pl->get_clip(i);
        int64_t cid = cut ? read_clip_id(*cut) : 0;
        delete cut;
        if (cid == id.value) { src_idx = i; break; }
    }
    if (src_idx < 0) return false;

    // Carry the clip's existing cut over instead of re-cutting it from
    // the library. A retimed clip is a cut of a timewarp producer, so
    // re-cutting from the source at its current in/out would both drop
    // the retime and reinterpret timewarp frames as source frames.
    Mlt::Producer* cut = src_pl->get_clip(src_idx);
    if (!cut) return false;
    src_pl->remove(src_idx);

    auto& dst_pl = tracks_[dst_tidx].playlist;
    int64_t dst_len = dst_pl->get_length();
    if (dest_position > dst_len) {
        dst_pl->blank(static_cast<int>(dest_position - dst_len));
    }
    int idx = dst_pl->insert_at(static_cast<int>(dest_position), *cut, 0);
    delete cut;
    if (idx < 0) return false;
    rebuild_clip_view();
    return true;
}

bool TimelineModel::trim_clip(ClipId id, int64_t new_in, int64_t new_out) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return false;
    int tidx = track_index(found->second.track_id);
    if (tidx < 0) return false;
    auto& pl = tracks_[tidx].playlist;

    const int64_t old_in = found->second.in_point;
    const int64_t old_start = found->second.start;
    const int64_t media_index = found->second.media_index;

    // Standard NLE semantics: when the LEFT edge is trimmed (new_in
    // changes), the clip's timeline start moves by the same delta so
    // the left edge visually follows the mouse while the right edge
    // stays fixed. MLT's resize_clip alone keeps the clip's playlist
    // position (and thus its timeline start) unchanged — only the
    // in/out and length change — which looks like "the yellow handle
    // doesn't move". We remove + reinsert at the moved start to keep
    // both the stable ClipId and the NLE-correct visual.
    int64_t delta = new_in - old_in;
    int64_t new_start = old_start + delta;
    if (new_start < 0) {
        // Clamp: don't let the clip start before the timeline origin.
        new_in -= new_start;  // equivalently, pull in back by |new_start|
        new_start = 0;
    }

    // Locate and remove the existing clip entry.
    int idx = -1;
    for (int i = 0; i < pl->count(); ++i) {
        Mlt::Producer* cut = pl->get_clip(i);
        int64_t cid = cut ? read_clip_id(*cut) : 0;
        delete cut;
        if (cid == id.value) { idx = i; break; }
    }
    if (idx < 0) return false;
    pl->remove(idx);

    // Re-stamp a fresh cut with the new in/out and the SAME stable id,
    // then insert at the moved timeline start so the left edge tracks.
    Mlt::Producer* cut = make_stamped_cut(library_, static_cast<int>(media_index),
                                          new_in, new_out, id.value);
    if (!cut) return false;
    if (new_start > pl->get_length()) {
        pl->blank(static_cast<int>(new_start - pl->get_length()));
    }
    int new_idx = pl->insert_at(static_cast<int>(new_start), *cut, 0);
    delete cut;
    if (new_idx < 0) return false;
    rebuild_clip_view();
    return true;
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

// ---- advanced editing (M4) ----

ClipId TimelineModel::overwrite_clip(TrackId track, int media_index,
                                      int64_t position,
                                      int64_t in_point, int64_t out_point) {
    int tidx = track_index(track);
    if (tidx < 0) return ClipId{0};

    ClipId id = next_clip_id();
    Mlt::Producer* cut = make_stamped_cut(library_, media_index,
                                          in_point, out_point, id.value);
    if (!cut) return ClipId{0};

    auto& pl = tracks_[tidx].playlist;
    // mode=1: overwrite blanks at the target position.
    int rc = pl->insert_at(static_cast<int>(position), *cut, 1);
    delete cut;
    if (rc < 0) return ClipId{0};

    rebuild_clip_view();
    return id;
}

bool TimelineModel::lift_clip(ClipId id) {
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
            pl->replace_with_blank(i);
            rebuild_clip_view();
            return true;
        }
    }
    return false;
}

bool TimelineModel::change_clip_speed(ClipId id, double speed) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return false;
    if (speed == 0.0) return false;

    int tidx = track_index(found->second.track_id);
    if (tidx < 0) return false;

    const auto* item = library_.item(found->second.media_index);
    if (!item) return false;

    const double abs_speed = std::abs(speed);

    auto& pl = tracks_[tidx].playlist;
    for (int i = 0; i < pl->count(); ++i) {
        Mlt::Producer* cut = pl->get_clip(i);
        if (!cut) continue;
        int64_t cid = read_clip_id(*cut);
        if (cid != id.value) {
            delete cut;
            continue;
        }
        const double cur_speed = std::abs(read_clip_speed(*cut));
        delete cut;

        // in/out are frames of the current producer, which is already
        // timewarped when the clip was retimed before. Convert back to
        // source frames first, otherwise repeated calls compound and
        // 2.0 twice would not equal 4.0.
        const int64_t src_in =
            static_cast<int64_t>(std::llround(found->second.in_point * cur_speed));
        const int64_t src_out =
            static_cast<int64_t>(std::llround(found->second.out_point * cur_speed));

        Mlt::Producer* new_cut = nullptr;
        if (abs_speed == 1.0) {
            new_cut = make_stamped_cut(library_,
                                       static_cast<int>(found->second.media_index),
                                       src_in, src_out, id.value);
        } else {
            // MLT's timewarp producer exposes source frame f * speed at
            // its own frame f, so the source range maps to itself
            // divided by the speed.
            std::string tw_res = std::to_string(speed) + ":" + item->path;
            Mlt::Producer tw(profile_, "timewarp", tw_res.c_str());
            if (!tw.is_valid()) return false;
            new_cut = tw.cut(static_cast<int>(std::llround(src_in / abs_speed)),
                             static_cast<int>(std::llround(src_out / abs_speed)));
            if (new_cut && new_cut->is_valid()) {
                stamp_clip_id(*new_cut, id.value);
                stamp_media_index(*new_cut,
                                  static_cast<int>(found->second.media_index));
                stamp_clip_speed(*new_cut, speed);
            }
        }
        if (!new_cut || !new_cut->is_valid()) {
            delete new_cut;
            return false;
        }

        pl->remove(i);
        pl->insert(*new_cut, i, 0, -1);
        delete new_cut;
        rebuild_clip_view();
        return true;
    }
    return false;
}

bool TimelineModel::add_transition(TrackId track, int clip_index,
                                    int length, const std::string& type) {
    int tidx = track_index(track);
    if (tidx < 0) return false;
    auto& pl = tracks_[tidx].playlist;
    if (clip_index < 0 || clip_index >= pl->count() - 1) return false;

    // mlt_playlist_mix plants a 2-track mix tractor at clip_index+1 but,
    // with a NULL transition, plants no blend at all -- the upper track
    // just sits on the lower one. A visible transition requires planting
    // transitions into the mix tractor via mix_add (Shotcut's pattern).
    if (pl->mix(clip_index, length, nullptr) != 0) return false;

    // The mix tractor is the new clip at clip_index+1.
    Mlt::Producer* mix_cut = pl->get_clip(clip_index + 1);
    if (!mix_cut) return false;

    // Pick the video transition service by requested type. "mix" is a
    // plain audio-style dissolve; the wipe family runs through "luma"
    // with a named luma resource. Unknown types fall back to a luma
    // dissolve (no resource = soft cross-dissolve).
    std::string video_service = "luma";
    std::string luma_resource;
    if (type == "mix" || type == "dissolve" || type.empty()) {
        video_service = "luma";        // soft dissolve (no luma file)
    } else if (type == "luma" || type == "wipe") {
        video_service = "luma";        // default diagonal wipe via softness
    } else if (type == "bar" || type == "barn" || type == "clock" ||
               type == "iris" || type == "spiral") {
        video_service = "luma";
        luma_resource = type;          // named built-in luma pattern
    } else {
        video_service = "luma";
    }

    {
        Mlt::Transition dissolve(profile_, video_service.c_str());
        if (dissolve.is_valid()) {
            // alpha_over keeps the blend correct over transparent areas.
            dissolve.set("alpha_over", 1);
            dissolve.set("fix_background_alpha", 1);
            if (!luma_resource.empty())
                dissolve.set("resource", luma_resource.c_str());
            pl->mix_add(clip_index + 1, &dissolve);
        }
        // Always cross-fade audio across the same region.
        Mlt::Transition cross_fade(profile_, "mix:-2");
        if (cross_fade.is_valid())
            pl->mix_add(clip_index + 1, &cross_fade);
    }

    // Record the requested type on the mix tractor so state.get and the
    // GUI can read it back. mlt_mix tractors are identified downstream by
    // the "mlt_mix" data property; we add our own human-facing type tag.
    mix_cut->parent().set(kTransitionTypeProp, type.c_str());

    delete mix_cut;
    rebuild_clip_view();
    return true;
}

ClipId TimelineModel::detach_audio(ClipId id) {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return ClipId{0};

    const auto* item = library_.item(found->second.media_index);
    if (!item || !item->has_audio) return ClipId{0};

    Mlt::Producer* producer = library_.producer(found->second.media_index);
    if (!producer) return ClipId{0};

    // Create a new audio track.
    TrackId audio_track = add_track(TrackKind::Audio, "Audio (detached)");
    if (!audio_track.valid()) return ClipId{0};

    // Append a clip to the audio track with the same in/out.
    auto audio_clip = append_clip(audio_track, found->second.media_index,
                                   found->second.start, found->second.start + found->second.length);
    return audio_clip;
}

bool TimelineModel::set_clip_audio_enabled(ClipId id, bool enabled) {
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    if (enabled) {
        // Restore the original audio_index from the parent producer.
        Mlt::Producer parent(cut->get_producer());
        if (parent.is_valid()) {
            int orig = parent.get_int("audio_index");
            cut->set("audio_index", orig);
        }
    } else {
        cut->set("audio_index", -1);
    }
    delete cut;
    return true;
}

bool TimelineModel::set_clip_video_enabled(ClipId id, bool enabled) {
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    if (enabled) {
        Mlt::Producer parent(cut->get_producer());
        if (parent.is_valid()) {
            int orig = parent.get_int("video_index");
            cut->set("video_index", orig);
        }
    } else {
        cut->set("video_index", -1);
    }
    delete cut;
    return true;
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

int TimelineModel::playlist_index_of(ClipId id) const {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return -1;
    TrackId tid = found->second.track_id;
    int tidx = track_index(tid);
    if (tidx < 0) return -1;
    auto& pl = tracks_[tidx].playlist;
    for (int i = 0; i < pl->count(); ++i) {
        if (pl->is_blank(i)) continue;
        Mlt::Producer* cut = pl->get_clip(i);
        if (!cut) continue;
        int64_t cid = cut->get_int("vedit_clip_id");
        delete cut;
        if (cid == id.value) return i;
    }
    return -1;
}

int TimelineModel::count_clips_for_media(int media_index) const {
    int n = 0;
    for (const auto& [_, info] : clips_) {
        if (info.media_index == media_index) ++n;
    }
    return n;
}

void TimelineModel::on_media_removed(int removed_index) {
    for (auto& ts : tracks_) {
        if (!ts.playlist) continue;
        for (int i = 0; i < ts.playlist->count(); ++i) {
            Mlt::Producer* cut = ts.playlist->get_clip(i);
            if (!cut) continue;
            int mi = read_media_index(*cut);
            // Shift tail entries down by one. Clips at exactly
            // removed_index should not exist (caller guarantees), but
            // be defensive: leave any orphaned stamp alone so it stays
            // detectable rather than silently aliasing another source.
            if (mi > removed_index) {
                stamp_media_index(*cut, mi - 1);
            }
            delete cut;
        }
    }
    rebuild_clip_view();
}

void TimelineModel::reconnect_clips(int media_index) {
    // Snapshot affected clips before rebuild_clip_view clears clips_.
    std::vector<ClipInfo> affected;
    for (const auto& [cid, info] : clips_) {
        if (info.media_index == media_index) {
            affected.push_back(info);
        }
    }

    for (const auto& ci : affected) {
        int tidx = track_index(ci.track_id);
        if (tidx < 0) continue;
        auto& pl = tracks_[tidx].playlist;
        for (int i = 0; i < pl->count(); ++i) {
            Mlt::Producer* cut = pl->get_clip(i);
            if (!cut) continue;
            int64_t cid = read_clip_id(*cut);
            delete cut;
            if (cid != ci.id.value) continue;

            pl->remove(i);
            Mlt::Producer* new_cut = make_stamped_cut(
                library_, media_index,
                ci.in_point, ci.out_point, ci.id.value);
            if (!new_cut) return;
            pl->insert(*new_cut, i, -1, -1);
            delete new_cut;
            break;
        }
    }

    rebuild_clip_view();
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
    int64_t max_end = 0;
    for (const auto& kv : clips_) {
        int64_t end = kv.second.start + kv.second.length;
        if (end > max_end) max_end = end;
    }
    return max_end;
}

void TimelineModel::replant_compositions() {
    // Disconnect existing composites from the field, then drop them.
    Mlt::Field* field = tractor_->field();
    if (field) {
        for (auto& t : composites_) {
            if (t && t->is_valid()) field->disconnect_service(*t);
        }
        delete field;
    }
    composites_.clear();

    // Plant one composite transition between each consecutive pair of
    // video tracks so track N+1 overlays track N. a_track is the
    // lower (background) track, b_track the upper (foreground).
    for (size_t i = 1; i < tracks_.size(); ++i) {
        if (tracks_[i - 1].info.kind != TrackKind::Video) continue;
        if (tracks_[i].info.kind != TrackKind::Video) continue;
        auto comp = std::make_unique<Mlt::Transition>(profile_, "composite");
        if (!comp || !comp->is_valid()) {
            Logger::log(Logger::Level::Error,
                        "replant_compositions: composite transition unavailable");
            continue;
        }
        comp->set("progressive", 1);
        tractor_->plant_transition(*comp, static_cast<int>(i - 1),
                                   static_cast<int>(i));
        composites_.push_back(std::move(comp));
    }
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

// ---- filters (M2) ----

Mlt::Producer* TimelineModel::find_cut_producer(ClipId id, int* track_idx) const {
    auto found = clips_.find(id.value);
    if (found == clips_.end()) return nullptr;
    int tidx = track_index(found->second.track_id);
    if (tidx < 0) return nullptr;
    if (track_idx) *track_idx = tidx;

    const auto& pl = tracks_[tidx].playlist;
    for (int i = 0; i < pl->count(); ++i) {
        Mlt::Producer* cut = pl->get_clip(i);
        if (!cut) continue;
        if (read_clip_id(*cut) == id.value) {
            return cut;  // caller owns
        }
        delete cut;
    }
    return nullptr;
}

namespace {

// Find an attached filter by service name on a cut producer.
int find_attached_filter(Mlt::Producer& cut, const std::string& name) {
    int n = cut.filter_count();
    for (int i = 0; i < n; ++i) {
        Mlt::Filter* filter = cut.filter(i);
        if (filter && filter->is_valid() && filter->get("mlt_service") == name) {
            delete filter;
            return i;
        }
        delete filter;
    }
    return -1;
}

} // namespace

bool TimelineModel::attach_filter(ClipId clip_id, const std::string& filter_name,
                                    int in, int out) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return false;

    Mlt::Filter filter(profile_, filter_name.c_str());
    if (!filter.is_valid()) {
        delete cut;
        return false;
    }
    filter.set(kUserFilterProp, 1);

    // Attach to cut->parent() — the shared root producer stored in the
    // playlist. pl->get_clip() returns a temporary cut wrapper; its
    // parent() is the actual producer that plays back.
    Mlt::Producer& parent = cut->parent();
    int rc = mlt_service_attach(MLT_SERVICE(parent.get_producer()),
                                MLT_FILTER(filter.get_filter()));

    // set_in_and_out must be called AFTER attach because
    // mlt_service_attach resets out to 0 (mlt_service_clone_filter).
    if (rc == 0 && (in >= 0 || out >= 0)) {
        int fidx = find_attached_filter(parent, filter_name);
        if (fidx >= 0) {
            Mlt::Filter* attached = parent.filter(fidx);
            if (attached) {
                int fin = (in >= 0) ? in : 0;
                int fout = (out >= 0) ? out : 0;
                attached->set_in_and_out(fin, fout);
                delete attached;
            }
        }
    }

    delete cut;
    return rc == 0;
}

bool TimelineModel::set_filter_in_out(ClipId clip_id,
                                       const std::string& filter_name,
                                       int in, int out) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return false;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, filter_name);
    if (fidx < 0) {
        delete cut;
        return false;
    }
    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return false;
    }
    filter->set_in_and_out(in, out);
    delete filter;
    delete cut;
    return true;
}

bool TimelineModel::detach_filter(ClipId clip_id, const std::string& filter_name) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return false;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, filter_name);
    if (fidx < 0) {
        delete cut;
        return false;
    }
    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return false;
    }
    int rc = mlt_service_detach(MLT_SERVICE(parent.get_producer()),
                                MLT_FILTER(filter->get_filter()));
    delete filter;
    delete cut;
    return rc == 0;
}

bool TimelineModel::set_filter_param(ClipId clip_id,
                                      const std::string& filter_name,
                                      const std::string& param,
                                      const std::string& value) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return false;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, filter_name);
    if (fidx < 0) {
        delete cut;
        return false;
    }
    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return false;
    }
    filter->set(param.c_str(), value.c_str());
    delete filter;
    delete cut;
    return true;
}

std::string TimelineModel::get_filter_param(ClipId clip_id,
                                             const std::string& filter_name,
                                             const std::string& param) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return "";

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, filter_name);
    if (fidx < 0) {
        delete cut;
        return "";
    }
    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return "";
    }
    const char* val = filter->get(param.c_str());
    std::string result = val ? val : "";
    delete filter;
    delete cut;
    return result;
}

// ---- Keyframe-aware param API (M3 / P2) ----

static mlt_keyframe_type keyframe_type_from_string(const std::string& s) {
    if (s == "smooth") return mlt_keyframe_smooth;
    if (s == "discrete") return mlt_keyframe_discrete;
    return mlt_keyframe_linear;  // default
}

static const char* keyframe_type_to_string(mlt_keyframe_type t) {
    switch (t) {
        case mlt_keyframe_discrete: return "discrete";
        case mlt_keyframe_smooth:   return "smooth";
        default:                    return "linear";
    }
}

bool TimelineModel::set_filter_param_keyframes(
        ClipId clip_id, const std::string& filter_name,
        const std::string& param, const nlohmann::json& keyframes) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return false;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, filter_name);
    if (fidx < 0) {
        delete cut;
        return false;
    }
    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return false;
    }

    if (!keyframes.is_array() || keyframes.empty()) {
        filter->set(param.c_str(), "");
        delete filter;
        delete cut;
        return true;
    }

    // Build the MLT animation string: "frame=value;frame=value;..."
    // mlt_properties_anim_set cannot be called twice on the same
    // property (upstream MLT bug: second call segfaults). Instead
    // set the serialized animation string directly via mlt_properties_set,
    // which MLT auto-parses as an animation.
    std::string anim_str;
    for (const auto& kf : keyframes) {
        if (!anim_str.empty()) anim_str += ';';
        anim_str += std::to_string(kf.value("frame", 0));
        anim_str += '=';
        const auto& v = kf["value"];
        if (v.is_string())
            anim_str += v.get<std::string>();
        else
            anim_str += v.dump();
    }
    mlt_properties_set(filter->get_properties(),
                       param.c_str(), anim_str.c_str());

    // Set keyframe types. Use frame-number lookup because MLT sorts
    // keyframes internally after parsing the animation string.
    for (const auto& kf : keyframes) {
        if (kf.contains("type") && kf["type"] != "linear") {
            mlt_animation anim = mlt_properties_get_animation(
                filter->get_properties(), param.c_str());
            if (anim) {
                int target_frame = kf.value("frame", 0);
                int n = mlt_animation_key_count(anim);
                for (int j = 0; j < n; ++j) {
                    mlt_animation_item_s item;
                    item.property = mlt_property_init();
                    if (mlt_animation_key_get(anim, &item, j) == 0 &&
                        item.frame == target_frame) {
                        mlt_animation_key_set_type(anim, j,
                            keyframe_type_from_string(
                                kf["type"].get<std::string>()));
                        break;
                    }
                    mlt_property_close(item.property);
                }
                // Do NOT call mlt_animation_close — borrowed pointer.
            }
        }
    }

    delete filter;
    delete cut;
    return true;
}

nlohmann::json TimelineModel::get_filter_param_keyframes(
        ClipId clip_id, const std::string& filter_name,
        const std::string& param) {
    Mlt::Producer* cut = find_cut_producer(clip_id, nullptr);
    if (!cut) return nlohmann::json::object();

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, filter_name);
    if (fidx < 0) {
        delete cut;
        return nlohmann::json::object();
    }
    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return nlohmann::json::object();
    }

    Mlt::Properties props(filter->get_properties());
    if (props.is_anim(param.c_str())) {
        // mlt_properties_get_animation may return NULL on some MLT
        // versions (upstream bug). Fall back to reading the raw
        // string value and wrapping it as keyframes.
        mlt_animation anim = mlt_properties_get_animation(
            props.get_properties(), param.c_str());
        if (anim) {
            nlohmann::json kfs = nlohmann::json::array();
            int n = mlt_animation_key_count(anim);
            mlt_animation_item_s item;
            item.property = mlt_property_init();
            for (int i = 0; i < n; ++i) {
                if (mlt_animation_key_get(anim, &item, i) == 0) {
                    nlohmann::json kf;
                    kf["frame"] = item.frame;
                    const char* val = mlt_property_get_string(item.property);
                    kf["value"] = val ? val : "";
                    kf["type"] = keyframe_type_to_string(item.keyframe_type);
                    kfs.push_back(std::move(kf));
                }
            }
            // Do NOT call mlt_animation_close — the animation is
            // owned by the mlt_properties, closing it frees memory
            // still in use (use-after-free).
            mlt_property_close(item.property);
            delete filter;
            delete cut;
            nlohmann::json result;
            result["keyframes"] = std::move(kfs);
            return result;
        }
        // Fallback: return the raw animation string.
        const char* raw = filter->get(param.c_str());
        delete filter;
        delete cut;
        nlohmann::json result;
        result["value"] = raw ? raw : "";
        return result;
    }

    // Static param.
    const char* val = filter->get(param.c_str());
    std::string s = val ? val : "";
    delete filter;
    delete cut;
    nlohmann::json result;
    result["value"] = s;
    return result;
}

// ---- per-track filters ----
//
// Per-track filters are attached to the track's playlist producer,
// same API as per-clip (mlt_service_attach). This is Shotcut's
// pattern and simpler than field-planting. The filter applies to
// every clip on the track because it runs on the playlist output.

bool TimelineModel::track_attach_filter(TrackId track_id,
                                         const std::string& filter_name) {
    int tidx = track_index(track_id);
    if (tidx < 0) return false;

    Mlt::Filter filter(profile_, filter_name.c_str());
    if (!filter.is_valid()) return false;
    filter.set(kUserFilterProp, 1);

    auto& pl = tracks_[tidx].playlist;
    mlt_service_attach(MLT_SERVICE(pl->get_producer()),
                       MLT_FILTER(filter.get_filter()));
    return true;
}

namespace {

Mlt::Filter* find_playlist_filter(Mlt::Playlist& pl, const std::string& name) {
    int n = pl.filter_count();
    for (int i = 0; i < n; ++i) {
        Mlt::Filter* f = pl.filter(i);
        if (!f) continue;
        const char* svc = f->get("mlt_service");
        if (svc && svc == name) return f;
        delete f;
    }
    return nullptr;
}

} // namespace

bool TimelineModel::track_detach_filter(TrackId track_id,
                                         const std::string& filter_name) {
    int tidx = track_index(track_id);
    if (tidx < 0) return false;

    auto& pl = tracks_[tidx].playlist;
    Mlt::Filter* found = find_playlist_filter(*pl, filter_name);
    if (!found) return false;
    mlt_service_detach(MLT_SERVICE(pl->get_producer()),
                       MLT_FILTER(found->get_filter()));
    delete found;
    return true;
}

bool TimelineModel::track_set_filter_param(TrackId track_id,
                                            const std::string& filter_name,
                                            const std::string& param,
                                            const std::string& value) {
    int tidx = track_index(track_id);
    if (tidx < 0) return false;

    auto& pl = tracks_[tidx].playlist;
    Mlt::Filter* found = find_playlist_filter(*pl, filter_name);
    if (!found) return false;
    found->set(param.c_str(), value.c_str());
    delete found;
    return true;
}

std::string TimelineModel::track_get_filter_param(TrackId track_id,
                                                    const std::string& filter_name,
                                                    const std::string& param) {
    int tidx = track_index(track_id);
    if (tidx < 0) return "";

    auto& pl = tracks_[tidx].playlist;
    Mlt::Filter* found = find_playlist_filter(*pl, filter_name);
    if (!found) return "";
    const char* val = found->get(param.c_str());
    std::string result = val ? val : "";
    delete found;
    return result;
}

bool TimelineModel::track_set_filter_param_keyframes(
        TrackId track_id, const std::string& filter_name,
        const std::string& param, const nlohmann::json& keyframes) {
    int tidx = track_index(track_id);
    if (tidx < 0) return false;

    auto& pl = tracks_[tidx].playlist;
    Mlt::Filter* found = find_playlist_filter(*pl, filter_name);
    if (!found) return false;

    if (!keyframes.is_array() || keyframes.empty()) {
        found->set(param.c_str(), "");
        delete found;
        return true;
    }

    std::string anim_str;
    for (const auto& kf : keyframes) {
        if (!anim_str.empty()) anim_str += ';';
        anim_str += std::to_string(kf.value("frame", 0));
        anim_str += '=';
        const auto& v = kf["value"];
        if (v.is_string())
            anim_str += v.get<std::string>();
        else
            anim_str += v.dump();
    }
    found->set(param.c_str(), anim_str.c_str());

    // Set keyframe types. The input JSON array may not be sorted by
    // frame, but MLT internally sorts keyframes by frame after
    // parsing the animation string. Use frame-number lookup instead
    // of array index to set the correct keyframe.
    for (const auto& kf : keyframes) {
        if (kf.contains("type") && kf["type"] != "linear") {
            mlt_animation anim = mlt_properties_get_animation(
                found->get_properties(), param.c_str());
            if (anim) {
                int target_frame = kf.value("frame", 0);
                int n = mlt_animation_key_count(anim);
                for (int j = 0; j < n; ++j) {
                    mlt_animation_item_s item;
                    item.property = mlt_property_init();
                    if (mlt_animation_key_get(anim, &item, j) == 0 &&
                        item.frame == target_frame) {
                        mlt_animation_key_set_type(anim, j,
                            keyframe_type_from_string(
                                kf["type"].get<std::string>()));
                        break;
                    }
                    mlt_property_close(item.property);
                }
                // Do NOT call mlt_animation_close — borrowed pointer.
            }
        }
    }

    delete found;
    return true;
}

nlohmann::json TimelineModel::track_get_filter_param_keyframes(
        TrackId track_id, const std::string& filter_name,
        const std::string& param) {
    int tidx = track_index(track_id);
    if (tidx < 0) return nlohmann::json::object();

    auto& pl = tracks_[tidx].playlist;
    Mlt::Filter* found = find_playlist_filter(*pl, filter_name);
    if (!found) return nlohmann::json::object();

    Mlt::Properties props(found->get_properties());
    if (props.is_anim(param.c_str())) {
        nlohmann::json kfs = nlohmann::json::array();
        mlt_animation anim = mlt_properties_get_animation(
            props.get_properties(), param.c_str());
        if (anim) {
            int n = mlt_animation_key_count(anim);
            mlt_animation_item_s item;
            item.property = mlt_property_init();
            for (int i = 0; i < n; ++i) {
                if (mlt_animation_key_get(anim, &item, i) == 0) {
                    nlohmann::json kf;
                    kf["frame"] = item.frame;
                    const char* val = mlt_property_get_string(item.property);
                    kf["value"] = val ? val : "";
                    kf["type"] = keyframe_type_to_string(item.keyframe_type);
                    kfs.push_back(std::move(kf));
                }
            }
            // Do NOT call mlt_animation_close — borrowed pointer.
            mlt_property_close(item.property);
        }
        delete found;
        nlohmann::json result;
        result["keyframes"] = std::move(kfs);
        return result;
    }

    const char* val = found->get(param.c_str());
    std::string s = val ? val : "";
    delete found;
    nlohmann::json result;
    result["value"] = s;
    return result;
}

static nlohmann::json param_value_to_json(Mlt::Properties& props,
                                          const char* key) {
    if (props.is_anim(key)) {
        nlohmann::json kfs = nlohmann::json::array();
        mlt_animation anim = mlt_properties_get_animation(
            props.get_properties(), key);
        if (anim) {
            int n = mlt_animation_key_count(anim);
            mlt_animation_item_s item;
            item.property = mlt_property_init();
            for (int i = 0; i < n; ++i) {
                if (mlt_animation_key_get(anim, &item, i) == 0) {
                    nlohmann::json kf;
                    kf["frame"] = item.frame;
                    const char* val = mlt_property_get_string(item.property);
                    kf["value"] = val ? val : "";
                    kf["type"] = keyframe_type_to_string(
                        item.keyframe_type);
                    kfs.push_back(std::move(kf));
                }
            }
            // Do NOT call mlt_animation_close — borrowed pointer.
            mlt_property_close(item.property);
        }
        nlohmann::json result;
        result["keyframes"] = std::move(kfs);
        return result;
    }
    const char* val = props.get(key);
    nlohmann::json result;
    result["value"] = val ? val : "";
    return result;
}

std::vector<TimelineModel::AttachedFilterInfo>
TimelineModel::attached_filters() const {
    std::vector<AttachedFilterInfo> result;

    for (const auto& ts : tracks_) {
        if (!ts.playlist) continue;
        // Per-clip filters. Use the parent producer (not the cut
        // from get_clip()) so we see the same filter set that
        // attach_filter writes to.
        for (int i = 0; i < ts.playlist->count(); ++i) {
            if (ts.playlist->is_blank(i)) continue;
            Mlt::Producer* cut = ts.playlist->get_clip(i);
            if (!cut) continue;
            int64_t clip_id = read_clip_id(*cut);
            Mlt::Producer& parent = cut->parent();
            int nf = parent.filter_count();
            for (int f = 0; f < nf; ++f) {
                Mlt::Filter* filter = parent.filter(f);
                if (!filter) continue;
                const char* svc = filter->get("mlt_service");
                if (!svc || !svc[0]) { delete filter; continue; }
                // Report only filters vedit attached for the user/agent.
                // A producer carries its own internal filters
                // (avcolor_space, crop, resize, swscale, ...); filtering
                // those by service name also hid user-attached filters of
                // the same name (e.g. an agent's crop). The marker
                // distinguishes intent, not service identity.
                if (!filter->get_int(kUserFilterProp)) { delete filter; continue; }
                AttachedFilterInfo info;
                info.target_type = "clip";
                info.target_id = clip_id;
                info.filter_name = svc;
                Mlt::Properties props(filter->get_properties());
                for (int p = 0; p < props.count(); ++p) {
                    const char* key = props.get_name(p);
                    if (!key || key[0] == '_') continue;
                    if (std::strcmp(key, "mlt_service") == 0 ||
                        std::strcmp(key, "mlt_type") == 0 ||
                        std::strcmp(key, "mlt_filter") == 0) continue;
                    info.params.emplace_back(
                        key, param_value_to_json(props, key));
                }
                result.push_back(std::move(info));
                delete filter;
            }
            delete cut;
        }

        // Per-track filters.
        int nf = ts.playlist->filter_count();
        for (int f = 0; f < nf; ++f) {
            Mlt::Filter* filter = ts.playlist->filter(f);
            if (!filter) continue;
            const char* svc = filter->get("mlt_service");
            if (!svc || !svc[0]) { delete filter; continue; }
            if (!filter->get_int(kUserFilterProp)) { delete filter; continue; }
            AttachedFilterInfo info;
            info.target_type = "track";
            info.target_id = ts.info.id.value;
            info.filter_name = svc;
            Mlt::Properties props(filter->get_properties());
            for (int p = 0; p < props.count(); ++p) {
                const char* key = props.get_name(p);
                if (!key || key[0] == '_') continue;
                if (std::strcmp(key, "mlt_service") == 0 ||
                    std::strcmp(key, "mlt_type") == 0 ||
                    std::strcmp(key, "mlt_filter") == 0) continue;
                info.params.emplace_back(
                    key, param_value_to_json(props, key));
            }
            result.push_back(std::move(info));
            delete filter;
        }
    }
    return result;
}

bool TimelineModel::set_composite_geometry(vedit::TrackId track_id,
                                            double x, double y,
                                            double w, double h) {
    int tidx = track_index(track_id);
    if (tidx < 0) return false;
    if (tidx == 0) return false;
    if (tracks_[tidx].info.kind != TrackKind::Video) return false;
    if (tracks_[tidx - 1].info.kind != TrackKind::Video) return false;

    int composite_idx = -1;
    for (int i = 1; i <= tidx; ++i) {
        if (tracks_[i - 1].info.kind == TrackKind::Video &&
            tracks_[i].info.kind == TrackKind::Video) {
            ++composite_idx;
        }
    }
    if (composite_idx < 0 ||
        composite_idx >= static_cast<int>(composites_.size())) {
        return false;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f%%/%.0f%%:%.0f%%x%.0f%%",
                  x, y, w, h);
    composites_[composite_idx]->set("geometry", buf);
    return true;
}

bool TimelineModel::get_composite_geometry(TrackId track_id,
                                            double& x, double& y,
                                            double& w, double& h) const {
    int tidx = track_index(track_id);
    if (tidx < 0) return false;
    if (tidx == 0) return false;
    if (tracks_[tidx].info.kind != TrackKind::Video) return false;
    if (tracks_[tidx - 1].info.kind != TrackKind::Video) return false ;

    int composite_idx = -1;
    for (int i = 1; i <= tidx; ++i) {
        if (tracks_[i - 1].info.kind == TrackKind::Video &&
            tracks_[i].info.kind == TrackKind::Video) {
            ++composite_idx;
        }
    }
    if (composite_idx < 0 ||
        composite_idx >= static_cast<int>(composites_.size())) {
        return false;
    }

    const char* geo = composites_[composite_idx]->get("geometry");
    if (!geo) return false;
    // Geometry format: "X%/Y%:W%xH%"
    double gx, gy, gw, gh;
    if (std::sscanf(geo, "%lf%%/%lf%%:%lf%%x%lf%%",
                    &gx, &gy, &gw, &gh) != 4) {
        return false;
    }
    x = gx; y = gy; w = gw; h = gh;
    return true;
}

// ---- per-clip geometry (affine filter) ----

namespace {

// Format an affine rect string from percentage coordinates.
// MLT affine accepts "X/Y:WxH" with normalized values (0-1 range).
// We divide by 100 to convert from our 0-100 API to MLT's 0-1 range.
std::string affine_rect_string(double x, double y, double w, double h) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f/%.4f:%.4fx%.4f",
                  x / 100.0, y / 100.0, w / 100.0, h / 100.0);
    return buf;
}

// Parse an affine rect string back to percentage values.
// MLT stores mlt_rect as space-delimited "x y w h o" after parsing
// the colon/slash input format, so the get() return is e.g.
// "0.25 0.25 0.5 0.5 1". The input format "X%/Y%:W%xH%" is parsed
// once by MLT and never returned by get().
bool parse_affine_rect(const std::string& s,
                       double& x, double& y, double& w, double& h) {
    double o = 0;
    if (std::sscanf(s.c_str(), "%lf %lf %lf %lf %lf", &x, &y, &w, &h, &o) >= 4) {
        // MLT divides % values by 100 internally, so 100% becomes 1.0.
        x *= 100.0;
        y *= 100.0;
        w *= 100.0;
        h *= 100.0;
        return true;
    }
    // Also accept colon/slash format in case an older or different
    // serialiser returns it.
    if (std::sscanf(s.c_str(), "%lf/%lf:%lfx%lf", &x, &y, &w, &h) == 4) {
        x *= 100.0;
        y *= 100.0;
        w *= 100.0;
        h *= 100.0;
        return true;
    }
    return false;
}

// Default geometry: clip fills its track (100% width/height at origin).
constexpr double kDefaultGeoX = 0.0;
constexpr double kDefaultGeoY = 0.0;
constexpr double kDefaultGeoW = 100.0;
constexpr double kDefaultGeoH = 100.0;
constexpr double kDefaultRotation = 0.0;

} // namespace

Mlt::Filter* TimelineModel::ensure_affine_filter(ClipId id) const {
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return nullptr;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, "affine");
    if (fidx >= 0) {
        Mlt::Filter* filter = parent.filter(fidx);
        delete cut;
        return filter;
    }

    // Affine filter not yet attached — create and attach it.
    // Use const_cast: clip_set_geometry is logically non-const but
    // ensure_affine_filter is called from both const and non-const
    // contexts (get_geometry is const).
    delete cut;
    return nullptr;
}

bool TimelineModel::geometry_is_default(ClipId id, double x, double y,
                                         double w, double h,
                                         double rot) const {
    return x == kDefaultGeoX && y == kDefaultGeoY &&
           w == kDefaultGeoW && h == kDefaultGeoH &&
           rot == kDefaultRotation;
}

bool TimelineModel::clip_set_geometry(ClipId id, double x, double y,
                                       double w, double h) {
    // Read current rotation so we can detach when everything is default.
    double current_rot = kDefaultRotation;
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, "affine");
    if (fidx >= 0) {
        Mlt::Filter* filter = parent.filter(fidx);
        if (filter) {
            const char* rot_str = filter->get("transition.fix_rotate_z");
            if (rot_str) current_rot = std::atof(rot_str);
            delete filter;
        }
    }
    delete cut;

    if (geometry_is_default(id, x, y, w, h, current_rot)) {
        // Detach the affine filter if present.
        detach_filter(id, "affine");
        return true;
    }

    // Attach affine filter if not already present.
    if (fidx < 0) {
        if (!attach_filter(id, "affine")) return false;
    }

    // Set the rect on the affine filter.
    std::string rect = affine_rect_string(x, y, w, h);
    return set_filter_param(id, "affine", "transition.rect", rect);
}

bool TimelineModel::clip_set_rotation(ClipId id, double degrees) {
    // Read current geometry so we can detach when everything is default.
    double gx = kDefaultGeoX, gy = kDefaultGeoY, gw = kDefaultGeoW, gh = kDefaultGeoH;
    clip_get_geometry(id, gx, gy, gw, gh);

    if (geometry_is_default(id, gx, gy, gw, gh, degrees)) {
        detach_filter(id, "affine");
        return true;
    }

    // Attach affine filter if not already present.
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    int fidx = find_attached_filter(cut->parent(), "affine");
    delete cut;
    if (fidx < 0) {
        if (!attach_filter(id, "affine")) return false;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", degrees);
    return set_filter_param(id, "affine", "transition.fix_rotate_z", buf);
}

bool TimelineModel::clip_get_geometry(ClipId id, double& x, double& y,
                                       double& w, double& h) const {
    x = kDefaultGeoX;
    y = kDefaultGeoY;
    w = kDefaultGeoW;
    h = kDefaultGeoH;

    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return true;  // no clip, return defaults

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, "affine");
    if (fidx < 0) {
        delete cut;
        return true;  // no affine filter, return defaults
    }

    Mlt::Filter* filter = parent.filter(fidx);
    if (!filter) {
        delete cut;
        return true;
    }

    const char* rect_str = filter->get("transition.rect");
    if (rect_str && rect_str[0]) {
        parse_affine_rect(rect_str, x, y, w, h);
    }
    delete filter;
    delete cut;
    return true;
}

// ---- per-clip crop ----

bool TimelineModel::clip_set_crop(ClipId id, int left, int right,
                                   int top, int bottom) {
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, "crop");
    delete cut;

    if (left == 0 && right == 0 && top == 0 && bottom == 0) {
        if (fidx >= 0) detach_filter(id, "crop");
        return true;
    }

    if (fidx < 0) {
        if (!attach_filter(id, "crop")) return false;
    } else {
        // The producer may already carry a crop filter (MLT
        // auto-attaches one for normalization). Stamp our marker
        // so attached_filters() reports it.
        cut = find_cut_producer(id, nullptr);
        if (cut) {
            Mlt::Filter* f = cut->parent().filter(fidx);
            if (f) {
                f->set(kUserFilterProp, 1);
                delete f;
            }
            delete cut;
        }
    }

    auto set_int = [&](const char* param, int value) {
        set_filter_param(id, "crop", param, std::to_string(value));
    };
    set_int("left", left);
    set_int("right", right);
    set_int("top", top);
    set_int("bottom", bottom);
    return true;
}

bool TimelineModel::clip_get_crop(ClipId id, int& left, int& right,
                                   int& top, int& bottom) const {
    left = 0;
    right = 0;
    top = 0;
    bottom = 0;

    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return true;

    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, "crop");
    if (fidx < 0) {
        delete cut;
        return true;
    }

    Mlt::Filter* filter = parent.filter(fidx);
    if (filter) {
        left   = filter->get_int("left");
        right  = filter->get_int("right");
        top    = filter->get_int("top");
        bottom = filter->get_int("bottom");
        delete filter;
    }
    delete cut;
    return true;
}

// ---- per-clip fade ----

namespace {

nlohmann::json fade_keyframes(int64_t start_frame, int64_t end_frame,
                              double start_val, double end_val) {
    return nlohmann::json::array({
        {{"frame", start_frame}, {"value", start_val}, {"type", "linear"}},
        {{"frame", end_frame},   {"value", end_val},   {"type", "linear"}}
    });
}

} // namespace

bool TimelineModel::clip_set_fade_in(ClipId id, int duration_frames) {
    const ClipInfo* ci = clip(id);
    if (!ci) return false;
    int64_t clip_len = ci->length;
    if (clip_len <= 0) return false;

    if (duration_frames <= 0) {
        detach_filter(id, "brightness");
        detach_filter(id, "volume");
        return true;
    }
    if (duration_frames > clip_len) duration_frames = static_cast<int>(clip_len);

    // Ensure brightness filter for video fade.
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    int bfidx = find_attached_filter(cut->parent(), "brightness");
    delete cut;
    if (bfidx < 0) {
        if (!attach_filter(id, "brightness")) return false;
    }

    // Ensure volume filter for audio fade.
    cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    int vfidx = find_attached_filter(cut->parent(), "volume");
    delete cut;
    if (vfidx < 0) {
        if (!attach_filter(id, "volume")) return false;
    }

    nlohmann::json kfs = fade_keyframes(0, duration_frames - 1, 0.0, 1.0);
    bool video_ok = set_filter_param_keyframes(id, "brightness", "level", kfs);
    // Audio fade: volume.level in dB: -60=off, 0=normal.
    nlohmann::json audio_kfs = fade_keyframes(0, duration_frames - 1, -60.0, 0.0);
    bool audio_ok = set_filter_param_keyframes(id, "volume", "level", audio_kfs);
    return video_ok && audio_ok;
}

bool TimelineModel::clip_set_fade_out(ClipId id, int duration_frames) {
    const ClipInfo* ci = clip(id);
    if (!ci) return false;
    int64_t clip_len = ci->length;
    if (clip_len <= 0) return false;

    if (duration_frames <= 0) {
        detach_filter(id, "brightness");
        detach_filter(id, "volume");
        return true;
    }
    if (duration_frames > clip_len) duration_frames = static_cast<int>(clip_len);

    // Ensure brightness filter.
    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    int bfidx = find_attached_filter(cut->parent(), "brightness");
    delete cut;
    if (bfidx < 0) {
        if (!attach_filter(id, "brightness")) return false;
    }

    // Ensure volume filter.
    cut = find_cut_producer(id, nullptr);
    if (!cut) return false;
    int vfidx = find_attached_filter(cut->parent(), "volume");
    delete cut;
    if (vfidx < 0) {
        if (!attach_filter(id, "volume")) return false;
    }

    int64_t start = clip_len - duration_frames;
    nlohmann::json kfs = fade_keyframes(start, clip_len - 1, 1.0, 0.0);
    bool video_ok = set_filter_param_keyframes(id, "brightness", "level", kfs);
    nlohmann::json audio_kfs = fade_keyframes(start, clip_len - 1, 0.0, -60.0);
    bool audio_ok = set_filter_param_keyframes(id, "volume", "level", audio_kfs);
    return video_ok && audio_ok;
}

// ---- analysis.track ----

nlohmann::json TimelineModel::clip_analyze_track(
        ClipId id, const std::string& rect,
        const std::string& algo, int steps) {
    const ClipInfo* ci = clip(id);
    if (!ci || ci->length <= 0) return nlohmann::json::array();

    Mlt::Producer* cut = find_cut_producer(id, nullptr);
    if (!cut) return nlohmann::json::array();

    // Attach opencv.tracker to the parent producer.
    Mlt::Producer& parent = cut->parent();
    int fidx = find_attached_filter(parent, "opencv.tracker");
    if (fidx < 0) {
        delete cut;
        if (!attach_filter(id, "opencv.tracker")) return nlohmann::json::array();
        cut = find_cut_producer(id, nullptr);
        if (!cut) return nlohmann::json::array();
    }
    delete cut;

    // Configure the tracker.
    set_filter_param(id, "opencv.tracker", "rect", rect);
    set_filter_param(id, "opencv.tracker", "algo", algo);
    set_filter_param(id, "opencv.tracker", "steps", std::to_string(steps));
    // Disable overlay rendering during analysis.
    set_filter_param(id, "opencv.tracker", "shape_width", "0");

    // Run a null consumer over the tractor to force every frame
    // through the filter chain. The tracker's analyze() path fires on
    // each frame and accumulates per-frame rects in _results.
    // terminate_on_pause=1 stops cleanly after the last frame.
    tractor_->set_speed(1.0);
    tractor_->seek(0);
    {
        Mlt::Consumer null_consumer(profile_, "null", "");
        null_consumer.set("real_time", -1);
        null_consumer.set("terminate_on_pause", 1);
        null_consumer.connect(*tractor_);
        null_consumer.run();
        null_consumer.stop();
    }

    // Read the results as keyframes.
    return get_filter_param_keyframes(id, "opencv.tracker", "results");
}

} // namespace vedit
