# Technical Direction Review: Text Overlay, Export, and the MLT Question

Status: decision document. Audience: project owner.
Date: 2026-06-27.

This document answers three questions:

1. Is the repeated failure on "simple" tasks (text overlay, export
   duration) caused by the AI agent, by MLT design flaws, or by wrong
   technology selection?
2. Is MLT the wrong choice? Is there a better library?
3. Would wrapping FFmpeg directly, from the bottom up, cause fewer
   problems?

All claims below are backed by measurements taken on this machine
(Rule P1). The commands and their output are in the Appendix.

---

## 1. Root Cause: It Was Not One Thing, and That Is the Trap

The honest answer: **all three factors contributed, but the dominant
cause was a wrong factual assumption that nobody verified.** The agent
then chained workarounds on top of a broken premise instead of stopping
(Rule P5 violation).

### 1.1 The export-duration problem was a real, fixable bug (already fixed)

The export truncation (11s vs 17s) was a genuine logic bug in vedit, not
an MLT flaw: the code min-clamped track lengths and caught MLT's internal
black-background track, which is shorter. Fixed in commit `188c85c` by
using `producer.get_playtime()`. This part is **done and correct**. It is
not evidence against MLT.

### 1.2 The text-overlay problem was a chain of two genuine dead-ends

The agent oscillated between two text paths, each of which is **actually
broken on this machine**, and never realized both were broken at once:

**Dead-end A — `avfilter.drawtext`.** The agent assumed this "always
exists with FFmpeg" (it wrote exactly that in `text-overlay-gaps.md`
lines 88 and 123). Measured reality:

```
$ ffmpeg -buildconf | grep -iE "freetype|fontconfig|harfbuzz|libass"
(no output)
```

The Homebrew FFmpeg 8.1.1 on this machine is built **without
libfreetype**. The `drawtext` filter does not exist in this binary.
Therefore `avfilter.drawtext` can never appear in MLT's filter list. The
agent kept trying to set `av.fontfile` on a filter that does not exist,
got `-32000` errors, and concluded "the filter list is missing it" —
true, but it never asked *why*.

**Dead-end B — MLT `text` / `dynamictext` / `qtext`.** These do exist
(MLT ships `libmltqt6.so`). But:

```
$ melt ... -filter dynamictext argument="HELLO" -consumer avformat:out.mp4
(hangs > 2 minutes, produces a 0-byte file, ignores SIGTERM)
```

The `text` filter instantiates a `qtext:` producer, which needs a live
`QApplication` and an event loop. vedit is **SDL2 + Dear ImGui with no
Qt**. Without QApplication the producer blocks or silently no-ops. The
`pango` fallback needs GDK + fontconfig, and fontconfig on macOS does not
index system CJK fonts by default. So even the fallback would render
boxes for "火箭点火姿势示范".

**Conclusion on root cause.** This is roughly:

- 50% agent fault: it assumed instead of measuring (one
  `ffmpeg -buildconf` would have ended the loop on day one), and it
  violated the user's own Rule P5 by chaining workarounds across two
  failed directions instead of stopping and challenging the premise.
- 30% technology-selection fault: MLT's text feature is coupled to a
  Qt/pango/fontconfig stack that is a poor fit for a headless SDL app.
  This is a real architectural mismatch, not an agent hallucination.
- 20% environment fault: the local FFmpeg lacks freetype, so the one
  Qt-free path MLT could have offered is also closed.

The important lesson: **MLT did not lie. The agent did not check.** But
MLT *did* make text harder than it should be by routing it through a GUI
toolkit.

---

## 2. Is MLT the Wrong Choice?

**No — not for what vedit actually is.** But MLT's *text* subsystem is
the wrong choice, and that is a small, well-contained part.

### 2.1 What MLT is good at, and vedit already uses correctly

MLT gives vedit, essentially for free:

