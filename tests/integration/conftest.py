"""Shared fixtures for the hardware-in-the-loop pytest suite. Requires a
real Agent Fingers board attached over USB — every test here talks to
actual hardware, there is no mock/fake transport (see handoff.md's step 7
notes on why: real hardware was available, so it was used instead).

Run with:
    pytest tests/integration/ [--device SERIAL]

Needs `host/` installed: pip install -e host/
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "host"))
from device import Device, DeviceError  # noqa: E402


def pytest_addoption(parser):
    parser.addoption("--device", default=None, help="Serial number of the board under test")


@pytest.fixture(scope="session")
def device(request):
    serial = request.config.getoption("--device")
    try:
        dev = Device(serial=serial)
    except DeviceError as e:
        pytest.skip(f"no Agent Fingers device available: {e}")
    yield dev
    dev.close()


def _reset(dev):
    """Best-effort return to a clean idle state. Every call is independent
    and swallows DeviceError — a test that already left things clean
    shouldn't fail the NEXT test's setup because e.g. disarming when
    nothing is armed raises nothing anyway, but a capture that was never
    started might."""
    for fn in (
        lambda: dev.disarm_outputs(),
        lambda: dev.digital_capture_stop(),
        lambda: dev.adc_scan_config(0, 0, 0),
        lambda: dev.i2c_slave_config(False),
        lambda: dev.spi_slave_config(False),
    ):
        try:
            fn()
        except DeviceError:
            pass
    # Drain any stale queued events from a previous test so they don't leak
    # into the next one's poll_event() calls.
    while dev.poll_event(timeout=0.05) is not None:
        pass


@pytest.fixture(autouse=True)
def clean_state(device):
    """Every test starts and ends with outputs disarmed and no active
    capture/scan/slave — HIL tests must not leak state to the next test."""
    _reset(device)
    yield
    _reset(device)
