"""Regenerate tests/protocol/golden_vectors/ from protocol_constants.py.

Run after any wire-format change:
    python3 tests/protocol/generate_golden_vectors.py

These are encoder/decoder fixtures (synthetic, not device captures) for the
C parser tests and the Python host client to check against, per plan step 4
("Include golden binary vectors").
"""

import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "host" / "device"))
import protocol_constants as pc

OUT_DIR = pathlib.Path(__file__).resolve().parent / "golden_vectors"


def main():
    OUT_DIR.mkdir(exist_ok=True)
    manifest = []

    def emit(name, frame, description):
        path = OUT_DIR / f"{name}.bin"
        path.write_bytes(frame)
        manifest.append({"name": name, "file": path.name, "description": description,
                          "size": len(frame)})

    emit("request_get_info", pc.build_frame(1, pc.OPCODE_GET_INFO),
         "Minimal valid request, no payload.")

    emit("request_get_capabilities", pc.build_frame(2, pc.OPCODE_GET_CAPABILITIES),
         "Minimal valid request, no payload.")

    emit("request_ping_echo", pc.build_frame(3, pc.OPCODE_PING, b"golden-vector-echo"),
         "Request with a payload the PING handler must echo back.")

    emit("response_unsupported_opcode",
         pc.build_frame(4, 999, frame_type=pc.FRAME_TYPE_ERROR,
                         status=pc.STATUS_UNSUPPORTED_OPCODE),
         "Synthetic ERROR response for an opcode that doesn't exist (999).")

    resync_prefix = b"\x00\xff garbage bytes before a real frame \x01\x02"
    emit("resync_after_garbage_prefix",
         resync_prefix + pc.build_frame(5, pc.OPCODE_GET_INFO),
         "Garbage bytes followed by a valid frame — parser must resync on "
         "magic and decode the trailing frame, incrementing magic_resyncs.")

    bad_crc = bytearray(pc.build_frame(6, pc.OPCODE_GET_INFO))
    bad_crc[-1] ^= 0xFF
    emit("bad_crc", bytes(bad_crc),
         "Valid header/payload, corrupted trailing CRC byte — must yield "
         "STATUS_BAD_CRC and not dispatch the opcode.")

    arm_payload = pc.ARM_OUTPUTS_REQUEST_STRUCT.pack(b"\x00" * 16, 0b00000011, b"\x00\x00\x00", 3000)
    emit("request_arm_outputs", pc.build_frame(7, pc.OPCODE_ARM_OUTPUTS, arm_payload),
         "ARM_OUTPUTS with an all-zero challenge (never valid against a real "
         "device — for wire-layout decode testing only) requesting GP0/GP1 "
         "as output with a 3000ms lease.")

    write_payload = pc.DIGITAL_WRITE_MASKED_REQUEST_STRUCT.pack(0b00000011, 0b00000001)
    emit("request_digital_write_masked", pc.build_frame(8, pc.OPCODE_DIGITAL_WRITE_MASKED, write_payload),
         "Drive GP0 high, GP1 low, leave the rest untouched.")

    read_payload = pc.DIGITAL_READ_REQUEST_STRUCT.pack(0xFF)
    emit("request_digital_read", pc.build_frame(9, pc.OPCODE_DIGITAL_READ, read_payload),
         "Read all 8 channels.")

    capture_start_payload = pc.DIGITAL_CAPTURE_START_REQUEST_STRUCT.pack(5000, 0)
    emit("request_digital_capture_start",
         pc.build_frame(10, pc.OPCODE_DIGITAL_CAPTURE_START, capture_start_payload),
         "Start an unbounded capture at 5000 Hz.")

    capture_read_payload = pc.DIGITAL_CAPTURE_READ_REQUEST_STRUCT.pack(1, 0, 400, 0)
    emit("request_digital_capture_read",
         pc.build_frame(11, pc.OPCODE_DIGITAL_CAPTURE_READ, capture_read_payload),
         "Read 400 samples from offset 0 of capture_id=1.")

    capture_complete_event = pc.DIGITAL_CAPTURE_STOP_RESPONSE_STRUCT.pack(2, 200)
    emit("event_digital_capture_complete",
         pc.build_frame(0, pc.OPCODE_DIGITAL_CAPTURE_STOP, capture_complete_event,
                         frame_type=pc.FRAME_TYPE_EVENT, status=pc.STATUS_OK),
         "Unsolicited capture-complete EVENT for a bounded capture (id=2, "
         "200/200 samples) — sequence=0 is the unsolicited-frame convention.")

    pwm_payload = pc.PWM_CONFIG_REQUEST_STRUCT.pack(0, 1, 0, 1000, 500, 0)
    emit("request_pwm_config",
         pc.build_frame(20, pc.OPCODE_PWM_CONFIG, pwm_payload),
         "Enable PWM on channel 0 at 1000 Hz, 50% duty (channel must "
         "already be in the ARM_OUTPUTS armed mask on a real device).")

    spi_xfer_payload = pc.SPI_XFER_REQUEST_STRUCT.pack(0, 0, 100000, 4) + bytes([0xDE, 0xAD, 0xBE, 0xEF])
    emit("request_spi_xfer",
         pc.build_frame(26, pc.OPCODE_SPI_XFER, spi_xfer_payload),
         "Mode 0 transfer at 100kHz, 4 bytes full-duplex.")

    spi_slave_payload = pc.SPI_SLAVE_CONFIG_REQUEST_STRUCT.pack(1, b"\x00\x00\x00")
    emit("request_spi_slave_config",
         pc.build_frame(27, pc.OPCODE_SPI_SLAVE_CONFIG, spi_slave_payload),
         "Enable the SPI slave (GP12-15).")

    i2c_xfer_payload = pc.I2C_XFER_REQUEST_STRUCT.pack(0x50, 0, 1, 4, 200) + bytes([0x00])
    emit("request_i2c_xfer",
         pc.build_frame(24, pc.OPCODE_I2C_XFER, i2c_xfer_payload),
         "Write register pointer 0x00 to device 0x50, then read 4 bytes back "
         "(repeated start, combined write-then-read).")

    i2c_slave_payload = pc.I2C_SLAVE_CONFIG_REQUEST_STRUCT.pack(1, 0x42, 0)
    emit("request_i2c_slave_config",
         pc.build_frame(25, pc.OPCODE_I2C_SLAVE_CONFIG, i2c_slave_payload),
         "Enable the I2C slave (i2c1) at address 0x42.")

    uart_config_payload = pc.UART_CONFIG_REQUEST_STRUCT.pack(115200, 8, 1, 0, 0)
    emit("request_uart_config",
         pc.build_frame(21, pc.OPCODE_UART_CONFIG, uart_config_payload),
         "Configure UART0 at 115200 baud, 8N1.")

    emit("request_uart_write",
         pc.build_frame(22, pc.OPCODE_UART_WRITE, b"hello uart"),
         "UART_WRITE payload is the raw bytes to send, no wrapper struct.")

    uart_read_payload = pc.UART_READ_REQUEST_STRUCT.pack(64)
    emit("request_uart_read",
         pc.build_frame(23, pc.OPCODE_UART_READ, uart_read_payload),
         "Read up to 64 bytes from the RX ring.")

    adc_config_payload = pc.ADC_SCAN_CONFIG_REQUEST_STRUCT.pack(0b111, b"\x00\x00\x00", 3000, 0)
    emit("request_adc_scan_config",
         pc.build_frame(12, pc.OPCODE_ADC_SCAN_CONFIG, adc_config_payload),
         "Start an unbounded 3-channel round-robin scan at 3000 Hz total.")

    adc_read_payload = pc.ADC_SCAN_READ_REQUEST_STRUCT.pack(1, 0, 242, 0)
    emit("request_adc_scan_read",
         pc.build_frame(13, pc.OPCODE_ADC_SCAN_READ, adc_read_payload),
         "Read 242 samples (max that fits one 512-byte frame at 2 bytes "
         "each) from offset 0 of scan_id=1.")

    adc_complete_event = pc.ADC_SCAN_COMPLETE_EVENT_STRUCT.pack(3, 300)
    emit("event_adc_scan_complete",
         pc.build_frame(0, pc.OPCODE_ADC_SCAN_READ, adc_complete_event,
                         frame_type=pc.FRAME_TYPE_EVENT, status=pc.STATUS_OK),
         "Unsolicited ADC block-ready EVENT for a bounded scan (id=3, 300 samples).")

    manifest_path = OUT_DIR / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {len(manifest)} vectors to {OUT_DIR}")


if __name__ == "__main__":
    main()
