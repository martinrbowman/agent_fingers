"""Hand-maintained Python mirror of firmware/protocol/protocol.h.

Source of truth is protocol.md at the repo root — keep this file, protocol.h,
and protocol.md in sync by hand until a generator exists.
"""

import struct

MAGIC = 0x5349474C
VERSION = 1
HEADER_SIZE = 20
MAX_PAYLOAD = 16384
RX_PAYLOAD_CAP = 8192  # request/response payload buffer size, see protocol.md

# magic,version,type,flags,sequence,opcode,status,payload_len
HEADER_STRUCT = struct.Struct("<IBBHIHHI")
assert HEADER_STRUCT.size == HEADER_SIZE

FRAME_TYPE_REQUEST = 1
FRAME_TYPE_RESPONSE = 2
FRAME_TYPE_EVENT = 3
FRAME_TYPE_ERROR = 4

FRAME_FLAG_ACK_REQUIRED = 1 << 0
FRAME_FLAG_MORE = 1 << 1
FRAME_FLAG_IDEMPOTENT = 1 << 2
FRAME_FLAG_ARMED = 1 << 3

STATUS_OK = 0
STATUS_UNSUPPORTED_OPCODE = 1
STATUS_BAD_VERSION = 2
STATUS_BAD_LENGTH = 3
STATUS_BAD_CRC = 4
STATUS_MALFORMED = 5
STATUS_PAYLOAD_TOO_LARGE = 6
STATUS_INTERNAL_ERROR = 7
STATUS_RESOURCE_BUSY = 8  # pins owned by a debug-probe session

OPCODE_GET_INFO = 1
OPCODE_GET_CAPABILITIES = 2
OPCODE_GET_STATUS = 3
OPCODE_SET_SAFE_STATE = 4
OPCODE_DIGITAL_CHANNEL_CONFIG = 5
OPCODE_ARM_OUTPUTS = 6
OPCODE_DISARM_OUTPUTS = 7
OPCODE_DIGITAL_READ = 8
OPCODE_DIGITAL_WRITE_MASKED = 9
OPCODE_DIGITAL_CAPTURE_START = 10
OPCODE_DIGITAL_CAPTURE_STOP = 11
OPCODE_DIGITAL_CAPTURE_READ = 12
OPCODE_PWM_CONFIG = 13
OPCODE_ADC_SCAN_CONFIG = 14
OPCODE_ADC_SCAN_READ = 15
OPCODE_I2C_XFER = 16
OPCODE_I2C_SLAVE_CONFIG = 17
OPCODE_UART_CONFIG = 18
OPCODE_UART_WRITE = 19
OPCODE_UART_READ = 20
OPCODE_SPI_XFER = 21
OPCODE_SPI_SLAVE_CONFIG = 22
OPCODE_PING = 23
OPCODE_RESET_TO_BOOTSEL = 24
OPCODE_SPI_SLAVE_READ = 25
OPCODE_DEBUG_SESSION_EVENT = 26  # EVENT-only: debug-probe session started/ended

# Request/response payload layouts for the opcodes implemented so far.
INFO_RESPONSE_STRUCT = struct.Struct("<BBBB8s16s")
CAPABILITIES_RESPONSE_STRUCT = struct.Struct("<BBBBBBBB")
STATUS_RESPONSE_STRUCT = struct.Struct("<QBBBBIIIIIII16sBBBBI")

# active,mode,end_reason,reserved,session_count
DEBUG_SESSION_EVENT_STRUCT = struct.Struct("<BBBBI")
DEBUG_MODE_NAMES = {0: "none", 1: "swd", 2: "jtag"}
DEBUG_END_REASON_NAMES = {0: "none", 1: "disconnect", 2: "timeout", 3: "usb"}

ARM_OUTPUTS_REQUEST_STRUCT = struct.Struct("<16sB3sI")  # challenge,output_mask,reserved,lease_ms
ARM_OUTPUTS_RESPONSE_STRUCT = struct.Struct("<I")        # granted_lease_ms
DIGITAL_WRITE_MASKED_REQUEST_STRUCT = struct.Struct("<BB")  # mask,values
DIGITAL_READ_REQUEST_STRUCT = struct.Struct("<B")        # mask
DIGITAL_READ_RESPONSE_STRUCT = struct.Struct("<B")       # values

PWM_MAX_FREQUENCY_HZ = 100000

# channel,enabled,reserved,frequency_hz,duty_permille,reserved2
PWM_CONFIG_REQUEST_STRUCT = struct.Struct("<BBHIHH")
# granted_frequency_hz,granted_duty_permille,reserved
PWM_CONFIG_RESPONSE_STRUCT = struct.Struct("<IHH")

DIGITAL_CAPTURE_MAX_RATE_HZ = 200000
DIGITAL_CAPTURE_BUFFER_SAMPLES = 4096

# rate_hz,max_samples
DIGITAL_CAPTURE_START_REQUEST_STRUCT = struct.Struct("<II")
# capture_id,granted_rate_hz,buffer_capacity_samples
DIGITAL_CAPTURE_START_RESPONSE_STRUCT = struct.Struct("<III")
# capture_id,total_captured -- also the capture-complete EVENT payload
DIGITAL_CAPTURE_STOP_RESPONSE_STRUCT = struct.Struct("<II")
# capture_id,offset,limit,reserved
DIGITAL_CAPTURE_READ_REQUEST_STRUCT = struct.Struct("<IIHH")
# capture_id,total_captured,overrun_count,start_timestamp_us,rate_hz,returned_count,reserved
# followed by returned_count raw sample bytes
DIGITAL_CAPTURE_READ_RESPONSE_STRUCT = struct.Struct("<IIIQIHH")

