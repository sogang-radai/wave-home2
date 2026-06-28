#!/usr/bin/env python3
"""Verify PerChirp IQ phase alignment and recommend chirp mode for vital signs."""

from __future__ import annotations

import argparse
import math
import sys

import numpy as np

from iq_client import Target, request_once


def circular_coherence(samples: list[complex]) -> tuple[float, float]:
    """Return (weighted_R, top10pct_R) in [0, 1]. 1 = all phases identical."""
    z = np.asarray(samples, dtype=np.complex128)
    mags = np.abs(z)
    if mags.sum() == 0:
        return 0.0, 0.0

    w = mags / mags.sum()
    phases = np.angle(z)
    r_weighted = float(np.abs(np.sum(w * np.exp(1j * phases))))

    peak = mags.max()
    mask = mags >= 0.1 * peak
    r_top = float(np.abs(np.mean(np.exp(1j * phases[mask])))) if mask.sum() >= 2 else float("nan")
    return r_weighted, r_top


def frame_diff_std_deg(phases_deg: list[float]) -> float:
    p = np.unwrap(np.radians(phases_deg))
    if len(p) < 2:
        return float("nan")
    return float(np.degrees(np.std(np.diff(p))))


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify 64-chirp phase coherence from iq-server")
    parser.add_argument("--host", default="192.168.0.33")
    parser.add_argument("--port", type=int, default=29171)
    parser.add_argument("--distance", type=float, default=1.0)
    parser.add_argument("--frames", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    per_chirp = Target(0.0, 0.0, args.distance, chirp_mode=0)
    average = Target(0.0, 0.0, args.distance, chirp_mode=1)
    max_abs = Target(0.0, 0.0, args.distance, chirp_mode=2)

    print(f"target: az=0 el=0 R={args.distance:.3f}m  host={args.host}:{args.port}")
    print("\n[1] PerChirp (64 chirps, single frame) — in-frame phase alignment")
    print("    Expect R << 1 for TDM-MIMO (chirps are not meant to be phase-aligned).\n")

    for frame in range(1, args.frames + 1):
        status, parsed = request_once(args.host, args.port, [per_chirp], args.timeout)
        if status != 0:
            print(f"frame {frame}: iq-server status={status}")
            return 1
        z = [complex(s.i, s.q) for s in parsed[0]]
        mags = np.abs(z)
        rw, rt = circular_coherence(z)
        peak = int(np.argmax(mags))
        print(
            f"  frame {frame}: R={rw:.3f}  R(top10%)={rt:.3f}  "
            f"peak=c{peak:02d} |IQ|={mags[peak]:.0f}  mean|IQ|={mags.mean():.0f}"
        )

    print("\n[2] Slow-time stability (40 frames) — relevant for vital signs")
    modes = [
        ("Average (mode=1)", average),
        ("MaxAbs (mode=2)", max_abs),
        ("PerChirp c00 (mode=0, chirp 0 only)", per_chirp),
    ]

    for label, target in modes:
        phases: list[float] = []
        mags: list[float] = []
        for _ in range(40):
            status, parsed = request_once(args.host, args.port, [target], args.timeout)
            if status != 0:
                print(f"  {label}: status={status}")
                break
            if target.chirp_mode == 0:
                sample = parsed[0][0]
            else:
                sample = parsed[0][0]
            z = complex(sample.i, sample.q)
            mags.append(abs(z))
            phases.append(math.degrees(math.atan2(sample.q, sample.i)))
        else:
            dstd = frame_diff_std_deg(phases)
            print(f"  {label:36s} mean|IQ|={np.mean(mags):8.1f}  dphase_std={dstd:6.2f} deg")

    print(
        "\nConclusion:"
        "\n  - 64 chirps in one frame: phases do NOT align (low R is normal for 4ST TDM)."
        "\n  - Do NOT use Average for vital signs (incoherent sum across chirps)."
        "\n  - Prefer MaxAbs (mode=2) or a fixed chirp index for frame-rate phase tracking."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
