"""I2C master (GP20/21) transfers and slave-mode (GP18/19) config."""

from PySide6.QtWidgets import (
    QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit, QPushButton,
    QSpinBox, QVBoxLayout, QWidget,
)

from .util import bytes_from_hex, bytes_to_display


class I2CTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        layout = QVBoxLayout(self)
        layout.addWidget(self._build_master_box())
        layout.addWidget(self._build_slave_box())
        layout.addStretch(1)

        self._all_buttons = [self.xfer_btn, self.slave_apply_btn]
        ctx.connectionChanged.connect(self._on_connection_changed)
        self._on_connection_changed(ctx.is_connected())

    def _build_master_box(self):
        box = QGroupBox("Master transfer (GP20 SDA / GP21 SCL)")
        form = QFormLayout(box)

        self.address_spin = QSpinBox()
        self.address_spin.setRange(0, 0x7F)
        self.address_spin.setDisplayIntegerBase(16)
        self.address_spin.setPrefix("0x")
        self.address_spin.setValue(0x50)
        form.addRow("Address:", self.address_spin)

        self.write_field = QLineEdit()
        self.write_field.setPlaceholderText("hex bytes to write, e.g. 00")
        form.addRow("Write data:", self.write_field)

        self.read_len_spin = QSpinBox()
        self.read_len_spin.setRange(0, 480)
        form.addRow("Read length:", self.read_len_spin)

        self.timeout_spin = QSpinBox()
        self.timeout_spin.setRange(0, 5000)
        self.timeout_spin.setValue(200)
        form.addRow("Timeout (ms):", self.timeout_spin)

        self.xfer_btn = QPushButton("Transfer")
        self.xfer_btn.clicked.connect(self._on_xfer)
        form.addRow(self.xfer_btn)

        self.xfer_result = QLabel("")
        self.xfer_result.setWordWrap(True)
        form.addRow(self.xfer_result)
        return box

    def _on_xfer(self):
        try:
            write_data = bytes_from_hex(self.write_field.text())
        except ValueError as e:
            self.ctx.statusBar().showMessage(f"Bad hex: {e}", 4000)
            return
        address = self.address_spin.value()
        read_len = self.read_len_spin.value()
        timeout_ms = self.timeout_spin.value()
        self.ctx.invoke(
            lambda dev: dev.i2c_xfer(address, write_data, read_len, timeout_ms),
            on_success=self._on_xfer_result,
        )

    def _on_xfer_result(self, result):
        self.xfer_result.setText(
            f"write_status={result['write_status']} read_status={result['read_status']} "
            f"bytes_written={result['bytes_written']}\n"
            f"data: {bytes_to_display(result['data'])}"
        )

    def _build_slave_box(self):
        box = QGroupBox("Slave mode (GP18 SDA / GP19 SCL)")
        form = QFormLayout(box)

        controls = QHBoxLayout()
        self.slave_address_spin = QSpinBox()
        self.slave_address_spin.setRange(0, 0x7F)
        self.slave_address_spin.setDisplayIntegerBase(16)
        self.slave_address_spin.setPrefix("0x")
        self.slave_address_spin.setValue(0x42)
        controls.addWidget(QLabel("Address:"))
        controls.addWidget(self.slave_address_spin)

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

        self.slave_apply_btn = self.slave_enable_btn  # for enable/disable toggling
        return box

    def _on_slave_apply(self, enabled):
        address = self.slave_address_spin.value()
        self.ctx.invoke(
            lambda dev: dev.i2c_slave_config(enabled, address),
            on_success=self._on_slave_result,
        )

    def _on_slave_result(self, result):
        self.slave_result.setText(
            f"bytes_received={result['bytes_received']} "
            f"bytes_sent={result['bytes_sent']} "
            f"transaction_count={result['transaction_count']}"
        )

    def _on_connection_changed(self, connected):
        for btn in self._all_buttons:
            btn.setEnabled(connected)
        self.slave_disable_btn.setEnabled(connected)
