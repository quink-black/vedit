"""MCP server exposing the vedit GUI editor as tools to AI agents.

Bridge from the Model Context Protocol (stdio JSON-RPC over MCP) to the
in-process JSON-RPC 2.0 server every running `vedit` instance hosts on
/tmp/vedit-<pid>.sock (POSIX only).

Discovery model
---------------
Multiple vedit windows are the common case (open / close / re-open
many times in a session).  Each tool call therefore:

  1. Globs /tmp/vedit-*.sock and probes each via app.identity.
  2. If exactly one instance is alive, talks to it.
  3. If zero or more-than-one are alive, returns a structured error
     so the agent can surface the situation to the user and ask which
     PID to target.

The user can pin a specific pid with the VEDIT_PID environment
variable.  Pinning silences the multi-instance prompt.

Tool surface (mirrors vedit RPC, with friendlier names)
-------------------------------------------------------
  list_instances     -- show every live vedit window
  get_state          -- snapshot of the active instance (tracks, clips, library)
  import_media       -- load one media file into the library
  add_track          -- add a video/audio track
  remove_track       -- remove a track
  append_clip        -- append a clip to a track
  insert_clip        -- insert a clip at a position
  remove_clip        -- remove a clip
  move_clip          -- move a clip (within or across tracks)
  trim_clip          -- set clip in/out points
  split_clip         -- split a clip at a position
  play               -- start playback
  pause              -- pause playback
  seek               -- jump to a frame
  save_project       -- save project to MLT XML
  open_project       -- open a project from MLT XML
  export             -- render the timeline to a file
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import sys
from typing import Any, Optional

from vedit_client import (
    VeditClient,
    VeditConnectionError,
    VeditRpcError,
    Instance,
    discover_instances,
    select_instance,
)

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    stream=sys.stderr,
)
log = logging.getLogger("vedit-mcp")

app = Server("vedit")


# ---------------------------------------------------------------------
# Helpers

def _pin_pid() -> Optional[int]:
    """Read VEDIT_PID; return None if unset / not an integer."""
    raw = os.environ.get("VEDIT_PID")
    if not raw:
        return None
    try:
        return int(raw)
    except ValueError:
        log.warning("VEDIT_PID=%r is not an integer; ignoring", raw)
        return None


def _format_instances(instances: list[Instance]) -> str:
    if not instances:
        return "(none running)"
    lines = []
    for i in instances:
        lines.append(f"  - pid={i.pid:<6} label={i.label:<14} socket={i.socket_path}")
    return "\n".join(lines)


def _ambiguous_error(instances: list[Instance], pinned: Optional[int]) -> str:
    """Build the error text shown to the agent when the target is
    ambiguous.  Stays terse so agents can quote it back to the user
    without adding their own framing."""
    if not instances:
        return (
            "No vedit instance is running.\n"
            "Start vedit (and import any media) before invoking vedit MCP "
            "tools."
        )
    if pinned is not None:
        return (
            f"VEDIT_PID={pinned} but no live vedit instance has that pid.\n"
            f"Live instances:\n{_format_instances(instances)}\n"
            f"Either start a vedit with that pid, or unset VEDIT_PID and "
            f"ask the user which instance to target."
        )
    # Multiple alive, no pin.
    return (
        f"Multiple vedit instances are running. "
        f"Ask the user which one to target, then either:\n"
        f"  - export VEDIT_PID=<pid> before launching, or\n"
        f"  - restart the agent with VEDIT_PID set.\n"
        f"Live instances:\n{_format_instances(instances)}"
    )


def _resolve() -> tuple[Optional[Instance], Optional[str], list[Instance]]:
    """Pick the target instance, or return an error message.

    Returns `(instance, error, all_live)`.  Exactly one of `instance`
    and `error` is non-None.  `all_live` is always populated (possibly
    empty) so callers can include it in their response if they want.
    """
    pinned = _pin_pid()
    instance, all_live = select_instance(pinned)
    if instance is None:
        return None, _ambiguous_error(all_live, pinned), all_live
    return instance, None, all_live


def _call(instance: Instance, method: str, params: Optional[dict] = None):
    """Round-trip the underlying RPC, mapping failures to readable text."""
    try:
        with VeditClient(instance.socket_path) as c:
            return c.call(method, params)
    except VeditConnectionError as ex:
        raise RuntimeError(
            f"could not reach {instance}: {ex}") from ex
    except VeditRpcError as ex:
        # Pass the structured error through so the agent sees the
        # specific code (-32602 InvalidParams etc.) and the message
        # the server attached.
        raise RuntimeError(f"{ex} (instance: {instance})") from ex


def _ok(value: Any) -> list[TextContent]:
    """MCP tools return TextContent payloads.  Encode JSON for
    machine-readability and pretty-print for human-readability."""
    text = json.dumps(value, indent=2, ensure_ascii=False)
    return [TextContent(type="text", text=text)]


def _error(text: str) -> list[TextContent]:
    return [TextContent(type="text", text=text)]


# ---------------------------------------------------------------------
# Tool listing

@app.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="list_instances",
            description=(
                "List every live vedit window currently running on this "
                "machine. Use this when the user mentions multiple vedit "
                "windows, when a tool returned an ambiguous-instance "
                "error, or to confirm VEDIT_PID points at the intended "
                "window. Returns label, pid, and socket path for each."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="get_state",
            description=(
                "Snapshot the active vedit instance: identity, library "
                "(imported media with width/height/frames), tracks, and "
                "clips per track. Always start here before issuing any "
                "modifying call -- clip ids and track ids come from this "
                "snapshot and change as the user edits."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="import_media",
            description=(
                "Import one media file (video/audio/image) into the "
                "project library. Same code path as the GUI File > "
                "Import. Returns the media index to use in later "
                "clip.append / clip.insert calls."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Absolute path to the media file",
                    },
                },
                "required": ["path"],
            },
        ),
        Tool(
            name="add_track",
            description=(
                "Add a new track to the timeline. 'kind' is 'video' "
                "(default) or 'audio'. Optional 'name' sets the track "
                "label. Returns the new track_id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "kind": {
                        "type": "string",
                        "enum": ["video", "audio"],
                        "description": "Track kind (default: video)",
                    },
                    "name": {
                        "type": "string",
                        "description": "Optional track label",
                    },
                },
            },
        ),
        Tool(
            name="remove_track",
            description=(
                "Remove a track and every clip on it. track_id comes "
                "from get_state.tracks[].id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track_id": {
                        "type": "integer",
                        "description": "Track id from get_state",
                    },
                },
                "required": ["track_id"],
            },
        ),
        Tool(
            name="append_clip",
            description=(
                "Append a clip from the media library to the end of a "
                "track. Optional 'in' and 'out' (frame numbers) select "
                "a sub-range of the source; omit for the whole clip. "
                "Returns the new clip_id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track_id": {
                        "type": "integer",
                        "description": "Destination track id",
                    },
                    "media_index": {
                        "type": "integer",
                        "description": "Media index from import_media",
                    },
                    "in": {
                        "type": "integer",
                        "description": "Source in-point (frames)",
                    },
                    "out": {
                        "type": "integer",
                        "description": "Source out-point (frames)",
                    },
                },
                "required": ["track_id", "media_index"],
            },
        ),
        Tool(
            name="insert_clip",
            description=(
                "Insert a clip at a specific timeline position on a "
                "track, padding with blank if past the current end. "
                "Returns the new clip_id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track_id": {
                        "type": "integer",
                        "description": "Destination track id",
                    },
                    "media_index": {
                        "type": "integer",
                        "description": "Media index from import_media",
                    },
                    "position": {
                        "type": "integer",
                        "description": "Timeline position in frames",
                    },
                    "in": {
                        "type": "integer",
                        "description": "Source in-point (frames). Omit for start.",
                    },
                    "out": {
                        "type": "integer",
                        "description": "Source out-point (frames). Omit for end.",
                    },
                },
                "required": ["track_id", "media_index", "position"],
            },
        ),
        Tool(
            name="remove_clip",
            description=(
                "Remove a clip from the timeline. clip_id comes from "
                "get_state.tracks[].clips[].id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {
                        "type": "integer",
                        "description": "Clip id from get_state",
                    },
                },
                "required": ["clip_id"],
            },
        ),
        Tool(
            name="move_clip",
            description=(
                "Move a clip to a new position, optionally on a "
                "different track. The clip id is preserved."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {
                        "type": "integer",
                        "description": "Clip id to move",
                    },
                    "dest_track_id": {
                        "type": "integer",
                        "description": "Destination track id",
                    },
                    "position": {
                        "type": "integer",
                        "description": "New timeline position in frames",
                    },
                },
                "required": ["clip_id", "dest_track_id", "position"],
            },
        ),
        Tool(
            name="trim_clip",
            description=(
                "Set a clip's in and out points (frames). Both are "
                "source-frame numbers, not timeline positions."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {
                        "type": "integer",
                        "description": "Clip id to trim",
                    },
                    "in": {
                        "type": "integer",
                        "description": "New in-point (source frame)",
                    },
                    "out": {
                        "type": "integer",
                        "description": "New out-point (source frame)",
                    },
                },
                "required": ["clip_id", "in", "out"],
            },
        ),
        Tool(
            name="split_clip",
            description=(
                "Split a clip at a timeline position, producing two "
                "clips. The original clip becomes the left half; "
                "returns the new (right) clip_id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {
                        "type": "integer",
                        "description": "Clip id to split",
                    },
                    "at_position": {
                        "type": "integer",
                        "description": "Timeline frame to split at",
                    },
                },
                "required": ["clip_id", "at_position"],
            },
        ),
        Tool(
            name="play",
            description=(
                "Start playback from the current position."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="pause",
            description=(
                "Pause playback."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="seek",
            description=(
                "Jump the playhead to a frame. Returns the resulting "
                "position (may be clamped to the timeline length)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "position": {
                        "type": "integer",
                        "description": "Frame to seek to",
                    },
                },
                "required": ["position"],
            },
        ),
        Tool(
            name="save_project",
            description=(
                "Save the current project to an MLT XML file. The "
                "file includes the timeline assembly and library "
                "references."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Output .mlt or .xml path",
                    },
                },
                "required": ["path"],
            },
        ),
        Tool(
            name="open_project",
            description=(
                "Open a previously saved MLT XML project file, "
                "replacing the current timeline."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Project .mlt or .xml path",
                    },
                },
                "required": ["path"],
            },
        ),
        Tool(
            name="export",
            description=(
                "Render the timeline to a video file (MP4 by default). "
                "Same assembly as preview, swapped consumer -- the "
                "export reflects exactly what is on the timeline. "
                "Returns a job_id immediately; use export_poll to "
                "check progress (state, fraction)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Output file path (.mp4)",
                    },
                },
                "required": ["path"],
            },
        ),
        # ---- Filter operations ----
        Tool(
            name="list_filters",
            description=(
                "List all available MLT filters with their parameter "
                "metadata. Use this to discover what filters can be "
                "applied and what parameters they accept."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="add_filter",
            description=(
                "Add a filter to a clip or track. Specify either "
                "clip_id (from get_state.tracks[].clips[].id) or "
                "track_id (from get_state.tracks[].id). filter_name "
                "comes from list_filters."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "target_type": {
                        "type": "string",
                        "enum": ["clip", "track"],
                        "description": "Apply to a clip or a track",
                    },
                    "target_id": {
                        "type": "integer",
                        "description": "clip_id or track_id from get_state",
                    },
                    "filter_name": {
                        "type": "string",
                        "description": "Filter identifier from list_filters (e.g. 'avfilter.hflip')",
                    },
                },
                "required": ["target_type", "target_id", "filter_name"],
            },
        ),
        Tool(
            name="remove_filter",
            description="Remove a filter from a clip or track.",
            inputSchema={
                "type": "object",
                "properties": {
                    "target_type": {
                        "type": "string",
                        "enum": ["clip", "track"],
                    },
                    "target_id": {"type": "integer"},
                    "filter_name": {"type": "string"},
                },
                "required": ["target_type", "target_id", "filter_name"],
            },
        ),
        Tool(
            name="set_filter_param",
            description=(
                "Set a filter parameter value. target_type/target_id "
                "select the clip or track. filter_name is the filter "
                "identifier. param is the parameter name (from "
                "list_filters). value is a string. For animated "
                "(keyframe) values, pass keyframes as a JSON array of "
                "{frame, value, type} objects."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "target_type": {
                        "type": "string",
                        "enum": ["clip", "track"],
                    },
                    "target_id": {"type": "integer"},
                    "filter_name": {"type": "string"},
                    "param": {"type": "string"},
                    "value": {
                        "type": "string",
                        "description": "Scalar value as string, e.g. '3' or '0xff0000ff'",
                    },
                },
                "required": ["target_type", "target_id", "filter_name", "param", "value"],
            },
        ),
        Tool(
            name="get_filter_param",
            description=(
                "Get a filter parameter value (including keyframes). "
                "Returns the current value or animation data."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "target_type": {
                        "type": "string",
                        "enum": ["clip", "track"],
                    },
                    "target_id": {"type": "integer"},
                    "filter_name": {"type": "string"},
                    "param": {"type": "string"},
                },
                "required": ["target_type", "target_id", "filter_name", "param"],
            },
        ),
        # ---- Media library ----
        Tool(
            name="remove_media",
            description=(
                "Remove a media item from the library. Refuses if any "
                "clip on the timeline still references it."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "index": {
                        "type": "integer",
                        "description": "Media index from get_state.library[].index",
                    },
                },
                "required": ["index"],
            },
        ),
        # ---- Composite geometry ----
        Tool(
            name="set_composite_geometry",
            description=(
                "Set the position and size of a video track's content "
                "on the canvas. x, y, w, h are percentages (0-100) of "
                "the output resolution. Track 0 (background) has no "
                "composite. Use this to create picture-in-picture or "
                "split-screen layouts."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track_id": {
                        "type": "integer",
                        "description": "Track id from get_state (must be a video track > 0)",
                    },
                    "x": {
                        "type": "number",
                        "description": "X offset as percentage (default: 0)",
                    },
                    "y": {
                        "type": "number",
                        "description": "Y offset as percentage (default: 0)",
                    },
                    "w": {
                        "type": "number",
                        "description": "Width as percentage (default: 100)",
                    },
                    "h": {
                        "type": "number",
                        "description": "Height as percentage (default: 100)",
                    },
                },
                "required": ["track_id"],
            },
        ),
        # ---- Advanced clip operations ----
        Tool(
            name="overwrite_clip",
            description=(
                "Insert a clip at a position, overwriting any existing "
                "clips at that position (ripple insert with overwrite)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track_id": {"type": "integer"},
                    "media_index": {"type": "integer"},
                    "position": {
                        "type": "integer",
                        "description": "Timeline frame position",
                    },
                    "in": {
                        "type": "integer",
                        "description": "Source in-point (frames). Omit for start.",
                    },
                    "out": {
                        "type": "integer",
                        "description": "Source out-point (frames). Omit for end.",
                    },
                },
                "required": ["track_id", "media_index", "position"],
            },
        ),
        Tool(
            name="lift_clip",
            description=(
                "Remove a clip and close the gap (ripple delete). "
                "Shifts later clips left to fill the space."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {
                        "type": "integer",
                        "description": "Clip id from get_state",
                    },
                },
                "required": ["clip_id"],
            },
        ),
        Tool(
            name="change_speed",
            description=(
                "Change the playback speed of a clip. speed=1.0 is "
                "normal, 2.0 is double speed, 0.5 is half speed."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {"type": "integer"},
                    "speed": {
                        "type": "number",
                        "description": "Speed multiplier (default: 1.0)",
                    },
                },
                "required": ["clip_id", "speed"],
            },
        ),
        Tool(
            name="detach_audio",
            description=(
                "Split audio from a video clip into a separate audio "
                "track. Returns the new audio clip id."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clip_id": {"type": "integer"},
                },
                "required": ["clip_id"],
            },
        ),
        Tool(
            name="add_transition",
            description=(
                "Add a cross-fade transition between two adjacent clips "
                "on a track. clip_index is the playlist index (including "
                "blanks). length is in frames (default: 30)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "track_id": {"type": "integer"},
                    "clip_index": {
                        "type": "integer",
                        "description": "Playlist index of the first clip",
                    },
                    "length": {
                        "type": "integer",
                        "description": "Transition length in frames (default: 30)",
                    },
                },
                "required": ["track_id", "clip_index"],
            },
        ),
        # ---- Frame capture ----
        Tool(
            name="get_frame",
            description=(
                "Render a single composed frame at a specific position "
                "and save it as a PNG file. Returns the file path and "
                "frame position."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "position": {
                        "type": "integer",
                        "description": "Frame number to capture",
                    },
                    "path": {
                        "type": "string",
                        "description": "Output PNG file path",
                    },
                },
                "required": ["path"],
            },
        ),
        Tool(
            name="screenshot",
            description=(
                "Capture a screenshot of the current playhead position "
                "as a PNG file. Convenience wrapper around get_frame "
                "that uses the current position."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Output PNG file path",
                    },
                },
                "required": ["path"],
            },
        ),
        # ---- Project settings ----
        Tool(
            name="set_profile",
            description=(
                "Set the output canvas resolution and framerate. Call "
                "before placing clips; the profile determines render "
                "dimensions."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "width": {
                        "type": "integer",
                        "description": "Canvas width in pixels",
                    },
                    "height": {
                        "type": "integer",
                        "description": "Canvas height in pixels",
                    },
                    "fps": {
                        "type": "number",
                        "description": "Frames per second (e.g. 25, 29.97)",
                    },
                },
                "required": ["width", "height", "fps"],
            },
        ),
        # ---- Export management ----
        Tool(
            name="export_poll",
            description=(
                "Check the progress of an async export job. Returns "
                "state ('running'/'done'/'failed'/'cancelled') and "
                "fraction (0.0 to 1.0)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "job_id": {
                        "type": "integer",
                        "description": "Job id from export",
                    },
                },
                "required": ["job_id"],
            },
        ),
        Tool(
            name="export_cancel",
            description="Cancel a running export job.",
            inputSchema={
                "type": "object",
                "properties": {
                    "job_id": {"type": "integer"},
                },
                "required": ["job_id"],
            },
        ),
        Tool(
            name="export_list_presets",
            description=(
                "List available MLT avformat export presets (H.264, "
                "H.265, ProRes, etc.)."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="export_load_preset",
            description=(
                "Load a specific export preset and return its "
                "default codec parameters."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "file": {
                        "type": "string",
                        "description": "Preset file path from export_list_presets",
                    },
                },
                "required": ["file"],
            },
        ),
    ]


# ---------------------------------------------------------------------
# Tool dispatch

@app.call_tool()
async def call_tool(
    name: str, arguments: dict[str, Any]
) -> list[TextContent]:

    # list_instances doesn't need a single resolved target -- it
    # always reports every live one.
    if name == "list_instances":
        instances = discover_instances()
        pinned = _pin_pid()
        return _ok({
            "instances": [
                {
                    "pid": i.pid,
                    "label": i.label,
                    "socket": i.socket_path,
                }
                for i in instances
            ],
            "pinned_pid": pinned,
            "count": len(instances),
        })

    instance, err, _all = _resolve()
    if err is not None:
        return _error(err)
    assert instance is not None

    try:
        if name == "get_state":
            return _ok(_call(instance, "state.get"))

        if name == "import_media":
            return _ok(_call(instance, "library.import",
                             {"path": str(arguments["path"])}))

        if name == "add_track":
            params = {}
            if "kind" in arguments:
                params["kind"] = str(arguments["kind"])
            if "name" in arguments:
                params["name"] = str(arguments["name"])
            return _ok(_call(instance, "track.add", params))

        if name == "remove_track":
            return _ok(_call(instance, "track.remove",
                             {"track_id": int(arguments["track_id"])}))

        if name == "append_clip":
            params = {
                "track_id": int(arguments["track_id"]),
                "media_index": int(arguments["media_index"]),
            }
            if "in" in arguments:
                params["in"] = int(arguments["in"])
            if "out" in arguments:
                params["out"] = int(arguments["out"])
            return _ok(_call(instance, "clip.append", params))

        if name == "insert_clip":
            params = {
                "track_id": int(arguments["track_id"]),
                "media_index": int(arguments["media_index"]),
                "position": int(arguments["position"]),
            }
            if "in" in arguments:
                params["in"] = int(arguments["in"])
            if "out" in arguments:
                params["out"] = int(arguments["out"])
            return _ok(_call(instance, "clip.insert", params))

        if name == "remove_clip":
            return _ok(_call(instance, "clip.remove",
                             {"clip_id": int(arguments["clip_id"])}))

        if name == "move_clip":
            return _ok(_call(instance, "clip.move", {
                "clip_id": int(arguments["clip_id"]),
                "dest_track_id": int(arguments["dest_track_id"]),
                "position": int(arguments["position"]),
            }))

        if name == "trim_clip":
            return _ok(_call(instance, "clip.trim", {
                "clip_id": int(arguments["clip_id"]),
                "in": int(arguments["in"]),
                "out": int(arguments["out"]),
            }))

        if name == "split_clip":
            return _ok(_call(instance, "clip.split", {
                "clip_id": int(arguments["clip_id"]),
                "at_position": int(arguments["at_position"]),
            }))

        if name == "play":
            return _ok(_call(instance, "player.play"))

        if name == "pause":
            return _ok(_call(instance, "player.pause"))

        if name == "seek":
            return _ok(_call(instance, "player.seek",
                             {"position": int(arguments["position"])}))

        if name == "save_project":
            return _ok(_call(instance, "project.save",
                             {"path": str(arguments["path"])}))

        if name == "open_project":
            return _ok(_call(instance, "project.open",
                             {"path": str(arguments["path"])}))

        if name == "export":
            return _ok(_call(instance, "export.start",
                             {"path": str(arguments["path"])}))

        # ---- Filter operations ----
        if name == "list_filters":
            return _ok(_call(instance, "filter.list"))

        if name == "add_filter":
            target_type = str(arguments["target_type"])
            filter_name = str(arguments["filter_name"])
            target_id = int(arguments["target_id"])
            if target_type == "track":
                return _ok(_call(instance, "filter.add",
                                 {"track_id": target_id, "filter": filter_name}))
            else:
                return _ok(_call(instance, "filter.add",
                                 {"clip_id": target_id, "filter": filter_name}))

        if name == "remove_filter":
            target_type = str(arguments["target_type"])
            filter_name = str(arguments["filter_name"])
            target_id = int(arguments["target_id"])
            if target_type == "track":
                return _ok(_call(instance, "filter.remove",
                                 {"track_id": target_id, "filter": filter_name}))
            else:
                return _ok(_call(instance, "filter.remove",
                                 {"clip_id": target_id, "filter": filter_name}))

        if name == "set_filter_param":
            target_type = str(arguments["target_type"])
            filter_name = str(arguments["filter_name"])
            target_id = int(arguments["target_id"])
            param = str(arguments["param"])
            value = str(arguments["value"])
            params = {"filter": filter_name, "param": param, "value": value}
            if target_type == "track":
                params["track_id"] = target_id
            else:
                params["clip_id"] = target_id
            return _ok(_call(instance, "filter.set_param", params))

        if name == "get_filter_param":
            target_type = str(arguments["target_type"])
            filter_name = str(arguments["filter_name"])
            target_id = int(arguments["target_id"])
            param = str(arguments["param"])
            params = {"filter": filter_name, "param": param}
            if target_type == "track":
                params["track_id"] = target_id
            else:
                params["clip_id"] = target_id
            return _ok(_call(instance, "filter.get_param", params))

        # ---- Media library ----
        if name == "remove_media":
            return _ok(_call(instance, "library.remove",
                             {"index": int(arguments["index"])}))

        # ---- Composite geometry ----
        if name == "set_composite_geometry":
            params = {"track_id": int(arguments["track_id"])}
            if "x" in arguments:
                params["x"] = float(arguments["x"])
            if "y" in arguments:
                params["y"] = float(arguments["y"])
            if "w" in arguments:
                params["w"] = float(arguments["w"])
            if "h" in arguments:
                params["h"] = float(arguments["h"])
            return _ok(_call(instance, "track.set_composite_geometry", params))

        # ---- Advanced clip operations ----
        if name == "overwrite_clip":
            params = {
                "track_id": int(arguments["track_id"]),
                "media_index": int(arguments["media_index"]),
                "position": int(arguments["position"]),
            }
            if "in" in arguments:
                params["in"] = int(arguments["in"])
            if "out" in arguments:
                params["out"] = int(arguments["out"])
            return _ok(_call(instance, "clip.overwrite", params))

        if name == "lift_clip":
            return _ok(_call(instance, "clip.lift",
                             {"clip_id": int(arguments["clip_id"])}))

        if name == "change_speed":
            return _ok(_call(instance, "clip.change_speed", {
                "clip_id": int(arguments["clip_id"]),
                "speed": float(arguments["speed"]),
            }))

        if name == "detach_audio":
            return _ok(_call(instance, "clip.detach_audio",
                             {"clip_id": int(arguments["clip_id"])}))

        if name == "add_transition":
            params = {
                "track_id": int(arguments["track_id"]),
                "clip_index": int(arguments["clip_index"]),
            }
            if "length" in arguments:
                params["length"] = int(arguments["length"])
            return _ok(_call(instance, "transition.add", params))

        # ---- Frame capture ----
        if name == "get_frame":
            params = {"path": str(arguments["path"])}
            if "position" in arguments:
                params["position"] = int(arguments["position"])
            return _ok(_call(instance, "player.get_frame", params))

        if name == "screenshot":
            return _ok(_call(instance, "view.screenshot",
                             {"path": str(arguments["path"])}))

        # ---- Project settings ----
        if name == "set_profile":
            return _ok(_call(instance, "project.set_profile", {
                "width": int(arguments["width"]),
                "height": int(arguments["height"]),
                "fps": float(arguments["fps"]),
            }))

        # ---- Export management ----
        if name == "export_poll":
            return _ok(_call(instance, "export.poll",
                             {"job_id": int(arguments["job_id"])}))

        if name == "export_cancel":
            return _ok(_call(instance, "export.cancel",
                             {"job_id": int(arguments["job_id"])}))

        if name == "export_list_presets":
            return _ok(_call(instance, "export.list_presets"))

        if name == "export_load_preset":
            return _ok(_call(instance, "export.load_preset",
                             {"file": str(arguments["file"])}))

        return _error(f"unknown tool: {name}")

    except RuntimeError as ex:
        return _error(str(ex))


# ---------------------------------------------------------------------
# Entrypoint

async def _main_async() -> None:
    async with stdio_server() as (read_stream, write_stream):
        await app.run(
            read_stream,
            write_stream,
            app.create_initialization_options(),
        )


def main() -> None:
    asyncio.run(_main_async())


if __name__ == "__main__":
    main()
