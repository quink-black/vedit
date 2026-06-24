#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include "core/mlt_runtime.h"
#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/timeline_model.h"
#include "rpc/rpc_dispatcher.h"
#include "rpc/rpc_methods.h"
#include "rpc/rpc_server.h"
#include "util/logger.h"

// ============================================================================
// Helpers
// ============================================================================

static std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

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

// Open a native file-open dialog via zenity (fork+exec). Returns the
// selected path or empty string if cancelled / zenity unavailable.
static std::string open_file_dialog() {
    FILE* pipe = popen(
        "zenity --file-selection "
        "--title='Open video file' "
        "--file-filter='Video | *.mp4 *.mkv *.mov *.avi *.webm *.m4v' "
        "--file-filter='All files | *' 2>/dev/null",
        "r");
    if (!pipe) return "";
    std::string result;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    // Strip trailing newline.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static std::string save_file_dialog(const char* default_name) {
    std::string cmd =
        "zenity --file-selection --save --confirm-overwrite "
        "--title='Export video' --filename='" + std::string(default_name) +
        "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// ============================================================================
// GUI state
// ============================================================================

// What kind of drag is in progress on the timeline.
enum class DragMode { None, Move, TrimLeft, TrimRight, Scrub };

struct GuiState {
    int selected_media = -1;
    vedit::ClipId selected_clip{0};
    vedit::TrackId active_track{0};   // target track for "add clip"
    std::string status = "Open a video to begin (File > Open).";
    double project_fps = 25.0;
    float zoom = 6.0f;  // pixels per frame on the timeline

    // Drag state for clip move/trim.
    DragMode drag = DragMode::None;
    vedit::ClipId drag_clip{0};
    int64_t drag_grab_offset = 0;     // frames between clip start and grab point
    int64_t drag_orig_in = 0;
    int64_t drag_orig_out = 0;
    int64_t drag_orig_start = 0;
};

// ============================================================================
// Panels
// ============================================================================

// Resolve the track a new clip should go on: the active track if it's
// still valid, otherwise the first track, auto-creating a video track if
// the timeline is empty.
static vedit::TrackId resolve_target_track(vedit::AppController& app,
                                           GuiState& gui) {
    auto& tl = app.project().timeline();
    if (gui.active_track.valid() && tl.track(gui.active_track) != nullptr) {
        return gui.active_track;
    }
    if (tl.track_count() == 0) {
        auto id = app.track_add(vedit::TrackKind::Video);
        gui.active_track = id;
        return id;
    }
    auto first = tl.tracks()[0].id;
    gui.active_track = first;
    return first;
}

// Append the selected media to the active track at the end of that track.
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
                             "  —  double-click it below to add to the timeline.";
            } else {
                gui.status = "Failed to open: " + path;
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d imported)", app.library_count());

    ImGui::BeginChild("media_list", ImVec2(0, 150), ImGuiChildFlags_Border);
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
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Double-click a media item to add it to the active track.");

    // Show / pick the active track so "Add" is unambiguous.
    auto& tl = app.project().timeline();
    std::string active_label = "(none)";
    if (gui.active_track.valid() && tl.track(gui.active_track))
        active_label = tl.track(gui.active_track)->name;
    ImGui::Text("Active track: %s", active_label.c_str());

    bool can_add = gui.selected_media >= 0;
    if (!can_add) ImGui::BeginDisabled();
    if (ImGui::Button("Add to active track")) {
        add_media_to_timeline(app, gui, gui.selected_media);
    }
    if (!can_add) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ Video track")) {
        gui.active_track = app.track_add(vedit::TrackKind::Video);
        gui.status = "Added a video track. New clips go here. (Click a track label to change the active track.)";
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Audio track")) {
        gui.active_track = app.track_add(vedit::TrackKind::Audio);
        gui.status = "Added an audio track.";
    }
}

static void draw_preview_panel(vedit::AppController& app, GuiState& gui,
                               GLuint tex, int tex_w, int tex_h) {
    ImGui::TextUnformatted("Preview");
    ImGui::Separator();
    int64_t dur = app.project().timeline().duration();
    ImGui::Text("Frame %lld / %lld   %s",
                static_cast<long long>(app.position()),
                static_cast<long long>(dur),
                timecode(app.position(), gui.project_fps).c_str());

    if (tex && tex_w > 0 && tex_h > 0) {
        // Fit the image into the available region preserving aspect.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        avail.y -= 4;
        float aspect = static_cast<float>(tex_w) / tex_h;
        float draw_w = avail.x;
        float draw_h = draw_w / aspect;
        if (draw_h > avail.y) { draw_h = avail.y; draw_w = draw_h * aspect; }
        if (draw_w < 1) draw_w = 1;
        if (draw_h < 1) draw_h = 1;
        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex)),
                     ImVec2(draw_w, draw_h));
    } else {
        ImGui::TextDisabled("No video. Add a clip to the timeline to see a preview here.");
    }
}

