"""GP20<->GP18 (SDA) and GP21<->GP19 (SCL) loopback jumpers are wired, so
test_register_map_round_trip below is a real master<->slave
verification. Genuine bus-timeout/recovery (a stuck SDA/SCL line) still
isn't -- that needs deliberately jamming the bus, not just a jumper, and
isn't set up here. See handoff.md's "Pending" section."""


def test_nack_on_nonexistent_address(device):
    result = device.i2c_xfer(0x50, write_data=bytes([0x00]), read_len=4, timeout_ms=200)
    assert result["write_status"] == 1  # nack
    assert result["bytes_written"] == 0


def test_read_phase_skipped_after_failed_write(device):
    result = device.i2c_xfer(0x50, write_data=bytes([0x00]), read_len=4, timeout_ms=200)
    assert result["write_status"] == 1
    assert result["read_status"] == 0  # not attempted, not "ok" from a real read
    assert result["data"] == b""


def test_register_map_round_trip(device):
    """Real master (GP20/21) <-> slave (GP18/19) verification: write a
    known payload into the slave's register map starting at register 0,
    reposition the pointer back to 0, and read it back byte-identical.

    The slave's protocol (see i2c_bus.c's i2c_slave_event_handler): the
    first byte of a write transaction sets the register pointer, every
    following write byte lands at regmap[ptr++]; a read continues from
    wherever the pointer was last left. i2c_xfer's write and read phases
    are two separate bus transactions (not a combined repeated-start), so
    repositioning to register 0 for the read needs its own single-byte
    write first."""
    device.i2c_slave_config(True, address=0x42)
    payload = bytes(range(16))

    write_result = device.i2c_xfer(0x42, write_data=bytes([0]) + payload, timeout_ms=200)
    assert write_result["write_status"] == 0
    assert write_result["bytes_written"] == 1 + len(payload)

    reposition = device.i2c_xfer(0x42, write_data=bytes([0]), timeout_ms=200)
    assert reposition["write_status"] == 0

    read_result = device.i2c_xfer(0x42, read_len=len(payload), timeout_ms=200)
    assert read_result["read_status"] == 0
    assert read_result["data"] == payload

    slave_result = device.i2c_slave_config(False)
    assert slave_result["bytes_received"] == len(payload) + 2  # payload + 2 pointer-set bytes
    assert slave_result["bytes_sent"] == len(payload)
    assert slave_result["transaction_count"] == 3  # write, reposition, read


def test_slave_config_enable_disable_counters(device):
    result = device.i2c_slave_config(True, address=0x42)
    assert result == {"bytes_received": 0, "bytes_sent": 0, "transaction_count": 0}

    result2 = device.i2c_slave_config(True, address=0x42)  # reconfig, fetch-and-reset
    assert result2 == {"bytes_received": 0, "bytes_sent": 0, "transaction_count": 0}

    device.i2c_slave_config(False)
