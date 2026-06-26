#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <nfd.h>

#include "core/mlt_runtime.h"
#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/timeline_model.h"
#include "rpc/rpc_dispatcher.h"
#include "rpc/rpc_methods.h"
#include "rpc/rpc_server.h"
#include "util/logger.h"
#include "util/platform_compat.h"

// ============================================================================
// Helpers
// ============================================================================

static std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Human-readable timecode HH:MM:SS.ff at a given fps.
static std::string timecode(int64_t frame, double fps) {
    if (fps <= 0) fps = 25.0;
    if (frame < 0) frame = 0;
    int total_sec = static_cast<int>(frame / fps);
    int h = total_sec / 3600;
    int m = (total_sec % 3600) / 60;
    int s = total_sec % 60;
    int f = static_cast<int>(frame % static_cast<int>(fps));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%02d", h, m, s, f);
    return buf;
}

// Format seconds as M:SS for the ruler so it reads like a clock, not frames.
static std::string ruler_label(int64_t frame, double fps) {
    if (fps <= 0) fps = 25.0;
    int total_sec = static_cast<int>(frame / fps);
    int m = total_sec / 60;
    int s = total_sec % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// Native file-open dialog via NFD.
static std::string open_file_dialog() {
    nfdfilteritem_t filters[] = {
        { "Video", "mp4,mkv,mov,avi,webm,m4v" },
        { "All files", "*" },
    };
    nfdchar_t* out = nullptr;
    nfdresult_t res = NFD_OpenDialog(
        &out, filters, sizeof(filters) / sizeof(filters[0]), nullptr);
    std::string result;
    if (res == NFD_OKAY && out) {
        result = out;
        NFD_FreePath(out);
    } else if (res == NFD_ERROR) {
        const char* err = NFD_GetError();
        vedit::Logger::log(vedit::Logger::Level::Warn,
                           err ? err : "NFD open dialog error");
    }
    return result;
}

static std::string save_file_dialog(const char* default_name) {
    nfdfilteritem_t filters[] = { { "MP4 video", "mp4" } };
    nfdchar_t* out = nullptr;
    nfdresult_t res = NFD_SaveDialog(
        &out, filters, sizeof(filters) / sizeof(filters[0]), nullptr,
        default_name ? default_name : "export.mp4");
    std::string result;
    if (res == NFD_OKAY && out) {
        result = out;
        NFD_FreePath(out);
    } else if (res == NFD_ERROR) {
        const char* err = NFD_GetError();
        vedit::Logger::log(vedit::Logger::Level::Warn,
                           err ? err : "NFD save dialog error");
    }
    return result;
}

// Ensure the export path ends with .mp4 (NFD may not append it).
static std::string ensure_mp4_ext(std::string path) {
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".mp4") != 0)
        path += ".mp4";
    return path;
}

static void set_window_icon(SDL_Window* window) {
#ifdef __APPLE__
    (void)window;
    // On macOS the .icns file in the bundle handles the Dock icon.
#else
    (void)window;
    // TODO: load and set window icon from resource/vedit.png
#endif
}

// ============================================================================
// GUI state
// ============================================================================

enum class DragMode { None, Move, TrimLeft, TrimRight, Scrub };

struct GuiState {
    int selected_media = -1;
    vedit::ClipId selected_clip{0};
    vedit::TrackId active_track{0};
    std::string status = "Open a video to begin (File > Open).";
    double project_fps = 25.0;
    float zoom = 8.0f;
    bool zoom_auto = true;
    int64_t last_fit_duration = -1;

    DragMode drag = DragMode::None;
    vedit::ClipId drag_clip{0};
    int64_t drag_grab_offset = 0;
    int64_t drag_orig_in = 0;
    int64_t drag_orig_out = 0;
    int64_t drag_orig_start = 0;
    int64_t drag_source_len = 0;  // source clip length, for trim clamping

    // Async export job id (0 = none).
    int64_t export_job_id = 0;

    // Profile settings dialog.
    bool show_profile_dialog = false;
    int profile_dialog_w = 1920;
    int profile_dialog_h = 1080;
    float profile_dialog_fps = 25.0f;
};

// ============================================================================
// Media → timeline
// ============================================================================

static vedit::TrackId resolve_target_track(vedit::AppController& app,
                                           GuiState& gui) {
    auto& tl = app.project().timeline();
    if (gui.active_track.valid() && tl.track(gui.active_track) != nullptr)
        return gui.active_track;
    if (tl.track_count() == 0) {
        auto id = app.track_add(vedit::TrackKind::Video);
        gui.active_track = id;
        return id;
    }
    auto first = tl.tracks()[0].id;
    gui.active_track = first;
    return first;
}

static void add_media_to_timeline(vedit::AppController& app, GuiState& gui,
                                  int media_index) {
    if (media_index < 0) return;
    vedit::TrackId track = resolve_target_track(app, gui);
    auto id = app.clip_append(track, media_index);
    if (id.valid()) {
        gui.selected_clip = id;
        const auto* item = app.project().library().item(media_index);
        gui.status = "Added " + (item ? basename_of(item->path) : "clip") +
                     " to " + app.project().timeline().track(track)->name + ".";
    } else {
        gui.status = "Could not add clip.";
    }
}

// ============================================================================
// Panels
// ============================================================================

