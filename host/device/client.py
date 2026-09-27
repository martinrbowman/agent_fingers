"""Typed Python client for Agent Fingers — one method per
implemented opcode, translating between Python-friendly arguments/return
values and the wire structs in protocol_constants.py.

This is the only thing MCP tools and the CLI should import from `device`
(besides the exception types) — per the plan, MCP code must not parse
binary frames or make USB calls directly. All of that lives in
transport.py, which this module wraps.
"""

import struct

from . import protocol_constants as pc
from .transport import DeviceStatusError, Transport

__all__ = ["Device", "DeviceStatusError"]


class Device:
    """A single open connection to one physical board. Use as a context
    manager so the exclusive lock and USB interface claim are always
    released:

        with Device() as dev:
            print(dev.get_info())
    """

    def __init__(self, serial=None, timeout_s=2.0):
        self._t = Transport(serial=serial, timeout_s=timeout_s)

    @property
    def serial(self):
        return self._t.serial

    def close(self):
        self._t.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _call(self, opcode, payload=b""):
        return self._t.request(opcode, payload)["payload"]

    # -- GET_INFO / GET_CAPABILITIES / GET_STATUS / PING --------------------

    def get_info(self):
        proto_ver, major, minor, patch, board_id, git_hash = \
            pc.INFO_RESPONSE_STRUCT.unpack(self._call(pc.OPCODE_GET_INFO))
        return {
            "protocol_version": proto_ver,
            "firmware_version": f"{major}.{minor}.{patch}",
            "board_unique_id": board_id.hex(),
            "git_hash": git_hash.split(b"\x00", 1)[0].decode(),
        }

    def get_capabilities(self):
        fields = pc.CAPABILITIES_RESPONSE_STRUCT.unpack(self._call(pc.OPCODE_GET_CAPABILITIES))
        keys = ["digital_channel_count", "spi_master_count", "spi_slave_count",
                "i2c_master_count", "i2c_slave_count", "uart_count",
                "adc_channel_count", "debug_probe_ports"]
        return dict(zip(keys, fields))

    def get_status(self):
        (uptime_us, reset_cause, safe_state_reason, outputs_armed, armed_mask,
         frames_ok, magic_resyncs, err_bad_version, err_bad_length, err_bad_crc,
         err_oversize, err_stalled_frame, arm_challenge, debug_active, debug_mode,
         debug_last_end_reason, _reserved, debug_session_count) = pc.STATUS_RESPONSE_STRUCT.unpack(
            self._call(pc.OPCODE_GET_STATUS)
        )
        return {
            "uptime_us": uptime_us,
            "reset_cause": reset_cause,
            "safe_state_reason": safe_state_reason,
            "outputs_armed": bool(outputs_armed),
            "armed_mask": armed_mask,
            "parser_counters": {
                "frames_ok": frames_ok,
                "magic_resyncs": magic_resyncs,
                "err_bad_version": err_bad_version,
                "err_bad_length": err_bad_length,
                "err_bad_crc": err_bad_crc,
                "err_oversize": err_oversize,
                "err_stalled_frame": err_stalled_frame,
            },
            "arm_challenge": arm_challenge,  # raw 16 bytes; feed straight into arm_outputs()
            # CMSIS-DAP probe session: started/ended by the DAP interface only.
            "debug_session": {
                "active": bool(debug_active),
                "mode": pc.DEBUG_MODE_NAMES.get(debug_mode, debug_mode),
                "last_end_reason": pc.DEBUG_END_REASON_NAMES.get(debug_last_end_reason,
                                                                 debug_last_end_reason),
                "session_count": debug_session_count,
            },
        }

    def ping(self, payload: bytes = b""):
        resp = self._call(pc.OPCODE_PING, payload)
        uptime_us = struct.unpack_from("<Q", resp, 0)[0]
        echo = resp[8:]
        return {"uptime_us": uptime_us, "echo": echo}

    # -- Digital bank: arm/disarm/read/write ---------------------------------

    def arm_outputs(self, output_mask: int, lease_ms: int = 0):
        """Arms output_mask (a bitmask over GP0-7) for driving. Fetches a
        fresh challenge from GET_STATUS itself, so callers never need to
        manage that. Returns the granted lease in milliseconds."""
        challenge = self.get_status()["arm_challenge"]
        req = pc.ARM_OUTPUTS_REQUEST_STRUCT.pack(challenge, output_mask, b"\x00\x00\x00", lease_ms)
        granted_lease_ms, = pc.ARM_OUTPUTS_RESPONSE_STRUCT.unpack(self._call(pc.OPCODE_ARM_OUTPUTS, req))
        return granted_lease_ms

    def disarm_outputs(self):
        self._call(pc.OPCODE_DISARM_OUTPUTS)

    def digital_write_masked(self, mask: int, values: int):
        self._call(pc.OPCODE_DIGITAL_WRITE_MASKED, pc.DIGITAL_WRITE_MASKED_REQUEST_STRUCT.pack(mask, values))

    def digital_read(self, mask: int = 0xFF) -> int:
        values, = pc.DIGITAL_READ_RESPONSE_STRUCT.unpack(
            self._call(pc.OPCODE_DIGITAL_READ, pc.DIGITAL_READ_REQUEST_STRUCT.pack(mask))
        )
        return values

    # -- Digital capture ------------------------------------------------------

    def digital_capture_start(self, rate_hz: int, max_samples: int = 0):
        req = pc.DIGITAL_CAPTURE_START_REQUEST_STRUCT.pack(rate_hz, max_samples)
        capture_id, granted_rate_hz, buffer_capacity = pc.DIGITAL_CAPTURE_START_RESPONSE_STRUCT.unpack(
            self._call(pc.OPCODE_DIGITAL_CAPTURE_START, req)
        )
        return {"capture_id": capture_id, "granted_rate_hz": granted_rate_hz,
                "buffer_capacity_samples": buffer_capacity}

    def digital_capture_stop(self):
        capture_id, total_captured = pc.DIGITAL_CAPTURE_STOP_RESPONSE_STRUCT.unpack(
            self._call(pc.OPCODE_DIGITAL_CAPTURE_STOP)
        )
        return {"capture_id": capture_id, "total_captured": total_captured}

    def digital_capture_read(self, capture_id: int, offset: int, limit: int):
        req = pc.DIGITAL_CAPTURE_READ_REQUEST_STRUCT.pack(capture_id, offset, limit, 0)
        resp = self._call(pc.OPCODE_DIGITAL_CAPTURE_READ, req)
        header_size = pc.DIGITAL_CAPTURE_READ_RESPONSE_STRUCT.size
        (cid, total_captured, overrun_count, start_timestamp_us, rate_hz,
         returned_count, _reserved) = pc.DIGITAL_CAPTURE_READ_RESPONSE_STRUCT.unpack(resp[:header_size])
        samples = resp[header_size:header_size + returned_count]
        return {"capture_id": cid, "total_captured": total_captured,
                "overrun_count": overrun_count, "start_timestamp_us": start_timestamp_us,
                "rate_hz": rate_hz, "samples": samples}

    # -- ADC scan ---------------------------------------------------------------

    def adc_scan_config(self, channel_mask: int, rate_hz: int = 0, max_samples: int = 0):
        """channel_mask=0 stops whatever scan is active (no separate stop opcode)."""
        req = pc.ADC_SCAN_CONFIG_REQUEST_STRUCT.pack(channel_mask, b"\x00\x00\x00", rate_hz, max_samples)
        scan_id, granted_rate_hz, channel_count, _r, buffer_capacity, vref_mv, _r2 = \
            pc.ADC_SCAN_CONFIG_RESPONSE_STRUCT.unpack(self._call(pc.OPCODE_ADC_SCAN_CONFIG, req))
        return {"scan_id": scan_id, "granted_rate_hz": granted_rate_hz,
                "channel_count": channel_count, "buffer_capacity_samples": buffer_capacity,
                "vref_millivolts": vref_mv}

    def adc_scan_read(self, scan_id: int, offset: int, limit: int):
        req = pc.ADC_SCAN_READ_REQUEST_STRUCT.pack(scan_id, offset, limit, 0)
        resp = self._call(pc.OPCODE_ADC_SCAN_READ, req)
        header_size = pc.ADC_SCAN_READ_RESPONSE_STRUCT.size
        (sid, total_captured, overrun_count, start_timestamp_us, rate_hz,
         channel_count, _reserved, returned_count) = pc.ADC_SCAN_READ_RESPONSE_STRUCT.unpack(resp[:header_size])
        samples = struct.unpack(f"<{returned_count}H", resp[header_size:header_size + returned_count * 2])
        return {"scan_id": sid, "total_captured": total_captured, "overrun_count": overrun_count,
                "start_timestamp_us": start_timestamp_us, "rate_hz": rate_hz,
                "channel_count": channel_count, "samples": samples}

    # -- PWM ----------------------------------------------------------------------

    def pwm_config(self, channel: int, enabled: bool, frequency_hz: int = 0, duty_permille: int = 0):
        req = pc.PWM_CONFIG_REQUEST_STRUCT.pack(channel, 1 if enabled else 0, 0, frequency_hz, duty_permille, 0)
        granted_hz, granted_duty, _r = pc.PWM_CONFIG_RESPONSE_STRUCT.unpack(self._call(pc.OPCODE_PWM_CONFIG, req))
        return {"granted_frequency_hz": granted_hz, "granted_duty_permille": granted_duty}

    # -- UART -----------------------------------------------------------------------

    def uart_config(self, baud_rate: int, data_bits: int = 8, stop_bits: int = 1, parity: int = 0):
        req = pc.UART_CONFIG_REQUEST_STRUCT.pack(baud_rate, data_bits, stop_bits, parity, 0)
        granted_baud, = pc.UART_CONFIG_RESPONSE_STRUCT.unpack(self._call(pc.OPCODE_UART_CONFIG, req))
        return granted_baud

    def uart_write(self, data: bytes):
        """Fire-and-forget; raises DeviceStatusError if a previous write's
        DMA transfer is still draining — retry after a short wait."""
        self._call(pc.OPCODE_UART_WRITE, data)

    def uart_read(self, max_bytes: int = 480):
        resp = self._call(pc.OPCODE_UART_READ, pc.UART_READ_REQUEST_STRUCT.pack(max_bytes))
        header_size = pc.UART_READ_RESPONSE_STRUCT.size
        (framing_errors, parity_errors, break_errors, overrun_errors,
         ring_overrun_count, returned_count, _reserved) = pc.UART_READ_RESPONSE_STRUCT.unpack(resp[:header_size])
        data = resp[header_size:header_size + returned_count]
        return {"data": data, "framing_errors": framing_errors, "parity_errors": parity_errors,
                "break_errors": break_errors, "overrun_errors": overrun_errors,
                "ring_overrun_count": ring_overrun_count}

    # -- I2C ------------------------------------------------------------------------

    def i2c_xfer(self, address: int, write_data: bytes = b"", read_len: int = 0, timeout_ms: int = 0):
        req = pc.I2C_XFER_REQUEST_STRUCT.pack(address, 0, len(write_data), read_len, timeout_ms) + write_data
        resp = self._t.request(pc.OPCODE_I2C_XFER, req, raise_on_error=False)["payload"]
        write_status, read_status, bytes_written, bytes_read = pc.I2C_XFER_RESPONSE_STRUCT.unpack(
            resp[:pc.I2C_XFER_RESPONSE_STRUCT.size]
        )
        data = resp[pc.I2C_XFER_RESPONSE_STRUCT.size:pc.I2C_XFER_RESPONSE_STRUCT.size + bytes_read]
        return {"write_status": write_status, "read_status": read_status,
                "bytes_written": bytes_written, "data": data}

    def i2c_slave_config(self, enabled: bool, address: int = 0):
        req = pc.I2C_SLAVE_CONFIG_REQUEST_STRUCT.pack(1 if enabled else 0, address, 0)
        bytes_received, bytes_sent, transaction_count = pc.I2C_SLAVE_CONFIG_RESPONSE_STRUCT.unpack(
            self._call(pc.OPCODE_I2C_SLAVE_CONFIG, req)
        )
        return {"bytes_received": bytes_received, "bytes_sent": bytes_sent,
                "transaction_count": transaction_count}

    # -- SPI ------------------------------------------------------------------------

    def spi_xfer(self, tx_data: bytes, hz: int = 100000, mode: int = 0):
        req = pc.SPI_XFER_REQUEST_STRUCT.pack(mode, 0, hz, len(tx_data)) + tx_data
        resp = self._call(pc.OPCODE_SPI_XFER, req)
        header_size = pc.SPI_XFER_RESPONSE_STRUCT.size
        granted_hz, length, _r = pc.SPI_XFER_RESPONSE_STRUCT.unpack(resp[:header_size])
        rx_data = resp[header_size:header_size + length]
        return {"granted_hz": granted_hz, "data": rx_data}

    def spi_slave_config(self, enabled: bool):
        req = pc.SPI_SLAVE_CONFIG_REQUEST_STRUCT.pack(1 if enabled else 0, b"\x00\x00\x00")
        bytes_received, bytes_sent, transaction_count = pc.SPI_SLAVE_CONFIG_RESPONSE_STRUCT.unpack(
            self._call(pc.OPCODE_SPI_SLAVE_CONFIG, req)
        )
        return {"bytes_received": bytes_received, "bytes_sent": bytes_sent,
                "transaction_count": transaction_count}

    def spi_slave_read(self, max_bytes: int = 256):
        """Drains MOSI bytes the slave captured since the last read. Only
        valid while the slave is enabled (raises DeviceStatusError otherwise)."""
        resp = self._call(pc.OPCODE_SPI_SLAVE_READ, pc.SPI_SLAVE_READ_REQUEST_STRUCT.pack(max_bytes))
        header_size = pc.SPI_SLAVE_READ_RESPONSE_STRUCT.size
        total_received, ring_overrun_count, returned_count, _reserved = \
            pc.SPI_SLAVE_READ_RESPONSE_STRUCT.unpack(resp[:header_size])
        return {"data": resp[header_size:header_size + returned_count],
                "total_received": total_received, "ring_overrun_count": ring_overrun_count}

    # -- Events -----------------------------------------------------------------------

    def poll_event(self, timeout: float = 0.0):
        """Returns the next unsolicited EVENT frame as {"opcode":, "status":,
        "payload":}, or None if none arrived within `timeout` seconds.
        Currently emitted: digital-capture-complete (opcode=DIGITAL_CAPTURE_STOP),
        ADC block-ready (opcode=ADC_SCAN_READ), and debug-probe session
        start/end (opcode=DEBUG_SESSION_EVENT, see parse_debug_session_event)."""
        frame = self._t.poll_event(timeout=timeout)
        if frame is None:
            return None
        return {"opcode": frame["opcode"], "status": frame["status"], "payload": frame["payload"]}


def parse_debug_session_event(event):
    """Decodes a DEBUG_SESSION_EVENT payload from poll_event()."""
    active, mode, end_reason, _reserved, session_count = \
        pc.DEBUG_SESSION_EVENT_STRUCT.unpack(event["payload"])
    return {"active": bool(active), "mode": pc.DEBUG_MODE_NAMES.get(mode, mode),
            "end_reason": pc.DEBUG_END_REASON_NAMES.get(end_reason, end_reason),
            "session_count": session_count}
