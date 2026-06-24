#ifndef VEDIT_DOMAIN_PLAYER_CONTROLLER_H
#define VEDIT_DOMAIN_PLAYER_CONTROLLER_H

#include <cstdint>

#include "core/preview_renderer.h"

namespace vedit {

class ProjectModel;

// Drives realtime preview of a ProjectModel's tractor. Owns a
// PreviewRenderer connected to the project's producer. Exposes
// transport controls (play/pause/seek/scrub/step) at the domain level.
//
// Threading: all methods called on the main thread. The MLT consumer
// thread is owned by PreviewRenderer; frame hand-off back to the main
// thread is handled there.
class PlayerController {
public:
    explicit PlayerController(ProjectModel& project);
    ~PlayerController();

    PlayerController(const PlayerController&) = delete;
    PlayerController& operator=(const PlayerController&) = delete;

    // Begin realtime playback from the current position. Idempotent.
    bool play();
    // Pause playback. The playhead stays at its current position.
    void pause();
    // True while the consumer is running.
    bool is_playing() const;

    // Seek to an absolute timeline frame.
    void seek(int64_t frame);
    // Step one frame forward / backward (pauses if playing).
    void step_forward();
    void step_backward();

    int64_t position() const { return position_; }
    int64_t duration() const;

    // Pull the latest rendered frame for display. Returns false if no
    // new frame is available. Thin wrapper over PreviewRenderer.
    bool pull_latest_frame(mlt_image_format& fmt, int& w, int& h,
                           const uint8_t*& image, int64_t& pos) {
        return renderer_.pull_latest_frame(fmt, w, h, image, pos);
    }

    // Select the audio consumer ("sdl2_audio" default, "rtaudio",
    // "null" for headless). Must be called before play().
    void set_audio_consumer(const char* name) {
        renderer_.set_audio_consumer(name);
    }

private:
    ProjectModel& project_;
    PreviewRenderer renderer_;
    bool attached_ = false;
    int64_t position_ = 0;
};

} // namespace vedit

#endif
