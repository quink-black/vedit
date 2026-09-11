#include "app_controller.h"

#include "media_library.h"
#include "project_save_load.h"
#include "text_filter.h"
#include "timeline_model.h"
#include "util/logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif
#include <thread>

namespace vedit {

namespace {
inline int current_pid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}
} // namespace


AppController::AppController()
    : project_(std::make_unique<ProjectModel>()),
      player_(std::make_unique<PlayerController>(*project_)),
      filter_catalog_(std::make_unique<FilterCatalog>()) {
    // MLT's Factory is up by the time an AppController exists, in both
    // the app and the tests, which is what makes this a single place to
    // register the in-process text filter.
    static bool text_filter_registered = false;
    if (!text_filter_registered) {
        register_text_filter();
        text_filter_registered = true;
    }

    // Every mutator ends with player_->refresh(), so this one hook
    // records history for all of them. Seed the history with the empty
    // project so the first mutation is undoable.
    player_->set_state_changed_listener([this]() { record_history(); });
    record_history();
}

AppController::~AppController() = default;

// --- media library ---

int AppController::library_import(const std::string& path,
                                   int video_index, int audio_index) {
    // Check if this is the first import and the user hasn't explicitly
    // set a canvas resolution. If so, auto-detect the profile from the
    // first media source.
    bool is_first = project_->library().count() == 0;
    int idx = project_->library().import(path, video_index, audio_index);
    if (idx >= 0 && is_first && !project_->profile_is_user_set()) {
        Mlt::Producer* producer = project_->library().producer(idx);
        if (producer && producer->is_valid()) {
            project_->profile().from_producer(*producer);
            project_->profile().set_progressive(1);
        }
    }
    return idx;
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
    // MediaLibrary::set_stream re-opens the source with a fresh
    // producer using the new stream selection, then reconnect_clips
    // re-cuts every referencing timeline clip from the new producer.
    // ClipId, playlist index, and ClipInfo are preserved.
    if (!project_->library().set_stream(index, video_index, audio_index)) {
        return false;
    }
    project_->timeline().reconnect_clips(index);
    player_->refresh();
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

bool AppController::track_set_muted(TrackId id, bool muted) {
    bool ok = project_->timeline().set_track_muted(id, muted);
    player_->refresh();
    return ok;
}

bool AppController::track_set_hidden(TrackId id, bool hidden) {
    bool ok = project_->timeline().set_track_hidden(id, hidden);
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

bool AppController::clip_slip(ClipId id, int64_t new_in, int64_t new_out) {
    bool ok = project_->timeline().slip_clip(id, new_in, new_out);
    player_->refresh();
    return ok;
}

ClipId AppController::clip_split(ClipId id, int64_t at_timeline_position) {
    auto new_id = project_->timeline().split_clip(id, at_timeline_position);
    player_->refresh();
    return new_id;
}

ClipId AppController::clip_overwrite(TrackId track, int media_index,
                                      int64_t position,
                                      int64_t in_point, int64_t out_point) {
    auto id = project_->timeline().overwrite_clip(
        track, media_index, position, in_point, out_point);
    player_->refresh();
    return id;
}

bool AppController::clip_lift(ClipId id) {
    bool ok = project_->timeline().lift_clip(id);
    player_->refresh();
    return ok;
}

bool AppController::clip_change_speed(ClipId id, double speed) {
    bool ok = project_->timeline().change_clip_speed(id, speed);
    player_->refresh();
    return ok;
}

bool AppController::clip_add_transition(TrackId track, int clip_index,
                                         int length, const std::string& type) {
    bool ok = project_->timeline().add_transition(
        track, clip_index, length, type);
    player_->refresh();
    return ok;
}

ClipId AppController::clip_detach_audio(ClipId id) {
    auto new_id = project_->timeline().detach_audio(id);
    player_->refresh();
    return new_id;
}

bool AppController::clip_set_audio_enabled(ClipId id, bool enabled) {
    bool ok = project_->timeline().set_clip_audio_enabled(id, enabled);
    player_->refresh();
    return ok;
}

bool AppController::clip_set_video_enabled(ClipId id, bool enabled) {
    bool ok = project_->timeline().set_clip_video_enabled(id, enabled);
    player_->refresh();
    return ok;
}

bool AppController::clip_set_geometry(ClipId id, double x, double y,
                                       double w, double h) {
    bool ok = project_->timeline().clip_set_geometry(id, x, y, w, h);
    player_->refresh();
    return ok;
}

bool AppController::clip_set_rotation(ClipId id, double degrees) {
    bool ok = project_->timeline().clip_set_rotation(id, degrees);
    player_->refresh();
    return ok;
}

bool AppController::clip_get_geometry(ClipId id, double& x, double& y,
                                       double& w, double& h) const {
    return project_->timeline().clip_get_geometry(id, x, y, w, h);
}

bool AppController::clip_set_crop(ClipId id, int left, int right,
                                   int top, int bottom) {
    bool ok = project_->timeline().clip_set_crop(id, left, right, top, bottom);
    player_->refresh();
    return ok;
}

bool AppController::clip_get_crop(ClipId id, int& left, int& right,
                                   int& top, int& bottom) const {
    return project_->timeline().clip_get_crop(id, left, right, top, bottom);
}

bool AppController::clip_set_fade_in(ClipId id, int duration_frames) {
    bool ok = project_->timeline().clip_set_fade_in(id, duration_frames);
    player_->refresh();
    return ok;
}

bool AppController::clip_set_fade_out(ClipId id, int duration_frames) {
    bool ok = project_->timeline().clip_set_fade_out(id, duration_frames);
    player_->refresh();
    return ok;
}

nlohmann::json AppController::clip_analyze_track(
        ClipId id, const std::string& rect,
        const std::string& algo, int steps) {
    return project_->timeline().clip_analyze_track(id, rect, algo, steps);
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

int64_t AppController::export_start(const std::string& output_path,
                                     const nlohmann::json& params,
                                     int64_t in_point, int64_t out_point) {
    if (output_path.empty()) return 0;
    pause();

    auto job = std::make_shared<ExportService::ExportJob>();
    job->output_path = output_path;

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

    ExportService::CancelFn on_cancel =
        [job_weak]() -> bool {
            if (auto j = job_weak.lock()) {
                return j->cancel_requested.load();
            }
            return true;
        };

    int64_t job_id = next_export_job_id_++;
    export_jobs_[job_id] = job;

    job->worker = std::thread([job, producer, profile, output_path,
                               on_progress = std::move(on_progress),
                               on_cancel = std::move(on_cancel),
                               params, in_point, out_point]() mutable {
        ExportService svc;
        bool ok = false;
        try {
            ok = svc.render(*producer, *profile, output_path,
                            std::move(on_progress), params,
                            in_point, out_point,
                            std::move(on_cancel));
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(job->mu);
            job->error = ex.what();
        }
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

bool AppController::filter_add(ClipId clip_id, const std::string& name,
                                 int in, int out) {
    bool ok = project_->timeline().attach_filter(clip_id, name, in, out);
    player_->refresh();
    return ok;
}

bool AppController::filter_set_in_out(ClipId clip_id,
                                       const std::string& filter_name,
                                       int in, int out) {
    return project_->timeline().set_filter_in_out(
        clip_id, filter_name, in, out);
    player_->refresh();
}

bool AppController::filter_remove(ClipId clip_id, const std::string& name) {
    bool ok = project_->timeline().detach_filter(clip_id, name);
    player_->refresh();
    return ok;
}

bool AppController::filter_set_param(ClipId clip_id,
                                      const std::string& filter_name,
                                      const std::string& param,
                                      const std::string& value) {
    return project_->timeline().set_filter_param(
        clip_id, filter_name, param, value);
    player_->refresh();
}

std::string AppController::filter_get_param(ClipId clip_id,
                                             const std::string& filter_name,
                                             const std::string& param) {
    return project_->timeline().get_filter_param(
        clip_id, filter_name, param);
}

bool AppController::track_filter_add(TrackId track_id, const std::string& name) {
    bool ok = project_->timeline().track_attach_filter(track_id, name);
    player_->refresh();
    return ok;
}

bool AppController::track_filter_remove(TrackId track_id, const std::string& name) {
    bool ok = project_->timeline().track_detach_filter(track_id, name);
    player_->refresh();
    return ok;
}

bool AppController::track_filter_set_param(TrackId track_id,
                                            const std::string& filter_name,
                                            const std::string& param,
                                            const std::string& value) {
    return project_->timeline().track_set_filter_param(
        track_id, filter_name, param, value);
    player_->refresh();
}

std::string AppController::track_filter_get_param(TrackId track_id,
                                                   const std::string& filter_name,
                                                   const std::string& param) {
    return project_->timeline().track_get_filter_param(
        track_id, filter_name, param);
}

bool AppController::filter_set_param_keyframes(
        ClipId clip_id, const std::string& filter_name,
        const std::string& param, const nlohmann::json& keyframes) {
    return project_->timeline().set_filter_param_keyframes(
        clip_id, filter_name, param, keyframes);
    player_->refresh();
}

nlohmann::json AppController::filter_get_param_keyframes(
        ClipId clip_id, const std::string& filter_name,
        const std::string& param) {
    return project_->timeline().get_filter_param_keyframes(
        clip_id, filter_name, param);
}

bool AppController::track_filter_set_param_keyframes(
        TrackId track_id, const std::string& filter_name,
        const std::string& param, const nlohmann::json& keyframes) {
    return project_->timeline().track_set_filter_param_keyframes(
        track_id, filter_name, param, keyframes);
    player_->refresh();
}

nlohmann::json AppController::track_filter_get_param_keyframes(
        TrackId track_id, const std::string& filter_name,
        const std::string& param) {
    return project_->timeline().track_get_filter_param_keyframes(
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
    player_->refresh();
}

bool AppController::composite_geometry_get(TrackId track_id,
                                            double& x, double& y,
                                            double& w, double& h) const {
    return project_->timeline().get_composite_geometry(
        track_id, x, y, w, h);
}

// --- undo/redo ---

// Snapshots live in a per-process directory; the seq counter keeps
// concurrent processes from clobbering each other.
static std::filesystem::path undo_snapshot_dir() {
    return std::filesystem::temp_directory_path() / "vedit-undo";
}

std::string AppController::snapshot_state() {
    std::error_code ec;
    std::filesystem::create_directories(undo_snapshot_dir(), ec);
    if (ec) return {};
    static std::atomic<int> seq{0};
    std::string path = (undo_snapshot_dir() /
                        ("snap-" + std::to_string(current_pid()) + "-" +
                         std::to_string(seq++) + ".xml"))
                           .string();
    if (!ProjectSaveLoad::save_xml(*project_, path)) return {};
    return path;
}

// MLT's XML consumer writes nothing at all for a tractor with no tracks,
// so a snapshot of an empty project is a missing file rather than a
// document. Restoring it legitimately means an empty timeline.
bool AppController::restore_snapshot(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) ||
        std::filesystem::file_size(path, ec) == 0) {
        auto& tl = project_->timeline();
        while (tl.track_count() > 0)
            tl.remove_track(tl.tracks()[0].id);
        return true;
    }
    return ProjectSaveLoad::load_xml(*project_, path);
}

void AppController::record_history() {
    if (restoring_) return;
    std::string path = snapshot_state();
    if (path.empty()) return;

    // A mutation that changed nothing (a failed call still refreshes)
    // must not enter history, so compare against the current entry.
    std::error_code ec;
    if (history_pos_ >= 0 &&
        std::filesystem::file_size(history_[history_pos_], ec) ==
            std::filesystem::file_size(path, ec)) {
        std::ifstream a(history_[history_pos_], std::ios::binary);
        std::ifstream b(path, std::ios::binary);
        if (a && b && std::equal(std::istreambuf_iterator<char>(a),
                                 std::istreambuf_iterator<char>(),
                                 std::istreambuf_iterator<char>(b))) {
            std::remove(path.c_str());
            return;
        }
    }

    // Anything after the current position is a redo tail; a new mutation
    // discards it.
    for (int i = history_pos_ + 1; i < (int)history_.size(); ++i)
        std::remove(history_[i].c_str());
    history_.resize(history_pos_ + 1);

    constexpr size_t kMaxHistory = 50;
    if (history_.size() >= kMaxHistory) {
        std::remove(history_.front().c_str());
        history_.erase(history_.begin());
        --history_pos_;
    }
    history_.push_back(std::move(path));
    history_pos_ = static_cast<int>(history_.size()) - 1;
}

void AppController::clear_history() {
    for (auto& p : history_) std::remove(p.c_str());
    history_.clear();
    history_pos_ = -1;
}

bool AppController::can_undo() const {
    return !restoring_ && history_pos_ > 0;
}

bool AppController::can_redo() const {
    return !restoring_ && history_pos_ < static_cast<int>(history_.size()) - 1;
}

bool AppController::undo() {
    if (!can_undo()) return false;
    restoring_ = true;
    --history_pos_;
    bool ok = restore_snapshot(history_[history_pos_]);
    if (!ok) ++history_pos_;
    // The refresh must still see restoring_ == true: it exists to update
    // the preview, not to record the restored state as a new mutation.
    player_->refresh();
    restoring_ = false;
    return ok;
}

bool AppController::redo() {
    if (!can_redo()) return false;
    restoring_ = true;
    ++history_pos_;
    bool ok = restore_snapshot(history_[history_pos_]);
    if (!ok) --history_pos_;
    player_->refresh();
    restoring_ = false;
    return ok;
}

void AppController::project_new() {
    clear_history();
    auto& tl = project_->timeline();
    while (tl.track_count() > 0)
        tl.remove_track(tl.tracks()[0].id);
    project_->library().clear();
    player_->refresh();
}

} // namespace vedit