static void draw_transport_panel(vedit::AppController& app, GuiState& gui) {
    int64_t dur = std::max<int64_t>(1, app.project().timeline().duration());

    if (ImGui::Button("|<")) app.seek(0);
    ImGui::SameLine();
    if (ImGui::Button("<")) app.player().step_backward();
    ImGui::SameLine();
    if (ImGui::Button(app.is_playing() ? "Pause" : "Play")) {
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
}

// Full timeline: ruler (scrub), per-track lanes with labels (click to set
// active track), clip blocks that can be selected, moved (within/across
// tracks), and trimmed by their edges.
static void draw_timeline_panel(vedit::AppController& app, GuiState& gui) {
    ImGui::TextUnformatted("Timeline");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("zoom", &gui.zoom, 1.0f, 30.0f, "%.0f px/f");

    bool has_sel = gui.selected_clip.valid() &&
                   app.project().timeline().clip(gui.selected_clip) != nullptr;
    ImGui::SameLine();
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Split at playhead") && has_sel) {
        auto new_id = app.clip_split(gui.selected_clip, app.position());
        if (new_id.valid()) gui.status = "Split clip at playhead.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete clip") && has_sel) {
        app.clip_remove(gui.selected_clip);
        gui.selected_clip = vedit::ClipId{0};
        gui.status = "Deleted clip.";
    }
    if (!has_sel) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("| drag clip body = move, drag edges = trim, click label = active track");

    ImGui::Separator();

    auto& tl = app.project().timeline();
    auto tracks = tl.tracks();
    if (tracks.empty()) {
        ImGui::TextDisabled("No tracks. Import a video and double-click it, "
                            "or press '+ Video track'.");
        return;
    }

    const float ruler_h = 18.0f;
    const float track_h = 46.0f;
    const float label_w = 56.0f;
    const float px_per_frame = gui.zoom;
    const float edge_grab = 6.0f;  // px hot-zone for edge trim
    int64_t dur = std::max<int64_t>(1, tl.duration());

    ImGui::BeginChild("tl_scroll", ImVec2(0, 0), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float content_w = label_w + dur * px_per_frame + 200;
    float content_h = ruler_h + tracks.size() * track_h + 8;

    ImGui::InvisibleButton("##tlcanvas", ImVec2(content_w, content_h));
    bool canvas_hovered = ImGui::IsItemHovered();
    bool mouse_pressed = ImGui::IsItemClicked(0);
    bool mouse_down = ImGui::IsMouseDown(0);
    bool mouse_released = ImGui::IsMouseReleased(0);
    ImVec2 mouse = ImGui::GetIO().MousePos;

    float tl_x0 = origin.x + label_w;        // frame 0 x
    float lanes_y0 = origin.y + ruler_h;     // first lane y

    auto x_to_frame = [&](float x) -> int64_t {
        float rel = x - tl_x0;
        if (rel < 0) rel = 0;
        int64_t f = static_cast<int64_t>(rel / px_per_frame + 0.5f);
        if (f < 0) f = 0;
        return f;
    };

    // Background.
    dl->AddRectFilled(origin, ImVec2(origin.x + content_w, origin.y + content_h),
                      IM_COL32(24, 24, 28, 255));

    // Ruler with second ticks.
    dl->AddRectFilled(ImVec2(origin.x, origin.y),
                      ImVec2(origin.x + content_w, lanes_y0),
                      IM_COL32(40, 40, 48, 255));
    {
        int fps = static_cast<int>(gui.project_fps > 0 ? gui.project_fps : 25);
        for (int64_t f = 0; f <= dur; f += fps) {
            float x = tl_x0 + f * px_per_frame;
            dl->AddLine(ImVec2(x, origin.y + 4), ImVec2(x, lanes_y0),
                        IM_COL32(90, 90, 100, 255), 1.0f);
            char tb[16];
            std::snprintf(tb, sizeof(tb), "%llds",
                          static_cast<long long>(f / fps));
            dl->AddText(ImVec2(x + 2, origin.y + 2),
                        IM_COL32(150, 150, 160, 255), tb);
        }
    }

    // Determine what the mouse is over (for cursor + press handling).
    vedit::ClipId hover_clip{0};
    DragMode hover_mode = DragMode::None;
    int hover_track_index = -1;

    // Lanes + clips.
    for (size_t t = 0; t < tracks.size(); ++t) {
        float y0 = lanes_y0 + t * track_h + 2;
        float y1 = y0 + track_h - 4;
        bool is_active = (tracks[t].id == gui.active_track);

        // Lane background (active track highlighted).
        dl->AddRectFilled(ImVec2(origin.x, y0),
                          ImVec2(origin.x + content_w, y1),
                          is_active ? IM_COL32(44, 48, 60, 255)
                                    : IM_COL32(34, 34, 40, 255));
        // Track label cell.
        dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(tl_x0 - 2, y1),
                          is_active ? IM_COL32(60, 80, 120, 255)
                                    : IM_COL32(44, 44, 52, 255));
        dl->AddText(ImVec2(origin.x + 6, y0 + track_h * 0.5f - 7),
                    IM_COL32(220, 220, 230, 255), tracks[t].name.c_str());

        // Mouse over the label cell?
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
            // Edge handles (visual).
            if (selected) {
                dl->AddRectFilled(ImVec2(cx0, y0 + 2), ImVec2(cx0 + 3, y1 - 2),
                                  IM_COL32(255, 230, 120, 255));
                dl->AddRectFilled(ImVec2(cx1 - 3, y0 + 2), ImVec2(cx1, y1 - 2),
                                  IM_COL32(255, 230, 120, 255));
            }
            const auto* item = app.project().library().item(c.media_index);
            std::string name = item ? basename_of(item->path) : "clip";
            dl->PushClipRect(ImVec2(cx0 + 4, y0), ImVec2(cx1 - 4, y1), true);
            dl->AddText(ImVec2(cx0 + 6, y0 + 6), IM_COL32(255, 255, 255, 255),
                        name.c_str());
            dl->PopClipRect();

            // Hover hit-test (only when not already dragging).
            if (gui.drag == DragMode::None && canvas_hovered &&
                mouse.y >= y0 && mouse.y <= y1 &&
                mouse.x >= cx0 - edge_grab && mouse.x <= cx1 + edge_grab) {
                hover_clip = c.id;
                if (mouse.x <= cx0 + edge_grab) hover_mode = DragMode::TrimLeft;
                else if (mouse.x >= cx1 - edge_grab) hover_mode = DragMode::TrimRight;
                else hover_mode = DragMode::Move;
            }
        }
    }

    // Cursor feedback for trim edges.
    if (hover_mode == DragMode::TrimLeft || hover_mode == DragMode::TrimRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (hover_mode == DragMode::Move)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Playhead.
    float ph_x = tl_x0 + app.position() * px_per_frame;
    dl->AddLine(ImVec2(ph_x, origin.y),
                ImVec2(ph_x, origin.y + content_h),
                IM_COL32(255, 80, 80, 255), 2.0f);

    // ---- Press: decide what we're doing ----
    if (mouse_pressed) {
        if (hover_track_index >= 0) {
            // Clicked a track label -> make it the active track.
            gui.active_track = tracks[hover_track_index].id;
            gui.status = "Active track: " + tracks[hover_track_index].name +
                         "  (new clips go here)";
        } else if (mouse.y < lanes_y0) {
            // Clicked the ruler -> scrub.
            gui.drag = DragMode::Scrub;
            app.seek(x_to_frame(mouse.x));
        } else if (hover_clip.valid()) {
            // Clicked a clip -> select + begin move/trim.
            gui.selected_clip = hover_clip;
            const auto* ci = tl.clip(hover_clip);
            if (ci) {
                gui.drag = hover_mode;
                gui.drag_clip = hover_clip;
                gui.drag_orig_in = ci->in_point;
                gui.drag_orig_out = ci->out_point;
                gui.drag_orig_start = ci->start;
                gui.drag_grab_offset = x_to_frame(mouse.x) - ci->start;
            }
        } else {
            // Empty lane area -> scrub.
            gui.drag = DragMode::Scrub;
            app.seek(x_to_frame(mouse.x));
        }
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
                // Which track lane is the mouse over?
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
                // Move in-point: dragging right shortens from the left.
                int64_t delta = frame - gui.drag_orig_start;
                int64_t new_in = gui.drag_orig_in + delta;
                if (new_in < 0) new_in = 0;
                if (new_in > gui.drag_orig_out - 1) new_in = gui.drag_orig_out - 1;
                app.clip_trim(gui.drag_clip, new_in, gui.drag_orig_out);
                break;
            }
            case DragMode::TrimRight: {
                if (!ci) break;
                int64_t new_out = gui.drag_orig_in +
                                  (frame - gui.drag_orig_start);
                if (new_out <= gui.drag_orig_in + 1)
                    new_out = gui.drag_orig_in + 1;
                app.clip_trim(gui.drag_clip, gui.drag_orig_in, new_out);
                break;
            }
            default: break;
        }
    }

    if (mouse_released && gui.drag != DragMode::None) {
        if (gui.drag == DragMode::Move) gui.status = "Moved clip.";
        else if (gui.drag == DragMode::TrimLeft ||
                 gui.drag == DragMode::TrimRight) gui.status = "Trimmed clip.";
        gui.drag = DragMode::None;
        gui.drag_clip = vedit::ClipId{0};
    }

    ImGui::EndChild();
}

