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

    enum class JobState { Running, Done, Failed, Cancelled };

    // Handle to an async render job. Safe to poll from any thread;
    // the worker updates state under a mutex. The ExportJob owns its
    // own thread and joins it on destruction, so keeping the shared_ptr
    // alive guarantees the worker has finished before resources release.
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

    // Render the given producer (tractor) to `output_path`. Returns
    // true on success. The profile carries resolution/fps. Blocks the
    // calling thread until completion — use only from non-main threads.
    // `on_progress` (if set) is called from the same thread as render.
    //
    // Thread-safety note: MLT producers are not thread-safe. The
    // caller MUST ensure the preview consumer is stopped (call
    // AppController::pause() or similar) before invoking this from a
    // worker thread, and must not drive further edits while it runs.
    bool render(Mlt::Producer& producer, Mlt::Profile& profile,
                const std::string& output_path,
                ProgressFn on_progress = nullptr);

    // Pull a single frame from the producer at `position` and save it
    // as a PNG image. Returns true on success.
    bool get_frame(Mlt::Producer& producer, Mlt::Profile& profile,
                   int64_t position, const std::string& image_path);
};

} // namespace vedit

#endif
