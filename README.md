# vedit — Agent-Native Collaborative NLE

**vedit** is an experimental non-linear video editor built ON
[MLT](https://www.mltframework.org/) (the engine behind Shotcut,
Kdenlive, Flowblade). Its defining architectural choice is a
**single-state, dual-channel control plane**: a local GUI and a remote
agent (JSON-RPC over Unix domain socket) drive the same domain model
through the same `AppController` seam. Neither channel has a privileged
code path.

> Status: **Milestone 1 complete** (multi-track editing + composition +
> export + save/load + RPC control plane). See [docs/](docs/) for the
> full design rationale, scope boundary, and task breakdown.

---

## Architecture

```
┌─────────────┐     ┌─────────────────────┐     ┌──────────────┐
│  ImGui GUI   │     │   JSON-RPC Agent    │     │  FFmpeg CLI  │
│ (SDL2+GL)   │     │ (UDS /tmp/vedit-*.sock) │   │  (export)    │
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

### Build

```bash
cd vedit
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Run (desktop)

```bash
./build/src/app/vedit
```

### Run (headless smoke test)

```bash
SDL_VIDEODRIVER=offscreen VEDIT_AUDIO_CONSUMER=null ./build/src/app/vedit
```

### Run tests

```bash
cd build
VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ctest
# or directly:
VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ./tests/vedit_tests

# Run a specific tag:
./tests/vedit_tests "[t5]"
./tests/vedit_tests "[export]"
```

48 test cases across 6 files (smoke, domain, app_controller, rpc, rpc_server).

---

## RPC Protocol

vedit exposes a JSON-RPC 2.0 control plane over a Unix domain socket
(`/tmp/vedit-<pid>.sock`). Wire format: 4-byte big-endian length prefix
+ UTF-8 JSON. Max 64 MiB per frame.

### Available Methods

| Method | Params | Returns | Description |
|---|---|---|---|
| `state.get` | — | `{"tracks": [...], "clips": [...]}` | Full timeline JSON projection |
| `library.import` | `{"path": "..."}` | `{"index": N}` | Import a media file |
| `track.add` | `{"kind": "video"|"audio", "name": "..."}` | `{"track_id": N}` | Add a track |
| `track.remove` | `{"track_id": N}` | `{"ok": true}` | Remove a track |
| `clip.append` | `{"track_id": N, "media_index": N, "in": N?, "out": N?}` | `{"clip_id": N}` | Append a clip (optional in/out source range) |
| `clip.insert` | `{"track_id": N, "media_index": N, "position": N}` | `{"clip_id": N}` | Insert at position |
| `clip.remove` | `{"clip_id": N}` | `{"ok": true}` | Remove a clip |
| `clip.move` | `{"clip_id": N, "dest_track_id": N, "position": N}` | `{"ok": true}` | Move clip (cross-track) |
| `clip.trim` | `{"clip_id": N, "in": N, "out": N}` | `{"ok": true}` | Trim in/out points |
| `clip.split` | `{"clip_id": N, "at_position": N}` | `{"clip_id": N}` | Split at position (returns new right-half id) |
| `player.play` | — | `{"ok": true}` | Start playback |
| `player.pause` | — | `{"ok": true}` | Pause playback |
| `player.seek` | `{"position": N}` | `{"position": N}` | Seek to frame |
| `project.save` | `{"path": "..."}` | `{"ok": true}` | Save to MLT XML |
| `project.open` | `{"path": "..."}` | `{"ok": true}` | Load from MLT XML |
| `export.start` | `{"path": "..."}` | `{"ok": true}` | Export to H.264/AAC MP4 |

### Example (Python)

```python
import socket, struct, json

req = json.dumps({"jsonrpc":"2.0","method":"state.get","id":1})
data = struct.pack('>I', len(req)) + req.encode()

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(f"/tmp/vedit-{pid}.sock")
s.sendall(data)
s.close()
```

### MCP Server

`tools/vedit-mcp/` provides a Model Context Protocol server that bridges
MCP clients (CodeBuddy Code, Claude Desktop) to vedit's RPC control
plane. It auto-discovers running vedit instances via `/tmp/vedit-*.sock`.

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
  tests/                  Catch2 test suite (48 cases in 6 files)
```

---

## Design Decisions

All decisions are evidence-based — measured against the local MLT,
Shotcut, and idiff source trees. Full rationale in
[docs/phase1-design.md](docs/phase1-design.md).

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

See [docs/phase1-scope.md](docs/phase1-scope.md) for the full milestone
roadmap. Key future work:

- **M2**: Filters (per-clip/per-track video/audio filters via MLT's
  filter framework + yml-driven parameter discovery)
- **M3**: Keyframes (parameter animation via MLT's animation API)
- **M4**: GPU rendering pipeline (movit integration)
- **M5**: Agent workflow scenarios (idiff integration, batch processing)

---

## License

TBD
