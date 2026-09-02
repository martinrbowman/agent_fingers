"""Owns the one Device connection and runs every USB call on a dedicated
QThread, off the UI thread. pyusb calls block, and Device is not meant to
be touched from more than one thread at a time (see transport.py's
exclusive-lock/reader-thread design) -- so all device access funnels
through this single worker.

Usage from the UI thread: never call DeviceWorker's methods directly.
Instead emit MainWindow's requestConnect/requestDisconnect/requestInvoke
signals, which Qt auto-queues onto the worker thread since the worker
object lives there. Results/errors come back the same way, queued onto
whichever thread the receiving slot's object lives in (the UI thread).
"""

from PySide6.QtCore import QObject, Signal, Slot

from device import Device, DeviceError, list_devices


class DeviceWorker(QObject):
    connected = Signal(str)          # serial
    connect_failed = Signal(str)     # error message
    disconnected = Signal()
    result_ready = Signal(str, object)  # request_id, result
    error_raised = Signal(str, str)     # request_id, error message
    devices_listed = Signal(list)       # [serial, ...]

    def __init__(self):
        super().__init__()
        self.device = None

    @Slot()
    def refresh_devices(self):
        self.devices_listed.emit([s for s in list_devices() if s])

    @Slot(str)
    def connect_device(self, serial: str):
        try:
            self.device = Device(serial=serial or None)
            self.connected.emit(self.device.serial)
        except DeviceError as e:
            self.device = None
            self.connect_failed.emit(str(e))

    @Slot()
    def disconnect_device(self):
        if self.device is not None:
            self.device.close()
            self.device = None
        self.disconnected.emit()

    @Slot(str, object)
    def invoke(self, request_id: str, fn):
        """fn is a one-argument callable: fn(device) -> result. Runs here,
        on the worker thread, and reports back via result_ready/error_raised
        tagged with request_id so the caller can match responses to calls."""
        if self.device is None:
            self.error_raised.emit(request_id, "not connected")
            return
        try:
            result = fn(self.device)
            self.result_ready.emit(request_id, result)
        except DeviceError as e:
            self.error_raised.emit(request_id, str(e))
        except Exception as e:  # noqa: BLE001 -- surfacing to the UI, not swallowing
            self.error_raised.emit(request_id, f"{type(e).__name__}: {e}")
