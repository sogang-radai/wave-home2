# EEPROM calibration applicator — `FUN_0041d598` (r4fn)

How firmware turns EEPROM **amplitude / delay / phase** tables into per-range-bin complex correction factors for the SPT accelerator. Complements [CALIBRATION_DATA.md](CALIBRATION_DATA.md) (file layout, measured values, indexing).

**Disassembly:** `src/r4sn/disassembly/r4fn.elf.c`  
**iq-server status:** cube samples are **already calibrated** by SPT (`kRangeCubeIsCalibrated = true` in `radar_params.hpp`). `calibration_coefs.h` exists for future offline re-steering; **not wired into iq-server yet**.

---

## 1. Boot-time load order

Entry: init path around `FUN_00414810` (SPT kernel data setup).

```
1. Open EEPROM (FUN_0044f2d4)
2. FUN_0041dbb0 — verify EEPROM parts 2–4 exist and Valid=1
3. If valid:
     FUN_0041dc08(eeprom_handle)
       → sets globals, calls FUN_0041d598(eeprom_handle)
4. Else if /user/config/cfg_profile/r4fn_cal_coeff.dat (or /user/config/) exists:
     load pre-baked int16 table into DAT_004a7af0 (calibrationCoeffsBufH host mirror)
5. Upload buffer to SPT: calibrationCoeffsBufH (see r4fn_SPTInitKernelData logs)
```

`FUN_0041dc08` constants (when building from EEPROM):

| Global | Value | Role |
|--------|-------|------|
| `DAT_00499a40` | `0x400000001` | Outer passes = **4** (phase contexts 0..3) |
| `DAT_00499a50` | `0x100` | Range axis length = **256** bins (`fIdx` 0..255) |
| `DAT_00499a54` | `DAT_00499a20 × DAT_00499a30` | Channels per pass = **48** (`12 tiles × 4 RX`) |
| `DAT_00499a4c` | `0xb` | int16 scale = **1 << 11 = 2048** |

Init calibration (EEPROM part 1, 2736 B) is loaded separately via `FUN_0044bba8(..., part=1, ...)` into mmWaveLink init structures — **not** passed through this applicator.

---

## 2. EEPROM → RAM (`FUN_0041d598` load phase)

`FUN_0044bba8(handle, part_id, offset, buf, size)` reads EEPROM parts 2–4 into stack buffers:

| Part id | EEPROM | Buffer | Size | Fallback if read fails |
|---------|--------|--------|------|------------------------|
| 2 | Amplitude | `local_300` | 768 B (192×float) | Copy default table `DAT_0045ec80`, or fill **1.0** |
| 4 | Phase | `local_600` | 768 B | Zeros (no phase offset) |
| 3 | Delay | `local_900` | 768 B | Zeros; optional unit rescale (§4) |

Load attempts (simplified):

- Try amplitude (2) → phase (4) → delay (3); on partial failure, zero missing tables and continue where possible.
- Close EEPROM handle (`FUN_0044f408`) after reads.

---

## 3. Applicator core — build SPT coefficient LUT

Triple nested loop (`FUN_0041d598` ~29705–29735):

```text
for pass in 0 .. 3:                    // phase context = sub_ant // 4
  for row in 0 .. 47:                  // row = tile×4 + (sub_ant % 4)
    amp   = local_300[pass×48 + row]
    delay = local_900[pass×48 + row]
    phase = local_600[pass×48 + row]
    out   = &calibrationCoeffsBufH[pass×48 + row]   // stride 0x400 bytes per row

    for fIdx in 0 .. 255:              // range calibration axis
      θ = fIdx × delay + phase        // radians (sincos argument)
      I = round( cos(θ) × amp × 2048 )
      Q = round( sin(θ) × amp × 2048 )
      out[fIdx] = (Q, I)               // int16 pair; Q @+0, I @+2 in cube layout
```

### 3.1 Index mapping (EEPROM float ↔ cube VA)

