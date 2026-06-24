#include "app_controller.h"

#include "media_library.h"
#include "timeline_model.h"
#include "util/logger.h"

namespace vedit {

AppController::AppController()
    : project_(std::make_unique<ProjectModel>()),
      player_(std::make_unique<PlayerController>(*project_)) {}

AppController::~AppController() = default;

// --- media library ---

int AppController::library_import(const std::string& path) {
    return project_->library().import(path);
}

int AppController::library_count() const {
    return project_->library().count();
}

std::string AppController::library_item_path(int index) const {
    const auto* item = project_->library().item(index);
    return item ? item->path : "";
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

} // namespace vedit