ADC_SCAN_MAX_RATE_HZ = 200000
ADC_SCAN_BUFFER_SAMPLES = 2048
ADC_SCAN_VREF_MILLIVOLTS = 3300

# channel_mask,reserved(3),rate_hz,max_samples
ADC_SCAN_CONFIG_REQUEST_STRUCT = struct.Struct("<B3sII")
# scan_id,granted_rate_hz,channel_count,reserved(3),buffer_capacity_samples,vref_millivolts,reserved2
ADC_SCAN_CONFIG_RESPONSE_STRUCT = struct.Struct("<IIB3sIHH")
# scan_id,total_captured -- also the "ADC block-ready" EVENT payload
ADC_SCAN_COMPLETE_EVENT_STRUCT = struct.Struct("<II")
# scan_id,offset,limit,reserved
ADC_SCAN_READ_REQUEST_STRUCT = struct.Struct("<IIHH")
# scan_id,total_captured,overrun_count,start_timestamp_us,rate_hz,channel_count,reserved,returned_count
# followed by returned_count raw u16 ADC codes (0-4095)
ADC_SCAN_READ_RESPONSE_STRUCT = struct.Struct("<IIIQIBBH")

# baud_rate,data_bits,stop_bits,parity,reserved
UART_CONFIG_REQUEST_STRUCT = struct.Struct("<IBBBB")
# granted_baud_rate
UART_CONFIG_RESPONSE_STRUCT = struct.Struct("<I")
# UART_WRITE request is the raw bytes to send, no wrapper struct.
# max_bytes
UART_READ_REQUEST_STRUCT = struct.Struct("<H")
# framing_errors,parity_errors,break_errors,overrun_errors,ring_overrun_count,returned_count,reserved
# followed by returned_count raw data bytes
UART_READ_RESPONSE_STRUCT = struct.Struct("<IIIIIHH")

# address,reserved,write_len,read_len,timeout_ms -- followed by write_len raw bytes
I2C_XFER_REQUEST_STRUCT = struct.Struct("<BBHHH")
# write_status,read_status,bytes_written,bytes_read -- followed by bytes_read raw bytes
I2C_XFER_RESPONSE_STRUCT = struct.Struct("<BBHH")

# enabled,address,reserved
I2C_SLAVE_CONFIG_REQUEST_STRUCT = struct.Struct("<BBH")
# bytes_received,bytes_sent,transaction_count
I2C_SLAVE_CONFIG_RESPONSE_STRUCT = struct.Struct("<III")

SPI_MASTER_MIN_HZ = 10000
SPI_MASTER_MAX_HZ = 1000000

# mode,reserved,hz,len -- followed by len raw TX bytes
SPI_XFER_REQUEST_STRUCT = struct.Struct("<BBIH")
# granted_hz,len,reserved -- followed by len raw RX bytes
SPI_XFER_RESPONSE_STRUCT = struct.Struct("<IHH")

# enabled,reserved
SPI_SLAVE_CONFIG_REQUEST_STRUCT = struct.Struct("<B3s")
# bytes_received,bytes_sent,transaction_count
SPI_SLAVE_CONFIG_RESPONSE_STRUCT = struct.Struct("<III")

SPI_SLAVE_READ_REQUEST_STRUCT = struct.Struct("<H")  # max_bytes
# total_received,ring_overrun_count,returned_count,reserved -- followed by
# returned_count raw MOSI bytes
SPI_SLAVE_READ_RESPONSE_STRUCT = struct.Struct("<IIHH")


def crc32c(data: bytes) -> int:
    """Standard CRC32C (Castagnoli): poly 0x1EDC6F41 reflected 0x82F63B78,
    init/final XOR 0xFFFFFFFF. Matches firmware/protocol/crc32c.c exactly."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x82F63B78 if (crc & 1) else (crc >> 1)
    return crc ^ 0xFFFFFFFF


def build_frame(sequence: int, opcode: int, payload: bytes = b"",
                 frame_type: int = FRAME_TYPE_REQUEST, flags: int = 0,
                 status: int = 0) -> bytes:
    header = HEADER_STRUCT.pack(MAGIC, VERSION, frame_type, flags, sequence,
                                 opcode, status, len(payload))
    body = header + payload
    return body + struct.pack("<I", crc32c(body))


def parse_frame(buf: bytes) -> dict:
    header = buf[:HEADER_SIZE]
    magic, version, frame_type, flags, sequence, opcode, status, payload_len = \
        HEADER_STRUCT.unpack(header)
    payload = buf[HEADER_SIZE:HEADER_SIZE + payload_len]
    crc_received = struct.unpack("<I", buf[HEADER_SIZE + payload_len:HEADER_SIZE + payload_len + 4])[0]
    crc_ok = crc_received == crc32c(buf[:HEADER_SIZE + payload_len])
    return dict(magic=magic, version=version, type=frame_type, flags=flags,
                sequence=sequence, opcode=opcode, status=status,
                payload=payload, crc_ok=crc_ok)
