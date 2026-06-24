#include "preview_renderer.h"

#include "util/logger.h"

#include <atomic>
#include <cstdio>

namespace vedit {

PreviewRenderer::PreviewRenderer() = default;

PreviewRenderer::~PreviewRenderer() {
    stop();
}

void PreviewRenderer::set_producer(Mlt::Producer* producer) {
    producer_ = producer;
}

void PreviewRenderer::set_audio_consumer(const char* name) {
    if (name && *name) audio_consumer_ = name;
}

bool PreviewRenderer::start() {
    if (running_) return true;
    if (!producer_ || !producer_->is_valid()) {
        Logger::log(Logger::Level::Error, "PreviewRenderer: no valid producer");
        return false;
    }

    // Audio consumer carries the A/V master clock + frame-drop. Video
    // frames are taken from consumer-frame-show, not rendered by MLT.
    // Choice of audio consumer is the caller's (set_audio_consumer):
    //   - "sdl2_audio" (default for real runs with an audio device)
    //   - "rtaudio" (fallback)
    //   - "null" (headless / no audio device: drives the frame pipeline
    //     without opening an audio output; MLT's consumer_null)
    // On a headless box without an audio device, sdl2_audio/rtaudio
    // crash in their audio-output threads, so the smoke-test path uses
    // "null"; real desktop runs use sdl2_audio.
    const char* audio = audio_consumer_.c_str();
    consumer_ = new Mlt::Consumer(producer_->get_profile(), audio);
    if (!consumer_ || !consumer_->is_valid()) {
        Logger::log(Logger::Level::Error,
                    "PreviewRenderer: audio consumer unavailable");
        delete consumer_;
        consumer_ = nullptr;
        return false;
    }

    consumer_->listen("consumer-frame-show", this,
                      reinterpret_cast<mlt_listener>(on_frame_show));
    consumer_->listen("consumer-stopped", this,
                      reinterpret_cast<mlt_listener>(on_consumer_stopped));

    consumer_->set("real_time", 1);
    consumer_->set("buffer", 25);
    consumer_->set("prefill", 1);
    // Stop the consumer automatically when the producer ends, so the
    // app does not idle with the audio thread alive (mirrors MLT's
    // examples/play.cpp).
    consumer_->set("terminate_on_pause", 1);

    consumer_->connect(*producer_);
    if (consumer_->start() < 0) {
        Logger::log(Logger::Level::Error, "PreviewRenderer: consumer start failed");
        delete consumer_;
        consumer_ = nullptr;
        return false;
    }

    running_ = true;
    self_stopped_.store(false, std::memory_order_release);
    Logger::log(Logger::Level::Info, "PreviewRenderer started");
    return true;
}

void PreviewRenderer::stop() {
    if (!running_) return;
    running_ = false;
    // Gate the callback before joining, so any in-flight or final
    // consumer-frame-show event bails out instead of touching state.
    stopping_.store(true, std::memory_order_release);
    if (consumer_) {
        consumer_->stop();
        // stop() joins the consumer thread; it is now safe to delete.
        delete consumer_;
        consumer_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(frame_mutex_);
    has_pending_ = false;
    pending_buffer_.clear();
    delivered_buffer_.clear();
    stopping_.store(false, std::memory_order_release);
    Logger::log(Logger::Level::Info, "PreviewRenderer stopped");
}

void PreviewRenderer::on_consumer_stopped(mlt_consumer /*c*/, PreviewRenderer* self,
                                         mlt_event_data /*data*/) {
    // MLT stopped the consumer itself (terminate_on_pause at clip end).
    // Flag it so the main loop can exit cleanly. The main thread still
    // owns the explicit stop()/teardown.
    self->self_stopped_.store(true, std::memory_order_release);
    Logger::log(Logger::Level::Info, "PreviewRenderer: consumer self-stopped (end of clip)");
}

void PreviewRenderer::on_frame_show(mlt_consumer /*consumer*/,
                                    PreviewRenderer* self,
                                    mlt_event_data data) {
    // Drop callbacks that arrive during shutdown. stop() sets this
    // before joining the consumer thread, so once we see it true the
    // instance state is being torn down and must not be touched.
    if (self->stopping_.load(std::memory_order_acquire)) return;
    Mlt::Frame frame = Mlt::EventData(data).to_frame();
    if (!frame.is_valid()) return;
    if (!frame.get_int("rendered")) return;
    if (self->stopping_.load(std::memory_order_acquire)) return;
    self->handle_frame(frame);
}

void PreviewRenderer::handle_frame(Mlt::Frame& frame) {
    mlt_image_format format = mlt_image_rgba;
    int width = 0;
    int height = 0;
    uint8_t* image = frame.get_image(format, width, height);
    if (!image || width <= 0 || height <= 0) return;

    const size_t bytes = static_cast<size_t>(width) * height *
                         (format == mlt_image_rgba ? 4 : 3);

    std::lock_guard<std::mutex> lk(frame_mutex_);
    // Drop-on-overflow: if the main thread hasn't consumed the previous
    // pending frame, we simply overwrite it. This is the correct policy
    // for a live preview -- newest frame wins, latency stays bounded.
    pending_format_ = format;
    pending_width_ = width;
    pending_height_ = height;
    pending_position_ = frame.get_position();
    pending_buffer_.assign(image, image + bytes);
    has_pending_ = true;

    // First-frame + periodic progress logging so the preview path is
    // observable in a headless smoke test (ImGui text isn't visible
    // without a display). Uses relaxed atomics; logging only.
    static std::atomic<int64_t> frame_counter{0};
    int64_t n = frame_counter.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
        Logger::log(Logger::Level::Info, "PreviewRenderer: first frame received");
    } else if (n % 50 == 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "PreviewRenderer: %lld frames (pos %lld)",
                      static_cast<long long>(n),
                      static_cast<long long>(pending_position_));
        Logger::log(Logger::Level::Debug, buf);
    }
}

bool PreviewRenderer::pull_latest_frame(mlt_image_format& out_format,
                                        int& out_width, int& out_height,
                                        const uint8_t*& out_image,
                                        int64_t& out_position) {
    std::lock_guard<std::mutex> lk(frame_mutex_);
    if (!has_pending_) return false;
    // Move pending into the delivered buffer so the caller can read it
    // safely until the next pull.
    delivered_buffer_ = std::move(pending_buffer_);
    pending_buffer_.clear();
    has_pending_ = false;

    out_format = pending_format_;
    out_width = pending_width_;
    out_height = pending_height_;
    out_position = pending_position_;
    out_image = delivered_buffer_.data();
    return true;
}

bool PreviewRenderer::render_frame_at(Mlt::Producer& producer, int64_t position,
                                      int& out_width, int& out_height,
                                      const uint8_t*& out_image) {
    if (!producer.is_valid()) return false;
    // Pull one frame directly from the producer at the requested
    // position. No realtime consumer involved.
    producer.seek(static_cast<int>(position));
    Mlt::Frame* frame = producer.get_frame();
    if (!frame || !frame->is_valid()) {
        delete frame;
        return false;
    }
    // Force RGBA so the GUI can upload it as a GL texture directly.
    mlt_image_format fmt = mlt_image_rgba;
    int w = 0, h = 0;
    uint8_t* image = frame->get_image(fmt, w, h);
    if (!image || w <= 0 || h <= 0) {
        delete frame;
        return false;
    }
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    sync_buffer_.assign(image, image + bytes);
    delete frame;

    out_width = w;
    out_height = h;
    out_image = sync_buffer_.data();
    return true;
}

} // namespace vedit
