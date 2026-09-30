#ifndef VEDIT_DOMAIN_APP_CONTROLLER_H
#define VEDIT_DOMAIN_APP_CONTROLLER_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Mute silences audio on the track. Hidden hides video on the
    // track. Both affect preview and export via MLT playlist hide prop.
    bool track_set_muted(TrackId id, bool muted);
    bool track_set_hidden(TrackId id, bool hidden);

    // --- clips ---
    ClipId clip_append(TrackId track, int media_index,
                       int64_t in_point = -1, int64_t out_point = -1);
    ClipId clip_insert(TrackId track, int media_index, int64_t position,
                       int64_t in_point = -1, int64_t out_point = -1);
    bool clip_remove(ClipId id);
    bool clip_move(ClipId id, TrackId dest_track, int64_t dest_position);
    bool clip_trim(ClipId id, int64_t new_in, int64_t new_out);
    bool clip_slip(ClipId id, int64_t new_in, int64_t new_out);
    ClipId clip_split(ClipId id, int64_t at_timeline_position);

    // --- advanced editing (M4) ---
    ClipId clip_overwrite(TrackId track, int media_index, int64_t position,
                          int64_t in_point = -1, int64_t out_point = -1);
    bool clip_lift(ClipId id);
    bool clip_change_speed(ClipId id, double speed);
    bool clip_add_transition(TrackId track, int clip_index,
                             int length, const std::string& type = "luma");
    ClipId clip_detach_audio(ClipId id);
    bool clip_set_audio_enabled(ClipId id, bool enabled);
    bool clip_set_video_enabled(ClipId id, bool enabled);

    // Per-clip geometry via MLT affine filter (percentages 0-100).
    bool clip_set_geometry(ClipId id, double x, double y,
                           double w, double h);
    bool clip_set_rotation(ClipId id, double degrees);
    bool clip_get_rotation(ClipId id, double& degrees);
    bool clip_get_geometry(ClipId id, double& x, double& y,
                           double& w, double& h) const;

    // Per-clip crop margins in pixels.
    bool clip_set_crop(ClipId id, int left, int right,
                       int top, int bottom);
    bool clip_get_crop(ClipId id, int& left, int& right,
                       int& top, int& bottom) const;

    // Per-clip fade-in/fade-out. duration_frames=0 removes the fade.
    bool clip_set_fade_in(ClipId id, int duration_frames);
    bool clip_set_fade_out(ClipId id, int duration_frames);

    // Run opencv.tracker analysis pass on a clip and return per-frame
    // rect keyframes. The tracker filter is left attached so a render
    // pass can overlay the tracking results.
    nlohmann::json clip_analyze_track(ClipId id,
                                      const std::string& rect,
                                      const std::string& algo = "KCF",
                                      int steps = 5);

    // --- filters (M2) ---
    const FilterCatalog& filter_catalog() const { return *filter_catalog_; }
    bool filter_add(ClipId clip_id, const std::string& name,
                    int in = -1, int out = -1);
    bool filter_remove(ClipId clip_id, const std::string& name);
    bool filter_set_in_out(ClipId clip_id, const std::string& filter_name,
                           int in, int out);
    bool filter_set_param(ClipId clip_id, const std::string& filter_name,
                          const std::string& param, const std::string& value);
    std::string filter_get_param(ClipId clip_id,
                                 const std::string& filter_name,
                                 const std::string& param);

    // Per-track filter variants.
    bool track_filter_add(TrackId track_id, const std::string& name);
    bool track_filter_remove(TrackId track_id, const std::string& name);
    bool track_filter_set_param(TrackId track_id,
                                const std::string& filter_name,
                                const std::string& param,
                                const std::string& value);
    std::string track_filter_get_param(TrackId track_id,
                                       const std::string& filter_name,
                                       const std::string& param);

    // Keyframe-aware variants (M3 / P2).
    bool filter_set_param_keyframes(ClipId clip_id,
                                    const std::string& filter_name,
                                    const std::string& param,
                                    const nlohmann::json& keyframes);
    nlohmann::json filter_get_param_keyframes(ClipId clip_id,
                                              const std::string& filter_name,
                                              const std::string& param);
    bool track_filter_set_param_keyframes(TrackId track_id,
                                          const std::string& filter_name,
                                          const std::string& param,
                                          const nlohmann::json& keyframes);
    nlohmann::json track_filter_get_param_keyframes(TrackId track_id,
                                                    const std::string& filter_name,
                                                    const std::string& param);

    // --- profile (canvas) ---
    void profile_set(int width, int height, double fps);

    // --- canvas composition ---
    bool composite_geometry_set(TrackId track_id,
                                 double x, double y, double w, double h);
    bool composite_geometry_get(TrackId track_id,
                                 double& x, double& y,
                                 double& w, double& h) const;

    // --- project management ---
    // Clear the timeline and library for a new project.
    void project_new();

    // Adopt the project stored at path, replacing the open one. Goes through
    // the same history and preview hook as every other mutation, so undo
    // returns to the project that was open before. Returns false and changes
    // nothing when the file cannot be parsed.
    bool project_open(const std::string& path);

    // --- undo/redo (G4) ---
    // History is recorded from a single hook: every mutator already ends
    // with player_->refresh(), and that listener lands here. A snapshot
    // identical to the current entry is dropped, which is what makes a
    // failed mutation a no-op without needing guards in every method.
    // Both channels (GUI and RPC) go through AppController, so both get
    // the same history. A restore re-mints clip and track ids, so
    // callers must re-read state.get before addressing them again.
    bool can_undo() const;
    bool can_redo() const;
    bool undo();
    bool redo();

private:
    void record_history();
    std::string snapshot_state();
    bool restore_snapshot(const std::string& path);
    void clear_history();

    // history_[history_pos_] is the state on screen now; entries after it
    // are the redo tail. restoring_ is true while a restore is in flight,
    // so the refresh() it triggers does not record the state again.
    std::vector<std::string> history_;
    int history_pos_ = -1;
    bool restoring_ = false;

public:
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
    int64_t export_start(const std::string& output_path,
                         const nlohmann::json& params = nullptr,
                         int64_t in_point = -1, int64_t out_point = -1);

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
