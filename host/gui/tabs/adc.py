"""ADC round-robin scan (GP26-28 / ADC0-2), with a live plot in millivolts."""

import pyqtgraph as pg
from PySide6.QtWidgets import (
    QCheckBox, QGroupBox, QHBoxLayout, QLabel, QPushButton, QSpinBox,
    QVBoxLayout, QWidget,
)

from device import protocol_constants as pc

CHANNEL_COLORS = ["#e6194B", "#3cb44b", "#4363d8"]


def _read_all_samples(dev, scan_id, total_captured, channel_count):
    chunk = 300
    offset = 0
    out = []
    while offset < total_captured:
        result = dev.adc_scan_read(scan_id, offset, chunk)
        got = result["samples"]
        if not got:
            break
        out.extend(got)
        offset += len(got)
    return out


class ADCTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._scan_id = None
        self._channel_count = 0
        self._vref_mv = 3300

        layout = QVBoxLayout(self)

        controls_box = QGroupBox("Scan (GP26-28)")
        controls = QVBoxLayout(controls_box)

        channel_row = QHBoxLayout()
        channel_row.addWidget(QLabel("Channels:"))
        self.channel_checks = []
        for ch in range(3):
            cb = QCheckBox(f"ADC{ch} (GP{26 + ch})")
            cb.setChecked(True)
            channel_row.addWidget(cb)
            self.channel_checks.append(cb)
        channel_row.addStretch(1)
        controls.addLayout(channel_row)

        rate_row = QHBoxLayout()
        rate_row.addWidget(QLabel("Rate (Hz):"))
        self.rate_spin = QSpinBox()
        self.rate_spin.setRange(1, 500000)
        self.rate_spin.setValue(1000)
        rate_row.addWidget(self.rate_spin)

        rate_row.addWidget(QLabel("Max samples/channel (0=unbounded):"))
        self.max_samples_spin = QSpinBox()
        self.max_samples_spin.setRange(0, 4096)
        self.max_samples_spin.setValue(200)
        rate_row.addWidget(self.max_samples_spin)

        self.start_btn = QPushButton("Start")
        self.start_btn.clicked.connect(self._on_start)
        rate_row.addWidget(self.start_btn)

        self.stop_btn = QPushButton("Stop")
        self.stop_btn.clicked.connect(self._on_stop)
        self.stop_btn.setEnabled(False)
        rate_row.addWidget(self.stop_btn)
        rate_row.addStretch(1)
        controls.addLayout(rate_row)

        self.status_label = QLabel("Idle")
        controls.addWidget(self.status_label)
        layout.addWidget(controls_box)

        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Sample")
        self.plot.setLabel("left", "mV")
        layout.addWidget(self.plot)

        ctx.connectionChanged.connect(self._on_connection_changed)
        ctx.eventReceived.connect(self._on_event)
        self._on_connection_changed(ctx.is_connected())

    def _channel_mask(self):
        mask = 0
        for ch, cb in enumerate(self.channel_checks):
            if cb.isChecked():
                mask |= 1 << ch
        return mask

    def _on_start(self):
        mask = self._channel_mask()
        if mask == 0:
            self.status_label.setText("Select at least one channel")
            return
        rate_hz = self.rate_spin.value()
        max_samples = self.max_samples_spin.value()
        self.start_btn.setEnabled(False)
        self.ctx.invoke(
            lambda dev: dev.adc_scan_config(mask, rate_hz, max_samples),
            on_success=self._on_started,
            on_error=self._on_start_error,
        )

    def _on_started(self, result):
        self._scan_id = result["scan_id"]
        self._channel_count = result["channel_count"]
        self._vref_mv = result["vref_millivolts"]
        self.status_label.setText(
            f"Running: scan_id={self._scan_id}, "
            f"granted_rate_hz={result['granted_rate_hz']}, "
            f"channels={self._channel_count}"
        )
        self.stop_btn.setEnabled(True)

    def _on_start_error(self, message):
        self.start_btn.setEnabled(True)
        self.status_label.setText(f"Start failed: {message}")

    def _on_stop(self):
        self.stop_btn.setEnabled(False)
        self.ctx.invoke(lambda dev: dev.adc_scan_config(0, 0, 0), on_success=self._on_stopped)

    def _on_stopped(self, result):
        self.start_btn.setEnabled(True)
        # adc_scan_config(0,0,0) doubles as stop; its own response isn't the
        # completed scan's sample count, so pull the real total from a read.
        if self._scan_id is not None:
            scan_id = self._scan_id
            channel_count = self._channel_count
            self.ctx.invoke(
                lambda dev: dev.adc_scan_read(scan_id, 0, 0),
                on_success=lambda r: self._fetch_and_plot(scan_id, r["total_captured"], channel_count),
                on_error=lambda msg: self.status_label.setText(f"Stopped (read failed: {msg})"),
            )
        self._scan_id = None

    def _on_event(self, event):
        if event["opcode"] == pc.OPCODE_ADC_SCAN_READ and self._scan_id is not None:
            scan_id, total_captured = pc.ADC_SCAN_COMPLETE_EVENT_STRUCT.unpack(event["payload"])
            self.stop_btn.setEnabled(False)
            self.start_btn.setEnabled(True)
            channel_count = self._channel_count
            self._scan_id = None
            self.status_label.setText(f"Complete: {total_captured} samples/channel")
            self._fetch_and_plot(scan_id, total_captured, channel_count)

    def _fetch_and_plot(self, scan_id, total_captured, channel_count):
        if total_captured <= 0 or channel_count <= 0:
            return
        total_raw_samples = total_captured * channel_count
        self.ctx.invoke(
            lambda dev: _read_all_samples(dev, scan_id, total_raw_samples, channel_count),
            on_success=lambda samples: self._plot_samples(samples, channel_count),
        )

    def _plot_samples(self, samples, channel_count):
        self.plot.clear()
        if not samples or channel_count <= 0:
            return
        scale = self._vref_mv / 4095.0
        for ch in range(channel_count):
            channel_samples = samples[ch::channel_count]
            ys = [s * scale for s in channel_samples]
            xs = list(range(len(ys)))
            color = CHANNEL_COLORS[ch % len(CHANNEL_COLORS)]
            self.plot.plot(xs, ys, pen=pg.mkPen(color, width=1.5), name=f"ADC{ch}")

    def _on_connection_changed(self, connected):
        self.start_btn.setEnabled(connected)
        if not connected:
            self.stop_btn.setEnabled(False)
