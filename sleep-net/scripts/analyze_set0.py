"""
Dataset statistics for sleep-net/dataset/set0 (labels 0..4).

Computes, per label, distributions of:
  - point count per frame (+ empty-frame ratio)
  - x / y / z position
  - z height (posture proxy for a ceiling-down radar)
  - per-frame spatial spread (std of points within a frame)
  - per-frame horizontal footprint (x/y extent = max-min)
  - doppler (+ |doppler| = motion/activity proxy)
  - power in dB (see POWER_DB formula below)
  - session count / frame count / duration

The goal is to inform bed-net / toss-net stride and augmentation parameters,
and to check whether "lying (asleep, 2/3/4)" vs "present/awake (1)" are
separable by posture (z, footprint) rather than motion alone.
"""

import math
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from pcr_loader import PCRFile  # noqa: E402

# ============================================================================
# Constants
# ============================================================================

DATASET_DIR = SCRIPT_DIR / "dataset" / "set0"
LABELS = [0, 1, 2, 3, 4]

# Power -> dB. Matches the existing preprocess convention power_to_db().
# NOTE: user wrote "db = log10(x+1)", but a raw log10 cannot yield 25..60.
# The x10 form (10*log10(x+1)) reproduces the stated 25..60 range and the
# existing POWER_DB_MIN/MAX = 30/55 constants, so we use it here.
POWER_DB_SCALE = 10.0

# |doppler| thresholds (m/s-ish, radar units) to report "moving frame" ratios.
MOVING_THRESHOLDS = [0.05, 0.1, 0.2, 0.4]

PERCENTILES = [0, 1, 5, 25, 50, 75, 95, 99, 100]

_VARIANT_PATTERN = re.compile(r"^(\d+)-(\d+)$")


# ============================================================================
# Helpers
# ============================================================================

def log(message: str = "") -> None:
    print(message, flush=True)


def power_to_db(power: np.ndarray) -> np.ndarray:
    return POWER_DB_SCALE * np.log10(power + 1.0)


def find_dll() -> Path:
    root = SCRIPT_DIR.parent  # wave-home2
    candidates = [
        root / "thirdparty" / "lzav" / "bin" / "x64" / "lzav_lib.dll",
        root / "thirdparty" / "lzav" / "bin" / "x64" / "lzav_lib.so",
        root / "thirdparty" / "lzav" / "bin" / "arm64" / "lzav_lib.so",
        root / "bin" / "x64" / "lzav_lib.dll",
        root / "bin" / "x64" / "lzav_lib.so",
        SCRIPT_DIR / "bin" / "x64" / "lzav_lib.dll",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "lzav_lib not found. Checked:\n  " + "\n  ".join(str(c) for c in candidates)
    )


def discover_pcr_files(input_dir: Path, label: int) -> List[Path]:
    variant_paths: List[Tuple[int, Path]] = []
    for path in input_dir.glob("*.pcr"):
        match = _VARIANT_PATTERN.match(path.stem)
        if match and int(match.group(1)) == label:
            variant_paths.append((int(match.group(2)), path))

    if variant_paths:
        variant_paths.sort(key=lambda item: item[0])
        return [path for _, path in variant_paths]

    single_path = input_dir / f"{label}.pcr"
    if single_path.exists():
        return [single_path]
    return []


def describe(values: np.ndarray, unit: str = "") -> str:
    if values.size == 0:
        return "        (no data)"
    pcts = np.percentile(values, PERCENTILES)
    suffix = f" {unit}" if unit else ""
    header = "        p" + "  p".join(f"{p:>3}" for p in PERCENTILES)
    line = "        " + "  ".join(f"{v:6.3f}" for v in pcts)
    return (
        f"        mean={values.mean():.3f}{suffix}  std={values.std():.3f}  "
        f"min={values.min():.3f}  max={values.max():.3f}\n"
        f"{header}\n{line}"
    )


# ============================================================================
# Per-label processing
# ============================================================================

