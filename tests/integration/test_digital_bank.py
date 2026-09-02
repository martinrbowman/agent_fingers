import time

import pytest

from device import DeviceStatusError, protocol_constants as pc


def test_arm_write_read(device):
    granted = device.arm_outputs(0b00000011, lease_ms=30000)
    assert granted == 30000
    status = device.get_status()
    assert status["outputs_armed"] is True
    assert status["armed_mask"] == 0b00000011

    device.digital_write_masked(0b00000011, 0b00000001)
    value = device.digital_read(0xFF)
    assert value & 0b01 == 0b01
    assert value & 0b10 == 0b00


def test_arm_rejects_stale_challenge(device):
    """Bypasses the client's automatic fresh-challenge fetch to prove the
    device itself rejects a wrong/replayed challenge, not just that the
    client always happens to send a valid one."""
    bad_challenge = b"\x00" * 16
    req = pc.ARM_OUTPUTS_REQUEST_STRUCT.pack(bad_challenge, 0b00000001, b"\x00\x00\x00", 5000)
    with pytest.raises(DeviceStatusError) as exc_info:
        device._t.request(pc.OPCODE_ARM_OUTPUTS, req)
    assert exc_info.value.status == pc.STATUS_MALFORMED


def test_write_outside_armed_mask_rejected(device):
    device.arm_outputs(0b00000001, lease_ms=5000)
    with pytest.raises(DeviceStatusError):
        device.digital_write_masked(0b00000010, 0b00000010)


def test_write_without_arm_rejected(device):
    with pytest.raises(DeviceStatusError):
        device.digital_write_masked(0b00000001, 0b00000001)


def test_lease_expiry_forces_safe(device):
    device.arm_outputs(0b00000001, lease_ms=500)
    time.sleep(1.0)  # no ping sent -- lease must expire on its own
    status = device.get_status()
    assert status["outputs_armed"] is False
    assert status["safe_state_reason"] == 2  # SAFE_STATE_HOST_LOST


def test_ping_renews_lease(device):
    device.arm_outputs(0b00000001, lease_ms=500)
    for _ in range(4):
        time.sleep(0.3)
        device.ping()  # renews the lease each time -- must never expire
    assert device.get_status()["outputs_armed"] is True


def test_disarm_is_immediate(device):
    device.arm_outputs(0b00000001, lease_ms=30000)
    device.disarm_outputs()
    status = device.get_status()
    assert status["outputs_armed"] is False
    assert status["safe_state_reason"] == 4  # SAFE_STATE_EXPLICIT_DISARM
