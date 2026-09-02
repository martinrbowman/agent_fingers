"""MCP stdio server for Agent Fingers — the only layer
Claude Code (or another MCP client) talks to. Per the plan, this module
must not parse binary frames or make USB calls directly; all of that is in
device.transport/device.client, imported here only through the typed
Device API.

Run directly: `python -m mcp_server.server [--device SERIAL]`
Or via the installed console script: `rp2350-signal-mcp [--device SERIAL]`
"""

import argparse
import functools
import sys

from device import Device, DeviceError

from mcp.server.mcpserver import MCPServer

from . import audit, policy

mcp = MCPServer(
    "agent-fingers",
    instructions=(
        "Controls a physical Agent Fingers board over USB: an "
        "8-channel bidirectional digital I/O bank (GP0-7), a 3-channel ADC "
        "(GP26-28), PWM on any digital-bank channel, UART0 (GP16/17), I2C "
        "master+slave (GP20/21, GP18/19), and SPI master+slave "
        "(GP8-11, GP12-15, mode 0 only). All pins are 3.3V CMOS — never "
        "attach 5V logic or negative/over-VREF analog voltages directly. "
        "Digital/PWM outputs require an explicit arm lease (see "
        "outputs_arm) and expire automatically if not renewed. Every write "
        "operation (digital/PWM/UART/I2C/SPI writes, arming, slave "
        "configuration) is disabled by default at this MCP layer, "
        "independent of the firmware's own arm/lease enforcement — set "
        "RP2350_SIGNAL_ALLOW_WRITES=1 in this server's environment to allow "
        "them. I2C transfers additionally require the target address to be "
        "listed in RP2350_SIGNAL_I2C_ALLOWLIST (comma-separated hex)."
    ),
)

_device: Device | None = None


def _get_device() -> Device:
    if _device is None:
        raise DeviceError("no device connection — server did not start correctly")
    return _device


def _audited(tool_name):
    """Wraps a tool function so every call (success or failure) is
    recorded, and PolicyError/DeviceError propagate as normal Python
    exceptions for the MCP SDK to surface as a tool error to the client."""

    def decorator(fn):
        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            try:
                result = fn(*args, **kwargs)
                audit.record(tool_name, kwargs, "ok")
                return result
            except Exception as e:
                audit.record(tool_name, kwargs, "error", error=str(e))
                raise

        return wrapper

    return decorator


# -- Device info / status --------------------------------------------------


@mcp.tool()
@_audited("device_info")
def device_info() -> dict:
    """Firmware version, protocol version, board unique ID, and git commit
    of the connected Agent Fingers board."""
    return _get_device().get_info()


@mcp.tool()
@_audited("device_capabilities")
def device_capabilities() -> dict:
    """Static channel counts for this board's peripherals (digital bank,
    ADC, UART, I2C, SPI)."""
    return _get_device().get_capabilities()


@mcp.tool()
@_audited("device_status")
def device_status() -> dict:
    """Uptime, last reset cause, safe-state reason, current arm state, and
    protocol parser health counters (a climbing error count means a
    transport or framing bug, not device damage)."""
    status = dict(_get_device().get_status())
    status.pop("arm_challenge", None)  # internal, used by outputs_arm itself
    return status


@mcp.tool()
@_audited("device_ping")
def device_ping(payload_hex: str = "") -> dict:
    """Round-trip check: echoes payload_hex back and reports device uptime
    at the moment of the reply. Also renews the output-arm lease if one is
    currently armed."""
    result = _get_device().ping(bytes.fromhex(payload_hex))
    return {"uptime_us": result["uptime_us"], "echo_hex": result["echo"].hex()}


# -- Digital bank: arm/disarm/read/write ------------------------------------


@mcp.tool()
@_audited("outputs_arm")
def outputs_arm(output_mask: int, lease_ms: int = 0) -> dict:
    """Arms a subset of the 8 digital-bank channels (GP0-7, bit0=GP0) for
    driving. Required before digital_write or pwm_set(enabled=True) on any
    of those channels. lease_ms defaults to 2000 if 0, clamped to
    [100, 30000] — call device_ping periodically to renew it, or it expires
    and the bank snaps back to input/high-Z automatically. Blocked unless
    RP2350_SIGNAL_ALLOW_WRITES=1 is set in this server's environment."""
    policy.require_writes_enabled("outputs_arm")
    granted_lease_ms = _get_device().arm_outputs(output_mask, lease_ms)
    return {"granted_lease_ms": granted_lease_ms}


@mcp.tool()
@_audited("outputs_disarm")
def outputs_disarm() -> dict:
    """Immediately forces the digital bank (and any active PWM) back to
    input/high-Z. Always allowed, even without RP2350_SIGNAL_ALLOW_WRITES —
    disarming is the safe direction, never a hazard."""
    _get_device().disarm_outputs()
    return {"ok": True}


@mcp.tool()
@_audited("digital_write")
def digital_write(mask: int, values: int) -> dict:
    """Drives the given bits (mask) to the given levels (values) on the
    digital bank. Every bit set in mask must already be armed via
    outputs_arm, or this is rejected. Blocked unless
    RP2350_SIGNAL_ALLOW_WRITES=1."""
    policy.require_writes_enabled("digital_write")
    _get_device().digital_write_masked(mask, values)
    return {"ok": True}


