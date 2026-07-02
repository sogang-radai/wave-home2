"""
Preprocess PCR files (dataset/set0) for the sleep-net pipeline.

Two heads share one PointNet encoder:
  - bed-net  (status): 0 none / 1 awake / 2 asleep       window = BED_WINDOW  (~8s)
  - toss-net (toss)  : 0 calm / 1 slight / 2 moderate     window = TOSS_WINDOW (~2s)

Source label -> head label mapping (set0):
  0 (empty)          -> bed 0
  1 (present/awake)  -> bed 1
  2 (asleep, still)  -> bed 2, toss 0
  3 (asleep, toss)   -> bed 2, toss 1
  4 (asleep, heavy)  -> bed 2, toss 2

No augmentation yet: labels 3/4 are fed to bed-net as-is (status=2).

--------------------------------------------------------------------------------
Output format (single pickle, sleep-net/samples/set0-0.pkl)
--------------------------------------------------------------------------------
The entire dataset is stored in ONE file. Normalized frames are stored ONCE in a
shared pool; every sample only references frame indices. This keeps overlapping
windows (and later, spliced augmentations) cheap because frames are never
duplicated.

{
    "meta": { ...configuration echo... },
    "frames": [ np.ndarray(N_i, 5) float32, ... ],   # shared frame pool (all points kept)
    "sessions": [ {"source_label", "pool_start", "pool_end", "split_point"}, ... ],
    "bed_samples":  [ {"frame_indices": [...], "label", "source_label", "split"}, ... ],
    "toss_samples": [ {"frame_indices": [...], "label", "source_label", "split"}, ... ],
}

Each frame is (num_points, 5): [x, y, z, doppler, power] normalized to [-1, 1].
Empty frames are kept as shape (0, 5); point masking is handled at batch time by
the training collate function, and the real point count is used at inference.
"""

import math
import pickle
import sys
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np

SCRIPT_DIR = Path(__file__).resolve().parent
SLEEP_NET_DIR = SCRIPT_DIR.parent
sys.path.insert(0, str(SLEEP_NET_DIR))

from pcr_loader import PCRFile  # noqa: E402

# ============================================================================
# Constants
# ============================================================================

# Paths
DATASET_DIR = SLEEP_NET_DIR / "dataset"
INPUT_SET_DIR = DATASET_DIR / "set0"
SAMPLES_DIR = SLEEP_NET_DIR / "samples"
OUTPUT_PKL = SAMPLES_DIR / "set0-0.pkl"

# Capture rate (measured: label 0 = 12081 frames / 603.3 s ~= 20 fps)
FPS = 20

# Temporal window lengths (frames)
BED_WINDOW = 160   # ~8 s
TOSS_WINDOW = 40   # ~2 s

# Source labels present in set0
SOURCE_LABELS = [0, 1, 2, 3, 4]

# bed-net (status) label map and class count
BED_LABEL_MAP: Dict[int, int] = {0: 0, 1: 1, 2: 2, 3: 2, 4: 2}
BED_CLASS_COUNT = 3

# toss-net label map (asleep sources only) and class count
TOSS_LABEL_MAP: Dict[int, int] = {2: 0, 3: 1, 4: 2}
TOSS_CLASS_COUNT = 3

# Training strides (frames) per source label. Windowing is dense; the training
# sampler is expected to balance classes, so these are approximate.
BED_STRIDE: Dict[int, int] = {0: 40, 1: 32, 2: 20, 3: 20, 4: 20}
TOSS_STRIDE: Dict[int, int] = {2: 8, 3: 8, 4: 8}

# Validation split: the FRONT fraction of each session becomes validation.
# Windows straddling the train/val boundary are dropped to avoid leakage.
VALIDATION_FRACTION: Dict[int, float] = {0: 0.2, 1: 0.2, 2: 0.2, 3: 0.2, 4: 0.2}
DEFAULT_VALIDATION_FRACTION = 0.2

# Normalization ranges (from radar rangeMin/Max + set0 statistics)
X_MIN, X_MAX = -0.8, 0.8
Y_MIN, Y_MAX = -1.0, 1.0
Z_MIN, Z_MAX = -2.1, 0.0
DOPPLER_MIN, DOPPLER_MAX = -2.5, 2.5
POWER_DB_MIN, POWER_DB_MAX = 25.0, 60.0
POWER_DB_SCALE = 10.0  # dB = 10 * log10(power + 1)


# ============================================================================
# Helpers
# ============================================================================

def log(message: str = "") -> None:
    print(message, flush=True)


