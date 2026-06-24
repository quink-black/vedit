#ifndef VEDIT_DOMAIN_EXPORT_SERVICE_H
#define VEDIT_DOMAIN_EXPORT_SERVICE_H

#include <cstdint>
#include <functional>
#include <string>

#include <Mlt.h>

namespace vedit {

// Renders the timeline tractor to a file using consumer_avformat.
// Same tractor as the preview path; just swaps the consumer.
//
// For M1, export is synchronous (runs on the calling thread). Progress
// is reported via a callback. The consumer runs in "offline" mode
// (real_time=-1) so it renders as fast as the CPU can go.
class ExportService {
public:
    using ProgressFn = std::function<void(double fraction)>;

    ExportService() = default;
    ~ExportService() = default;

    // Render the given producer (tractor) to `output_path`. Returns
    // true on success. The profile carries resolution/fps.
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
