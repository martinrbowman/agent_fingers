"""Digital bank (GP0-7): per-channel write/read plus rate-paced capture
with a live waveform plot -- the flagship "generate and capture" panel.
"""

import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QGridLayout, QGroupBox, QHBoxLayout, QLabel, QPushButton, QSpinBox,
    QVBoxLayout, QWidget,
)

from device import protocol_constants as pc

CHANNEL_COLORS = [
    "#e6194B", "#3cb44b", "#ffe119", "#4363d8",
    "#f58231", "#911eb4", "#42d4f4", "#f032e6",
]


def _read_all_samples(dev, capture_id, total_captured):
    """Runs on the worker thread (inside an invoke() callable): loops
    DIGITAL_CAPTURE_READ until every already-captured sample is fetched.
    A single response is capped well under 1024 samples, so a full 1024-
    sample ring needs several round trips."""
    chunk = 400
    offset = 0
    out = bytearray()
    while offset < total_captured:
        result = dev.digital_capture_read(capture_id, offset, chunk)
        got = result["samples"]
        if not got:
            break
        out += got
        offset += len(got)
    return bytes(out)


class DigitalIOTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._capture_id = None
        self._capture_bounded = False

        layout = QHBoxLayout(self)
        layout.addWidget(self._build_channels_box(), 0)
        layout.addWidget(self._build_capture_box(), 1)

        ctx.connectionChanged.connect(self._on_connection_changed)
        ctx.eventReceived.connect(self._on_event)
        self._on_connection_changed(ctx.is_connected())

    # -- channels ---------------------------------------------------------

    def _build_channels_box(self):
        box = QGroupBox("Channels (GP0-7)")
        grid = QGridLayout(box)
        grid.addWidget(QLabel("<b>Pin</b>"), 0, 0)
        grid.addWidget(QLabel("<b>Write</b>"), 0, 1)
        grid.addWidget(QLabel("<b>Read</b>"), 0, 2)

        self._write_buttons = []
        self._read_leds = []
        for ch in range(8):
            grid.addWidget(QLabel(f"GP{ch}"), ch + 1, 0)

            btn = QPushButton("0")
            btn.setCheckable(True)
            btn.setFixedWidth(40)
            btn.clicked.connect(lambda checked, c=ch: self._on_write_clicked(c))
            grid.addWidget(btn, ch + 1, 1)
            self._write_buttons.append(btn)

            led = QLabel()
            led.setFixedSize(18, 18)
            led.setStyleSheet("background:#555; border-radius:9px;")
            grid.addWidget(led, ch + 1, 2)
            self._read_leds.append(led)

        read_btn = QPushButton("Read All")
        read_btn.clicked.connect(self._on_read_all)
        grid.addWidget(read_btn, 9, 0, 1, 3)

        self._channels_widgets = [*self._write_buttons, read_btn]
        return box

    def _on_write_clicked(self, ch):
        btn = self._write_buttons[ch]
        value = 1 if btn.isChecked() else 0
        btn.setText(str(value))
        mask = 1 << ch
        values = value << ch
        self.ctx.invoke(
            lambda dev: dev.digital_write_masked(mask, values),
            on_error=lambda msg: self._write_error(ch, msg),
        )

    def _write_error(self, ch, message):
        self.ctx.statusBar().showMessage(f"GP{ch} write failed: {message}", 5000)

    def _on_read_all(self):
        self.ctx.invoke(lambda dev: dev.digital_read(0xFF), on_success=self._update_leds)

    def _update_leds(self, values):
        for ch, led in enumerate(self._read_leds):
            on = bool(values & (1 << ch))
            color = CHANNEL_COLORS[ch] if on else "#555"
            led.setStyleSheet(f"background:{color}; border-radius:9px;")

    # -- capture ------------------------------------------------------------

    def _build_capture_box(self):
        box = QGroupBox("Capture")
        layout = QVBoxLayout(box)

        controls = QHBoxLayout()
        controls.addWidget(QLabel("Rate (Hz):"))
        self.rate_spin = QSpinBox()
        self.rate_spin.setRange(1, pc.DIGITAL_CAPTURE_MAX_RATE_HZ)
        self.rate_spin.setValue(1000)
        controls.addWidget(self.rate_spin)

        controls.addWidget(QLabel("Max samples (0=unbounded):"))
        self.max_samples_spin = QSpinBox()
        self.max_samples_spin.setRange(0, 1024)
        self.max_samples_spin.setValue(200)
        controls.addWidget(self.max_samples_spin)

        self.start_btn = QPushButton("Start")
        self.start_btn.clicked.connect(self._on_start)
        controls.addWidget(self.start_btn)

        self.stop_btn = QPushButton("Stop")
        self.stop_btn.clicked.connect(self._on_stop)
        self.stop_btn.setEnabled(False)
        controls.addWidget(self.stop_btn)
        controls.addStretch(1)
        layout.addLayout(controls)

        self.capture_status = QLabel("Idle")
        layout.addWidget(self.capture_status)

        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Sample")
        self.plot.setYRange(-0.5, 8 * 1.5)
        self.plot.getAxis("left").setTicks([[(i * 1.5 + 0.5, f"GP{i}") for i in range(8)]])
        layout.addWidget(self.plot)

        self._capture_widgets = [self.start_btn]
        return box

    def _on_start(self):
        rate_hz = self.rate_spin.value()
        max_samples = self.max_samples_spin.value()
        self.start_btn.setEnabled(False)
        self.ctx.invoke(
            lambda dev: dev.digital_capture_start(rate_hz=rate_hz, max_samples=max_samples),
            on_success=self._on_started,
            on_error=self._on_capture_error,
        )

    def _on_started(self, result):
        self._capture_id = result["capture_id"]
        self._capture_bounded = self.max_samples_spin.value() > 0
        self.capture_status.setText(
            f"Running: capture_id={self._capture_id}, "
            f"granted_rate_hz={result['granted_rate_hz']}"
        )
        self.stop_btn.setEnabled(True)

    def _on_capture_error(self, message):
        self.start_btn.setEnabled(True)
        self.capture_status.setText(f"Start failed: {message}")

    def _on_stop(self):
        self.stop_btn.setEnabled(False)
        self.ctx.invoke(lambda dev: dev.digital_capture_stop(), on_success=self._on_stopped)

    def _on_stopped(self, result):
        self.start_btn.setEnabled(True)
        capture_id = result["capture_id"]
        total = result["total_captured"]
        self.capture_status.setText(f"Stopped: {total} samples captured")
        self._capture_id = None
        if total > 0:
            self.ctx.invoke(
                lambda dev: _read_all_samples(dev, capture_id, total),
                on_success=self._plot_samples,
            )

    def _on_event(self, event):
        # Bounded captures self-complete and emit this EVENT; unbounded
        # captures need an explicit Stop click instead.
        if event["opcode"] == pc.OPCODE_DIGITAL_CAPTURE_STOP and self._capture_id is not None:
            self.stop_btn.setEnabled(False)
            self._on_stopped({
                "capture_id": self._capture_id,
                "total_captured": int.from_bytes(event["payload"][4:8], "little"),
            })

    def _plot_samples(self, samples: bytes):
        self.plot.clear()
        n = len(samples)
        if n == 0:
            return
        for ch in range(8):
            # Manual staircase: each sample becomes a flat segment [i, i+1)
            # at its level, so consecutive differing samples produce a
            # vertical edge instead of a sloped ramp between them.
            xs, ys = [], []
            for i, b in enumerate(samples):
                level = ((b >> ch) & 1) + ch * 1.5
                xs += [i, i + 1]
                ys += [level, level]
            self.plot.plot(xs, ys, pen=pg.mkPen(CHANNEL_COLORS[ch], width=1.5))

    # -- connection state --------------------------------------------------

    def _on_connection_changed(self, connected):
        for w in self._channels_widgets:
            w.setEnabled(connected)
        self.start_btn.setEnabled(connected)
        if not connected:
            self.stop_btn.setEnabled(False)
