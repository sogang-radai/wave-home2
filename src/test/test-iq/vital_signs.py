"""HR / BR vital-signs DSP for test-iq GUI (Bennya / Hao / TI hybrid).

See docs/vital-signs.md for the pipeline baseline.
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field

import numpy as np

PI = math.pi

DEFAULT_BUFFER_FRAMES = 400
DEFAULT_FFT_SIZE = 2048  # finer BPM bins for display / estimation (~0.6 bpm @ 20 fps)

# λ ≈ 3.89 mm @ 77 GHz → displacement = λ / (4π) * Δψ
WAVELENGTH_M = 3.89e-3
PHASE_TO_DISPLACEMENT = WAVELENGTH_M / (4.0 * PI)

PROCESS_EVERY_N = 8
PREVIEW_EVERY_N = 4
PREVIEW_MIN_FILL_RATIO = 0.5
WARMUP_LOOPS = 7
HEART_JUMP_LIMIT = 12
HEART_DECISION_THRESH = 3

BREATH_HZ = (0.10, 0.70)
HEART_BAND_HZ = (0.80, 3.33)
HEART_SEARCH_HZ = (0.80, 2.50)

BREATH_WEIGHTS = (0.45, 0.35, 0.20)  # fft, autocorr, peaks
HEART_WEIGHTS = (0.20, 0.15, 0.15, 0.20, 0.15, 0.15)


@dataclass(frozen=True)
class AnalysisBins:
    breath_start: int
    breath_end: int
    heart_band_start: int
    heart_band_end: int
    heart_search_start: int
    heart_search_end: int


@dataclass
class VitalSpectrumDetail:
    breath_spectrum: np.ndarray
    heart_spectrum: np.ndarray
    breath_peak_bin: int
    heart_peak_bin: int


@dataclass
class VitalPreview:
    displacement_m: np.ndarray
    breath_spectrum: np.ndarray | None = None
    heart_spectrum: np.ndarray | None = None
    breath_peak_bin: int = 0
    heart_peak_bin: int = 0


@dataclass
class VitalSignsResult:
    heart_rate_bpm: float = 0.0
    breathing_rate_bpm: float = 0.0
    breathing_deviation: float = 0.0
    range_bin: int = 0
    detail: VitalSpectrumDetail | None = None
    heart_history: list[float] = field(default_factory=list)
    breath_history: list[float] = field(default_factory=list)


@dataclass
class _PhaseTracker:
    phase_prev: float = 0.0
    diff_cum: float = 0.0
    last_unwrapped: float = 0.0
    initialized: bool = False


def spectrum_scale(frame_rate_hz: float, fft_size: int = DEFAULT_FFT_SIZE) -> float:
    """BPM (or brpm) per FFT bin."""
    if fft_size <= 0 or frame_rate_hz <= 0:
        return 0.0
    return frame_rate_hz * 60.0 / fft_size


def analysis_bins(
    frame_rate_hz: float,
    fft_size: int = DEFAULT_FFT_SIZE,
    buffer_frames: int = DEFAULT_BUFFER_FRAMES,
) -> AnalysisBins:
    """Map physiological bands (Hz) to FFT bin indices."""
    del buffer_frames  # reserved for future window-aware band tweaks
    scale_hz = frame_rate_hz / max(fft_size, 1)

    def hz_to_bin(hz: float) -> int:
        if scale_hz <= 0:
            return 0
        return int(round(hz / scale_hz))

    half = fft_size // 2
    breath_start = max(1, hz_to_bin(BREATH_HZ[0]))
    breath_end = min(half - 1, max(breath_start + 1, hz_to_bin(BREATH_HZ[1])))
    heart_band_start = max(1, hz_to_bin(HEART_BAND_HZ[0]))
    heart_band_end = min(half - 1, max(heart_band_start + 1, hz_to_bin(HEART_BAND_HZ[1])))
    heart_search_start = max(heart_band_start, hz_to_bin(HEART_SEARCH_HZ[0]))
    heart_search_end = min(heart_band_end, max(heart_search_start + 1, hz_to_bin(HEART_SEARCH_HZ[1])))
    return AnalysisBins(
        breath_start=breath_start,
        breath_end=breath_end,
        heart_band_start=heart_band_start,
        heart_band_end=heart_band_end,
        heart_search_start=heart_search_start,
        heart_search_end=heart_search_end,
    )


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
    return phase + diff_phase_correction_cum, diff_phase_correction_cum


def _moving_average(x: np.ndarray, window: int = 5, passes: int = 1) -> np.ndarray:
    if x.size < 2 or window < 2 or passes < 1:
        return x
    kernel = np.ones(window, dtype=np.float64) / window
    out = x.astype(np.float64, copy=True)
    for _ in range(passes):
        out = np.convolve(out, kernel, mode="same")
    return out


def _bandpass_fft(x: np.ndarray, frame_rate_hz: float, lo_hz: float, hi_hz: float) -> np.ndarray:
    if x.size < 8 or frame_rate_hz <= 0:
        return x.copy()
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(x.size, d=1.0 / frame_rate_hz)
    mask = (freqs >= lo_hz) & (freqs <= hi_hz)
    filtered = np.zeros_like(spec)
    filtered[mask] = spec[mask]
    return np.fft.irfft(filtered, n=x.size)


def _power_spectrum(x: np.ndarray, fft_size: int) -> np.ndarray:
    padded = np.zeros(fft_size, dtype=np.float64)
    n = min(x.size, fft_size)
    if n > 0:
        padded[:n] = x[:n]
    return np.abs(np.fft.fft(padded)) ** 2


def _top_peak_bins(spectrum: np.ndarray, start: int, end: int, count: int) -> list[int]:
    start = max(1, start)
    end = min(spectrum.size - 1, end)
    if end <= start:
        return [start]
    region = spectrum[start:end].copy()
    peaks: list[int] = []
    for _ in range(max(1, count)):
        if region.size == 0:
            break
        local = int(np.argmax(region))
        peaks.append(start + local)
        lo = max(0, local - 1)
        hi = min(region.size, local + 2)
        region[lo:hi] = 0.0
    return peaks or [start]


def _fft_rate_bin(spectrum: np.ndarray, start: int, end: int, top_k: int) -> float:
    peaks = _top_peak_bins(spectrum, start, end, top_k)
    if not peaks:
        return float(start)
    weights = [max(float(spectrum[p]), 1e-12) for p in peaks]
    return float(np.average(peaks, weights=weights))


def _autocorr_rate_bin(
    x: np.ndarray,
    frame_rate_hz: float,
    lo_hz: float,
    hi_hz: float,
    fft_size: int,
) -> float:
    if x.size < 16 or frame_rate_hz <= 0:
        return 0.0
    x = x - np.mean(x)
    corr = np.correlate(x, x, mode="full")
    corr = corr[corr.size // 2 :]
    min_lag = max(1, int(frame_rate_hz / hi_hz))
    max_lag = min(corr.size - 1, max(min_lag + 1, int(frame_rate_hz / lo_hz)))
    if max_lag <= min_lag:
        return 0.0
    lag = min_lag + int(np.argmax(corr[min_lag : max_lag + 1]))
    if lag <= 0:
        return 0.0
    hz = frame_rate_hz / lag
    return hz * 60.0 / spectrum_scale(frame_rate_hz, fft_size)


def _peak_count_rate_bin(
    x: np.ndarray,
    frame_rate_hz: float,
    lo_hz: float,
    hi_hz: float,
    fft_size: int,
) -> float:
    if x.size < 16 or frame_rate_hz <= 0:
        return 0.0
    x = x - np.mean(x)
    thr = 0.2 * float(np.max(np.abs(x)) + 1e-12)
    peaks = 0
    for i in range(1, x.size - 1):
        if x[i] > x[i - 1] and x[i] >= x[i + 1] and x[i] > thr:
            peaks += 1
    duration_s = x.size / frame_rate_hz
    if duration_s <= 0:
        return 0.0
    hz = peaks / duration_s
    hz = min(max(hz, lo_hz), hi_hz)
    return hz * 60.0 / spectrum_scale(frame_rate_hz, fft_size)


def _triplet_peak(spectrum: np.ndarray, start: int, end: int) -> tuple[float, int]:
    peak_value = 0.0
    peak_idx = start
    start = max(1, start)
    end = min(spectrum.size - 1, end)
    for idx in range(start, end):
        compare = float(spectrum[idx - 1] + spectrum[idx] + spectrum[idx + 1])
        if compare > peak_value:
            peak_value = compare
            peak_idx = idx
    return peak_value, peak_idx


def _quintuplet_peak(spectrum: np.ndarray, start: int, end: int) -> tuple[float, int]:
    peak_value = 0.0
    peak_idx = start
    start = max(2, start)
    end = min(spectrum.size - 2, end)
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


def _comb_notch_time(x: np.ndarray, breath_hz: float, frame_rate_hz: float, harmonics: int = 3) -> np.ndarray:
    """Notch breath fundamental and harmonics in the time domain via FFT zeroing."""
    if x.size < 8 or breath_hz <= 0 or frame_rate_hz <= 0:
        return x.copy()
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(x.size, d=1.0 / frame_rate_hz)
    width = max(breath_hz * 0.15, frame_rate_hz / x.size)
    for h in range(1, harmonics + 1):
        f0 = breath_hz * h
        spec[(freqs >= f0 - width) & (freqs <= f0 + width)] = 0.0
    return np.fft.irfft(spec, n=x.size)


def _comb_notch_spectrum(spectrum: np.ndarray, breath_bin: int, harmonics: int = 3) -> np.ndarray:
    out = spectrum.copy()
    if breath_bin <= 0:
        return out
    for h in range(1, harmonics + 1):
        center = breath_bin * h
        lo = max(0, center - 2)
        hi = min(out.size, center + 3)
        out[lo:hi] = 0.0
    return out


def _compute_deviation(samples: np.ndarray) -> float:
    if samples.size < 1:
        return -1.0
    mean = float(np.mean(samples))
    return float(np.mean(samples * samples) - mean * mean)


class VitalSignsProcessor:
    """Ingest beamformed IQ per range bin and estimate HR / BR."""

    def __init__(
        self,
        frame_rate_hz: float = 20.0,
        buffer_frames: int = DEFAULT_BUFFER_FRAMES,
        fft_size: int = DEFAULT_FFT_SIZE,
    ) -> None:
        self.frame_rate_hz = max(frame_rate_hz, 1e-3)
        self.buffer_frames = max(int(buffer_frames), 16)
        self.fft_size = max(int(fft_size), 64)
        self.scale = spectrum_scale(self.frame_rate_hz, self.fft_size)
        self.bins = analysis_bins(self.frame_rate_hz, self.fft_size, self.buffer_frames)

        self._frame_count = 0
        self._vs_loop = 0
        self._buffers: dict[int, deque[complex]] = {}
        self._trackers: dict[int, _PhaseTracker] = {}
        self._phase_deltas: dict[int, deque[float]] = {}
        self._previous_heart_peak = [0, 0, 0, 0]
        self._last_preview: VitalPreview | None = None
        self._center_rb: int | None = None

    @property
    def last_preview(self) -> VitalPreview | None:
        return self._last_preview

    @property
    def min_buffer_fill(self) -> int:
        if not self._buffers:
            return 0
        return min(len(buf) for buf in self._buffers.values())

    def set_buffer_frames(self, buffer_frames: int) -> None:
        self.buffer_frames = max(int(buffer_frames), 16)
        self.bins = analysis_bins(self.frame_rate_hz, self.fft_size, self.buffer_frames)
        for rb, buf in list(self._buffers.items()):
            new_buf: deque[complex] = deque(buf, maxlen=self.buffer_frames)
            self._buffers[rb] = new_buf
        for rb, deltas in list(self._phase_deltas.items()):
            self._phase_deltas[rb] = deque(deltas, maxlen=max(self.buffer_frames - 1, 1))

    def reset(self) -> None:
        self._frame_count = 0
        self._vs_loop = 0
        self._buffers.clear()
        self._trackers.clear()
        self._phase_deltas.clear()
        self._previous_heart_peak = [0, 0, 0, 0]
        self._last_preview = None
        self._center_rb = None

    def add_sample(self, range_bin: int, sample: complex) -> None:
        if range_bin not in self._buffers:
            self._buffers[range_bin] = deque(maxlen=self.buffer_frames)
            self._phase_deltas[range_bin] = deque(maxlen=max(self.buffer_frames - 1, 1))
            self._trackers[range_bin] = _PhaseTracker()

        self._buffers[range_bin].append(sample)
        tracker = self._trackers[range_bin]
        phase = math.atan2(sample.imag, sample.real)
        if not tracker.initialized:
            tracker.phase_prev = phase
            tracker.diff_cum = 0.0
            tracker.last_unwrapped = phase
            tracker.initialized = True
        else:
            unwrapped, tracker.diff_cum = compute_phase_unwrap(
                phase, tracker.phase_prev, tracker.diff_cum
            )
            delta_psi = unwrapped - tracker.last_unwrapped
            tracker.last_unwrapped = unwrapped
            tracker.phase_prev = phase
            self._phase_deltas[range_bin].append(float(delta_psi))

        self._frame_count += 1
        if self._center_rb is None:
            self._center_rb = range_bin

        fill = self.min_buffer_fill
        if (
            fill >= int(self.buffer_frames * PREVIEW_MIN_FILL_RATIO)
            and self._frame_count % PREVIEW_EVERY_N == 0
        ):
            self._last_preview = self._build_preview()

    def process(self, indicate_no_target: bool = False) -> VitalSignsResult | None:
        if self._frame_count == 0 or self._frame_count % PROCESS_EVERY_N != 0:
            return None

        if indicate_no_target or self.min_buffer_fill < self.buffer_frames:
            self._vs_loop = 0
            return VitalSignsResult()

        range_bins = sorted(self._buffers.keys())
        if not range_bins:
            return VitalSignsResult()

        center_idx = len(range_bins) // 2
        center_rb = range_bins[center_idx]
        self._center_rb = center_rb

        breath_bins: list[int] = []
        heart_bins: list[int] = []
        heart_sub1: list[int] = []

        breath_storage = np.zeros(self.fft_size // 2, dtype=np.float64)
        heart_storage = np.zeros(self.fft_size // 2, dtype=np.float64)
        center_displacement: np.ndarray | None = None
        center_breath_spec: np.ndarray | None = None
        center_heart_spec: np.ndarray | None = None

        for rb_idx, rb in enumerate(range_bins):
            displacement = self._displacement_series(rb, passes=8)
            if displacement.size < 16:
                continue

            breath_wave = _bandpass_fft(displacement, self.frame_rate_hz, *BREATH_HZ)
            heart_wave = _bandpass_fft(displacement, self.frame_rate_hz, *HEART_BAND_HZ)

            breath_spec = _power_spectrum(breath_wave, self.fft_size)
            half = self.fft_size // 2
            breath_half = breath_spec[:half]

            breath_est = self._estimate_breath(breath_wave, breath_spec)
            breath_peak = int(round(breath_est))
            breath_bins.append(breath_peak)

            heart_pre = self._estimate_heart(heart_wave, breath_est_hz=breath_est * self.scale / 60.0)
            heart_bins.append(int(round(heart_pre)))

            heart_spec = _power_spectrum(heart_wave, self.fft_size)
            heart_half = heart_spec[:half]
            tmp = heart_half.copy()
            p1 = int(round(heart_pre))
            tmp[max(0, p1 - 1) : p1 + 2] = 0.0
            _, p2 = _triplet_peak(tmp, self.bins.heart_search_start, self.bins.heart_search_end)
            heart_sub1.append(p2)

            b0, b1 = self.bins.breath_start, self.bins.breath_end
            h0, h1 = self.bins.heart_search_start, self.bins.heart_search_end
            breath_storage[b0:b1] += breath_half[b0:b1]
            heart_storage[h0:h1] += heart_half[h0:h1]

            if rb_idx == center_idx:
                center_displacement = displacement
                center_breath_spec = breath_half
                center_heart_spec = heart_half

        if not breath_bins:
            return VitalSignsResult()

        breath_hist = np.zeros(self.fft_size, dtype=np.float64)
        for peak in breath_bins:
            if 0 <= peak < breath_hist.size:
                breath_hist[peak] += 1.0
        _, breath_hist_index = _triplet_peak(breath_hist, self.bins.breath_start, self.bins.breath_end)

        n_bins = len(heart_bins)
        if n_bins >= 5:
            for block in range(n_bins // 5):
                base = block * 5
                for offset in (0, 4):
                    heart_bins[base + offset] = 0
                    heart_sub1[base + offset] = 0

        heart_hist = np.zeros(self.fft_size, dtype=np.float64)
        for idx in range(len(heart_bins)):
            if 0 <= heart_bins[idx] < heart_hist.size:
                heart_hist[heart_bins[idx]] += 1.0
            if 0 <= heart_sub1[idx] < heart_hist.size:
                heart_hist[heart_sub1[idx]] += 1.0

        _, heart_hist_index = _quintuplet_peak(
            heart_hist, self.bins.heart_search_start, self.bins.heart_search_end
        )

        heart_temp = heart_storage.copy()
        present_peaks: list[int] = []
        for _ in range(5):
            _, peak_idx = _triplet_peak(heart_temp, self.bins.heart_search_start, self.bins.heart_search_end)
            present_peaks.append(peak_idx)
            heart_temp[max(0, peak_idx - 1) : peak_idx + 2] = 0.0

        compare_previous = self._previous_heart_peak[3]
        diffs = [abs(p - compare_previous) for p in present_peaks]
        compare_index = int(np.argmin(diffs)) if diffs else 0
        compare_value = diffs[compare_index] if diffs else HEART_DECISION_THRESH + 1

        if compare_value < HEART_DECISION_THRESH:
            heart_peak_idx = present_peaks[compare_index]
        else:
            heart_peak_idx = heart_hist_index

        prev0 = self._previous_heart_peak[0]
        if abs(heart_peak_idx - prev0) > HEART_JUMP_LIMIT and self._vs_loop > WARMUP_LOOPS:
            if heart_peak_idx > prev0:
                heart_peak_idx = prev0 + HEART_JUMP_LIMIT
            else:
                heart_peak_idx = prev0 - HEART_JUMP_LIMIT

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
        if center_displacement is not None and center_displacement.size >= 99:
            breathing_deviation = _compute_deviation(center_displacement[59:99])

        detail = None
        if center_breath_spec is not None and center_heart_spec is not None:
            detail = VitalSpectrumDetail(
                breath_spectrum=center_breath_spec,
                heart_spectrum=center_heart_spec,
                breath_peak_bin=breath_hist_index,
                heart_peak_bin=heart_peak_idx,
            )
            self._last_preview = VitalPreview(
                displacement_m=center_displacement if center_displacement is not None else np.array([]),
                breath_spectrum=center_breath_spec,
                heart_spectrum=center_heart_spec,
                breath_peak_bin=breath_hist_index,
                heart_peak_bin=heart_peak_idx,
            )

        result = VitalSignsResult(
            heart_rate_bpm=heart_peak_idx * self.scale,
            breathing_rate_bpm=breath_hist_index * self.scale,
            breathing_deviation=breathing_deviation,
            range_bin=center_rb,
            detail=detail,
        )

        if self._vs_loop < WARMUP_LOOPS:
            result.heart_rate_bpm = 0.0
            result.breathing_rate_bpm = 0.0
        else:
            result.heart_history = [result.heart_rate_bpm]
            result.breath_history = [result.breathing_rate_bpm]

        self._vs_loop += 1
        return result

    def _displacement_series(self, range_bin: int, passes: int) -> np.ndarray:
        deltas = self._phase_deltas.get(range_bin)
        if not deltas or len(deltas) < 2:
            # Fallback: recompute from IQ buffer
            buf = self._buffers.get(range_bin)
            if not buf or len(buf) < 2:
                return np.array([], dtype=np.float64)
            series = np.array(buf, dtype=np.complex128)
            phases = np.empty(len(series), dtype=np.float64)
            diff_cum = 0.0
            phase_prev = math.atan2(series[0].imag, series[0].real)
            phases[0] = phase_prev
            for idx in range(1, len(series)):
                point_phase = math.atan2(series[idx].imag, series[idx].real)
                unwrapped, diff_cum = compute_phase_unwrap(point_phase, phase_prev, diff_cum)
                phase_prev = point_phase
                phases[idx] = unwrapped
            delta = np.diff(phases)
        else:
            delta = np.array(deltas, dtype=np.float64)

        delta = delta - np.mean(delta)
        delta = _moving_average(delta, window=5, passes=passes)
        return delta * PHASE_TO_DISPLACEMENT

    def _estimate_breath(self, breath_wave: np.ndarray, breath_spec: np.ndarray) -> float:
        fft_bin = _fft_rate_bin(breath_spec, self.bins.breath_start, self.bins.breath_end, top_k=4)
        ac_bin = _autocorr_rate_bin(breath_wave, self.frame_rate_hz, *BREATH_HZ, self.fft_size)
        pk_bin = _peak_count_rate_bin(breath_wave, self.frame_rate_hz, *BREATH_HZ, self.fft_size)
        w = BREATH_WEIGHTS
        return w[0] * fft_bin + w[1] * ac_bin + w[2] * pk_bin

    def _estimate_heart(self, heart_wave: np.ndarray, breath_est_hz: float) -> float:
        # Pre-comb estimators
        spec_pre = _power_spectrum(heart_wave, self.fft_size)
        fft_pre = _fft_rate_bin(spec_pre, self.bins.heart_search_start, self.bins.heart_search_end, top_k=6)
        ac_pre = _autocorr_rate_bin(heart_wave, self.frame_rate_hz, *HEART_SEARCH_HZ, self.fft_size)
        pk_pre = _peak_count_rate_bin(heart_wave, self.frame_rate_hz, *HEART_SEARCH_HZ, self.fft_size)

        # Comb filter
        notched = _comb_notch_time(heart_wave, breath_est_hz, self.frame_rate_hz)
        breath_bin = int(round(breath_est_hz * 60.0 / max(self.scale, 1e-9)))
        spec_post = _comb_notch_spectrum(_power_spectrum(notched, self.fft_size), breath_bin)
        fft_post = _fft_rate_bin(spec_post, self.bins.heart_search_start, self.bins.heart_search_end, top_k=6)
        ac_post = _autocorr_rate_bin(notched, self.frame_rate_hz, *HEART_SEARCH_HZ, self.fft_size)
        pk_post = _peak_count_rate_bin(notched, self.frame_rate_hz, *HEART_SEARCH_HZ, self.fft_size)

        w = HEART_WEIGHTS
        return (
            w[0] * fft_pre
            + w[1] * ac_pre
            + w[2] * pk_pre
            + w[3] * fft_post
            + w[4] * ac_post
            + w[5] * pk_post
        )

    def _build_preview(self) -> VitalPreview:
        range_bins = sorted(self._buffers.keys())
        if not range_bins:
            return VitalPreview(displacement_m=np.array([]))
        center_rb = range_bins[len(range_bins) // 2]
        displacement = self._displacement_series(center_rb, passes=2)
        if displacement.size < 16:
            return VitalPreview(displacement_m=displacement)

        breath_wave = _bandpass_fft(displacement, self.frame_rate_hz, *BREATH_HZ)
        heart_wave = _bandpass_fft(displacement, self.frame_rate_hz, *HEART_BAND_HZ)
        half = self.fft_size // 2
        breath_spec = _power_spectrum(breath_wave, self.fft_size)[:half]
        heart_spec = _power_spectrum(heart_wave, self.fft_size)[:half]
        _, breath_peak = _triplet_peak(breath_spec, self.bins.breath_start, self.bins.breath_end)
        _, heart_peak = _triplet_peak(heart_spec, self.bins.heart_search_start, self.bins.heart_search_end)
        return VitalPreview(
            displacement_m=displacement,
            breath_spectrum=breath_spec,
            heart_spectrum=heart_spec,
            breath_peak_bin=breath_peak,
            heart_peak_bin=heart_peak,
        )
