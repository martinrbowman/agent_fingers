"""SPI master (GP8-11) transfers and slave-mode (GP12-15) config.
Mode 0 only -- the firmware rejects anything else -- so there's no mode
selector, just a fixed note."""

from PySide6.QtWidgets import (
    QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit, QPushButton,
    QSpinBox, QVBoxLayout, QWidget,
)

from .util import bytes_from_hex, bytes_to_display


class SPITab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        layout = QVBoxLayout(self)
        layout.addWidget(self._build_master_box())
        layout.addWidget(self._build_slave_box())
        layout.addStretch(1)

        self._all_buttons = [self.xfer_btn]
        ctx.connectionChanged.connect(self._on_connection_changed)
        self._on_connection_changed(ctx.is_connected())

    def _build_master_box(self):
        box = QGroupBox("Master transfer (GP8 SCK / GP9 MOSI / GP10 MISO / GP11 CS) -- Mode 0")
        form = QFormLayout(box)

        self.tx_field = QLineEdit()
        self.tx_field.setPlaceholderText("hex bytes to send, e.g. 00 01 02 03")
        form.addRow("TX data:", self.tx_field)

        self.hz_spin = QSpinBox()
        self.hz_spin.setRange(1, 20_000_000)
        self.hz_spin.setValue(100_000)
        form.addRow("Clock (Hz):", self.hz_spin)

        self.xfer_btn = QPushButton("Transfer")
        self.xfer_btn.clicked.connect(self._on_xfer)
        form.addRow(self.xfer_btn)

        self.xfer_result = QLabel("")
        self.xfer_result.setWordWrap(True)
        form.addRow(self.xfer_result)
        return box

    def _on_xfer(self):
        try:
            tx_data = bytes_from_hex(self.tx_field.text())
        except ValueError as e:
            self.ctx.statusBar().showMessage(f"Bad hex: {e}", 4000)
            return
        hz = self.hz_spin.value()
        self.ctx.invoke(
            lambda dev: dev.spi_xfer(tx_data, hz=hz, mode=0),
            on_success=self._on_xfer_result,
            on_error=lambda msg: self.xfer_result.setText(f"Failed: {msg}"),
        )

    def _on_xfer_result(self, result):
        self.xfer_result.setText(
            f"granted_hz={result['granted_hz']}\n"
            f"RX: {bytes_to_display(result['data'])}"
        )

    def _build_slave_box(self):
        box = QGroupBox("Slave mode (GP12 SCK / GP13 MOSI / GP14 MISO / GP15 CS) -- "
                         "fixed repeating 0x00..0x07 TX pattern, no register map")
        form = QFormLayout(box)

        controls = QHBoxLayout()
        self.slave_enable_btn = QPushButton("Enable")
        self.slave_enable_btn.clicked.connect(lambda: self._on_slave_apply(True))
        controls.addWidget(self.slave_enable_btn)

        self.slave_disable_btn = QPushButton("Disable")
        self.slave_disable_btn.clicked.connect(lambda: self._on_slave_apply(False))
        controls.addWidget(self.slave_disable_btn)
        controls.addStretch(1)
        form.addRow(controls)

        self.slave_result = QLabel("")
        form.addRow(self.slave_result)
        return box

    def _on_slave_apply(self, enabled):
        self.ctx.invoke(
            lambda dev: dev.spi_slave_config(enabled),
            on_success=self._on_slave_result,
        )

    def _on_slave_result(self, result):
        self.slave_result.setText(
            f"bytes_received={result['bytes_received']} "
            f"bytes_sent={result['bytes_sent']} (pipeline pre-fill artifact, "
            f"stable ~5 at idle -- see protocol.md)\n"
            f"transaction_count={result['transaction_count']}"
        )

    def _on_connection_changed(self, connected):
        for btn in self._all_buttons:
            btn.setEnabled(connected)
        self.slave_enable_btn.setEnabled(connected)
        self.slave_disable_btn.setEnabled(connected)
