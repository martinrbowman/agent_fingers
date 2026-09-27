# Agent Fingers

Free for personal and non-commercial use. If this is useful to you,
consider [buying me a coffee](https://buymeacoffee.com/redwolfelectronics).

An AI-agent-controllable digital-signal instrument and debug probe on a
stock **Raspberry Pi Pico 2** — no custom board. Drive and measure real
signals from Claude Code (or any MCP client), a CLI, a desktop GUI, or
Python, and debug STM32 and ESP32 targets with standard tools — all over
one USB cable.

## Features

**Signal instrument**
- 8-channel bidirectional digital I/O with per-channel direction and
  rate-paced capture up to 200 kHz
- 3-channel ADC, round-robin scan up to 200 kHz
- PWM on any digital channel, up to 100 kHz
- UART with configurable baud and format
- I2C master and slave (256-byte register map)
- SPI master and slave, with MOSI capture for sniffing a target's SPI bus

**Debug probe (CMSIS-DAP v2)**
- SWD and JTAG, including ARM over JTAG
- SWO trace capture (UART mode, up to 18.75 Mbaud)
- Works with OpenOCD, pyOCD and Espressif's OpenOCD — no MCP call needed
- Runs alongside the instrument: watch a target's buses while you step
  through its code

**Control**
- MCP server for Claude Code and other MCP clients
- Command-line tool for bench use
- Desktop GUI with live waveform and analog plots
- Typed Python library underneath all of them
- Windows, macOS and Linux; no driver install on Windows (WinUSB)

**Safety**
- Digital and PWM outputs need an explicit, time-limited arm lease that
  expires on its own
- Outputs drop to a safe state on watchdog timeout, host loss or protocol
  fault
- MCP write tools are disabled by default, with an audit log of every call
- Debug sessions are the one deliberate exception: a debugger drives its
  pins directly, and sessions are reported to the host

## Key specs

| | |
|---|---|
| Board | Raspberry Pi Pico 2 (RP2350), unmodified |
| USB | Full-speed composite device: control channel, CMSIS-DAP v2, diagnostic serial console |
| Debug clocks | SWD up to 6.8 MHz, JTAG up to 4.7 MHz |
| Target voltage | 3.3 V |

## What you need

- A Raspberry Pi Pico 2 (non-W)
- A Raspberry Pi Debug Probe (or a Pico running debugprobe) to flash it,
  or just the Pico's BOOTSEL button and the UF2 file
- Python 3.10+ for the host tools

## Getting started

Building, flashing, connecting to Claude Code, the pinout, debug-probe
wiring and troubleshooting are all in **[userguide.md](userguide.md)**.
