#!/usr/bin/env python3
"""IQ server GUI — target list, distance profile, RDM (PySide6/PyQt6 + pyqtgraph)."""

from __future__ import annotations

import math
import sys
import time
from dataclasses import dataclass
from enum import Enum, auto
from typing import Literal

import numpy as np

try:
    from PySide6.QtCore import Qt, QThread, Signal, Slot, QRectF
    from PySide6.QtGui import QColor, QFont, QPainter, QPen
    from PySide6.QtWidgets import (
        QApplication,
        QComboBox,
        QDoubleSpinBox,
        QFormLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPlainTextEdit,
        QPushButton,
        QScrollArea,
        QSpinBox,
        QSplitter,
        QStatusBar,
        QTabWidget,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )

    QT_API = "PySide6"
except ImportError:
    from PyQt6.QtCore import Qt, QThread, pyqtSignal as Signal, pyqtSlot as Slot, QRectF
    from PyQt6.QtGui import QColor, QFont, QPainter, QPen
    from PyQt6.QtWidgets import (
        QApplication,
        QComboBox,
        QDoubleSpinBox,
        QFormLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPlainTextEdit,
        QPushButton,
        QScrollArea,
        QSpinBox,
        QSplitter,
        QStatusBar,
        QTabWidget,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )

    QT_API = "PyQt6"

import pyqtgraph as pg

try:
    import matplotlib.cm as mpl_cm
except ImportError:
    mpl_cm = None

from iq_client import (
    CHIRP_MODES,
    DEFAULT_HOST,
    DEFAULT_POLL_INTERVAL_S,
    DEFAULT_PORT,
    DEFAULT_TIMEOUT_S,
    RANGE_BIN_SIZE_M,
    VA_COMBINE_AVERAGE,
    VA_COMBINE_BEAMFORM,
    VA_COMBINE_MODES,
    VA_COMBINE_SINGLE,
    VELOCITY_BIN_SIZE_MPS,
    IqResponse,
    IqSession,
    RdmResponse,
    RdmSpec,
    Target,
    build_range_profile_targets,
    parse_targets_text,
    va_label,
)

DEFAULT_TARGET_TEXT = "\n".join(
    [
        "# azimuth_rad, elevation_rad, distance_m [, chirp_mode]",
        "# 0=PerChirp 1=Average 2=MaxAbs 3=FirstChirp",
        "0,0,1.000,1",
        "0,0,1.075,1",
        "0,0,1.150,1",
    ]
)

PROFILE_PAD_BINS = 2
PROFILE_Y_DEFAULT = (0.0, 7000.0)
COLORMAPS = ["viridis", "inferno", "plasma", "magma", "cividis", "turbo"]
PHASE_COLORMAPS = ["coolwarm", "twilight", "hsv", "PiYG", "RdBu", "seismic", "turbo"]
# Peak |IQ| below this value is normalized against the floor (noise stays dark).
RDM_MAGNITUDE_FLOOR = 2000.0

RDM_VIEW_POWER = "power"
RDM_VIEW_PHASE = "phase"
RDM_VIEW_BLEND = "blend"

RDM_VIEW_OPTIONS: list[tuple[str, str]] = [
    ("Power |IQ|", RDM_VIEW_POWER),
    ("Phase", RDM_VIEW_PHASE),
    ("Phase × |IQ|", RDM_VIEW_BLEND),
]

RDM_DEFAULT_CMAPS: dict[str, str] = {
    RDM_VIEW_POWER: "viridis",
    RDM_VIEW_PHASE: "coolwarm",
    RDM_VIEW_BLEND: "coolwarm",
}


class YZoomPlotWidget(pg.PlotWidget):
    """PlotWidget with fixed X range and Y-only mouse wheel zoom."""

    def __init__(self, y_bounds: list[float], **kwargs) -> None:
        super().__init__(**kwargs)
        self._y_bounds = y_bounds
        self.setLabel("bottom", "Distance", units="m")
        self.setLabel("left", "|IQ|")
        self.showGrid(x=True, y=True, alpha=0.3)
        vb = self.getViewBox()
        vb.setMouseEnabled(x=False, y=True)
        vb.setLimits(minYRange=100.0)

    def set_y_bounds(self, y0: float, y1: float) -> None:
        self._y_bounds = [y0, y1]
        self.setYRange(y0, y1, padding=0)

    def wheelEvent(self, event) -> None:  # noqa: N802
        delta = event.angleDelta().y()
        if delta == 0:
            super().wheelEvent(event)
            return
        factor = 0.9 if delta > 0 else 1.1
        y0, y1 = self._y_bounds
        center = (y0 + y1) / 2.0
        half = max((y1 - y0) / 2.0 * factor, 50.0)
        self._y_bounds = [max(0.0, center - half), center + half]
        self.setYRange(self._y_bounds[0], self._y_bounds[1], padding=0)
        event.accept()


class ViewMode(Enum):
    TARGET_LIST = auto()
    DISTANCE_PROFILE = auto()
    RDM = auto()


@dataclass
class ProfileState:
    targets: list[Target]
    plot_x_min: float
    plot_x_max: float
    va_combine_mode: int = VA_COMBINE_SINGLE
    tile: int = 0
    sub_ant: int = 0


@dataclass
class PollPayload:
    view_mode: ViewMode
    targets: list[Target] | None = None
    rdm_spec: RdmSpec | None = None
    profile: ProfileState | None = None
    va_combine_mode: int = VA_COMBINE_BEAMFORM
    tile: int = 0
    sub_ant: int = 0
    rdm_pick_range_m: float | None = None