Same as [CALIBRATION_DATA.md §4](CALIBRATION_DATA.md#4-firmware-mapping-r4fn):

```text
va_index  = tile × 16 + sub_ant          // sub_ant ∈ [0,15]
rx        = sub_ant % 4
context   = sub_ant // 4                  // 0..3  == outer pass index
row       = tile × 4 + rx                // 0..47
coef_index = row + 48 × context
         = row × 4 + context             // row-major 48×4 layout in .bin
```

Each EEPROM `.bin` is **192 float32 LE** in this order (identical for ampli, delay, phase).

### 3.2 Physical meaning

For channel `i` at range bin `r`:

$$\theta_i(r) = delay_i \cdot r + \phi_i$$

$$C_i(r) = amp_i \cdot e^{j\theta_i(r)}$$

SPT applies $C_i(r)$ to the range-FFT sample for that TX–RX virtual path at bin `r`. This is **range-dependent phasor correction** (FMCW group-delay slope + fixed phase + per-channel gain).

**Units (this unit, serial 172594600021):**

| Coef | EEPROM units | In applicator |
|------|----------------|---------------|
| `amp_i` | linear gain | multiplies cos/sin before int16 quantisation |
| `phase_i` | radians | added to `sincos` argument |
| `delay_i` | slope × `fIdx` | `fIdx` is integer 0..255 along cal axis (not metres); see §4 for rescale |

---

## 4. Delay unit rescale (edge case)

If **phase** EEPROM load fails but **delay** loads, firmware checks `local_900[1]` (second delay coefficient):

- If `delay[1] < -1.0` **or** `delay[1] > 1.0`, all delay entries are scaled:

```text
delay[i] ← delay[i] × (float)DAT_00499a48 / (float)DAT_00499a4a / 1000.0
```

This maps factory EEPROM delay units to the slope expected in `sincos(fIdx × delay + phase)`. On a healthy export (`delay[1]` inside ±1), this branch is skipped.

---

## 5. Output buffer layout

Host mirror: `DAT_004a7af0` → SPT `calibrationCoeffsBufH`.

| Dimension | Count | Notes |
|-----------|-------|-------|
| Pass (context) | 4 | Matches `sub_ant // 4` |
| Row (tile×RX) | 48 | 12×4 |
| Range `fIdx` | 256 | One int16 Q/I pair per bin |
| **Total rows** | 192 | 4×48; matches VA coef count |
| **Bytes per row** | `0x400` | 256 bins × 4 B (Q+I int16) |

Logged cube (`rangeOutputBufH` / `r4sn_cube_log`) is produced **after** SPT range processing with this LUT already applied. iq-server `cube_mmap` therefore sees **calibrated** int16 IQ.

---

## 6. Fallback: `r4fn_cal_coeff.dat`

If EEPROM parts are invalid, firmware loads a precomputed **int16** table from:

1. `/user/config/cfg_profile/r4fn_cal_coeff.dat`
2. `/user/config/r4fn_cal_coeff.dat`

Same Q/I layout as §5 — not raw float EEPROM; already the output of this applicator (or factory equivalent).

---

## 7. Implications for iq-server beamforming

| Topic | Note |
|-------|------|
| Cube mmap | Samples include EEPROM cal; **do not** multiply by `calibration_coefs.h` again. |
| Re-steering / simulation | Start from **raw** range FFT or explicitly **inverse** $C_i(r)$ before custom beamforming. |
| `va_geometry.h` | Antenna positions for steering; orthogonal to EEPROM amp/phase/delay. |
| AoA in firmware | Uniform λ model (`kAntSpacingLambda`); EEPROM cal is per-VA range correction, not the AoA FFT grid. |

### Future iq-server integration (not implemented)

For each `(tile, sub_ant, range_bin)` when combining VAs on **uncalibrated** data:

```python
i = row * 4 + context
θ = delay[i] * range_bin + phase[i]
C = ampli[i] * (cos(θ) + 1j*sin(θ))
S_cal = S_raw * C
```

With calibrated cube: use `S_cal` directly from mmap.

---

## 8. Symbol cross-reference

| Symbol | Address / name | Role |
|--------|----------------|------|
| `FUN_0041d598` | — | Load EEPROM + build LUT |
| `FUN_0041dc08` | — | Set dims; call applicator |
| `FUN_0044bba8` | — | EEPROM part read |
| `FUN_0041dbb0` | — | EEPROM parts 2–4 valid check |
| `DAT_004a7af0` | `calibrationCoeffsBufH` | Host LUT |
| `DAT_0045ec80` | — | Default amplitude table |

---

*Derived from `r4fn.elf.c` decompilation and [CALIBRATION_DATA.md](CALIBRATION_DATA.md), June 2026.*
