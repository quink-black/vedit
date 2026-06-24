#ifndef VEDIT_DOMAIN_CLIP_ID_H
#define VEDIT_DOMAIN_CLIP_ID_H

#include <cstdint>

namespace vedit {

// Stable, app-owned identifier for a clip on the timeline.
//
// MLT playlists are reindexed on every mutation (insert/remove/split/
// move), so a playlist index is not a stable handle. We hand out
// monotonic ClipIds from the TimelineModel and maintain the
// ClipId <-> (track, playlist index) mapping internally. External code
// (GUI, RPC, agent) only ever sees ClipIds.
struct ClipId {
    int64_t value = 0;
    bool valid() const { return value > 0; }
    bool operator==(const ClipId& o) const { return value == o.value; }
    bool operator!=(const ClipId& o) const { return value != o.value; }
};

// Stable identifier for a track.
struct TrackId {
    int64_t value = 0;
    bool valid() const { return value > 0; }
    bool operator==(const TrackId& o) const { return value == o.value; }
    bool operator!=(const TrackId& o) const { return value != o.value; }
};

} // namespace vedit

#endif
