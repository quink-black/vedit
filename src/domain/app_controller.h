#ifndef VEDIT_DOMAIN_APP_CONTROLLER_H
#define VEDIT_DOMAIN_APP_CONTROLLER_H

#include <cstdint>
#include <memory>
#include <string>

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
    int library_import(const std::string& path);
    int library_count() const;
    std::string library_item_path(int index) const;

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

    // --- transport ---
    bool play();
    void pause();
    void seek(int64_t frame);
    bool is_playing() const;
    int64_t position() const;

private:
    std::unique_ptr<ProjectModel> project_;
    std::unique_ptr<PlayerController> player_;
};

} // namespace vedit

#endif
