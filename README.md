# Agent Fingers

Free for personal and non-commercial use. If this is useful to you,
consider [buying me a coffee](https://buymeacoffee.com/redwolfelectronics).

An AI-agent-controllable digital-signal instrument on a stock **Raspberry
Pi Pico 2** — no custom board. An 8-channel bidirectional digital I/O
bank, 3-channel ADC, PWM, UART, I2C (master + slave), and SPI
(master + slave), all reachable from Claude Code (or any MCP client)
over USB via a real firmware protocol — not simulated.

This doc gets you from a blank Pico 2 to a working MCP connection as fast
as possible. For everything else, see:
- **`userguide.md`** — full peripheral-by-peripheral usage, pinout,
  troubleshooting.
- **`protocol.md`** — wire protocol spec.
- **`handoff.md`** — build status, verification status per peripheral,
  known issues, real bugs found during hardware bring-up.
- **`rp2350_digital_signal_agent_plan.md`** — design background and
  rationale.

## What you need

- A Raspberry Pi Pico 2 (non-W).
- A Raspberry Pi Debug Probe (or a second Pico running debugprobe
  firmware) for flashing, wired to the target's SWD pins.
- The Raspberry Pi Pico VSCode extension installed — it provides the
  SDK/toolchain/openocd used below, no separate install needed.
- Python 3.10+ for the host tools (library, CLI, GUI, MCP server).

## 1. Flash the Pico 2

```sh
cd build
cmake -G Ninja ..
ninja
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" \
  -c "program build/agent_fingers.elf verify reset exit"
```

Don't use a Homebrew/apt-installed `openocd` if you have one — it's a
different build without RP2350 support. Use the `.pico-sdk` one above.

No debug probe handy? Hold BOOTSEL while plugging the Pico in, it mounts
as a USB drive — drag `build/agent_fingers.uf2` onto it instead.

The onboard LED blinks fast (200ms) while USB is unconfigured, slow (1s)
once the host has enumerated it — a no-terminal-needed check that
firmware booted and USB came up.

## 2. Install the host tools

```sh
host/setup.sh
```

Creates a venv at `host/.venv`, installs the device library/CLI/MCP
server in editable mode. Re-run anytime after pulling new commits.

Confirm the board answers:

```sh
host/.venv/bin/rp2350-signal-cli list     # attached device serials
host/.venv/bin/rp2350-signal-cli info     # firmware version, board ID
```

## 3. Connect the MCP server to Claude Code

```sh
claude mcp add rp2350-signal /absolute/path/to/host/.venv/bin/rp2350-signal-mcp -- --device YOUR_SERIAL
```

`--device` is optional with exactly one board attached; get the serial
from `rp2350-signal-cli list` once you have more than one. Use an
**absolute path** — Claude Code doesn't activate your shell's venv.

Every write-capable tool (arming outputs, digital/PWM/UART/I2C/SPI
writes, slave-mode config) is **disabled by default**, independent of
the firmware's own arm/lease enforcement. To allow writes, add an `env`
block instead of using `claude mcp add` directly — edit your MCP config
(`.mcp.json` or Claude Code settings) to:

```json
{
  "mcpServers": {
    "rp2350-signal": {
      "command": "/absolute/path/to/host/.venv/bin/rp2350-signal-mcp",
      "args": ["--device", "YOUR_SERIAL"],
      "env": {
        "RP2350_SIGNAL_ALLOW_WRITES": "1",
        "RP2350_SIGNAL_I2C_ALLOWLIST": "0x50,0x68"
      }
    }
  }
}
```

Read-only tools (status/info/digital_read/adc_scan/digital_capture/
uart_read) work with no `env` block at all. Full policy variable list in
`userguide.md`.

## 4. Optional: the GUI

A standalone PySide6 app for driving the board by hand:

```sh
host/gui/setup.sh
host/.venv/bin/rp2350-signal-gui
```

## What it can do

| Peripheral | Pins | Notes |
|---|---|---|
| Digital I/O bank | GP0-7 | Bidirectional, per-channel direction, arm/lease-gated writes, rate-paced capture into a DMA ring |
| ADC | GP26-28 (ADC0-2) | Round-robin scan, DMA ring, block-ready event on bounded scans |
| PWM | Any GP0-7 channel | Reuses the digital bank's arm/lease, no separate claim opcode |
| UART | GP16 (TX) / GP17 (RX) | Configurable baud/format, DMA RX ring, fire-and-forget TX |
| I2C | Master: GP20/21 · Slave: GP18/19 | Master does combined write-then-read with NACK/timeout reporting + auto bus-recovery; slave is a 256-byte IRQ-driven register-map responder |
| SPI | Master: GP8-11 · Slave: GP12-15 | PIO-based (not hardware SPI0/1) for pin-placement flexibility; Mode 0 only |

Every write path is gated: firmware-level arm/lease for digital/PWM
outputs, plus an independent MCP policy layer (deny-by-default,
env-var-controlled) covering every write-capable tool including buses the
firmware itself doesn't gate.

## Speed / specs

| Spec | Value |
|---|---|
| Digital capture rate | up to 200,000 Hz (8 channels, 1024-sample ring) |
| ADC scan rate | up to 200,000 Hz total, round-robin across up to 3 channels (2048-sample ring) |
| PWM frequency | up to 100,000 Hz (software ceiling, not a hardware limit) |
| SPI clock | 10,000-1,000,000 Hz (software-clamped safety floor, not a hardware limit) |
| I2C | fixed 100 kHz standard mode |
| USB | full-speed (12 Mbit/s), vendor-bulk transport, custom framed binary protocol |
| Wire frame | 20-byte header + up to 16,384-byte payload + 4-byte CRC32C; 512 bytes buffered on-device, larger declared lengths are skip-discarded cleanly |
| Firmware footprint | ~48 KB flash (.text), ~18 KB RAM (.bss) |

All rate ceilings above are **provisional software limits chosen for
safety margin** (bounding worst-case block time under the watchdog
window, or avoiding an unverified hardware claim), not measured hardware
maximums — see `protocol.md` for the reasoning behind each one.

## Hardware-verified status

Every peripheral above has been exercised against a real, physically
wired Pico 2 this project — not just compiled and assumed correct. Full
per-peripheral verification status (what's proven, what's still
outstanding, and every real bug found and fixed along the way) is in
`handoff.md`.
