#include "player_controller.h"

#include "project_model.h"
#include "timeline_model.h"
#include "util/logger.h"

namespace vedit {

PlayerController::PlayerController(ProjectModel& project)
    : project_(project), renderer_() {}

PlayerController::~PlayerController() {
    pause();
}

bool PlayerController::play() {
    // (Re)attach the renderer to the project's current tractor in case
    // the timeline changed since the last play().
    Mlt::Producer* producer = project_.producer();
    if (!producer || !producer->is_valid()) {
        Logger::log(Logger::Level::Warn, "PlayerController: no producer to play");
        return false;
    }
    if (!attached_) {
        renderer_.set_producer(producer);
        attached_ = true;
    }
    return renderer_.start();
}

void PlayerController::pause() {
    renderer_.stop();
}

bool PlayerController::is_playing() const {
    return renderer_.is_running();
}

void PlayerController::seek(int64_t frame) {
    // Seeking while playing requires stopping/restarting the consumer
    // in M1's simple model. ProjectModel's tractor is seeked via its
    // producer interface.
    Mlt::Producer* producer = project_.producer();
    if (!producer) return;
    bool was_playing = is_playing();
    if (was_playing) pause();
    producer->seek(static_cast<int>(frame));
    position_ = frame;
    if (was_playing) play();
}

void PlayerController::step_forward() {
    bool was_playing = is_playing();
    if (was_playing) pause();
    seek(position_ + 1);
}

void PlayerController::step_backward() {
    bool was_playing = is_playing();
    if (was_playing) pause();
    if (position_ > 0) seek(position_ - 1);
}

int64_t PlayerController::duration() const {
    return project_.timeline().duration();
}

} // namespace vedit