class LabelAccumulator:
    def __init__(self) -> None:
        self.file_count = 0
        self.session_count = 0
        self.frame_count = 0
        self.empty_frame_count = 0
        self.total_us = 0

        # per-point arrays (concatenated at the end)
        self._x: List[np.ndarray] = []
        self._y: List[np.ndarray] = []
        self._z: List[np.ndarray] = []
        self._doppler: List[np.ndarray] = []
        self._power_db: List[np.ndarray] = []

        # per-frame scalar metrics
        self.point_counts: List[int] = []
        self.spread_x: List[float] = []
        self.spread_y: List[float] = []
        self.spread_z: List[float] = []
        self.extent_x: List[float] = []
        self.extent_y: List[float] = []
        self.extent_z: List[float] = []
        self.centroid_z: List[float] = []
        self.abs_doppler_mean: List[float] = []

    def add_frame(self, points: np.ndarray) -> None:
        # points: (N, 5) -> x, y, z, doppler, power
        self.frame_count += 1
        n = points.shape[0]
        self.point_counts.append(n)
        if n == 0:
            self.empty_frame_count += 1
            return

        x, y, z, doppler, power = (points[:, i] for i in range(5))
        power_db = power_to_db(power)

        self._x.append(x)
        self._y.append(y)
        self._z.append(z)
        self._doppler.append(doppler)
        self._power_db.append(power_db)

        self.spread_x.append(float(x.std()))
        self.spread_y.append(float(y.std()))
        self.spread_z.append(float(z.std()))
        self.extent_x.append(float(x.max() - x.min()))
        self.extent_y.append(float(y.max() - y.min()))
        self.extent_z.append(float(z.max() - z.min()))
        self.centroid_z.append(float(z.mean()))
        self.abs_doppler_mean.append(float(np.abs(doppler).mean()))

    def finalize(self) -> Dict[str, np.ndarray]:
        def cat(chunks: List[np.ndarray]) -> np.ndarray:
            return np.concatenate(chunks) if chunks else np.empty(0, dtype=np.float32)

        return {
            "x": cat(self._x),
            "y": cat(self._y),
            "z": cat(self._z),
            "doppler": cat(self._doppler),
            "power_db": cat(self._power_db),
            "point_counts": np.asarray(self.point_counts, dtype=np.float32),
            "spread_x": np.asarray(self.spread_x, dtype=np.float32),
            "spread_y": np.asarray(self.spread_y, dtype=np.float32),
            "spread_z": np.asarray(self.spread_z, dtype=np.float32),
            "extent_x": np.asarray(self.extent_x, dtype=np.float32),
            "extent_y": np.asarray(self.extent_y, dtype=np.float32),
            "extent_z": np.asarray(self.extent_z, dtype=np.float32),
            "centroid_z": np.asarray(self.centroid_z, dtype=np.float32),
            "abs_doppler_mean": np.asarray(self.abs_doppler_mean, dtype=np.float32),
        }


def frame_to_array(points) -> np.ndarray:
    if not points:
        return np.zeros((0, 5), dtype=np.float32)
    buffer = np.empty((len(points), 5), dtype=np.float32)
    for i, p in enumerate(points):
        buffer[i, 0] = p.x
        buffer[i, 1] = p.y
        buffer[i, 2] = p.z
        buffer[i, 3] = p.doppler
        buffer[i, 4] = p.power
    return buffer


def process_label(label: int, dll_path: Path) -> Tuple[LabelAccumulator, Dict[str, np.ndarray]]:
    acc = LabelAccumulator()
    pcr_paths = discover_pcr_files(DATASET_DIR, label)
    if not pcr_paths:
        log(f"[label {label}] no PCR files found, skipping")
        return acc, acc.finalize()

    log(f"[label {label}] files: {', '.join(p.name for p in pcr_paths)}")

    for pcr_path in pcr_paths:
        log(f"  loading {pcr_path.name} ...")
        pcr = PCRFile(pcr_path, dll_path=dll_path)
        acc.file_count += 1
        acc.session_count += len(pcr.sessions)

        n_sessions = len(pcr.sessions)
        for session_idx in range(n_sessions):
            frames = pcr.get_session_frames(session_idx)
            total_frames = len(frames)
            if total_frames:
                acc.total_us += pcr.frame_infos[pcr.sessions[session_idx].end_frame_index].accumulated_us
            for frame_idx, points in enumerate(frames, start=1):
                acc.add_frame(frame_to_array(points))
                if frame_idx % 500 == 0 or frame_idx == total_frames:
                    log(
                        f"    session {session_idx + 1}/{n_sessions}: "
                        f"{frame_idx}/{total_frames} frames"
                    )

    log(f"  -> {acc.frame_count} frames, {acc.session_count} sessions")
    return acc, acc.finalize()


# ============================================================================
# Reporting
# ============================================================================

