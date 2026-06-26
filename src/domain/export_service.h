#ifndef VEDIT_DOMAIN_EXPORT_SERVICE_H
#define VEDIT_DOMAIN_EXPORT_SERVICE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <Mlt.h>

#include <nlohmann/json.hpp>

namespace vedit {

// Renders the timeline tractor to a file using consumer_avformat.
// Same tractor as the preview path; just swaps the consumer.
//
// render() is synchronous (blocks the calling thread). render_async()
// runs the encode on a worker thread and returns a ExportJob handle
// the caller can poll for progress / completion. The GUI main loop and
// the RPC dispatcher must not block on export — they use render_async.
class ExportService {
public:
    using ProgressFn = std::function<void(double fraction)>;

    // CancelFn is called from the worker thread when cancel is
    // requested. The implementation should interrupt the encode.
    using CancelFn = std::function<bool()>;

    enum class JobState { Running, Done, Failed, Cancelled };

    struct ExportJob {
        std::atomic<JobState> state{JobState::Running};
        std::atomic<double> fraction{0.0};
        std::atomic<bool> cancel_requested{false};
        std::string output_path;
        std::string error;

        std::thread worker;
        std::mutex mu;  // guards `error` writes from the worker

        ~ExportJob() {
            cancel_requested.store(true);
            if (worker.joinable()) worker.join();
        }
    };

    ExportService() = default;
    ~ExportService() = default;

    // Render with optional codec parameters and in/out range.
    // `params` is a JSON object of consumer properties (e.g.
    // {"vcodec":"libx264","crf":23}). `in_point`/`out_point` are
    // frame numbers for range export; -1 = full timeline.
    // `on_cancel` is polled each frame; if it returns true the
    // render is stopped mid-stream.
    bool render(Mlt::Producer& producer, Mlt::Profile& profile,
                const std::string& output_path,
                ProgressFn on_progress = nullptr,
                const nlohmann::json& params = nullptr,
                int64_t in_point = -1, int64_t out_point = -1,
                CancelFn on_cancel = nullptr);

    // Pull a single frame from the producer at `position` and save it
    // as a PNG image. Returns true on success.
    bool get_frame(Mlt::Producer& producer, Mlt::Profile& profile,
                   int64_t position, const std::string& image_path);

    // Read available export presets from the MLT preset directory.
    struct PresetInfo {
        std::string name;        // meta.preset.name
        std::string extension;   // meta.preset.extension
        std::string description; // meta.preset.note
        std::string file_path;
    };
    static std::vector<PresetInfo> list_presets();
    // Load a preset file and return its properties as JSON.
    static nlohmann::json load_preset(const std::string& file_path);
};

} // namespace vedit

#endif
