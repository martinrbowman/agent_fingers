"""Malformed-frame fuzz campaign, per the plan's step 8 ("randomized
malformed-frame campaign") and verification-strategy section ("arbitrary
byte streams must not crash, overrun, allocate unbounded memory, alter
outputs, or leave parser state wedged").

Writes raw bytes directly to the vendor-bulk OUT endpoint, bypassing
Transport's normal request/response framing entirely — these are
deliberately malformed or truncated, so there's no well-formed request to
correlate a response to. The device is expected to just resync silently
(or emit an ERROR frame that nothing is listening for) and keep working.
"""

import random
import struct
import time

from device import protocol_constants as pc
from device.transport import EP_OUT


def _raw_write(device, data: bytes):
    device._t._dev.write(EP_OUT, data, timeout=500)


def test_random_garbage_does_not_crash_or_corrupt_state(device):
    baseline = device.get_status()["parser_counters"]["frames_ok"]

    rng = random.Random(1234)  # deterministic and reproducible
    for _ in range(200):
        length = rng.randint(1, 64)
        garbage = bytes(rng.randrange(256) for _ in range(length))
        try:
            _raw_write(device, garbage)
        except Exception:
            pass  # a stalled write here is itself a finding, not fatal to the test

    # Device must still be alive, responsive, and un-armed after the barrage.
    info = device.get_info()
    assert info["protocol_version"] == pc.VERSION

    status = device.get_status()
    assert status["outputs_armed"] is False, "garbage must never accidentally arm/drive an output"
    assert status["parser_counters"]["frames_ok"] > baseline, "device must still answer real requests"


def test_resync_after_garbage_prefix(device):
    """A real, valid request immediately following a garbage prefix must
    still be answered correctly -- proves the magic-scan resync, not just
    that the device eventually recovers given enough idle time."""
    baseline_resyncs = device.get_status()["parser_counters"]["magic_resyncs"]

    garbage = bytes([0x00, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC])
    _raw_write(device, garbage)

    info = device.get_info()  # goes through the normal, correlated request path
    assert info["protocol_version"] == pc.VERSION

    resyncs = device.get_status()["parser_counters"]["magic_resyncs"]
    assert resyncs > baseline_resyncs


def test_corrupted_crc_reported_not_crashed(device):
    baseline = device.get_status()["parser_counters"]["err_bad_crc"]

    frame = bytearray(pc.build_frame(0xFEED, pc.OPCODE_GET_INFO))
    frame[-1] ^= 0xFF  # flip the last CRC byte
    _raw_write(device, bytes(frame))

    info = device.get_info()  # device must still be fully functional afterward
    assert info["protocol_version"] == pc.VERSION
    assert device.get_status()["parser_counters"]["err_bad_crc"] > baseline


def test_oversize_declared_length_rejected_cleanly(device):
    """A payload_len within the wire max (16384) but beyond the firmware's
    bring-up buffer cap must be skip-discarded, not buffered or crashed.

    The firmware can't reject on the declared length alone without seeing
    it: it's a byte-counted skip, so a truncated oversize header (no
    payload/CRC following) leaves the parser mid-skip, waiting for bytes
    that never come. That's caught by the stalled-partial-frame idle
    timeout (protocol/dispatch.c), not an immediate error frame -- so this
    waits past that timeout rather than making an immediate request, which
    would otherwise get silently swallowed as the frame's "missing" tail
    and time out (a real cascade this test found before the timeout was
    added: see err_stalled_frame in handoff.md)."""
    baseline = device.get_status()["parser_counters"]["err_stalled_frame"]

    header = pc.HEADER_STRUCT.pack(pc.MAGIC, pc.VERSION, pc.FRAME_TYPE_REQUEST,
                                    0, 0xF00D, pc.OPCODE_GET_INFO, 0, 4000)
    _raw_write(device, header)
    time.sleep(0.2)  # past PROTOCOL_STALLED_FRAME_TIMEOUT_US (100ms)

    info = device.get_info()
    assert info["protocol_version"] == pc.VERSION
    assert device.get_status()["parser_counters"]["err_stalled_frame"] > baseline


def test_wrong_protocol_version_rejected_cleanly(device):
    baseline = device.get_status()["parser_counters"]["err_bad_version"]

    frame = pc.build_frame(0xC0DE, pc.OPCODE_GET_INFO)
    frame = bytearray(frame)
    frame[4] = 99  # version byte, offset 4 in the header
    # Recompute CRC over the tampered header so this is JUST a bad version,
    # not also a bad CRC -- isolates the specific rejection reason.
    header_and_payload = bytes(frame[:-4])
    new_crc = pc.crc32c(header_and_payload)
    frame[-4:] = struct.pack("<I", new_crc)
    _raw_write(device, bytes(frame))

    info = device.get_info()
    assert info["protocol_version"] == pc.VERSION
    assert device.get_status()["parser_counters"]["err_bad_version"] > baseline
