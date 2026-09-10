# Geometry & Transition: MCP RPC vs Manual Feasibility

> **SUPERSEDED (2026-09-11).** The gaps listed here were implemented
> after this note was written: `clip.set_geometry`, `clip.set_rotation`,
> `clip.set_crop`, and transition type selection all exist in
> `TimelineModel` and are exposed over RPC. Do not use this file as a
> to-do list; it is kept only for its background on the MLT filter and
> transition mechanisms. Current direction and priority live in
> `doc/next-phase-design.md`.

## Summary

| Operation                | MCP RPC | Manual (GUI) | Gap                    |
| ------------------------ | ------- | ------------ | ---------------------- |
| Per-clip crop            | No      | No           | Need clip-level filter |
| Per-clip scale/stretch   | No      | No           | Need clip-level filter |
| Per-clip padding         | No      | No           | Need clip-level filter |
| Per-clip rotate/position | No      | No           | Need clip-level filter |
| Track-level composite    | Yes     | Yes          | --                     |
| Cross-fade transition    | Yes     | Yes          | type param is ignored  |
| Luma-wipe transition     | No      | No           | Need transition config |
| Dissolve/wave transition | No      | No           | Need transition config |

## What Exists Today

### Track-level composite geometry

RPC `track.set_composite_geometry` sets (x, y, w, h) as percentages on a video
track's composite transition. This positions and scales the **entire track** on
the canvas, not individual clips.

Implementation: a `Mlt::Transition("composite")` is planted between consecutive
video tracks, with geometry string `"X%/Y%:W%xH%"`.

This is the **only** geometry mechanism available today, and it works at the
track level.

### Cross-fade transition

RPC `transition.add` creates a cross-fade between adjacent clips on the same
track via `Mlt::Playlist::mix()`. The `type` parameter is accepted but
**ignored** -- the code always uses the default cross-fade created by `mix()`.

## What Is Missing

### 1. Per-Clip Geometry (crop, scale, stretch, padding, rotate, position)

**Status: not possible via RPC or GUI.**

The `ClipInfo` data model has no geometry fields. There is no per-clip geometry
transform anywhere in the timeline model or app controller.

**MLT capability exists.** MLT provides filters that could handle these
operations per-clip:

| Operation | MLT Filter | Parameters                                |
| --------- | ---------- | ----------------------------------------- |
| Crop      | `crop`     | left, right, top, bottom, center, use_profile |
| Transform | `affine`   | rect (X/Y:WxH[:opacity]), ox, oy, rotate_*, scale_*, halign, valign, fill, distort |
| Pad       | `avfilter.pad` | width, height, x, y, color           |
| Scale     | `avfilter.scale` | width, height, eval, flags         |

The `filter.add` RPC can already attach arbitrary MLT filters to clips. So the
**raw capability exists** -- a user (or RPC client) could manually attach the
`affine` or `crop` filter to a clip and set its parameters. However:

1. The `crop` filter is **hidden from `state.get`** by the attached_filters()
   blacklist in timeline_model.cpp:1188. A client that enumerates attached
   filters would not see it. It can still be attached and its params
   read/written via the explicit `filter.get_param`/`filter.set_param` RPCs,
   but it won't appear in the project state snapshot.
2. The `affine` filter has a complex `rect` parameter format
   (`"X/Y:WxH[:opacity]"`) that is not validated or explained by the RPC layer.
3. There is **no high-level RPC** like `clip.set_geometry` or
   `clip.set_transform`. The user must know the MLT filter name and parameter
   format.
4. The **GUI** has no geometry manipulation widgets for individual clips.

### 2. Rich Transitions (luma-wipe, dissolve, etc.)

**Status: cross-fade works; all other types are non-functional.**

The RPC `transition.add` accepts a `type` parameter but ignores it:

```cpp
// timeline_model.cpp:452-456
pl->mix(clip_index, length, nullptr);
// For simplicity, use the default cross-fade that mix() creates.
return true;
```

**MLT capability exists.** MLT provides these transition types:

| Transition  | Description                                   |
| ----------- | --------------------------------------------- |
| `mix`       | Simple cross-fade (this is what `mix()` uses) |
| `luma`      | Luminance-based wipe between clips            |
| `composite` | Alpha-composite overlay (already used for tracks) |

To implement: `mix()` creates a blank transition; the code must then replace it
with the requested type and set type-specific parameters (e.g., luma file,
softness, direction). The current implementation does not do this.

## Required Work

### For per-clip geometry (crop/scale/padding/rotate/position):

**A. Data model** -- add geometry fields to `ClipInfo` or a separate
`ClipTransform` struct (crop l/r/t/b, scale x/y, rotation, offset x/y, anchor
point).

**B. Domain layer** -- add `set_clip_geometry()` / `get_clip_geometry()` to
`TimelineModel` and `AppController`. Implement by attaching/detaching the
`affine` and `crop` MLT filters to individual clip producers.

**C. RPC layer** -- add `clip.set_geometry` and `clip.get_geometry` methods
with parameters: `clip_id`, `crop_l`, `crop_r`, `crop_t`, `crop_b`, `scale_x`,
`scale_y`, `rotate`, `offset_x`, `offset_y`.

**D. GUI** -- geometry panel per clip (sliders/draggers for crop, scale,
rotation, position).

### For rich transitions:

**A. Domain layer** -- replace `pl->mix(clip_index, length, nullptr)` with
proper transition construction: create the transition by type, set
type-specific properties (luma file, softness, direction), and connect it to
the two adjacent clips.

**B. RPC layer** -- extend `transition.add` to actually use the `type`
parameter. Add a `transition.list_types` method to enumerate available
transition types and their parameters (similar to `filter.list`).

**C. GUI** -- transition property panel per transition (type selector,
duration, type-specific params).

## Assessment

| Layer                | Effort   | Risk  |
| -------------------- | -------- | ----- |
| Data model (ClipInfo) | Low      | Low   |
| Domain (set_clip_geometry) | Medium   | Medium -- affine filter parameter format is complex |
| RPC (clip.set_geometry) | Low      | Low   |
| Domain (transitions) | Low      | Low   |
| GUI (geometry panel) | High     | Medium |
| GUI (transition panel) | Medium   | Low   |

The RPC **can** already do raw clip-level geometry today through
`filter.add` + `filter.set_param` with the `affine` filter -- but the UX is
poor (opaque MLT parameter names, complex `rect` format, no validation). A
proper high-level RPC and data model would make this usable.
