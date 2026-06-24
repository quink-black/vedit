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

| Tool | What it does |
|---|---|
| `list_instances` | Enumerate every live vedit window |
| `get_state` | Snapshot: identity, library, tracks, clips |
| `import_media` | Import a media file into the library |
| `add_track` | Add a video/audio track |
| `remove_track` | Remove a track and its clips |
| `append_clip` | Append a clip to the end of a track |
| `insert_clip` | Insert a clip at a timeline position |
| `remove_clip` | Remove a clip |
| `move_clip` | Move a clip (within or across tracks) |
| `trim_clip` | Set clip in/out points |
| `split_clip` | Split a clip at a timeline position |
| `play` | Start playback |
| `pause` | Pause playback |
| `seek` | Jump the playhead to a frame |
| `save_project` | Save the project to MLT XML |
| `open_project` | Open a project from MLT XML |
| `export` | Render the timeline to a video file |

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
