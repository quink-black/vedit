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
    return project_->timeline().add_track(kind, name);
}

bool AppController::track_remove(TrackId id) {
    return project_->timeline().remove_track(id);
}

int AppController::track_count() const {
    return project_->timeline().track_count();
}

// --- clips ---

ClipId AppController::clip_append(TrackId track, int media_index,
                                  int64_t in_point, int64_t out_point) {
    return project_->timeline().append_clip(track, media_index, in_point, out_point);
}

ClipId AppController::clip_insert(TrackId track, int media_index,
                                  int64_t position, int64_t in_point,
                                  int64_t out_point) {
    return project_->timeline().insert_clip(track, media_index, position,
                                            in_point, out_point);
}

bool AppController::clip_remove(ClipId id) {
    return project_->timeline().remove_clip(id);
}

bool AppController::clip_move(ClipId id, TrackId dest_track,
                              int64_t dest_position) {
    return project_->timeline().move_clip(id, dest_track, dest_position);
}

bool AppController::clip_trim(ClipId id, int64_t new_in, int64_t new_out) {
    return project_->timeline().trim_clip(id, new_in, new_out);
}

ClipId AppController::clip_split(ClipId id, int64_t at_timeline_position) {
    return project_->timeline().split_clip(id, at_timeline_position);
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
