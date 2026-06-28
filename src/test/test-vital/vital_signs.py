"""TI IWRL6432 vital signs algorithm (Python port of vitalsign.c)."""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field

import numpy as np

PI = math.pi

# vitalsign.h
REFRESH_RATE = 32
VS_TOTAL_FRAME = 128
VS_FFT_SIZE = 512
PHASE_FFT_SIZE = 512

VS_NUM_ANGLE_SEL_BIN = 9
VS_NUM_RANGE_SEL_BIN = 5

HEART_INDEX_START = 68
HEART_INDEX_END = 128
HEART_RATE_DECISION_THRESH = 3
BREATH_INDEX_START = 3
BREATH_INDEX_END = 50
HEART_RATE_JUMP_LIMIT = 12
VITALS_MASK_LOOP_NO = 7


def spectrum_scale(frame_rate_hz: float, fft_size: int = PHASE_FFT_SIZE) -> float:
    """BPM per FFT bin (TI SPECTRUM_MULTIPLICATION_FACTOR)."""
    return frame_rate_hz * 60.0 / fft_size


def compute_phase_unwrap(phase: float, phase_prev: float, diff_phase_correction_cum: float) -> tuple[float, float]:
    """Port of MmwDemo_computePhaseUnwrap."""
    diff_phase = phase - phase_prev

    if diff_phase > PI:
        mod_factor = 1.0
    elif diff_phase < -PI:
        mod_factor = -1.0
    else:
        mod_factor = 0.0

    diff_phase_mod = diff_phase - mod_factor * 2.0 * PI

    if diff_phase_mod == -PI and diff_phase > 0:
        diff_phase_mod = PI

    diff_phase_correction = diff_phase_mod - diff_phase

    if (0.0 < diff_phase_correction < PI) or (-PI < diff_phase_correction < 0.0):
        diff_phase_correction = 0.0

    diff_phase_correction_cum += diff_phase_correction
    phase_out = phase + diff_phase_correction_cum
    return phase_out, diff_phase_correction_cum


def compute_deviation(samples: np.ndarray) -> float:
    """Port of MmwDemo_computeMyDeviation (population variance)."""
    if samples.size < 1:
        return -1.0
    mean = float(np.mean(samples))
    return float(np.mean(samples * samples) - mean * mean)


def _triplet_peak(spectrum: np.ndarray, start: int, end: int) -> tuple[float, int]:
    peak_value = 0.0
    peak_idx = start
    for idx in range(start, end):
        compare = float(spectrum[idx - 1] + spectrum[idx] + spectrum[idx + 1])
        if compare > peak_value:
            peak_value = compare
            peak_idx = idx
    return peak_value, peak_idx


def _quintuplet_peak(spectrum: np.ndarray, start: int, end: int) -> tuple[float, int]:
    peak_value = 0.0
    peak_idx = start
    for idx in range(start, end):
        compare = float(
            spectrum[idx - 2]
            + spectrum[idx - 1]
            + spectrum[idx]
            + spectrum[idx + 1]
            + spectrum[idx + 2]
        )
        if compare > peak_value:
            peak_value = compare
            peak_idx = idx
    return peak_value, peak_idx


def _extract_phase_series(complex_series: np.ndarray) -> np.ndarray:
    """Unwrap phase and return frame-to-frame delta (127 samples)."""
    phases = np.empty(len(complex_series), dtype=np.float64)
    diff_cum = 0.0
    phase_prev = math.atan2(complex_series[0].imag, complex_series[0].real)
    phases[0] = phase_prev

    for idx in range(1, len(complex_series)):
        point_phase = math.atan2(complex_series[idx].imag, complex_series[idx].real)
        unwrapped, diff_cum = compute_phase_unwrap(point_phase, phase_prev, diff_cum)
        phase_prev = point_phase
        phases[idx] = unwrapped

    return np.diff(phases)


