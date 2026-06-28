#!/usr/bin/env python3
"""IQ server console client — poll targets and print received beamformed IQ data."""

from __future__ import annotations

import argparse
import csv
import signal
import sys
import time
from pathlib import Path
from typing import Iterable, TextIO

from iq_client import (
    CHIRP_MODES,
    DEFAULT_HOST,
    DEFAULT_PORT,
    DEFAULT_TIMEOUT_S,
    IqResponse,
    IqSample,
    Target,
    parse_target_line,
    request_once,
)

LOG_FIELDS = [
    "frame",
    "elapsed_s",
    "wall_time",
    "target_idx",
    "chirp_idx",
    "azimuth_rad",
    "elevation_rad",
    "distance_m",
    "chirp_mode",
    "i",
    "q",
    "magnitude",
    "phase_deg",
]


def fmt_signed(value: float, width: int, precision: int) -> str:
    return f"{value:+{width}.{precision}f}"


def fmt_unsigned(value: float, width: int, precision: int) -> str:
    return f"{value:{width}.{precision}f}"


def parse_target_arg(text: str) -> Target:
    try:
        return parse_target_line(text, "target")
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def load_targets_file(path: Path) -> list[Target]:
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"target file not found: {path}")

    targets: list[Target] = []
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            targets.append(parse_target_line(line, f"{path}:{line_no}"))
        except ValueError as exc:
            raise argparse.ArgumentTypeError(str(exc)) from exc

    if not targets:
        raise argparse.ArgumentTypeError(f"target file is empty: {path}")
    return targets


def resolve_target_arg(text: str) -> list[Target]:
    path = Path(text)
    if path.is_file():
        return load_targets_file(path)
    return [parse_target_arg(text)]


class CsvLogger:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._path = path
        self._file: TextIO = path.open("w", encoding="utf-8", newline="")
        self._writer = csv.DictWriter(self._file, fieldnames=LOG_FIELDS)
        self._writer.writeheader()
        self._file.flush()

    @property
    def path(self) -> Path:
        return self._path

    def write_frame(
        self,
        frame: int,
        elapsed_s: float,
        wall_time: str,
        targets: list[Target],
        response: IqResponse,
    ) -> None:
        if response.status != 0:
            return

        for target_idx, (spec, samples) in enumerate(zip(targets, response.targets)):
            for chirp_idx, sample in enumerate(samples):
                self._writer.writerow(
                    {
                        "frame": frame,
                        "elapsed_s": f"{elapsed_s:.6f}",
                        "wall_time": wall_time,
                        "target_idx": target_idx,
                        "chirp_idx": chirp_idx,
                        "azimuth_rad": f"{spec.azimuth_rad:.8f}",
                        "elevation_rad": f"{spec.elevation_rad:.8f}",
                        "distance_m": f"{spec.distance_m:.8f}",
                        "chirp_mode": spec.chirp_mode,
                        "i": f"{sample.i:.8f}",
                        "q": f"{sample.q:.8f}",
                        "magnitude": f"{sample.magnitude:.8f}",
                        "phase_deg": f"{sample.phase_deg:.8f}",
                    }
                )
        self._file.flush()

    def close(self) -> None:
        self._file.close()


def format_target_spec(index: int, target: Target) -> str:
    return (
        f"T{index:d}["
        f"az={fmt_signed(target.azimuth_rad, 7, 3)} "
        f"el={fmt_signed(target.elevation_rad, 7, 3)} "
        f"R={fmt_unsigned(target.distance_m, 6, 2)}m "
        f"mode={target.mode_name:8s}]"
    )


def format_iq_values(sample: IqSample) -> str:
    return (
        f"I={fmt_signed(sample.i, 10, 2)} "
        f"Q={fmt_signed(sample.q, 10, 2)} "
        f"|IQ|={fmt_unsigned(sample.magnitude, 10, 2)} "
        f"phase={fmt_signed(sample.phase_deg, 7, 2)}deg"
    )


def format_chirp_line(chirp_idx: int, sample: IqSample) -> str:
    return f"c{chirp_idx:02d}: {format_iq_values(sample)}"