// ============================================================================
// main
// ============================================================================

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "vedit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1400, 860, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow failed\n"); SDL_Quit(); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) { std::fprintf(stderr, "GL context failed\n"); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    vedit::MltRuntime mlt;
    mlt.init();
    vedit::AppController app;
    if (const char* ac = std::getenv("VEDIT_AUDIO_CONSUMER"))
        app.player().set_audio_consumer(ac);

    vedit::rpc::Dispatcher dispatcher;
    vedit::rpc::register_methods(dispatcher, app);
    vedit::rpc::RpcServer rpc_server;
    rpc_server.start([&](const std::string& req) {
        return dispatcher.handle_request(req);
    });

    GuiState gui;

    GLuint preview_tex = 0;
    glGenTextures(1, &preview_tex);
    glBindTexture(GL_TEXTURE_2D, preview_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    int tex_w = 0, tex_h = 0;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        rpc_server.drain();

        // Get the current frame (realtime if playing, freeze if paused)
        // and upload it to the preview texture.
        int fw = 0, fh = 0;
        const uint8_t* fimg = nullptr;
        if (app.player().current_frame(fw, fh, fimg) && fimg && fw > 0 && fh > 0) {
            glBindTexture(GL_TEXTURE_2D, preview_tex);
            if (fw != tex_w || fh != tex_h) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fimg);
                tex_w = fw; tex_h = fh;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh,
                                GL_RGBA, GL_UNSIGNED_BYTE, fimg);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Full-window layout: one host window split into regions, so the
        // app looks like a real editor rather than scattered float panels.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("vedit_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_MenuBar);

        // Menu bar.
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
                            // Synchronous export (blocks the UI). M1 scope —
                            // pause playback first so the consumer is free.
                            app.pause();
                            vedit::ExportService svc;
                            bool ok = svc.render(*app.project().producer(),
                                                 app.profile(), out);
                            gui.status = ok ? ("Exported to " + out)
                                            : ("Export failed: " + out);
                        }
                    } else {
                        gui.status = "Nothing to export — timeline is empty.";
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) running = false;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        float full_w = ImGui::GetContentRegionAvail().x;
        float full_h = ImGui::GetContentRegionAvail().y;
        float left_w = full_w * 0.28f;
        float top_h = full_h * 0.55f;

        // --- Left: media ---
        ImGui::BeginChild("left", ImVec2(left_w, top_h), ImGuiChildFlags_Border);
        draw_media_panel(app, gui);
        ImGui::EndChild();

        ImGui::SameLine();

        // --- Right: preview ---
        ImGui::BeginChild("preview", ImVec2(0, top_h), ImGuiChildFlags_Border);
        draw_preview_panel(app, gui, preview_tex, tex_w, tex_h);
        ImGui::EndChild();

        // --- Transport bar ---
        ImGui::BeginChild("transport", ImVec2(0, 36), ImGuiChildFlags_Border);
        draw_transport_panel(app, gui);
        ImGui::EndChild();

        // --- Bottom: timeline ---
        ImGui::BeginChild("timeline", ImVec2(0, 0), ImGuiChildFlags_Border);
        draw_timeline_panel(app, gui);
        ImGui::EndChild();

        ImGui::End();  // root

        // Status line overlay.
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8,
                                       vp->WorkPos.y + vp->WorkSize.y - 28));
        ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted(gui.status.c_str());
        ImGui::End();

        ImGui::Render();
        int ww = 0, wh = 0;
        SDL_GL_GetDrawableSize(window, &ww, &wh);
        glViewport(0, 0, ww, wh);
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    app.pause();
    rpc_server.stop();
    if (preview_tex) glDeleteTextures(1, &preview_tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
