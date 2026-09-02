import time

import pytest

from device import DeviceStatusError


def test_scan_config_reports_correct_metadata(device):
    scan = device.adc_scan_config(0b111, rate_hz=3000, max_samples=0)
    assert scan["channel_count"] == 3
    assert scan["buffer_capacity_samples"] == 2048
    assert scan["vref_millivolts"] == 3300


def test_scan_read_returns_samples_in_valid_range(device):
    scan = device.adc_scan_config(0b111, rate_hz=3000, max_samples=0)
    time.sleep(0.15)
    result = device.adc_scan_read(scan["scan_id"], 0, 400)
    assert result["total_captured"] > 0
    assert all(0 <= s <= 4095 for s in result["samples"])


def test_bounded_scan_completes_exactly(device):
    scan = device.adc_scan_config(0b001, rate_hz=5000, max_samples=300)

    event = None
    deadline = time.time() + 2.0
    while time.time() < deadline:
        event = device.poll_event(timeout=0.2)
        if event:
            break
    assert event is not None
    assert event["opcode"] == 15  # OPCODE_ADC_SCAN_READ

    r1 = device.adc_scan_read(scan["scan_id"], 0, 242)
    r2 = device.adc_scan_read(scan["scan_id"], 242, 242)
    assert r1["total_captured"] == 300
    assert len(r1["samples"]) + len(r2["samples"]) == 300


def test_read_with_wrong_scan_id_rejected(device):
    scan = device.adc_scan_config(0b001, rate_hz=3000, max_samples=50)
    with pytest.raises(DeviceStatusError):
        device.adc_scan_read(scan["scan_id"] + 999, 0, 10)
