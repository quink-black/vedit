#include "export_service.h"

#include "util/logger.h"

#include <MltTractor.h>

#include <cstdio>
#include <utility>

namespace vedit {

namespace {

// Static callback for the consumer-frame-show event during export.
// The userdata pointer is a pointer-to-pair of (total_frames, ProgressFn).
void on_export_frame(mlt_consumer, void* ud, mlt_event_data data) {
    if (!ud) return;
    auto* pair = static_cast<std::pair<int, ExportService::ProgressFn*>*>(ud);
    if (!pair->second || !*pair->second) return;
    Mlt::Frame frame = Mlt::EventData(data).to_frame();
    if (!frame.is_valid() || !frame.get_int("rendered")) return;
    int pos = frame.get_position();
    int total = pair->first;
    if (total > 0) {
        (*pair->second)(static_cast<double>(pos) / total);
    }
}

} // namespace

bool ExportService::render(Mlt::Producer& producer, Mlt::Profile& profile,
                           const std::string& output_path, ProgressFn on_progress) {
    // Consumer ctor: profile, "avformat", output_path.
    Mlt::Consumer consumer(profile, "avformat", output_path.c_str());
    if (!consumer.is_valid()) {
        Logger::log(Logger::Level::Error, "ExportService: avformat consumer unavailable");
        return false;
    }

    // M1 preset: H.264 video, AAC audio, MP4 container.
    // libx264 is GPL-licensed and not included in all ffmpeg builds
    // (e.g. vcpkg on Windows). Fall back to the built-in mpeg4 encoder
    // which is available in every ffmpeg build.
    consumer.set("real_time", -1);        // render as fast as possible
    consumer.set("terminate_on_pause", 1);
#ifdef _WIN32
    consumer.set("vcodec", "mpeg4");
#else
    consumer.set("vcodec", "libx264");
#endif
    consumer.set("acodec", "aac");
    consumer.set("f", "mp4");
    consumer.set("crf", 23);
    consumer.set("preset", "medium");
    consumer.set("threads", 0);           // auto

    // Reset the producer to a clean render state. The preview consumer
    // runs with terminate_on_pause and, when it reaches the end of the
    // timeline, leaves the producer paused at the last frame with
    // speed 0. An export consumer (also terminate_on_pause) inheriting
    // speed 0 sees "already paused" and stops before emitting a single
    // frame, producing a valid-but-empty 262-byte MP4. Restore forward
    // playback and rewind to the start so export always renders the
    // whole timeline regardless of where preview left the playhead.
    producer.set_speed(1.0);
    producer.seek(0);

    // Compute the effective export length. The tractor's get_length()
    // returns the maximum across all tracks. When tracks have different
    // durations (e.g., differing in/out points), the tractor produces
    // blank frames on the shorter track after it ends. Clamp to the
    // minimum track length so every track has content for the full
    // export. Set on the producer before connect() so the consumer
    // picks up the corrected length.
    int total = producer.get_length();
    int orig_out = producer.get_int("out");
    {
        Mlt::Tractor tractor(producer);
        if (tractor.is_valid()) {
            Mlt::Multitrack* mt = tractor.multitrack();
            if (mt && mt->is_valid()) {
                int min_len = total;
                for (int i = 0; i < mt->count(); ++i) {
                    Mlt::Producer* track = mt->track(i);
                    if (track && track->is_valid()) {
                        int len = track->get_length();
                        if (len > 0 && len < min_len) min_len = len;
                    }
                }
                if (min_len > 0 && min_len < total) total = min_len;
            }
            delete mt;
        }
    }
    producer.set("out", total - 1);

    consumer.connect(producer);

    // Progress reporting via consumer-frame-show.
    // Re-read total in case producer.set("out", ...) changed it.
    int total2 = producer.get_length();
    std::pair<int, ProgressFn*> progress_data(total2, &on_progress);
    if (on_progress) {
        consumer.listen("consumer-frame-show", &progress_data,
                        reinterpret_cast<mlt_listener>(on_export_frame));
    }

    consumer.run();  // synchronous -- blocks until done or error
    // run() returns when terminate_on_pause fires, but the avformat
    // consumer's encoder worker threads are only joined and their
    // buffers freed by an explicit stop(). Without it FFmpeg's
    // per-thread state leaks and teardown can race. Always stop.
    consumer.stop();
    // Restore the original out point if we modified it.
    if (orig_out >= 0) producer.set("out", orig_out);
    return true;
}

bool ExportService::get_frame(Mlt::Producer& producer, Mlt::Profile& profile,
                              int64_t position, const std::string& image_path) {
    // A prior consumer (preview or another get_frame call) leaves the
    // producer paused with speed 0. Without restoring forward playback
    // the image2 consumer (terminate_on_pause) stops before emitting a
    // frame — same defect as the empty-export bug. See
    // lessons-mistakes-to-avoid.md #10.
    producer.set_speed(1.0);
    producer.seek(static_cast<int>(position));

    Mlt::Consumer consumer(profile, "avformat", image_path.c_str());
    if (!consumer.is_valid()) {
        Logger::log(Logger::Level::Error, "ExportService: get_frame — avformat unavailable");
        return false;
    }
    consumer.set("real_time", -1);
    consumer.set("terminate_on_pause", 1);
#ifdef _WIN32
    // vcpkg's minimal ffmpeg build may not include the PNG encoder.
    // Use mjpeg (always available) instead — a single-frame image2
    // muxer output, same semantics, different byte format.
    consumer.set("vcodec", "mjpeg");
#else
    consumer.set("vcodec", "png");
#endif
    consumer.set("f", "image2");
    // Without -update, the image2 muxer rejects a plain filename
    // (it expects a %03d sequence pattern). We write one still frame.
    consumer.set("update", 1);
    // One frame only — seek above positions the producer; the
    // terminate_on_pause + single-frame emit + stop below keep this
    // cheap. Explicit frame cap in case the producer is longer.
    consumer.set("frames", 1);

    consumer.connect(producer);
    consumer.run();
    consumer.stop();
    return true;
}

} // namespace vedit
