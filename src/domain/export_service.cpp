#include "export_service.h"

#include "util/logger.h"

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
    consumer.set("real_time", -1);        // render as fast as possible
    consumer.set("terminate_on_pause", 1);
    consumer.set("vcodec", "libx264");
    consumer.set("acodec", "aac");
    consumer.set("f", "mp4");
    consumer.set("crf", 23);
    consumer.set("preset", "medium");
    consumer.set("threads", 0);           // auto

    consumer.connect(producer);

    // Progress reporting via consumer-frame-show.
    int total = producer.get_length();
    std::pair<int, ProgressFn*> progress_data(total, &on_progress);
    if (on_progress) {
        consumer.listen("consumer-frame-show", &progress_data,
                        reinterpret_cast<mlt_listener>(on_export_frame));
    }

    consumer.run();  // synchronous — blocks until done or error
    return true;
}

bool ExportService::get_frame(Mlt::Producer& producer, Mlt::Profile& profile,
                              int64_t position, const std::string& image_path) {
    producer.seek(static_cast<int>(position));

    Mlt::Consumer consumer(profile, "avformat", image_path.c_str());
    if (!consumer.is_valid()) {
        Logger::log(Logger::Level::Error, "ExportService: get_frame — avformat unavailable");
        return false;
    }
    consumer.set("real_time", -1);
    consumer.set("terminate_on_pause", 1);
    consumer.set("vcodec", "png");
    consumer.set("f", "image2");

    consumer.connect(producer);
    consumer.run();
    return true;
}

} // namespace vedit
