# vedit

Experimental non-linear editor built on MLT. A local ImGui GUI and a
JSON-RPC agent drive the same state through the same `AppController`
seam; neither channel has a privileged path.

Direction, priority and the current phase plan live in
`doc/next-phase-design.md`. Read it before starting any task.

## Hard Rules

These rules exist because this project accumulated 54 RPC methods and 51
MCP tools while undo, save fidelity and drag & drop were still missing,
and because three features were committed as done while being fake
(`change_speed` built a timewarp producer and discarded it;
`detach_audio` mixed source and timeline frame spaces; `export.cancel`
had two identical if/else branches). All 105 tests passed the whole time.

### R1 Definition of Done

Do not say "done", "complete", "implemented" or "100%" unless all hold:

1. Behavior is observable: `state.get` output or rendered pixels change
   as intended. "The call did not error" is not completion.
2. A test asserts a **result value**, not merely `ok == true`, `id > 0`,
   `!is_null()` or "did not crash".
3. Both channels reach it: any `AppController` capability is available
   over RPC and in the GUI, or is explicitly documented as RPC-only.
4. Round-trip coverage: timeline-mutating capabilities survive
   save then open, or are listed in the Phase 1.2 fidelity backlog.
5. README and `tools/vedit-mcp/README.md` updated in the same change.

### R2 No Fake Implementations

Reject any change containing these shapes:

- An MLT object constructed in a function but never `connect`ed,
  `attach`ed, `insert`ed or `set` in that function. Delete it instead.
- if/else branches with identical bodies.
- `in`/`out` parameters whose frame space is not declared. `src_in`/
  `src_out` and `timeline_start`/`timeline_end` are never interchanged.
- Comments describing behavior the code does not implement.
- User-visible UI text promising a capability with no implementation.

### R3 Basic Before New

Until Phase 0 and Phase 1 of `doc/next-phase-design.md` are complete:
no new RPC methods, no new MCP tools, and no M3/M4/M5 work (keyframe
curve editor, movit GPU rendering, workflow integration). New capability
requires finishing the current phase plus explicit user agreement.

### R4 Disprove Before Building

Before depending on any MLT or FFmpeg capability, verify with one
reproducible command that it exists and works on this machine, and paste
the command and output into the commit message or design doc. Do not
stack a second workaround on a failed direction: two consecutive failures
means stop and report, not silently switch to a third direction.

### R7 Do Not Blame Upstream Without a Probe

Before writing any comment or report claiming MLT (or any dependency)
lacks a capability, has a bug, or "cannot" do something, prove it with a
standalone probe program and paste the command and output. Measured
example: `timeline_model.cpp:982-985` claims `mlt_properties_anim_set`
segfaults on the second call; it actually survives repeated calls,
duplicate positions, and interleaved `key_set_type`
(`doc/mlt-capability-audit.md`). The real crashes were vedit's own
animation-lifecycle bugs.

**Why:** a false limitation comment becomes the justification for a fake
feature, which the next agent inherits as fact. This loop already cost
this project a fake `change_clip_speed` even though MLT's `timewarp`
producer works.

**How to apply:** if an MLT call fails, first suspect our own usage --
missing `Factory::init()`, wrong signature, borrowed-pointer ownership.
Only after a probe fails does an upstream limitation become a valid
explanation, and it must be recorded with the probe.

### R5 Docs Single Source of Truth

`doc/next-phase-design.md` is the only authority on direction. Stale gap
docs must be deleted or marked SUPERSEDED, never kept as to-do sources.
Citing a file that does not exist is a defect.

### R6 Report Progress Honestly

Progress is reported as G1-G4 attainment, completed plan items, and new
semantic assertions. Never as counts of new RPC methods, MCP tools or
lines of code.

## Verify Before Claiming

```bash
cmake --build build -j8
cd build && VEDIT_AUDIO_CONSUMER=null SDL_VIDEODRIVER=offscreen ./tests/vedit_tests
```

A passing suite is necessary, not sufficient. If `build/` fails to
configure with a catch2 include-path error, the CMakeCache is stale after
a Homebrew upgrade: remove `build/CMakeCache.txt` and `build/CMakeFiles`
and reconfigure.
