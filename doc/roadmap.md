# Roadmap

## Completed

### Milestone 1 — Agent-Native Multi-Track Editor

**Status: Complete** (2024-06-24)

Multi-track editing with composition, export, save/load, and a
JSON-RPC control plane for external agents.

- [x] MLT engine integration (system package, no source build)
- [x] Layered CMake project with compile-time layer separation
- [x] Preview render pipeline (consumer-frame-show → GL texture)
- [x] Domain model with stable clip-id mapping
- [x] Multi-track edit operations (append, insert, remove, move, trim, split)
- [x] Cross-track composite transitions
- [x] PlayerController (play, pause, seek, step)
- [x] Minimal ImGui GUI (media library, timeline, preview, transport)
- [x] AppController single-interface seam
- [x] ExportService (H.264/AAC/MP4 + single-frame PNG)
- [x] Project save/load (MLT XML + state JSON projection)
- [x] JSON-RPC 2.0 control plane over Unix domain socket
- [x] 33 Catch2 tests, 198 assertions

See [milestone-1.md](milestone-1.md) for the full task breakdown.

---

## Planned

### Milestone 2 — Filters

Add per-clip and per-track video/audio filters via MLT's filter
framework. Use MLT's yml metadata to auto-discover available filters
and their parameter schemas (P1: yml-driven generic parameter surface).

- [ ] Filter discovery from MLT module yml
- [ ] Per-clip filter attachment (plant_filter on playlist cuts)
- [ ] Per-track filter attachment (plant_filter on tractor tracks)
- [ ] Filter parameter get/set via the stable clip-id system
- [ ] Filter GUI panel (list, add, configure)
- [ ] Filter RPC methods (filter.list, filter.add, filter.set)
- [ ] Test suite for filter operations

### Milestone 3 — Keyframes

Support animated parameters via MLT's animation API. Allow parameters
to carry `{value}` (scalar) or `{keyframes}` (time-varying) shapes (P2).

- [ ] Keyframe data model (time-value pairs, interpolation)
- [ ] MLT animation property integration (anim_set/anim_get)
- [ ] Keyframe editor GUI widget
- [ ] Keyframe RPC methods (parameter.set with keyframe payload)
- [ ] Test suite for keyframe round-trip

### Milestone 4 — GPU Rendering

Enable GPU-accelerated compositing and effects via MLT's movit module.

- [ ] movit module validation (already present in apt MLT)
- [ ] GPU consumer path (consumer_movit or sdl2 GPU path)
- [ ] Performance comparison (CPU vs GPU for common operations)
- [ ] Graceful fallback when GPU is unavailable

### Milestone 5 — Agent Workflow Integration

Demonstrate the agent-native architecture in real scenarios.

- [ ] idiff integration (vedit as an idiff comparison target)
- [ ] Batch processing workflow (agent script builds timeline, exports)
- [ ] Multi-instance collaboration (two vedit instances share state)
- [ ] Performance benchmarks for agent-driven operations

---

## Non-Goals (Explicitly Out of Scope)

These are real NLE features that we deliberately do not build:

- **Audio mixing console** — MLT has audio mixing; we reuse it but
  don't build a dedicated mixing UI.
- **Color grading wheels/scopes** — Real NLE features; not relevant to
  the agent-native control plane goal.
- **Title/animation tool** — kdenlive-style title editor is a massive
  feature; we use MLT's text filter instead.
- **Multi-camera editing** — Not on the roadmap.
- **Proxy/offline editing** — MLT supports it; we don't build a UI for
  proxy workflow management.
- **Timeline nesting** — MLT supports nested sequences; we don't expose
  nesting operations in M1–M5.
- **Hardware-accelerated encoding** — NVENC/QSV/VAAPI encoding is
  available through MLT's consumer_avformat; we don't build encoder
  selection UI beyond the presets.
