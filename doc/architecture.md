# Architecture

vedit is a single-state, dual-channel non-linear editor. One domain
model, two equal control channels:

```
                     ┌──────────────────┐
                     │   AppController  │  ← single seam
                     └────────┬─────────┘
               ┌──────────────┴──────────────┐
               ▼                             ▼
     ┌──────────────────┐          ┌──────────────────┐
     │    ImGui GUI      │          │   JSON-RPC Agent │
     │  (SDL2 + OpenGL)  │          │ (UDS socket)     │
     └──────────────────┘          └──────────────────┘
```

No code path is privileged. The GUI calls `AppController` methods in
response to button clicks; the RPC server calls the same methods in
response to JSON-RPC requests. The domain layer does not know which
channel a call came from.

## Layer Stack

```
vedit (exe)          ImGui panels, SDL2 window, RPC server lifecycle
    │
vedit_rpc            JSON-RPC 2.0 dispatcher + UDS server
    │
vedit_domain         AppController, ProjectModel, TimelineModel,
                     MediaLibrary, PlayerController, ExportService,
                     ProjectSaveLoad
    │
vedit_core           MltRuntime, PreviewRenderer (MLT lifecycle +
                     consumer-frame-show → GL texture pipeline)
    │
vedit_util           Logger
    │
mlt++                MLT C++ wrapper (Tractor, Multitrack, Playlist,
                     Consumer, Producer, Transition, Filter, ...)
    │
libmlt               MLT C framework
    │
FFmpeg               libavformat, libavcodec, libswscale, ...
```

### Compile-Time Layer Separation

Each layer is a static library. Include paths are scoped so a layer
can only see what's above it (its dependencies) and itself:

| Layer | Can include | Must NOT include |
|---|---|---|
| `vedit_util` | std headers | MLT, ImGui, domain |
| `vedit_core` | util, mlt++ | GUI headers (SDL, ImGui), domain |
| `vedit_domain` | core, mlt++ | GUI headers |
| `vedit_rpc` | domain, nlohmann-json | GUI headers |
| `vedit` (exe) | all layers | — |

## Thread Model

```
Main thread (GUI)                MLT consumer thread          RPC I/O thread
─────────────────                ────────────────────         ──────────────
ImGui frame loop                 consumer-frame-show          accept() / recv()
    │                                │                            │
    ├─ pull_latest_frame() ←──── mutex ←── handle_frame()        │
    │   (GL texture upload)          │   (frame decode)           │
    │                                │                            │
    ├─ rpc_server.drain() ←─── mutex ←────────────────────────────┤
    │   (dispatch to handlers)       │                            │
    │                                │                            │
    └─ ImGui::Render()               │                            │
```

- **MLT consumer thread**: owned by `PreviewRenderer`. Decodes frames,
  hands them to the main thread via a mutex-protected drop-on-overflow
  buffer. Never touches GL or ImGui.

- **RPC I/O thread**: owned by `RpcServer`. Accepts connections, reads
  length-prefixed JSON frames, pushes them into a mutex-protected queue.
  Never touches AppController, MLT, or GL.

- **Main thread**: the only thread that runs domain logic (handlers),
  uploads GL textures, and renders ImGui. Calls `rpc_server.drain()`
  between frames to process pending requests.

## Stable Clip IDs

MLT playlists reindex on every mutation (insert, remove, split, move).
A playlist index is not a stable handle. vedit stamps a monotonic
`_vedit_clip_id` integer on each cut producer's properties. After every
playlist mutation, `TimelineModel::rebuild_clip_view()` walks all
playlists, reads the stamps back, and rebuilds the `ClipId → ClipInfo`
mapping. External code (GUI, RPC, agent) only ever sees `ClipId` values
— never MLT-internal indices.

The `_` prefix on the property name prevents MLT from serializing it
to XML (underscore-prefixed properties are internal).

## Data Flow: Preview

```
TimelineModel::producer() → Mlt::Tractor
    │
    ▼
PlayerController::play()
    │
    ▼
PreviewRenderer::start()
    │  Consumer(profile, "sdl2_audio")  ← A/V master clock
    │  consumer.connect(tractor)
    │  consumer.listen("consumer-frame-show", on_frame_show)
    ▼
MLT consumer thread: on_frame_show()
    │  Mlt::EventData(data).to_frame()
    │  frame.get_image(mlt_image_rgba, w, h)
    │  mutex → pending_buffer_
    ▼
Main thread: pull_latest_frame()
    │  glTexSubImage2D(preview_texture, ...)
    ▼
ImGui::Image(preview_texture)
```

## Data Flow: Export

```
TimelineModel::producer() → same Mlt::Tractor
    │
    ▼
ExportService::render()
    │  Consumer(profile, "avformat", output_path)
    │  consumer.set("real_time", -1)  ← render as fast as possible
    │  consumer.set("vcodec", "libx264")
    │  consumer.set("acodec", "aac")
    │  consumer.connect(tractor)
    │  consumer.run()  ← synchronous, blocks until done
    ▼
output.mp4
```

The same tractor feeds both preview (realtime consumer) and export
(offline consumer). Preview ↔ Export composition consistency is
guaranteed by MLT.

## Data Flow: Save/Load

```
Save:  ProjectModel → consumer_xml → MLT XML file
Load:  MLT XML file → producer_xml → walk tracks → rebuild TimelineModel
Agent: state_json() → JSON projection (agents never parse XML)
```

## RPC Wire Protocol

```
┌────────────────┬──────────────────────────────┐
│  4 bytes (BE)  │  UTF-8 JSON (up to 64 MiB)   │
│  frame length  │  JSON-RPC 2.0 request        │
└────────────────┴──────────────────────────────┘
```

Socket path: `/tmp/vedit-<pid>.sock`. The socket is created on startup
and cleaned up on shutdown. Stale sockets from crashed sessions are
swept by the next start.

See [../README.md](../README.md#rpc-protocol) for the full method list.

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Build ON MLT, not from scratch | The hardest 80% of an NLE (decode, seek, cache, encode, playlist logic, transitions, filters) already exists and is hardened in three shipping editors |
| Single AppController seam | Both channels are equal; no "GUI is the real thing, RPC is an afterthought" |
| Stable clip IDs stamped on cuts | Survives MLT playlist reindexing; no UUID bloat |
| MLT XML for serialization | Native, lossless, round-trip tested. JSON projection for agents (they never parse XML) |
| consumer-frame-show for preview | MLT owns the A/V master clock; we own the pixels. Same pattern as Shotcut's videowidget |
| ImGui for GUI | Immediate-mode, minimal, same toolkit as idiff |
| Layered CMake with include scoping | Catches layer violations at compile time, not code review |
