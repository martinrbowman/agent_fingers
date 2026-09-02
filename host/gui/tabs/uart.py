"""UART0 (GP16 TX / GP17 RX): configure, write, and read with a hex/ASCII
toggle. No loopback wired as of this GUI's authoring -- write and read are
independent actions, not a round-trip."""

from PySide6.QtWidgets import (
    QComboBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
    QPushButton, QSpinBox, QVBoxLayout, QWidget,
)

from .util import bytes_from_hex, bytes_to_display


class UARTTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        layout = QVBoxLayout(self)
        layout.addWidget(self._build_config_box())
        layout.addWidget(self._build_write_box())
        layout.addWidget(self._build_read_box())
        layout.addStretch(1)

        self._all_buttons = [self.config_btn, self.write_btn, self.read_btn]
        ctx.connectionChanged.connect(self._on_connection_changed)
        self._on_connection_changed(ctx.is_connected())

    def _build_config_box(self):
        box = QGroupBox("Configure")
        form = QFormLayout(box)

        self.baud_spin = QSpinBox()
        self.baud_spin.setRange(300, 921600)
        self.baud_spin.setValue(115200)
        form.addRow("Baud:", self.baud_spin)

        self.data_bits_combo = QComboBox()
        self.data_bits_combo.addItems(["5", "6", "7", "8"])
        self.data_bits_combo.setCurrentText("8")
        form.addRow("Data bits:", self.data_bits_combo)

        self.stop_bits_combo = QComboBox()
        self.stop_bits_combo.addItems(["1", "2"])
        form.addRow("Stop bits:", self.stop_bits_combo)

        self.parity_combo = QComboBox()
        self.parity_combo.addItems(["None", "Even", "Odd"])
        form.addRow("Parity:", self.parity_combo)

        self.config_btn = QPushButton("Configure")
        self.config_btn.clicked.connect(self._on_configure)
        form.addRow(self.config_btn)

        self.config_result = QLabel("")
        form.addRow(self.config_result)
        return box

    def _on_configure(self):
        baud = self.baud_spin.value()
        data_bits = int(self.data_bits_combo.currentText())
        stop_bits = int(self.stop_bits_combo.currentText())
        parity = self.parity_combo.currentIndex()
        self.ctx.invoke(
            lambda dev: dev.uart_config(baud, data_bits, stop_bits, parity),
            on_success=lambda granted: self.config_result.setText(f"Granted baud: {granted}"),
            on_error=lambda msg: self.config_result.setText(f"Failed: {msg}"),
        )

    def _build_write_box(self):
        box = QGroupBox("Write")
        layout = QHBoxLayout(box)
        self.write_field = QLineEdit()
        self.write_field.setPlaceholderText("hex bytes, e.g. 48 65 6c 6c 6f")
        layout.addWidget(self.write_field)
        self.write_btn = QPushButton("Write")
        self.write_btn.clicked.connect(self._on_write)
        layout.addWidget(self.write_btn)
        return box

    def _on_write(self):
        try:
            data = bytes_from_hex(self.write_field.text())
        except ValueError as e:
            self.ctx.statusBar().showMessage(f"Bad hex: {e}", 4000)
            return
        self.ctx.invoke(
            lambda dev: dev.uart_write(data),
            on_success=lambda _: self.ctx.statusBar().showMessage(
                f"Wrote {len(data)} bytes", 3000),
            on_error=lambda msg: self.ctx.statusBar().showMessage(
                f"Write failed: {msg}", 4000),
        )

    def _build_read_box(self):
        box = QGroupBox("Read")
        layout = QVBoxLayout(box)
        controls = QHBoxLayout()
        controls.addWidget(QLabel("Max bytes:"))
        self.max_bytes_spin = QSpinBox()
        self.max_bytes_spin.setRange(1, 480)
        self.max_bytes_spin.setValue(64)
        controls.addWidget(self.max_bytes_spin)
        self.read_btn = QPushButton("Read")
        self.read_btn.clicked.connect(self._on_read)
        controls.addWidget(self.read_btn)
        controls.addStretch(1)
        layout.addLayout(controls)

        self.read_result = QLabel("")
        self.read_result.setWordWrap(True)
        layout.addWidget(self.read_result)
        return box

    def _on_read(self):
        max_bytes = self.max_bytes_spin.value()
        self.ctx.invoke(lambda dev: dev.uart_read(max_bytes), on_success=self._on_read_result)

    def _on_read_result(self, result):
        self.read_result.setText(
            f"Data: {bytes_to_display(result['data'])}\n"
            f"framing={result['framing_errors']} parity={result['parity_errors']} "
            f"break={result['break_errors']} overrun={result['overrun_errors']} "
            f"ring_overrun={result['ring_overrun_count']}"
        )

    def _on_connection_changed(self, connected):
        for btn in self._all_buttons:
            btn.setEnabled(connected)
