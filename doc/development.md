# Development Guide

## Prerequisites

```bash
sudo apt install cmake ninja-build pkg-config \
  libmlt++-dev libsdl2-dev libimgui-dev \
  libavformat-dev libavcodec-dev librtaudio-dev \
  libopencv-dev libfrei0r-dev libasio-dev \
  nlohmann-json3-dev catch2
```

## Build (Linux / macOS)

```bash
cd vedit
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Build (Windows, MSVC + vcpkg)

Prerequisites: Visual Studio 2022 with the C++ workload, and
[vcpkg](https://github.com/microsoft/vcpkg) at a known location.

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
```

vcpkg manifest mode installs all dependencies except MLT. MLT is built
from source via CMake ExternalProject_Add (avformat, xml, sdl2, and
core modules only). First build: ~10 minutes (MLT compile + vcpkg
installs). Incremental builds only recompile vedit.

## Run (Desktop)

```bash
# Linux / macOS
./build/src/app/vedit
```
```bat
:: Windows
build\src\app\Debug\vedit.exe
```

## Run (Headless Smoke Test)

On boxes without a display or audio device:

```bash
# Linux / macOS
SDL_VIDEODRIVER=offscreen VEDIT_AUDIO_CONSUMER=null ./build/src/app/vedit
```
```bat
:: Windows: SDL2 uses "dummy" for headless, not "offscreen"
set SDL_VIDEODRIVER=dummy
set VEDIT_AUDIO_CONSUMER=null
build\src\app\Debug\vedit.exe
```

## Run Tests

```bash
# Linux / macOS
cd build
VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ctest

# Or run the binary directly:
VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ./tests/vedit_tests

# Run a specific tag:
./tests/vedit_tests "[t5]"       # edit operations
./tests/vedit_tests "[t9]"       # export
./tests/vedit_tests "[t13]"      # preview/scrubbing
./tests/vedit_tests "[export]"   # all export tests (LSan off)
```
```bat
:: Windows
cd build
set SDL_VIDEODRIVER=dummy
set VEDIT_AUDIO_CONSUMER=null
ctest -C Debug
```

On Windows, `test_rpc_server.cpp` is excluded from the build (its raw
AF_UNIX client is a deferred port); the other ~40 test cases run.

## Known Quirks

- **MLT teardown SIGSEGV**: `consumer_avformat` and `rtaudio` consumers
  may crash during destructor cleanup. This is a known MLT quirk — the
  OS reclaims resources at process exit. It affects `ctest` (which sees
  the SIGSEGV as failure) but not the test binary itself.
- **No audio on headless**: Use `VEDIT_AUDIO_CONSUMER=null` for headless
  testing. The `null` consumer drives the frame pipeline without opening
  an audio device.
- **LADSPA warning**: "No LADSPA plugins were found!" at startup is
  harmless — we don't use LADSPA.
- **Windows headless driver**: SDL2 on Windows uses `SDL_VIDEODRIVER=
  dummy` for headless, not `offscreen` (which is the Linux value).

## Adding a New RPC Method

1. Add the handler lambda in `src/rpc/rpc_methods.cpp`:
   ```cpp
   d.register_method("new.method", [&](const nlohmann::json& params) {
       // Parse params, call app.method(), return result or throw.
       return nlohmann::json{{"ok", true}};
   });
   ```
2. Add a test in `tests/test_rpc.cpp` that verifies the method is
   registered and returns the expected response.
3. Update `README.md` method table.

## Adding a New Domain Feature

1. Add the model logic in `src/domain/` (e.g., a new method on
   `TimelineModel`).
2. Expose it through `AppController` (`src/domain/app_controller.cpp`).
3. Add a Catch2 test in `tests/test_domain.cpp` or
   `tests/test_app_controller.cpp`.
4. If it should be accessible via RPC, add a handler in
   `src/rpc/rpc_methods.cpp`.
5. If it needs a GUI widget, add it in `src/app/main.cpp`.

## Layer Discipline

- `vedit_util` → no MLT, no ImGui, no domain
- `vedit_core` → mlt++ only, no GUI
- `vedit_domain` → mlt++ + core, no GUI
- `vedit_rpc` → domain + nlohmann-json, no GUI
- `vedit` (exe) → everything

The build system enforces this through include paths — a `vedit_domain`
translation unit that tries to `#include <imgui.h>` will fail to
compile because that path is not in its include directories.

## MCP Server

`tools/vedit-mcp/` provides an MCP (Model Context Protocol) server that
exposes vedit's RPC surface to MCP clients. It connects over stdio and
auto-discovers running vedit instances.

```bash
cd tools/vedit-mcp
bash setup.sh   # creates .venv, installs deps, prints mcp.json config
```

## Git Workflow

- One commit per completed task (T1, T2, ...).
- Commit messages are imperative, ≤72 chars subject, body explains why.
- Stage explicit paths only (`git add src/... tests/...`), never `-A`.
- Each commit must build and pass tests.
