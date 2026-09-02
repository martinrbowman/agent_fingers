import time

import pytest

from device import DeviceStatusError


def test_bounded_capture_completes_exactly(device):
    device.arm_outputs(0b00000001, lease_ms=30000)
    device.digital_write_masked(0b00000001, 0b00000001)

    cap = device.digital_capture_start(rate_hz=5000, max_samples=200)
    assert cap["granted_rate_hz"] > 0

    event = None
    deadline = time.time() + 2.0
    while time.time() < deadline:
        event = device.poll_event(timeout=0.2)
        if event:
            break
    assert event is not None, "no capture-complete EVENT received"
    assert event["opcode"] == 11  # OPCODE_DIGITAL_CAPTURE_STOP

    result = device.digital_capture_read(cap["capture_id"], 0, 200)
    assert result["total_captured"] == 200
    assert len(result["samples"]) == 200
    # GP0 was driven high for the whole capture -- every sample's bit0 must be set.
    assert all(b & 1 for b in result["samples"])


def test_capture_read_pagination(device):
    cap = device.digital_capture_start(rate_hz=5000, max_samples=500)
    time.sleep(0.15)
    r1 = device.digital_capture_read(cap["capture_id"], 0, 484)
    assert r1["overrun_count"] == 0


def test_read_with_wrong_capture_id_rejected(device):
    cap = device.digital_capture_start(rate_hz=5000, max_samples=50)
    with pytest.raises(DeviceStatusError):
        device.digital_capture_read(cap["capture_id"] + 999, 0, 10)


def test_new_start_replaces_previous_capture(device):
    cap1 = device.digital_capture_start(rate_hz=5000, max_samples=50)
    cap2 = device.digital_capture_start(rate_hz=5000, max_samples=50)
    assert cap2["capture_id"] != cap1["capture_id"]
    with pytest.raises(DeviceStatusError):
        device.digital_capture_read(cap1["capture_id"], 0, 10)