def report_label(label: int, acc: LabelAccumulator, stats: Dict[str, np.ndarray]) -> None:
    log("=" * 78)
    duration_s = acc.total_us / 1e6
    empty_ratio = (acc.empty_frame_count / acc.frame_count * 100.0) if acc.frame_count else 0.0
    log(
        f"LABEL {label}  | files={acc.file_count} sessions={acc.session_count} "
        f"frames={acc.frame_count} duration~{duration_s:.1f}s "
        f"empty_frames={acc.empty_frame_count} ({empty_ratio:.2f}%)"
    )
    log("-" * 78)

    log("  point count per frame:")
    log(describe(stats["point_counts"], "pts"))

    log("  z (height):")
    log(describe(stats["z"]))
    log("  per-frame centroid z:")
    log(describe(stats["centroid_z"]))

    log("  x position:")
    log(describe(stats["x"]))
    log("  y position:")
    log(describe(stats["y"]))

    log("  per-frame spread (std within frame)  x / y / z:")
    log(describe(stats["spread_x"]))
    log(describe(stats["spread_y"]))
    log(describe(stats["spread_z"]))

    log("  per-frame extent (max-min)  x / y / z (horizontal footprint = x,y):")
    log(describe(stats["extent_x"]))
    log(describe(stats["extent_y"]))
    log(describe(stats["extent_z"]))

    log("  doppler:")
    log(describe(stats["doppler"]))
    log("  per-frame mean |doppler| (motion/activity proxy):")
    log(describe(stats["abs_doppler_mean"]))
    motion = stats["abs_doppler_mean"]
    if motion.size:
        parts = [
            f">{t}: {float((motion > t).mean() * 100.0):.1f}%" for t in MOVING_THRESHOLDS
        ]
        log("        moving-frame ratio  " + "  ".join(parts))

    log(f"  power (dB = {POWER_DB_SCALE:g}*log10(power+1)):")
    log(describe(stats["power_db"], "dB"))
    log()


def report_comparison(all_stats: Dict[int, Tuple[LabelAccumulator, Dict[str, np.ndarray]]]) -> None:
    log("=" * 78)
    log("COMPARISON TABLE (key discriminative metrics)")
    log("=" * 78)

    def safe_mean(arr: np.ndarray) -> float:
        return float(arr.mean()) if arr.size else float("nan")

    def safe_median(arr: np.ndarray) -> float:
        return float(np.median(arr)) if arr.size else float("nan")

    header = (
        f"{'lbl':>3} | {'empty%':>7} | {'pts_med':>7} | {'z_mean':>7} | "
        f"{'foot_x':>7} | {'foot_y':>7} | {'|dop|':>7} | {'pow_dB':>7}"
    )
    log(header)
    log("-" * len(header))
    for label in LABELS:
        if label not in all_stats:
            continue
        acc, s = all_stats[label]
        empty_ratio = (acc.empty_frame_count / acc.frame_count * 100.0) if acc.frame_count else 0.0
        row = (
            f"{label:>3} | {empty_ratio:>7.2f} | {safe_median(s['point_counts']):>7.1f} | "
            f"{safe_mean(s['z']):>7.3f} | {safe_mean(s['extent_x']):>7.3f} | "
            f"{safe_mean(s['extent_y']):>7.3f} | {safe_mean(s['abs_doppler_mean']):>7.3f} | "
            f"{safe_mean(s['power_db']):>7.2f}"
        )
        log(row)
    log()
    log("Hints:")
    log("  - foot_x/foot_y (horizontal extent) + z_mean => posture: lying should")
    log("    spread wider horizontally / sit differently in z than awake(1).")
    log("  - |dop| (mean abs doppler) => activity: 2(still) < 3 < 4, 1(awake) varies.")
    log("  - if 1 vs 2/3/4 separate well by posture, motion-based confusion risk drops.")


def main() -> None:
    log("=" * 78)
    log("set0 dataset statistics")
    log("=" * 78)
    dll_path = find_dll()
    log(f"lzav DLL: {dll_path}")
    log(f"dataset : {DATASET_DIR}")
    log()

    all_stats: Dict[int, Tuple[LabelAccumulator, Dict[str, np.ndarray]]] = {}
    for label in LABELS:
        log(f"### processing label {label} ...")
        acc, stats = process_label(label, dll_path)
        all_stats[label] = (acc, stats)
        log()

    log()
    for label in LABELS:
        acc, stats = all_stats[label]
        report_label(label, acc, stats)

    report_comparison(all_stats)
    log("done.")


if __name__ == "__main__":
    main()