class PhaseDial(QWidget):
    """Circular phase indicator (hand angle only, not amplitude)."""

    def __init__(self, diameter: int = 40, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._phase_rad = 0.0
        self._distance_m = 0.0
        self.setFixedSize(diameter + 4, diameter + 18)

    def set_sample(self, distance_m: float, phase_rad: float) -> None:
        self._distance_m = distance_m
        self._phase_rad = phase_rad
        self.update()

    def paintEvent(self, _event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w = self.width()
        dial = min(w - 4, self.height() - 16)
        cx = w / 2.0
        cy = dial / 2.0 + 2.0
        radius = dial / 2.0 - 2.0

        painter.setPen(QPen(QColor("#666666"), 1.5))
        painter.setBrush(QColor("#2a2a2a"))
        painter.drawEllipse(int(cx - radius), int(cy - radius), int(2 * radius), int(2 * radius))

        # Hand: phase 0 → +x (3 o'clock), CCW positive (atan2)
        hx = cx + radius * 0.75 * math.cos(self._phase_rad)
        hy = cy - radius * 0.75 * math.sin(self._phase_rad)
        painter.setPen(QPen(QColor("#ffeb3b"), 2.0))
        painter.drawLine(int(cx), int(cy), int(hx), int(hy))

        painter.setPen(QColor("#aaaaaa"))
        painter.drawText(QRectF(0, dial + 2, w, 14), Qt.AlignmentFlag.AlignHCenter, f"{self._distance_m:.2f}m")


def make_colormap_lut(name: str) -> np.ndarray:
    if mpl_cm is not None:
        cmap = mpl_cm.get_cmap(name)
        rgba = cmap(np.linspace(0.0, 1.0, 256))
        return (rgba[:, :3] * 255).astype(np.uint8)

    # Fallback blue-yellow if matplotlib missing
    t = np.linspace(0, 1, 256)
    return np.stack([t * 180, t * 120 + 50, (1 - t) * 200 + 55], axis=1).astype(np.uint8)


def scale_magnitude_grid(
    mag: np.ndarray,
    percentile: float = 99.0,
    floor: float = RDM_MAGNITUDE_FLOOR,
) -> tuple[np.ndarray, float]:
    if mag.size == 0:
        return mag, 1.0
    vmax = float(np.percentile(mag, percentile))
    if vmax <= 0:
        vmax = float(mag.max()) if mag.max() > 0 else 1.0
    scale = max(vmax, floor)
    return np.clip(mag / scale, 0.0, 1.0), vmax


def get_pg_colormap(name: str) -> pg.ColorMap:
    try:
        return pg.colormap.get(name, source="matplotlib")
    except Exception:
        lut = make_colormap_lut(name)
        return pg.ColorMap(pos=np.linspace(0, 1, len(lut)), color=lut)


def arrange_rdm_grid(grid: np.ndarray) -> np.ndarray:
    """[range][doppler] → display image (velocity↑ rows, range→ cols)."""
    return np.flipud(grid.T)


def phase_to_unit(phase: np.ndarray) -> np.ndarray:
    return np.clip((phase + np.pi) / (2.0 * np.pi), 0.0, 1.0)


def build_phase_rgba(phase: np.ndarray, opacity: np.ndarray, cmap_name: str) -> np.ndarray:
    phase_u = phase_to_unit(phase)
    lut = get_pg_colormap(cmap_name).getLookupTable(nPts=256)
    idx = np.clip((phase_u * 255.0).astype(np.int32), 0, 255)
    rgb = lut[idx]
    if rgb.shape[-1] == 4:
        rgb = rgb[..., :3]
    alpha = np.clip(opacity * 255.0, 0, 255).astype(np.uint8)
    return np.ascontiguousarray(np.dstack([rgb.astype(np.uint8), alpha]))


class PollWorker(QThread):
    frame_ready = Signal(int, float, object, float)  # frame, elapsed, packet, fps
    poll_failed = Signal(str)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._running = False
        self._host = DEFAULT_HOST
        self._port = DEFAULT_PORT
        self._timeout = DEFAULT_TIMEOUT_S
        self._interval_ms = int(DEFAULT_POLL_INTERVAL_S * 1000)
        self._payload: PollPayload | None = None
        self._frame = 0
        self._t0 = 0.0
        self._session = IqSession()
        self._last_poll = 0.0
        self._fps_ema = 0.0

    def configure(
        self,
        host: str,
        port: int,
        timeout: float,
        interval_ms: int,
        payload: PollPayload,
    ) -> None:
        self._host = host
        self._port = port
        self._timeout = timeout
        self._interval_ms = max(1, interval_ms)
        self._payload = payload

    def stop(self) -> None:
        self._running = False

    def close_session(self) -> None:
        self._session.close()

    def run(self) -> None:
        self._running = True
        self._frame = 0
        self._t0 = time.monotonic()
        self._last_poll = time.perf_counter()
        self._fps_ema = 0.0
        try:
            self._session.open(self._host, self._port, self._timeout)
        except OSError as exc:
            self.poll_failed.emit(str(exc))
            return

        while self._running:
            payload = self._payload
            if payload is None:
                self.msleep(self._interval_ms)
                continue

            self._frame += 1
            elapsed = time.monotonic() - self._t0
            t0 = time.perf_counter()
            try:
                if payload.view_mode == ViewMode.RDM and payload.rdm_spec is not None:
                    response = self._session.request_rdm(payload.rdm_spec)
                    pick_iq: IqResponse | None = None
                    if payload.rdm_pick_range_m is not None:
                        spec = payload.rdm_spec
                        pick_target = Target(
                            spec.azimuth_rad,
                            spec.elevation_rad,
                            payload.rdm_pick_range_m,
                            chirp_mode=1,
                        )
                        pick_iq = self._session.request_iq(
                            [pick_target],
                            use_v2=True,
                            va_combine_mode=spec.va_combine_mode,
                            tile=spec.tile,
                            sub_ant=spec.sub_ant,
                        )
                    packet = ("rdm", response, pick_iq)
                elif payload.targets:
                    response = self._session.request_iq(
                        payload.targets,
                        use_v2=True,
                        va_combine_mode=payload.va_combine_mode,
                        tile=payload.tile,
                        sub_ant=payload.sub_ant,
                    )
                    packet = ("iq", payload.targets, response, payload.profile)
                else:
                    self.msleep(self._interval_ms)
                    continue
            except OSError as exc:
                self.poll_failed.emit(str(exc))
                self.msleep(self._interval_ms)
                continue

            dt = time.perf_counter() - t0
            if dt > 0:
                inst_fps = 1.0 / dt
                self._fps_ema = inst_fps if self._fps_ema <= 0 else (0.85 * self._fps_ema + 0.15 * inst_fps)

            self.frame_ready.emit(self._frame, elapsed, packet, self._fps_ema)

            poll_dt = time.perf_counter() - t0
            sleep_ms = max(0, int(self._interval_ms - poll_dt * 1000))
            if sleep_ms > 0:
                self.msleep(sleep_ms)

        self._session.close()


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("test-iq viewer")
        self.resize(1200, 820)

        self._connected = False
        self._profile_plot_bounds = (0.0, 2.0)
        self._profile_y_bounds = list(PROFILE_Y_DEFAULT)
        self._phase_dials: list[PhaseDial] = []
        self._rdm_vmax = 1.0
        self._rdm_rect: tuple[float, float, float, float] | None = None
        self._last_rdm_response: RdmResponse | None = None
        self._rdm_pick_range_m: float | None = None

        self._worker = PollWorker(self)
        self._worker.frame_ready.connect(self._on_frame)
        self._worker.poll_failed.connect(self._on_poll_error)

        self._build_ui()
        self._wire_signals()
        self._refresh_target_parse()
        self._refresh_profile_targets()
        self._on_rdm_params_changed()

    def _build_ui(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        layout.addWidget(self._build_connection_bar())

        self.tabs = QTabWidget()
        self.tabs.addTab(self._build_target_list_tab(), "Target List")
        self.tabs.addTab(self._build_distance_profile_tab(), "Distance Profile")
        self.tabs.addTab(self._build_rdm_tab(), "Range-Doppler Map")
        layout.addWidget(self.tabs, stretch=1)

        self.setStatusBar(QStatusBar())
        self._status_label = QLabel("Disconnected")
        self.statusBar().addPermanentWidget(self._status_label)

    def _build_connection_bar(self) -> QGroupBox:
        box = QGroupBox("Connection")
        row = QHBoxLayout(box)

        row.addWidget(QLabel("Host"))
        self.host_edit = QLineEdit(DEFAULT_HOST)
        self.host_edit.setMinimumWidth(140)
        row.addWidget(self.host_edit)

        row.addWidget(QLabel("Port"))
        self.port_spin = QSpinBox()
        self.port_spin.setRange(1, 65535)
        self.port_spin.setValue(DEFAULT_PORT)
        row.addWidget(self.port_spin)

        row.addWidget(QLabel("Timeout (s)"))
        self.timeout_spin = QDoubleSpinBox()
        self.timeout_spin.setRange(0.5, 120.0)
        self.timeout_spin.setDecimals(1)
        self.timeout_spin.setValue(DEFAULT_TIMEOUT_S)
        row.addWidget(self.timeout_spin)

        row.addWidget(QLabel("Poll (ms)"))
        self.interval_spin = QSpinBox()
        self.interval_spin.setRange(10, 5000)
        self.interval_spin.setValue(int(DEFAULT_POLL_INTERVAL_S * 1000))
        row.addWidget(self.interval_spin)

        row.addStretch(1)

        self.fps_label = QLabel("FPS: —")
        self.fps_label.setMinimumWidth(88)
        row.addWidget(self.fps_label)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setMinimumWidth(100)
        row.addWidget(self.connect_btn)
        return box

    def _build_target_list_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        hint = QLabel(
            "One target per line: azimuth_rad, elevation_rad, distance_m [, chirp_mode]. "
            "Parsed live while typing."
        )
        hint.setWordWrap(True)
        layout.addWidget(hint)

        splitter = QSplitter(Qt.Orientation.Vertical)
        self.target_editor = QPlainTextEdit()
        self.target_editor.setPlainText(DEFAULT_TARGET_TEXT)
        font = QFont("monospace")
        font.setStyleHint(QFont.StyleHint.Monospace)
        self.target_editor.setFont(font)
        splitter.addWidget(self.target_editor)

        self.target_table = QTableWidget(0, 8)
        self.target_table.setHorizontalHeaderLabels(
            ["#", "az (rad)", "el (rad)", "dist (m)", "mode", "|IQ|", "phase (deg)", "I / Q"]
        )
        self.target_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        splitter.addWidget(self.target_table)
        splitter.setStretchFactor(0, 2)
        splitter.setStretchFactor(1, 3)
        layout.addWidget(splitter, stretch=1)

        self.target_parse_label = QLabel()
        self.target_parse_label.setWordWrap(True)
        layout.addWidget(self.target_parse_label)
        return page

    def _build_distance_profile_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(4)

        controls = QHBoxLayout()
        controls.setSpacing(8)

        beam_box = QGroupBox("Beamforming")
        beam_form = QFormLayout(beam_box)
        beam_form.setContentsMargins(6, 4, 6, 4)
        self.profile_az = QDoubleSpinBox()
        self.profile_az.setRange(-90.0, 90.0)
        self.profile_az.setDecimals(2)
        self.profile_az.setSuffix(" deg")
        beam_form.addRow("Azimuth", self.profile_az)
        self.profile_el = QDoubleSpinBox()
        self.profile_el.setRange(-90.0, 90.0)
        self.profile_el.setDecimals(2)
        self.profile_el.setSuffix(" deg")
        beam_form.addRow("Elevation", self.profile_el)
        controls.addWidget(beam_box)

        range_box = QGroupBox("Range")
        range_form = QFormLayout(range_box)
        range_form.setContentsMargins(6, 4, 6, 4)
        self.profile_dist_min = QDoubleSpinBox()
        self.profile_dist_min.setRange(0.0, 30.0)
        self.profile_dist_min.setDecimals(3)
        self.profile_dist_min.setSuffix(" m")
        self.profile_dist_min.setValue(0.5)
        range_form.addRow("Min", self.profile_dist_min)
        self.profile_dist_max = QDoubleSpinBox()
        self.profile_dist_max.setRange(0.0, 30.0)
        self.profile_dist_max.setDecimals(3)
        self.profile_dist_max.setSuffix(" m")
        self.profile_dist_max.setValue(2.0)
        range_form.addRow("Max", self.profile_dist_max)
        self.profile_step = QDoubleSpinBox()
        self.profile_step.setRange(0.001, 1.0)
        self.profile_step.setDecimals(4)
        self.profile_step.setSuffix(" m")
        self.profile_step.setValue(RANGE_BIN_SIZE_M)
        range_form.addRow("Step", self.profile_step)
        controls.addWidget(range_box)

        proc_box = QGroupBox("Processing")
        proc_form = QFormLayout(proc_box)
        proc_form.setContentsMargins(6, 4, 6, 4)
        self.profile_mode = QComboBox()
        for mode_id, (label, _) in CHIRP_MODES.items():
            self.profile_mode.addItem(f"{label} ({mode_id})", mode_id)
        self.profile_mode.setCurrentIndex(1)
        proc_form.addRow("Chirp mode", self.profile_mode)
        controls.addWidget(proc_box)

        ant_box = QGroupBox("Antenna")
        ant_form = QFormLayout(ant_box)
        ant_form.setContentsMargins(6, 4, 6, 4)
        self.profile_va_mode = QComboBox()
        for mode_id, label in VA_COMBINE_MODES.items():
            self.profile_va_mode.addItem(label, mode_id)
        self.profile_va_mode.setCurrentIndex(VA_COMBINE_SINGLE)
        ant_form.addRow("Combine", self.profile_va_mode)
        self.profile_tile = QSpinBox()
        self.profile_tile.setRange(0, 11)
        ant_form.addRow("TX tile", self.profile_tile)
        self.profile_sub_ant = QSpinBox()
        self.profile_sub_ant.setRange(0, 15)
        ant_form.addRow("RX sub", self.profile_sub_ant)
        self.profile_va_label = QLabel()
        ant_form.addRow("VA", self.profile_va_label)
        controls.addWidget(ant_box)

        controls.addStretch(1)
        layout.addLayout(controls)

        self.profile_parse_label = QLabel()
        self.profile_parse_label.setMaximumHeight(20)
        layout.addWidget(self.profile_parse_label)

        pg.setConfigOptions(antialias=True)
        self.profile_plot = YZoomPlotWidget(
            y_bounds=self._profile_y_bounds,
            title="Range profile (|IQ| vs distance)",
        )
        self.profile_curve = self.profile_plot.plot(pen=pg.mkPen("#4fc3f7", width=2), symbol="o", symbolSize=5)
        self.profile_peak_line = pg.InfiniteLine(
            angle=90,
            movable=False,
            pen=pg.mkPen("#ffb74d", width=1, style=Qt.PenStyle.DashLine),
        )
        self.profile_plot.addItem(self.profile_peak_line)
        self._apply_profile_x_axis()
        layout.addWidget(self.profile_plot, stretch=1)

        self.phase_dial_host = QWidget()
        self.phase_dial_row = QHBoxLayout(self.phase_dial_host)
        self.phase_dial_row.setContentsMargins(4, 0, 4, 0)
        self.phase_dial_row.setSpacing(6)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.phase_dial_host)
        scroll.setFixedHeight(64)
        layout.addWidget(scroll)
        self._sync_profile_antenna_controls()
        return page

    def _build_rdm_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(4)

        controls = QHBoxLayout()
        controls.setSpacing(8)

        beam_box = QGroupBox("Beamforming")
        beam_form = QFormLayout(beam_box)
        beam_form.setContentsMargins(6, 4, 6, 4)
        self.rdm_az = QDoubleSpinBox()
        self.rdm_az.setRange(-90.0, 90.0)
        self.rdm_az.setDecimals(2)
        self.rdm_az.setSuffix(" deg")
        beam_form.addRow("Azimuth", self.rdm_az)
        self.rdm_el = QDoubleSpinBox()
        self.rdm_el.setRange(-90.0, 90.0)
        self.rdm_el.setDecimals(2)
        self.rdm_el.setSuffix(" deg")
        beam_form.addRow("Elevation", self.rdm_el)
        controls.addWidget(beam_box)

        range_box = QGroupBox("Range")
        range_form = QFormLayout(range_box)
        range_form.setContentsMargins(6, 4, 6, 4)
        self.rdm_dist_min = QDoubleSpinBox()
        self.rdm_dist_min.setRange(0.0, 30.0)
        self.rdm_dist_min.setDecimals(3)
        self.rdm_dist_min.setSuffix(" m")
        self.rdm_dist_min.setValue(0.3)
        range_form.addRow("Min", self.rdm_dist_min)
        self.rdm_dist_max = QDoubleSpinBox()
        self.rdm_dist_max.setRange(0.0, 30.0)
        self.rdm_dist_max.setDecimals(3)
        self.rdm_dist_max.setSuffix(" m")
        self.rdm_dist_max.setValue(2.5)
        range_form.addRow("Max", self.rdm_dist_max)
        controls.addWidget(range_box)

        dop_box = QGroupBox("Doppler")
        dop_form = QFormLayout(dop_box)
        dop_form.setContentsMargins(6, 4, 6, 4)
        self.rdm_vel_min = QDoubleSpinBox()
        self.rdm_vel_min.setRange(-20.0, 20.0)
        self.rdm_vel_min.setDecimals(3)
        self.rdm_vel_min.setSuffix(" m/s")
        self.rdm_vel_min.setValue(-2.0)
        dop_form.addRow("Min", self.rdm_vel_min)
        self.rdm_vel_max = QDoubleSpinBox()
        self.rdm_vel_max.setRange(-20.0, 20.0)
        self.rdm_vel_max.setDecimals(3)
        self.rdm_vel_max.setSuffix(" m/s")
        self.rdm_vel_max.setValue(2.0)
        dop_form.addRow("Max", self.rdm_vel_max)
        self.rdm_mode = QComboBox()
        for mode_id, (label, _) in CHIRP_MODES.items():
            self.rdm_mode.addItem(f"{label} ({mode_id})", mode_id)
        self.rdm_mode.setCurrentIndex(0)
        dop_form.addRow("Mode", self.rdm_mode)
        controls.addWidget(dop_box)

        ant_box = QGroupBox("Antenna")
        ant_form = QFormLayout(ant_box)
        ant_form.setContentsMargins(6, 4, 6, 4)
        self.rdm_va_mode = QComboBox()
        for mode_id, label in VA_COMBINE_MODES.items():
            self.rdm_va_mode.addItem(label, mode_id)
        ant_form.addRow("Combine", self.rdm_va_mode)
        self.rdm_tile = QSpinBox()
        self.rdm_tile.setRange(0, 11)
        ant_form.addRow("TX tile", self.rdm_tile)
        self.rdm_sub_ant = QSpinBox()
        self.rdm_sub_ant.setRange(0, 15)
        ant_form.addRow("RX sub", self.rdm_sub_ant)
        self.rdm_va_label = QLabel()
        ant_form.addRow("VA", self.rdm_va_label)
        controls.addWidget(ant_box)

        disp_box = QGroupBox("Display")
        disp_form = QFormLayout(disp_box)
        disp_form.setContentsMargins(6, 4, 6, 4)
        self.rdm_view_mode = QComboBox()
        for label, mode_id in RDM_VIEW_OPTIONS:
            self.rdm_view_mode.addItem(label, mode_id)
        disp_form.addRow("View", self.rdm_view_mode)
        self.rdm_mag_floor = QSpinBox()
        self.rdm_mag_floor.setRange(1, 100_000)
        self.rdm_mag_floor.setSingleStep(100)
        self.rdm_mag_floor.setValue(int(RDM_MAGNITUDE_FLOOR))
        self.rdm_mag_floor.setToolTip("Peak |IQ| below this uses the floor for normalization / opacity.")
        disp_form.addRow("Min scale", self.rdm_mag_floor)
        self.rdm_cmap = QComboBox()
        disp_form.addRow("Colormap", self.rdm_cmap)
        controls.addWidget(disp_box)

        pick_box = QGroupBox("Pick")
        pick_layout = QVBoxLayout(pick_box)
        pick_layout.setContentsMargins(6, 4, 6, 4)
        pick_layout.setSpacing(2)
        self.rdm_pick_dial = PhaseDial(diameter=52)
        pick_layout.addWidget(self.rdm_pick_dial, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.rdm_pick_info = QLabel("L-click: range line\nR-click: clear")
        self.rdm_pick_info.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        self.rdm_pick_info.setWordWrap(True)
        pick_layout.addWidget(self.rdm_pick_info)
        controls.addWidget(pick_box)

        controls.addStretch(1)
        layout.addLayout(controls)

        self._populate_rdm_colormap_combo(RDM_VIEW_POWER)

        self.rdm_parse_label = QLabel()
        self.rdm_parse_label.setMaximumHeight(20)
        layout.addWidget(self.rdm_parse_label)

        self.rdm_plot = pg.PlotWidget(title="Range-Doppler Map")
        self.rdm_plot.setLabel("bottom", "Distance", units="m")
        self.rdm_plot.setLabel("left", "Velocity", units="m/s")
        self.rdm_image = pg.ImageItem(axisOrder="row-major")
        self.rdm_plot.addItem(self.rdm_image)

        self.rdm_colorbar = pg.ColorBarItem(
            values=(0, 1),
            colorMap=pg.colormap.get("viridis", source="matplotlib"),
            label="|IQ| (norm.)",
        )
        self.rdm_colorbar.setImageItem(self.rdm_image, insert_in=self.rdm_plot.getPlotItem())
        self.rdm_pick_line = pg.InfiniteLine(
            angle=90,
            movable=False,
            pen=pg.mkPen("#ffeb3b", width=2, style=Qt.PenStyle.SolidLine),
        )
        self.rdm_pick_line.setVisible(False)
        self.rdm_plot.addItem(self.rdm_pick_line)
        layout.addWidget(self.rdm_plot, stretch=1)

        self._apply_rdm_colormap()
        self._sync_rdm_antenna_controls()
        self.rdm_mag_floor.setEnabled(True)
        return page

    def _wire_signals(self) -> None:
        self.connect_btn.clicked.connect(self._toggle_connection)
        self.target_editor.textChanged.connect(self._refresh_target_parse)
        self.tabs.currentChanged.connect(self._on_tab_changed)

        for widget in (
            self.profile_az,
            self.profile_el,
            self.profile_dist_min,
            self.profile_dist_max,
            self.profile_step,
        ):
            widget.valueChanged.connect(self._on_profile_params_changed)
        self.profile_mode.currentIndexChanged.connect(self._on_profile_params_changed)
        self.profile_va_mode.currentIndexChanged.connect(self._on_profile_va_mode_changed)
        self.profile_tile.valueChanged.connect(self._on_profile_va_index_changed)
        self.profile_sub_ant.valueChanged.connect(self._on_profile_va_index_changed)

        for widget in (
            self.rdm_az,
            self.rdm_el,
            self.rdm_dist_min,
            self.rdm_dist_max,
            self.rdm_vel_min,
            self.rdm_vel_max,
        ):
            widget.valueChanged.connect(self._on_rdm_params_changed)
        self.rdm_mode.currentIndexChanged.connect(self._on_rdm_params_changed)
        self.rdm_va_mode.currentIndexChanged.connect(self._on_rdm_va_mode_changed)
        self.rdm_tile.valueChanged.connect(self._on_rdm_va_index_changed)
        self.rdm_sub_ant.valueChanged.connect(self._on_rdm_va_index_changed)
        self.rdm_view_mode.currentIndexChanged.connect(self._on_rdm_view_mode_changed)
        self.rdm_mag_floor.valueChanged.connect(self._on_rdm_display_changed)
        self.rdm_cmap.currentIndexChanged.connect(self._on_rdm_display_changed)
        self.rdm_plot.scene().sigMouseClicked.connect(self._on_rdm_plot_clicked)

    def _active_view_mode(self) -> ViewMode:
        idx = self.tabs.currentIndex()
        if idx == 1:
            return ViewMode.DISTANCE_PROFILE
        if idx == 2:
            return ViewMode.RDM
        return ViewMode.TARGET_LIST

    def _apply_profile_x_axis(self) -> None:
        x0, x1 = self._profile_plot_bounds
        self.profile_plot.setXRange(x0, x1, padding=0)
        vb = self.profile_plot.getViewBox()
        span = max(x1 - x0, 1e-3)
        vb.setLimits(xMin=x0, xMax=x1, minXRange=span, maxXRange=span)

    def _on_profile_params_changed(self) -> None:
        self._refresh_profile_targets()
        self._apply_profile_x_axis()

    def _profile_targets(self) -> tuple[ProfileState | None, list[str]]:
        try:
            targets, plot_min, plot_max = build_range_profile_targets(
                self.profile_az.value(),
                self.profile_el.value(),
                self.profile_dist_min.value(),
                self.profile_dist_max.value(),
                self.profile_step.value(),
                self.profile_mode.currentData(),
                pad_bins=PROFILE_PAD_BINS,
            )
            self._profile_plot_bounds = (plot_min, plot_max)
            user_min = self.profile_dist_min.value()
            user_max = self.profile_dist_max.value()
            msg = f"{len(targets)} bins (plot {plot_min:.3f}–{plot_max:.3f} m, user {user_min:.3f}–{user_max:.3f} m)"
            if len(targets) > 64:
                msg += " — large request"
            return ProfileState(
                targets,
                plot_min,
                plot_max,
                va_combine_mode=self.profile_va_mode.currentData(),
                tile=self.profile_tile.value(),
                sub_ant=self.profile_sub_ant.value(),
            ), [] if targets else ["no range bins"]
        except ValueError as exc:
            return None, [str(exc)]

    def _sync_profile_antenna_controls(self) -> None:
        mode = self.profile_va_mode.currentData()
        single = mode == VA_COMBINE_SINGLE
        beamform = mode == VA_COMBINE_BEAMFORM
        self.profile_tile.setEnabled(single)
        self.profile_sub_ant.setEnabled(single)
        self.profile_va_label.setEnabled(single)
        self.profile_az.setEnabled(beamform)
        self.profile_el.setEnabled(beamform)
        if single:
            self.profile_va_label.setText(va_label(self.profile_tile.value(), self.profile_sub_ant.value()))
        else:
            self.profile_va_label.setText("—")

    def _on_profile_va_mode_changed(self) -> None:
        self._sync_profile_antenna_controls()
        self._on_profile_params_changed()

    def _on_profile_va_index_changed(self) -> None:
        if self.profile_va_mode.currentData() == VA_COMBINE_SINGLE:
            self.profile_va_label.setText(va_label(self.profile_tile.value(), self.profile_sub_ant.value()))
        self._on_profile_params_changed()

    def _rdm_spec(self) -> tuple[RdmSpec | None, list[str]]:
        if self.rdm_dist_max.value() < self.rdm_dist_min.value():
            return None, ["distance_max < distance_min"]
        if self.rdm_vel_max.value() < self.rdm_vel_min.value():
            return None, ["velocity_max < velocity_min"]
        spec = RdmSpec(
            azimuth_rad=math.radians(self.rdm_az.value()),
            elevation_rad=math.radians(self.rdm_el.value()),
            distance_min_m=self.rdm_dist_min.value(),
            distance_max_m=self.rdm_dist_max.value(),
            velocity_min_mps=self.rdm_vel_min.value(),
            velocity_max_mps=self.rdm_vel_max.value(),
            chirp_mode=self.rdm_mode.currentData(),
            va_combine_mode=self.rdm_va_mode.currentData(),
            tile=self.rdm_tile.value(),
            sub_ant=self.rdm_sub_ant.value(),
        )
        return spec, []

    def _sync_rdm_antenna_controls(self) -> None:
        mode = self.rdm_va_mode.currentData()
        single = mode == VA_COMBINE_SINGLE
        beamform = mode == VA_COMBINE_BEAMFORM
        self.rdm_tile.setEnabled(single)
        self.rdm_sub_ant.setEnabled(single)
        self.rdm_va_label.setEnabled(single)
        self.rdm_az.setEnabled(beamform)
        self.rdm_el.setEnabled(beamform)
        if single:
            self.rdm_va_label.setText(va_label(self.rdm_tile.value(), self.rdm_sub_ant.value()))
        else:
            self.rdm_va_label.setText("—")

    def _on_rdm_va_mode_changed(self) -> None:
        self._sync_rdm_antenna_controls()
        self._on_rdm_params_changed()

    def _on_rdm_va_index_changed(self) -> None:
        if self.rdm_va_mode.currentData() == VA_COMBINE_SINGLE:
            self.rdm_va_label.setText(va_label(self.rdm_tile.value(), self.rdm_sub_ant.value()))
        self._on_rdm_params_changed()

    def _current_payload(self) -> tuple[PollPayload | None, list[str]]:
        mode = self._active_view_mode()
        if mode == ViewMode.TARGET_LIST:
            targets, errors = parse_targets_text(self.target_editor.toPlainText())
            if errors or not targets:
                return None, errors or ["no targets"]
            return PollPayload(mode, targets=targets), []
        if mode == ViewMode.DISTANCE_PROFILE:
            profile, errors = self._profile_targets()
            if errors or profile is None:
                return None, errors or ["no profile"]
            return PollPayload(
                mode,
                targets=profile.targets,
                profile=profile,
                va_combine_mode=profile.va_combine_mode,
                tile=profile.tile,
                sub_ant=profile.sub_ant,
            ), []
        spec, errors = self._rdm_spec()
        if errors or spec is None:
            return None, errors or ["invalid RDM spec"]
        return PollPayload(mode, rdm_spec=spec, rdm_pick_range_m=self._rdm_pick_range_m), []

    def _refresh_target_parse(self) -> None:
        targets, errors = parse_targets_text(self.target_editor.toPlainText())
        self._set_parse_label(self.target_parse_label, len(targets), errors)
        self.target_table.setRowCount(len(targets))
        for row, target in enumerate(targets):
            self._set_table_row(self.target_table, row, target, None)
        if self._connected and self._active_view_mode() == ViewMode.TARGET_LIST:
            self._push_worker_payload()

    def _refresh_profile_targets(self) -> None:
        profile, errors = self._profile_targets()
        if profile:
            self._set_parse_label(self.profile_parse_label, len(profile.targets), errors, profile)
            self._ensure_phase_dials(len(profile.targets))
        else:
            self._set_parse_label(self.profile_parse_label, 0, errors)
        if self._connected and self._active_view_mode() == ViewMode.DISTANCE_PROFILE:
            self._push_worker_payload()

    def _on_rdm_params_changed(self) -> None:
        spec, errors = self._rdm_spec()
        if spec:
            va_name = VA_COMBINE_MODES.get(spec.va_combine_mode, "?")
            if spec.va_combine_mode == VA_COMBINE_SINGLE:
                va_name = va_label(spec.tile, spec.sub_ant)
            self.rdm_parse_label.setStyleSheet("color: #2e7d32;")
            self.rdm_parse_label.setText(
                f"OK — R={spec.distance_min_m:.2f}–{spec.distance_max_m:.2f} m, "
                f"v={spec.velocity_min_mps:.2f}–{spec.velocity_max_mps:.2f} m/s, "
                f"mode={CHIRP_MODES[spec.chirp_mode][0]}, antenna={va_name}"
            )
        else:
            self.rdm_parse_label.setStyleSheet("color: #c62828;")
            self.rdm_parse_label.setText(" | ".join(errors))
        if self._connected and self._active_view_mode() == ViewMode.RDM:
            self._push_worker_payload()

    def _set_parse_label(
        self,
        label: QLabel,
        count: int,
        errors: list[str],
        profile: ProfileState | None = None,
    ) -> None:
        if errors:
            label.setStyleSheet("color: #c62828;")
            label.setText(f"Parse errors ({len(errors)}): " + " | ".join(errors))
        elif count == 0:
            label.setStyleSheet("color: #e65100;")
            label.setText("No valid targets.")
        else:
            label.setStyleSheet("color: #2e7d32;")
            if profile:
                va_name = VA_COMBINE_MODES.get(profile.va_combine_mode, "?")
                if profile.va_combine_mode == VA_COMBINE_SINGLE:
                    va_name = va_label(profile.tile, profile.sub_ant)
                label.setText(
                    f"OK — {count} bins, plot {profile.plot_x_min:.3f}–{profile.plot_x_max:.3f} m, "
                    f"antenna={va_name}"
                )
            else:
                label.setText(f"OK — {count} target(s)")

    def _ensure_phase_dials(self, count: int) -> None:
        while len(self._phase_dials) < count:
            dial = PhaseDial()
            self._phase_dials.append(dial)
            self.phase_dial_row.addWidget(dial)
        while len(self._phase_dials) > count:
            dial = self._phase_dials.pop()
            self.phase_dial_row.removeWidget(dial)
            dial.deleteLater()

    def _set_table_row(
        self,
        table: QTableWidget,
        row: int,
        target: Target,
        sample: tuple[float, float, float, float] | None,
    ) -> None:
        values: list[str] = [
            str(row),
            f"{target.azimuth_rad:.4f}",
            f"{target.elevation_rad:.4f}",
            f"{target.distance_m:.3f}",
            target.mode_name,
        ]
        if sample is None:
            values.extend(["—", "—", "—"])
        else:
            i_val, q_val, mag, phase = sample
            values.extend([f"{mag:.1f}", f"{phase:.1f}", f"{i_val:.1f}, {q_val:.1f}"])
        for col, text in enumerate(values):
            item = QTableWidgetItem(text)
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
            table.setItem(row, col, item)

    @Slot()
    def _on_tab_changed(self) -> None:
        if self._connected:
            self._push_worker_payload()
        if self.tabs.currentIndex() == 2:
            self._on_rdm_params_changed()

    def _push_worker_payload(self) -> None:
        payload, errors = self._current_payload()
        if payload is None:
            return
        self._worker.configure(
            self.host_edit.text().strip() or DEFAULT_HOST,
            self.port_spin.value(),
            self.timeout_spin.value(),
            self.interval_spin.value(),
            payload,
        )

    @Slot()
    def _toggle_connection(self) -> None:
        if self._connected:
            self._disconnect()
            return

        payload, errors = self._current_payload()
        if errors:
            QMessageBox.warning(self, "Parse error", "\n".join(errors))
            return
        if payload is None:
            QMessageBox.warning(self, "No request", "Configure a valid request first.")
            return

        host = self.host_edit.text().strip() or DEFAULT_HOST
        port = self.port_spin.value()
        timeout = self.timeout_spin.value()
        probe = IqSession()
        try:
            probe.open(host, port, timeout)
            if payload.view_mode == ViewMode.RDM and payload.rdm_spec:
                probe.request_rdm(payload.rdm_spec)
            elif payload.targets:
                probe.request_iq(payload.targets[:1], use_v2=True)
        except OSError as exc:
            QMessageBox.critical(self, "Connection failed", str(exc))
            return
        finally:
            probe.close()

        self._connected = True
        self.fps_label.setText("FPS: —")
        self.connect_btn.setText("Disconnect")
        self.host_edit.setEnabled(False)
        self.port_spin.setEnabled(False)
        self.timeout_spin.setEnabled(False)
        self._push_worker_payload()
        if not self._worker.isRunning():
            self._worker.start()
        self._status_label.setText(f"Connected to {host}:{port} ({QT_API})")

    def _disconnect(self) -> None:
        self._worker.stop()
        self._worker.wait(3000)
        self._worker.close_session()
        self._connected = False
        self.connect_btn.setText("Connect")
        self.fps_label.setText("FPS: —")
        self.host_edit.setEnabled(True)
        self.port_spin.setEnabled(True)
        self.timeout_spin.setEnabled(True)
        self._status_label.setText("Disconnected")

    @Slot(int, float, object, float)
    def _on_frame(self, frame: int, elapsed: float, packet: object, fps: float) -> None:
        if fps > 0:
            self.fps_label.setText(f"FPS: {fps:.1f}")
        kind = packet[0]
        if kind == "rdm":
            response: RdmResponse = packet[1]
            pick_iq: IqResponse | None = packet[2] if len(packet) > 2 else None
            if response.status != 0:
                self._status_label.setText(f"frame {frame}: RDM status={response.status}")
                return
            self._update_rdm_plot(response)
            if pick_iq is not None and pick_iq.status == 0:
                self._refresh_rdm_pick_from_iq(pick_iq)
            self._status_label.setText(
                f"frame {frame} t={elapsed:.2f}s RDM {response.range_count}x{response.doppler_count} OK"
            )
            return

        _kind, targets, response, profile = packet
        iq: IqResponse = response
        if iq.status != 0:
            self._status_label.setText(f"frame {frame}: iq-server status={iq.status}")
            return

        mode = self._active_view_mode()
        if mode == ViewMode.TARGET_LIST:
            self._update_target_table(targets, iq)
        else:
            self._update_profile_plot(targets, iq, profile)
        self._status_label.setText(f"frame {frame} t={elapsed:.2f}s targets={len(targets)} OK")

    def _update_target_table(self, targets: list[Target], response: IqResponse) -> None:
        self.target_table.setRowCount(len(targets))
        for row, (target, samples) in enumerate(zip(targets, response.targets)):
            s = samples[0]
            self._set_table_row(self.target_table, row, target, (s.i, s.q, s.magnitude, s.phase_deg))

    def _update_profile_plot(
        self,
        targets: list[Target],
        response: IqResponse,
        profile: ProfileState | None,
    ) -> None:
        distances: list[float] = []
        mags: list[float] = []
        phases: list[float] = []
        for target, samples in zip(targets, response.targets):
            s = samples[0]
            distances.append(target.distance_m)
            mags.append(s.magnitude)
            phases.append(s.phase_rad)

        self.profile_curve.setData(distances, mags)
        if profile:
            self._profile_plot_bounds = (profile.plot_x_min, profile.plot_x_max)
        self._apply_profile_x_axis()

        if mags:
            peak_idx = max(range(len(mags)), key=lambda i: mags[i])
            self.profile_peak_line.setValue(distances[peak_idx])
            self.profile_plot.setTitle(
                f"Range profile — peak {distances[peak_idx]:.3f} m  |IQ|={mags[peak_idx]:.0f}"
            )

        self._ensure_phase_dials(len(distances))
        for dial, dist, phase in zip(self._phase_dials, distances, phases):
            dial.set_sample(dist, phase)

    def _rdm_mag_floor_value(self) -> float:
        return float(self.rdm_mag_floor.value())

    def _populate_rdm_colormap_combo(self, view_mode: str, *, keep_selection: bool = False) -> None:
        cmaps = COLORMAPS if view_mode == RDM_VIEW_POWER else PHASE_COLORMAPS
        default = RDM_DEFAULT_CMAPS[view_mode]
        current = self.rdm_cmap.currentData() if keep_selection else None
        self.rdm_cmap.blockSignals(True)
        self.rdm_cmap.clear()
        for name in cmaps:
            self.rdm_cmap.addItem(name, name)
        if keep_selection and current in cmaps:
            self.rdm_cmap.setCurrentIndex(cmaps.index(current))
        else:
            self.rdm_cmap.setCurrentIndex(cmaps.index(default))
        self.rdm_cmap.blockSignals(False)

    def _on_rdm_view_mode_changed(self) -> None:
        view_mode = self.rdm_view_mode.currentData()
        self._populate_rdm_colormap_combo(view_mode)
        uses_floor = view_mode in (RDM_VIEW_POWER, RDM_VIEW_BLEND)
        self.rdm_mag_floor.setEnabled(uses_floor)
        self._on_rdm_display_changed()

    def _on_rdm_display_changed(self) -> None:
        self._apply_rdm_colormap()
        if self._last_rdm_response is not None:
            self._update_rdm_plot(self._last_rdm_response)

    def _rdm_plot_rect(self, response: RdmResponse) -> tuple[float, float, float, float]:
        range_min = response.range_min_m
        range_max = range_min + response.range_step_m * max(response.range_count - 1, 0)
        vel_min = response.velocity_min_mps
        if response.doppler_count > 1:
            vel_max = vel_min + response.velocity_step_mps * (response.doppler_count - 1)
        else:
            vel_max = vel_min
        if response.doppler_count > 1 and vel_max == vel_min:
            vel_max = vel_min + VELOCITY_BIN_SIZE_MPS * (response.doppler_count - 1)
        range_span = max(range_max - range_min, response.range_step_m)
        vel_span = max(vel_max - vel_min, response.velocity_step_mps or VELOCITY_BIN_SIZE_MPS)
        return range_min, vel_min, range_span, vel_span

    def _reset_rdm_rgba_mode(self) -> None:
        """RGBA passthrough: ColorBarItem levels/LUT break alpha compositing."""
        self.rdm_image.setLevels(None, update=False)
        self.rdm_image.lut = None
        self.rdm_image._colorMap = None  # noqa: SLF001

    def _apply_rdm_colormap(self) -> None:
        view_mode = self.rdm_view_mode.currentData()
        name = self.rdm_cmap.currentData() or RDM_DEFAULT_CMAPS[view_mode]
        cmap = get_pg_colormap(name)
        self.rdm_colorbar.setColorMap(cmap)
        self.rdm_colorbar.setLevels((0.0, 1.0))
        axis = self.rdm_colorbar.getAxis("left")
        if view_mode == RDM_VIEW_POWER:
            axis.setLabel("|IQ| (norm.)")
        elif view_mode == RDM_VIEW_PHASE:
            axis.setLabel("phase (−π … +π)")
        else:
            axis.setLabel("phase (−π … +π), α←|IQ|")
        if view_mode == RDM_VIEW_BLEND:
            self._reset_rdm_rgba_mode()

    def _update_rdm_plot(self, response: RdmResponse) -> None:
        if not response.cells:
            return

        self._last_rdm_response = response
        view_mode = self.rdm_view_mode.currentData()
        cmap_name = self.rdm_cmap.currentData() or RDM_DEFAULT_CMAPS[view_mode]
        floor = self._rdm_mag_floor_value()

        mag = np.array(response.magnitude_grid(), dtype=np.float32)
        phase = np.array(response.phase_grid(), dtype=np.float32)
        norm, vmax = scale_magnitude_grid(mag.astype(np.float64), floor=floor)
        self._rdm_vmax = vmax

        mag_disp = arrange_rdm_grid(mag)
        phase_disp = arrange_rdm_grid(phase)
        norm_disp = arrange_rdm_grid(norm.astype(np.float32))

        if view_mode == RDM_VIEW_POWER:
            cmap = get_pg_colormap(cmap_name)
            self.rdm_image.setLookupTable(cmap.getLookupTable(nPts=256))
            self.rdm_image.setImage(norm_disp, autoLevels=False, autoDownsample=True)
            title = f"RDM power — peak |IQ|={vmax:.0f} (min scale {floor:.0f})"
        elif view_mode == RDM_VIEW_PHASE:
            phase_u = phase_to_unit(phase_disp)
            cmap = get_pg_colormap(cmap_name)
            self.rdm_image.setLookupTable(cmap.getLookupTable(nPts=256))
            self.rdm_image.setImage(phase_u, autoLevels=False, autoDownsample=True)
            title = f"RDM phase — peak |IQ|={vmax:.0f}"
        else:
            rgba = build_phase_rgba(phase_disp, norm_disp, cmap_name)
            self._reset_rdm_rgba_mode()
            self.rdm_image.setImage(rgba, autoLevels=False, autoDownsample=True)
            title = f"RDM phase×|IQ| — peak |IQ|={vmax:.0f} (min scale {floor:.0f})"

        rect = self._rdm_plot_rect(response)
        if rect != self._rdm_rect:
            self._rdm_rect = rect
            self.rdm_image.setRect(QRectF(*rect))
            self.rdm_plot.setXRange(rect[0], rect[0] + rect[2], padding=0.02)
            self.rdm_plot.setYRange(rect[1], rect[1] + rect[3], padding=0.02)

        self.rdm_plot.setTitle(title)

    def _snap_rdm_range_m(self, response: RdmResponse, dist_m: float) -> float:
        step = response.range_step_m if response.range_step_m > 0 else RANGE_BIN_SIZE_M
        if response.range_count <= 0:
            return dist_m
        range_idx = int(round((dist_m - response.range_min_m) / step))
        range_idx = max(0, min(range_idx, response.range_count - 1))
        return response.range_min_m + range_idx * step

    def _clear_rdm_pick(self) -> None:
        self._rdm_pick_range_m = None
        self.rdm_pick_line.setVisible(False)
        self.rdm_pick_dial.set_sample(0.0, 0.0)
        self.rdm_pick_info.setText("L-click: range line\nR-click: clear")
        if self._connected and self._active_view_mode() == ViewMode.RDM:
            self._push_worker_payload()

    def _refresh_rdm_pick_from_iq(self, pick_iq: IqResponse) -> None:
        if self._rdm_pick_range_m is None or not pick_iq.targets:
            return
        sample = pick_iq.targets[0][0]
        dist_m = self._rdm_pick_range_m
        self.rdm_pick_line.setValue(dist_m)
        self.rdm_pick_line.setVisible(True)
        self.rdm_pick_dial.set_sample(dist_m, sample.phase_rad)
        self.rdm_pick_info.setText(
            f"R={dist_m:.3f} m (range cube)\n"
            f"|IQ|={sample.magnitude:.0f}  φ={sample.phase_deg:.1f}°"
        )

    @Slot(object)
    def _on_rdm_plot_clicked(self, event) -> None:
        if self._last_rdm_response is None:
            return
        vb = self.rdm_plot.getPlotItem().vb
        if not vb.sceneBoundingRect().contains(event.scenePos()):
            return
        if event.button() == Qt.MouseButton.RightButton:
            self._clear_rdm_pick()
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        pos = vb.mapSceneToView(event.scenePos())
        self._rdm_pick_range_m = self._snap_rdm_range_m(self._last_rdm_response, pos.x())
        if self._connected and self._active_view_mode() == ViewMode.RDM:
            self._push_worker_payload()
        self.rdm_pick_line.setValue(self._rdm_pick_range_m)
        self.rdm_pick_line.setVisible(True)
        self.rdm_pick_info.setText(f"R={self._rdm_pick_range_m:.3f} m\n(awaiting IQ…)")

    @Slot(str)
    def _on_poll_error(self, message: str) -> None:
        self._status_label.setText(f"poll error: {message}")
        if self._connected:
            self._disconnect()
            QMessageBox.warning(self, "Connection lost", message)

    def closeEvent(self, event) -> None:  # noqa: N802
        if self._connected:
            self._disconnect()
        super().closeEvent(event)


def main() -> int:
    pg.setConfigOptions(foreground="#cccccc")
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
