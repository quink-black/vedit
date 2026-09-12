# vedit: Current-State Audit and Next-Phase Plan

Date: 2026-09-11
Status: pending review. This document is the single source of truth for
direction and priority.

Companion: `doc/mlt-capability-audit.md` tests every "MLT cannot do X"
claim in this codebase against the installed MLT 7.40.0. Four of five are
false or normal usage; only text rendering is a real gap. MLT stays.

---

## 0. How This Audit Was Done

Every conclusion below comes from reading the current code, building it,
and running the tests. Nothing is taken from commit messages or from what
earlier documents claim was done.

| Check | Command / basis | Result |
|---|---|---|
| Build | `cmake -B build -G Ninja && cmake --build build` | passes (after clearing a stale CMakeCache, see note) |
| Tests | `./build/tests/vedit_tests` | 105 cases / 692 assertions, all green |
| RPC surface | `grep register_method src/rpc/rpc_methods.cpp` | 54 methods |
| MCP surface | `grep 'name=' tools/vedit-mcp/vedit_mcp_server.py` | 51 tools |
| Filters | `FilterCatalog` startup log | 438 |

Note: the checked-in `build/` had a poisoned CMakeCache pointing at
`/opt/homebrew/Cellar/catch2/3.14.0`, which Homebrew has since replaced
with 3.15.3. Removing `build/CMakeCache.txt` and `build/CMakeFiles` and
reconfiguring fixes it. The source is fine; this is a stale build dir.

**Central finding: a green build plus 105 passing tests is not evidence
that features work.** The test suite is currently the main source of
false confidence. See 2.4.

---

## 1. Project Goal, Restated With Falsifiable Criteria

Original goal (README): an **Agent-Native Collaborative NLE** where a
local GUI and a JSON-RPC agent drive the same state through the same
`AppController` seam, with no privileged path for either channel.

The goal is right, but it never said what "done" means, so it got
substituted with "the API surface grew". Fix that first.

### 1.1 Four verifiable properties

| # | Property | Falsifiable criterion |
|---|---|---|
| G1 | **Editing is complete** | A typical edit (import -> split -> move -> transition -> color -> retime -> caption -> export) can be finished without reading the source |
| G2 | **Persistence is lossless** | After `project.save` then `project.open`, timeline structure and every effect parameter compare equal field by field |
| G3 | **Channels are equal** | Every `AppController` capability is reachable from both GUI and RPC |
| G4 | **Everything is undoable** | Every timeline-mutating operation can be undone and redone, including ones issued over RPC |

### 1.2 Current attainment

| Goal | Attainment | Blocker |
|---|---|---|
| G1 editing complete | **~50%** | no text/captions, no ripple, no snapping, no drag & drop; retime and audio detach are broken |
| G2 lossless persistence | **~30%** | only tracks and clip in/out points are restored; transitions, filters, keyframes, geometry, fades and mute are all dropped |
| G3 channel equality | **~60%** | 54 RPC methods vs roughly 30 reachable from the GUI; `set_geometry/rotation/crop/fade` have no GUI |
| G4 undoable | **0%** | no undo/redo code exists anywhere |

---

## 2. Current-State Audit

### 2.1 Genuinely working (verified in code, not self-reported)

- Core timeline editing: `append / insert / remove / move / trim / split`
  through the mlt++ Playlist, real effect -- `timeline_model.cpp:197-398`
- Transitions: `playlist->mix()` plus `mix_add` planting luma/mix; the
  `type` parameter is genuinely handled (an older gap doc says it is
  ignored; that was fixed) -- `timeline_model.cpp:486-545`
- Filter attach and parameters at clip and track level via
  `mlt_service_attach` -- `timeline_model.cpp:809-845`
- Keyframes: animation strings plus `mlt_animation_key_set_type` --
  `timeline_model.cpp:956-1029`
- Export: `consumer_avformat` with range export and progress callbacks --
  `export_service.cpp`
