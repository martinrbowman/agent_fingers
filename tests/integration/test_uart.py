"""GP16 (TX) <-> GP17 (RX) loopback jumper is wired, so
test_loopback_round_trip below is a real byte-level verification, not just
plumbing.

No "write/read before config rejected" tests here: uart_port has no
unconfigure opcode, so once anything (this suite, an earlier manual
session) calls uart_config, the port stays configured until the board
reboots. That state is real and correct for actual use -- there's no
legitimate reason to un-configure a UART you're actively using -- but it
means "not yet configured" isn't a state this suite can reliably force on
a warm, already-booted board, only a fresh one."""
import time

import pytest

from device import DeviceStatusError


def test_config_reports_granted_baud(device):
    granted = device.uart_config(115200)
    assert granted > 0


def test_idle_read_is_clean(device):
    device.uart_config(1200)
    result = device.uart_read(100)
    assert result["data"] == b""
    assert result["framing_errors"] == 0
    assert result["parity_errors"] == 0
    assert result["break_errors"] == 0
    assert result["overrun_errors"] == 0
    assert result["ring_overrun_count"] == 0


@pytest.mark.slow
def test_write_rejects_while_previous_still_draining(device):
    device.uart_config(1200)  # slow baud so a big payload takes a while
    big_payload = bytes(range(256)) * 2  # ~4.3s to drain at 1200 baud
    device.uart_write(big_payload)
    with pytest.raises(DeviceStatusError):
        device.uart_write(b"x")  # immediate retry, previous write still in flight
    time.sleep(5.0)
    device.uart_write(b"y")  # now free again


def test_loopback_round_trip(device):
    """Real byte-level verification via the GP16<->GP17 jumper: every byte
    value 0-255 sent, read back identical, no line errors."""
    baud = device.uart_config(115200)
    payload = bytes(range(256))
    device.uart_write(payload)

    bits_per_byte = 10  # 1 start + 8 data + 1 stop, matches uart_config default
    time.sleep(len(payload) * bits_per_byte / baud + 0.05)

    result = device.uart_read(len(payload))
    assert result["data"] == payload
    assert result["framing_errors"] == 0
    assert result["parity_errors"] == 0
    assert result["break_errors"] == 0
    assert result["overrun_errors"] == 0
    assert result["ring_overrun_count"] == 0


def test_reconfigure_resets_counters(device):
    device.uart_config(9600)
    device.uart_config(115200)
    result = device.uart_read(10)
    assert result == {
        "data": b"", "framing_errors": 0, "parity_errors": 0,
        "break_errors": 0, "overrun_errors": 0, "ring_overrun_count": 0,
    }
