"""Low-level framed request/response + event transport over the RP2350
Signal Agent's vendor-bulk USB interface.

This module only knows about frames (magic/CRC/sequence/opcode/status) — it
has no idea what any opcode means. See client.py for the typed API that MCP
tools and the CLI should actually use; per the plan, MCP code must not parse
binary frames or make USB calls directly, and this boundary is how that's
enforced in practice.
"""

import os
import queue
import struct
import tempfile
import threading

import usb.core
import usb.util

from . import protocol_constants as pc

USBD_VID = 0xCAFE
USBD_PID = 0x4001
# TODO: replace with a purchased production VID/PID before shipping —
# matches the placeholder in firmware/transport/usb_descriptors.c.

VENDOR_INTERFACE = 2  # interface 0/1 = CDC, interface 2 = vendor bulk
EP_OUT = 0x03
EP_IN = 0x83

DEFAULT_TIMEOUT_S = 2.0
DEFAULT_RETRIES = 1


class DeviceError(Exception):
    """Base class for every error this package raises."""


class DeviceNotFoundError(DeviceError):
    pass


class DeviceBusyError(DeviceError):
    """Another process already holds the exclusive lock for this serial."""


class RequestTimeoutError(DeviceError):
    pass


class ProtocolError(DeviceError):
    """A malformed or unexpected frame arrived — bad CRC, wrong sequence,
    or a frame that doesn't parse as one at all."""


class DeviceStatusError(DeviceError):
    """The device accepted and parsed the request but reported a
    non-OK outcome (type=ERROR) — e.g. STATUS_MALFORMED, STATUS_BAD_CRC.
    This is a normal, expected outcome for some calls (a NACK'd I2C
    transfer, an unarmed PWM channel), not necessarily a bug — callers are
    expected to catch this where a non-OK status is a legitimate result."""

    def __init__(self, opcode, status):
        self.opcode = opcode
        self.status = status
        super().__init__(f"opcode {opcode} failed with status {status}")


def list_devices():
    """Yields the serial number of every attached device matching this
    project's VID/PID. A None entry means a device was found but its
    serial string couldn't be read (permissions, or no OS driver bound)."""
    for dev in usb.core.find(find_all=True, idVendor=USBD_VID, idProduct=USBD_PID):
        try:
            yield usb.util.get_string(dev, dev.iSerialNumber)
        except (usb.core.USBError, ValueError):
            yield None