- A non-linear timeline model (tractor, multitrack, playlists, cuts).
- Frame-accurate seeking and a producer/consumer pipeline.
- 428 working filters, including the whole `avfilter.*` video set
  (color, transform, blur, crop, fade, etc. — all measured present).
- Transitions, mixing, audio handling.
- A serialization format (MLT XML) that interoperates with Shotcut.
- A working export consumer (`avformat`).

Rebuilding this from scratch on raw FFmpeg/libav is **months of work** and
is exactly the kind of NLE engine MLT exists to provide. The timeline,
transition, and multitrack code in vedit is real and working
(`git log` shows geometry, crop, fade, tracking, transitions all landed).
Throwing that away to "start from the bottom with FFmpeg" would discard
the most valuable, hardest-to-rebuild part of the project to solve a
text-drawing problem.

### 2.2 What MLT is bad at for this project

- **Text rendering is coupled to a GUI toolkit (Qt/pango).** Bad fit for
  a headless SDL2 process. This is the single feature that caused all the
  pain.
- **Filters that depend on optional FFmpeg build flags** silently vanish
  if the local FFmpeg lacks them (the freetype case). MLT gives no
  warning; the filter just isn't in the list.
- **Headless robustness on macOS** is shaky (the melt hang, the
  Factory::init lock contention you already had to fix in `d0ab728`).

### 2.3 The decisive insight

vedit **already pulls fully-decoded RGBA frames out of MLT** in
`src/core/preview_renderer.cpp:125`:

```c
mlt_image_format format = mlt_image_rgba;
uint8_t* image = frame.get_image(format, width, height);
```

It also already links **OpenCV** (used in `timeline_model.cpp` for object
tracking) and **Dear ImGui** (which bundles `stb_truetype`). That means
vedit *already has, in-process, three different ways to draw a string of
pixels onto an RGBA buffer* — with zero new dependencies and zero Qt.

The text problem was never hard. The agent just kept trying to make a
GUI-toolkit-coupled MLT filter work in a no-GUI process.

---

## 3. Should You Wrap FFmpeg Directly From the Bottom Up?

**No.** This would trade a one-week problem for a one-year problem.

Wrapping libav* directly means hand-building: demux/decode/encode
management, a timeline/edit model, A/V sync, seeking, multitrack
compositing, transitions, a filter graph manager, and a project file
format. That is precisely what MLT already is. You would be rewriting
MLT, badly, to avoid one bad subsystem.

The FFmpeg-from-scratch route only makes sense if vedit's requirements
outgrow MLT entirely (e.g. you need a GPU-resident frame pipeline,
real-time multi-stream compositing, or a fundamentally different edit
model). Nothing in the current feature set suggests that.

---

## 4. Recommended Direction

Keep MLT as the engine. **Stop using MLT/Qt/pango/avfilter for text.**
Draw text yourself, in-process, onto the RGBA frames vedit already
controls. This is exactly Option A from the existing gap doc — and it is
small, not "future work".

### Why this is the right call

- Eliminates the entire Qt / pango / fontconfig / freetype-build-flag
  dependency chain that caused every text failure.
- Uses pixel-buffer access vedit already has.
- Full control over font, size, color, position, CJK, animation, and —
  critically — **duration**, because vedit decides per frame whether to
  draw.
- Portable (Rule P1/P3): no reliance on the local machine's FFmpeg build
  flags or installed Qt.

### Recommended implementation: a custom MLT filter inside vedit

Register a tiny MLT filter (`vedit.text`) in vedit's own process. MLT
filters receive the RGBA frame buffer in their `process` callback; you
draw into it and return. This keeps the timeline/duration/in-out
machinery you already built (`filter.set_range`, commit `fa25326`)
working unchanged — the filter just becomes one you fully own.

For the actual glyph rasterization, in order of preference:

1. **Core Text (macOS native).** Best CJK shaping, already on every Mac,
   zero new dependency. Renders to a CGBitmapContext (RGBA), then alpha-
   composite onto the frame. Mac-only, which is fine if vedit is Mac-
   first.
