"""Append-only local audit log — one JSON line per tool call, per the
plan's "write an append-only local audit record containing requester/
session, tool, validated parameters, response status, and timestamp."

MCP's stdio transport doesn't give this server a strong caller identity
beyond the connection itself, so "session" here is a per-process id
generated at server startup, not a per-request identity.
"""

import json
import os
import time
import uuid
from pathlib import Path

_DEFAULT_PATH = Path.home() / ".rp2350-signal-agent" / "audit.jsonl"
_SESSION_ID = uuid.uuid4().hex[:12]


def _audit_path() -> Path:
    override = os.environ.get("RP2350_SIGNAL_AUDIT_LOG")
    return Path(override) if override else _DEFAULT_PATH


def record(tool: str, params: dict, status: str, error: str | None = None) -> None:
    """status is "ok" or "error"; error carries str(exception) when present.
    Never raises — a logging failure must not take down a tool call."""
    entry = {
        "timestamp": time.time(),
        "session_id": _SESSION_ID,
        "tool": tool,
        "params": params,
        "status": status,
        "error": error,
    }
    try:
        path = _audit_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a") as f:
            f.write(json.dumps(entry, default=str) + "\n")
    except OSError:
        pass
