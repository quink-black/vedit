# Milestone 1 — Task Breakdown

> Completed: 2024-06-24. 12 tasks, 11 commits, 33 tests, 198 assertions.

## Task List

| # | Task | Key Deliverable | Verification |
|---|---|---|---|
| T1 | Build & verify system MLT | Apt-installed MLT 7.22 has all needed modules | `pkg-config --exists mlt++-7`; smoke compile |
| T2 | Project skeleton + layered CMake | Empty ImGui window, MLT lifecycle | Build green; `SDL_VIDEODRIVER=offscreen` smoke run |
| T3 | Preview render pipeline | consumer-frame-show → GL texture | 100+ frames flow, clean stop at clip end |
| T4 | Domain model skeleton | ProjectModel/TimelineModel/MediaLibrary with stable ClipId | 9 tests: track lifecycle, clip-id survives rebuild |
| T5 | Multi-track edit operations | append/insert/remove/move(×track)/trim/split via mlt++ | 4 tests: position insert, same/cross-track move, trim |
| T6 | Cross-track composition + PlayerController | composite transition between video tracks; play/pause/seek | 3 tests: composed frame, player transport |
| T7 | Minimal multi-track GUI | ImGui: media library, timeline canvas, preview, transport | GUI launches, all actions through AppController |
| T8 | AppController | Single seam: GUI and RPC call the same methods | 5 tests: import, build timeline, transport, edit ops |
| T9 | ExportService | Same tractor → consumer_avformat → H.264/AAC/MP4; single-frame PNG | 2 tests: ffprobe-verified export, valid PNG |
| T10 | Project save/load | MLT XML round-trip; state JSON projection for agents | 2 tests: 2-track round-trip, JSON completeness |
| T11 | RPC control plane | JSON-RPC 2.0 over UDS; 16 M1 method handlers | 7 tests: dispatcher protocol; e2e socket smoke |
| T12 | M1 end-to-end | 2-track timeline → save → reload → export → duration match | 1 test: full pipeline, all assertions green |

## Design Decisions Made During M1

| Decision | Triggered By | Resolution |
|---|---|---|
| Use system MLT, don't build from source | T1: apt `libmlt++-dev` 7.22 has all 9 needed modules | Zero source-build; pkg-config integration |
| ImGui backends from apt doc/examples | T2: apt `libimgui-dev` ships backend `.cpp` under `/usr/share/doc/` | No FetchContent; all deps from apt |
| sdl2_audio default, null for headless | T3: rtaudio/sdl2_audio crash on boxes with no audio device | `VEDIT_AUDIO_CONSUMER=null` env for headless |
| Stable ClipId via stamped cut properties | T4: MLT playlist reindexing breaks direct index references | `_vedit_clip_id` stamped on cuts; rebuild_clip_view after mutations |
| Insert pads with blank, doesn't auto-split | T5: `mlt_playlist_insert_at` doesn't split existing clips | Documented contract; split-then-insert for that case |
| PlayerController separate from ProjectModel | T6: Player owns a PreviewRenderer; ProjectModel stays lean | T8 AppController wires them together |
| MLT XML for serialization, JSON for agents | T10: consumer_xml is native and lossless; agents need readable state | Two outputs: XML file (save/load), JSON projection (state.get) |
| Ported idiff dispatcher verbatim | T11: idiff's RPC layer is proven, pure, and thin (~240 lines) | Renamed namespace, identical logic |

## Test Coverage by Layer

| Layer | Tests | Assertions | Coverage |
|---|---|---|---|
| core (MltRuntime, PreviewRenderer) | 2 | 4 | Lifecycle, smoke |
| domain (ProjectModel, TimelineModel, MediaLibrary) | 9 | 50 | Track/clip ops, stable ids |
| domain (AppController, PlayerController) | 10 | 70 | Full seam: import → edit → transport |
| domain (ExportService, ProjectSaveLoad) | 4 | 25 | Export, save/load round-trip |
| rpc (Dispatcher, method handlers) | 7 | 37 | Protocol correctness |
| e2e (M1 gate) | 1 | 12 | Full pipeline: build → save → reload → export |
| **Total** | **33** | **198** | |
