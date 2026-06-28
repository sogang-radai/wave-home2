#!/usr/bin/env python3
"""Replay test-iq CSV logs through VitalSignsProcessor (offline validation)."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

from vital_signs import VitalSignsProcessor, _extract_phase_series, spectrum_scale

# Firmware rangeRes (r4fn.elf.c FUN_0040d6f0) — matches iq-server radar_params.hpp
RANGE_BIN_M = 0.07156503945589066


def distance_to_range_bin(distance_m: float) -> int:
    return int(round(distance_m / RANGE_BIN_M))


def load_frames(path: Path) -> list[list[dict[str, str]]]:
    by_frame: dict[int, list[dict[str, str]]] = defaultdict(list)
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            by_frame[int(row["frame"])].append(row)
    return [by_frame[i] for i in sorted(by_frame)]


def estimate_frame_rate(frames: list[list[dict[str, str]]]) -> float:
    if len(frames) < 2:
        return 20.0
    t0 = float(frames[0][0]["elapsed_s"])
    t1 = float(frames[-1][0]["elapsed_s"])
    dt = t1 - t0
    if dt <= 0:
        return 20.0
    return (len(frames) - 1) / dt


def analyze_signal_quality(frames: list[list[dict[str, str]]]) -> None:
    """Per range-bin magnitude and slow-time phase stability."""
    series: dict[int, list[complex]] = defaultdict(list)
    for frame_rows in frames:
        for row in frame_rows:
            if int(row["chirp_idx"]) != 0:
                continue
            rb = distance_to_range_bin(float(row["distance_m"]))
            series[rb].append(complex(float(row["i"]), float(row["q"])))

    print("\n=== Signal quality (slow-time) ===")
    print(f"{'bin':>4} {'dist_m':>7} {'mean|IQ|':>10} {'dphase_std':>12} {'coherence_R':>12}")
    best_rb = None
    best_mag = 0.0
    for rb in sorted(series):
        z = np.asarray(series[rb], dtype=np.complex128)
        mags = np.abs(z)
        mean_mag = float(mags.mean())
        phases = np.unwrap(np.angle(z))
        dstd = float(np.degrees(np.std(np.diff(phases)))) if len(phases) >= 2 else float("nan")
        w = mags / mags.sum() if mags.sum() else np.ones_like(mags) / len(mags)
        r_coh = float(np.abs(np.sum(w * np.exp(1j * np.angle(z)))))
        dist = rb * RANGE_BIN_M
        print(f"{rb:4d} {dist:7.3f} {mean_mag:10.1f} {dstd:11.2f}° {r_coh:12.3f}")
        if mean_mag > best_mag:
            best_mag = mean_mag
            best_rb = rb
    if best_rb is not None:
        print(f"\nStrongest range bin: {best_rb} ({best_rb * RANGE_BIN_M:.3f} m), mean|IQ|={best_mag:.1f}")


def plot_spectrum_hint(frames: list[list[dict[str, str]]], range_bin: int, frame_rate_hz: float) -> None:
    """Print dominant breath/heart bins from center range phase series."""
    samples: list[complex] = []
    for frame_rows in frames:
        for row in frame_rows:
            if distance_to_range_bin(float(row["distance_m"])) != range_bin:
                continue
            if int(row["chirp_idx"]) != 0:
                continue
            samples.append(complex(float(row["i"]), float(row["q"])))
            break

    if len(samples) < 128:
        print(f"\nNot enough samples for spectrum on bin {range_bin}")
        return

    z = np.array(samples[:128], dtype=np.complex128)
    disp = _extract_phase_series(z)
    padded = np.zeros(512, dtype=np.complex128)
    padded[: len(disp)] = disp
    spec = np.abs(np.fft.fft(padded)) ** 2
    scale = spectrum_scale(frame_rate_hz)

    breath_peak = int(np.argmax(spec[3:51]) + 3)
    heart_dec = np.zeros(256)
    for i in range(128):
        heart_dec[i] = spec[2 * i] * spec[i]
    heart_peak = int(np.argmax(heart_dec[68:129]) + 68)

    print(f"\n=== Phase spectrum (first 128 frames, bin {range_bin}) ===")
    print(f"  frame_rate ≈ {frame_rate_hz:.2f} Hz")
    print(f"  breath peak bin {breath_peak} → {breath_peak * scale:.1f} bpm")
    print(f"  heart  peak bin {heart_peak} → {heart_peak * scale:.1f} bpm")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Replay test-iq CSV through vital signs processor")
    p.add_argument("csv", type=Path, help="log CSV from test-iq")
    p.add_argument("--frame-rate", type=float, default=0.0, help="Hz (0=auto from elapsed_s)")
    p.add_argument("--refresh-rate", type=int, default=32)
    p.add_argument("--buffer-frames", type=int, default=128)
    p.add_argument("--no-quality", action="store_true", help="skip signal quality table")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if not args.csv.is_file():
        print(f"file not found: {args.csv}", file=sys.stderr)
        return 1

    frames = load_frames(args.csv)
    frame_rate = args.frame_rate or estimate_frame_rate(frames)
    elapsed = float(frames[-1][0]["elapsed_s"]) - float(frames[0][0]["elapsed_s"])

    print(f"CSV: {args.csv}")
    print(f"  frames={len(frames)}  duration={elapsed:.2f}s  frame_rate≈{frame_rate:.2f}Hz")
    chirp_modes = {int(r["chirp_mode"]) for fr in frames for r in fr}
    if 1 in chirp_modes:
        print("  NOTE: chirp_mode=1 (Average) — incoherent across 64 TDM chirps; MaxAbs(2) preferred")

    if not args.no_quality:
        analyze_signal_quality(frames)

    processor = VitalSignsProcessor(
        frame_rate_hz=frame_rate,
        refresh_rate=args.refresh_rate,
        buffer_frames=args.buffer_frames,
    )

    results: list[tuple[int, float, float, float, float]] = []

    for frame_rows in frames:
        frame_no = int(frame_rows[0]["frame"])
        elapsed_s = float(frame_rows[0]["elapsed_s"])
        for row in frame_rows:
            if int(row["chirp_idx"]) != 0:
                continue
            rb = distance_to_range_bin(float(row["distance_m"]))
            sample = complex(float(row["i"]), float(row["q"]))
            processor.add_sample(rb, sample)

        result = processor.process(indicate_no_target=False)
        if result is None:
            continue
        results.append(
            (
                frame_no,
                elapsed_s,
                result.breathing_rate_bpm,
                result.heart_rate_bpm,
                result.breathing_deviation,
            )
        )
        print(
            f"frame={frame_no:5d} t={elapsed_s:7.2f}s "
            f"breath={result.breathing_rate_bpm:5.1f} bpm "
            f"heart={result.heart_rate_bpm:5.1f} bpm "
            f"breath_dev={result.breathing_deviation:.5f} "
            f"center_bin={result.range_bin}",
            flush=True,
        )

    if not results:
        print("\nNo vital-signs outputs (need ≥128 frames per range bin).")
        return 1

    # Summary after warmup (vs_loop > 7)
    warm = [r for r in results if r[0] >= 128 + args.refresh_rate * 7]
    if warm:
        breath = [r[2] for r in warm if r[2] > 0]
        heart = [r[3] for r in warm if r[3] > 0]
        print("\n=== Summary (after algorithm warmup) ===")
        if breath:
            print(
                f"  breathing: median={np.median(breath):.1f} bpm  "
                f"mean={np.mean(breath):.1f}  std={np.std(breath):.1f}  n={len(breath)}"
            )
        if heart:
            print(
                f"  heart:     median={np.median(heart):.1f} bpm  "
                f"mean={np.mean(heart):.1f}  std={np.std(heart):.1f}  n={len(heart)}"
            )

    center_bin = results[-1][0]  # wrong - use processor center
    bins = sorted({distance_to_range_bin(float(r["distance_m"])) for fr in frames for r in fr})
    center = bins[len(bins) // 2]
    plot_spectrum_hint(frames, center, frame_rate)

    return 0


if __name__ == "__main__":
    sys.exit(main())
