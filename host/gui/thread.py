from PySide6.QtCore import QThread

from gui.worker import DeviceWorker


def start_worker():
    """Creates and starts the single background thread all Device I/O runs
    on. Caller must keep both returned objects alive for the app's
    lifetime (assign to attributes, not locals)."""
    thread = QThread()
    worker = DeviceWorker()
    worker.moveToThread(thread)
    thread.start()
    return thread, worker
