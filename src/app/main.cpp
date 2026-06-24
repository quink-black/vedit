#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include "core/mlt_runtime.h"
#include "domain/app_controller.h"
#include "domain/ids.h"
#include "domain/timeline_model.h"
#include "util/logger.h"

// ============= helpers ======================================================

static std::string basename(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

static std::string timecode_string(int64_t frame, double fps = 25.0) {
    int total_sec = static_cast<int>(frame / fps);
    int h = total_sec / 3600;
    int m = (total_sec % 3600) / 60;
    int s = total_sec % 60;
    int f = static_cast<int>(frame) % static_cast<int>(fps);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%02d", h, m, s, f);
    return buf;
}

// ============= timeline drawing =============================================

struct TimelineLayout {
    float x_start;       // pixel x of timeline origin
    float y_top;         // pixel y of first track
    float track_height;  // pixels per track
    float px_per_frame;  // horizontal scale
    ImDrawList* dl;
};

static void draw_timeline(TimelineLayout& tl, vedit::TimelineModel& model) {
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    tl.x_start = cursor.x;
    tl.y_top = cursor.y;
    tl.track_height = 32.0f;
    // ~8 px per frame so a 2s @ 25fps clip = 50 frames = 400 px wide
    tl.px_per_frame = 8.0f;
    tl.dl = ImGui::GetWindowDrawList();

    auto tracks = model.tracks();
    int n = static_cast<int>(tracks.size());
    float total_w = std::max(400.0f, model.duration() * tl.px_per_frame);
    float total_h = n * tl.track_height + 4.0f;

    ImGui::InvisibleButton("##timeline_canvas", ImVec2(total_w, total_h),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);

    // Background + grid.
    tl.dl->AddRectFilled(cursor, ImVec2(cursor.x + total_w, cursor.y + total_h),
                         IM_COL32(30, 30, 35, 255));
    for (int t = 0; t < n; ++t) {
        float y = cursor.y + t * tl.track_height;
        tl.dl->AddLine(ImVec2(cursor.x, y), ImVec2(cursor.x + total_w, y),
                       IM_COL32(60, 60, 65, 255), 1.0f);
    }
    tl.dl->AddLine(ImVec2(cursor.x, cursor.y + total_h - 1),
                   ImVec2(cursor.x + total_w, cursor.y + total_h - 1),
                   IM_COL32(60, 60, 65, 255), 1.0f);

    // Clips as colored rectangles with text labels.
    for (int t = 0; t < n; ++t) {
        float y = cursor.y + t * tl.track_height;
        auto clips = model.clips_on_track(tracks[t].id);
        for (const auto& c : clips) {
            float x0 = cursor.x + c.start * tl.px_per_frame;
            float x1 = x0 + c.length * tl.px_per_frame;
            if (x1 < x0 + 2.0f) x1 = x0 + 2.0f;
            ImU32 col = IM_COL32(80, 140, 220, 200);
            tl.dl->AddRectFilled(ImVec2(x0, y + 2), ImVec2(x1, y + tl.track_height - 2), col);
            tl.dl->AddRect(ImVec2(x0, y + 2), ImVec2(x1, y + tl.track_height - 2),
                           IM_COL32(120, 180, 255, 255));
            // Clip label: media index + frame range.
            char label[64];
            std::snprintf(label, sizeof(label), "m%d %lld-%lld",
                          c.media_index,
                          static_cast<long long>(c.in_point),
                          static_cast<long long>(c.out_point));
            tl.dl->AddText(ImVec2(x0 + 4, y + 6), IM_COL32(255, 255, 255, 255), label);
        }
        // Track label.
        char tname[16];
        std::snprintf(tname, sizeof(tname), "%s", tracks[t].name.c_str());
        tl.dl->AddText(ImVec2(cursor.x - 36, y + 4), IM_COL32(180, 180, 190, 255), tname);
    }
}

// ============= main =========================================================

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
        1400, 800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow failed\n"); SDL_Quit(); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) { std::fprintf(stderr, "SDL_GL_CreateContext failed\n"); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    vedit::MltRuntime mlt;
    mlt.init();
    vedit::AppController app;

    // Headless smoke path: use null audio consumer.
    if (const char* ac = std::getenv("VEDIT_AUDIO_CONSUMER"))
        app.player().set_audio_consumer(ac);

    // Preview texture for the renderer.
    GLuint preview_tex = 0;
    glGenTextures(1, &preview_tex);
    glBindTexture(GL_TEXTURE_2D, preview_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    int last_tex_w = 0, last_tex_h = 0;

    // UI state.
    char import_buf[512] = {0};
    std::vector<std::string> media_paths;
    std::string status_msg;
    int selected_media = -1;   // selected library index for "add to track"
    int selected_track = 0;    // which track index to append to
    float fps = 25.0f;

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

        // Pull latest frame from the player and upload to the texture.
        mlt_image_format fmt = mlt_image_none;
        int fw = 0, fh = 0;
        const uint8_t* fimg = nullptr;
        int64_t fpos = 0;
        if (app.player().pull_latest_frame(fmt, fw, fh, fimg, fpos)) {
            if (fimg && fw > 0 && fh > 0) {
                glBindTexture(GL_TEXTURE_2D, preview_tex);
                if (fw != last_tex_w || fh != last_tex_h) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, fimg);
                    last_tex_w = fw; last_tex_h = fh;
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh,
                                    GL_RGBA, GL_UNSIGNED_BYTE, fimg);
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ==================== Media Library ============================
        ImGui::Begin("Media Library");
        ImGui::InputText("Path", import_buf, sizeof(import_buf));
        ImGui::SameLine();
        if (ImGui::Button("Import") && import_buf[0]) {
            int idx = app.library_import(import_buf);
            if (idx >= 0) {
                media_paths.push_back(import_buf);
                status_msg = "imported: " + std::string(import_buf);
                import_buf[0] = 0;
            } else {
                status_msg = "import FAILED: " + std::string(import_buf);
            }
        }
        for (int i = 0; i < app.library_count(); ++i) {
            const auto* item = app.project().library().item(i);
            if (!item) continue;
            char label[256];
            std::snprintf(label, sizeof(label), "%d: %s (%dx%d, %.1ffps)",
                          i, basename(item->path).c_str(), item->width, item->height, item->fps);
            if (ImGui::Selectable(label, selected_media == i)) {
                selected_media = i;
                fps = item->fps > 0 ? item->fps : 25.0;
            }
        }
        ImGui::End();

        // ==================== Tracks ===================================
        ImGui::Begin("Timeline");
        auto& tl = app.project().timeline();
        if (ImGui::Button("+V")) { app.track_add(vedit::TrackKind::Video); }
        ImGui::SameLine();
        if (ImGui::Button("+A")) { app.track_add(vedit::TrackKind::Audio); }
        ImGui::SameLine();
        // Track selector for "append clip"
        ImGui::Text("append to track:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputInt("##append_track", &selected_track, 0, 0);
        ImGui::SameLine();
        if (ImGui::Button("Append Clip") && selected_media >= 0) {
            auto tracks = tl.tracks();
            if (selected_track >= 0 && selected_track < static_cast<int>(tracks.size())) {
                auto id = app.clip_append(tracks[selected_track].id, selected_media);
                if (id.valid()) {
                    status_msg = "appended clip " + std::to_string(id.value);
                } else {
                    status_msg = "append failed";
                }
            }
        }
        ImGui::Separator();

        // Draw the timeline canvas.
        if (tl.track_count() > 0) {
            ImGui::BeginChild("timeline_canvas", ImVec2(0, 0),
                              ImGuiChildFlags_Border);
            TimelineLayout tlay;
            draw_timeline(tlay, tl);
            ImGui::EndChild();
        }
        ImGui::End();

        // ==================== Preview ==================================
        ImGui::Begin("Preview");
        if (preview_tex && last_tex_w > 0) {
            ImGui::Text("%s", timecode_string(app.position(), fps).c_str());
            ImGui::Image(reinterpret_cast<ImTextureID>(
                             static_cast<intptr_t>(preview_tex)),
                         ImVec2(static_cast<float>(last_tex_w),
                                static_cast<float>(last_tex_h)));
        } else {
            ImGui::Text("no video");
        }
        ImGui::End();

        // ==================== Transport ================================
        ImGui::Begin("Transport");
        if (ImGui::Button("|<")) app.seek(0);
        ImGui::SameLine();
        if (ImGui::Button(app.is_playing() ? "||" : ">")) {
            if (app.is_playing()) app.pause(); else app.play();
        }
        ImGui::SameLine();
        // Seek bar: slider tied to position.
        int64_t dur = app.project().timeline().duration();
        int pos_int = static_cast<int>(app.position());
        if (ImGui::SliderInt("##seekbar", &pos_int, 0,
                             std::max(1, static_cast<int>(dur)),
                             "%d")) {
            app.seek(pos_int);
        }
        ImGui::SameLine();
        ImGui::Text("%s / %s",
                    timecode_string(app.position(), fps).c_str(),
                    timecode_string(dur, fps).c_str());
        ImGui::End();

        // ==================== Status ===================================
        ImGui::Begin("Status");
        ImGui::Text("%s", status_msg.c_str());
        ImGui::End();

        ImGui::Render();
        int ww = 0, wh = 0;
        SDL_GL_GetDrawableSize(window, &ww, &wh);
        glViewport(0, 0, ww, wh);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    app.pause();
    if (preview_tex) glDeleteTextures(1, &preview_tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
