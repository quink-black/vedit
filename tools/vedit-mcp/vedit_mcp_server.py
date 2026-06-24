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
                "Blocking: returns once the render is complete."
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
            return _ok(_call(instance, "clip.insert", {
                "track_id": int(arguments["track_id"]),
                "media_index": int(arguments["media_index"]),
                "position": int(arguments["position"]),
            }))

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
