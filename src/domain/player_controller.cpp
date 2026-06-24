#include "player_controller.h"

#include "project_model.h"
#include "timeline_model.h"
#include "util/logger.h"

namespace vedit {

PlayerController::PlayerController(ProjectModel& project)
    : project_(project), renderer_() {}

PlayerController::~PlayerController() {
    if (is_playing()) renderer_.stop();
}

bool PlayerController::play() {
    Mlt::Producer* producer = project_.producer();
    if (!producer || !producer->is_valid()) {
        Logger::log(Logger::Level::Warn, "PlayerController: no producer to play");
        return false;
    }
    // The realtime consumer self-stops at clip end (terminate_on_pause).
    // When that happens running_ stays true but is_running() is false,
    // so PreviewRenderer::start() would early-return and playback would
    // never resume. Tear the consumer down first so start() rebuilds it.
    if (renderer_.has_self_stopped()) {
        renderer_.stop();
        // Reached the end last time: rewind so Play restarts from the
        // top instead of sitting on the final frame.
        int64_t dur = project_.timeline().duration();
        if (dur > 0 && position_ >= dur - 1) position_ = 0;
    }
    // Always (re)point at the current tractor and seek to the playhead
    // so playback resumes from where the user left off.
    renderer_.set_producer(producer);
    attached_ = true;
    producer->set_speed(1.0);
    producer->seek(static_cast<int>(position_));
    freeze_valid_ = false;  // realtime frames take over now
    return renderer_.start();
}

void PlayerController::pause() {
    if (is_playing()) {
        // Capture where playback reached before stopping the consumer,
        // so the playhead and freeze frame land on the right frame.
        Mlt::Producer* producer = project_.producer();
        if (producer && producer->is_valid()) {
            position_ = producer->position();
        }
        renderer_.stop();
    }
    freeze_dirty_ = true;  // need a fresh freeze frame at position_
}

bool PlayerController::is_playing() const {
    return renderer_.is_running();
}

void PlayerController::seek(int64_t frame) {
    if (frame < 0) frame = 0;
    position_ = frame;
    if (is_playing()) {
        // Live seek: reposition the producer; the consumer will show the
        // new region on its next frames.
        Mlt::Producer* producer = project_.producer();
        if (producer && producer->is_valid()) {
            producer->seek(static_cast<int>(frame));
        }
    } else {
        // Paused: render a freeze frame at the new position (scrubbing).
        freeze_dirty_ = true;
        render_freeze();
    }
}

void PlayerController::step_forward() {
    if (is_playing()) pause();
    seek(position_ + 1);
}

void PlayerController::step_backward() {
    if (is_playing()) pause();
    if (position_ > 0) seek(position_ - 1);
}

int64_t PlayerController::position() const {
    if (is_playing()) {
        Mlt::Producer* producer = project_.producer();
        if (producer && producer->is_valid()) {
            return producer->position();
        }
    }
    return position_;
}

int64_t PlayerController::duration() const {
    return project_.timeline().duration();
}

void PlayerController::refresh() {
    // A timeline edit happened. If paused, re-render the freeze frame so
    // the preview reflects the edit immediately.
    if (!is_playing()) {
        freeze_dirty_ = true;
        render_freeze();
    }
}

void PlayerController::render_freeze() {
    if (!freeze_dirty_) return;
    Mlt::Producer* producer = project_.producer();
    if (!producer || !producer->is_valid() ||
        project_.timeline().duration() <= 0) {
        freeze_valid_ = false;
        return;
    }
    int64_t pos = position_;
    int64_t dur = project_.timeline().duration();
    if (pos >= dur) pos = dur - 1;
    if (pos < 0) pos = 0;
    int w = 0, h = 0;
    const uint8_t* img = nullptr;
    if (renderer_.render_frame_at(*producer, pos, w, h, img)) {
        freeze_w_ = w;
        freeze_h_ = h;
        freeze_image_ = img;
        freeze_valid_ = true;
        freeze_dirty_ = false;
    } else {
        freeze_valid_ = false;
    }
}

bool PlayerController::current_frame(int& w, int& h, const uint8_t*& image) {
    if (is_playing()) {
        // Prefer a fresh realtime frame; if none arrived this tick, keep
        // showing whatever the texture already holds (caller decides).
        mlt_image_format fmt = mlt_image_none;
        int fw = 0, fh = 0;
        const uint8_t* fimg = nullptr;
        int64_t fpos = 0;
        if (renderer_.pull_latest_frame(fmt, fw, fh, fimg, fpos)) {
            w = fw; h = fh; image = fimg;
            position_ = fpos;  // keep playhead roughly in sync for UI
            return true;
        }
        return false;
    }
    // Paused: ensure a freeze frame exists and return it.
    if (freeze_dirty_) render_freeze();
    if (freeze_valid_) {
        w = freeze_w_; h = freeze_h_; image = freeze_image_;
        return true;
    }
    return false;
}

} // namespace vedit
