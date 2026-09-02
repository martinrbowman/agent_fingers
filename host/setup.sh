#!/usr/bin/env bash
# Sets up (or reuses) a venv for the host/ Python tools: device library,
# MCP server, CLI, and the tests/integration/ pytest suite. Safe to re-run
# — only creates the venv if it doesn't already exist, always reinstalls/
# updates deps so pulling new commits and rerunning this is enough to stay
# current.
#
# Usage:
#   host/setup.sh                    # venv at host/.venv
#   RP2350_SIGNAL_VENV=/path setup.sh  # venv at a custom location
set -euo pipefail

HOST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${RP2350_SIGNAL_VENV:-$HOST_DIR/.venv}"

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
else
    echo "Reusing existing venv at $VENV_DIR"
fi

"$VENV_DIR/bin/pip" install -q --upgrade pip
"$VENV_DIR/bin/pip" install -q -e "$HOST_DIR"
"$VENV_DIR/bin/pip" install -q pytest

echo ""
echo "Done. Either activate the venv:"
echo "  source $VENV_DIR/bin/activate"
echo ""
echo "...or use the tools directly without activating:"
echo "  $VENV_DIR/bin/rp2350-signal-cli info"
echo "  $VENV_DIR/bin/rp2350-signal-mcp --device SERIAL"
echo "  $VENV_DIR/bin/pytest tests/integration/ --device SERIAL"
