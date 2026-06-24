#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include <Mlt.h>

#include "core/mlt_runtime.h"
#include "core/preview_renderer.h"
#include "domain/project_model.h"
#include "util/logger.h"

int main(int argc, char* argv[]) {
    std::string media_path;
    if (argc >= 2) media_path = argv[1];

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
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    vedit::MltRuntime mlt;
    if (!mlt.init()) {
        vedit::Logger::log(vedit::Logger::Level::Error, "MLT init failed");
    }
    vedit::ProjectModel project;

    // T3 preview path: a single Producer -> PreviewRenderer. The
    // tractor/timeline replaces this in T4-T6; PreviewRenderer then
    // connects to whatever producer ProjectModel exposes.
    std::unique_ptr<Mlt::Producer> producer;
    vedit::PreviewRenderer preview;
    bool preview_started = false;
    GLuint preview_texture = 0;
    glGenTextures(1, &preview_texture);
    glBindTexture(GL_TEXTURE_2D, preview_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    int last_tex_w = 0, last_tex_h = 0;
    int64_t last_position = -1;
    int frames_shown = 0;

    if (!media_path.empty()) {
        Mlt::Profile profile;
        producer = std::make_unique<Mlt::Producer>(profile, media_path.c_str());
        if (!producer->is_valid()) {
            vedit::Logger::log(vedit::Logger::Level::Error,
                               "failed to open producer (check path / format)");
            producer.reset();
        } else {
            // VEDIT_AUDIO_CONSUMER env lets the smoke test use "null" on
            // headless boxes without an audio device. Real runs leave it
            // unset to use the default sdl2_audio.
            if (const char* ac = std::getenv("VEDIT_AUDIO_CONSUMER")) {
                preview.set_audio_consumer(ac);
            }
            preview.set_producer(producer.get());
            preview_started = preview.start();
        }
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        // Pull the latest frame from the MLT consumer thread and upload
        // it to the preview texture. Main-thread only: GL is not safe
        // from the consumer thread.
        mlt_image_format fmt = mlt_image_none;
        int w = 0, h = 0;
        const uint8_t* image = nullptr;
        int64_t pos = 0;
        if (preview.pull_latest_frame(fmt, w, h, image, pos)) {
            if (image && w > 0 && h > 0) {
                glBindTexture(GL_TEXTURE_2D, preview_texture);
                if (w != last_tex_w || h != last_tex_h) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, image);
                    last_tex_w = w;
                    last_tex_h = h;
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                                    GL_RGBA, GL_UNSIGNED_BYTE, image);
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                last_position = pos;
                ++frames_shown;
            }
        }

        // Exit when MLT self-stops at end of clip (terminate_on_pause),
        // OR when the playhead reaches the producer length (fallback for
        // cases where the consumer-stopped event does not fire).
        if (preview_started) {
            if (!preview.is_running()) {
                vedit::Logger::log(vedit::Logger::Level::Info,
                                   "main: preview no longer running, exiting");
                running = false;
            } else if (producer && producer->is_valid() &&
                       last_position >= 0 &&
                       last_position >= producer->get_length() - 1) {
                vedit::Logger::log(vedit::Logger::Level::Info,
                                   "main: reached end of clip, exiting");
                running = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Preview");
        if (preview_texture && last_tex_w > 0) {
            ImGui::Text("frame %lld  (%dx%d)  shown:%d",
                        static_cast<long long>(last_position),
                        last_tex_w, last_tex_h, frames_shown);
            ImGui::Image(reinterpret_cast<ImTextureID>(
                             static_cast<intptr_t>(preview_texture)),
                         ImVec2(last_tex_w, last_tex_h));
        } else if (media_path.empty()) {
            ImGui::Text("usage: vedit <media-file>");
            ImGui::Text("(T3 smoke: pass a video file to test the preview path)");
        } else {
            ImGui::Text("no frames yet (producer valid? %s)",
                        producer && producer->is_valid() ? "yes" : "no");
        }
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

    preview.stop();
    producer.reset();
    if (preview_texture) glDeleteTextures(1, &preview_texture);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
