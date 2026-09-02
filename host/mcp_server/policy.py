"""MCP-layer safety policy, separate from (and in addition to) the
firmware's own arm/lease enforcement. The device already refuses digital/
PWM writes without a valid arm lease — but UART/I2C/SPI are buses, not
gated by that mechanism at all, so per the plan ("Default MCP policy
should be read-only; write tools require the explicit arm lease, and
bus-transfer tools should require an allowlisted address/rate profile")
this module adds the MCP-level gate that covers everything, including the
buses the firmware itself doesn't restrict.

Both checks are environment-variable driven and deny-by-default, read once
per process (not hot-reloaded) — read at server startup, not per-call, so
policy can't be changed mid-session without restarting the server.
"""

import os

_ALLOW_WRITES_ENV = "RP2350_SIGNAL_ALLOW_WRITES"
_I2C_ALLOWLIST_ENV = "RP2350_SIGNAL_I2C_ALLOWLIST"


class PolicyError(Exception):
    """Raised when a tool call is blocked by MCP-layer policy — distinct
    from DeviceError (a hardware/protocol failure) so callers can tell
    "not allowed" apart from "didn't work"."""


def writes_enabled() -> bool:
    return os.environ.get(_ALLOW_WRITES_ENV, "").strip().lower() in ("1", "true", "yes")


def require_writes_enabled(tool_name: str) -> None:
    if not writes_enabled():
        raise PolicyError(
            f"{tool_name} is a write operation and is blocked by default. "
            f"Set {_ALLOW_WRITES_ENV}=1 in this MCP server's environment to allow it."
        )


def i2c_address_allowlist() -> set:
    raw = os.environ.get(_I2C_ALLOWLIST_ENV, "")
    addrs = set()
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        addrs.add(int(token, 0))
    return addrs


def require_i2c_address_allowed(address: int) -> None:
    allowlist = i2c_address_allowlist()
    if address not in allowlist:
        raise PolicyError(
            f"I2C address {address:#04x} is not in the allowlist. Set "
            f"{_I2C_ALLOWLIST_ENV} (comma-separated, e.g. '0x50,0x68') to permit it."
        )
