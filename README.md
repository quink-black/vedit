# vedit — Agent-Native Collaborative NLE

**vedit** is an experimental non-linear video editor built ON
[MLT](https://www.mltframework.org/) (the engine behind Shotcut,
Kdenlive, Flowblade). Its defining architectural choice is a
**single-state, dual-channel control plane**: a local GUI and a remote
agent (JSON-RPC over Unix domain socket) drive the same domain model
through the same `AppController` seam. Neither channel has a privileged
code path.

> Status: **Milestone 1 complete** (multi-track editing + composition +
> export + save/load + RPC control plane). See [doc/](doc/) for the
> full design rationale, scope boundary, and task breakdown.

---

## Architecture

```
┌─────────────┐     ┌─────────────────────┐     ┌──────────────┐
│  ImGui GUI   │     │   JSON-RPC Agent    │     │  FFmpeg CLI  │
│ (SDL2+GL)   │     │ (UDS vedit-<pid>.sock)  │   │  (export)    │
└──────┬──────┘     └──────────┬──────────┘     └──────┬───────┘
       │                       │                       │
       └───────────┬───────────┘                       │
                   ▼                                   │
         ┌─────────────────┐                           │
         │  AppController  │  ← single seam            │
         │  (domain seam)  │                           │
         └────────┬────────┘                           │
                  │                                     │
     ┌────────────┼────────────┐                        │
     ▼            ▼            ▼                        │
┌─────────┐ ┌──────────┐ ┌──────────┐                   │
│Project  │ │Player    │ │Export    │                   │
│Model    │ │Controller│ │Service   │                   │
└────┬────┘ └────┬─────┘ └────┬─────┘                   │
     │           │             │                         │
     └───────┬───┴─────────────┘                         │
             ▼                                           │
     ┌───────────────┐                                   │
     │  MLT (mlt++)  │ ← engine                          │
     │  Tractor /    │                                   │
     │  Multitrack / │                                   │
     │  Playlist /   │                                   │
     │  Consumer     │                                   │
     └───────────────┘                                   │
```

### Layer Rules (enforced at compile time)

| Layer | Lib | May include | Must NOT include |
|---|---|---|---|
| `vedit_util` | Logger | std headers | MLT, ImGui, domain |
| `vedit_core` | MltRuntime, PreviewRenderer | util, mlt++ | GUI headers, domain |
| `vedit_domain` | ProjectModel, TimelineModel, PlayerController, etc. | core, mlt++ | GUI headers, ImGui |
| `vedit_rpc` | Dispatcher, RpcServer, method handlers | domain, nlohmann-json | GUI headers |
| `vedit` (exe) | ImGui panels, SDL2 window | all layers | — |

---

## Build & Run

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install cmake ninja-build pkg-config \
  libmlt++-dev libsdl2-dev libimgui-dev \
  libavformat-dev libavcodec-dev librtaudio-dev \
  libopencv-dev libfrei0r-dev libasio-dev \
  nlohmann-json3-dev catch2
```

### Build (Linux / macOS)

```bash
cd vedit
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Build (Windows, MSVC + vcpkg)

