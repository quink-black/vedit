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

struct GuiState {
    int selected_media = -1;
    vedit::ClipId selected_clip{0};
    std::string status = "Open a video to begin (File > Open, or the Open button).";
    double project_fps = 25.0;
    float zoom = 6.0f;  // pixels per frame on the timeline
};

// ============================================================================
// Panels
// ============================================================================

// Returns true if the media should be added to the timeline (double-click
// or the "Add to timeline" button).
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

    ImGui::BeginChild("media_list", ImVec2(0, 160), ImGuiChildFlags_Border);
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
                // Double-click = add to timeline. Auto-create a video
                // track if none exists, so the user never has to think
                // about tracks for the simple case.
                vedit::TrackId track;
                if (app.track_count() == 0) {
                    track = app.track_add(vedit::TrackKind::Video, "V1");
                } else {
                    track = app.project().timeline().tracks()[0].id;
                }
                auto id = app.clip_append(track, i);
                if (id.valid()) {
                    gui.selected_clip = id;
                    gui.status = "Added " + basename_of(item->path) +
                                 " to the timeline.";
                } else {
                    gui.status = "Could not add clip.";
                }
            }
        }
    }
    ImGui::EndChild();

    bool can_add = gui.selected_media >= 0;
    if (!can_add) ImGui::BeginDisabled();
    if (ImGui::Button("Add selected to timeline")) {
        vedit::TrackId track;
        if (app.track_count() == 0)
            track = app.track_add(vedit::TrackKind::Video, "V1");
        else
            track = app.project().timeline().tracks()[0].id;
        auto id = app.clip_append(track, gui.selected_media);
        if (id.valid()) {
            gui.selected_clip = id;
            gui.status = "Added clip to the timeline.";
        }
    }
    if (!can_add) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ Video track")) app.track_add(vedit::TrackKind::Video);
    ImGui::SameLine();
    if (ImGui::Button("+ Audio track")) app.track_add(vedit::TrackKind::Audio);
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

// Timeline with playhead, clip rectangles, click-to-scrub, click-to-select.
static void draw_timeline_panel(vedit::AppController& app, GuiState& gui) {
    ImGui::TextUnformatted("Timeline");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("zoom", &gui.zoom, 1.0f, 30.0f, "%.0f px/f");

    // Selected-clip actions.
    bool has_sel = gui.selected_clip.valid() &&
                   app.project().timeline().clip(gui.selected_clip) != nullptr;
    ImGui::SameLine();
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Split at playhead") && has_sel) {
        auto new_id = app.clip_split(gui.selected_clip, app.position());
        if (new_id.valid()) gui.status = "Split clip.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete clip") && has_sel) {
        app.clip_remove(gui.selected_clip);
        gui.selected_clip = vedit::ClipId{0};
        gui.status = "Deleted clip.";
    }
    if (!has_sel) ImGui::EndDisabled();

    ImGui::Separator();

    auto& tl = app.project().timeline();
    auto tracks = tl.tracks();
    if (tracks.empty()) {
        ImGui::TextDisabled("No tracks yet. Double-click a media item to add one automatically.");
        return;
    }

    const float track_h = 44.0f;
    const float label_w = 50.0f;
    const float px_per_frame = gui.zoom;
    int64_t dur = std::max<int64_t>(1, tl.duration());

    ImGui::BeginChild("tl_scroll", ImVec2(0, 0), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float content_w = label_w + dur * px_per_frame + 40;
    float content_h = tracks.size() * track_h + 8;

    // Reserve the canvas area + capture clicks.
    ImGui::InvisibleButton("##tlcanvas", ImVec2(content_w, content_h));
    bool canvas_hovered = ImGui::IsItemHovered();
    bool canvas_clicked = ImGui::IsItemClicked(0);
    ImVec2 mouse = ImGui::GetIO().MousePos;

    float tl_x0 = origin.x + label_w;  // where frame 0 starts

    // Background.
    dl->AddRectFilled(origin, ImVec2(origin.x + content_w, origin.y + content_h),
                      IM_COL32(24, 24, 28, 255));

    // Track lanes + labels + clips.
    vedit::ClipId clicked_clip{0};
    for (size_t t = 0; t < tracks.size(); ++t) {
        float y0 = origin.y + t * track_h + 2;
        float y1 = y0 + track_h - 4;
        // Lane background.
        dl->AddRectFilled(ImVec2(origin.x, y0),
                          ImVec2(origin.x + content_w, y1),
                          IM_COL32(34, 34, 40, 255));
        // Label.
        dl->AddText(ImVec2(origin.x + 6, y0 + track_h * 0.5f - 7),
                    IM_COL32(200, 200, 210, 255), tracks[t].name.c_str());

        auto clips = tl.clips_on_track(tracks[t].id);
        for (const auto& c : clips) {
            float cx0 = tl_x0 + c.start * px_per_frame;
            float cx1 = cx0 + std::max<float>(2.0f, c.length * px_per_frame);
            bool selected = (c.id == gui.selected_clip);
            ImU32 fill = selected ? IM_COL32(90, 160, 250, 235)
                                  : IM_COL32(70, 110, 170, 220);
            dl->AddRectFilled(ImVec2(cx0, y0 + 2), ImVec2(cx1, y1 - 2), fill, 3.0f);
            dl->AddRect(ImVec2(cx0, y0 + 2), ImVec2(cx1, y1 - 2),
                        selected ? IM_COL32(255, 255, 255, 255)
                                 : IM_COL32(120, 160, 210, 255),
                        3.0f, 0, selected ? 2.0f : 1.0f);
            // Clip name from media.
            const auto* item = app.project().library().item(c.media_index);
            std::string name = item ? basename_of(item->path) : "clip";
            dl->PushClipRect(ImVec2(cx0 + 2, y0), ImVec2(cx1 - 2, y1), true);
            dl->AddText(ImVec2(cx0 + 5, y0 + 6), IM_COL32(255, 255, 255, 255),
                        name.c_str());
            dl->PopClipRect();

            // Hit-test for selection.
            if (canvas_clicked && mouse.x >= cx0 && mouse.x <= cx1 &&
                mouse.y >= y0 && mouse.y <= y1) {
                clicked_clip = c.id;
            }
        }
    }

    // Playhead.
    float ph_x = tl_x0 + app.position() * px_per_frame;
    dl->AddLine(ImVec2(ph_x, origin.y),
                ImVec2(ph_x, origin.y + content_h),
                IM_COL32(255, 80, 80, 255), 2.0f);

    // Click handling: select a clip if one was hit, otherwise scrub.
    if (canvas_clicked) {
        if (clicked_clip.valid()) {
            gui.selected_clip = clicked_clip;
        }
        // Scrub to the clicked frame regardless (ruler-style).
        float rel = mouse.x - tl_x0;
        if (rel >= 0) {
            int64_t frame = static_cast<int64_t>(rel / px_per_frame);
            if (frame >= dur) frame = dur - 1;
            app.seek(frame);
        }
    }
    // Drag to scrub while holding the mouse on the canvas.
    if (canvas_hovered && ImGui::IsMouseDragging(0)) {
        float rel = mouse.x - tl_x0;
        if (rel >= 0) {
            int64_t frame = static_cast<int64_t>(rel / px_per_frame);
            if (frame >= dur) frame = dur - 1;
            if (frame < 0) frame = 0;
            app.seek(frame);
        }
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