def find_dll() -> Path:
    root = SLEEP_NET_DIR.parent  # wave-home2
    candidates = [
        root / "thirdparty" / "lzav" / "bin" / "x64" / "lzav_lib.dll",
        root / "thirdparty" / "lzav" / "bin" / "x64" / "lzav_lib.so",
        root / "thirdparty" / "lzav" / "bin" / "arm64" / "lzav_lib.so",
        SLEEP_NET_DIR / "bin" / "x64" / "lzav_lib.dll",
        SLEEP_NET_DIR / "bin" / "x64" / "lzav_lib.so",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "lzav_lib not found. Checked:\n  " + "\n  ".join(str(c) for c in candidates)
    )


def normalize_value(value: float, min_val: float, max_val: float) -> float:
    return 2.0 * (value - min_val) / (max_val - min_val) - 1.0


def power_to_db(power: float) -> float:
    return POWER_DB_SCALE * math.log10(power + 1.0)


def discover_pcr_files(input_dir: Path, label: int) -> List[Path]:
    """Return sorted PCR paths for a source label ({label}.pcr or {label}-{v}.pcr)."""
    variant_paths: List[Tuple[int, Path]] = []
    for path in input_dir.glob("*.pcr"):
        stem = path.stem
        if "-" in stem:
            base, _, variant = stem.partition("-")
            if base.isdigit() and variant.isdigit() and int(base) == label:
                variant_paths.append((int(variant), path))

    if variant_paths:
        variant_paths.sort(key=lambda item: item[0])
        return [path for _, path in variant_paths]

    single_path = input_dir / f"{label}.pcr"
    if single_path.exists():
        return [single_path]
    return []


def normalize_frame(points) -> np.ndarray:
    """Normalize one frame's points to (num_points, 5) float32. Keeps all points."""
    if not points:
        return np.zeros((0, 5), dtype=np.float32)

    buffer = np.empty((len(points), 5), dtype=np.float32)
    for i, p in enumerate(points):
        buffer[i, 0] = normalize_value(p.x, X_MIN, X_MAX)
        buffer[i, 1] = normalize_value(p.y, Y_MIN, Y_MAX)
        buffer[i, 2] = normalize_value(p.z, Z_MIN, Z_MAX)
        buffer[i, 3] = normalize_value(p.doppler, DOPPLER_MIN, DOPPLER_MAX)
        buffer[i, 4] = normalize_value(power_to_db(p.power), POWER_DB_MIN, POWER_DB_MAX)
    return buffer


def get_validation_fraction(source_label: int) -> float:
    return VALIDATION_FRACTION.get(source_label, DEFAULT_VALIDATION_FRACTION)


def make_windows(
    region_start: int,
    region_end: int,
    window: int,
    stride: int,
) -> List[List[int]]:
    """Contiguous windows fully inside [region_start, region_end), stepped by stride."""
    windows: List[List[int]] = []
    if stride <= 0:
        return windows
    current = region_start
    while current + window <= region_end:
        windows.append(list(range(current, current + window)))
        current += stride
    return windows


# ============================================================================
# Core processing
# ============================================================================