@dataclass
class VitalSignsResult:
    heart_rate_bpm: float = 0.0
    breathing_rate_bpm: float = 0.0
    breathing_deviation: float = 0.0
    range_bin: int = 0
    target_id: int = 0
    heart_history: list[float] = field(default_factory=list)
    breath_history: list[float] = field(default_factory=list)


class VitalSignsProcessor:
    """Process beamformed complex samples from multiple range bins."""

    def __init__(
        self,
        frame_rate_hz: float = 20.0,
        refresh_rate: int = REFRESH_RATE,
        buffer_frames: int = VS_TOTAL_FRAME,
        fft_size: int = VS_FFT_SIZE,
    ) -> None:
        self.frame_rate_hz = frame_rate_hz
        self.refresh_rate = refresh_rate
        self.buffer_frames = buffer_frames
        self.fft_size = fft_size
        self.scale = spectrum_scale(frame_rate_hz, fft_size)

        self._frame_count = 0
        self._vs_loop = 0
        self._buffers: dict[int, deque[complex]] = {}
        self._previous_heart_peak = [0, 0, 0, 0]
        self._heart_hist_index = 0
        self._breath_hist_index = 0

    def reset(self) -> None:
        self._frame_count = 0
        self._vs_loop = 0
        self._buffers.clear()
        self._previous_heart_peak = [0, 0, 0, 0]
        self._heart_hist_index = 0
        self._breath_hist_index = 0

    def add_sample(self, range_bin: int, sample: complex) -> None:
        if range_bin not in self._buffers:
            self._buffers[range_bin] = deque(maxlen=self.buffer_frames)
        self._buffers[range_bin].append(sample)
        self._frame_count += 1

    @property
    def min_buffer_fill(self) -> int:
        if not self._buffers:
            return 0
        return min(len(buf) for buf in self._buffers.values())

    @property
    def ready(self) -> bool:
        if not self._buffers:
            return False
        return self.min_buffer_fill >= self.buffer_frames

    def process(self, indicate_no_target: bool = False) -> VitalSignsResult | None:
        if self._frame_count == 0 or self._frame_count % self.refresh_rate != 0:
            return None

        if indicate_no_target or not self.ready:
            self._vs_loop = 0
            return VitalSignsResult()

        range_bins = sorted(self._buffers.keys())

        breath_rate_array: list[int] = []
        heartrate_array: list[int] = []
        heart_rate_sub1: list[int] = []
        heart_rate_sub2: list[int] = []

        breath_storage = np.zeros(PHASE_FFT_SIZE // 2, dtype=np.float64)
        heart_storage = np.zeros(PHASE_FFT_SIZE // 2, dtype=np.float64)
        breath_waveform_for_dev: np.ndarray | None = None

        center_idx = len(range_bins) // 2

        for rb_idx, range_bin in enumerate(range_bins):
            series = np.array(self._buffers[range_bin], dtype=np.complex128)
            displacement = _extract_phase_series(series)

            if rb_idx == center_idx:
                breath_waveform_for_dev = displacement.copy()

            padded = np.zeros(self.fft_size, dtype=np.complex128)
            n = min(len(displacement), self.fft_size)
            padded[:n] = displacement[:n]
            breath_spec = np.abs(np.fft.fft(padded)) ** 2

            _, breath_peak_idx = _triplet_peak(breath_spec, BREATH_INDEX_START, BREATH_INDEX_END)

            decimated = np.zeros(PHASE_FFT_SIZE // 2, dtype=np.float64)
            for spec_idx in range(PHASE_FFT_SIZE // 4):
                decimated[spec_idx] = breath_spec[2 * spec_idx] * breath_spec[spec_idx]

            breath_storage[BREATH_INDEX_START:BREATH_INDEX_END] += breath_spec[
                BREATH_INDEX_START:BREATH_INDEX_END
            ]
            heart_storage[HEART_INDEX_START:HEART_INDEX_END] += decimated[
                HEART_INDEX_START:HEART_INDEX_END
            ]

            _, heart_peak_idx = _triplet_peak(decimated, HEART_INDEX_START, HEART_INDEX_END)
            breath_rate_array.append(breath_peak_idx)
            heartrate_array.append(heart_peak_idx)

            decimated[heart_peak_idx - 1 : heart_peak_idx + 2] = 0.0
            _, heart_peak_idx = _triplet_peak(decimated, HEART_INDEX_START, HEART_INDEX_END)
            heart_rate_sub1.append(heart_peak_idx)

            decimated[heart_peak_idx - 1 : heart_peak_idx + 2] = 0.0
            _, heart_peak_idx = _triplet_peak(decimated, HEART_INDEX_START, HEART_INDEX_END)
            heart_rate_sub2.append(heart_peak_idx)

        breath_hist = np.zeros(self.fft_size, dtype=np.float64)
        for peak in breath_rate_array:
            breath_hist[peak] += 1.0
        _, breath_hist_index = _triplet_peak(breath_hist, BREATH_INDEX_START, BREATH_INDEX_END)
        self._breath_hist_index = breath_hist_index

        n_bins = len(heartrate_array)
        if n_bins >= 5:
            for block in range(n_bins // 5):
                base = block * 5
                for offset in (0, 4):
                    heartrate_array[base + offset] = 0
                    heart_rate_sub1[base + offset] = 0
                    heart_rate_sub2[base + offset] = 0

        heart_hist = np.zeros(self.fft_size, dtype=np.float64)
        for idx in range(len(heartrate_array)):
            heart_hist[heartrate_array[idx]] += 1.0
            heart_hist[heart_rate_sub1[idx]] += 1.0

        _, heart_hist_index = _quintuplet_peak(heart_hist, HEART_INDEX_START, HEART_INDEX_END)
        self._heart_hist_index = heart_hist_index

        heart_temp = heart_storage.copy()
        present_peaks: list[int] = []
        for _ in range(5):
            _, peak_idx = _triplet_peak(heart_temp, HEART_INDEX_START, HEART_INDEX_END)
            present_peaks.append(peak_idx)
            heart_temp[peak_idx - 1 : peak_idx + 2] = 0.0

        compare_previous = self._previous_heart_peak[3]
        diffs = [abs(p - compare_previous) for p in present_peaks]
        compare_index = int(np.argmin(diffs))
        compare_value = diffs[compare_index]

        if compare_value < HEART_RATE_DECISION_THRESH:
            heart_peak_idx = present_peaks[compare_index]
        else:
            heart_peak_idx = heart_hist_index

        prev0 = self._previous_heart_peak[0]
        heart_peak_diff = abs(heart_peak_idx - prev0)
        if heart_peak_diff > HEART_RATE_JUMP_LIMIT and self._vs_loop > VITALS_MASK_LOOP_NO:
            if heart_peak_idx > prev0:
                heart_peak_idx = prev0 + HEART_RATE_JUMP_LIMIT
            else:
                heart_peak_idx = prev0 - HEART_RATE_JUMP_LIMIT

        if self._vs_loop > 4:
            self._previous_heart_peak = [
                heart_peak_idx,
                self._previous_heart_peak[0],
                self._previous_heart_peak[1],
                self._previous_heart_peak[2],
            ]
        elif self._vs_loop == 0:
            self._previous_heart_peak = [0, 0, 0, 0]

        breathing_deviation = 0.0
        if breath_waveform_for_dev is not None and breath_waveform_for_dev.size >= 99:
            breathing_deviation = compute_deviation(breath_waveform_for_dev[59:99])

        result = VitalSignsResult(
            heart_rate_bpm=heart_peak_idx * self.scale,
            breathing_rate_bpm=breath_hist_index * self.scale,
            breathing_deviation=breathing_deviation,
            range_bin=range_bins[center_idx] if range_bins else 0,
        )

        if self._vs_loop < VITALS_MASK_LOOP_NO:
            result.heart_rate_bpm = 0.0
            result.breathing_rate_bpm = 0.0
        else:
            result.heart_history = [result.heart_rate_bpm]
            result.breath_history = [result.breathing_rate_bpm]

        self._vs_loop += 1
        return result
