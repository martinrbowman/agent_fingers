# Agent Fingers — User guide

Agent Fingers turns a stock Raspberry Pi Pico 2 into a USB signal
instrument and debug probe. This guide covers setting it up, every
feature and how to use it — from Claude Code (MCP), the command line, the
desktop GUI or Python — and how to use the board as a CMSIS-DAP debug
probe.

- [What you need](#what-you-need)
- [Build and flash](#build-and-flash)
- [Install the host tools](#install-the-host-tools)
- [Check the board is alive](#check-the-board-is-alive)
- [Pinout](#pinout)
- [Ways to control the board](#ways-to-control-the-board)
- [Safety: arming outputs](#safety-arming-outputs)
- [Digital I/O](#digital-io-gp0-7)
- [Digital capture](#digital-capture)
- [PWM](#pwm)
- [ADC](#adc-gp26-28)
- [UART](#uart-gp16-gp17)
- [I2C master and slave](#i2c-master-and-slave)
- [SPI master and slave](#spi-master-and-slave)
- [Events](#events)
- [Using the board as a debug probe](#using-the-board-as-a-debug-probe)
- [Specifications and limits](#specifications-and-limits)
- [Checking your board with the test suite](#checking-your-board-with-the-test-suite)
- [Troubleshooting](#troubleshooting)

## What you need

- A Raspberry Pi Pico 2 (non-W), connected to your computer over USB.
- To flash it, either:
  - a Raspberry Pi Debug Probe (or a second Pico running debugprobe
    firmware) wired to the Pico 2's 3-pin DEBUG connector, or
  - nothing extra: use the BOOTSEL button and the UF2 file.
- The Raspberry Pi Pico VS Code extension, which installs the SDK,
  toolchain and OpenOCD used below.
- Python 3.10 or newer for the host tools.

## Build and flash

Build:

```sh
cd build
cmake -G Ninja ..
ninja
cd ..
```

This produces `build/agent_fingers.elf` and `build/agent_fingers.uf2`.

**Flash with the Debug Probe:**

```sh
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -c "cmsis-dap vid_pid 0x2e8a 0x000c" \
  -f target/rp2350.cfg \
  -c "adapter speed 5000" \
  -c "program build/agent_fingers.elf verify reset exit"
```

- Use the OpenOCD from `~/.pico-sdk`, not a Homebrew or apt one — those
  don't support the RP2350.
- The `cmsis-dap vid_pid` line selects the Raspberry Pi Debug Probe. The
  board is itself a CMSIS-DAP probe, and without that line OpenOCD may
  pick the board instead of the probe that flashes it.
- In VS Code, the Flash and Debug tasks already do this through
  `tools/openocd/debugprobe.cfg`.

**Flash without a probe:** hold BOOTSEL while plugging the Pico 2 in. It
appears as a USB drive; copy `build/agent_fingers.uf2` onto it.

## Install the host tools

```sh
host/setup.sh
```

This creates a Python virtual environment at `host/.venv` and installs the
Python library, the command-line tool and the MCP server into it. Re-run
it after pulling new versions. To put the environment elsewhere:
`RP2350_SIGNAL_VENV=/path/to/venv host/setup.sh`.

For the GUI as well:

```sh
host/gui/setup.sh
```

## Check the board is alive

- **LED:** blinks fast (every 200 ms) until the computer has set up USB,
  then slowly (every second).
- **Command line:**
  ```sh
  host/.venv/bin/rp2350-signal-cli list     # attached boards (serial numbers)
  host/.venv/bin/rp2350-signal-cli info     # firmware version and board ID
  ```
- **Diagnostic console:** the board also appears as a serial port
  (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux, a COM port on
  Windows). Open it in any terminal program to see:
  ```
  agent_fingers 0.1.0 (<git hash>)
  debug probe: SWD max 6818 kHz, JTAG max 4687 kHz (bit-bang, measured)
  ```
  The console is information only; it doesn't accept commands.

The board is a USB composite device (VID `0xCAFE`, PID `0x4001`, a
development ID) with three functions: the diagnostic serial console, the
control channel used by the host tools, and a CMSIS-DAP v2 debug probe.

**Windows:** no driver installation or Zadig step is needed; the board
tells Windows to use its built-in WinUSB driver. (Not yet tried on a
Windows machine.)

**Reset cause after flashing:** after a flash through OpenOCD, the
board's status shows reset cause 2 ("core1 stall"). This is expected:
OpenOCD resets the RP2350's two cores one after the other, and the
firmware reboots the chip to recover cleanly.

## Pinout

| GPIO (header pin) | Function |
|---|---|
| GP0-GP7 (1, 2, 4-7, 9, 10) | Digital I/O bank, channels 0-7 (input/output per channel, PWM-capable) |
| GP8 (11) | SPI master SCK · debug SWCLK / TCK |
| GP9 (12) | SPI master MOSI · debug SWDIO / TDI |
| GP10 (14) | SPI master MISO · debug SWO / TDO |
| GP11 (15) | SPI master CS · debug SWD nRESET / JTAG TMS |
| GP12 (16) | SPI slave SCK |
| GP13 (17) | SPI slave MOSI |
| GP14 (19) | SPI slave MISO |
| GP15 (20) | SPI slave CS |
| GP16 (21) | UART TX |
| GP17 (22) | UART RX |
| GP18 (24) | I2C slave SDA |
| GP19 (25) | I2C slave SCL |
| GP20 (26) | I2C master SDA |
| GP21 (27) | I2C master SCL |
| GP22 (29) | Debug JTAG nRESET |
| GP26 (31) | ADC channel 0 |
| GP27 (32) | ADC channel 1 |
| GP28 (34) | ADC channel 2 |
| GP23-25, GP29 | Used by the Pico 2 board itself — don't connect |

GP8-11 belong to the SPI master normally, and to the debug probe while a
debug session is open.

**Electrical notes**
- 3.3 V logic only. Don't connect 5 V signals directly.
- Unconnected inputs float. GP0-7 have a weak internal pull-down, but the
  Pico 2's RP2350 (A2 revision) has a known issue (erratum E9) where an
  input can stick at about 2.2 V after being driven high. The firmware
  avoids it by keeping each input switched off except while it's being
  read, so outputs you release drop back low. If a channel must read a
  reliable low with nothing driving it, fit an external pull-down of
  8.2 kΩ or less.
- The ADC inputs have no pull at all and show a wandering value with
  nothing connected.

## Ways to control the board

Every feature is available four ways. They all use the same USB control
channel, and only one program can hold it at a time.

**Claude Code (MCP).** Add the MCP server with an absolute path to the
virtual environment's binary:

```sh
claude mcp add rp2350-signal /absolute/path/to/host/.venv/bin/rp2350-signal-mcp -- --device YOUR_SERIAL
```

`--device` can be left out when only one board is attached. Write-capable
tools are disabled by default; to enable them, configure the server with
an `env` block in your MCP config (`.mcp.json` or Claude Code settings):

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

| Setting | Effect |
|---|---|
| `RP2350_SIGNAL_ALLOW_WRITES=1` | Enables the write-capable tools. Without it the server is read-only. |
| `RP2350_SIGNAL_I2C_ALLOWLIST=0x50,0x68` | I2C addresses `i2c_xfer` may talk to. Required in addition to the write setting. |
| `RP2350_SIGNAL_AUDIT_LOG=/path/file.jsonl` | Where the audit log goes (default `~/.rp2350-signal-agent/audit.jsonl`). One JSON line per tool call. |

These must be in the server's `env` block — setting them in your shell
has no effect, because the MCP client starts the server itself.

The 23 tools:

| Area | Always available | Need `ALLOW_WRITES` |
|---|---|---|
| Device | `device_info`, `device_capabilities`, `device_status`, `device_ping` | |
| Digital I/O | `digital_read`, `outputs_disarm` | `outputs_arm`, `digital_write` |
| Capture | `digital_capture_start`, `digital_capture_read`, `digital_capture_stop` | |
| ADC | `adc_scan_start`, `adc_scan_read`, `adc_scan_stop` | |
| PWM | | `pwm_set` |
| UART | `uart_read` | `uart_config`, `uart_write` |
| I2C | | `i2c_xfer`, `i2c_slave_config` |
| SPI | `spi_slave_read` | `spi_xfer`, `spi_slave_config` |

**Command line.** `host/.venv/bin/rp2350-signal-cli` has these commands:
`list`, `info`, `status`, `ping`, `arm`, `disarm`, `digital-write`,
`digital-read`, `pwm`, `uart-config`, `uart-write`, `uart-read`,
`i2c-xfer`, `spi-xfer`. Put `--device SERIAL` before the command when more
than one board is attached, and use `<command> --help` for options.
Digital capture, ADC scans and the slave modes aren't in the CLI yet — use
MCP, the GUI or Python for those.

```sh
rp2350-signal-cli arm 0x03 --lease-ms 5000
rp2350-signal-cli digital-write 0x01 0x01
rp2350-signal-cli digital-read
rp2350-signal-cli i2c-xfer 0x50 --write-hex 00 --read-length 4
rp2350-signal-cli spi-xfer aabbccdd
```

**GUI.** `host/.venv/bin/rp2350-signal-gui`. Pick the board from the
dropdown and click Connect. The top bar arms outputs (mask and lease).
There's a tab per feature: Digital I/O (read/write plus capture with a
live waveform), PWM, UART, I2C, SPI and ADC (live millivolt plot).

**Python.** The library the other three are built on:

```python
from device import Device

with Device() as dev:                      # Device(serial="...") with several boards
    print(dev.get_info())
    dev.arm_outputs(0b0000_0001, lease_ms=5000)
    dev.digital_write_masked(0b0000_0001, 0b0000_0001)   # GP0 high
    print(dev.digital_read(0xFF))
    dev.disarm_outputs()
```

Errors from the board raise `device.DeviceStatusError`, whose `.status`
says why (for example 8 = resource busy).

## Safety: arming outputs

The board never drives a pin by surprise.

- All 8 digital channels start as inputs (high impedance).
- To drive any of them — as digital outputs or PWM — you first **arm**
  them, naming which channels and for how long (the lease, 100 ms to
  30 s, 2 s by default). The host tools handle the one-time challenge
  code this needs automatically.
- Keep the lease alive with `ping` (or any tool call that pings). If it
  lapses, every output returns to input on its own.
- **Disarm** whenever you're done. It never needs permission.
- The board also drops every output to its safe state on watchdog
  timeout, host loss or a protocol fault.
- The UART, I2C and SPI buses aren't armed — they're communication links,
  not static outputs — but their MCP tools are still off unless writes are
  enabled.
- The debug probe is the one deliberate exception: a debugger drives
  GP8-11 (and GP22 in JTAG mode) directly, with no arming or MCP step,
  because debugging must work on its own. See
  [the debug probe section](#using-the-board-as-a-debug-probe).

## Digital I/O (GP0-7)

Eight channels, each an input or an output.

- **Read:** `digital_read(mask)` returns the level of the requested
  channels. Works whether or not anything is armed; for an output it
  reads back what's being driven.
- **Write:** arm the channels, then `digital_write_masked(mask, values)`
  sets the masked channels to the given levels. Writing a channel that
  isn't armed is refused.
- **Release:** `disarm_outputs()`, or let the lease run out.

## Digital capture

Samples all 8 channels at a fixed rate into a 4,096-sample buffer. It runs
independently of reads and writes, armed or not.

1. **Start:** `digital_capture_start(rate_hz, max_samples)`. The rate goes
   up to 200 kHz. `max_samples` 0 captures until you stop it; otherwise it
   takes that many samples (up to 4,096) and stops by itself. You get back
   a `capture_id`.
2. **Read:** `digital_capture_read(capture_id, offset, limit)` returns
   samples from absolute position `offset`. Each sample is one byte, bit N
   = channel N. The whole buffer fits in one read.
3. **Stop:** `digital_capture_stop()` ends a continuous capture or cancels
   a bounded one. The data stays readable until the next start.
4. A bounded capture sends a completion [event](#events) when it finishes.
5. If you read too slowly and the buffer wraps, the read reports how many
   samples were lost (`overrun_count`) and returns what's still there.

Sample times follow from the rate: sample *i* was taken at
`start_timestamp_us + i / rate_hz`.

## PWM

PWM output on any digital channel, up to 100 kHz, with duty in 0.1% steps.

1. Arm the channel.
2. `pwm_config(channel, enabled=True, frequency_hz, duty_permille)` —
   `duty_permille` is 0-1000.
3. While PWM is on, plain digital writes to that channel are refused.
   Turn PWM off (`enabled=False`) to get normal control back.

Channels pair up — GP0/1, GP2/3, GP4/5, GP6/7 — and each pair shares one
frequency. Duty is independent per channel. Changing the frequency on one
channel of a pair changes its partner's frequency too. Losing the lease
turns PWM off and releases the pin like any other output.

## ADC (GP26-28)

Three 12-bit analog inputs, sampled round-robin into a 2,048-sample
buffer. Nothing is driven, so no arming.

1. **Start:** `adc_scan_config(channel_mask, rate_hz, max_samples)` —
   bits 0-2 of the mask select ADC0-2. `rate_hz` is the total rate across
   all selected channels (up to 200 kHz), so each channel gets
   `rate_hz / channels`. `max_samples` works as for digital capture (up
   to 2,048). The reply includes `scan_id` and `vref_millivolts`.
2. **Read:** `adc_scan_read(scan_id, offset, limit)` returns raw codes
   (0-4095). Samples cycle through the selected channels in order: with
   channels 0 and 2 selected, samples go 0, 2, 0, 2, …
3. **Convert:** `volts = code * vref_millivolts / 4096 / 1000`. The
   reference is the nominal 3.3 V supply, not a calibrated reference.
4. **Stop:** `adc_scan_config` with `channel_mask=0` (or the MCP tool
   `adc_scan_stop`).

Buffer wrap and completion events work as for digital capture.

## UART (GP16, GP17)

A serial port: TX on GP16, RX on GP17.

1. **Configure:** `uart_config(baud_rate, data_bits=8, stop_bits=1,
   parity=0)` — data bits 5-8, stop bits 1-2, parity 0 = none, 1 = even,
   2 = odd. This also clears the receive buffer and error counters.
2. **Send:** `uart_write(data)` sends in the background. If a previous
   send is still going out, it's refused rather than queued — wait and
   retry.
3. **Receive:** `uart_read(max_bytes)` returns everything received since
   the last read, plus running counts of framing, parity, break and
   overrun errors, and of bytes lost because the receive buffer filled up
   between reads.

## I2C master and slave

Two independent I2C ports, usable at the same time, both at 100 kHz. The
Pico's internal pull-ups are enabled on both.

**Master (GP20 SDA, GP21 SCL)** — talk to an external I2C device:

- `i2c_xfer(address, write_data, read_len, timeout_ms)` writes, then reads
  back, in one transaction with a repeated start — the usual "set register
  pointer, read value" pattern.
- Check the result's `write_status` and `read_status`: 0 = OK, 1 = no
  acknowledge (nothing at that address — a normal outcome), 2 = timeout.
  If the write fails, the read is skipped.
- On a timeout the board automatically frees a stuck bus (clocking SCL up
  to 9 times) before replying.
- Through MCP, the address must be on `RP2350_SIGNAL_I2C_ALLOWLIST`.

**Slave (GP18 SDA, GP19 SCL)** — the board answers as an I2C device:

- `i2c_slave_config(enabled=True, address)` makes the board respond at
  that address with a 256-byte register map. The first byte a master
  writes sets the register pointer, which then auto-increments — like a
  typical EEPROM or sensor.
- Calling `i2c_slave_config` again returns byte and transaction counts
  and clears the register map.

## SPI master and slave

Two SPI ports, both **Mode 0** (clock idles low, data sampled on the rising
edge), MSB first.

**Master (GP8 SCK, GP9 MOSI, GP10 MISO, GP11 CS):**

- `spi_xfer(tx_data, hz)` is full-duplex: the reply holds as many bytes as
  you sent. To read more than you have to send, pad with `0x00`.
- Clock from 10 kHz to 1 MHz.
- The call returns when the whole transfer is done.
- While a debug session is open, the debug probe owns these pins and
  `spi_xfer` returns status 8 (resource busy) — including a transfer that
  was running when the session started. It works again when the session
  ends.

**Slave (GP12 SCK, GP13 MOSI, GP14 MISO, GP15 CS)** — the board acts as an
SPI device, which also makes it an SPI **bus sniffer**:

- `spi_slave_config(enabled=True)` starts it. Its MISO repeats the bytes
  `0x00`–`0x07` for as long as the master clocks.
- `spi_slave_read(max_bytes)` returns what the master sent on MOSI since
  the last read — for example to watch a target's SPI flash traffic while
  debugging it. The buffer holds 256 bytes; read at least that often, or
  the oldest bytes are dropped and counted in `ring_overrun_count`.
- Calling `spi_slave_config` again returns byte and transaction counts
  (a transaction = one CS falling edge) and clears the buffer. The
  `bytes_sent` figure is approximate (it reads about 5 even when idle).
- Limitations: the slave doesn't wait for CS before sampling, so
  occasionally (roughly 1 transfer in 8) a transfer's data comes back
  shifted by one bit — retry if the data looks wrong. MISO is always
  driven, so the slave can't share a bus with other slaves.

**Wiring the master to the slave for a self-test:** jumper GP8↔GP12,
GP9↔GP13, GP10↔GP14, GP11↔GP15. Never jumper GP9↔GP10 while GP10↔GP14 is
fitted — two outputs would be shorted together.

## Events

Some things happen without a request, and the board announces them:

- a bounded digital capture finished,
- a bounded ADC scan finished,
- a debug session started or ended.

From Python, `dev.poll_event(timeout)` returns the next one (or `None`);
`device.parse_debug_session_event()` decodes the debug-session ones.

## Using the board as a debug probe

The board is a CMSIS-DAP v2 debug probe for SWD and JTAG targets, with SWO
trace capture. Debug tools use it directly — no MCP, CLI or Python step
needed — and the rest of the board keeps working meanwhile, so an agent
can watch a target's signals while you step through its code.

| Feature | Support |
|---|---|
| SWD | Up to 6.8 MHz (about 266 KiB/s memory reads through OpenOCD) |
| JTAG | Up to 4.7 MHz; raw TAP access (e.g. ESP32, FPGAs) and ARM over JTAG |
| SWO trace | UART (NRZ) mode, up to 18.75 Mbaud, 4 KiB buffer |
| Target voltage | 3.3 V only (no level shifting or target-voltage sensing) |
| Reset | nRESET pin (open-drain) |
| Not supported | Manchester SWO, SWO streaming, UART-over-DAP, atomic/queued commands, target-specific reset sequences |

Requested clock rates above the limits run at the limit; the probe never
runs faster than asked.

### Tools

- **Tested:** OpenOCD (SWD, JTAG, SWO), pyOCD (SWD and ARM over JTAG),
  Espressif's `openocd-esp32` (ESP32 over JTAG).
- **Should work** (not yet tried): probe-rs, Keil µVision/MDK, IAR,
  NXP MCUXpresso, and STM32CubeIDE through its OpenOCD debug
  configuration with a custom script.
- **Won't work:** tools that only accept their own vendor's probes
  (STM32CubeProgrammer, MPLAB X, SEGGER tools).

Select the board by its USB ID so the tool doesn't pick another probe:
`cmsis-dap vid_pid 0xcafe 0x4001` in OpenOCD, `-u <board serial>` in pyOCD
(`pyocd list` shows the serial).

### Debug sessions

- A session starts when the debug tool connects. The probe then takes
  GP8-11 (and GP22 in JTAG mode) from the SPI master — immediately, even
  if an SPI transfer is running.
- It ends when the tool disconnects or exits, after 10 seconds without
  debugger traffic (e.g. a tool that crashed), or if USB is unplugged.
  The pins then go back to the SPI master.
- Everything else — digital I/O, capture, ADC, UART, I2C, SPI slave —
  keeps working during a session. Disarming outputs or a lease running out
  doesn't affect it.
- The board's status (`device_status` in MCP) shows whether a session is
  open, its mode and how the last one ended, and an [event](#events) is
  sent at the start and end.
- Between sessions the probe's pins are high impedance.

### SWD wiring and use

Take any jumpers off GP8-11 first.

| Board pin (header) | Target |
|---|---|
| GP8 (11) | SWCLK |
| GP9 (12) | SWDIO |
| GP11 (15) | nRESET |
| GP10 (14) | SWO (optional, for trace) |
| GND | GND |

OpenOCD, for an STM32F1:

```sh
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -c "cmsis-dap vid_pid 0xcafe 0x4001" \
  -c "transport select swd" -f target/stm32f1x.cfg \
  -c "reset_config srst_only srst_nogate connect_assert_srst" \
  -c "adapter speed 4000"
```

The `reset_config` line connects with the target held in reset, which is
needed when the target's firmware sleeps or reconfigures its debug pins.

pyOCD:

```sh
pyocd commander -u <board serial> -t cortex_m -O connect_mode=under-reset \
  -c "read32 0xE000ED00" -c halt -c "reg pc sp" -c go
```

pyOCD's built-in target list doesn't cover every chip (for example the
STM32F103RB); `-t cortex_m` works for halting, registers and memory.

### SWO trace

SWO needs SWD mode and the SWO wire (GP10). With OpenOCD's TPIU commands,
for an STM32F1 running on its 8 MHz reset clock:

```sh
... -c init -c "reset halt" \
    -c "stm32f1x.tpiu configure -protocol uart -output swo.bin -traceclk 8000000 -pin-freq 2000000" \
    -c "stm32f1x.tpiu enable" -c "itm port 0 on"
```

`swo.bin` receives raw ITM packets — for example `0x01` followed by the
byte, for each byte written to stimulus port 0. `-traceclk` must match the
target's actual core clock.

### JTAG wiring and use

**ESP32** (use Espressif's `openocd-esp32`, installed with ESP-IDF):

| Board pin (header) | JTAG | ESP32 (30-pin DevKit label) |
|---|---|---|
| GP8 (11) | TCK | GPIO13 (D13) |
| GP9 (12) | TDI | GPIO12 (D12) |
| GP10 (14) | TDO | GPIO15 (D15) |
| GP11 (15) | TMS | GPIO14 (D14) |
| GP22 (29) | nRESET | EN |
| GND | GND | GND |

```sh
openocd -s <openocd-esp32>/share/openocd/scripts \
  -f interface/cmsis-dap.cfg -c "cmsis-dap vid_pid 0xcafe 0x4001" \
  -c "transport select jtag" -c "set ESP32_FLASH_VOLTAGE 3.3" \
  -f target/esp32.cfg
```

- Power the ESP32 from its own USB and connect only GND between the
  boards.
- The ESP32's application must leave GPIO12-15 free — they're the JTAG
  pins. An application that uses them stops JTAG from working a moment
  after reset. ESP-IDF's `hello_world` example leaves them alone.
- GPIO12 also selects the ESP32's flash voltage at reset. The probe keeps
  TDI low whenever it isn't shifting data, and holds it low for 2 ms as it
  releases EN, so resets through the probe boot normally.

**ARM chips over JTAG** — e.g. an STM32F103 with `transport select jtag`
and `target/stm32f1x.cfg`, wired TCK→PA14, TDI→PA15, TDO←PB3, TMS→PA13,
GP22→NRST. pyOCD's JTAG mode only handles chips with a single JTAG device
in the chain, so for chips that also have a boundary-scan device (STM32F1
does) use OpenOCD, or SWD.

### Nucleo boards

To debug the Nucleo's own microcontroller with this probe, **remove both
CN2 jumpers** (they connect the on-board ST-LINK to the debug lines) and
wire to the microcontroller's pins on the morpho headers. CN4 is the
ST-LINK's output for debugging *other* boards, not a way into the
Nucleo's own chip. For the Nucleo-F103RB: SWDIO CN7-13, SWCLK CN7-15,
NRST CN7-14, SWO CN10-31, GND CN7-19.

## Specifications and limits

| | |
|---|---|
| Digital capture | Up to 200 kHz, 8 channels, 4,096-sample buffer |
| ADC | 3 channels, 12-bit, up to 200 kHz total, 2,048-sample buffer |
| PWM | Up to 100 kHz, duty in 0.1% steps; pairs share frequency |
| UART | 5-8 data bits, 1-2 stop bits, none/even/odd parity |
| I2C | 100 kHz, master and slave, internal pull-ups |
| SPI | Mode 0 only; master 10 kHz-1 MHz; slave capture buffer 256 bytes |
| Output lease | 100 ms-30 s (default 2 s) |
| Debug probe | SWD ≤ 6.8 MHz, JTAG ≤ 4.7 MHz, SWO UART ≤ 18.75 Mbaud |
| Logic level | 3.3 V |

The capture, ADC, PWM and SPI master limits are conservative settings
chosen to keep the board responsive, not hardware maximums.

**Known limitations**
- SPI supports Mode 0 only, in both roles.
- The SPI slave can occasionally return a transfer shifted by one bit
  (see [SPI](#spi-master-and-slave)).
- I2C runs at 100 kHz only.
- I2C bus recovery after a timeout hasn't been tested against a genuinely
  stuck bus.
- The digital capture rate hasn't been checked against a logic analyzer.

## Checking your board with the test suite

The repository includes a hardware test suite. It needs exclusive use of
the board, so close the MCP server, CLI and GUI first.

```sh
host/setup.sh
host/.venv/bin/pytest tests/integration/ --device YOUR_SERIAL
```

`--device` is optional with one board attached. It takes about 30 s;
`-m "not slow"` skips the longer stress tests.

Some tests need jumpers, and fail without them:
- UART: GP16↔GP17.
- I2C: GP20↔GP18 and GP21↔GP19.
- SPI master↔slave: GP8↔GP12, GP9↔GP13, GP10↔GP14, GP11↔GP15.
- SPI master loop-back: GP9↔GP10 with GP10↔GP14 removed.
- Leave GP0 unconnected.

Debug-probe tests against a real target are off unless you ask for them
(with GP8-11 free of jumpers):

```sh
AF_SWD_TARGET=stm32f1x  host/.venv/bin/pytest tests/integration/test_swd_target.py
AF_JTAG_TARGET=stm32f1x host/.venv/bin/pytest tests/integration/test_jtag_target.py
AF_JTAG_TARGET=esp32    host/.venv/bin/pytest tests/integration/test_jtag_target.py
```

## Troubleshooting

- **OpenOCD: "Unknown target type rp2350"** — you're running a Homebrew
  or apt OpenOCD. Use `~/.pico-sdk/openocd/0.12.0+dev/openocd`.
- **Flashing fails with "SWD not supported", or OpenOCD talks to the
  wrong device** — it picked the board's own debug probe instead of the
  Raspberry Pi Debug Probe. Add `-c "cmsis-dap vid_pid 0x2e8a 0x000c"`
  after `interface/cmsis-dap.cfg`.
- **No serial port or board doesn't appear** — check the flash reported
  `** Verified OK **`, and that the Pico 2's own USB cable is connected
  (the Debug Probe's cable doesn't power the Pico's USB).
- **`DeviceBusyError: ... already locked by another process`** — another
  program (CLI, MCP server, GUI, a script) is using the board. Only one
  may connect at a time; close the other one.
- **MCP tools refuse with a policy error although you set the variables**
  — they must be in the server's `env` block in the MCP config, not your
  shell.
- **`rp2350-signal-mcp: no device found`** — check the `--device` serial
  matches `rp2350-signal-cli list`, and that the config points at the
  virtual environment's binary.
- **`spi_xfer` returns status 8 (resource busy)** — a debug session owns
  GP8-11. Close the debugger; a session left open by a crashed tool closes
  itself after 10 seconds.
- **Debugger can't read the target's memory (reads stall)** — often an
  STM32 whose firmware sleeps. Connect under reset (`reset_config srst_only
  srst_nogate connect_assert_srst`, with nRESET wired).
- **ESP32 JTAG scan reads all ones** — its application is using GPIO12-15.
  Flash one that leaves them free.
- **Debugger can't find the microcontroller on a Nucleo board** — remove
  the CN2 jumpers and wire to the morpho-header pins, not CN4.
- **An unconnected digital input reads 1** — floating input; see the
  electrical notes under [Pinout](#pinout).
