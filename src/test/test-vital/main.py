#!/usr/bin/env python3
"""Poll iq-server and estimate breathing / heart rate using TI vital signs logic."""

from __future__ import annotations

import argparse
import signal
import sys
import time
from pathlib import Path

from iq_client import Target, request_once


def fetch_complex(host: str, port: int, target: Target, timeout: float) -> complex:
    status, parsed = request_once(host, port, [target], timeout)
    if status != 0:
        raise ConnectionError(f"iq-server status={status}")
    sample = parsed[0][0]
    return complex(sample.i, sample.q)
from vital_signs import VitalSignsProcessor

DEFAULT_RANGE_BIN_M = 0.075
DEFAULT_FRAME_RATE_HZ = 20.0  # periodicity=50 ms in mmwaveconfig
DEFAULT_CHIRP_MODE = 2  # MaxAbs — Average incoherently sums 64 TDM chirps


def build_range_targets(
    azimuth_rad: float,
    elevation_rad: float,
    center_distance_m: float,
    range_bin_m: float,
    half_width: int,
    chirp_mode: int = DEFAULT_CHIRP_MODE,
) -> list[tuple[int, Target]]:
    """Return (range_bin_index, Target) for center ± half_width bins."""
    center_bin = round(center_distance_m / range_bin_m)
    out: list[tuple[int, Target]] = []
    for offset in range(-half_width, half_width + 1):
        rb = center_bin + offset
        distance = rb * range_bin_m
        out.append((rb, Target(azimuth_rad, elevation_rad, distance, chirp_mode=chirp_mode)))
    return out


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="TI vital signs over iq-server")
    parser.add_argument("--host", default="192.168.0.33")
    parser.add_argument("--port", type=int, default=29171)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--azimuth", type=float, default=0.0, help="rad")
    parser.add_argument("--elevation", type=float, default=0.0, help="rad")
    parser.add_argument("--distance", type=float, default=1.5, help="center distance (m)")
    parser.add_argument(
        "--chirp-mode",
        type=int,
        default=DEFAULT_CHIRP_MODE,
        choices=[0, 1, 2],
        help="0=PerChirp, 1=Average, 2=MaxAbs (recommended)",
    )
    parser.add_argument("--range-bin-m", type=float, default=DEFAULT_RANGE_BIN_M)
    parser.add_argument("--range-half-width", type=int, default=2, help="±bins (5 total at 2)")
    parser.add_argument("--frame-rate", type=float, default=DEFAULT_FRAME_RATE_HZ)
    parser.add_argument("--refresh-rate", type=int, default=32)
    parser.add_argument("--buffer-frames", type=int, default=128)
    parser.add_argument("--interval", type=float, default=0.0, help="poll interval (0=as fast as possible)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    running = True

    def stop(_signum, _frame) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    rb_targets = build_range_targets(
        args.azimuth,
        args.elevation,
        args.distance,
        args.range_bin_m,
        args.range_half_width,
        args.chirp_mode,
    )
    targets = [t for _, t in rb_targets]
    processor = VitalSignsProcessor(
        frame_rate_hz=args.frame_rate,
        refresh_rate=args.refresh_rate,
        buffer_frames=args.buffer_frames,
    )

    print(
        f"vital-signs -> {args.host}:{args.port} "
        f"center={args.distance:.2f}m bins={[rb for rb, _ in rb_targets]} "
        f"frame_rate={args.frame_rate}Hz",
        flush=True,
    )

    frame = 0
    t0 = time.monotonic()
    try:
        while running:
            frame += 1
            status, parsed = request_once(args.host, args.port, targets, args.timeout)
            if status != 0:
                print(f"frame={frame} iq-server status={status}", flush=True)
                time.sleep(0.1)
                continue

            for (rb, _target), samples in zip(rb_targets, parsed):
                sample = samples[0]
                processor.add_sample(rb, complex(sample.i, sample.q))

            result = processor.process(indicate_no_target=False)
            elapsed = time.monotonic() - t0
            if result is None:
                filled = processor.min_buffer_fill
                print(
                    f"frame={frame:5d} t={elapsed:7.2f}s buffering {filled}/{args.buffer_frames}",
                    flush=True,
                )
            else:
                print(
                    f"frame={frame:5d} t={elapsed:7.2f}s "
                    f"breath={result.breathing_rate_bpm:5.1f} bpm "
                    f"heart={result.heart_rate_bpm:5.1f} bpm "
                    f"breath_dev={result.breathing_deviation:.5f}",
                    flush=True,
                )

            if args.interval > 0:
                time.sleep(args.interval)
    except (TimeoutError, ConnectionError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"stopped after {frame} frame(s)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
