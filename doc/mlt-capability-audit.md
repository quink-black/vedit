# MLT Capability Audit: Are the Blamed Limitations Real?

Date: 2026-09-11
Question: the AI repeatedly got stuck on "capabilities MLT supposedly does
not have" and worked around them with hacks. Are those limitations real,
and do they justify changing the technology choice?

## Measurement Environment

| Item | Value |
|---|---|
| Machine | Apple Silicon arm64, macOS Darwin 25.6.0 |
| MLT | 7.40.0 (Homebrew `mlt-7.40.0_1`) |
| FFmpeg | 8.1.2 (Homebrew) |
| Method | standalone mlt++ probe programs in `/tmp/mlt-probe`, plus `melt` CLI and `ffprobe` |

Every claim below was tested against this build. Nothing is inferred from
existing comments or documents.

---

## Verdict Summary

| # | Claim (as written in vedit) | Verdict | Consequence |
|---|---|---|---|
| 1 | Retime is not really available; `change_clip_speed` builds a `timewarp` producer and discards it | **FALSE** | `timewarp` is a first-class MLT producer and works. This is a plain bug, not a limitation. |
| 2 | `mlt_properties_anim_set` cannot be called twice on one property (upstream MLT bug: second call segfaults) | **FALSE** | Survives 3 sequential calls, plus duplicate positions, length, and interleave with `key_set_type`. |
| 3 | Stream switching requires reopening the producer because MLT silently resets `audio_index` | **TRUE, but not a capability gap** | Setting the index before the first decode works and sticks. This is normal MLT usage, not a missing feature. |
| 4 | MLT text needs Qt/pango; no headless path; `drawtext` unavailable | **TRUE** | This is the one real gap. Contained, and fixable in vedit. |
| 5 | (Implicit in the lossy loader) MLT XML cannot round-trip effects | **FALSE** | Filters and transitions survive save/load pixel-for-pixel. The loss is vedit's own loader design. |

**Answer to question 1: MLT is not missing the required basic
capabilities, with one exception -- text rendering.**

**Answer to question 2: no major technology change is justified.** See
section 5.

---

## 1. Retime: `timewarp` Exists and Works

Claim source: `timeline_model.cpp:443-484`. The function builds
`Mlt::Producer tw(profile_, "timewarp", tw_res.c_str())` at line 464 and
never uses `tw` again; it inserts a plain cut with `out = length/speed`.

Query:

```
$ melt -query producers | grep -iE "timewarp"
  - timewarp
```

Measurement, source clip `tests/data/test_clip_640x360.mp4` (2.000 s):

| Command | Expected | Measured |
|---|---|---|
| baseline 1x | ~2.00 s | **2.005 s** |
| `timewarp:2.0:<src>` | ~1.00 s | **1.002 s** |
| `timewarp:0.5:<src>` | ~4.00 s | **4.010 s** |

```bash
melt -profile atsc_720p_25 "timewarp:2.0:$SRC" -consumer avformat:/tmp/tw_2x.mp4 \
  vcodec=libx264 acodec=aac
ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/tw_2x.mp4
# 1.002000
```

MLT provides correct, first-class variable-speed retiming. The fake
`change_clip_speed` has no MLT-related excuse: the correct producer was
constructed and then thrown away.

## 2. `mlt_properties_anim_set` Does Not Segfault

Claim source: `timeline_model.cpp:982-985` -- "mlt_properties_anim_set
cannot be called twice on the same property (upstream MLT bug: second
call segfaults)".

Tests, all passing, exit 0:

| Test | Conditions | Result |
|---|---|---|
| `test_anim` | 3 sequential `anim_set` on a plain producer property | 3 keys, serialized `0\|=0;50\|=50;100\|=100` |
| `test_anim2` | 3 sequential `anim_set` on `rect` of a real `affine` filter | 3 keys, correct serialization |
| `test_anim2` | 2 sequential `mlt_properties_set` with animation strings | correct |
| `test_anim3` | non-zero `length`, duplicate positions, out-of-order insert, `key_set_type` on every key, then another `anim_set` | 4 keys, no crash |

```
A: length=10 ...            ok
B: duplicate position ...   ok
C: out-of-order + key_set_type ...  keys=4 ... ok
D: anim_set after key_set_type ...  ok
SURVIVED ALL
```

The claim is not reproducible under any condition tested. Note that
vedit's own history contains `2e68f08 Fix use-after-free crash in
mlt_animation_key_get`, and the code carries the comment "Do NOT call
mlt_animation_close -- borrowed pointer". The crashes were almost
certainly vedit's own animation API misuse (property lifecycle on
`mlt_animation_item_s`, borrowed-pointer ownership) misattributed to
upstream.

MLT also ships a rich interpolation set (`mlt_keyframe_smooth_natural`,
`smooth_tight`, sinusoidal, quadratic, cubic, quartic, quintic,
exponential, circular, back, elastic, bounce). "No interpolation control"
is a missing vedit GUI feature, not an MLT gap.

## 3. Stream Switching: Real Constraint, Not a Missing Feature

Claim source: `media_library.cpp:191-198` -- "a subsequent get_frame()
will silently reset the property back to the cached stream's index.
Re-opening the producer is the only way."

Test file: 2 s clip with two audio streams, 440 Hz and 880 Hz.

**Setting the index AFTER decoding one frame** (the claimed poison step):

```
audio_index=1:  set -> readback=1   after render: audio_index=1
audio_index=2:  set -> readback=2   after render: audio_index=1   <- reset
md5 out_a1.wav = md5 out_a2.wav = e1484985e47fb0948c0aff6bb37d2e39
```

The claim is confirmed: the property silently reverts and both renders
are byte-identical.

**Setting the index BEFORE the first decode:**

```
audio_index=1:  set -> readback=1   after render: audio_index=1
audio_index=2:  set -> readback=2   after render: audio_index=2   <- sticks
md5 out_a1.wav = 8e382742bf5b8a05f0079013b1b38976
md5 out_a2.wav = ee2fa5c67836074ea5dd6e275b8da780   <- different
```

MLT fully supports stream selection. The requirement is simply to select
before the first decode, which is exactly what vedit's reopen workaround
achieves. The workaround is correct and standard; only the phrase "the
only way" overstates it. This is not a capability gap and needs no change.

## 4. Text: The One Real Gap

Re-verified because the earlier document measured MLT 7.38 / FFmpeg 8.1.1
and both have since been upgraded.

```
$ ffmpeg -hide_banner -buildconf | grep -iE "freetype|fontconfig|harfbuzz|libass"
(no output)
$ ffmpeg -hide_banner -filters | grep -i drawtext
(drawtext ABSENT)
$ melt -query filters | grep -iE "qtext|dynamictext|^  - text"
  - qtext
  - dynamictext
  - text
