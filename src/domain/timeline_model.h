#ifndef VEDIT_DOMAIN_TIMELINE_MODEL_H
#define VEDIT_DOMAIN_TIMELINE_MODEL_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mlt.h>

#include <nlohmann/json_fwd.hpp>

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

    // Ensure a composite transition is planted so a higher video track
    // overlays the base track. Idempotent: replanting on track add is
    // safe (MLT deduplicates by service identity). M1 plants one
    // composite per video track above track 0.
    void ensure_composite_transitions();

    // --- tracks ---
    TrackId add_track(TrackKind kind, const std::string& name = "");
    bool remove_track(TrackId id);

    // Mute silences audio on the track (MLT hide bit 2). Hidden hides
    // video on the track (MLT hide bit 1). Both can be set together.
    // The TrackInfo flags are updated and the MLT playlist producer
    // property is set so the effect is visible in preview and export.
    bool set_track_muted(TrackId id, bool muted);
    bool set_track_hidden(TrackId id, bool hidden);
    int track_count() const;
    const TrackInfo* track(TrackId id) const;
    std::vector<TrackInfo> tracks() const;

    // --- clips ---
    // Append a media item to a track. in/out are source frames; -1
    // means "from start" / "to end". Returns an invalid ClipId on
    // failure.
    ClipId append_clip(TrackId track, int media_index,
                       int64_t in_point = -1, int64_t out_point = -1);

    // Insert at a timeline position (frames from track start). If
    // `position` falls inside an existing clip, the clip is appended
    // after the last clip instead (M1 does not auto-split on insert;
    // use split_clip + insert into the gap for that). Pads with blank
    // when `position` is past the current end.
    ClipId insert_clip(TrackId track, int media_index, int64_t position,
                       int64_t in_point = -1, int64_t out_point = -1);

    // Resolve a clip's media_index from its cut's parent producer.
    int media_index_of(ClipId id) const;

    // Resolve a clip's playlist index (including blanks) within its
    // track. Returns -1 if not found. Used for add_transition which
    // needs the raw MLT playlist index.
    int playlist_index_of(ClipId id) const;

    // Count clips referencing a given library media index. Used by
    // MediaLibrary removal to refuse dropping a source still in use.
    int count_clips_for_media(int media_index) const;

    // Called by AppController after a MediaLibrary item at
    // `removed_index` has been dropped and the tail re-indexed down by
    // one. Walks every playlist clip and decrements any stamped
    // media_index > removed_index so future lookups still resolve to
    // the right library producer. Clips at exactly `removed_index`
    // must not exist (caller guarantees count_clips_for_media == 0).
    void on_media_removed(int removed_index);

    bool remove_clip(ClipId id);
    bool move_clip(ClipId id, TrackId dest_track, int64_t dest_position);
    bool trim_clip(ClipId id, int64_t new_in, int64_t new_out);
    ClipId split_clip(ClipId id, int64_t at_timeline_position);

    // --- advanced editing (M4) ---
    // Insert a clip at a timeline position, replacing any content
    // (blanks) at that position. Unlike insert_clip, this overwrites
    // instead of shifting existing clips.
    ClipId overwrite_clip(TrackId track, int media_index,
                          int64_t position,
                          int64_t in_point = -1, int64_t out_point = -1);
    // Replace a clip with blank space of the same duration, preserving
    // timeline timing for all other clips.
    bool lift_clip(ClipId id);
    // Wrap a clip in a timewarp producer to change its playback speed.
    // speed=1.0 is normal; 2.0 is double speed (half duration);
    // 0.5 is half speed (double duration). Negative = reverse.
    bool change_clip_speed(ClipId id, double speed);
    // Add a transition (cross-fade) between two adjacent clips on the
    // same track. length is the transition duration in frames.
    bool add_transition(TrackId track, int clip_index,
                        int length, const std::string& type = "luma");
    // Detach audio from a video clip: create an audio-only track and
    // place the clip's audio there. Returns the new clip id.
    ClipId detach_audio(ClipId id);
    // Enable or disable audio/video output for a specific clip.
    // When audio is disabled, the clip produces no audio frames (sets
    // audio_index=-1 on the MLT cut). Likewise for video.
    bool set_clip_audio_enabled(ClipId id, bool enabled);
    bool set_clip_video_enabled(ClipId id, bool enabled);

    // --- queries ---
    int clip_count() const;
    const ClipInfo* clip(ClipId id) const;
    std::vector<ClipInfo> clips_on_track(TrackId track) const;
    int64_t duration() const;  // total timeline length in frames

    // --- composition ---
    // (Re)plant composite transitions between consecutive video tracks
    // so track N+1 overlays track N. Call after add/remove_track.
    // Idempotent: clears existing tractor transitions first.
    void replant_compositions();

    // --- filters (M2) ---
    // Attach/detach an MLT filter (by service name) to/from a clip.
    // Returns false if the clip or filter is not found, or the MLT
    // filter cannot be created. The filter's parameters can then be
    // read/written via set_filter_param / get_filter_param.
    bool attach_filter(ClipId clip_id, const std::string& filter_name);
    bool detach_filter(ClipId clip_id, const std::string& filter_name);
    // Set/get a named parameter on an attached filter. The filter must
    // already be attached. `value` is the scalar (P2 {value} shape).
    bool set_filter_param(ClipId clip_id, const std::string& filter_name,
                          const std::string& param, const std::string& value);
    std::string get_filter_param(ClipId clip_id,
                                 const std::string& filter_name,
                                 const std::string& param);

    // Keyframe-aware variants (M3 / P2).
    // set_filter_param_keyframes replaces any existing animation on the
    // parameter with the given keyframes array. Each keyframe is
    // {frame: int, value: scalar, type: "linear"|"discrete"|"smooth"}.
    // Returns false if the clip or filter is not found.
    bool set_filter_param_keyframes(ClipId clip_id,
                                    const std::string& filter_name,
                                    const std::string& param,
                                    const nlohmann::json& keyframes);
    // get_filter_param_keyframes returns either
    // {"value": <scalar>} or {"keyframes": [{frame, value, type}, ...]}.
    nlohmann::json get_filter_param_keyframes(ClipId clip_id,
                                              const std::string& filter_name,
                                              const std::string& param);

    // Per-track filters (planted on tractor field, apply to all clips
    // on the track). Same param API as per-clip, prefixed track_.
    bool track_attach_filter(TrackId track_id, const std::string& filter_name);
    bool track_detach_filter(TrackId track_id, const std::string& filter_name);
    bool track_set_filter_param(TrackId track_id,
                                const std::string& filter_name,
                                const std::string& param,
                                const std::string& value);
    std::string track_get_filter_param(TrackId track_id,
                                       const std::string& filter_name,
                                       const std::string& param);

    // Keyframe-aware per-track variants (M3 / P2).
    bool track_set_filter_param_keyframes(TrackId track_id,
                                          const std::string& filter_name,
                                          const std::string& param,
                                          const nlohmann::json& keyframes);
    nlohmann::json track_get_filter_param_keyframes(TrackId track_id,
                                                    const std::string& filter_name,
                                                    const std::string& param);

    // Enumerate attached filters for state.get projection.
    struct AttachedFilterInfo {
        std::string target_type;  // "clip" or "track"
        int64_t target_id;
        std::string filter_name;
        // Each entry is {param_name, param_value} where param_value is
        // a JSON object: {"value": <scalar>} or {"keyframes": [...]}.
        std::vector<std::pair<std::string, nlohmann::json>> params;
    };
    std::vector<AttachedFilterInfo> attached_filters() const;

    // --- canvas composition (composite geometry) ---
    // Set the geometry rect for a video track's composite transition.
    // x, y, w, h are percentages (0-100) of the profile.
    bool set_composite_geometry(TrackId track_id,
                                 double x, double y, double w, double h);
    // Get the geometry rect for a video track's composite transition.
    // Returns false if the track has no composite (track 0, non-video).
    bool get_composite_geometry(TrackId track_id,
                                 double& x, double& y,
                                 double& w, double& h) const;

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

    // Find the playlist cut producer for a ClipId. Returns new'd
    // producer (caller owns) or nullptr.
    Mlt::Producer* find_cut_producer(ClipId id, int* track_idx = nullptr) const;

    Mlt::Profile& profile_;
    MediaLibrary& library_;
    std::unique_ptr<Mlt::Tractor> tractor_;
    std::unique_ptr<Mlt::Multitrack> multitrack_;
    std::vector<TrackState> tracks_;
    // Composite transitions planted between consecutive video tracks.
    // Tracked so replant_compositions can disconnect them first.
    std::vector<std::unique_ptr<Mlt::Transition>> composites_;

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
