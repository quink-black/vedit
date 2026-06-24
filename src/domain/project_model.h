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
    MediaLibrary& library() { return *library_; }
    TimelineModel& timeline() { return *timeline_; }
    PlayerController& player() { return *player_; }

    // The tractor to feed a preview consumer. Same assembly the
    // renderer will export (T9).
    Mlt::Producer* producer() { return timeline_.get() ? timeline_->producer() : nullptr; }

private:
    Mlt::Profile profile_;
    std::unique_ptr<MediaLibrary> library_;
    std::unique_ptr<TimelineModel> timeline_;
    std::unique_ptr<PlayerController> player_;
};

} // namespace vedit

#endif
