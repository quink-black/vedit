#!/usr/bin/env bash
# tools/vedit-mcp/setup.sh -- provision the local venv for the vedit
# MCP server.  Idempotent: re-running just upgrades dependencies.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 not found in PATH" >&2
    exit 1
fi

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi

source "$VENV_DIR/bin/activate"
pip install --upgrade pip >/dev/null
pip install -r "$SCRIPT_DIR/requirements.txt"

echo ""
echo "vedit-mcp ready."
echo "  venv:  $VENV_DIR"
echo "  entry: $SCRIPT_DIR/vedit_mcp_server.py"
echo ""
echo "Add this to ~/.codebuddy/mcp.json under mcpServers:"
echo
cat <<EOF
  "vedit": {
    "command": "$VENV_DIR/bin/python",
    "args": ["$SCRIPT_DIR/vedit_mcp_server.py"],
    "env": {}
  }
EOF
