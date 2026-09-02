import pytest

from device import DeviceStatusError


def test_pwm_requires_armed_channel(device):
    with pytest.raises(DeviceStatusError):
        device.pwm_config(0, True, 1000, 500)


def test_pwm_enable_disable(device):
    device.arm_outputs(0b00000011, lease_ms=30000)
    result = device.pwm_config(0, True, 1000, 500)
    assert 900 <= result["granted_frequency_hz"] <= 1100  # clkdiv quantization tolerance
    assert result["granted_duty_permille"] == 500
    device.pwm_config(0, False)


def test_write_masked_rejected_while_pwm_active(device):
    device.arm_outputs(0b00000011, lease_ms=30000)
    device.pwm_config(0, True, 1000, 500)
    with pytest.raises(DeviceStatusError):
        device.digital_write_masked(0b00000001, 0b00000001)
    device.pwm_config(0, False)
    device.digital_write_masked(0b00000001, 0b00000001)  # now released, must succeed


def test_shared_slice_duty_only_change(device):
    """GP0/GP1 share one PWM slice. Same frequency on both -> duty-only
    fast path internally; must not error."""
    device.arm_outputs(0b00000011, lease_ms=30000)
    device.pwm_config(0, True, 1000, 500)
    result = device.pwm_config(1, True, 1000, 250)  # same freq, different duty
    assert result["granted_frequency_hz"] > 0
    device.pwm_config(0, False)
    device.pwm_config(1, False)


def test_disarm_releases_active_pwm(device):
    """The trickiest safety property: disarming while PWM is actively
    driving must not leave the pin stuck driving."""
    device.arm_outputs(0b00000001, lease_ms=30000)
    device.pwm_config(0, True, 2000, 500)
    device.disarm_outputs()
    status = device.get_status()
    assert status["outputs_armed"] is False
    assert status["armed_mask"] == 0
    with pytest.raises(DeviceStatusError):
        device.pwm_config(0, True, 1000, 500)  # channel no longer armed
