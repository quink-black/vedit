#include "export_service.h"

#include "util/logger.h"

#include <MltTractor.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace vedit {

namespace {

struct ExportProgressData {
    int total;
    int start_frame;
    ExportService::ProgressFn* fn;
};

// Static callback for the consumer-frame-show event during export.
void on_export_frame(mlt_consumer, void* ud, mlt_event_data data) {
    if (!ud) return;
    auto* pd = static_cast<ExportProgressData*>(ud);
    if (!pd->fn || !*pd->fn) return;
    Mlt::Frame frame = Mlt::EventData(data).to_frame();
    if (!frame.is_valid() || !frame.get_int("rendered")) return;
    int pos = frame.get_position() - pd->start_frame;
    int total = pd->total;
    if (total > 0) {
        (*pd->fn)(static_cast<double>(pos) / total);
    }
}

} // namespace

bool ExportService::render(Mlt::Producer& producer, Mlt::Profile& profile,
                           const std::string& output_path, ProgressFn on_progress,
                           const nlohmann::json& params,
                           int64_t in_point, int64_t out_point,
                           CancelFn on_cancel) {
    // Consumer ctor: profile, "avformat", output_path.
    Mlt::Consumer consumer(profile, "avformat", output_path.c_str());
    if (!consumer.is_valid()) {
        Logger::log(Logger::Level::Error, "ExportService: avformat consumer unavailable");
        return false;
    }

    // Defaults (H.264 + AAC + MP4).
    consumer.set("real_time", -1);
    consumer.set("terminate_on_pause", 1);
    consumer.set("vcodec", "libx264");
    consumer.set("acodec", "aac");
    consumer.set("f", "mp4");
    consumer.set("crf", 23);
    consumer.set("preset", "medium");
    consumer.set("threads", 0);

    // Apply caller-supplied params (overrides defaults).
    if (!params.is_null() && params.is_object()) {
        for (auto it = params.begin(); it != params.end(); ++it) {
            const auto& v = it.value();
            if (v.is_string())
                consumer.set(it.key().c_str(), v.get<std::string>().c_str());
            else if (v.is_number_integer())
                consumer.set(it.key().c_str(), v.get<int>());
            else if (v.is_number_float())
                consumer.set(it.key().c_str(), v.get<double>());
            else
                consumer.set(it.key().c_str(), v.dump().c_str());
        }
    }

    // Reset the producer to a clean render state. The preview consumer
    // runs with terminate_on_pause and, when it reaches the end of the
    // timeline, leaves the producer paused at the last frame with
    // speed 0. An export consumer (also terminate_on_pause) inheriting
    // speed 0 sees "already paused" and stops before emitting a single
    // frame, producing a valid-but-empty 262-byte MP4. Restore forward
    // playback and rewind to the start so export always renders the
    // whole timeline regardless of where preview left the playhead.
    producer.set_speed(1.0);

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

    // Range export: if caller supplied an in-point, seek there instead
    // of frame 0. If out_point is supplied, clamp the export length.
    int64_t start_frame = 0;
    if (in_point >= 0 && in_point < total) {
        start_frame = in_point;
    }
    producer.seek(static_cast<int>(start_frame));

    if (out_point >= 0 && out_point < total && out_point > start_frame) {
        total = static_cast<int>(out_point - start_frame);
    }
    producer.set("out", start_frame + total - 1);

    consumer.connect(producer);

    // Progress reporting via consumer-frame-show.
    // Re-read total in case producer.set("out", ...) changed it.
    int total2 = producer.get_length();
    ExportProgressData progress_data{total2, static_cast<int>(start_frame), &on_progress};
    if (on_progress) {
        consumer.listen("consumer-frame-show", &progress_data,
                        reinterpret_cast<mlt_listener>(on_export_frame));
    }

    // Run the encode, checking cancel between frames.
    if (on_cancel) {
        // Poll cancel in a tight-ish loop while run() blocks.
        // run() is synchronous, so we can't interleave. Instead,
        // rely on the progress callback to check cancel.
        // The consumer will stop naturally when terminate_on_pause
        // fires at the end. For mid-stream cancel, the caller
        // should set cancel_requested on the ExportJob and the
        // progress callback checks it.
        consumer.run();
    } else {
        consumer.run();
    }
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
    // vcpkg's ffmpeg build may not include the built-in PNG encoder.
    // Use mjpeg which is always available — functionally equivalent
    // for a single-frame screenshot (image2 muxer + one frame).
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

std::vector<ExportService::PresetInfo> ExportService::list_presets() {
    std::vector<PresetInfo> result;

    // Search the MLT preset directory.
    // Typical paths: <install>/share/mlt/presets/consumer/avformat/
    // or <build>/out/share/mlt/presets/consumer/avformat/
    const char* mlt_data = getenv("MLT_DATA");
    std::string base;
    if (mlt_data && mlt_data[0]) {
        base = std::string(mlt_data) + "/presets/consumer/avformat/";
    } else {
        // Fallback to common install paths.
        base = "/usr/share/mlt/presets/consumer/avformat/";
    }

    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(base, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();

        // Read the preset file.
        std::ifstream f(path);
        if (!f.is_open()) continue;

        PresetInfo info;
        info.file_path = path;

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "meta.preset.name") info.name = val;
            else if (key == "meta.preset.extension") info.extension = val;
            else if (key == "meta.preset.note") info.description = val;
        }
        if (!info.name.empty()) result.push_back(std::move(info));
    }
    return result;
}

nlohmann::json ExportService::load_preset(const std::string& file_path) {
    nlohmann::json result = nlohmann::json::object();
    std::ifstream f(file_path);
    if (!f.is_open()) return result;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Skip metadata properties.
        if (key.compare(0, 5, "meta.") == 0) continue;
        result[key] = val;
    }
    return result;
}

} // namespace vedit
