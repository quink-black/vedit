#ifndef VEDIT_DOMAIN_TIMELINE_MODEL_H
#define VEDIT_DOMAIN_TIMELINE_MODEL_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mlt.h>

#include "ids.h"

namespace vedit {

class MediaLibrary;

enum class TrackKind { Video, Audio };

struct TrackInfo {
    TrackId id;
    TrackKind kind;
    std::string name;
    bool muted = false;
    bool hidden = false;
};

// A clip as the timeline exposes it. Carries OUR stable ids and the
// source range; the underlying MLT playlist clip is reached through
// the mapping maintained inside TimelineModel.
struct ClipInfo {
    ClipId id;
    TrackId track_id;
    int media_index = -1;     // index into MediaLibrary
    int64_t in_point = 0;     // source in (frames)
    int64_t out_point = 0;    // source out (frames, exclusive of length)
    int64_t start = 0;        // timeline position (frames)
    int64_t length = 0;       // frames on timeline
};

// Wraps an MLT tractor + multitrack + per-track playlists. Exposes a
// stable (TrackId, ClipId) addressing scheme so GUI/RPC never touch
// MLT indices directly. MLT objects are mutated through mlt++.
//
// Threading: not thread-safe. All calls must come from the same thread
// (the main thread, where AppController lives). PreviewRenderer reads
// the tractor only after set_producer hands it the producer pointer,
// and MLT's consumer thread pulls frames without touching the playlist
// structure (the tractor is immutable during playback).
class TimelineModel {
public:
    TimelineModel(Mlt::Profile& profile, MediaLibrary& library);
    ~TimelineModel();

    TimelineModel(const TimelineModel&) = delete;
    TimelineModel& operator=(const TimelineModel&) = delete;

    // The underlying producer to feed a consumer. Stays valid for the
    // model's lifetime; PreviewRenderer connects its consumer to this.
    Mlt::Producer* producer() { return tractor_.get(); }

    // --- tracks ---
    TrackId add_track(TrackKind kind, const std::string& name = "");
    bool remove_track(TrackId id);
    int track_count() const;
    const TrackInfo* track(TrackId id) const;
    std::vector<TrackInfo> tracks() const;

    // --- clips ---
    // Append a media item to a track. in/out are source frames; -1
    // means "from start" / "to end". Returns an invalid ClipId on
    // failure.
    ClipId append_clip(TrackId track, int media_index,
                       int64_t in_point = -1, int64_t out_point = -1);

    // Insert at a timeline position (frames from track start).
    ClipId insert_clip(TrackId track, int media_index, int64_t position,
                       int64_t in_point = -1, int64_t out_point = -1);

    // Resolve a clip's media_index from its cut's parent producer.
    int media_index_of(ClipId id) const;

    bool remove_clip(ClipId id);
    bool move_clip(ClipId id, TrackId dest_track, int64_t dest_position);
    bool trim_clip(ClipId id, int64_t new_in, int64_t new_out);
    ClipId split_clip(ClipId id, int64_t at_timeline_position);

    // --- queries ---
    int clip_count() const;
    const ClipInfo* clip(ClipId id) const;
    std::vector<ClipInfo> clips_on_track(TrackId track) const;
    int64_t duration() const;  // total timeline length in frames

private:
    struct TrackState {
        TrackInfo info;
        std::unique_ptr<Mlt::Playlist> playlist;
    };

    TrackState* find_track(TrackId id);
    int track_index(TrackId id) const;
    ClipId next_clip_id();
    TrackId next_track_id();

    // Rebuild our ClipInfo view from the playlists after a mutation.
    // The stable ClipId mapping is preserved by matching on the MLT
    // producer/cut identity, which survives playlist reindexing.
    void rebuild_clip_view();

    Mlt::Profile& profile_;
    MediaLibrary& library_;
    std::unique_ptr<Mlt::Tractor> tractor_;
    std::unique_ptr<Mlt::Multitrack> multitrack_;
    std::vector<TrackState> tracks_;

    int64_t next_clip_id_ = 1;
    int64_t next_track_id_ = 1;

    // Stable clip registry: ClipId -> ClipInfo (with current playlist
    // index refreshed on each rebuild).
    std::unordered_map<int64_t, ClipInfo> clips_;
    // Reverse lookup: (track index, playlist clip index) -> ClipId.
    // Recomputed in rebuild_clip_view().
    std::unordered_map<int64_t, int64_t> position_key_to_clip_;
};

} // namespace vedit

#endif
