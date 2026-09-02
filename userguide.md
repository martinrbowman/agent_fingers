# User guide

How to build, flash, and check the Agent Fingers firmware on a
Raspberry Pi Pico 2. For design background see
`rp2350_digital_signal_agent_plan.md`; for wire-protocol details see
`protocol.md`.

## What you need

- Raspberry Pi Pico 2 (non-W), connected over USB for the device itself.
- Raspberry Pi Debug Probe (or a second Pico running debugprobe firmware),
  wired to the target's SWD pins, connected over USB for programming.
- Raspberry Pi Pico VSCode extension installed (this is what provides the
  SDK/toolchain/openocd used below — no other setup needed).

## Build

```sh
cd build
cmake -G Ninja ..
ninja
```

Output: `build/agent_fingers.elf` / `.uf2`.

## Flash

Via the debug probe (recommended — also lets you catch a crash with a
debugger later):

```sh
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" \
  -c "program build/agent_fingers.elf verify reset exit"
```

Don't use `/opt/homebrew/bin/openocd` if you have it installed — it's a
different build without RP2350 support. Use the `.pico-sdk` one above.

Alternative without a debug probe: hold BOOTSEL while plugging in the Pico,
it mounts as a USB drive, drag `build/agent_fingers.uf2` onto it.

## Checking it's alive

After flashing, the board enumerates as a composite USB device (VID 0xCafe,
PID 0x4001 — a development placeholder, not final) with two interfaces:

1. **A CDC serial port** (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on
   Linux, a COM port on Windows) — open it with any terminal program. You'll
   see one line on connect:
   ```
   agent_fingers 0.1.0 (<git hash>)
   ```
   This port is diagnostic-only; it doesn't speak the binary protocol.

2. **A vendor-specific bulk interface** — this is where the real control
   protocol lives (see `protocol.md`). It has no OS-level device file; you
   need a USB library (pyusb, libusb, or the future host client) to talk to
   it. There's no host client yet (that's step 7 of the plan), but a quick
   manual check with Python + pyusb:
   ```python
   import usb.core, usb.util
   dev = usb.core.find(idVendor=0xCafe, idProduct=0x4001)
   intf = dev.get_active_configuration()[(2, 0)]
   usb.util.claim_interface(dev, intf.bInterfaceNumber)
   # build a GET_INFO frame per protocol.md and write to EP 0x03,
   # read the response from EP 0x83
   ```

The onboard LED blinks fast (200ms) while USB is unconfigured, slow (1s)
once the host has enumerated the device — a quick visual check that
firmware booted and USB came up, no terminal needed.

## Pinout (Pico 2, GP0-28)

Frozen allocation — see `hardware/pin_claims.yaml` (machine-readable source
of truth) and `rp2350_digital_signal_agent_plan.md` for the full rationale.
`GP23-25`/`GP29` are board functions, not header pins to wire up.

| GPIO    | Function                | Status |
|---------|--------------------------|--------|
| GP0-GP7 | Digital I/O bank (bidirectional, per-channel, PWM-capable) | **working** — see below |
| GP8     | SPI master SCK          | mode 0 only, data transfer verified (master↔slave loopback); self-loop not yet — see below |
| GP9     | SPI master MOSI         | mode 0 only, data transfer verified (master↔slave loopback); self-loop not yet — see below |
| GP10    | SPI master MISO         | mode 0 only, data transfer verified (master↔slave loopback); self-loop not yet — see below |
| GP11    | SPI master CS0_n        | mode 0 only, data transfer verified (master↔slave loopback); self-loop not yet — see below |
| GP12    | SPI slave SCK           | mode 0 only, **working**, data transfer verified (master↔slave loopback) — see below |
| GP13    | SPI slave MOSI          | mode 0 only, **working**, data transfer verified (master↔slave loopback) — see below |
| GP14    | SPI slave MISO          | mode 0 only, **working**, data transfer verified (master↔slave loopback) — see below |
| GP15    | SPI slave CS_n          | mode 0 only, **working**, data transfer verified (master↔slave loopback) — see below |
| GP16    | UART TX                 | **working**, fully verified (loopback round-trip) — see below |
| GP17    | UART RX                 | **working**, fully verified (loopback round-trip) — see below |
| GP18    | I2C slave SDA           | **working**, data transfer verified (loopback round-trip); bus-timeout recovery not yet — see below |
| GP19    | I2C slave SCL           | **working**, data transfer verified (loopback round-trip); bus-timeout recovery not yet — see below |
| GP20    | I2C master SDA          | **working**, data transfer verified (loopback round-trip); bus-timeout recovery not yet — see below |
| GP21    | I2C master SCL          | **working**, data transfer verified (loopback round-trip); bus-timeout recovery not yet — see below |
| GP22    | Debug/scope trigger     | not implemented |
| GP23-25 | *board function — do not use* | n/a |
| GP26    | ADC0                     | **working** — see below |
| GP27    | ADC1                     | **working** — see below |
| GP28    | ADC2                     | **working** — see below |
| GP29    | *board function (VSYS sense) — do not use* | n/a |