def build_dataset() -> dict:
    dll_path = find_dll()
    log(f"lzav DLL: {dll_path}")
    log(f"input   : {INPUT_SET_DIR}")
    log()

    frames_pool: List[np.ndarray] = []
    sessions_meta: List[dict] = []
    bed_samples: List[dict] = []
    toss_samples: List[dict] = []

    for source_label in SOURCE_LABELS:
        pcr_paths = discover_pcr_files(INPUT_SET_DIR, source_label)
        if not pcr_paths:
            log(f"[label {source_label}] no PCR files, skipping")
            continue

        val_fraction = get_validation_fraction(source_label)
        bed_stride = BED_STRIDE.get(source_label)
        toss_stride = TOSS_STRIDE.get(source_label)
        bed_label = BED_LABEL_MAP.get(source_label)
        toss_label = TOSS_LABEL_MAP.get(source_label)

        log(
            f"[label {source_label}] files={', '.join(p.name for p in pcr_paths)} "
            f"bed={bed_label} toss={toss_label} val_frac={val_fraction}"
        )

        label_bed = 0
        label_toss = 0

        for pcr_path in pcr_paths:
            log(f"  loading {pcr_path.name} ...")
            pcr = PCRFile(pcr_path, dll_path=dll_path)
            n_sessions = len(pcr.sessions)

            for session_idx in range(n_sessions):
                session_points = pcr.get_session_frames(session_idx)
                total = len(session_points)

                pool_start = len(frames_pool)
                for frame_idx, points in enumerate(session_points, start=1):
                    frames_pool.append(normalize_frame(points))
                    if frame_idx % 1000 == 0 or frame_idx == total:
                        log(f"    session {session_idx + 1}/{n_sessions}: "
                            f"normalized {frame_idx}/{total} frames")
                pool_end = len(frames_pool)

                # Front fraction -> validation, remainder -> train (leakage-free).
                session_len = pool_end - pool_start
                val_len = int(session_len * val_fraction)
                split_point = pool_start + val_len

                sessions_meta.append({
                    "source_label": source_label,
                    "pool_start": pool_start,
                    "pool_end": pool_end,
                    "split_point": split_point,
                })

                val_region = (pool_start, split_point)
                train_region = (split_point, pool_end)

                if bed_stride is not None:
                    for region, split in ((val_region, "val"), (train_region, "train")):
                        for indices in make_windows(region[0], region[1], BED_WINDOW, bed_stride):
                            bed_samples.append({
                                "frame_indices": indices,
                                "label": bed_label,
                                "source_label": source_label,
                                "split": split,
                            })
                            label_bed += 1

                if toss_stride is not None:
                    for region, split in ((val_region, "val"), (train_region, "train")):
                        for indices in make_windows(region[0], region[1], TOSS_WINDOW, toss_stride):
                            toss_samples.append({
                                "frame_indices": indices,
                                "label": toss_label,
                                "source_label": source_label,
                                "split": split,
                            })
                            label_toss += 1

        log(f"  -> bed_samples+={label_bed}, toss_samples+={label_toss}")
        log()

    meta = {
        "fps": FPS,
        "bed_window": BED_WINDOW,
        "toss_window": TOSS_WINDOW,
        "bed_class_count": BED_CLASS_COUNT,
        "toss_class_count": TOSS_CLASS_COUNT,
        "bed_label_map": BED_LABEL_MAP,
        "toss_label_map": TOSS_LABEL_MAP,
        "bed_stride": BED_STRIDE,
        "toss_stride": TOSS_STRIDE,
        "validation_fraction": VALIDATION_FRACTION,
        "normalization": {
            "x": [X_MIN, X_MAX],
            "y": [Y_MIN, Y_MAX],
            "z": [Z_MIN, Z_MAX],
            "doppler": [DOPPLER_MIN, DOPPLER_MAX],
            "power_db": [POWER_DB_MIN, POWER_DB_MAX],
            "power_db_scale": POWER_DB_SCALE,
        },
        "feature_order": ["x", "y", "z", "doppler", "power"],
    }

    return {
        "meta": meta,
        "frames": frames_pool,
        "sessions": sessions_meta,
        "bed_samples": bed_samples,
        "toss_samples": toss_samples,
    }


def summarize(dataset: dict) -> None:
    frames = dataset["frames"]
    bed = dataset["bed_samples"]
    toss = dataset["toss_samples"]

    def count_by(samples: List[dict], key: str, split: str = None) -> Dict[int, int]:
        counts: Dict[int, int] = {}
        for s in samples:
            if split is not None and s["split"] != split:
                continue
            counts[s[key]] = counts.get(s[key], 0) + 1
        return counts

    log("=" * 70)
    log("SUMMARY")
    log("=" * 70)
    log(f"frame pool         : {len(frames)} frames")
    log(f"sessions           : {len(dataset['sessions'])}")
    log(f"bed samples total  : {len(bed)}  (train={sum(1 for s in bed if s['split']=='train')}, "
        f"val={sum(1 for s in bed if s['split']=='val')})")
    log(f"  bed by class (train): {dict(sorted(count_by(bed, 'label', 'train').items()))}")
    log(f"  bed by class (val)  : {dict(sorted(count_by(bed, 'label', 'val').items()))}")
    log(f"toss samples total : {len(toss)}  (train={sum(1 for s in toss if s['split']=='train')}, "
        f"val={sum(1 for s in toss if s['split']=='val')})")
    log(f"  toss by class (train): {dict(sorted(count_by(toss, 'label', 'train').items()))}")
    log(f"  toss by class (val)  : {dict(sorted(count_by(toss, 'label', 'val').items()))}")


def main() -> None:
    log("=" * 70)
    log("Preprocessing set0 for sleep-net (bed-net + toss-net)")
    log("=" * 70)

    dataset = build_dataset()
    summarize(dataset)

    SAMPLES_DIR.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_PKL, "wb") as f:
        pickle.dump(dataset, f, protocol=pickle.HIGHEST_PROTOCOL)

    size_mb = OUTPUT_PKL.stat().st_size / (1024 * 1024)
    log()
    log(f"saved -> {OUTPUT_PKL}  ({size_mb:.1f} MB)")
    log("done.")


if __name__ == "__main__":
    main()