Prerequisites: Visual Studio 2022 with the C++ workload, and
[vcpkg](https://github.com/microsoft/vcpkg) at a known location.

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
```

vcpkg manifest mode installs all dependencies except MLT, which is
built from source via CMake ExternalProject_Add (only the avformat,
xml, sdl2, and core modules; Qt/opencv/frei0r are not pulled in). The
first build takes ~10 minutes for the MLT compile; incremental builds
only recompile vedit.

### Run (desktop)

```bash
# Linux
./build/src/app/vedit
# macOS -- the target is an app bundle
./build/src/app/vedit.app/Contents/MacOS/vedit
```
```bat
:: Windows
build\src\app\Debug\vedit.exe
```

### Run (headless smoke test)

```bash
# Linux
SDL_VIDEODRIVER=offscreen VEDIT_AUDIO_CONSUMER=null ./build/src/app/vedit
```
```bat
:: Windows (SDL2 uses "dummy", not "offscreen")
set SDL_VIDEODRIVER=dummy
set VEDIT_AUDIO_CONSUMER=null
build\src\app\Debug\vedit.exe
```

macOS has no working headless video driver: `SDL_VIDEODRIVER=offscreen`
fails with `SDL_CreateWindow failed` and `SDL_VIDEODRIVER=dummy` fails with
`SDL_CreateRenderer failed: Parameter 'renderer' is invalid`. The windowed
target needs a real window session there, while
`VEDIT_AUDIO_CONSUMER=null` still applies. The automated suite runs
headless on every platform because the test cases drive the controller
directly rather than the window.

### Run tests

```bash
# Linux / macOS
cd build
VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ctest
# or directly:
VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ./tests/vedit_tests

# Run a specific tag:
./tests/vedit_tests "[t5]"
./tests/vedit_tests "[export]"
```
```bat
:: Windows
cd build
set SDL_VIDEODRIVER=dummy
set VEDIT_AUDIO_CONSUMER=null
ctest -C Debug
```

On Windows, `test_rpc_server.cpp` is excluded from the build (its raw
AF_UNIX client is a deferred port); the remaining cases run.

The case list is generated by the build rather than kept here, because a
count written into a document is stale the next time a case lands:

```bash
cd build && ctest -N | tail -1
```

`tests/test_docs.cpp` is the check that keeps the method and tool tables
below honest: it compares them against what the code registers and fails on
either direction of the disagreement.

---

## RPC Protocol

vedit exposes a JSON-RPC 2.0 control plane over a Unix domain socket. The
path is `vedit::<pid>.sock` under the directory the platform uses for
per-user temporary files -- `$TMPDIR` on macOS, `/tmp` on Linux -- as
resolved by `vedit::default_socket_path()` in
`src/util/platform_compat.h`. Wire format: 4-byte big-endian length prefix
+ UTF-8 JSON. Max 64 MiB per frame.

### Available Methods

57 methods. This table is generated from the handlers in `src/rpc/rpc_methods.cpp`; `ctest` fails if it drifts.

| Method | Params | Description |
|---|---|---|
| `analysis.track` | `clip_id`, `rect`, `algo`, `steps` | Run OpenCV object tracking over a clip |
| `app.identity` | — | Identity of this vedit instance |
| `app.list_instances` | — | Enumerate running vedit instances |
| `clip.append` | `track_id`, `media_index`, `in`, `out` | Append a clip from the media library |
| `clip.change_speed` | `clip_id`, `speed` | Retime a clip (timewarp) |
| `clip.detach_audio` | `clip_id` | Copy a clip's audio onto a new audio track |
| `clip.get_crop` | `clip_id` | Read a clip's crop margins |
| `clip.get_geometry` | `clip_id` | Read a clip's position and size |
| `clip.get_rotation` | `clip_id` | Read a clip's rotation in degrees |
| `clip.insert` | `track_id`, `media_index`, `position`, `in`, `out` | Insert a clip at a timeline position |
| `clip.lift` | `clip_id` | Replace a clip with blank, keeping timing |
| `clip.move` | `clip_id`, `dest_track_id`, `position` | Move a clip, possibly across tracks |
| `clip.overwrite` | `track_id`, `media_index`, `position`, `in`, `out` | Insert a clip over blank space |
| `clip.remove` | `clip_id` | Remove a clip (closes the gap behind it) |
| `clip.set_audio_enabled` | `clip_id`, `enabled` | Enable or mute a clip's audio |
| `clip.set_crop` | `clip_id`, `left`, `right`, `top`, `bottom` | Crop a clip, in pixels |
| `clip.set_fade_in` | `clip_id`, `duration` | Fade a clip in over N frames |
| `clip.set_fade_out` | `clip_id`, `duration` | Fade a clip out over N frames |
| `clip.set_geometry` | `clip_id`, `x`, `y`, `w`, `h` | Position and size a clip, in percent |
| `clip.set_rotation` | `clip_id`, `degrees` | Rotate a clip, in degrees |
| `clip.set_video_enabled` | `clip_id`, `enabled` | Enable or hide a clip's video |
| `clip.split` | `clip_id`, `at_position` | Split a clip at a position |
| `clip.trim` | `clip_id`, `in`, `out` | Trim a clip's in/out points |
| `export.cancel` | `job_id` | Cancel a running export |
| `export.list_presets` | — | List MLT export presets |
| `export.load_preset` | `file` | Read a preset as consumer properties |
| `export.poll` | `job_id` | Poll an export job's state and progress |
| `export.start` | `path`, `params`, `in`, `out` | Start exporting to a file |
| `filter.add` | `filter`, `in`, `out`, `track_id`, `clip_id` | Attach a filter to a clip |
| `filter.get_param` | `filter`, `param`, `track_id`, `clip_id` | Read a filter parameter |
| `filter.list` | — | List available filters with parameters |
| `filter.remove` | `filter`, `track_id`, `clip_id` | Detach a filter from a clip |
| `filter.set_param` | `filter`, `param`, `track_id`, `keyframes`, `clip_id`, `value` | Set a filter parameter |
| `filter.set_range` | `filter`, `in`, `out`, `clip_id` | Limit a filter to a frame range |
| `library.import` | `path`, `video_index`, `audio_index` | Import a media file |
| `library.remove` | `index` | Drop an unused media item |
| `library.set_stream` | `index`, `video_index`, `audio_index` | Choose which streams a source uses |
| `player.get_frame` | `position`, `path` | Render one frame to a PNG |
| `player.pause` | — | Pause playback |
| `player.play` | — | Start playback |
| `player.seek` | `position` | Seek the playhead |
| `project.new` | — | Clear the project |
| `project.open` | `path` | Load a project from MLT XML |
| `project.save` | `path` | Save the project to MLT XML |
| `project.set_profile` | `width`, `height`, `fps` | Set canvas size and frame rate |
| `redo` | `expected_revision?` | Redo the last undone change |
| `state.get` | — | Project view: revision, profile, library, tracks, clips, transitions, attached filters |
| `track.add` | `kind`, `name` | Add a track |
| `track.get_composite_geometry` | `track_id` | Read a track's canvas placement |
| `track.remove` | `track_id` | Remove a track and its clips |
| `track.set_composite_geometry` | `track_id`, `x`, `y`, `w`, `h` | Place a track on the canvas |
| `track.set_hidden` | `track_id`, `hidden` | Show or hide a track's video |
| `track.set_muted` | `track_id`, `muted` | Mute or unmute a track |
| `transition.add` | `track_id`, `clip_index`, `length`, `type` | Add a transition between adjacent clips |
| `transition.list_types` | — | List available transition types |
| `undo` | `expected_revision?` | Undo the last change; refuses if the project moved past the revision given |
| `view.screenshot` | `path` | Render the frame under the playhead |

### Example (Python)

```python
import socket, struct, json, os, tempfile

req = json.dumps({"jsonrpc":"2.0","method":"state.get","id":1})
data = struct.pack('>I', len(req)) + req.encode()

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(os.path.join(tempfile.gettempdir(), f"vedit-{pid}.sock"))
s.sendall(data)
s.close()
```

### MCP Server

`tools/vedit-mcp/` provides a Model Context Protocol server that bridges
MCP clients (CodeBuddy Code, Claude Desktop) to vedit's RPC control
plane. It discovers running vedit instances by scanning the socket
directory named above, and reports each instance's own answer about what
it is when that answer is available.

```bash
cd tools/vedit-mcp
bash setup.sh          # creates .venv, installs deps, prints mcp.json snippet
```

---

## Project Structure

```
vedit/
  CMakeLists.txt          top-level build
  src/
    util/                 logger (vedit_util)
    core/                 MltRuntime, PreviewRenderer (vedit_core)
    domain/               AppController, ProjectModel, TimelineModel,
                          MediaLibrary, PlayerController,
                          ExportService, ProjectSaveLoad (vedit_domain)
    rpc/                  Dispatcher, RpcServer, method handlers (vedit_rpc)
    app/                  main.cpp: ImGui GUI panels (vedit exe)
  third_party/            ImGui SDL2+OpenGL3 backend compilation
  tools/
    vedit-mcp/            MCP server (Python stdio bridge) + client + auto-edit test
  tests/                  Catch2 test suite (case list: ctest -N)
```

---

## Design Decisions

All decisions are evidence-based — measured against the local MLT,
Shotcut, and idiff source trees. Full rationale in
[doc/next-phase-design.md](doc/next-phase-design.md), and the MLT capability audit in
[doc/mlt-capability-audit.md](doc/mlt-capability-audit.md).

| Decision | Choice | Rationale |
|---|---|---|
| Engine | Build ON MLT (not from scratch) | 80% of NLE engineering already exists and is hardened |
| GUI toolkit | Dear ImGui (SDL2 + OpenGL3) | Minimal, immediate-mode, same as idiff |
| Preview path | consumer-frame-show event → host GL texture | MLT owns A/V master clock; we own pixels |
| Serialization | MLT XML (consumer_xml) | Native, lossless, round-trip tested |
| Agent protocol | JSON-RPC 2.0 over UDS | Same pattern as idiff's control plane |
| Stable clip IDs | Stamped on cut producers via `_vedit_clip_id` | Survives MLT playlist reindexing |
| Build system | CMake + Ninja, layered static libs | Same pattern as idiff |

---

## Milestone 1 — What's Done

- **T1–T2**: Project skeleton, layered CMake, MLT system-package integration, empty ImGui window
- **T3**: Preview render pipeline (consumer-frame-show → GL texture, A/V master clock on MLT)
- **T4**: Domain model skeleton (MLT-backed ProjectModel/TimelineModel/MediaLibrary with stable ClipId mapping)
- **T5**: Multi-track edit operations (append, insert, remove, move cross-track, trim, split) — all via mlt++ Playlist/Multitrack
- **T6**: Cross-track composite transitions + PlayerController (play/pause/seek)
- **T7**: Minimal multi-track GUI (media library, timeline canvas, preview panel, transport bar)
- **T8**: AppController — single-interface seam for GUI and RPC
- **T9**: ExportService (consumer_avformat H.264/AAC/MP4 + single-frame PNG)
- **T10**: Project save/load (MLT XML round-trip + state JSON projection for agents)
- **T11**: JSON-RPC control plane (ported idiff dispatcher + server, 16 M1 methods)
- **T12**: M1 end-to-end verification (2-track timeline → save → reload → export → duration match)

### Known Limitations (M1)

- **Headless audio**: `sdl2_audio`/`rtaudio` crash on boxes without an audio device. Use `VEDIT_AUDIO_CONSUMER=null` for headless testing (consumer_null drives the frame pipeline without audio output).
- **MLT teardown quirk**: `consumer_avformat` and `rtaudio` consumers can SIGSEGV during destructor cleanup. This does not affect the application during normal use (process exit reclaims resources). Affects ctest but not the test binary itself.
- **No GPU rendering**: All rendering is CPU via MLT's software pipeline. GPU compositing (movit) is available in the MLT install but not yet wired.
- **Single audio consumer at a time**: No simultaneous audio monitoring + export.

---

## What's Next

See [doc/next-phase-design.md](doc/next-phase-design.md) for the full milestone
roadmap and for the G1-G5 attainment each item is measured against. The
remaining backlog:

- **Retimed audio on export**: a clip's speed change reaches the video, but
  the exported audio runs long against it, so the case is reported skipped.
- **One persistence loss**: per-track composite geometry does not survive a
  save and open. The geometry is in the document and is used while compositing,
  but a reopened project gives no handle to its composite transitions -- MLT 7
  exports no way to enumerate a tractor's transitions -- so the rect cannot be
  read or changed, and the next track change replants composites and resets it.
- **Two operations outside undo history**: `profile_set` and `clip_analyze_track`
  never reach the refresh that history is recorded from.
- **Caption drawing**: `vedit.text` renders through Core Text, so on other
  platforms the filter attaches and reports success while drawing nothing.
- **G5, two clients on one instance**: the goal describes the project as
  collaborative, and G5 states that as four checkable conditions. One
  client's mutation is already visible to the other's next read, and saving
  no longer publishes a half-written project or a project beside a sidecar
  from a different write. A client that names the revision it read is
  refused before stepping over a change it has not seen; one that names
  nothing still can. Nothing records who made a change, so no client can be
  told whose work an undo would touch.

---

## License

TBD
