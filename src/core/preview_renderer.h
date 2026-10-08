#ifndef VEDIT_CORE_PREVIEW_RENDERER_H
#define VEDIT_CORE_PREVIEW_RENDERER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Mlt.h>

namespace vedit {

// MLT hands back the format it can produce, not the format asked for:
// mlt_frame_get_image() converts only when the frame carries a
// converter, and once the get_image stack is empty it returns the
// cached image in whatever format that image holds. Requesting
// mlt_image_rgba can therefore still deliver mlt_image_rgb, which is
// 3 bytes per pixel.
//
// A frame is published in the format MLT produced it in -- 24-bit RGB
// or 32-bit RGBA -- never converted. The GUI picks the SDL pixel
// format that matches the buffer it was handed, so SDL_UpdateTexture()
// reads exactly as many bytes as the buffer holds. Publishing a 24-bit
// frame as if it were 32-bit made that read run off the end of the
// buffer and crash the app in the Metal backend.
//
// Copies `src` into `out` when `format` is one the GUI can upload, and
// reports false for anything else -- YUV, for instance -- without
// touching `out`, so the caller keeps showing the last frame it could
// publish.
//
// Byte order, measured on this machine against MLT 7.38: both a
// "colour:red" producer and a decoded H.264 clip deliver rgba as
// R,G,B,A and rgb as R,G,B, so the two formats differ only in whether
// the alpha byte is present.
bool copy_preview_frame(const uint8_t* src, mlt_image_format format,
                        int width, int height, std::vector<uint8_t>& out);

// Bytes per pixel of a format copy_preview_frame() will publish, or 0
// for one it rejects. The GUI uses this to size the upload.
int preview_bytes_per_pixel(mlt_image_format format);

// Owns the realtime preview path over an MLT tractor/producer:
//   - audio goes to an rtaudio consumer (which carries the A/V master
//     clock and the real-time frame-drop heuristic);
//   - video frames are pulled on the MLT consumer thread via the
//     "consumer-frame-show" event, copied into a back buffer, and
//     handed to the main thread for GL upload + draw.
//
// Threading contract: on_frame_show runs on the MLT consumer thread.
// pull_latest_frame() is called on the main thread. The hand-off is a
// single mutex + drop-on-overflow policy: if the main thread has not
// consumed the previous frame, the new frame overwrites the pending
// slot. This bounds latency without blocking either thread.
//
// T3 scope: drives a single Mlt::Producer (one file). The tractor /
// timeline wiring lands in T4-T6; PreviewRenderer then just connects
// to whatever producer the ProjectModel exposes.
class PreviewRenderer {
public:
    PreviewRenderer();
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    // Set the producer to preview. Must be called before start(). The
    // renderer connects its consumer to this producer. Ownership of the
    // producer stays with the caller (ProjectModel).
    void set_producer(Mlt::Producer* producer);

    // Audio consumer name: "rtaudio" (default) or "sdl2_audio". Both
    // carry the A/V master clock; the choice is a runtime trade-off.
    void set_audio_consumer(const char* name);

    // Begin realtime playback. Returns false if no producer is set or
    // the consumer failed to start.
    bool start();

    // Stop playback and the consumer thread.
    void stop();

    // True if the consumer is still running. Becomes false when MLT
    // stops the consumer itself (e.g. terminate_on_pause at clip end)
    // or after an explicit stop().
    bool is_running() const {
        return running_ && !self_stopped_.load(std::memory_order_acquire);
    }

    // True if MLT stopped the consumer itself at clip end while the
    // renderer still believes it is running. The owner must call stop()
    // to reset this before start() can rebuild the consumer.
    bool has_self_stopped() const {
        return running_ && self_stopped_.load(std::memory_order_acquire);
    }

    // Consumer-stopped callback (for terminate_on_pause end detection).
    static void on_consumer_stopped(mlt_consumer /*c*/, PreviewRenderer* self,
                                    mlt_event_data /*data*/);

    // Called on the main thread. If a new frame has arrived since the
    // last call, copies it into out_* and returns true. The image
    // buffer referenced by out_image is owned by the renderer and
    // remains valid only until the next pull_latest_frame() call.
    //
    // The frame keeps the format MLT produced it in -- out_format is
    // mlt_image_rgb or mlt_image_rgba -- and out_bytes is out_width *
    // out_height * preview_bytes_per_pixel(out_format). A caller that
    // uploads the buffer sizes the upload from out_bytes, so it can
    // never read past the end of it.
    bool pull_latest_frame(mlt_image_format& out_format,
                           int& out_width, int& out_height,
                           const uint8_t*& out_image,
                           size_t& out_bytes,
                           int64_t& out_position);

    // Synchronously render a single frame at `position` from `producer`.
    // Returns false on failure, including when MLT answers in a format
    // the GUI cannot upload. The returned buffer is owned by the
    // renderer and valid until the next render_frame_at() or
    // pull_latest_frame() call; it holds out_width * out_height pixels
    // in the format MLT produced.
    //
    // This does NOT use the realtime consumer — it pulls one frame
    // directly from the producer. Used for paused preview, scrubbing,
    // and "show the frame as soon as a clip is added" (no playback
    // required to see a picture). Must be called on the main thread
    // and not while the realtime consumer is running.
    bool render_frame_at(Mlt::Producer& producer, int64_t position,
                         int& out_width, int& out_height,
                         const uint8_t*& out_image,
                         size_t& out_bytes);

private:
    // MLT consumer-thread callback. Static because mlt_listener is a C
    // function pointer; it forwards to the instance via the user-data
    // argument (the renderer pointer set in listen()).
    static void on_frame_show(mlt_consumer consumer,
                              PreviewRenderer* self,
                              mlt_event_data data);

    void handle_frame(Mlt::Frame& frame);

    Mlt::Consumer* consumer_ = nullptr;
    Mlt::Producer* producer_ = nullptr;
    std::string audio_consumer_ = "sdl2_audio";

    // Pending frame hand-off. Updated on the MLT thread, read+cleared
    // on the main thread. Guarded by frame_mutex_.
    std::mutex frame_mutex_;
    bool has_pending_ = false;
    mlt_image_format pending_format_ = mlt_image_none;
    int pending_width_ = 0;
    int pending_height_ = 0;
    int64_t pending_position_ = 0;
    std::vector<uint8_t> pending_buffer_;  // latest decoded image bytes

    // The "current" buffer handed to the main thread via the last
    // pull_latest_frame(). Kept alive so the caller can finish drawing
    // it; overwritten on the next pull.
    std::vector<uint8_t> delivered_buffer_;

    // Buffer for synchronous single-frame rendering (render_frame_at).
    // Main-thread only; never touched by the consumer thread.
    std::vector<uint8_t> sync_buffer_;

    bool running_ = false;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> self_stopped_{false};
};

} // namespace vedit

#endif