static void draw_media_panel(vedit::AppController& app, GuiState& gui) {
    ImGui::TextUnformatted("Media");
    ImGui::Separator();

    if (ImGui::Button("Open video...")) {
        std::string path = open_file_dialog();
        if (!path.empty()) {
            int idx = app.library_import(path);
            if (idx >= 0) {
                gui.selected_media = idx;
                const auto* item = app.project().library().item(idx);
                if (item && item->fps > 0) gui.project_fps = item->fps;
                gui.status = "Imported " + basename_of(path) +
                             " — double-click it (or drag it) to add to the timeline.";
            } else {
                gui.status = "Could not open: " + path;
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d files)", app.library_count());

    ImGui::BeginChild("media_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (app.library_count() == 0) {
        ImGui::TextDisabled("No media yet.\nClick \"Open video...\" above,\n"
                            "or drag a file onto this window.");
    }
    for (int i = 0; i < app.library_count(); ++i) {
        const auto* item = app.project().library().item(i);
        if (!item) continue;
        char label[300];
        std::snprintf(label, sizeof(label), "%s  (%dx%d, %.0ffps)",
                      basename_of(item->path).c_str(),
                      item->width, item->height, item->fps);
        bool selected = (gui.selected_media == i);
        if (ImGui::Selectable(label, selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            gui.selected_media = i;
            if (item->fps > 0) gui.project_fps = item->fps;
            if (ImGui::IsMouseDoubleClicked(0)) {
                add_media_to_timeline(app, gui, i);
            }
        }
        if (ImGui::IsItemHovered()) {
            // Tooltip shows the stream list so the user can see which
            // tracks the source carries without opening ffprobe.
            std::string tip = "Double-click: append to the selected track.\n"
                              "Right-click: remove / switch stream.";
            if (!item->streams.empty()) {
                tip += "\n\nStreams:";
                for (const auto& s : item->streams) {
                    char line[160];
                    std::snprintf(line, sizeof(line),
                        "\n  [%d] %s %s%s%s",
                        s.index, s.type.c_str(), s.codec.c_str(),
                        s.is_default ? " (default)" : "",
                        s.language.empty() ? "" :
                            (" lang=" + s.language).c_str());
                    tip += line;
                    if (s.type == "video" && s.width > 0) {
                        char extra[64];
                        std::snprintf(extra, sizeof(extra),
                                      " %dx%d %.2ffps",
                                      s.width, s.height, s.frame_rate);
                        tip += extra;
                    } else if (s.type == "audio" && s.channels > 0) {
                        char extra[64];
                        std::snprintf(extra, sizeof(extra),
                                      " %dch %dHz", s.channels, s.sample_rate);
                        tip += extra;
                    }
                }
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (ImGui::BeginPopupContextItem(("##mctx%d" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem("Remove from library")) {
                if (app.library_remove(i)) {
                    gui.status = "Removed media from library.";
                    if (gui.selected_media == i) gui.selected_media = -1;
                    else if (gui.selected_media > i) --gui.selected_media;
                } else {
                    gui.status = "Cannot remove: media is in use on the timeline.";
                }
            }
            // Stream switch submenu. Lists each audio stream as a
            // radio-like entry; clicking one calls library.set_stream.
            // Refuses when the timeline references this source.
            bool has_audio_streams = false;
            for (const auto& s : item->streams) {
                if (s.type == "audio") { has_audio_streams = true; break; }
            }
            if (has_audio_streams && ImGui::BeginMenu("Switch audio stream")) {
                for (const auto& s : item->streams) {
                    if (s.type != "audio") continue;
                    char label2[128];
                    std::snprintf(label2, sizeof(label2),
                                  "[%d] %s %dch %dHz%s",
                                  s.index, s.codec.c_str(), s.channels,
                                  s.sample_rate,
                                  s.is_default ? "  *" : "");
                    if (ImGui::MenuItem(label2)) {
                        if (app.library_set_stream(i, -1, s.index)) {
                            gui.status = "Switched audio stream to [" +
                                         std::to_string(s.index) + "].";
                        } else {
                            gui.status = "Cannot switch: media is in use "
                                         "on the timeline.";
                        }
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

static void draw_preview_panel(vedit::AppController& app, GuiState& gui,
                               SDL_Texture* tex, int tex_w, int tex_h) {
    ImGui::TextUnformatted("Preview");
    ImGui::Separator();
    int64_t dur = app.project().timeline().duration();
    // Position / Duration — the total length is driven by the longest
    // track (add/move/trim clips to change it; the export matches this).
    ImGui::Text("Position %s   Duration %s",
                timecode(app.position(), gui.project_fps).c_str(),
                timecode(dur, gui.project_fps).c_str());

    if (tex && tex_w > 0 && tex_h > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        avail.y -= 4;
        float aspect = static_cast<float>(tex_w) / tex_h;
        float draw_w = avail.x;
        float draw_h = draw_w / aspect;
        if (draw_h > avail.y) { draw_h = avail.y; draw_w = draw_h * aspect; }
        if (draw_w < 1) draw_w = 1;
        if (draw_h < 1) draw_h = 1;
        ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                     ImVec2(draw_w, draw_h));
    } else {
        ImGui::TextDisabled("No preview yet.\nAdd a clip to the timeline.");
    }
}

static void draw_transport_panel(vedit::AppController& app, GuiState& gui) {
    int64_t dur = std::max<int64_t>(1, app.project().timeline().duration());

    if (ImGui::Button("|<")) app.seek(0);
    ImGui::SameLine();
    if (ImGui::Button("<")) app.player().step_backward();
    ImGui::SameLine();
    const char* play_label = app.is_playing() ? "Pause" : "Play";
    if (ImGui::Button(play_label)) {
        if (app.is_playing()) app.pause(); else app.play();
    }
    ImGui::SameLine();
    if (ImGui::Button(">")) app.player().step_forward();
    ImGui::SameLine();
    if (ImGui::Button(">|")) app.seek(dur - 1);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    int pos = static_cast<int>(app.position());
    if (ImGui::SliderInt("##seek", &pos, 0, static_cast<int>(dur - 1),
                         timecode(pos, gui.project_fps).c_str())) {
        app.seek(pos);
    }

    // Frame-accurate position display: frame number and total.
    ImGui::SameLine();
    ImGui::Text("f %d / %d", static_cast<int>(app.position()),
                static_cast<int>(dur));
}

// Clamp a trim so the clip stays within its source media bounds:
// 0 <= in < out <= source_len - 1.
static void clamp_trim(int64_t& new_in, int64_t& new_out, int64_t source_len) {
    if (new_in < 0) new_in = 0;
    if (source_len > 0 && new_out > source_len - 1) new_out = source_len - 1;
    if (new_out < new_in + 1) new_out = new_in + 1;
}

static void draw_timeline_panel(vedit::AppController& app, GuiState& gui) {
    // ---- Toolbar ----
    ImGui::TextUnformatted("Timeline");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);

    // Auto-fit: only re-fit when the user presses "Fit" or when content
    // first appears. We do NOT re-fit every frame, and we freeze auto-fit
    // entirely while a drag is in progress — otherwise duration changes
    // (move/trim) made zoom jump under the cursor, which felt like a
    // spring. The user gets linear, stable behavior during edits.
    int64_t dur_now = std::max<int64_t>(1, app.project().timeline().duration());
    bool editing = (gui.drag != DragMode::None);
    if (gui.zoom_auto && !editing && dur_now != gui.last_fit_duration) {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float usable = std::max(120.0f, avail_w - 64.0f - 40.0f);
        float fit = usable / static_cast<float>(dur_now);
        // Auto-fit floor is 0.005 px/frame so even a 30-min timeline
        // (45,000 frames) fits in ~225px. The clip is sub-pixel at
        // that zoom but both edges stay visible/grabbable. The user
        // zooms in for frame-level work.
        gui.zoom = std::clamp(fit, 0.005f, 30.0f);
        gui.last_fit_duration = dur_now;
    }

    // Plain language: "Zoom", not "px/f".
    float prev_zoom = gui.zoom;
    ImGui::SliderFloat("Zoom", &gui.zoom, 0.005f, 30.0f, "%.3f");
    if (gui.zoom != prev_zoom) {
        gui.zoom_auto = false;  // user took manual control
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) {
        gui.zoom_auto = true;
        gui.last_fit_duration = -1;  // force re-fit on next frame
    }

    ImGui::SameLine();
    bool has_sel = gui.selected_clip.valid() &&
                   app.project().timeline().clip(gui.selected_clip) != nullptr;
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Cut at playhead") && has_sel) {
        auto new_id = app.clip_split(gui.selected_clip, app.position());
        if (new_id.valid()) {
            gui.selected_clip = new_id;
            gui.status = "Cut the clip at the playhead.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete clip") && has_sel) {
        app.clip_remove(gui.selected_clip);
        gui.selected_clip = vedit::ClipId{0};
        gui.status = "Deleted clip.";
    }
    if (!has_sel) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("+ Video track")) {
        gui.active_track = app.track_add(vedit::TrackKind::Video);
        gui.status = "Added a video track. Upper tracks overlay lower ones.";
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Audio track")) {
        gui.active_track = app.track_add(vedit::TrackKind::Audio);
        gui.status = "Added an audio track.";
    }

    // Delete the active track. Visible button because right-click
    // context menus are not discoverable. Only enabled when there is
    // more than one track (the last track is kept so the timeline is
    // never empty).
    ImGui::SameLine();
    auto& tl_pre = app.project().timeline();
    bool can_delete_track = tl_pre.track_count() > 1 &&
                            gui.active_track.valid() &&
                            tl_pre.track(gui.active_track) != nullptr;
    if (!can_delete_track) ImGui::BeginDisabled();
    if (ImGui::Button("Delete track")) {
        std::string name = tl_pre.track(gui.active_track)->name;
        app.track_remove(gui.active_track);
        gui.active_track = tl_pre.tracks().empty()
            ? vedit::TrackId{0} : tl_pre.tracks()[0].id;
        gui.status = "Deleted track " + name + ".";
    }
    if (!can_delete_track) ImGui::EndDisabled();

    ImGui::Separator();

    auto& tl = app.project().timeline();
    auto tracks = tl.tracks();
    if (tracks.empty()) {
        ImGui::TextDisabled("No tracks yet. Open a video and double-click it "
                            "in the Media panel — a track is created for you.");
        return;
    }

    // Show which track new clips go to.
    if (gui.active_track.valid() && tl.track(gui.active_track)) {
        ImGui::TextDisabled("New clips go to: %s  (click another track's name to change it)",
                            tl.track(gui.active_track)->name.c_str());
    }

    const float ruler_h = 20.0f;
    const float track_h = 50.0f;
    const float label_w = 64.0f;
    const float px_per_frame = gui.zoom;
    const float edge_grab = 7.0f;
    int64_t dur = std::max<int64_t>(1, tl.duration());

    ImGui::BeginChild("tl_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Compute content size in double to avoid float precision loss on
    // long timelines at high zoom (e.g. 30 min at 30 px/frame). The
    // float cast is safe: even 10h at 30 px/frame fits in float with
    // sub-pixel precision.
    double content_w_d = static_cast<double>(label_w) +
                         static_cast<double>(dur) * static_cast<double>(px_per_frame) +
                         200.0;
    float content_w = static_cast<float>(content_w_d);
    float content_h = ruler_h + tracks.size() * track_h + 8;

    // InvisibleButton serves two purposes: (1) set the content width
    // so the horizontal scrollbar covers the full timeline, and (2)
    // provide hover/click hit-testing that accounts for scroll. The
    // button's rect is in absolute screen coordinates, so IsItemHovered
    // and IsItemClicked work correctly even when scrolled.
    ImGui::InvisibleButton("##tlcanvas", ImVec2(content_w, content_h));
    bool canvas_hovered = ImGui::IsItemHovered();
    bool mouse_pressed = ImGui::IsItemClicked(0);
    bool mouse_right = ImGui::IsItemClicked(1);
    bool mouse_down = ImGui::IsMouseDown(0);
    bool mouse_released = ImGui::IsMouseReleased(0);
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // Mouse-wheel zoom while hovering the timeline (Ctrl+wheel). Without
    // this the user can only zoom via the slider, and "zoom in to trim a
    // short clip" is painful.
    if (canvas_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            gui.zoom_auto = false;
            gui.zoom = std::clamp(gui.zoom * (1.0f + 0.1f * wheel), 0.005f, 30.0f);
        }
    }

    float tl_x0 = origin.x + label_w;
    float lanes_y0 = origin.y + ruler_h;

    auto x_to_frame = [&](float x) -> int64_t {
        float rel = x - tl_x0;
        if (rel < 0) rel = 0;
        return static_cast<int64_t>(rel / px_per_frame + 0.5f);
    };

    // Background.
    dl->AddRectFilled(origin, ImVec2(origin.x + content_w, origin.y + content_h),
                      IM_COL32(24, 24, 28, 255));

    // Ruler with second ticks (reads like a clock: 0:05, 0:10, ...).
    dl->AddRectFilled(ImVec2(origin.x, origin.y),
                      ImVec2(origin.x + content_w, lanes_y0),
                      IM_COL32(40, 40, 48, 255));
    {
        int fps = static_cast<int>(gui.project_fps > 0 ? gui.project_fps : 25);
        const float kMinLabelSpacing = 42.0f;
        float last_label_x = -kMinLabelSpacing;
        for (int64_t f = 0; f <= dur + fps; f += fps) {
            float x = tl_x0 + f * px_per_frame;
            if (x > origin.x + content_w) break;
            dl->AddLine(ImVec2(x, origin.y + 4), ImVec2(x, lanes_y0),
                        IM_COL32(90, 90, 100, 255), 1.0f);
            if (x - last_label_x >= kMinLabelSpacing) {
                std::string lbl = ruler_label(f, gui.project_fps);
                dl->AddText(ImVec2(x + 3, origin.y + 3),
                            IM_COL32(170, 170, 180, 255), lbl.c_str());
                last_label_x = x;
            }
        }
    }

    // Lanes + clips; record what the mouse is hovering.
    vedit::ClipId hover_clip{0};
    DragMode hover_mode = DragMode::None;
    int hover_track_index = -1;

    for (size_t t = 0; t < tracks.size(); ++t) {
        float y0 = lanes_y0 + t * track_h + 2;
        float y1 = y0 + track_h - 4;
        bool is_active = (tracks[t].id == gui.active_track);

        // Lane.
        dl->AddRectFilled(ImVec2(origin.x, y0),
                          ImVec2(origin.x + content_w, y1),
                          is_active ? IM_COL32(44, 48, 60, 255)
                                    : IM_COL32(34, 34, 40, 255));
        // Header cell.
        dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(tl_x0 - 2, y1),
                          is_active ? IM_COL32(60, 80, 120, 255)
                                    : IM_COL32(44, 44, 52, 255));
        dl->AddText(ImVec2(origin.x + 6, y0 + track_h * 0.5f - 7),
                    IM_COL32(230, 230, 240, 255), tracks[t].name.c_str());
        // Track type hint.
        const char* kind_hint = tracks[t].kind == vedit::TrackKind::Video
                                    ? "video" : "audio";
        dl->AddText(ImVec2(origin.x + 6, y0 + track_h * 0.5f + 6),
                    IM_COL32(150, 150, 160, 255), kind_hint);

        if (mouse.x >= origin.x && mouse.x < tl_x0 &&
            mouse.y >= y0 && mouse.y <= y1) {
            hover_track_index = static_cast<int>(t);
        }

        auto clips = tl.clips_on_track(tracks[t].id);
        for (const auto& c : clips) {
            float cx0 = tl_x0 + c.start * px_per_frame;
            float cx1 = cx0 + std::max<float>(3.0f, c.length * px_per_frame);
            bool selected = (c.id == gui.selected_clip);
            ImU32 fill = selected ? IM_COL32(90, 160, 250, 235)
                                  : IM_COL32(70, 110, 170, 220);
            dl->AddRectFilled(ImVec2(cx0, y0 + 2), ImVec2(cx1, y1 - 2), fill, 3.0f);
            dl->AddRect(ImVec2(cx0, y0 + 2), ImVec2(cx1, y1 - 2),
                        selected ? IM_COL32(255, 255, 255, 255)
                                 : IM_COL32(120, 160, 210, 255),
                        3.0f, 0, selected ? 2.0f : 1.0f);
            if (selected) {
                // Edge handles (visual cue that edges are grabbable).
                dl->AddRectFilled(ImVec2(cx0, y0 + 2), ImVec2(cx0 + 4, y1 - 2),
                                  IM_COL32(255, 230, 120, 255));
                dl->AddRectFilled(ImVec2(cx1 - 4, y0 + 2), ImVec2(cx1, y1 - 2),
                                  IM_COL32(255, 230, 120, 255));
            }
            const auto* item = app.project().library().item(c.media_index);
            std::string name = item ? basename_of(item->path) : "clip";
            dl->PushClipRect(ImVec2(cx0 + 5, y0), ImVec2(cx1 - 5, y1), true);
            dl->AddText(ImVec2(cx0 + 7, y0 + 6), IM_COL32(255, 255, 255, 255),
                        name.c_str());
            // Show the in/out range under the name.
            char rng[48];
            std::snprintf(rng, sizeof(rng), "in %lld  out %lld",
                          static_cast<long long>(c.in_point),
                          static_cast<long long>(c.out_point));
            dl->AddText(ImVec2(cx0 + 7, y0 + 22),
                        IM_COL32(220, 220, 230, 200), rng);
            dl->PopClipRect();

            // Hover hit-test. Only count the clip body (x >= tl_x0); the
            // track-label cell to the left is reserved for track selection.
            // Edge zones must not overlap: if the clip is too narrow for two
            // distinct edge zones, the whole clip is a Move (no trim) so the
            // user is never stuck "trimming when they meant to move".
            float clip_w = cx1 - cx0;
            bool has_room_for_edges = clip_w > edge_grab * 2 + 4;
            if (gui.drag == DragMode::None && canvas_hovered &&
                mouse.y >= y0 && mouse.y <= y1 &&
                mouse.x >= std::max(cx0, tl_x0) && mouse.x <= cx1) {
                hover_clip = c.id;
                if (has_room_for_edges && mouse.x <= cx0 + edge_grab)
                    hover_mode = DragMode::TrimLeft;
                else if (has_room_for_edges && mouse.x >= cx1 - edge_grab)
                    hover_mode = DragMode::TrimRight;
                else
                    hover_mode = DragMode::Move;
            }
        }
    }

    // Cursor feedback.
    if (hover_mode == DragMode::TrimLeft || hover_mode == DragMode::TrimRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (hover_mode == DragMode::Move)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Playhead (red vertical line).
    float ph_x = tl_x0 + app.position() * px_per_frame;
    dl->AddLine(ImVec2(ph_x, origin.y),
                ImVec2(ph_x, origin.y + content_h),
                IM_COL32(255, 80, 80, 255), 2.0f);

    // ---- Press: decide what we're doing ----
    if (mouse_pressed) {
        if (hover_track_index >= 0) {
            gui.active_track = tracks[hover_track_index].id;
            gui.status = "New clips will go to " +
                         tracks[hover_track_index].name + ".";
        } else if (mouse.y < lanes_y0) {
            gui.drag = DragMode::Scrub;
            app.seek(x_to_frame(mouse.x));
        } else if (hover_clip.valid()) {
            gui.selected_clip = hover_clip;
            const auto* ci = tl.clip(hover_clip);
            if (ci) {
                gui.drag = hover_mode;
                gui.drag_clip = hover_clip;
                gui.drag_orig_in = ci->in_point;
                gui.drag_orig_out = ci->out_point;
                gui.drag_orig_start = ci->start;
                gui.drag_grab_offset = x_to_frame(mouse.x) - ci->start;
                // Source length = out_point+1 relative to source; clamp trims
                // to [0, source_len-1]. We approximate source_len from the
                // media library item duration if available, else use out+1.
                const auto* item = app.project().library().item(ci->media_index);
                gui.drag_source_len = item ? item->frame_count : 0;
            }
        } else {
            gui.drag = DragMode::Scrub;
            app.seek(x_to_frame(mouse.x));
        }
    }

    // ---- Right-click on canvas: context menu for the hovered clip / track ----
    if (mouse_right) {
        if (hover_clip.valid()) {
            gui.selected_clip = hover_clip;
            ImGui::OpenPopup("##clip_ctx");
        } else if (hover_track_index >= 0) {
            gui.active_track = tracks[hover_track_index].id;
            ImGui::OpenPopup("##track_ctx");
        }
    }
    if (ImGui::BeginPopup("##clip_ctx")) {
        if (ImGui::MenuItem("Delete clip")) {
            app.clip_remove(gui.selected_clip);
            gui.selected_clip = vedit::ClipId{0};
            gui.status = "Deleted clip.";
        }
        if (ImGui::MenuItem("Cut at playhead")) {
            auto new_id = app.clip_split(gui.selected_clip, app.position());
            if (new_id.valid()) { gui.selected_clip = new_id;
                gui.status = "Cut the clip at the playhead."; }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##track_ctx")) {
        auto& tl2 = app.project().timeline();
        if (gui.active_track.valid() && tl2.track(gui.active_track)) {
            ImGui::Text("Track: %s", tl2.track(gui.active_track)->name.c_str());
            ImGui::Separator();
            if (tl2.track_count() > 1) {
                if (ImGui::MenuItem("Delete track")) {
                    app.track_remove(gui.active_track);
                    gui.active_track = tl2.tracks().empty()
                        ? vedit::TrackId{0} : tl2.tracks()[0].id;
                    gui.status = "Deleted track.";
                }
            } else {
                ImGui::TextDisabled("(can't delete the last track)");
            }
        }
        ImGui::EndPopup();
    }

    // ---- Drag: apply the in-progress operation ----
    if (mouse_down && gui.drag != DragMode::None) {
        int64_t frame = x_to_frame(mouse.x);
        const auto* ci = tl.clip(gui.drag_clip);
        switch (gui.drag) {
            case DragMode::Scrub:
                app.seek(std::min(frame, dur - 1));
                break;
            case DragMode::Move: {
                if (!ci) break;
                int64_t new_start = frame - gui.drag_grab_offset;
                if (new_start < 0) new_start = 0;
                vedit::TrackId dest = ci->track_id;
                int lane = static_cast<int>((mouse.y - lanes_y0) / track_h);
                if (lane >= 0 && lane < static_cast<int>(tracks.size()))
                    dest = tracks[lane].id;
                if (new_start != ci->start || dest != ci->track_id) {
                    app.clip_move(gui.drag_clip, dest, new_start);
                }
                break;
            }
            case DragMode::TrimLeft: {
                if (!ci) break;
                int64_t delta = frame - gui.drag_orig_start;
                int64_t new_in = gui.drag_orig_in + delta;
                int64_t new_out = gui.drag_orig_out;
                clamp_trim(new_in, new_out, gui.drag_source_len);
                app.clip_trim(gui.drag_clip, new_in, new_out);
                break;
            }
            case DragMode::TrimRight: {
                if (!ci) break;
                int64_t new_in = gui.drag_orig_in;
                int64_t new_out = gui.drag_orig_in +
                                  (frame - gui.drag_orig_start);
                clamp_trim(new_in, new_out, gui.drag_source_len);
                if (gui.drag_source_len > 0 && new_out >= gui.drag_source_len - 1) {
                    gui.status = "Reached the end of the source clip.";
                }
                app.clip_trim(gui.drag_clip, new_in, new_out);
                break;
            }
            default: break;
        }
    }

    if (mouse_released && gui.drag != DragMode::None) {
        if (gui.drag == DragMode::Move) gui.status = "Moved clip.";
        else if (gui.drag == DragMode::TrimLeft) gui.status = "Trimmed clip's left edge.";
        else if (gui.drag == DragMode::TrimRight) gui.status = "Trimmed clip's right edge.";
        gui.drag = DragMode::None;
        gui.drag_clip = vedit::ClipId{0};
        // Force auto-fit after trim/move so zoom adjusts to the new content.
        gui.last_fit_duration = -1;
    }

    ImGui::EndChild();

    // Plain-language help, shown only when no clip is selected.
    if (!has_sel) {
        ImGui::TextDisabled(
            "Tips: double-click media to add a clip | drag a clip to move it | "
            "drag a clip's edge to shorten/lengthen | click the ruler to scrub | "
            "right-click a clip or track for more");
    }
}

// ============================================================================
// Filter panel (right sidebar, always visible)
// ============================================================================

static void draw_filter_panel(vedit::AppController& app, GuiState& gui) {
    ImGui::TextUnformatted("Filters");
    ImGui::Separator();

    bool has_selection = gui.selected_clip.valid() || gui.active_track.valid();
    if (!has_selection) {
        ImGui::TextDisabled("Select a clip or track to edit filters.");
        return;
    }

    // Determine whether the active target is video, audio, or both.
    // A clip inherits its media type; a track is both unless all its
    // clips are the same type.
    bool target_is_video = true;
    bool target_is_audio = true;
    if (gui.selected_clip.valid()) {
        const auto* ci = app.project().timeline().clip(gui.selected_clip);
        if (ci) {
            const auto* item = app.project().library().item(ci->media_index);
            if (item) {
                target_is_video = item->has_video;
                target_is_audio = item->has_audio;
            }
        }
    }
    // For tracks, allow both video and audio filters.
    // Individual clip type filtering is only for clip-level filters.

    // Collect attached filters from state.
    auto attached = app.project().timeline().attached_filters();
    std::vector<const vedit::TimelineModel::AttachedFilterInfo*> visible;
    for (const auto& af : attached) {
        bool match = false;
        if (gui.selected_clip.valid() &&
            af.target_type == "clip" &&
            af.target_id == gui.selected_clip.value) match = true;
        if (gui.active_track.valid() &&
            af.target_type == "track" &&
            af.target_id == gui.active_track.value) match = true;
        if (match) visible.push_back(&af);
    }

    // --- Active filters ---
    if (visible.empty()) {
        ImGui::TextDisabled("No filters.");
    } else {
        ImGui::TextUnformatted("Active:");
        static int selected_filter_idx = -1;
        for (size_t i = 0; i < visible.size(); ++i) {
            const auto* af = visible[i];
            char label[128];
            std::snprintf(label, sizeof(label), "%s##act_%zu", af->filter_name.c_str(), i);
            bool sel = (static_cast<int>(i) == selected_filter_idx);
            if (ImGui::Selectable(label, &sel)) {
                selected_filter_idx = static_cast<int>(i);
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
            char xbtn[64];
            std::snprintf(xbtn, sizeof(xbtn), "X##rm_%zu", i);
            if (ImGui::SmallButton(xbtn)) {
                if (af->target_type == "clip") {
                    app.filter_remove(vedit::ClipId{af->target_id}, af->filter_name);
                } else {
                    app.track_filter_remove(vedit::TrackId{af->target_id}, af->filter_name);
                }
                selected_filter_idx = -1;
            }
        }

        // Show parameters for the selected filter.
        if (selected_filter_idx >= 0 &&
            selected_filter_idx < static_cast<int>(visible.size())) {
            const auto* af = visible[selected_filter_idx];
            ImGui::Separator();
            ImGui::TextUnformatted(af->filter_name.c_str());
            for (const auto& [k, v] : af->params) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", v.c_str());
                ImGui::PushID(k.c_str());
                ImGui::TextUnformatted(k.c_str());
                ImGui::SameLine(120);
                if (ImGui::InputText("##v", buf, sizeof(buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (af->target_type == "clip") {
                        app.filter_set_param(vedit::ClipId{af->target_id},
                                             af->filter_name, k, buf);
                    } else {
                        app.track_filter_set_param(vedit::TrackId{af->target_id},
                                                    af->filter_name, k, buf);
                    }
                }
                ImGui::PopID();
            }
        }
    }

    // --- Add filter ---
    ImGui::Separator();
    ImGui::TextUnformatted("Add:");

    // Build a filtered list: only show compatible filters.
    const auto& catalog = app.filter_catalog().filters();
    struct Candidate {
        size_t index;
        const char* label;
    };
    std::vector<Candidate> candidates;
    static char filter_search[64] = "";
    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##fsearch", "Search filters...",
                             filter_search, sizeof(filter_search));
    ImGui::PopItemWidth();

    for (size_t i = 0; i < catalog.size(); ++i) {
        const auto& f = catalog[i];
        // Only show compatible filters for clip targets.
        if (gui.selected_clip.valid()) {
            if (f.is_audio && !target_is_audio) continue;
            if (f.is_video && !target_is_video) continue;
        }
        // Filter by search text.
        if (filter_search[0]) {
            std::string lower_id = f.id;
            std::string lower_title = f.title;
            std::string lower_desc = f.description;
            std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(), ::tolower);
            std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(), ::tolower);
            std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(), ::tolower);
            std::string q(filter_search);
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            if (lower_id.find(q) == std::string::npos &&
                lower_title.find(q) == std::string::npos &&
                lower_desc.find(q) == std::string::npos) continue;
        }
        candidates.push_back({i, f.id.c_str()});
    }

    static int add_filter_idx = 0;
    if (candidates.empty()) {
        ImGui::TextDisabled("No matching filters.");
    } else {
        // Clamp add_filter_idx to valid range.
        if (add_filter_idx >= static_cast<int>(candidates.size()))
            add_filter_idx = 0;

        const char* preview = candidates[add_filter_idx].label;
        if (ImGui::BeginCombo("##addfilter", preview)) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                bool sel = (static_cast<int>(i) == add_filter_idx);
                const auto& f = catalog[candidates[i].index];
                char item_label[256];
                std::snprintf(item_label, sizeof(item_label), "%s##add_%zu", f.id.c_str(), i);
                if (ImGui::Selectable(item_label, sel)) {
                    add_filter_idx = static_cast<int>(i);
                }
                if (sel) ImGui::SetItemDefaultFocus();
                // Tooltip with description.
                if (ImGui::IsItemHovered() && !f.description.empty()) {
                    ImGui::SetTooltip("%s", f.description.c_str());
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
            const auto& fid = catalog[candidates[add_filter_idx].index].id;
            if (gui.selected_clip.valid()) {
                app.filter_add(gui.selected_clip, fid);
            } else if (gui.active_track.valid()) {
                app.track_filter_add(gui.active_track, fid);
            }
        }
    }

    // Composite geometry editor (video tracks only, not track 0).
    if (gui.active_track.valid()) {
        const auto* trk = app.project().timeline().track(gui.active_track);
        if (trk && trk->kind == vedit::TrackKind::Video) {
            int tidx = -1;
            auto tracks = app.project().timeline().tracks();
            for (size_t i = 0; i < tracks.size(); ++i) {
                if (tracks[i].id == gui.active_track) {
                    tidx = static_cast<int>(i);
                    break;
                }
            }
            if (tidx > 0) {
                ImGui::Separator();
                ImGui::TextUnformatted("Composite Geometry");
                ImGui::TextDisabled("x, y, w, h as %% of canvas");
                static float geo_x = 0, geo_y = 0, geo_w = 100, geo_h = 100;
                if (ImGui::SliderFloat("X %", &geo_x, 0, 100, "%.0f")) {
                    app.composite_geometry_set(gui.active_track,
                                               geo_x, geo_y, geo_w, geo_h);
                }
                if (ImGui::SliderFloat("Y %", &geo_y, 0, 100, "%.0f")) {
                    app.composite_geometry_set(gui.active_track,
                                               geo_x, geo_y, geo_w, geo_h);
                }
                if (ImGui::SliderFloat("W %", &geo_w, 0, 100, "%.0f")) {
                    app.composite_geometry_set(gui.active_track,
                                               geo_x, geo_y, geo_w, geo_h);
                }
                if (ImGui::SliderFloat("H %", &geo_h, 0, 100, "%.0f")) {
                    app.composite_geometry_set(gui.active_track,
                                               geo_x, geo_y, geo_w, geo_h);
                }
            }
        }
    }
}

// ============================================================================
// main
// ============================================================================

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_Window* window = SDL_CreateWindow(
        "vedit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1400, 860, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow failed\n"); SDL_Quit(); return 1; }

    set_window_icon(window);

#ifdef __APPLE__
    // Prefer Metal on macOS; SDL2 falls back to OpenGL automatically
    // if Metal is unavailable (e.g. older macOS versions).
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // DPI handling: compute the framebuffer-to-window ratio so we can
    // rasterise the font atlas at physical pixel density.  On macOS
    // Retina this is 2.0; on standard displays it is 1.0.  The
    // SDL_RenderSetScale call at the end makes SDL draw at physical
    // pixels, while ImGui layout stays in logical pixels thanks to
    // FontGlobalScale = 1/dpi_scale.
    ImGuiIO& io = ImGui::GetIO();
    int fb_w = 0, fb_h = 0;
    SDL_GetRendererOutputSize(renderer, &fb_w, &fb_h);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    float dpi_scale = (win_w > 0) ? static_cast<float>(fb_w) / win_w : 1.0f;
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;

    // Rasterize the font atlas at physical pixel density so glyphs
    // stay crisp on HiDPI displays.
    const float base_font_size = 16.0f;
    const float pixel_font_size = base_font_size * dpi_scale;
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 2;
    font_cfg.PixelSnapH = false;
    static const char* kFontCandidates[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
#elif defined(_WIN32)
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    ImFont* ui_font = nullptr;
    for (const char* path : kFontCandidates) {
        if (FILE* probe = std::fopen(path, "rb")) {
            std::fclose(probe);
            ui_font = io.Fonts->AddFontFromFileTTF(
                path, pixel_font_size, &font_cfg,
                io.Fonts->GetGlyphRangesChineseFull());
            if (ui_font) break;
        }
    }
    if (!ui_font) {
        ImFontConfig default_cfg;
        default_cfg.SizePixels = pixel_font_size;
        default_cfg.OversampleH = 2;
        default_cfg.OversampleV = 2;
        io.Fonts->AddFontDefault(&default_cfg);
    }
    // Layout stays in logical pixels; the atlas is in physical pixels.
    io.FontGlobalScale = 1.0f / dpi_scale;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    SDL_RenderSetScale(renderer, dpi_scale, dpi_scale);

    vedit::MltRuntime mlt;
    mlt.init();
    vedit::AppController app;
    if (const char* ac = std::getenv("VEDIT_AUDIO_CONSUMER"))
        app.player().set_audio_consumer(ac);

    if (NFD_Init() != NFD_OKAY) {
        vedit::Logger::log(vedit::Logger::Level::Warn,
                           "NFD_Init failed; file dialogs will not work");
    }

    vedit::rpc::Dispatcher dispatcher;
    vedit::rpc::register_methods(dispatcher, app);
    std::string socket_path = vedit::default_socket_path();
    vedit::rpc::RpcServer rpc_server(socket_path, dispatcher);
    rpc_server.start();

    GuiState gui;

    // Preview frame texture. Created lazily / recreated when the frame
    // size changes; streaming so per-frame pixel updates are cheap.
    SDL_Texture* preview_tex = nullptr;
    int tex_w = 0, tex_h = 0;

    bool running = true;
    while (running) {
        // Poll any active async export job and surface its progress.
        if (gui.export_job_id > 0) {
            auto r = app.export_poll(gui.export_job_id);
            if (r.found) {
                if (r.state == "running") {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "Exporting %s: %d%%",
                                  r.output_path.c_str(),
                                  static_cast<int>(r.fraction * 100));
                    gui.status = buf;
                } else {
                    gui.export_job_id = 0;
                    if (r.state == "done") {
                        gui.status = "Exported to " + r.output_path;
                    } else if (r.state == "cancelled") {
                        gui.status = "Export cancelled: " + r.output_path;
                    } else {
                        gui.status = "Export failed: " +
                                    (r.error.empty() ? r.output_path : r.error);
                    }
                }
            }
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                running = false;
            if (event.type == SDL_DROPFILE && event.drop.file) {
                std::string path = event.drop.file;
                SDL_free(event.drop.file);
                int idx = app.library_import(path);
                if (idx >= 0) {
                    gui.selected_media = idx;
                    const auto* item = app.project().library().item(idx);
                    if (item && item->fps > 0) gui.project_fps = item->fps;
                    gui.status = "Imported " + basename_of(path) +
                                 " — double-click it in Media to add to the timeline.";
                } else {
                    gui.status = "Could not open: " + path;
                }
            }
            // Arrow keys step frame-by-frame through the timeline.
            // Consume the event so ImGui does not also use it for
            // slider/focus navigation.
            if (event.type == SDL_KEYDOWN) {
                bool arrow_handled = false;
                if (event.key.keysym.sym == SDLK_LEFT) {
                    app.player().step_backward();
                    arrow_handled = true;
                } else if (event.key.keysym.sym == SDLK_RIGHT) {
                    app.player().step_forward();
                    arrow_handled = true;
                }
                if (arrow_handled) continue;
            }
        }

        rpc_server.drain();

        // If playback reached EOF (consumer self-stopped with
        // terminate_on_pause), the GUI's is_playing() is already false
        // but the consumer thread + running_ flag linger until an
        // explicit stop. Tear it down here so the producer is in a
        // clean state for the next Play/Seek, and the freeze frame
        // shown matches the final frame. Without this, Play after EOF
        // was flaky: the first click early-returned in start() (running_
        // still true), the second rebuilt the consumer against a
        // producer still holding the stale speed/position.
        if (app.player().has_self_stopped()) {
            app.pause();
        }

        int fw = 0, fh = 0;
        const uint8_t* fimg = nullptr;
        if (app.player().current_frame(fw, fh, fimg) && fimg && fw > 0 && fh > 0) {
            if (!preview_tex || fw != tex_w || fh != tex_h) {
                if (preview_tex) SDL_DestroyTexture(preview_tex);
                preview_tex = SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ABGR8888,
                    SDL_TEXTUREACCESS_STREAMING, fw, fh);
                tex_w = fw; tex_h = fh;
            }
            if (preview_tex) {
                // MLT delivers tightly packed RGBA; pitch is width * 4.
                SDL_UpdateTexture(preview_tex, nullptr, fimg, fw * 4);
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("vedit_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open video...")) {
                    std::string path = open_file_dialog();
                    if (!path.empty()) {
                        int idx = app.library_import(path);
                        if (idx >= 0) {
                            gui.selected_media = idx;
                            const auto* item = app.project().library().item(idx);
                            if (item && item->fps > 0) gui.project_fps = item->fps;
                            gui.status = "Imported " + basename_of(path);
                        }
                    }
                }
                if (ImGui::MenuItem("Export video...")) {
                    if (app.project().timeline().duration() > 0) {
                        std::string out = save_file_dialog("export.mp4");
                        if (!out.empty()) {
                            out = ensure_mp4_ext(out);
                            int64_t job_id = app.export_start(out);
                            if (job_id > 0) {
                                gui.export_job_id = job_id;
                                gui.status = "Exporting to " + out + " ...";
                            } else {
                                gui.status = "Export failed to start: " + out;
                            }
                        }
                    } else {
                        gui.status = "Nothing to export — add a clip first.";
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Project")) {
                if (ImGui::MenuItem("Canvas resolution...")) {
                    auto& p = app.profile();
                    gui.profile_dialog_w = p.width();
                    gui.profile_dialog_h = p.height();
                    gui.profile_dialog_fps = static_cast<float>(p.fps());
                    gui.show_profile_dialog = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // ---- Project settings dialog ----
        if (gui.show_profile_dialog) {
            ImGui::OpenPopup("Project Settings");
            gui.show_profile_dialog = false;
        }
        if (ImGui::BeginPopupModal("Project Settings", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Canvas resolution and framerate.");
            ImGui::Separator();

            // Raw inputs without +/- step buttons.
            ImGui::InputInt("Width", &gui.profile_dialog_w, 0, 0,
                            ImGuiInputTextFlags_CharsDecimal);
            ImGui::InputInt("Height", &gui.profile_dialog_h, 0, 0,
                            ImGuiInputTextFlags_CharsDecimal);
            // Quick-fill resolution presets.
            if (ImGui::Button("720p")) {
                gui.profile_dialog_w = 1280; gui.profile_dialog_h = 720;
            }
            ImGui::SameLine();
            if (ImGui::Button("1080p")) {
                gui.profile_dialog_w = 1920; gui.profile_dialog_h = 1080;
            }
            ImGui::SameLine();
            if (ImGui::Button("4K")) {
                gui.profile_dialog_w = 3840; gui.profile_dialog_h = 2160;
            }

            ImGui::Spacing();

            ImGui::InputFloat("FPS", &gui.profile_dialog_fps, 0.0f, 0.0f, "%.2f",
                              ImGuiInputTextFlags_CharsDecimal);
            // Quick-fill FPS presets.
            if (ImGui::Button("24")) gui.profile_dialog_fps = 24.0f;
            ImGui::SameLine();
            if (ImGui::Button("25")) gui.profile_dialog_fps = 25.0f;
            ImGui::SameLine();
            if (ImGui::Button("30")) gui.profile_dialog_fps = 30.0f;
            ImGui::SameLine();
            if (ImGui::Button("60")) gui.profile_dialog_fps = 60.0f;

            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                if (gui.profile_dialog_w > 0 && gui.profile_dialog_h > 0 &&
                    gui.profile_dialog_fps > 0) {
                    app.profile_set(gui.profile_dialog_w,
                                    gui.profile_dialog_h,
                                    gui.profile_dialog_fps);
                    gui.status = "Profile set to " +
                                 std::to_string(gui.profile_dialog_w) + "x" +
                                 std::to_string(gui.profile_dialog_h) + " @ " +
                                 std::to_string(gui.profile_dialog_fps) + "fps";
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        float full_w = ImGui::GetContentRegionAvail().x;
        float full_h = ImGui::GetContentRegionAvail().y;
        float left_w = full_w * 0.26f;
        float top_h = full_h * 0.52f;

        ImGui::BeginChild("left", ImVec2(left_w, top_h), ImGuiChildFlags_Borders);
        draw_media_panel(app, gui);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("preview", ImVec2(0, top_h), ImGuiChildFlags_Borders);
        draw_preview_panel(app, gui, preview_tex, tex_w, tex_h);
        ImGui::EndChild();

        // Transport bar height must follow the (DPI-scaled) widget
        // metrics, not a fixed pixel count: one button row plus the
        // child's top and bottom window padding. A hardcoded height
        // clipped the controls once ScaleAllSizes enlarged them.
        float transport_h = ImGui::GetFrameHeightWithSpacing()
                          + ImGui::GetStyle().WindowPadding.y * 2.0f;
        ImGui::BeginChild("transport", ImVec2(0, transport_h),
                          ImGuiChildFlags_Borders);
        draw_transport_panel(app, gui);
        ImGui::EndChild();

        float tl_w = full_w * 0.74f;
        ImGui::BeginChild("timeline", ImVec2(tl_w, 0), ImGuiChildFlags_Borders);
        draw_timeline_panel(app, gui);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("filters", ImVec2(0, 0), ImGuiChildFlags_Borders);
        draw_filter_panel(app, gui);
        ImGui::EndChild();

        ImGui::End();

        // Status line. Offsets derive from the scaled frame height so
        // the chip is not clipped at the window edge on HiDPI.
        float margin = ImGui::GetStyle().FramePadding.x + 4.0f;
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::SetNextWindowPos(ImVec2(
            vp->WorkPos.x + margin,
            vp->WorkPos.y + vp->WorkSize.y - ImGui::GetFrameHeight() - margin));
        ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted(gui.status.c_str());
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 18, 23, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    app.pause();
    rpc_server.stop();
    if (preview_tex) SDL_DestroyTexture(preview_tex);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    NFD_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
