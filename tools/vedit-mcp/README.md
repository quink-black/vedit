# vedit-mcp

MCP server bridging the [vedit](../../) video editor to AI agents.

vedit hosts a JSON-RPC 2.0 server inside every running window, on
`/tmp/vedit-<pid>.sock` (POSIX). This MCP server discovers those sockets,
picks the right one, and exposes vedit's RPC methods as MCP tools so an
agent (CodeBuddy / Claude Desktop / Cursor / ...) can drive vedit from
a conversation.

## Setup

```bash
cd tools/vedit-mcp
bash setup.sh
```

The script provisions a local `.venv/`, installs `mcp>=1.0.0`, and
prints a snippet you can paste into `~/.codebuddy/mcp.json`.

## Discovery model

- **One vedit window running** -> auto-targeted, no config needed.
- **Multiple vedit windows running** -> every tool call returns a
  structured error listing the live instances. The agent should ask
  the user which one to target, then either restart with
  `VEDIT_PID=<pid>` in the environment, or call `list_instances`
  first to confirm.
- **No vedit running** -> tools return a "start vedit first" message.

## Tools

Generated from the tool definitions in `vedit_mcp_server.py`; `ctest` fails if they drift apart. 51 tools.

| Tool | What it does |
|---|---|
| `add_filter` | Add a filter to a clip or track. Specify either clip_id (from get_state.tracks[].clips[].id) or track_id (from get_state.tracks[].id). filter_name comes from list_filters. Optional in/out (frame numbers, clip-relative) set the filter's active range. |
| `add_track` | Add a new track to the timeline. 'kind' is 'video' (default) or 'audio'. Optional 'name' sets the track label. Returns the new track_id. |
| `add_transition` | Add a transition between two adjacent clips on a track. clip_index is the playlist index (including blanks). length is in frames (default: 30). type selects the transition style (default: dissolve); call list_transition_types for the available values. |
| `analyze_track` | Run OpenCV object tracking on a clip. Given an initial bounding rect, tracks the object across all frames and returns per-frame rect keyframes. The tracker filter stays attached so the results can be rendered as an overlay. algo selects the OpenCV tracker (KCF, MIL, CSRT, MOSSE, ...). steps controls keyframe spacing. |
| `append_clip` | Append a clip from the media library to the end of a track. Optional 'in' and 'out' (frame numbers) select a sub-range of the source; omit for the whole clip. Returns the new clip_id. |
| `change_speed` | Change the playback speed of a clip. speed=1.0 is normal, 2.0 is double speed, 0.5 is half speed. |
| `detach_audio` | Split audio from a video clip into a separate audio track. Returns the new audio clip id. |
| `export` | Render the timeline to a video file (MP4 by default). Same assembly as preview, swapped consumer -- the export reflects exactly what is on the timeline. Returns a job_id immediately; use export_poll to check progress (state, fraction). |
| `export_cancel` | Cancel a running export job. |
| `export_list_presets` | List available MLT avformat export presets (H.264, H.265, ProRes, etc.). |
| `export_load_preset` | Load a specific export preset and return its default codec parameters. |
| `export_poll` | Check the progress of an async export job. Returns state ('running'/'done'/'failed'/'cancelled') and fraction (0.0 to 1.0). |
| `get_composite_geometry` | Get the current position and size of a video track's content on the canvas. Returns x, y, w, h as percentages (0-100). Track 0 always reports 0/0/100/100. |
| `get_crop` | Get the current per-clip crop margins in pixels. Returns (0,0,0,0) when no crop has been set. |
| `get_filter_param` | Get a filter parameter value (including keyframes). Returns the current value or animation data. |
| `get_frame` | Render a single composed frame at a specific position and save it as a PNG file. Returns the file path and frame position. |
| `get_geometry` | Get the current per-clip geometry: position and size as percentages (0-100). Returns defaults (0/0/100/100) when no geometry has been set on the clip. |
| `get_state` | Snapshot the active vedit instance: identity, library (imported media with width/height/frames), tracks, and clips per track. Always start here before issuing any modifying call -- clip ids and track ids come from this snapshot and change as the user edits. |
| `import_media` | Import one media file (video/audio/image) into the project library. Same code path as the GUI File > Import. Returns the media index to use in later clip.append / clip.insert calls. |
| `insert_clip` | Insert a clip at a specific timeline position on a track, padding with blank if past the current end. Returns the new clip_id. |
| `lift_clip` | Remove a clip and close the gap (ripple delete). Shifts later clips left to fill the space. |
| `list_filters` | List all available MLT filters with their parameter metadata. Use this to discover what filters can be applied and what parameters they accept. |
| `list_instances` | List every live vedit window currently running on this machine. Use this when the user mentions multiple vedit windows, when a tool returned an ambiguous-instance error, or to confirm VEDIT_PID points at the intended window. Returns label, pid, and socket path for each. |
| `list_transition_types` | List the transition types add_transition can apply. Each entry has id, title, and description. Only these ids produce a visible result. |
| `move_clip` | Move a clip to a new position, optionally on a different track. The clip id is preserved. |
| `open_project` | Open a previously saved MLT XML project file, replacing the current timeline. |
| `overwrite_clip` | Insert a clip at a position, overwriting any existing clips at that position (ripple insert with overwrite). |
| `pause` | Pause playback. |
| `play` | Start playback from the current position. |
| `project_new` | Clear the current timeline and media library to start a fresh project. All clips, tracks, and imports are discarded. Call this before building a new timeline from scratch. |
| `remove_clip` | Remove a clip from the timeline. clip_id comes from get_state.tracks[].clips[].id. |
| `remove_filter` | Remove a filter from a clip or track. |
| `remove_media` | Remove a media item from the library. Refuses if any clip on the timeline still references it. |
| `remove_track` | Remove a track and every clip on it. track_id comes from get_state.tracks[].id. |
| `save_project` | Save the current project to an MLT XML file. The file includes the timeline assembly and library references. |
| `screenshot` | Capture a screenshot of the current playhead position as a PNG file. Convenience wrapper around get_frame that uses the current position. |
| `seek` | Jump the playhead to a frame. Returns the resulting position (may be clamped to the timeline length). |
| `set_audio_enabled` | Enable or disable the audio stream on a clip. When disabled the clip is silent in preview and export but stays in the timeline. |
| `set_composite_geometry` | Set the position and size of a video track's content on the canvas. x, y, w, h are percentages (0-100) of the output resolution. Track 0 (background) has no composite. Use this to create picture-in-picture or split-screen layouts. |
| `set_crop` | Set per-clip crop margins in pixels. Omitted params keep their current value. Set all to 0 to remove the crop filter entirely. |
| `set_fade_in` | Add or remove a fade-in on a clip. Duration is in frames. The fade animates brightness (video) and volume (audio) from 0 to normal over the given duration. Set duration=0 to remove the fade. |
| `set_fade_out` | Add or remove a fade-out on a clip. Duration is in frames. The fade animates brightness (video) and volume (audio) from normal to 0 at the clip end. Set duration=0 to remove the fade. |
| `set_filter_param` | Set a filter parameter value. target_type/target_id select the clip or track. filter_name is the filter identifier. param is the parameter name (from list_filters). value is a string. For animated (keyframe) values, pass keyframes as a JSON array of {frame, value, type} objects. |
| `set_geometry` | Set per-clip position and size on the canvas. x, y, w, h are percentages (0-100) of the profile resolution. Omitted params keep their current value. Use this for picture-in-picture, split-screen, or zoom/pan. |
| `set_profile` | Set the output canvas resolution and framerate. Call before placing clips; the profile determines render dimensions. |
| `set_rotation` | Set per-clip rotation in degrees (clockwise). The clip rotates around its center. Set to 0 to remove rotation. |
| `set_track_hidden` | Show or hide the video on a track. When hidden, the track's video is invisible in preview and export (the audio still plays). Does not remove or change clips on the track. |
| `set_track_muted` | Mute or unmute the audio on a track. When muted, the track's audio is silent in preview and export. Does not remove or change clips on the track. |
| `set_video_enabled` | Enable or disable the video stream on a clip. When disabled the clip shows black in preview and export but stays in the timeline. |
| `split_clip` | Split a clip at a timeline position, producing two clips. The original clip becomes the left half; returns the new (right) clip_id. |
| `trim_clip` | Set a clip's in and out points (frames). Both are source-frame numbers, not timeline positions. |

## Pinning a specific instance

```bash
VEDIT_PID=12345 codebuddy ...
```

The MCP server reads `VEDIT_PID` once per call. Pinning silences the
multi-instance prompt and refuses to fall back to any other window.

## Implementation notes

- `vedit_client.py` holds the raw RPC client and discovery logic.
  Stand-alone -- no MCP dependency. Reusable from ad-hoc scripts.
- `vedit_mcp_server.py` is the MCP shim. Each MCP tool maps 1-to-1 to
  one vedit RPC method, with friendlier names and inline schemas so
  the agent picks the right one.
- Tools open a fresh socket per call (cheap on a Unix domain socket).
  This makes the server stateless and survives the user closing /
  reopening vedit between calls.
