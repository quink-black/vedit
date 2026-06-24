#ifndef VEDIT_DOMAIN_PLAYER_CONTROLLER_H
#define VEDIT_DOMAIN_PLAYER_CONTROLLER_H

#include <cstdint>

#include "core/preview_renderer.h"

namespace vedit {

class ProjectModel;

// Drives preview of a ProjectModel's tractor. Owns a PreviewRenderer.
//
// Two preview modes, transparently selected:
//   - Playing: the realtime consumer drives playback; frames arrive via
//     consumer-frame-show and the playhead advances with the producer.
//   - Paused / scrubbing: a single frame is rendered synchronously at
//     the current playhead. This means there is ALWAYS a picture as soon
//     as the timeline has content — no need to press play to see it.
//
// Threading: all methods on the main thread. The MLT consumer thread is
// owned by PreviewRenderer.
class PlayerController {
public:
    explicit PlayerController(ProjectModel& project);
    ~PlayerController();

    PlayerController(const PlayerController&) = delete;
    PlayerController& operator=(const PlayerController&) = delete;

    // Begin realtime playback from the current position. Idempotent.
    bool play();
    // Pause playback. The playhead stays at its current position and a
    // freeze frame is rendered so the preview keeps showing it.
    void pause();
    // True while the realtime consumer is running.
    bool is_playing() const;

    // Seek to an absolute timeline frame. While paused this renders a
    // freeze frame at the new position immediately (scrubbing).
    void seek(int64_t frame);
    // Step one frame forward / backward (pauses if playing).
    void step_forward();
    void step_backward();

    int64_t position() const;
    int64_t duration() const;

    // Re-render the preview at the current playhead. Call after a
    // timeline edit (append/trim/split/...) so the preview reflects the
    // change without requiring playback. No-op while playing.
    void refresh();

    // Get the frame to display this UI tick. Handles both modes:
    //   - playing: returns the latest realtime frame if one arrived;
    //   - paused: returns the freeze frame (re-rendering if needed).
    // Returns false if there's nothing to show (empty timeline).
    // out_image is owned by the renderer; valid until the next call.
    bool current_frame(int& w, int& h, const uint8_t*& image);

    // Select the audio consumer ("sdl2_audio" default, "rtaudio",
    // "null" for headless). Must be called before play().
    void set_audio_consumer(const char* name) {
        renderer_.set_audio_consumer(name);
    }

private:
    // Render a freeze frame at position_ into the freeze cache. Sets
    // freeze_valid_ on success.
    void render_freeze();

    ProjectModel& project_;
    PreviewRenderer renderer_;
    bool attached_ = false;
    int64_t position_ = 0;

    // Freeze-frame cache (paused preview). Points into the renderer's
    // sync buffer; valid only while freeze_valid_ is true and until the
    // next render call.
    bool freeze_valid_ = false;
    bool freeze_dirty_ = true;  // needs re-render at current position
    int freeze_w_ = 0;
    int freeze_h_ = 0;
    const uint8_t* freeze_image_ = nullptr;
};

} // namespace vedit

#endif
