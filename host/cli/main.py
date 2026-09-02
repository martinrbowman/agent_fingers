"""Operator CLI for diagnostics and fixture control. Uses the same
device.Device library as the MCP server (per the plan: "make it use the
same library as MCP") — but talks to the board directly, with no MCP-layer
write policy in front of it. This is the tool for a human operator at a
bench, not the agent-facing surface.
"""

import argparse
import json
import sys

from device import Device, DeviceError, list_devices


def _print(obj):
    print(json.dumps(obj, indent=2, default=lambda o: o.hex() if isinstance(o, (bytes, bytearray)) else str(o)))


def cmd_list(args):
    serials = list(list_devices())
    if not serials:
        print("no devices found", file=sys.stderr)
        return 1
    for s in serials:
        print(s or "<unreadable serial>")
    return 0


def cmd_info(args):
    with Device(serial=args.device) as dev:
        _print(dev.get_info())
    return 0


def cmd_status(args):
    with Device(serial=args.device) as dev:
        status = dict(dev.get_status())
        status["arm_challenge"] = status["arm_challenge"].hex()
        _print(status)
    return 0


def cmd_ping(args):
    with Device(serial=args.device) as dev:
        _print(dev.ping(bytes.fromhex(args.payload_hex)))
    return 0


def cmd_arm(args):
    with Device(serial=args.device) as dev:
        granted = dev.arm_outputs(args.mask, args.lease_ms)
        _print({"granted_lease_ms": granted})
    return 0


def cmd_disarm(args):
    with Device(serial=args.device) as dev:
        dev.disarm_outputs()
    return 0


def cmd_digital_write(args):
    with Device(serial=args.device) as dev:
        dev.digital_write_masked(args.mask, args.values)
    return 0


def cmd_digital_read(args):
    with Device(serial=args.device) as dev:
        _print({"values": dev.digital_read(args.mask)})
    return 0


def cmd_pwm(args):
    with Device(serial=args.device) as dev:
        _print(dev.pwm_config(args.channel, not args.disable, args.frequency_hz, args.duty_permille))
    return 0


def cmd_uart_config(args):
    with Device(serial=args.device) as dev:
        _print({"granted_baud_rate": dev.uart_config(args.baud, args.data_bits, args.stop_bits, args.parity)})
    return 0


def cmd_uart_write(args):
    with Device(serial=args.device) as dev:
        dev.uart_write(bytes.fromhex(args.data_hex))
    return 0


def cmd_uart_read(args):
    with Device(serial=args.device) as dev:
        result = dict(dev.uart_read(args.max_bytes))
        result["data"] = result["data"].hex()
        _print(result)
    return 0


def cmd_i2c_xfer(args):
    with Device(serial=args.device) as dev:
        result = dict(dev.i2c_xfer(args.address, bytes.fromhex(args.write_hex), args.read_length, args.timeout_ms))
        result["data"] = result["data"].hex()
        _print(result)
    return 0


def cmd_spi_xfer(args):
    with Device(serial=args.device) as dev:
        result = dict(dev.spi_xfer(bytes.fromhex(args.tx_hex), args.hz, args.mode))
        result["data"] = result["data"].hex()
        _print(result)
    return 0


def build_parser():
    parser = argparse.ArgumentParser(prog="rp2350-signal-cli")
    parser.add_argument("--device", metavar="SERIAL", default=None,
                         help="Serial number to select (required if more than one device is attached)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="List attached device serial numbers").set_defaults(func=cmd_list)
    sub.add_parser("info", help="GET_INFO").set_defaults(func=cmd_info)
    sub.add_parser("status", help="GET_STATUS").set_defaults(func=cmd_status)

    p = sub.add_parser("ping", help="PING")
    p.add_argument("payload_hex", nargs="?", default="")
    p.set_defaults(func=cmd_ping)

    p = sub.add_parser("arm", help="ARM_OUTPUTS")
    p.add_argument("mask", type=lambda x: int(x, 0), help="output_mask, e.g. 0x03")
    p.add_argument("--lease-ms", type=int, default=0, dest="lease_ms")
    p.set_defaults(func=cmd_arm)

    sub.add_parser("disarm", help="DISARM_OUTPUTS").set_defaults(func=cmd_disarm)

    p = sub.add_parser("digital-write", help="DIGITAL_WRITE_MASKED")
    p.add_argument("mask", type=lambda x: int(x, 0))
    p.add_argument("values", type=lambda x: int(x, 0))
    p.set_defaults(func=cmd_digital_write)

    p = sub.add_parser("digital-read", help="DIGITAL_READ")
    p.add_argument("--mask", type=lambda x: int(x, 0), default=0xFF)
    p.set_defaults(func=cmd_digital_read)

    p = sub.add_parser("pwm", help="PWM_CONFIG")
    p.add_argument("channel", type=int)
    p.add_argument("--frequency-hz", type=int, default=0, dest="frequency_hz")
    p.add_argument("--duty-permille", type=int, default=0, dest="duty_permille")
    p.add_argument("--disable", action="store_true")
    p.set_defaults(func=cmd_pwm)

    p = sub.add_parser("uart-config", help="UART_CONFIG")
    p.add_argument("baud", type=int)
    p.add_argument("--data-bits", type=int, default=8, dest="data_bits")
    p.add_argument("--stop-bits", type=int, default=1, dest="stop_bits")
    p.add_argument("--parity", type=int, default=0, help="0=none 1=even 2=odd")
    p.set_defaults(func=cmd_uart_config)

    p = sub.add_parser("uart-write", help="UART_WRITE")
    p.add_argument("data_hex")
    p.set_defaults(func=cmd_uart_write)

    p = sub.add_parser("uart-read", help="UART_READ")
    p.add_argument("--max-bytes", type=int, default=480, dest="max_bytes")
    p.set_defaults(func=cmd_uart_read)

    p = sub.add_parser("i2c-xfer", help="I2C_XFER (master)")
    p.add_argument("address", type=lambda x: int(x, 0))
    p.add_argument("--write-hex", default="", dest="write_hex")
    p.add_argument("--read-length", type=int, default=0, dest="read_length")
    p.add_argument("--timeout-ms", type=int, default=0, dest="timeout_ms")
    p.set_defaults(func=cmd_i2c_xfer)

    p = sub.add_parser("spi-xfer", help="SPI_XFER (master, mode 0 only)")
    p.add_argument("tx_hex")
    p.add_argument("--hz", type=int, default=100000)
    p.add_argument("--mode", type=int, default=0)
    p.set_defaults(func=cmd_spi_xfer)

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    try:
        sys.exit(args.func(args) or 0)
    except DeviceError as e:
        print(f"rp2350-signal-cli: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