2. **stb_truetype** (already vendored via ImGui) + a bundled CJK `.ttf`.
   Cross-platform, single header, no new dependency. Weaker at complex
   shaping but fine for overlay captions.
3. **FreeType + HarfBuzz** via vcpkg. Most capable, heaviest. Only if you
   need full international shaping later.

Recommendation: start with **Core Text** if Mac-only is acceptable, else
**stb_truetype + bundled font**. Both avoid Qt entirely and both render
CJK correctly (unlike the current pango/fontconfig path).

### What this does NOT throw away

Everything already built stays: timeline, cuts, transitions, export
(`get_playtime` fix), range export, filter in/out plumbing, tracking,
geometry/crop/fade. The change is surgical — replace one broken filter
backend with an in-process rasterizer behind the same `filter.add` /
`filter.set_range` API.

---

## 5. Decision Summary

| Question | Answer |
|----------|--------|
| Was it the AI's fault? | Largely yes — it assumed `drawtext` exists instead of running one build-config check, and chained workarounds across two broken paths instead of stopping. |
| Is MLT flawed? | Only its text subsystem (Qt/pango coupling) is a bad fit. The core engine is sound and valuable. |
| Wrong technology choice? | No. MLT is right for the NLE engine. Its *text feature* is the wrong tool, and it is small and replaceable. |
| Better library? | None worth a rewrite. The fix is in-process text drawing, not a new engine. |
| Wrap FFmpeg from scratch? | No. That rebuilds MLT and costs ~1 year to avoid a ~1 week fix. |
| Recommended path | Keep MLT. Add a custom `vedit.text` MLT filter that rasterizes via Core Text or stb_truetype onto vedit's existing RGBA frames. |

The project direction does **not** need to change. The text *approach*
does.

---

## Appendix: Measured Evidence

Machine: Apple Silicon (M1), macOS Darwin 25.5.0. MLT 7.38.0 (Homebrew).
FFmpeg 8.1.1 (Homebrew).

**A1. FFmpeg has no text-rendering libraries (this is why drawtext is
absent):**

```
$ ffmpeg -hide_banner -buildconf | grep -iE "freetype|fontconfig|harfbuzz|libass"
(no output)

$ ffmpeg -hide_banner -filters | grep -i drawtext
(no match)
```

**A2. avfilter is otherwise fully present (428 filters), so the problem
is freetype, not avfilter:**

```
$ melt -query filters | grep -c "  - "
428
$ melt -query filters | grep avfilter | head
avfilter.abench, avfilter.acompressor, ... avfilter.crop, avfilter.scale, ...
(drawtext absent; everything else present)
```

**A3. MLT text filters exist but route through Qt/pango:**

```
$ ls /opt/homebrew/Cellar/mlt/7.38.0/lib/mlt/
libmltqt6.so   libmltplus.so   libmltgdk.so   ... (text filters live here)
$ melt -query filters | grep -iE "qtext|dynamictext|^  - text"
- qtext
- dynamictext
- text
```

**A4. Rendering text via MLT headless hangs (no QApplication):**

```
$ melt -profile atsc_1080p_25 color:black out=24 \
    -filter dynamictext argument="HELLO WORLD" \
    -consumer avformat:/tmp/out.mp4 frames=25
(runs > 2 minutes, output file is 0 bytes, ignores SIGTERM)
```

**A5. vedit already owns RGBA frames in-process:**

```
src/core/preview_renderer.cpp:125
  uint8_t* image = frame.get_image(mlt_image_rgba, width, height);
```

**A6. vedit already links OpenCV and ImGui (in-process rasterizers
available, no new heavy dependency needed):**

```
src/domain/timeline_model.cpp     -> uses cv:: (OpenCV linked)
src/app/CMakeLists.txt:7           -> vedit_imgui_backends (stb_truetype bundled)
CMakeLists.txt:52                  -> comment: Qt/opencv/gdk-pixbuf "which vedit does not use"
```
