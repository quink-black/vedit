#ifndef VEDIT_DOMAIN_APP_CONTROLLER_H
#define VEDIT_DOMAIN_APP_CONTROLLER_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "export_service.h"
#include "filter_catalog.h"
#include "ids.h"
#include "project_model.h"
#include "player_controller.h"

namespace vedit {

// Single-interface seam for all domain operations. GUI and RPC both
// go through this — never directly to ProjectModel or PlayerController.
// Every method is an action (verb), returning a structured result.
//
// Threading: all calls from the main thread. The MLT consumer thread
// is owned by PlayerController; frame hand-off back to the main thread
// is handled by PreviewRenderer.
class AppController {
public:
    AppController();
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // --- read-only state ---
    ProjectModel& project() { return *project_; }
    PlayerController& player() { return *player_; }
    Mlt::Profile& profile() { return project_->profile(); }

    // --- media library ---
    // Returns the library index, or -1 on failure.
    // video_index / audio_index: optional stream selection at import
    // time. -1 = MLT default (first of type); -2 audio = "all".
    int library_import(const std::string& path,
                        int video_index = -1, int audio_index = -1);
    int library_count() const;
    std::string library_item_path(int index) const;
    // Remove a library item. Refuses (returns false) if any clip on the
    // timeline still references it; otherwise drops the item and shifts
    // higher indices down so future lookups stay correct.
    bool library_remove(int index);
    // Switch the selected video/audio stream on an imported item.
    // Refuses if the timeline still references it. Stream-index
    // convention matches library_import. Returns false on bad index,
    // in-use, or producer error.
    bool library_set_stream(int index, int video_index, int audio_index);

    // --- tracks ---
    TrackId track_add(TrackKind kind, const std::string& name = "");
    bool track_remove(TrackId id);
    int track_count() const;

    // --- clips ---
    ClipId clip_append(TrackId track, int media_index,
                       int64_t in_point = -1, int64_t out_point = -1);
    ClipId clip_insert(TrackId track, int media_index, int64_t position,
                       int64_t in_point = -1, int64_t out_point = -1);
    bool clip_remove(ClipId id);
    bool clip_move(ClipId id, TrackId dest_track, int64_t dest_position);
    bool clip_trim(ClipId id, int64_t new_in, int64_t new_out);
    ClipId clip_split(ClipId id, int64_t at_timeline_position);

    // --- filters (M2) ---
    const FilterCatalog& filter_catalog() const { return *filter_catalog_; }

    // --- transport ---
    bool play();
    void pause();
    void seek(int64_t frame);
    bool is_playing() const;
    int64_t position() const;

    // --- export (async) ---
    //
    // Start rendering the current timeline to `output_path`. Returns a
    // job id (>= 1) or 0 on immediate failure. The render runs on a
    // worker thread; the caller polls export_poll(job_id) for state.
    // The preview consumer is paused first so the worker has exclusive
    // access to the producer.
    int64_t export_start(const std::string& output_path);

    // Poll an export job. `state` is one of "running" / "done" /
    // "failed" / "cancelled"; `fraction` in [0,1]. Returns false if
    // `job_id` is unknown.
    struct ExportPollResult {
        bool found = false;
        std::string state;
        double fraction = 0.0;
        std::string output_path;
        std::string error;
    };
    ExportPollResult export_poll(int64_t job_id);

    // Request cancellation of a running job. Returns true if the job
    // was running and cancel was signalled. The worker observes the
    // flag at its next frame and exits; final state becomes
    // "cancelled" once the worker joins.
    bool export_cancel(int64_t job_id);

private:
    std::unique_ptr<ProjectModel> project_;
    std::unique_ptr<PlayerController> player_;
    std::unique_ptr<FilterCatalog> filter_catalog_;

    // Active and recently-finished export jobs. The shared_ptr is held
    // here so the worker stays joined even if the caller drops its
    // reference; entries linger until export_start is called again or
    // the controller is destroyed.
    std::unordered_map<int64_t, std::shared_ptr<ExportService::ExportJob>> export_jobs_;
    int64_t next_export_job_id_ = 1;
};

} // namespace vedit

#endif
