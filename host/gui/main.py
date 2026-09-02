"""Standalone PySide6 GUI for Agent Fingers -- generate and
capture signals on every implemented bus without writing Python. Built on
the same `device.Device` library as the CLI and MCP server; all USB I/O
happens on a worker thread (see worker.py) so the UI never blocks.

Run: rp2350-signal-gui   (installed by host/gui/setup.sh)
"""

import sys
import uuid

from PySide6.QtCore import QTimer, Signal, Slot
from PySide6.QtWidgets import (
    QApplication, QComboBox, QGroupBox, QHBoxLayout, QLabel, QMainWindow,
    QPushButton, QSpinBox, QStatusBar, QTabWidget, QVBoxLayout, QWidget,
)

from gui.thread import start_worker
from gui.tabs.digital_io import DigitalIOTab
from gui.tabs.pwm import PWMTab
from gui.tabs.uart import UARTTab
from gui.tabs.i2c import I2CTab
from gui.tabs.spi import SPITab
from gui.tabs.adc import ADCTab


class MainWindow(QMainWindow):
    # -- signals the UI thread emits; Qt auto-queues these onto the worker
    # thread since DeviceWorker lives there. Never call worker methods
    # directly from UI code -- always go through these, or through invoke().
    requestListDevices = Signal()
    requestConnect = Signal(str)
    requestDisconnect = Signal()
    requestInvoke = Signal(str, object)

    # -- signals tabs subscribe to, so they don't each need their own
    # plumbing to the worker.
    connectionChanged = Signal(bool)
    eventReceived = Signal(dict)

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Agent Fingers")
        self.resize(1000, 700)

        self._pending = {}  # request_id -> (on_success, on_error)
        self._connected = False

        self.thread, self.worker = start_worker()
        self.requestListDevices.connect(self.worker.refresh_devices)
        self.requestConnect.connect(self.worker.connect_device)
        self.requestDisconnect.connect(self.worker.disconnect_device)
        self.requestInvoke.connect(self.worker.invoke)
        self.worker.devices_listed.connect(self._on_devices_listed)
        self.worker.connected.connect(self._on_connected)
        self.worker.connect_failed.connect(self._on_connect_failed)
        self.worker.disconnected.connect(self._on_disconnected)
        self.worker.result_ready.connect(self._on_result)
        self.worker.error_raised.connect(self._on_error)

        self.setStatusBar(QStatusBar())
        self._build_connection_bar()
        self._build_tabs()

        self._status_timer = QTimer(self)
        self._status_timer.timeout.connect(self._poll_status)
        self._status_timer.start(1000)

        self._event_timer = QTimer(self)
        self._event_timer.timeout.connect(self._poll_event)
        self._event_timer.start(150)

        self.requestListDevices.emit()

    def closeEvent(self, event):
        self._status_timer.stop()
        self._event_timer.stop()
        self.requestDisconnect.emit()
        self.thread.quit()
        self.thread.wait(2000)
        super().closeEvent(event)

    # -- connection bar -------------------------------------------------

    def _build_connection_bar(self):
        bar = QWidget()
        layout = QHBoxLayout(bar)

        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(220)
        layout.addWidget(QLabel("Device:"))
        layout.addWidget(self.device_combo)

        refresh_btn = QPushButton("Refresh")
        refresh_btn.clicked.connect(lambda: self.requestListDevices.emit())
        layout.addWidget(refresh_btn)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._on_connect_clicked)
        layout.addWidget(self.connect_btn)

        layout.addSpacing(20)
        layout.addWidget(QLabel("Output mask (GP0-7):"))
        self.arm_mask_spin = QSpinBox()
        self.arm_mask_spin.setRange(0, 0xFF)
        self.arm_mask_spin.setDisplayIntegerBase(16)
        self.arm_mask_spin.setPrefix("0x")
        layout.addWidget(self.arm_mask_spin)

        layout.addWidget(QLabel("Lease ms:"))
        self.lease_spin = QSpinBox()
        self.lease_spin.setRange(100, 30000)
        self.lease_spin.setValue(5000)
        layout.addWidget(self.lease_spin)

        self.arm_btn = QPushButton("Arm")
        self.arm_btn.clicked.connect(self._on_arm_clicked)
        self.arm_btn.setEnabled(False)
        layout.addWidget(self.arm_btn)

        self.disarm_btn = QPushButton("Disarm")
        self.disarm_btn.clicked.connect(self._on_disarm_clicked)
        self.disarm_btn.setEnabled(False)
        layout.addWidget(self.disarm_btn)

        layout.addStretch(1)

        wrapper = QGroupBox()
        outer = QVBoxLayout(wrapper)
        outer.addWidget(bar)
        self._status_label = QLabel("Not connected")
        outer.addWidget(self._status_label)

        central = QWidget()
        root = QVBoxLayout(central)
        root.addWidget(wrapper)
        self._central_layout = root
        self.setCentralWidget(central)

    def _build_tabs(self):
        self.tabs = QTabWidget()
        self.digital_tab = DigitalIOTab(self)
        self.tabs.addTab(self.digital_tab, "Digital I/O")
        self.tabs.addTab(PWMTab(self), "PWM")
        self.tabs.addTab(UARTTab(self), "UART")
        self.tabs.addTab(I2CTab(self), "I2C")
        self.tabs.addTab(SPITab(self), "SPI")
        self.tabs.addTab(ADCTab(self), "ADC")
        self._central_layout.addWidget(self.tabs)

    # -- invoke: the one path tabs use to call the device --------------

    def invoke(self, fn, on_success=None, on_error=None):
        """fn(device) -> result, runs on the worker thread. on_success(result)
        and on_error(message) run back on the UI thread. Returns the
        request_id (rarely needed by callers)."""
        request_id = str(uuid.uuid4())
        if on_success is not None or on_error is not None:
            self._pending[request_id] = (on_success, on_error)
        self.requestInvoke.emit(request_id, fn)
        return request_id

    def is_connected(self) -> bool:
        return self._connected

    # -- connection lifecycle -------------------------------------------

    def _on_connect_clicked(self):
        if self._connected:
            self.requestDisconnect.emit()
        else:
            serial = self.device_combo.currentText() or None
            self.connect_btn.setEnabled(False)
            self.requestConnect.emit(serial or "")

    @Slot(list)
    def _on_devices_listed(self, serials):
        current = self.device_combo.currentText()
        self.device_combo.clear()
        self.device_combo.addItems(serials)
        if current in serials:
            self.device_combo.setCurrentText(current)

    @Slot(str)
    def _on_connected(self, serial):
        self._connected = True
        self.connect_btn.setText("Disconnect")
        self.connect_btn.setEnabled(True)
        self.arm_btn.setEnabled(True)
        self.disarm_btn.setEnabled(True)
        self._status_label.setText(f"Connected: {serial}")
        self.connectionChanged.emit(True)

    @Slot(str)
    def _on_connect_failed(self, message):
        self.connect_btn.setEnabled(True)
        self.statusBar().showMessage(f"Connect failed: {message}", 5000)

    @Slot()
    def _on_disconnected(self):
        self._connected = False
        self.connect_btn.setText("Connect")
        self.connect_btn.setEnabled(True)
        self.arm_btn.setEnabled(False)
        self.disarm_btn.setEnabled(False)
        self._status_label.setText("Not connected")
        self.connectionChanged.emit(False)

    def _on_arm_clicked(self):
        mask = self.arm_mask_spin.value()
        lease_ms = self.lease_spin.value()
        self.invoke(
            lambda dev: dev.arm_outputs(mask, lease_ms=lease_ms),
            on_success=lambda granted: self.statusBar().showMessage(
                f"Armed 0x{mask:02X}, lease {granted}ms", 3000),
        )

    def _on_disarm_clicked(self):
        self.invoke(
            lambda dev: dev.disarm_outputs(),
            on_success=lambda _: self.statusBar().showMessage("Disarmed", 3000),
        )

    # -- generic result/error dispatch -----------------------------------

    @Slot(str, object)
    def _on_result(self, request_id, result):
        callbacks = self._pending.pop(request_id, None)
        if callbacks and callbacks[0] is not None:
            callbacks[0](result)

    @Slot(str, str)
    def _on_error(self, request_id, message):
        callbacks = self._pending.pop(request_id, None)
        if callbacks and callbacks[1] is not None:
            callbacks[1](message)
        else:
            self.statusBar().showMessage(f"Error: {message}", 5000)

    # -- periodic polling --------------------------------------------------

    def _poll_status(self):
        if not self._connected:
            return
        self.invoke(lambda dev: dev.get_status(), on_success=self._update_status_label)

    def _update_status_label(self, status):
        armed = "armed 0x%02X" % status["armed_mask"] if status["outputs_armed"] else "disarmed"
        self._status_label.setText(
            f"Connected -- uptime {status['uptime_us'] / 1e6:.1f}s, {armed}"
        )

    def _poll_event(self):
        if not self._connected:
            return
        self.invoke(lambda dev: dev.poll_event(timeout=0.0), on_success=self._dispatch_event)

    def _dispatch_event(self, event):
        if event is not None:
            self.eventReceived.emit(event)


def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
