# Text Overlay: Design and Implementation Plan

vedit is SDL2 + Dear ImGui. No Qt dependency.

## Architecture

MLT's `text` filter (plus module) renders text via an internal
producer. On this system:
- libmltqt6.so is installed, so the `text` filter tries `qtext:`
  producer first. This requires QApplication — fails silently in vedit.
- libmltgdk.so is installed, so pango producer is the fallback.

The `text` filter (confirmed working via pixel measurement) uses
pango for rendering. Pango uses fontconfig for font discovery.

For the long term, vedit should not depend on MLT's text rendering
at all. The proper design is:

1. vedit owns the frame pipeline (it already does via the tractor)
2. A text overlay is a **custom MLT filter** written in vedit's own
   code that draws text using a lightweight library (stb_truetype,
   FreeType, or Core Text on macOS)
3. This eliminates the pango/gtk/qt dependency chain entirely

But as a first step, use what MLT already provides (pango fallback)
and fix the plumbing.

## What's Missing (in priority order)

### 1. Filter in/out (duration control)

MLT filters have `set_in_and_out(in, out)` — a first-class API.
When set, `mlt_filter_get_frame()` skips the filter outside [in,out].

**Problem**: vedit never calls `set_in_and_out()`. The filter applies
to the entire clip duration.

**Fix**:

```
attach_filter(clip_id, filter_name)
  -->  attach_filter(clip_id, filter_name, int in = -1, int out = -1)
```

If in/out >= 0, call `filter.set_in_and_out(in, out)` before
`mlt_service_attach()`. Also add a `set_filter_in_out()` method for
changing the range on an already-attached filter.

Files:
- `src/domain/timeline_model.h` — signature change + new method
- `src/domain/timeline_model.cpp` — implementation
- `src/domain/app_controller.h/.cpp` — pass through
- `src/rpc/rpc_methods.cpp` — `filter.add` accepts `in`/`out`;
  new `filter.set_range` method
- `tools/vedit-mcp/vedit_mcp_server.py` — `add_filter` accepts
  `in`/`out`

### 2. CJK font rendering

Pango uses fontconfig. On macOS, fontconfig may not index system
fonts (PingFang, STHeiti) unless configured.

**Verify**: `fc-list :lang=zh` — if empty, fontconfig needs config.

**Fix options** (pick one):
- a) Ship a fontconfig config that adds `/System/Library/Fonts`
- b) Set `family` to a font fontconfig already knows (check fc-list)
- c) Long-term: use Core Text (macOS native) or FreeType directly

### 3. Text filter is coupled to Qt/pango through MLT

The `text` filter in libmltplus.so creates a `qtext:` producer
internally. If Qt init fails, it falls back to `pango:` producer.
If pango isn't available either, no text renders.

This is fragile. The proper solution:

**Option A — Custom MLT filter in vedit (recommended)**:
- Write `filter_vedit_text.cpp` that renders text directly
- Use stb_truetype or FreeType + HarfBuzz for shaping
- Register as an MLT filter in the vedit process
- No external dependency beyond a .ttf font file
- Full control: font, size, color, position, animation, CJK

**Option B — Use avfilter.drawtext (FFmpeg's text filter)**:
- MLT wraps FFmpeg's drawtext via `avfilter.drawtext`
- Works without Qt or pango — uses libfreetype directly
- Already available in the installed MLT (libmltavformat.so)
- Parameters: `text`, `fontfile`, `fontsize`, `fontcolor`, `x`, `y`
- Simpler than Option A but less control over animation

### 4. No GUI for filter management

Current state: filters can only be added/configured via RPC/MCP.
The GUI has no panel for:
- Adding/removing filters on a clip
- Setting filter parameters
- Setting filter duration (in/out trim)
- Previewing filter effect

This is a larger UI feature. Defer to after the plumbing works.

## Implementation Order

### Step 1: filter.set_range (make duration work)

Add `set_in_and_out` support. This unblocks everything else — once
filters have duration control, any rendering approach (pango, custom,
avfilter) works with timed text.

### Step 2: Verify `text` filter with pango fallback

With duration working, test:
```
filter.add {clip_id:1, filter:"text", in:0, out:47}
filter.set_param {clip_id:1, filter:"text", param:"argument", value:"Test"}
```
If CJK doesn't render, check fontconfig. If pango itself isn't
working, move to Step 3.

### Step 3: avfilter.drawtext as backup

If pango fails, use FFmpeg's drawtext filter which only needs
freetype (always available with FFmpeg):
```
filter.add {clip_id:1, filter:"avfilter.drawtext", in:0, out:47}
filter.set_param {clip_id:1, filter:"avfilter.drawtext",
    param:"av.text", value:"Hello"}
filter.set_param {clip_id:1, filter:"avfilter.drawtext",
    param:"av.fontsize", value:"72"}
filter.set_param {clip_id:1, filter:"avfilter.drawtext",
    param:"av.fontcolor", value:"white"}
filter.set_param {clip_id:1, filter:"avfilter.drawtext",
    param:"av.x", value:"(w-text_w)/2"}
filter.set_param {clip_id:1, filter:"avfilter.drawtext",
    param:"av.y", value:"50"}
```

This works without Qt, without pango, without fontconfig. Just
freetype + a font file path.

### Step 4 (future): Custom text filter

Write vedit's own text rendering using stb_truetype or FreeType.
Register as MLT filter. Full control, zero external dependency.

## Summary

| Gap | Impact | Fix | Effort |
|-----|--------|-----|--------|
| No filter in/out | Can't time any filter | Add set_in_and_out to attach path | Small |
| CJK fonts | Chinese text = boxes | Check fontconfig; or use fontfile | Small |
| Qt dependency in text filter | qtext silently fails | Use pango fallback or avfilter.drawtext | None (already falls back) |
| No filter GUI | MCP/RPC only | Add imgui panel | Medium |
| Coupled to MLT text stack | Fragile, hard to debug | Custom filter or avfilter.drawtext | Medium |