@mcp.tool()
@_audited("digital_read")
def digital_read(mask: int = 0xFF) -> dict:
    """Reads the true electrical level of the requested digital-bank
    channels (bit0=GP0..bit7=GP7). For an armed/driven channel this is a
    readback of what's being driven; for an input channel, the real
    level — noisy/meaningless if the channel is unconnected. Always
    allowed, a pure read."""
    return {"values": _get_device().digital_read(mask)}


# -- Digital capture ---------------------------------------------------------


@mcp.tool()
@_audited("digital_capture_start")
def digital_capture_start(rate_hz: int, max_samples: int = 0) -> dict:
    """Starts continuous periodic sampling of all 8 digital-bank channels
    into a 1024-sample ring, independent of arm state. rate_hz is clamped
    to what the hardware clock divider can reach on this board (roughly
    763Hz-200kHz, the ceiling is an unvalidated software limit, not a
    measured one). max_samples=0 runs until digital_capture_stop; a
    nonzero value is a bounded burst (capped at 1024) that completes on
    its own — poll digital_capture_read and check total_captured to see
    when it's done. Always allowed, a pure read of external signals."""
    return _get_device().digital_capture_start(rate_hz, max_samples)


@mcp.tool()
@_audited("digital_capture_read")
def digital_capture_read(capture_id: int, offset: int, limit: int) -> dict:
    """Pages out up to `limit` samples (each one byte, bit-per-channel)
    starting at absolute sample index `offset` from the named capture.
    overrun_count is nonzero if the ring wrapped past data you hadn't read
    yet — the read still returns the best data still available."""
    result = dict(_get_device().digital_capture_read(capture_id, offset, limit))
    result["samples_hex"] = result.pop("samples").hex()
    return result


@mcp.tool()
@_audited("digital_capture_stop")
def digital_capture_stop() -> dict:
    """Stops the active capture (or cancels a bounded one early). The
    buffer stays readable via digital_capture_read until a new
    digital_capture_start replaces it."""
    return _get_device().digital_capture_stop()


# -- ADC scan -----------------------------------------------------------------


@mcp.tool()
@_audited("adc_scan_start")
def adc_scan_start(channel_mask: int, rate_hz: int = 0, max_samples: int = 0) -> dict:
    """Starts round-robin sampling of ADC0-2 (bit0=ADC0..bit2=ADC2) into a
    2048-sample ring. rate_hz is the TOTAL round-robin rate shared across
    however many channels are selected, not a per-channel rate — with 3
    channels each is effectively sampled at rate_hz/3. Always allowed, a
    pure read."""
    return _get_device().adc_scan_config(channel_mask, rate_hz, max_samples)


@mcp.tool()
@_audited("adc_scan_stop")
def adc_scan_stop() -> dict:
    """Stops the active ADC scan."""
    return _get_device().adc_scan_config(0, 0, 0)


@mcp.tool()
@_audited("adc_scan_read")
def adc_scan_read(scan_id: int, offset: int, limit: int) -> dict:
    """Pages out up to `limit` raw 12-bit ADC codes (0-4095) starting at
    absolute sample index `offset`. Sample i belongs to channel
    (i % channel_count) of the selected channels, in ascending order.
    Convert to volts yourself: raw * vref_millivolts / 4096 / 1000
    (vref comes from adc_scan_start's response — nominal 3.3V, not
    independently calibrated per board)."""
    return _get_device().adc_scan_read(scan_id, offset, limit)


# -- PWM ------------------------------------------------------------------------


@mcp.tool()
@_audited("pwm_set")
def pwm_set(channel: int, enabled: bool, frequency_hz: int = 0, duty_permille: int = 0) -> dict:
    """Configures PWM on one digital-bank channel (0-7); that channel must
    already be armed via outputs_arm. duty_permille is 0-1000 (0.1%
    steps). GP0/1, GP2/3, GP4/5, GP6/7 each share one hardware PWM slice —
    changing frequency on one channel of a pair silently changes what its
    sibling is actually outputting too, even if untouched by this call.
    Blocked unless RP2350_SIGNAL_ALLOW_WRITES=1."""
    policy.require_writes_enabled("pwm_set")
    return _get_device().pwm_config(channel, enabled, frequency_hz, duty_permille)


# -- UART -----------------------------------------------------------------------


@mcp.tool()
@_audited("uart_config")
def uart_config(baud_rate: int, data_bits: int = 8, stop_bits: int = 1, parity: int = 0) -> dict:
    """Configures UART0 (GP16 TX / GP17 RX). parity: 0=none, 1=even,
    2=odd. Resets the RX buffer and error counters. Blocked unless
    RP2350_SIGNAL_ALLOW_WRITES=1 — this changes device state even though
    it doesn't drive a digital-bank output."""
    policy.require_writes_enabled("uart_config")
    return {"granted_baud_rate": _get_device().uart_config(baud_rate, data_bits, stop_bits, parity)}


