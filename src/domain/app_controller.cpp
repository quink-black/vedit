#include "app_controller.h"

#include "media_library.h"
#include "timeline_model.h"
#include "util/logger.h"

#include <chrono>
#include <thread>

namespace vedit {

AppController::AppController()
    : project_(std::make_unique<ProjectModel>()),
      player_(std::make_unique<PlayerController>(*project_)),
      filter_catalog_(std::make_unique<FilterCatalog>()) {}

AppController::~AppController() = default;

// --- media library ---

int AppController::library_import(const std::string& path,
                                   int video_index, int audio_index) {
    return project_->library().import(path, video_index, audio_index);
}

int AppController::library_count() const {
    return project_->library().count();
}

std::string AppController::library_item_path(int index) const {
    const auto* item = project_->library().item(index);
    return item ? item->path : "";
}

bool AppController::library_remove(int index) {
    // Refuse if the timeline still references this source; dropping it
    // would orphan the cut's parent producer and corrupt our stable-id
    // contract.
    if (project_->timeline().count_clips_for_media(index) > 0) {
        return false;
    }
    if (!project_->library().remove(index)) return false;
    // Shift remaining clips' stamped media_index down past the hole.
    project_->timeline().on_media_removed(index);
    // Library indices are now compact again; the preview producer
    // holds no per-source state, so no refresh needed unless the
    // currently selected source was the one removed (GUI's concern).
    return true;
}

bool AppController::library_set_stream(int index, int video_index, int audio_index) {
    // Same refuse-in-use guard as library_remove: changing streams on
    // a producer whose cuts are already on the timeline would silently
    // mutate already-placed clips (per-media design — the contract is
    // that all clips from one library item share the chosen streams).
    // Caller must drop the clips first if they want to switch streams.
    if (project_->timeline().count_clips_for_media(index) > 0) {
        return false;
    }
    if (!project_->library().set_stream(index, video_index, audio_index)) {
        return false;
    }
    // The library item's stream metadata is now updated; preview of any
    // (now-removed) clip would refresh, but since we refused in-use,
    // there are none to refresh.
    return true;
}

// --- tracks ---

TrackId AppController::track_add(TrackKind kind, const std::string& name) {
    auto id = project_->timeline().add_track(kind, name);
    player_->refresh();
    return id;
}

bool AppController::track_remove(TrackId id) {
    bool ok = project_->timeline().remove_track(id);
    player_->refresh();
    return ok;
}

int AppController::track_count() const {
    return project_->timeline().track_count();
}

// --- clips ---
//
// After any edit that changes the rendered output, refresh the preview
// so the (possibly paused) player shows the new state without needing
// playback.

ClipId AppController::clip_append(TrackId track, int media_index,
                                  int64_t in_point, int64_t out_point) {
    auto id = project_->timeline().append_clip(track, media_index, in_point, out_point);
    player_->refresh();
    return id;
}

ClipId AppController::clip_insert(TrackId track, int media_index,
                                  int64_t position, int64_t in_point,
                                  int64_t out_point) {
    auto id = project_->timeline().insert_clip(track, media_index, position,
                                               in_point, out_point);
    player_->refresh();
    return id;
}

bool AppController::clip_remove(ClipId id) {
    bool ok = project_->timeline().remove_clip(id);
    player_->refresh();
    return ok;
}

bool AppController::clip_move(ClipId id, TrackId dest_track,
                              int64_t dest_position) {
    bool ok = project_->timeline().move_clip(id, dest_track, dest_position);
    player_->refresh();
    return ok;
}

bool AppController::clip_trim(ClipId id, int64_t new_in, int64_t new_out) {
    bool ok = project_->timeline().trim_clip(id, new_in, new_out);
    player_->refresh();
    return ok;
}

ClipId AppController::clip_split(ClipId id, int64_t at_timeline_position) {
    auto new_id = project_->timeline().split_clip(id, at_timeline_position);
    player_->refresh();
    return new_id;
}

// --- transport ---

bool AppController::play() {
    return player_->play();
}

void AppController::pause() {
    player_->pause();
}

void AppController::seek(int64_t frame) {
    player_->seek(frame);
}

bool AppController::is_playing() const {
    return player_->is_playing();
}

int64_t AppController::position() const {
    return player_->position();
}

// --- export (async) ---

int64_t AppController::export_start(const std::string& output_path) {
    if (output_path.empty()) return 0;
    // Pause preview so the worker has exclusive access to the producer.
    // MLT producers are not thread-safe; running export and preview
    // simultaneously on the same tractor corrupts state.
    pause();

    auto job = std::make_shared<ExportService::ExportJob>();
    job->output_path = output_path;

    // Capture by value: producer/profile live on project_ (owned by
    // this AppController, which outlives the job because we hold the
    // shared_ptr in export_jobs_). The worker reads cancel_requested
    // in its progress callback.
    Mlt::Producer* producer = project_->producer();
    Mlt::Profile* profile = &project_->profile();
    if (!producer || !profile) return 0;

    auto job_weak = std::weak_ptr<ExportService::ExportJob>(job);
    ExportService::ProgressFn on_progress =
        [job_weak](double fraction) {
            if (auto j = job_weak.lock()) {
                j->fraction.store(fraction);
            }
        };

    int64_t job_id = next_export_job_id_++;
    export_jobs_[job_id] = job;

    // The worker lives on job->worker, joined by ExportJob's
    // destructor (which runs when the last shared_ptr drops — either
    // here in AppController's destructor or after an explicit clear).
    // This guarantees the producer/profile the worker touches stay
    // alive for the duration of the encode: AppController owns the
    // project_, and export_jobs_ holds the job shared_ptr, so neither
    // can be destroyed while the worker runs.
    job->worker = std::thread([job, producer, profile, output_path,
                               on_progress = std::move(on_progress)]() mutable {
        ExportService svc;
        bool ok = false;
        try {
            ok = svc.render(*producer, *profile, output_path,
                            std::move(on_progress));
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(job->mu);
            job->error = ex.what();
        }
        // Cancel signal is best-effort: MLT's run() blocks until
        // terminate_on_pause fires, so a long encode can't be
        // interrupted mid-stream. We still record the requested state
        // so the caller knows cancellation was attempted.
        if (job->cancel_requested.load()) {
            job->state.store(ExportService::JobState::Cancelled);
        } else if (ok) {
            job->state.store(ExportService::JobState::Done);
            job->fraction.store(1.0);
        } else {
            std::lock_guard<std::mutex> lk(job->mu);
            if (job->error.empty()) job->error = "render returned false";
            job->state.store(ExportService::JobState::Failed);
        }
    });

    return job_id;
}

AppController::ExportPollResult AppController::export_poll(int64_t job_id) {
    ExportPollResult r;
    auto it = export_jobs_.find(job_id);
    if (it == export_jobs_.end()) return r;
    r.found = true;
    r.output_path = it->second->output_path;
    r.fraction = it->second->fraction.load();
    switch (it->second->state.load()) {
        case ExportService::JobState::Running:   r.state = "running";   break;
        case ExportService::JobState::Done:      r.state = "done";      break;
        case ExportService::JobState::Failed:    r.state = "failed";     break;
        case ExportService::JobState::Cancelled: r.state = "cancelled"; break;
    }
    {
        std::lock_guard<std::mutex> lk(it->second->mu);
        r.error = it->second->error;
    }
    return r;
}

bool AppController::export_cancel(int64_t job_id) {
    auto it = export_jobs_.find(job_id);
    if (it == export_jobs_.end()) return false;
    if (it->second->state.load() != ExportService::JobState::Running) return false;
    it->second->cancel_requested.store(true);
    return true;
}

// --- filters (M2) ---

bool AppController::filter_add(ClipId clip_id, const std::string& name) {
    return project_->timeline().attach_filter(clip_id, name);
}

bool AppController::filter_remove(ClipId clip_id, const std::string& name) {
    return project_->timeline().detach_filter(clip_id, name);
}

bool AppController::filter_set_param(ClipId clip_id,
                                      const std::string& filter_name,
                                      const std::string& param,
                                      const std::string& value) {
    return project_->timeline().set_filter_param(
        clip_id, filter_name, param, value);
}

std::string AppController::filter_get_param(ClipId clip_id,
                                             const std::string& filter_name,
                                             const std::string& param) {
    return project_->timeline().get_filter_param(
        clip_id, filter_name, param);
}

bool AppController::track_filter_add(TrackId track_id, const std::string& name) {
    return project_->timeline().track_attach_filter(track_id, name);
}

bool AppController::track_filter_remove(TrackId track_id, const std::string& name) {
    return project_->timeline().track_detach_filter(track_id, name);
}

bool AppController::track_filter_set_param(TrackId track_id,
                                            const std::string& filter_name,
                                            const std::string& param,
                                            const std::string& value) {
    return project_->timeline().track_set_filter_param(
        track_id, filter_name, param, value);
}

std::string AppController::track_filter_get_param(TrackId track_id,
                                                   const std::string& filter_name,
                                                   const std::string& param) {
    return project_->timeline().track_get_filter_param(
        track_id, filter_name, param);
}

void AppController::profile_set(int width, int height, double fps) {
    project_->set_profile(width, height, fps);
}

bool AppController::composite_geometry_set(TrackId track_id,
                                            double x, double y,
                                            double w, double h) {
    return project_->timeline().set_composite_geometry(
        track_id, x, y, w, h);
}

} // namespace vedit