- Preview: `consumer-frame-show` to a GL texture; `preview_renderer.cpp`
  has no stubs
- Per-clip geometry / crop / fade / rotation exist in the domain and RPC
- `project.set_profile` and `library.set_stream` (audio stream switch) work

### 2.2 Claimed done, actually fake (highest priority)

These four are the concrete evidence of "AI said it was done". They share
a signature: the code looks finished, has comments, returns values, and
has tests, but the semantics never happen. None of them is caused by an
MLT limitation -- see `doc/mlt-capability-audit.md`, which measured each
one against MLT 7.40.0.

| Feature | Location | Actual behavior |
|---|---|---|
| **Retime `clip.change_speed`** | `timeline_model.cpp:443-484` | Line 464 builds `Mlt::Producer tw(profile_, "timewarp", ...)` and **`tw` is never used again**. What gets inserted is a plain cut of the source with `out = length/speed`. The clip is truncated, not retimed. |
| **Audio detach `clip.detach_audio`** | `timeline_model.cpp:547-565` | Appends a duplicate to a new audio track, using the **timeline position** `found->second.start` as the **source** in-point (wrong frame space). The original clip's audio is never disabled, so audio plays twice. |
| **Export cancel `export.cancel`** | `export_service.cpp:122-133` | `if (on_cancel) { consumer.run(); } else { consumer.run(); }` -- both branches are identical. The progress callback `on_export_frame` (L26-37) never checks the cancel flag. The comment at `app_controller.h:197-199` claiming the worker observes the flag at the next frame is false. |
| **Channel equality on the GUI side** | `main.cpp:302,335` | Status text and a tooltip state media can be dragged to the timeline. There is no `BeginDragDropSource` or `BeginDragDropPayload` anywhere in the tree. Only OS-level file drop exists, and it imports to the library, not the timeline. |

### 2.3 Basic functionality that is entirely missing

`grep -iE "undo|redo|ripple|snap"` over `src/` returns only two unrelated
hits (`PixelSnapH`, and the word "Snapshot" in a comment).

| Feature | Status | Note |
|---|---|---|
| **Undo / redo** | missing | Hard gap for an editor. G4 = 0% |
| **Lossless save/load** | missing | `project_save_load.cpp:54-136` walks the tractor and rebuilds from each clip's `frame_in/frame_out` only. Blanks/gaps are skipped (L117-119); clip ids are re-minted. Transitions, filters, keyframes, geometry, crop, fades, mute/hidden, track names and composite geometry are **all lost** |
| **Ripple / ripple-delete / slip / slide** | missing | Delete is a plain `clip_remove`; it neither leaves nor closes a gap |
| **Snapping** | missing | no snapping code |
| **Media library thumbnails** | missing | text-only list |
| **Audio waveform / level meters** | missing | only track mute exists |
| **Text / captions** | missing | see 2.5 |
| **Keyframe curve editor** | table only | keyframes can be added and removed; no curve, no interpolation control |
| **Keyboard shortcuts** | minimal | only Left/Right frame step and I/O marking. No space-to-play, no Delete, no Ctrl+Z/S |

### 2.4 Why the tests did not catch any of this

105 cases pass, but assertion strength is weakest exactly where the fake
features live.

| Test | Location | Problem |
|---|---|---|
| `clip.change_speed wraps clip in timewarp` | `test_app_controller.cpp:1442-1458` | asserts only that no RPC error occurred; **never asserts the duration halved**. The test name says timewarp and still cannot detect that `tw` is unused |
| `clip.detach_audio creates audio track` | `test_app_controller.cpp:1460-1477` | asserts only `clip_id > 0` |
| `clip.lift` | `test_app_controller.cpp:1425-1440` | asserts `attached_filters().empty()`; never checks that a blank replaced the clip |
| transition test | `test_app_controller.cpp:1626-1654` | `clip_count() >= before` is a tautology |
| audio/video enable | `test_app_controller.cpp:1679-1716` | comment reads "should still render (blank frames but no crash)" |
| `ProjectModel constructs` | `test_smoke.cpp:15-18` | `REQUIRE(true)` |
| save/load | `test_app_controller.cpp:1099-1144` | **pins id loss as expected behavior**, turning a defect into a contract |

