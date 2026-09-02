"""Repeated-cycling stress tests that run within the shared session
connection (see conftest.py). USB reconnect-cycling and the full 24-hour
soak scenario need their own dedicated, exclusive connection for their
whole duration and can't share the session fixture — those are standalone
scripts (soak_reconnect.py, soak_24h.py), not pytest tests; run manually.
"""

import time

import pytest

from device import protocol_constants as pc


@pytest.mark.slow
def test_repeated_arm_disarm_cycling(device):
    for i in range(50):
        device.arm_outputs(0b00000001, lease_ms=2000)
        device.digital_write_masked(0b00000001, i % 2)
        device.disarm_outputs()
    assert device.get_status()["outputs_armed"] is False


@pytest.mark.slow
def test_rapid_ping_loop(device):
    for _ in range(100):
        result = device.ping(b"x")
        assert result["echo"] == b"x"


@pytest.mark.slow
def test_sustained_capture_at_max_rate_does_not_hang(device):
    """Not about correctness of the captured data -- at max rate for 2s
    the 1024-sample ring wraps many times over, so overrun is *expected*.
    The property under test is: the device keeps responding throughout and
    afterward, nothing crashes or wedges."""
    cap = device.digital_capture_start(rate_hz=pc.DIGITAL_CAPTURE_MAX_RATE_HZ, max_samples=0)
    time.sleep(2.0)
    # Device must still answer other requests WHILE a capture is running.
    info = device.get_info()
    assert info["protocol_version"] == pc.VERSION
    result = device.digital_capture_read(cap["capture_id"], 0, 100)
    assert result["total_captured"] > 0
    device.digital_capture_stop()


@pytest.mark.slow
def test_repeated_capture_start_stop_cycling(device):
    for _ in range(20):
        cap = device.digital_capture_start(rate_hz=5000, max_samples=50)
        device.digital_capture_stop()
        assert cap["capture_id"] > 0


@pytest.mark.slow
def test_repeated_adc_scan_start_stop_cycling(device):
    for _ in range(20):
        scan = device.adc_scan_config(0b111, rate_hz=3000, max_samples=50)
        device.adc_scan_config(0, 0, 0)
        assert scan["scan_id"] > 0