```

Headless render attempt:

```
$ timeout 30 melt -profile atsc_720p_25 color:black out=24 \
    -filter dynamictext argument="HELLO" \
    -consumer avformat:/tmp/txt.mp4 vcodec=libx264
```

Result: ran **3 m 11 s** before being killed by hand. `timeout 30` did not
stop it -- the process **ignores SIGTERM**. Output file: **0 bytes**.

This reproduces the June measurement exactly. MLT's text subsystem routes
through a `qtext:` producer requiring a live `QApplication`, and the
pango fallback needs GDK/fontconfig. vedit is SDL2 + ImGui with no Qt, and
the local FFmpeg has no freetype, so neither the MLT path nor the
`avfilter.drawtext` path is available.

This gap is real. It is also one feature, not the engine.

## 5. Save/Load: vedit's Own Defect, Not MLT's

vedit's loader (`project_save_load.cpp:54-136`) rebuilds the timeline by
walking the tractor and re-appending clips from `frame_in`/`frame_out`,
dropping transitions, filters, keyframes, geometry, fades, mute and track
names. Nothing in MLT forces this.

MLT XML stores effects natively:

```
$ melt ... -attach-clip brightness alpha=0.5 ... -transition luma -consumer xml:rt.mlt
filters: 1  transitions: 1
```

Pixel-level round-trip test with `avfilter.negate`:

| Render | md5 of frame 10 |
|---|---|
| A: direct render with filter | `bb68a56a62ca4debb89d77c24fbe8df0` |
| B: saved to XML, reloaded, rendered | `bb68a56a62ca4debb89d77c24fbe8df0` |
| C: no filter (control) | `de313f8bafb69397edf20c27cc5dc86a` |

A equals B, and both differ from C. The filter is genuinely applied and
survives the round trip exactly.

Note: MLT wraps a loaded XML document as an opaque `xml` producer
(`<property name="xml">was here</property>`), so re-serializing a loaded
document yields a wrapper reference rather than an expanded tree. That is
why a naive "reload and dump XML again" check appears to lose effects; it
is an artifact of the check, not data loss. Rendering proves the content
is intact.

**Consequence:** fix the loader by holding the native MLT producer
instead of rebuilding (Phase 1.2). The same mechanism gives undo/redo
(Phase 1.1) -- snapshot the XML, restore on undo, which is what Shotcut
does.

---

## Why the AI Keeps Blaming MLT

The measured pattern is consistent across all five cases:

1. The AI tries an MLT API.
2. It fails, usually because of the AI's own misuse (missing
   `Factory::init()`, wrong function signature, borrowed-pointer
   ownership).
3. It writes a comment asserting an MLT limitation or upstream bug.
4. It adds a hack, or abandons the feature while leaving the code looking
   finished.

Evidence of step 2 in this project: `Factory::init()` is mandatory before
producer creation (my first probe segfaulted without it), and vedit
already carries a cross-process lock around it (`mlt_runtime.cpp`, commit
`d0ab728`). The animation crashes were property-lifecycle bugs. The
timewarp case needed no workaround at all -- the correct object was built
and discarded.

The cost of step 3 is compounding: a false comment becomes the stated
reason a feature is fake, which then justifies skipping it, and the next
agent inherits the false belief. This is what R4 in `CODEBUDDY.md` is
meant to stop.

## Recommendation

**Keep MLT. Do not change the technology choice.**

- Four of five blamed limitations are false or normal usage. The engine is
  not the problem.
- The one real gap, text rendering, is already correctly analysed in
  `doc/tech-direction-text-and-mlt.md`: vedit already owns decoded RGBA
  frames (`preview_renderer.cpp:125`) and already links OpenCV and ImGui,
  so an in-process `vedit.text` filter needs no new dependency and no Qt.
- Rewriting on raw libav* would rebuild the timeline, multitrack,
  transition, seek and serialization machinery that measurably works
  today, to solve a text-rasterization problem.

## Actions

1. Correct the false comments at `timeline_model.cpp:982-985` and
   `media_library.cpp:191-198` ("the only way"). Replace with what was
   measured here.
2. Fix `change_clip_speed` by attaching the `timewarp` producer that is
   already constructed. No MLT obstacle exists. (Phase 0.1.)
3. Implement `vedit.text` per `doc/tech-direction-text-and-mlt.md`
   section 4. (Phase 2.)
4. Rewrite the save/load path to keep the native MLT producer (Phase 1.2),
   which also unblocks undo/redo via XML snapshots (Phase 1.1).
5. Add R7 to `CODEBUDDY.md`: no upstream-blame comment without a
   reproducible probe.