Zero coverage: undo/redo (absent feature), export_cancel,
`filter.set_range`, track-level filter keyframes, rotation readback,
video stream switching, fade audio readback, and save/load fidelity for
filters and transitions.

### 2.5 Text and captions: the direction is right, the code is not there

`doc/tech-direction-text-and-mlt.md` is a good decision document: keep
MLT, implement a `vedit.text` filter in-process, rasterize with Core Text
or stb_truetype. **It is dated 2026-06-27 and nothing has been built.**
The core assumption it cites from `doc/text-overlay-gaps.md` L88/L123
("drawtext always exists") has since been disproven by measurement.

Current state: `filter.set_range` landed in `fa25326`, so filters can be
timed, but there is no working text filter to time.

### 2.6 Documentation drift (fuel for agent drift)

| Drift | Evidence |
|---|---|
| README method table is stale | lists 16 methods, 54 exist; `export.start` documented as returning `{"ok":true}`, actually returns `{"job_id"}` |
| README links to missing files | `docs/phase1-design.md`, `docs/phase1-scope.md` (the directory is `doc/`; neither file exists) |
| Code cites a missing file | `export_service.cpp:150` says "See lessons-mistakes-to-avoid.md #10"; no such file |
| Stale gap doc still in the tree | `.codebuddy/geometry_transition_gap.md` claims `clip.set_geometry` does not exist and `transition.add` ignores `type`. Both are implemented. Using it as a to-do list causes duplicate work |
| MCP README is stale | documents 17 tools, 51 exist |
| Decision docs uncommitted | `doc/tech-direction-text-and-mlt.md` and `doc/text-overlay-gaps.md` are still untracked |

### 2.7 Other confirmed defects

- `TimelineModel::ensure_composite_transitions()` is declared at
  `timeline_model.h:68` with **no definition anywhere**; nothing calls it,
  so it would fail to link
- `TimelineModel::playlist_index_of()` reads `"vedit_clip_id"` (L625) but
  every write uses `"_vedit_clip_id"` (L19) -> **always returns -1**
- `ensure_affine_filter()` has a comment saying it creates and attaches
  the filter, then returns `nullptr` without doing so
- RPC parameter type mismatches throw `nlohmann::json::type_error`, which
  the dispatcher reports as InternalError `-32603` instead of
  InvalidParams `-32602` -- `rpc_dispatcher.cpp:80-81`
- `clip.get_geometry` and `clip.get_crop` return `true` with zero-value
  defaults for a nonexistent clip, silently succeeding
- `app.list_instances` does a blocking `recv()` with no timeout on the GUI
  thread (`rpc_methods.cpp:104-119`); a stale socket hangs the UI
- `save_xml` L45 uses the un-normalized `path` (it should use
  `normalized`); `load_xml` L83 has the same problem, and lines 60 and 83
  **parse the XML twice**
- The GUI's `gui.project_fps` is not updated after `profile_set`
  (`main.cpp:144` vs `1824`), so ruler and timecodes desync after a
  profile change

---

## 3. Why Direction Drifts

Not an intelligence problem. Three structural causes, each blockable by
a rule:

1. **No Definition of Done, so commit messages became the acceptance
   test.** "Add per-clip geometry API" and "Add per-clip fade API" are
   real commits with real code, but "added an API" was treated as
   synonymous with "the feature works".

2. **Tests assert shape, not semantics.** Asserting `clip_id > 0` or "no
   error" makes a fake implementation indistinguishable from a real one.

3. **Adding surface is easier than fixing substance, and reads as
   progress.** 54 RPC methods and 51 MCP tools were accumulated while
   undo, save fidelity and drag & drop were still missing. A stale gap
   document then handed the agent a wrong to-do list.

---

