#!/usr/bin/env bash
# Sets up (or reuses) a venv for the GUI: same venv host/setup.sh uses,
# with the PySide6/pyqtgraph extras added on top. Safe to re-run.
#
# Usage:
#   host/gui/setup.sh                    # venv at host/.venv
#   RP2350_SIGNAL_VENV=/path setup.sh    # venv at a custom location
set -euo pipefail

HOST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${RP2350_SIGNAL_VENV:-$HOST_DIR/.venv}"

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
else
    echo "Reusing existing venv at $VENV_DIR"
fi

"$VENV_DIR/bin/pip" install -q --upgrade pip
"$VENV_DIR/bin/pip" install -q -e "$HOST_DIR[gui]"

echo ""
echo "Done. Launch the GUI:"
echo "  $VENV_DIR/bin/rp2350-signal-gui"
