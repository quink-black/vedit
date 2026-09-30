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
    ExportService::CancelFn* cancel;
    mlt_consumer consumer;
    // Frames the encoder reported as rendered. Whether anything came out at
    // all is the only part of a render's outcome MLT leaves accessible: the
    // consumer reports no error to its caller.
    int frames_rendered = 0;
};

// Static callback for the consumer-frame-show event during export.
void on_export_frame(mlt_consumer, void* ud, mlt_event_data data) {
    if (!ud) return;
    auto* pd = static_cast<ExportProgressData*>(ud);

    // Stop the consumer from inside the callback. run() is synchronous
    // and cannot be interleaved, so this is the only point where a
    // cancel request can actually interrupt the encode.
    if (pd->cancel && *pd->cancel && (*pd->cancel)()) {
        if (pd->consumer) mlt_consumer_stop(pd->consumer);
        return;
    }

    Mlt::Frame frame = Mlt::EventData(data).to_frame();
    if (!frame.is_valid() || !frame.get_int("rendered")) return;
    ++pd->frames_rendered;

    if (!pd->fn || !*pd->fn) return;
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
    // Nothing to encode: running the consumer over an empty timeline never
    // returns, so this is settled before the consumer exists. Measured on the
    // code that lacked the check: get_playtime() and get_length() were both
    // zero and the call sat for over two minutes.
    if (producer.get_playtime() <= 0) {
        Logger::log(Logger::Level::Error,
                    "ExportService: the timeline has no frames to render");
        return false;
    }

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

    // The tractor's get_playtime() is the timeline duration: the
    // longest playlist across all user tracks. Use it directly as the
    // export length. Prior code clamped to the minimum track length,
    // but that broke single-track exports because MLT's tractor always
    // includes an internal black background track whose length can be
    // shorter than the user's clip.
    int total = producer.get_playtime();
    int orig_out = producer.get_int("out");

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
    // The listener counts rendered frames, reports progress and honours
    // cancellation, so it is attached whether or not the caller asked to be
    // told. Without it a render that emitted nothing looks like a success.
    ExportProgressData progress_data{total2, static_cast<int>(start_frame),
                                     &on_progress,
                                     on_cancel ? &on_cancel : nullptr,
                                     consumer.get_consumer(),
                                     0};
    consumer.listen("consumer-frame-show", &progress_data,
                    reinterpret_cast<mlt_listener>(on_export_frame));

    consumer.run();
    // run() returns when terminate_on_pause fires, but the avformat
    // consumer's encoder worker threads are only joined and their
    // buffers freed by an explicit stop(). Without it FFmpeg's
    // per-thread state leaks and teardown can race. Always stop.
    consumer.stop();
    // Restore the original out point if we modified it.
    if (orig_out >= 0) producer.set("out", orig_out);

    // Nothing came back from the consumer to check, so the evidence is that
    // frames were rendered and a file was left behind. A consumer that
    // stopped early used to answer true and leave the caller with a short or
    // empty file it believed was the finished export.
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(output_path, ec);
    if (progress_data.frames_rendered == 0) {
        Logger::log(Logger::Level::Error,
                    "ExportService: no frame was rendered; the output is not "
                    "an export");
        return false;
    }
    if (ec || bytes == 0) {
        Logger::log(Logger::Level::Error,
                    "ExportService: frames were rendered but no output file "
                    "is there to show for it");
        return false;
    }
    return true;
}

bool ExportService::get_frame(Mlt::Producer& producer, Mlt::Profile& profile,
                              int64_t position, const std::string& image_path) {
    // Seeking the producer and then running the consumer ignores the
    // seek: MLT's consumer restarts the producer from its start, so
    // positions 0/10/20/40 all yielded byte-identical PNGs (measured).
    // Cutting the single wanted frame out of the producer and rendering
    // that cut is the form that actually honors the position.
    int pos = static_cast<int>(position);
    if (pos < 0) pos = 0;
    if (pos >= producer.get_length()) pos = producer.get_length() - 1;
    if (pos < 0) {
        Logger::log(Logger::Level::Error, "ExportService: get_frame — empty producer");
        return false;
    }
    Mlt::Producer* frame_cut = producer.cut(pos, pos);
    if (!frame_cut || !frame_cut->is_valid()) {
        delete frame_cut;
        Logger::log(Logger::Level::Error, "ExportService: get_frame — cut failed");
        return false;
    }

    Mlt::Consumer consumer(profile, "avformat", image_path.c_str());
    if (!consumer.is_valid()) {
        delete frame_cut;
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
    // The cut already contains exactly one frame; the cap keeps the
    // consumer from running past it.
    consumer.set("frames", 1);

    consumer.connect(*frame_cut);
    consumer.run();
    consumer.stop();
    delete frame_cut;

    // Same evidence as render(): the consumer says nothing, so the file has
    // to be there.
    std::error_code ec;
    if (std::filesystem::file_size(image_path, ec) == 0 || ec) {
        Logger::log(Logger::Level::Error,
                    "ExportService: get_frame wrote no image");
        return false;
    }
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