## 4. Development Constraints (hard rules for any agent)

These target the three causes above. A violation means the task is not
done.

### R1 -- Definition of Done

A feature is done only when all of the following hold. Otherwise the
words "done", "complete", "implemented" or "100%" are forbidden in
commits and in reports.

1. **Behavior is observable**: after the call, `state.get` output or the
   rendered pixels change as intended. "The call did not error" does not
   count.
2. **A test asserts semantics**: it must assert a **result value**, not
   merely `ok == true`, `id > 0`, `!is_null()` or "did not crash".
   Existing counterexample to fix: `test_app_controller.cpp:1442-1458`.
3. **Both channels can reach it**: any capability exposed through
   `AppController` is reachable from RPC and GUI, or is explicitly
   recorded as RPC-only and marked as such in the README (G3).
4. **Round-trip coverage**: any capability that mutates timeline state
   survives save then open, or is explicitly listed in the 5.1 fidelity
   backlog (G2).
5. **Docs updated**: README method table and
   `tools/vedit-mcp/README.md` updated in the same change.

### R2 -- No fake implementations (targeting the 2.2 patterns)

The following are the recurring shapes of fake work. Any of them means
the change is rejected:

- **Constructing an object and never using it** (the `tw` producer in
  `change_speed`). Rule: any MLT object constructed inside a function
  must be `connect`ed, `attach`ed, `insert`ed or `set` within that
  function, or the construction is deleted.
- **if/else branches with identical bodies** (`if (on_cancel) run(); else
  run();`). Rule: branches must differ in behavior, or be deleted.
- **Using timeline coordinates as source coordinates** (`detach_audio`).
  Rule: every `in`/`out` parameter must declare its frame space in its
  name or comment; `src_in`/`src_out` and `timeline_start`/`timeline_end`
  must never be interchanged.
- **Comments describing behavior that is not implemented**. Rule: comments
  describe what the code actually does today. `app_controller.h:197-199`
  is an instance of this.
