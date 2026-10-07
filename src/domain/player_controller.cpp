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
    //
    // Rewind if position_ is at the end, regardless of whether the
    // consumer self-stopped. After pause() tears down the renderer at
    // EOF (running_ = false), has_self_stopped() returns false and the
    // old code skipped the rewind. The producer would seek to dur-1,
    // play one frame, and self-stop again -- "Play after EOF was flaky:
    // the first click did nothing, the second worked."
    int64_t dur = project_.timeline().duration();
    bool at_end = (dur > 0 && position_ >= dur - 1);
    if (renderer_.has_self_stopped()) {
        renderer_.stop();
        if (at_end) position_ = 0;
    } else if (at_end) {
        position_ = 0;
    }
    // Always (re)point at the current tractor and seek to the playhead
    // so playback resumes from where the user left off.
    renderer_.set_producer(producer);
    attached_ = true;
    producer->set_speed(1.0);
    producer->seek(static_cast<int>(position_));
    freeze_valid_ = false;  // realtime frames take over now
    playing_ = renderer_.start();
    return playing_;
}

void PlayerController::pause() {
    if (playing_) {
        // Capture where playback reached before stopping the consumer,
        // so the playhead and freeze frame land on the right frame.
        Mlt::Producer* producer = project_.producer();
        if (producer && producer->is_valid()) {
            position_ = producer->position();
        }
        renderer_.stop();
        playing_ = false;
    } else if (renderer_.has_self_stopped()) {
        // Consumer self-stopped at EOF but we haven't torn it down yet.
        // Clean up so the renderer is ready for the next play/seek.
        renderer_.stop();
    }
    freeze_dirty_ = true;  // need a fresh freeze frame at position_
}

bool PlayerController::is_playing() const {
    return playing_;
}

void PlayerController::seek(int64_t frame) {
    if (frame < 0) frame = 0;
    position_ = frame;
    if (playing_) {
        // If the consumer self-stopped at EOF, tear it down and
        // transition to paused mode so the freeze frame is rendered.
        if (renderer_.has_self_stopped()) {
            renderer_.stop();
            playing_ = false;
            freeze_dirty_ = true;
            render_freeze();
        } else {
            // Live seek: reposition the producer; the consumer will
            // show the new region on its next frames.
            Mlt::Producer* producer = project_.producer();
            if (producer && producer->is_valid()) {
                producer->seek(static_cast<int>(frame));
            }
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

void PlayerController::set_state_changed_listener(std::function<void()> fn) {
    state_changed_ = std::move(fn);
}

void PlayerController::refresh() {
    if (state_changed_) state_changed_();
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
    size_t bytes = 0;
    const uint8_t* img = nullptr;
    if (renderer_.render_frame_at(*producer, pos, w, h, img, bytes)) {
        freeze_w_ = w;
        freeze_h_ = h;
        freeze_bytes_ = bytes;
        freeze_image_ = img;
        freeze_valid_ = true;
        freeze_dirty_ = false;
    } else {
        freeze_valid_ = false;
    }
}

bool PlayerController::current_frame(int& w, int& h, const uint8_t*& image,
                                     size_t& bytes) {
    if (playing_) {
        // Prefer a fresh realtime frame; if none arrived this tick, keep
        // showing whatever the texture already holds (caller decides).
        mlt_image_format fmt = mlt_image_none;
        int fw = 0, fh = 0;
        size_t fbytes = 0;
        const uint8_t* fimg = nullptr;
        int64_t fpos = 0;
        if (renderer_.pull_latest_frame(fmt, fw, fh, fimg, fbytes, fpos)) {
            w = fw; h = fh; image = fimg; bytes = fbytes;
            position_ = fpos;  // keep playhead roughly in sync for UI
            return true;
        }
        return false;
    }
    // Paused: ensure a freeze frame exists and return it.
    if (freeze_dirty_) render_freeze();
    if (freeze_valid_) {
        w = freeze_w_; h = freeze_h_; image = freeze_image_;
        bytes = freeze_bytes_;
        return true;
    }
    return false;
}

} // namespace vedit