class Transport:
    """One instance per physical device. Claims the vendor-bulk interface,
    takes an exclusive OS-level lock keyed by serial number (USB itself
    doesn't stop two host processes from both opening the same device), and
    runs a background reader thread that demultiplexes incoming frames:
    responses are matched to their request by sequence number and delivered
    to whichever call is waiting; frames with sequence=0 (the "unsolicited"
    convention — see protocol.md) are queued as events instead.
    """

    def __init__(self, serial=None, timeout_s=DEFAULT_TIMEOUT_S):
        self._timeout_s = timeout_s
        self._dev = self._find(serial)
        self.serial = self._claim(self._dev)
        self._lock_file = self._acquire_exclusive_lock(self.serial)

        self._sequence = 0
        self._seq_lock = threading.Lock()
        self._pending = {}
        self._pending_lock = threading.Lock()
        self._events = queue.Queue()

        self._stop = threading.Event()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    @staticmethod
    def _find(serial):
        devices = list(usb.core.find(find_all=True, idVendor=USBD_VID, idProduct=USBD_PID))
        if not devices:
            raise DeviceNotFoundError(f"no device found (VID={USBD_VID:#06x} PID={USBD_PID:#06x})")
        if serial is None:
            if len(devices) > 1:
                raise DeviceError(
                    f"{len(devices)} devices attached; pass serial= (or --device) to "
                    "select one explicitly — never silently picking the first one"
                )
            return devices[0]
        for dev in devices:
            try:
                if usb.util.get_string(dev, dev.iSerialNumber) == serial:
                    return dev
            except (usb.core.USBError, ValueError):
                continue
        raise DeviceNotFoundError(f"no device with serial {serial!r}")

    @staticmethod
    def _claim(dev):
        try:
            if dev.is_kernel_driver_active(VENDOR_INTERFACE):
                dev.detach_kernel_driver(VENDOR_INTERFACE)
        except (NotImplementedError, usb.core.USBError):
            pass
        cfg = dev.get_active_configuration()
        intf = cfg[(VENDOR_INTERFACE, 0)]
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
        return usb.util.get_string(dev, dev.iSerialNumber)

    @staticmethod
    def _acquire_exclusive_lock(serial):
        path = os.path.join(tempfile.gettempdir(), f"rp2350-signal-{serial}.lock")
        f = open(path, "w")
        try:
            import fcntl

            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except ImportError:
            # Windows: best-effort only for now — msvcrt.locking() would go
            # here. Not implemented yet; document rather than pretend.
            pass
        except OSError as e:
            f.close()
            raise DeviceBusyError(
                f"device {serial} is already locked by another process"
            ) from e
        return f

    def close(self):
        self._stop.set()
        self._reader_thread.join(timeout=1.0)
        try:
            usb.util.release_interface(self._dev, VENDOR_INTERFACE)
        except usb.core.USBError:
            pass
        self._lock_file.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _next_sequence(self):
        with self._seq_lock:
            self._sequence += 1
            if self._sequence > 0xFFFFFFFF or self._sequence == 0:
                self._sequence = 1
            return self._sequence

    def _reader_loop(self):
        buf = bytearray()
        while not self._stop.is_set():
            try:
                data = self._dev.read(EP_IN, 1024, timeout=200)
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError:
                if self._stop.is_set():
                    return
                continue
            buf.extend(data)
            self._drain_frames(buf)

    def _drain_frames(self, buf):
        while True:
            if len(buf) < pc.HEADER_SIZE:
                return
            magic = struct.unpack_from("<I", buf, 0)[0]
            if magic != pc.MAGIC:
                # Resync: scan forward for the next magic occurrence rather
                # than dropping just one byte at a time, matching the
                # firmware parser's own resync behavior.
                idx = buf.find(struct.pack("<I", pc.MAGIC), 1)
                del buf[: idx if idx >= 0 else len(buf)]
                if idx < 0:
                    return
                continue
            _, _, _, _, _, _, _, payload_len = pc.HEADER_STRUCT.unpack_from(buf, 0)
            total = pc.HEADER_SIZE + payload_len + 4
            if len(buf) < total:
                return
            frame = pc.parse_frame(bytes(buf[:total]))
            del buf[:total]
            self._dispatch(frame)

    def _dispatch(self, frame):
        if frame["sequence"] == 0:
            self._events.put(frame)
            return
        with self._pending_lock:
            q = self._pending.pop(frame["sequence"], None)
        if q is not None:
            q.put(frame)
        # else: response to a request we gave up waiting on (timeout) — drop it.

    def request(self, opcode, payload=b"", retries=DEFAULT_RETRIES, raise_on_error=True):
        """Sends one request, blocks for the matching response (by
        sequence), and returns the parsed frame dict. Raises
        RequestTimeoutError / ProtocolError on transport-level failure, and
        DeviceStatusError if the device replied with type=ERROR (unless
        raise_on_error=False, for callers that want to inspect a non-OK
        status themselves)."""
        last_error = None
        for _ in range(retries + 1):
            seq = self._next_sequence()
            q = queue.Queue(maxsize=1)
            with self._pending_lock:
                self._pending[seq] = q
            try:
                frame = pc.build_frame(seq, opcode, payload)
                self._dev.write(EP_OUT, frame, timeout=int(self._timeout_s * 1000))
                try:
                    resp = q.get(timeout=self._timeout_s)
                except queue.Empty:
                    last_error = RequestTimeoutError(
                        f"opcode {opcode} (seq {seq}) timed out after {self._timeout_s}s"
                    )
                    continue
                if not resp["crc_ok"]:
                    last_error = ProtocolError(f"bad CRC in response to opcode {opcode}")
                    continue
                if raise_on_error and resp["type"] == pc.FRAME_TYPE_ERROR:
                    raise DeviceStatusError(opcode, resp["status"])
                return resp
            finally:
                with self._pending_lock:
                    self._pending.pop(seq, None)
        raise last_error

    def poll_event(self, timeout=0.0):
        """Returns the next queued EVENT frame (parsed dict), or None if
        none arrived within `timeout` seconds."""
        try:
            return self._events.get(timeout=timeout)
        except queue.Empty:
            return None
