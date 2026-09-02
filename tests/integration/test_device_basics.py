from device import protocol_constants as pc


def test_get_info(device):
    info = device.get_info()
    assert info["protocol_version"] == pc.VERSION
    assert len(info["board_unique_id"]) == 16  # 8 bytes, hex-encoded
    assert info["git_hash"]


def test_get_capabilities(device):
    caps = device.get_capabilities()
    assert caps["digital_channel_count"] == 8
    assert caps["adc_channel_count"] == 3
    assert caps["spi_master_count"] == 1
    assert caps["spi_slave_count"] == 1
    assert caps["i2c_master_count"] == 1
    assert caps["i2c_slave_count"] == 1
    assert caps["uart_count"] == 1


def test_get_status_idle_baseline(device):
    # Parser counters are cumulative since boot, not since-test-start -- a
    # board that's run any fuzz test since its last power-on legitimately
    # has nonzero magic_resyncs/err_*. Only outputs_armed/armed_mask are a
    # real idle invariant (enforced by conftest's autouse clean_state);
    # counter behavior is exercised properly via baseline-diffs in
    # test_fuzz.py instead.
    status = device.get_status()
    assert status["outputs_armed"] is False
    assert status["armed_mask"] == 0
    assert set(status["parser_counters"]) == {
        "frames_ok", "magic_resyncs", "err_bad_version", "err_bad_length",
        "err_bad_crc", "err_oversize", "err_stalled_frame",
    }


def test_ping_echoes_payload(device):
    result = device.ping(b"pytest-hil")
    assert result["echo"] == b"pytest-hil"
    assert result["uptime_us"] > 0


def test_ping_empty_payload(device):
    result = device.ping(b"")
    assert result["echo"] == b""