def print_response(
    frame: int,
    elapsed_s: float,
    targets: list[Target],
    response: IqResponse,
    compact: bool,
) -> None:
    ts = time.strftime("%H:%M:%S")
    print(
        f"[{ts}] "
        f"frame={frame:5d} "
        f"t={fmt_signed(elapsed_s, 8, 3)}s "
        f"status={response.status:d}"
    )

    if response.status != 0:
        print(f"  error status={response.status:d}")
        return

    for idx, (spec, samples) in enumerate(zip(targets, response.targets)):
        print(f"  {format_target_spec(idx, spec)}")
        if spec.chirp_mode == 0:
            mags = [s.magnitude for s in samples]
            peak = max(mags)
            mean_mag = sum(mags) / len(mags)
            print(
                "    "
                f"chirps=64 "
                f"peak|IQ|={fmt_unsigned(peak, 10, 2)} "
                f"mean|IQ|={fmt_unsigned(mean_mag, 10, 2)}"
            )
            if compact:
                show = samples[:3] + samples[-1:]
                indices = list(range(3)) + [63]
                for chirp_idx, sample in zip(indices, show):
                    print(f"    {format_chirp_line(chirp_idx, sample)}")
            else:
                for chirp_idx, sample in enumerate(samples):
                    print(f"    {format_chirp_line(chirp_idx, sample)}")
        else:
            print(f"    IQ: {format_iq_values(samples[0])}")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Poll iq-server and print beamformed IQ data to the console.",
    )
    parser.add_argument("--host", "-H", default=DEFAULT_HOST, help="IQ server host")
    parser.add_argument("--port", "-p", type=int, default=DEFAULT_PORT, help="IQ server port")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S, help="TCP timeout (seconds)")
    parser.add_argument(
        "--interval",
        "-i",
        type=float,
        default=0.05,
        help="Delay between polls in seconds (0 = as fast as possible)",
    )
    parser.add_argument(
        "--count",
        "-n",
        type=int,
        default=0,
        help="Number of polls (0 = run until Ctrl+C)",
    )
    parser.add_argument(
        "--target",
        "-t",
        action="append",
        metavar="SPEC_OR_FILE",
        help=(
            "Target az,el,dist[,mode] or path to a target list file "
            "(one target per line). Repeat to combine specs and files."
        ),
    )
    parser.add_argument("--azimuth", type=float, default=0.0, help="Default azimuth (rad)")
    parser.add_argument("--elevation", type=float, default=0.0, help="Default elevation (rad)")
    parser.add_argument("--distance", type=float, default=2.5, help="Default distance (m)")
    parser.add_argument(
        "--mode",
        "-m",
        type=int,
        default=1,
        choices=sorted(CHIRP_MODES),
        help="Default chirp mode: 0=PerChirp, 1=Average, 2=MaxAbs",
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="For PerChirp, print first 3 and last chirp only",
    )
    parser.add_argument(
        "--log",
        "--log-file",
        dest="log_file",
        metavar="PATH",
        help="Append captured IQ samples to CSV (one row per chirp sample)",
    )
    return parser


def resolve_targets(args: argparse.Namespace) -> list[Target]:
    if args.target:
        targets: list[Target] = []
        for text in args.target:
            targets.extend(resolve_target_arg(text))
        return targets
    return [
        Target(
            azimuth_rad=args.azimuth,
            elevation_rad=args.elevation,
            distance_m=args.distance,
            chirp_mode=args.mode,
        )
    ]


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    targets = resolve_targets(args)

    running = True

    def stop(_signum, _frame) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    logger: CsvLogger | None = None
    if args.log_file:
        logger = CsvLogger(Path(args.log_file))

    print(
        f"iq-client -> {args.host}:{args.port} "
        f"targets={len(targets)} interval={args.interval}s",
        flush=True,
    )
    for idx, target in enumerate(targets):
        print(f"  {format_target_spec(idx, target)}", flush=True)
    if logger is not None:
        print(f"  logging -> {logger.path}", flush=True)
    print("Press Ctrl+C to stop.", flush=True)

    frame = 0
    t0 = time.monotonic()
    try:
        while running:
            frame += 1
            started = time.monotonic()
            elapsed_s = started - t0
            wall_time = time.strftime("%Y-%m-%d %H:%M:%S")
            response = request_once(args.host, args.port, targets, args.timeout)
            print_response(frame, elapsed_s, targets, response, args.compact)
            if logger is not None:
                logger.write_frame(frame, elapsed_s, wall_time, targets, response)

            if args.count > 0 and frame >= args.count:
                break

            if args.interval > 0:
                elapsed = time.monotonic() - started
                sleep_for = args.interval - elapsed
                if sleep_for > 0:
                    time.sleep(sleep_for)
    except KeyboardInterrupt:
        pass
    except (TimeoutError, ConnectionError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        if logger is not None:
            logger.close()

    print(f"stopped after {frame} frame(s)", flush=True)
    if logger is not None:
        print(f"log saved: {logger.path}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
