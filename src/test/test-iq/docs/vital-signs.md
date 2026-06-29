# HR / BR Vital Signs — test-iq GUI (current version)

Baseline document for the **HR / BR** tab pipeline. When the algorithm changes, append a dated section at the bottom rather than rewriting this file.

**Code:** `src/test/test-vital/vital_signs.py`  
**GUI:** `src/test/test-iq/gui.py`  
**References:** `src/r4sn/docs/research.md`, `docs/papers/` (Bennya BioCAS 2023, Hao Sci Reports 2025, Ren arXiv 2024)

---

## 1. Data path (sensor → GUI)

1. **IQ server** beamforms 192 VA at user az/el (`Chirp mode = Average`) and returns one complex sample per range target per frame (~20 fps).
2. **GUI poll** sends one merged request: range profile targets + 5 vital range bins (centre ±2 firmware bins) when a pick is active.
3. **`VitalSignsProcessor`** ingests IQ for 5 range bins, extracts phase, estimates HR/BR.

| Setting | Default | Notes |
|---------|---------|--------|
| Buffer frames | 400 | ~20 s @ 20 fps; adjustable in GUI |
| FFT size | 512 | Zero-padded slow-time FFT |
| HR/BR update | every 8 frames | ~0.4 s @ 20 fps |
| Live spectrum | every 4 frames | After ≥50% buffer fill |
| Vital chirp mode | Average (1) | Preserves phase; ~18 dB coherent gain |

---

## 2. Preprocessing (per range bin)

1. **Phase extraction** — TI `atan2sp_i` via `libatan2sp.so` (`atan2(imag, real)`).
2. **Incremental unwrap** — TI `MmwDemo_computePhaseUnwrap` (streaming `PhaseTracker` per bin).
3. **Phase differencing** — consecutive unwrapped phase delta (removes static clutter).
4. **DC removal** — subtract mean of delta series.
5. **Impulse smoothing** — Hao et al.: 8-pass moving average, window 5 (batch path); lighter 2-pass on live preview.
6. **Displacement** — \( \Delta x = \frac{\lambda}{4\pi} \Delta\psi \), λ ≈ 3.89 mm @ 77 GHz.

---

## 3. Rate estimation

### Breathing (Bennya-style fusion)

Bandpass 0.10–0.70 Hz (~6–42 brpm), then:

| Estimator | Weight |
|-----------|--------|
| FFT top-4 peak average | 0.45 |
| Autocorrelation | 0.35 |
| Time-domain peak count | 0.20 |

Multi-range bins (5): histogram vote + triplet peak (TI legacy).

### Heart rate (Bennya 6-estimator + comb filter)

Bandpass 0.80–3.33 Hz; peak search 0.80–2.50 Hz (~48–150 bpm).

**Before comb filter:** FFT top-6, autocorrelation, peak count.  
**Comb filter:** notch breath fundamental and harmonics (time + spectrum).  
**After comb:** repeat three estimators.

| Estimate | Weight |
|----------|--------|
| FFT (pre-comb) | 0.20 |
| Autocorr (pre-comb) | 0.15 |
| Peaks (pre-comb) | 0.15 |
| FFT (post-comb) | 0.20 |
| Autocorr (post-comb) | 0.15 |
| Peaks (post-comb) | 0.15 |

Multi-range fusion: TI quintuplet histogram, jump limiter (±12 bins), 7-frame warmup mask.

### BPM scale

`BPM per bin = frame_rate_hz × 60 / fft_size`  
Displayed HR in **bpm**, BR in **brpm**; spectrum X-axis in **BPM**.

---

## 4. GUI behaviour

- **Pick:** L-click range profile → snap to profile grid, then firmware range-bin grid (`RANGE_BIN_SIZE_M`).
- **5 dials** under profile: phase per profile bin.
- **Buffer label:** `fill/need frames @ Hz`; fills only after pick resolves to 5 IQ indices.
- **Plots:** live displacement (mm); spectrum vs **BPM** with breath/heart band shading.

---

## 5. Known constraints

- MaxAbs chirp mode breaks phase continuity — not used for vitals.
- Buffer must fill on **all 5** range bins (`min_buffer_fill`).
- A-VMD (Hao) not implemented in Python path (CPU cost); comb + multi-estimator used instead.

---

## Changelog

### 2026-06-28 — initial baseline

- 400-frame default buffer, Bennya/Hao/TI hybrid pipeline, BPM spectrum axis, pick→index resolution fix.