**GP0-7 and GP26-28 are unconnected on a bare board.** An unconnected
channel reads as noise, not a clean value — GP0-7 has a weak internal
pull-down that's easily overridden by floating-pin coupling; GP26-28 (ADC)
has no pull at all and will show a wandering raw code with nothing wired to
it. Don't trust a read from a channel with nothing connected.

## What the device can do right now

Every opcode in the plan is now implemented: `GET_INFO`, `GET_CAPABILITIES`,
`GET_STATUS`, `PING`, `ARM_OUTPUTS`, `DISARM_OUTPUTS`, `DIGITAL_READ`,
`DIGITAL_WRITE_MASKED`, `DIGITAL_CAPTURE_START`, `DIGITAL_CAPTURE_STOP`,
`DIGITAL_CAPTURE_READ`, `ADC_SCAN_CONFIG`, `ADC_SCAN_READ`, `PWM_CONFIG`,
`UART_CONFIG`, `UART_WRITE`, `UART_READ`, `I2C_XFER`, `I2C_SLAVE_CONFIG`,
`SPI_XFER`, `SPI_SLAVE_CONFIG` — the last two mode-0-only and not yet
verified against real hardware, see below. Step 6 (peripherals) is
code-complete; step 7 (host library + MCP server) hasn't started.

