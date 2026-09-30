#ifndef VEDIT_DOMAIN_PROJECT_MODEL_H
#define VEDIT_DOMAIN_PROJECT_MODEL_H

#include <memory>

#include <Mlt.h>

#include "media_library.h"
#include "player_controller.h"
#include "timeline_model.h"

namespace vedit {

// The single authoritative state for a project. Owns the MLT profile,
// the media library, and the timeline. AppController (T8) wraps this;
// GUI and RPC both go through AppController, never here directly.
class ProjectModel {
public:
    ProjectModel();
    ~ProjectModel();

    Mlt::Profile& profile() { return profile_; }

    // How many times the project has changed, counting a restore as a
    // change. A second client reads it with state.get and passes it back to
    // undo or redo, which is how it finds out that someone else moved the
    // project out from under it instead of silently stepping over that
    // work. It is not a version number for the file on disk and says nothing
    // about who made the change.
    int64_t revision() const { return revision_; }
    void bump_revision() { ++revision_; }
    void set_revision(int64_t value) { revision_ = value; }
    MediaLibrary& library() { return *library_; }
    TimelineModel& timeline() { return *timeline_; }
    PlayerController& player() { return *player_; }

    // The tractor to feed a preview consumer. Same assembly the
    // renderer will export (T9).
    Mlt::Producer* producer() { return timeline_.get() ? timeline_->producer() : nullptr; }

    // Set the output canvas resolution and framerate. Must be called
    // before clips are placed (the profile determines the render
    // dimensions). Width/height are pixels; fps is frames per second.
    void set_profile(int width, int height, double fps);

    // Whether the profile has been explicitly set by the user or RPC.
    // When false, the first media import will sync the profile from
    // the source media (auto-detect). When true, the profile is
    // locked and imports will not overwrite it.
    bool profile_is_user_set() const { return profile_user_set_; }

private:
    Mlt::Profile profile_;
    int64_t revision_ = 0;
    bool profile_user_set_ = false;
    std::unique_ptr<MediaLibrary> library_;
    std::unique_ptr<TimelineModel> timeline_;
    std::unique_ptr<PlayerController> player_;
};

} // namespace vedit

#endif
