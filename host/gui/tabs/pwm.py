"""PWM output on any GP0-7 channel. Reuses the digital bank's arm/lease --
the channel must be armed as an output (top toolbar) before PWM_CONFIG
will take effect, same as the firmware's own DIGITAL_WRITE_MASKED guard."""

from PySide6.QtWidgets import (
    QCheckBox, QFormLayout, QLabel, QPushButton, QSpinBox, QVBoxLayout,
    QWidget,
)

from device import protocol_constants as pc


class PWMTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(
            "Channel must already be armed as output (top toolbar) or "
            "PWM_CONFIG is rejected."
        ))

        form = QFormLayout()
        self.channel_spin = QSpinBox()
        self.channel_spin.setRange(0, 7)
        form.addRow("Channel (GP):", self.channel_spin)

        self.enabled_check = QCheckBox("Enabled")
        self.enabled_check.setChecked(True)
        form.addRow(self.enabled_check)

        self.freq_spin = QSpinBox()
        self.freq_spin.setRange(1, pc.PWM_MAX_FREQUENCY_HZ)
        self.freq_spin.setValue(1000)
        form.addRow("Frequency (Hz):", self.freq_spin)

        self.duty_spin = QSpinBox()
        self.duty_spin.setRange(0, 1000)
        self.duty_spin.setValue(500)
        form.addRow("Duty (permille):", self.duty_spin)

        layout.addLayout(form)

        self.apply_btn = QPushButton("Apply")
        self.apply_btn.clicked.connect(self._on_apply)
        layout.addWidget(self.apply_btn)

        self.result_label = QLabel("")
        layout.addWidget(self.result_label)
        layout.addStretch(1)

        ctx.connectionChanged.connect(self.apply_btn.setEnabled)
        self.apply_btn.setEnabled(ctx.is_connected())

    def _on_apply(self):
        channel = self.channel_spin.value()
        enabled = self.enabled_check.isChecked()
        freq = self.freq_spin.value()
        duty = self.duty_spin.value()
        self.ctx.invoke(
            lambda dev: dev.pwm_config(channel, enabled, freq, duty),
            on_success=self._on_result,
            on_error=lambda msg: self.result_label.setText(f"Failed: {msg}"),
        )

    def _on_result(self, result):
        self.result_label.setText(
            f"Granted: {result['granted_frequency_hz']} Hz, "
            f"{result['granted_duty_permille']}/1000 duty"
        )