Firmware size, for reference: ~46 KB flash (1.1% of the RP2350's 4 MB),
~17 KB RAM (3.3% of 520 KB) — all static allocation, no headroom pressure
at this point in the build.

### Using the digital bank (GP0-7)

All 8 channels boot as input/high-Z. Driving any of them requires arming
first — this is a deliberate safety gate, not red tape (see plan's "Safety
policy"):

1. `GET_STATUS` → read the 16-byte `arm_challenge` field.
2. `ARM_OUTPUTS` with that exact challenge, an `output_mask` (which of
   GP0-7 to claim as output), and a `lease_ms` (defaults to 2000 if 0,
   clamped to [100, 30000]).
3. `DIGITAL_WRITE_MASKED` to drive values — only on channels within the
   armed mask.
4. `PING` periodically to renew the lease, or it expires and the bank snaps
   back to input/high-Z on its own (`safe_state_reason` becomes
   `host_lost` in the next `GET_STATUS`).
5. `DISARM_OUTPUTS` when done — no challenge needed, always safe to call.

`DIGITAL_READ` works regardless of arm state and returns the true pin
level for every requested channel (for an armed/driven channel, that's a
readback of what you're driving — for an input channel, the real
electrical level, noise included).

### PWM on a digital-bank channel

No separate claim step — PWM reuses the arm/lease above directly:

1. Arm the channel via `ARM_OUTPUTS` (above) like any other output.
2. `PWM_CONFIG(channel, enabled=1, frequency_hz, duty_permille)` — channel
   must be in the armed mask or this is rejected. `duty_permille` is 0-1000
   (0.1% steps).
3. `DIGITAL_WRITE_MASKED` on a PWM-active channel is rejected — the pad is
   muxed to the PWM peripheral now, not the digital bank. Disable PWM first
   (`PWM_CONFIG` with `enabled=0`) to get plain digital control back.
4. GP0/1, GP2/3, GP4/5, GP6/7 each share one PWM slice's frequency — only
   duty is independent per pin in a pair. Changing frequency on one member
   of a pair silently changes what its sibling is actually outputting too,
   even without touching the sibling directly. Give paired channels the
   same frequency if you need both driven independently.
5. Losing the arm lease (expiry, disarm, `PING` not sent in time, any
   safe-state trigger) disables PWM and releases the pin, same as any other
   armed output — verified on hardware, no stuck-driving state.

Full wire layout for these opcodes is in `protocol.md`.

### Capturing digital samples

Continuous periodic sampling of all 8 channels, independent of arm state —
works whether or not anything is armed as output, and can run at the same
time as `DIGITAL_READ`/`DIGITAL_WRITE_MASKED`.

1. `DIGITAL_CAPTURE_START` with a `rate_hz` (clamped to what's achievable at
   the board's actual clock, up to 200000 Hz — provisional, not yet
   validated against a logic analyzer) and `max_samples` (0 = runs until you
   stop it; otherwise a bounded burst, capped at the 1024-sample ring).
   Response gives you a `capture_id` — every following call must echo it.
2. Poll `DIGITAL_CAPTURE_READ` with an `offset`/`limit` to page through
   samples as they arrive. `offset` is an absolute sample index since START,
   not a ring position — the wraparound is handled for you.
3. For a bounded capture, an unsolicited EVENT frame arrives when it
   auto-completes (`sequence=0` — that's the general marker for a frame
   with no originating request). No need to poll just to find out if it's done.
4. `DIGITAL_CAPTURE_STOP` ends an unbounded capture (or cancels a bounded
   one early). The buffer stays readable afterward until a new START
   replaces it.
5. If you read too slowly and the 1024-sample ring wraps past your last
   read position, `DIGITAL_CAPTURE_READ`'s `overrun_count` tells you how
   many samples were lost — the read still returns the best data still
   available rather than failing outright.

### Reading the ADC (GP26-28)

Same shape as digital capture, deliberately — round-robin sampling into a
ring buffer, paged out via READ. No arming needed; this is a pure read
path, nothing is ever driven.

1. `ADC_SCAN_CONFIG` with a `channel_mask` (bits 0-2 = ADC0/1/2) and a
   `rate_hz` — this is the *total* round-robin rate, not per-channel: with
   3 channels selected, each individual channel is sampled at rate_hz/3.
   `max_samples` works the same as digital capture (0 = unbounded, else a
   bounded burst capped at 2048). Response includes `vref_millivolts`
   (nominal 3.3V — not independently calibrated, there's no separate
   precision reference on this board) — convert a raw code to volts
   yourself: `volts = raw_code * vref_millivolts / 4096 / 1000`.
2. Poll `ADC_SCAN_READ` with `offset`/`limit` to page through raw 12-bit
   codes. Because each sample is 2 bytes (vs 1 for digital capture), a
   512-byte frame only fits 242 samples per call, not 484 — page accordingly.
3. Sample order is fixed: sample `i` belongs to the `(i % channel_count)`-th
   selected channel in ascending order (0, 1, 2, 0, 1, 2, ...) — not tagged
   per-sample, derive it from the index.
4. `ADC_SCAN_CONFIG` with `channel_mask=0` stops the scan — there's no
   separate stop opcode.
5. Same overrun/bounded-completion/EVENT behavior as digital capture (the
   EVENT here carries `opcode=ADC_SCAN_READ` instead).

### Using UART0 (GP16 TX / GP17 RX)

Not gated by `ARM_OUTPUTS` — it's a communication bus, not a static output
level, so no arming needed.

1. `UART_CONFIG(baud_rate, data_bits, stop_bits, parity)` first —
   `UART_WRITE`/`UART_READ` before this return an error. Reconfiguring
   resets the RX ring, read cursor, and error counters, and aborts any
   in-flight TX.
2. `UART_WRITE(data)` — the whole request payload is the bytes to send, no
   wrapper fields. It's fire-and-forget: if a previous write's DMA
   transfer is still draining (e.g. a big payload at a slow baud rate),
   this is rejected rather than blocking the device or queuing — wait and
   retry, or check via `UART_READ`.
3. `UART_READ(max_bytes)` drains whatever's arrived since the last read —
   no offset, it's a plain stream, not a paginated capture. The response
   also carries cumulative framing/parity/break/overrun/ring-overrun error
   counters, reset on the next `UART_CONFIG`.
4. **Fully verified**, including actual byte-level TX/RX transfer: with a
   GP16-to-GP17 loopback jumper, all 256 byte values round-trip
   byte-identical with zero framing/parity/break/overrun errors
   (`test_loopback_round_trip`). Also verified: pre-config rejection,
   busy-rejection on a slow/large write, counters idle at zero,
   counters/cursor reset on reconfigure.

### Using I2C: master (GP20/21) and slave (GP18/19)

Two independent roles on separate hardware — both can be active
simultaneously. Neither is gated by `ARM_OUTPUTS`; both are buses. Fixed
100kHz for now.

**As master** (talking to an external I2C device):

1. `I2C_XFER(address, write_len, read_len, timeout_ms, <write bytes>)` — a
   combined write-then-read using a repeated start, covering the common
   "write register pointer, read data back" pattern in one call.
2. The opcode itself succeeds even if the I2C transaction fails — check
   `write_status`/`read_status` in the response (`0`=ok, `1`=nack,
   `2`=timeout). A NACK just means no device answered at that address;
   that's a normal, expected outcome, not a protocol error.
3. If the write phase fails, the read phase is skipped entirely — check
   `write_status` before trusting `bytes_read`.
4. A genuine timeout (not NACK) automatically runs a bus-recovery sequence
   (GPIO bit-bang, up to 9 SCL pulses) before the response comes back, so
   the bus is left clean for your next call either way.

**As slave** (this board answers as an I2C device on the bus):

1. `I2C_SLAVE_CONFIG(enabled=1, address)` sets up a 256-byte register-map
   responder: whatever writes it, the first byte becomes a pointer
   (auto-incrementing from there), matching the common EEPROM/sensor
   convention — write pointer+data to store, write pointer then read to
   fetch. There's no opcode to inspect the register map's contents
   remotely; it only exists for an external master to interact with
   over the physical bus.
2. Call `I2C_SLAVE_CONFIG` again (same params or to disable) to
   fetch-and-reset `bytes_received`/`bytes_sent`/`transaction_count` —
   this also wipes the register map back to zero.

**Verified**: NACK-on-nonexistent-address, read phase correctly skipped
after a failed write, slave enable/disable/counter-reset, and — with
GP20↔GP18 (SDA) and GP21↔GP19 (SCL) jumpered together (both sides already
pull up internally, no external resistors needed) — a real
master↔slave register-map round-trip (`test_register_map_round_trip`):
write a payload, reposition the pointer, read it back byte-identical.
**Not yet verified**: genuine bus-timeout recovery — needs a deliberately
stuck bus, not just a loopback jumper.

### Using SPI: master (GP8-11) and slave (GP12-15)

Not gated by `ARM_OUTPUTS`. **Mode 0 (CPOL=0, CPHA=0) only, both roles** —
modes 1-3 return an error.

**As master:**

1. `SPI_XFER(mode=0, hz, len, <tx bytes>)` — full-duplex, one length for
   both directions. If you want more bytes back than you have real data to
   send, pad your TX buffer with `0x00` dummy bytes to reach the length you
   want — the standard `spi_transfer(tx, rx, len)` convention (same as
   Arduino's `SPI.transfer`, Linux `spidev`, etc.).
2. It's blocking — the response only comes back once the whole transfer
   (and its RX data) is ready, unlike UART's fire-and-forget writes. `hz`
   is clamped to [10000, 1000000]; the floor is a deliberate safety margin
   on response time, not a hardware limit.
3. Framing is true MSB-first on the wire, same as virtually every real SPI
   device expects.

**As slave:**

1. `SPI_SLAVE_CONFIG(enabled=1)` starts it — no address or mode field (mode
   0 is hardcoded). MISO continuously repeats a fixed 8-byte pattern
   (`0x00..0x07`) for as long as a master keeps clocking; there's no
   opcode to set custom response data. MOSI is captured only for counting
   (`bytes_received` in the response), not remotely readable.
2. Call `SPI_SLAVE_CONFIG` again (same params, or to disable) to
   fetch-and-reset `bytes_received`/`bytes_sent`/`transaction_count`.
   Note: `bytes_sent` is *not* a real transmitted-byte count — it reads a
   stable ~5 even at idle (the TX DMA pipeline gets pre-filled on enable
   regardless of real activity); only trust it as a rough "more than ~5
   bytes consumed" signal during sustained real use.
3. CS isn't gated in the slave's PIO program — it relies on the connected
   master not toggling SCK while this slave is deselected, standard
   SPI etiquette. MISO isn't tri-stated when deselected either, so this
   slave isn't meant to share a bus with other slaves. One consequence:
   if the slave's state machine happens to still be mid-loop from idle
   noise when a real transfer's first clock edge arrives, that transfer's
   captured *data* can come back shifted by exactly one bit (byte
   *counts* stay exact regardless) — happens about 1 in 8 tries. Retry
   the transfer if you hit this; a real fix means adding CS-wait framing
   to the slave's PIO program, not done yet.

**Master↔slave data transfer is fully verified** (GP8↔GP12, GP9↔GP13,
GP10↔GP14, GP11↔GP15 jumpered): the master reads back the slave's real
`0x00..0x07` pattern byte-identical, and both sides' byte counters are
exact — including for transfers whose length exactly matches the
256-byte RX ring, an edge case that found a real counting bug (see
handoff.md). This hardware bring-up found and fixed five real firmware
bugs along the way. **Not yet independently verified**: a true master
self-loop — jumper GP9↔GP10 (MOSI↔MISO), send known bytes, confirm they
read back identical. The master's own TX/RX bit path is already proven
correct via the slave round-trip, but that's not quite the same test.

## Host library, MCP server, and CLI

Everything above talks to the device with raw USB frames. There's now a
real Python layer on top: `device.Device` (typed client, one method per
opcode), an MCP server (`rp2350-signal-mcp`) for agent use, and a CLI
(`rp2350-signal-cli`) for bench/operator use — both built on the same
`Device` library.

### Install

```sh
host/setup.sh
```

Creates a venv at `host/.venv` (reused if it already exists), installs
`host/` in editable mode plus pytest, and prints usage. Re-run anytime
after pulling new commits to stay current. Custom venv location:
`RP2350_SIGNAL_VENV=/path/to/venv host/setup.sh`.

Equivalent by hand:
```sh
python3 -m venv .venv        # anywhere outside the repo is fine too
.venv/bin/pip install -e host/
```

Either way this pulls in `pyusb` and the official `mcp` SDK (2.x) and
installs both console scripts into the venv's `bin/`.

### CLI

```sh
.venv/bin/rp2350-signal-cli list          # attached device serials
.venv/bin/rp2350-signal-cli info
.venv/bin/rp2350-signal-cli status
.venv/bin/rp2350-signal-cli arm 0x03 --lease-ms 5000
.venv/bin/rp2350-signal-cli digital-write 0x01 0x01
.venv/bin/rp2350-signal-cli digital-read
.venv/bin/rp2350-signal-cli disarm
.venv/bin/rp2350-signal-cli i2c-xfer 0x50 --write-hex 00 --read-length 4
.venv/bin/rp2350-signal-cli spi-xfer aabbccdd
```

Pass `--device SERIAL` (before the subcommand) to select a specific board
if more than one is attached — the library refuses to silently pick one
for you. Run `rp2350-signal-cli <subcommand> --help` for full options; not
every opcode has a subcommand yet (digital capture and ADC scan aren't
wired into the CLI, only the MCP server — reach them via a short Python
script using `device.Device` directly if you need them from the bench).

### Claude Code (MCP)

The MCP server exposes 22 tools (device info/status, digital I/O, capture,
ADC, PWM, UART, I2C, SPI) over stdio. **Every write-capable tool is
disabled by default** — this is a deliberate MCP-layer policy, separate
from and in addition to the firmware's own arm/lease enforcement (which
only covers digital/PWM outputs; UART/I2C/SPI aren't gated by the
firmware at all, only by this policy):

```sh
claude mcp add rp2350-signal /absolute/path/to/.venv/bin/rp2350-signal-mcp -- --device YOUR_SERIAL
```

Or add it manually to your MCP config (`.mcp.json` or Claude Code's
settings, depending on scope):

```json
{
  "mcpServers": {
    "rp2350-signal": {
      "command": "/absolute/path/to/.venv/bin/rp2350-signal-mcp",
      "args": ["--device", "YOUR_SERIAL"],
      "env": {
        "RP2350_SIGNAL_ALLOW_WRITES": "1",
        "RP2350_SIGNAL_I2C_ALLOWLIST": "0x50,0x68"
      }
    }
  }
}
```

`--device` is optional if exactly one board is attached, but always pass
it once you have more than one — get the serial from `rp2350-signal-cli
list` or `device_info`. Use an **absolute path** to the venv's binary;
Claude Code doesn't activate your shell's venv for you.

**Policy environment variables** (set in the `env` block above, not as
shell exports — the MCP client launches this as a fresh subprocess):
- `RP2350_SIGNAL_ALLOW_WRITES=1` — required for any tool that changes
  device state (arming, digital/PWM/UART/I2C/SPI writes, slave config).
  Omit it to run fully read-only (status/info/digital_read/adc_scan/
  digital_capture/uart_read all still work).
- `RP2350_SIGNAL_I2C_ALLOWLIST=0x50,0x68` — comma-separated hex addresses
  `i2c_xfer` is permitted to talk to. Required in addition to the write
  gate; an address not listed here is blocked even with writes enabled.
- `RP2350_SIGNAL_AUDIT_LOG=/path/to/file.jsonl` — overrides the audit log
  location (default `~/.rp2350-signal-agent/audit.jsonl`). One JSON line
  per tool call: timestamp, tool name, parameters, and outcome.

Diagnostics go to stderr only — stdout is the JSON-RPC channel and must
stay clean, so don't route `print()` output there if you extend this server.

## GUI

A standalone PySide6 app for driving the board by hand -- generate and
capture signals on every bus without writing Python.

```sh
host/gui/setup.sh
host/.venv/bin/rp2350-signal-gui
```

Pick your board from the device dropdown, hit Connect. The top bar arms
GP0-7 as outputs (pick a mask + lease, hit Arm) -- do this before writing
digital pins or configuring PWM, same challenge/lease rule as everywhere
else in this project. One tab per bus:

- **Digital I/O** -- per-pin write/read, plus a rate-paced capture with a
  live waveform plot. Set a sample rate and max-sample count (0 = runs
  until you hit Stop), click Start.
- **PWM** -- pick a channel (must already be armed as output), frequency,
  duty cycle.
- **UART** -- configure baud/format, write and read hex payloads.
- **I2C** -- master transfers (address/write-data/read-length) and
  slave-mode enable with live byte/transaction counters.
- **SPI** -- master transfers and slave-mode enable, Mode 0 only
  (matches the firmware -- there's no mode selector to get wrong).
- **ADC** -- round-robin scan across ADC0-2 with a live millivolts plot.

## Running the test suite against your board

```sh
host/setup.sh
host/.venv/bin/pytest tests/integration/ --device YOUR_SERIAL
```

`--device` is optional if only one board is plugged in. 51 tests exercise
every opcode plus a malformed-frame fuzz campaign; takes under 20s.
`-m "not slow"` skips the cycling/stress tests for a quick pass. Needs an
exclusive USB claim on the device — close the MCP server, CLI, or any
other client first, or the run will fail to connect.

## Troubleshooting

- **`openocd` says "Error: Unknown target type rp2350"** — you're running
  the Homebrew openocd, not the `.pico-sdk` one. Use the full path above.
- **No `/dev/cu.usbmodem*` appears** — check the debug probe actually
  flashed successfully (`** Verified OK **` in the openocd output), and that
  the Pico 2's own USB cable (not just the probe's) is connected — the
  target board needs its own USB connection to enumerate as a device.
- **Device enumerates but doesn't respond on the vendor interface** — make
  sure nothing else (another terminal, another script) already has the
  vendor interface claimed; only one client at a time.
- **`DeviceBusyError: device ... is already locked by another process`** —
  the CLI, MCP server, or a leftover script already holds the exclusive
  lock. Only one `Device`/`Transport` connection is allowed per serial at a
  time (a deliberate safety property, not a bug); close the other one first.
- **MCP tool calls fail with a policy error even though you set the env
  vars** — env vars in an MCP config's `env` block only apply to *that*
  subprocess; setting them in your shell before running `claude` does
  nothing, the client launches the server fresh. Double-check they're in
  the `env` object of the server's config entry.
- **`rp2350-signal-mcp: no device found`** on stderr right after Claude
  Code starts it — same causes as the CDC/vendor-interface issues above,
  plus: check the `--device` serial matches exactly (`rp2350-signal-cli
  list`), and that you're pointing at the venv's binary, not a
  same-named script elsewhere on PATH.