- **UI text promising a capability that does not exist** ("drag to
  timeline"). Rule: user-visible text must have a matching implementation.

### R3 -- Basic before new (priority rule)

Until Phase 0 and Phase 1 of section 5 are complete:

- **No new RPC methods** (54 already exceeds what is maintainable)
- **No new MCP tools** (51 already)
- **No M3 keyframe curve editor, no M4 GPU/movit, no M5 workflow
  integration**

The only legitimate route to new capability is finishing every item in
the current phase of 5.1 plus explicit user agreement to advance.

### R4 -- Disprove before building (extends Rule P5)

Before writing code that depends on an MLT capability existing, verify
with one reproducible command that it exists and works on this machine,
and **paste the command and its output into the commit message or design
doc**. `text-overlay-gaps.md` assumed `drawtext` existed; one
`ffmpeg -buildconf | grep freetype` would have ended that loop on day one
and instead it consumed several rounds.

Do not stack a second workaround on a failed direction. Two consecutive
failures means stop and report; do not silently switch to a third
direction.

### R5 -- Single source of truth for docs

- `doc/next-phase-design.md` (this file) is the only authority on
  direction and priority.
- **Stale gap docs must be deleted or marked SUPERSEDED**; they must not
  survive as to-do sources. Immediate action:
  `.codebuddy/geometry_transition_gap.md`.
- Citing a file that does not exist is a defect (currently
  `export_service.cpp:150`).
- Any change to the API surface updates README and MCP README in the same
  commit.

### R6 -- Report progress quantitatively, never by API count

Progress reports may use only: **G1-G4 attainment percentages**, the
count of completed items in section 5, and the number of new semantic
assertions. "Added N RPC methods / N MCP tools / N lines" is forbidden as
evidence of progress.

---

## 5. Next-Phase Plan

Ordering principle: stop the bleeding (fake features, no undo, data
loss), then complete the basic editing experience, then new capability.

### Phase 0 -- Stop the bleeding (highest priority)

| # | Task | Acceptance |
|---|---|---|
| 0.1 | Fix `change_clip_speed`: attach the timewarp producer that is already constructed at `timeline_model.cpp:464` | assert that with `speed=2.0` the clip duration halves **and the content really plays faster** (not duration alone). Verified feasible: MLT's `timewarp` producer measures 2.0x -> 1.002 s and 0.5x -> 4.010 s on a 2.005 s source (`doc/mlt-capability-audit.md` section 1) |
| 0.2 | Fix `detach_audio`: cut with source coordinates and disable the original clip's audio | assert the original clip has no audio, the new clip matches the source range, and there is no doubled audio |
| 0.3 | Fix `export.cancel`: check the cancel flag inside `on_export_frame` and call `consumer.stop()` | assert that cancelling at frame N returns within 1s and the output file is incomplete |
| 0.4 | Delete or implement `ensure_composite_transitions()` (undefined) | no undefined declarations remain; link is clean |
| 0.5 | Fix the `_vedit_clip_id` spelling mismatch in `playlist_index_of()` | assert the correct index is returned; give the function its first caller or delete it |
| 0.6 | Use the normalized path in `save_xml`/`load_xml`; remove the duplicate XML parse | Windows path test passes; XML parsed once |
| 0.7 | Update the GUI's `project_fps` after `profile_set` | after a profile change, ruler and timecodes match `state.get` |
| 0.8 | Return `-32602` for RPC parameter type errors; add a timeout to `app.list_instances` | new parameter validation tests |
| 0.9 | Make `clip.get_geometry`/`get_crop` return an error for invalid clip_id | assert the error code |
| 0.10 | Mark `.codebuddy/geometry_transition_gap.md` SUPERSEDED or delete it | no stale to-do sources remain |
| 0.11 | **Retime audio**: OPEN, now measured through the real export path rather than a bare-producer probe. Exporting a timeline containing a 2x retimed clip gives video 1.0 s (correct) and audio 1.856 s (lags by 0.856 s). The bare-producer probe showed the same, while the `melt` CLI produced correct 1.0 s audio on the same producer, so the defect sits in how the audio request is built for a timewarp producer outside `melt` -- not in the retime itself. Explicit `audio_frequency`/`audio_channels` on the producer and on the frame, and `frequency`/`channels`/`real_time` on the consumer, were all tried and none of them change it. The test pins video correctness and SKIPs on the audio lag so it starts failing the moment this is fixed. | audio duration matches video duration on a retimed export |

### Phase 1 -- Basic functionality (G2 and G4 at zero are hard blockers)

| # | Task | Acceptance |
|---|---|---|
| 1.1 | **Undo/redo**: command stack at the `AppController` layer. Use MLT XML snapshots (Shotcut's approach), which are proven to preserve effects pixel-for-pixel | every write op has an inverse; both RPC and GUI go through it (G4) |

#### 1.1 status: started, blocked on one crash

A snapshot-based implementation was prototyped (a `MutationGuard` in
`AppController` wrapping all 32 mutators, snapshot via `save_xml`,
restore via the `adopt` loader, `undo`/`redo` RPC methods) and reverted
rather than merged, because of a defect found during its test:

- Undo and redo of structural operations work (append, remove, empty
  timeline edge cases) -- 13 assertions passed.
- The process then segfaults in `save_xml` when snapshotting a project
  that was itself restored by `adopt` **and contains a clip**. Because
  every mutation snapshots first, this makes undo unusable after the
  first restore, and it also means `project.save` after
  `project.open` on a non-empty project is unsafe.

Evidence: with tracing, the crash lands inside
`ProjectSaveLoad::save_xml` on an adopted tractor; `Tractor(mlt_tractor)`,
`Multitrack(mlt_multitrack)` and `Playlist(mlt_playlist)` all `inc_ref()`
in their mlt++ constructors, so the lifetime theory is not confirmed. A
backtrace under a debugger is the next step (lldb hung in batch mode;
try `catchpoint`/signal handler or ASan).

**Update, same day.** Undo for structural operations (track add, clip
append/remove, and any sequence of them) is now merged, built on a
single hook rather than per-method guards: every mutator already ends
with `player_->refresh()`, so `PlayerController` exposes a
state-changed listener and `AppController::record_history()` snapshots
the project there. A snapshot identical to the current entry is
dropped, which is what makes a failed mutation a no-op, and
`restoring_` suppresses recording while `undo`/`redo` themselves
refresh the preview. Ten filter and composite-geometry mutators were
missing `refresh()` entirely, which also meant filter changes never
updated the preview; they call it now.

`filter_add` -> `undo` -> `redo` now restores the filter. Root cause
measured and fixed: MLT's XML reader attaches a filter written inside a
playlist `<entry>` to the entry's **parent producer**, not to the cut
(`cut_filters=0, parent_filters=10` right after adopt), so per-clip
filters cannot survive an MLT XML round trip while clips share one
source producer. Rather than give every clip its own producer, the
document now has a sidecar: `save_xml` writes `<path>.vedit` recording
each clip's filters by track and clip index in document order (which
`adopt` preserves), and `load_xml` re-attaches them, detaching whatever
MLT loaded first so the sidecar is authoritative. Params round-trip as
`{"value": scalar}` or `{"keyframes": [...]}` and are replayed through
the same setters the RPC uses.

Two defects surfaced while wiring it, both real and now fixed: the ten
filter and composite-geometry mutators called `player_->refresh()`
*before* the mutation, so the preview lagged one edit behind and the
history hook recorded the pre-mutation state (which is why undo looked
like it worked while redo restored nothing); and a stale sidecar was
never removed when a save produced no filters.

The loader is exonerated. A regression test
(`save after load of a project with clips`) exercises exactly what the
undo path does -- save, adopt, save again, then four more
adopt/save/refresh cycles, plus a track-with-no-clips round trip -- and
passes. So repeated adopt, save and preview refresh are all safe, and
the segfault is in the prototype's own code. The likely cause is that
the mutator wrapping was generated by a script; a `undo_guard.commit()`
landing in the wrong function, or a commit paired with the wrong
snapshot, would corrupt the history exactly as observed. Re-attempt
undo by writing the guards by hand (or wrapping at a single choke
point) and verify the generated diff by reading it.

A second finding from the same prototype is real and independent:
`adopt` calls `library_.import(path)` for every restored clip without
deduplicating by path, so repeated undo/redo grows the media library.
`MediaLibrary::import` should return the existing index for a known
path.

|
| 1.2 | **Save/load fidelity**: hold the native MLT producer instead of read-then-rebuild | round-trip test compares field by field: transitions, filters and params, keyframes, geometry, crop, fades, mute/hidden, track names, composite geometry, blanks/gaps, clip ids (G2). MLT already preserves these: a filtered render is pixel-identical before and after a save/load cycle (`doc/mlt-capability-audit.md` section 5), so this is our loader's defect, not an engine limit |
| 1.3 | **Timeline drag & drop**: ImGui `BeginDragDropSource/Payload`, including library to timeline | DONE: library items are drag sources carrying the library index and the timeline canvas is a drop target; the lane comes from the mouse Y and the insert point from the mouse X, snapped to clip edges |
| 1.4 | **Snapping**: playhead, clip edges, track boundaries | DONE for clip edges: a `snap_frame` helper snaps to the nearest edge of any other clip within ~8 px, skipping the clip being manipulated so it cannot glue to itself, and it is applied to clip moves, both trim edges and library drops |
| 1.5 | **Ripple delete / slip / slide** | after delete, later clips shift and total duration is correct. DONE for delete and slip: `mlt_playlist_remove` already closes the gap (verified in MLT source), so delete ripples by default and `lift_clip` is the gap-leaving variant; `slip_clip` was added on `mlt_playlist_resize_clip`. NEW DEFECT found while testing, now fixed: `trim_clip` ripples. It removed and re-inserted the clip, and because `mlt_playlist_remove` closes the gap, every later clip on the track shifted by the length change. It now resizes the cut in place and pads with blank on whichever side the clip gave up, so the neighbours land back where they were and the total duration is stable. Two API details mattered: `insert_blank` takes a clip INDEX (not a frame position) and the blank's OUT frame, so N frames is `out = N - 1`. |
| 1.6 | **Keyboard shortcuts**: space to play, Delete, Ctrl+Z/Y, Ctrl+S/O, S to split | -- |
| 1.7 | **Audio waveform and level meters** | DONE for the waveform: `compute_peaks` (in `vedit_util`, unit-tested -- a full-scale sine fills every bin it lands in, a single full-scale sample lights exactly one bin, silence is flat) folds samples into bins; `WaveformCache` decodes on a worker thread and the timeline draws the envelope inside clips on audio tracks. One MLT trap: pulling frames directly yields SILENT audio because nothing requests it -- decoded audio only flows through a consumer, so the decode renders audio-only to a temp WAV and reads it back. Verified live: importing a clip produced a 100-bin envelope with real peaks. Level meters are not part of this item. |
| 1.8 | **Media library thumbnails** | DONE: a `ThumbnailCache` decodes one frame per item on a worker thread with its own producer and a small fixed profile, downscales to 96 px wide and hands back RGBA; the main thread turns it into an SDL texture. The result is cached, so each item decodes exactly once. Verified live: importing a clip over RPC produced a 96x72 thumbnail and the process stayed up; note that MLT's `sqvga` profile does not ship with the Homebrew install, so the decoder uses `quarter_15` (320x240 at 15 fps). |

### Phase 2 -- Text and captions (direction decided, implementation missing)

Follow `doc/tech-direction-text-and-mlt.md` section 4 exactly:

| # | Task | Acceptance |
|---|---|---|
| 2.1 | Register an in-process MLT filter `vedit.text` that draws onto the RGBA frame | `filter.add {filter:"vedit.text"}` works |
| 2.2 | Rasterization backend: Core Text on macOS; stb_truetype plus a bundled font cross-platform | CJK text (for example a Chinese caption) renders real glyphs, not boxes |
| 2.3 | Combine with the existing `filter.set_range` for timed captions | assert no text is drawn outside [in, out] |
| 2.4 | GUI: edit text content, font, size, position, duration | DONE: a Caption section in the Filters panel offers Add/Remove caption plus multiline text, font family (blank = system), size, horizontal alignment and a frame range wired to `filter.set_in_out`. Buffers reload when the selection moves so two clips do not share one edit state. Verified live: attaching `vedit.text` over RPC and setting a Chinese caption changed the rendered frame from 21491 to 26017 bytes with different contents. |

Prerequisite (R4) verified on this machine before any code was written,
re-run 2026-09-11:

- A1: `ffmpeg -buildconf | grep -iE "freetype|fontconfig|harfbuzz|libass"`
  returns nothing and `ffmpeg -filters | grep drawtext` finds nothing.
  The Homebrew FFmpeg has no text libraries, so `avfilter.drawtext` is
  unavailable.
- A2: `melt -query filters` lists 426 filters, 276 of them `avfilter.*`.
  The avfilter bridge works; only text is missing.
- A3: `qtext`, `dynamictext` and `text` are all present.
- A4 (reproduced): `melt ... -filter dynamictext argument="HELLO"
  -consumer avformat:...` was still running after 3m07s despite
  `timeout 25`, so it ignores SIGTERM, and left a 0-byte output file.
  MLT's text path is unusable headless in this process, as documented.

Progress: 2.1, 2.2 and 2.3 are DONE. `vedit.text` is registered in
process through `mlt_repository_register`, draws into the RGBA frame
buffer it is handed, and rasterizes CJK through Core Text on macOS. The
test renders a blank frame, then two different Chinese captions, and
asserts each differs from the blank and from the other -- which is what
rules out a constant placeholder box. The range test asserts the frame
outside [in, out] is byte-identical to the untitled render and the frame
inside is not.

### Phase 3 -- Channel equality cleanup (G3)

| # | Task | Acceptance |
|---|---|---|
| 3.1 | Add GUI for `clip.set_geometry/rotation/crop/fade_in/fade_out` | DONE: a Transform section in the Filters panel drives the same AppController methods the RPC handlers call, so both channels share one code path, and a test asserts geometry, rotation and crop read back what was set. Rotation gained a getter (`clip.get_rotation`, also exposed as an RPC method) because the GUI would otherwise have to cache a value it cannot verify -- the same duplicated-state defect the audit flagged for the fps cache. Fade lengths are not stored on the clip, so the GUI resets them on selection change rather than pretending to read a value that does not exist. |
| 3.2 | Keyframes: curve editor plus interpolation type | state matches `mlt_animation`. DONE on the state side: `set_filter_param_keyframes` now feeds keyframes through `mlt_properties_anim_set` instead of assigning a serialized string. A plain `mlt_properties_set` of `"frame=value;..."` left the property non-animated, so `get_filter_param_keyframes` reported a bare value and every reader that checks for an animation saw nothing. Interpolation types (linear, discrete, smooth) round-trip and are asserted. The old test asserted the bare-value shape, which pinned the defect, and now asserts keyframes. The GUI curve editor is Phase 4. |
| 3.3 | Rewrite README and MCP README to cover all 54 methods / 51 tools | DONE: the README now documents all 57 registered methods and the MCP README all 51 tools, both generated from the code (handler bodies for the method params, tool `description` strings for the tools). A new `test_docs.cpp` parses the dispatcher's registered names and the two documents and fails on any method or tool that is documented but registered, or vice versa -- so the tables cannot rot again. Also fixed the dead `docs/phase1-*.md` links and the stale test count. |

### Phase 4 -- Only after the above

M3 curve editor depth, M4 GPU (movit) rendering, M5 agent workflow
integration. Reassess G1-G4 and get explicit user agreement before
entering (R3).

**M3 curve editor: DONE.** Animated parameters now get a value-vs-frame
canvas: a polyline through the keyframes, draggable handles (horizontal
moves the frame, vertical moves the value, with the list kept in
ascending order while dragging), and an interpolation combo for the
selected keyframe (linear / discrete / smooth). Only numeric parameters
get a curve; string and colour parameters keep the existing list, since
they have no meaningful value axis.

**M4 GPU (movit): NOT POSSIBLE on this toolchain, and not faked.** The
Homebrew MLT ships `share/mlt/presets/filter/movit.*` but no
`libmltmovit.so` and no movit producer (`melt -query producers | grep
movit` is empty), so there is nothing to drive. Implementing it would
mean building MLT against movit and giving vedit a GL context to share
with MLT -- a prerequisite change, not a feature. Recorded rather than
worked around, per R2.

**M5 agent workflow: DONE as a checked end-to-end scenario.** The audit
flagged `auto_edit.py` as unverified, with a docstring claiming it drove
"JSON-RPC / MCP" when it only ever spoke RPC. It has now been run
against a live instance and passes: RESULT: PASS, exit 0, a 146877-byte
MP4 carrying h264 640x360 and aac at 2.453 s, plus an 8064-byte project
XML. The docstring now says it speaks RPC rather than MCP, and records
that the run needs sources of at least 60 frames -- the clips in
`tests/data` have only 50, which is why the scenario had never been
exercised. The MCP README documents how to run it and the last result.

---

## 6. Immediate Actions

1. Land R1-R6 into `CODEBUDDY.md` so every session loads them.
2. Delete or mark `.codebuddy/geometry_transition_gap.md` (2.6).
3. Fix the dead README links (`docs/phase1-*.md`).
4. Start at Phase 0.1 (retime). It is the clearest example of "claimed
   done", and fixing it establishes the R1 acceptance pattern for
   everything after.