@mcp.tool()
@_audited("uart_write")
def uart_write(data_hex: str) -> dict:
    """Sends data_hex over UART0, fire-and-forget. Rejected if a previous
    write is still draining (a large payload at a slow baud rate) — retry
    after a short wait rather than assuming failure. Blocked unless
    RP2350_SIGNAL_ALLOW_WRITES=1."""
    policy.require_writes_enabled("uart_write")
    _get_device().uart_write(bytes.fromhex(data_hex))
    return {"ok": True}


@mcp.tool()
@_audited("uart_read")
def uart_read(max_bytes: int = 480) -> dict:
    """Drains up to max_bytes from UART0's RX buffer, plus cumulative
    framing/parity/break/overrun error counters since the last
    uart_config. Always allowed, a pure read."""
    result = dict(_get_device().uart_read(max_bytes))
    result["data_hex"] = result.pop("data").hex()
    return result


# -- I2C ------------------------------------------------------------------------


@mcp.tool()
@_audited("i2c_xfer")
def i2c_xfer(address: int, write_hex: str = "", read_length: int = 0, timeout_ms: int = 0) -> dict:
    """Combined write-then-read I2C transaction (repeated start, no STOP
    in between) against i2c0 (GP20 SDA / GP21 SCL) — the common "write
    register pointer, read data back" pattern in one call. write_status/
    read_status in the response are 0=ok, 1=nack, 2=timeout: a NACK is a
    normal, expected outcome (no device at that address), not a tool
    failure — check the status fields rather than assuming success.
    Blocked unless RP2350_SIGNAL_ALLOW_WRITES=1 AND address is present in
    RP2350_SIGNAL_I2C_ALLOWLIST (comma-separated hex, e.g. '0x50,0x68') —
    gated even for a read-mostly call, since every I2C transaction writes
    at least an address byte."""
    policy.require_writes_enabled("i2c_xfer")
    policy.require_i2c_address_allowed(address)
    result = dict(_get_device().i2c_xfer(address, bytes.fromhex(write_hex), read_length, timeout_ms))
    result["data_hex"] = result.pop("data").hex()
    return result


@mcp.tool()
@_audited("i2c_slave_config")
def i2c_slave_config(enabled: bool, address: int = 0) -> dict:
    """Enables/disables this board acting as an I2C slave (i2c1, GP18
    SDA / GP19 SCL) with a 256-byte register-map responder — the first
    byte an external master writes sets a pointer, subsequent bytes
    read/write from there. Always resets the register map and counters.
    Blocked unless RP2350_SIGNAL_ALLOW_WRITES=1."""
    policy.require_writes_enabled("i2c_slave_config")
    return _get_device().i2c_slave_config(enabled, address)


# -- SPI ------------------------------------------------------------------------


@mcp.tool()
@_audited("spi_xfer")
def spi_xfer(tx_hex: str, hz: int = 100000, mode: int = 0) -> dict:
    """Full-duplex SPI transfer on the master (GP8 SCK / GP9 MOSI /
    GP10 MISO / GP11 CS0_n). Only mode=0 (CPOL=0, CPHA=0) is implemented —
    any other value is rejected. If you want more RX bytes back than you
    have real TX data for, pad tx_hex with trailing '00' bytes yourself
    (the standard spi_transfer(tx, rx, len) convention). hz is clamped to
    [10000, 1000000]. Blocked unless RP2350_SIGNAL_ALLOW_WRITES=1 — SPI has
    no read-only mode, every transfer drives MOSI/SCK/CS."""
    policy.require_writes_enabled("spi_xfer")
    result = dict(_get_device().spi_xfer(bytes.fromhex(tx_hex), hz, mode))
    result["data_hex"] = result.pop("data").hex()
    return result


@mcp.tool()
@_audited("spi_slave_config")
def spi_slave_config(enabled: bool) -> dict:
    """Enables/disables this board acting as an SPI slave (GP12 SCK /
    GP13 MOSI / GP14 MISO / GP15 CS_n), mode 0 only. MISO continuously
    repeats a fixed 0x00-0x07 pattern; MOSI is captured only for counting.
    bytes_sent in the response is an approximate pipeline-fill count, not
    an exact transmitted-byte count (reads ~5 even at idle — see
    protocol.md). Blocked unless RP2350_SIGNAL_ALLOW_WRITES=1."""
    policy.require_writes_enabled("spi_slave_config")
    return _get_device().spi_slave_config(enabled)


# -- Entry point ---------------------------------------------------------------


def main() -> None:
    global _device

    parser = argparse.ArgumentParser(prog="rp2350-signal-mcp")
    parser.add_argument(
        "--device",
        metavar="SERIAL",
        default=None,
        help="Serial number of the board to control (required if more than one is attached)",
    )
    args = parser.parse_args()

    try:
        _device = Device(serial=args.device)
    except DeviceError as e:
        # stdout is the JSON-RPC channel -- diagnostics only ever go to stderr.
        print(f"rp2350-signal-mcp: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"rp2350-signal-mcp: connected to {_device.serial}", file=sys.stderr)
    try:
        mcp.run()
    finally:
        _device.close()


if __name__ == "__main__":
    main()
