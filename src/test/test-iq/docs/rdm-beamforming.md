# RDM Beamforming — test-iq viewer notes

Baseline for **Range–Doppler Map** beamforming behaviour, performance, and noise. Append dated sections when behaviour changes.

**Code:** `src/r4sn/iq-server/signal_processor.cpp` (`processRdm`), `steering_cache.hpp`, `va_geometry.h`  
**GUI:** `src/test/test-iq/gui.py` (RDM tab)

---

## 1. Work per RDM frame (PerChirp mode)

| Mode | Ops per cell | Typical grid | Beamforms / frame |
|------|--------------|--------------|-------------------|
| Single TX–RX | 1 VA read | 32 × 55 | 0 |
| Average all VA | 192 reads + sum | 32 × 55 | 0 |
| **Beamform** | **192 reads + 192 MAC** | **32 × 55** | **~337 920** |

At ~6 fps the bottleneck is almost entirely **192-VA coherent sum per RDM cell**. Profile/vital paths already use chirp fusion (Average) and NEON dot products; RDM PerChirp cannot fuse chirps without losing Doppler resolution.

**2026-06-28:** RDM beamform uses fused NEON multiply–accumulate (`beamformAtNeon`) — one pass over cube samples without a temporary VA buffer.

---

## 2. Why beamform RDM looks noisier than Single TX–RX

### 2.1 Display scaling (GUI)

Power view normalises with `scale = max(99th-percentile |IQ|, min_scale)` (`RDM_MAGNITUDE_FLOOR` default **2000**).

| Mode | Typical peak | Effective scale | Noise appearance |
|------|--------------|-----------------|------------------|
| Single TX–RX | ~400 | **2000** (floor) | Dark background; only strong bins visible |
| Beamform (az=0, el=0) | ~2500–3000 | ~2800 (peak) | Speckled floor; clutter visible |

The beamform sum is **not divided by N** (192); magnitudes are ~N× larger than `Average all VA`. Lower **Min scale** or switch to **Average all VA** for a fair clutter comparison at boresight.

### 2.2 Physical / signal processing

1. **Coherent integration of sidelobes** — summing 192 VAs raises the main target but also integrates energy from grating/side lobes of the sparse MIMO array across every range–Doppler cell.
2. **Boresight steering (az=el=0)** — phase weights are all ≈1; geometry errors do **not** affect on-axis steering. Off-axis targets are where `va_geometry.h` vs firmware matters.
3. **Firmware AoA model** — `radar_params.hpp` notes uniform λ-spacing FFT in firmware; iq-server beamform uses **measured** positions from `physical-antenna.txt`. Mismatch affects off-boresight steering, not axis-aligned weights.
4. **Cube is pre-calibrated** (`kRangeCubeIsCalibrated = true`) — EEPROM phase/amplitude already applied per VA; iq-server does not re-apply calibration coefs.
5. **PerChirp vs Average** — RDM PerChirp keeps all Doppler bins (55× cost vs zero-Doppler only). Clutter spreads in Doppler; beamform shows it in every bin.

### 2.3 Quick checks

| Test | Expected if geometry OK at boresight |
|------|--------------------------------------|
| Beamform az=0 el=0 vs **Average all VA** | Same phase alignment; beamform ≈ N × average magnitude |
| Single TX–RX vs beamform at target range | Beamform peak several× single (partial coherence, not 192×) |
| Rotate az ±5° | If geometry wrong, peak smears / noise rises faster than firmware AoA |

---

## 3. VA index ↔ geometry

```
va_idx = tile × 16 + sub_ant
tile 0..11  →  S{tile/3+1}-TX{tile%3+1}
sub 0..15   →  S{sub/4+1}-RX{sub%4+1}  (paired with tile TX)
```

Positions: `src/r4sn/iq-server/va_geometry.h` (λ/4 grid, S1-TX1 origin).  
Verify against `src/r4sn/docs/physical-antenna.txt` if off-axis beamforming degrades.

---

## Changelog

### 2026-06-28

- Document RDM beamform cost, noise mechanisms, and va_geometry verification steps.
- iq-server: NEON fused `beamformAtNeon` for RDM/profile hot path.
