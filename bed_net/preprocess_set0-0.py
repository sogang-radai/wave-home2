"""
Preprocess PCR files for gesture recognition training (set7+).

set7 introduces multiple PCR files per gesture label (e.g. 5-0.pcr, 5-1.pcr).
All other preprocessing behavior matches preprocess_set6/set7.
"""

import math
import pickle
import random
import re
from pathlib import Path
from typing import List, Tuple

import numpy as np

from pcr_loader import PCRFile

# ============================================================================
# Constants
# ============================================================================

GESTURE_COUNT = 16
SEQUENCE_LENGTH = 36
EXCLUDED_GESTURE_LABELS = {7, 8, 9}
OUTPUT_GESTURE_LABELS = [label for label in range(GESTURE_COUNT) if label not in EXCLUDED_GESTURE_LABELS]
OUTPUT_GESTURE_COUNT = len(OUTPUT_GESTURE_LABELS)

# Data paths
DATASET_DIR = Path(__file__).parent.parent / "dataset"
INPUT_SET_DIR = DATASET_DIR / "set7"
OUTPUT_DIR = DATASET_DIR / "processed7-3"
TRAIN_DIR = OUTPUT_DIR / "train"
VALIDATION_DIR = OUTPUT_DIR / "validation"

# Normalization ranges
X_MIN, X_MAX = -0.8, 0.8
Y_MIN, Y_MAX = 0.0, 1.5
Z_MIN, Z_MAX = -0.75, 0.0
DOPPLER_MIN, DOPPLER_MAX = -2.5, 2.5
POWER_DB_MIN, POWER_DB_MAX = 30.0, 55.0

# Feature configuration
USE_POLAR_COORDINATES = False
THETA_MIN, THETA_MAX = -0.5 * math.pi, 0.5 * math.pi
PHI_MIN, PHI_MAX = -0.5 * math.pi, 0.5 * math.pi

# One stride per gesture/file for training and validation separately.
GESTURE_VALIDATION_SPLIT = [
    0.5,  # Label 0
    0.5,  # Label 1
    0.5,  # Label 2
    0.5,  # Label 3
    0.5,  # Label 4
    40.0,  # Label 5
    40.0,  # Label 6
    40.0,  # Label 7
    40.0,  # Label 8
    40.0,  # Label 9
    0.5,  # Label 10
    0.5,  # Label 11
    0.5,  # Label 12
    0.5,  # Label 13
    0.5,  # Label 14
    0.5,  # Label 15
]

VALIDATION_GESTURE_STRIDES = [
    4,  # Label 0
    6,  # Label 1
    4,  # Label 2
    4,  # Label 3
    4,  # Label 4
    4,  # Label 5
    4,  # Label 6
    4,  # Label 7
    4,  # Label 8
    4,  # Label 9
    4,  # Label 10
    4,  # Label 11
    4,  # Label 12
    4,  # Label 13
    4,  # Label 14
    4,  # Label 15
]

TRAIN_GESTURE_STRIDES = [
    4,  # Label 0
    6,  # Label 1
    4,  # Label 2
    4,  # Label 3
    4,  # Label 4
    4,  # Label 5
    4,  # Label 6
    4,  # Label 7
    4,  # Label 8
    4,  # Label 9
    4,  # Label 10
    4,  # Label 11
    4,  # Label 12
    4,  # Label 13
    4,  # Label 14
    4,  # Label 15
]

# Padding controls.
# Each entry is a list of tuples: [(source_gesture_label, max_padding_samples, padding_ratio), ...]
# This allows multiple source-augmenters per target label.
BASE_PADDING_FRONT_CONFIGS: List[List[Tuple[int, int, float]]] = [
    [],  # Label 0
    [
        (3, 18, 0.2),
        (4, 18, 0.2),
        (10, 18, 0.2),
        (11, 18, 0.2),
        (12, 18, 0.2),
        (13, 18, 0.2),
        (14, 18, 0.2),
        (15, 18, 0.2),
    ],  # Label 1
    [],  # Label 2
    [(1, 24, 0.4), (4, 24, 0.3)],  # Label 3
    [(1, 24, 0.4), (3, 24, 0.3)],  # Label 4
    [],  # Label 5
    [],  # Label 6
    [],  # Label 7
    [],  # Label 8
    [],  # Label 9
    [(1, 24, 0.4), (11, 24, 0.3)],  # Label 10
    [(1, 24, 0.4), (10, 24, 0.3)],  # Label 11
    [(1, 24, 0.5)],  # Label 12
    [(1, 24, 0.4), (14, 24, 0.3)],  # Label 13
    [(1, 24, 0.4), (13, 24, 0.3)],  # Label 14
    [(1, 12, 0.5)],  # Label 15
]

BASE_PADDING_BACK_CONFIGS: List[List[Tuple[int, int, float]]] = [
    [],  # Label 0
    [],  # Label 1
    [],  # Label 2
    [],  # Label 3
    [],  # Label 4
    [],  # Label 5
    [],  # Label 6
    [],  # Label 7
    [],  # Label 8
    [],  # Label 9
    [],  # Label 10
    [],  # Label 11
    [],  # Label 12
    [],  # Label 13
    [],  # Label 14
    [],  # Label 15
]
# Session trim per label: (front_trim, back_trim)
# By default no trimming; apply (10,10) for labels 5..9 per user request.
SESSION_TRIM_CONFIGS: List[Tuple[int, int]] = [
    (0, 0),  # Label 0
    (0, 0),  # Label 1
    (0, 0),  # Label 2
    (0, 0),  # Label 3
    (0, 0),  # Label 4
    (10, 10),  # Label 5
    (10, 10),  # Label 6
    (10, 10),  # Label 7
    (10, 10),  # Label 8
    (10, 10),  # Label 9
    (0, 0),  # Label 10
    (0, 0),  # Label 11
    (0, 0),  # Label 12
    (0, 0),  # Label 13
    (0, 0),  # Label 14
    (0, 0),  # Label 15
]
# Session masks per gesture label.
# If the first value is -1, all sessions are included for that label.
SESSION_MASKS = [[-1] for _ in range(GESTURE_COUNT)]

_VARIANT_PATTERN = re.compile(r"^(\d+)-(\d+)$")

# ============================================================================
# Helpers
# ============================================================================

def log(message: str = "") -> None:
    print(message, flush=True)


def normalize_value(value: float, min_val: float, max_val: float) -> float:
    return 2.0 * (value - min_val) / (max_val - min_val) - 1.0


def power_to_db(power: float) -> float:
    return 10.0 * math.log10(power + 1.0)


def should_include_session(label: int, session_idx: int, session_masks: List[List[int]]) -> bool:
    session_mask = session_masks[label]
    if not session_mask:
        return True
    if session_mask[0] == -1:
        return True
    return bool(session_mask[session_idx % len(session_mask)])


def get_gesture_validation_fraction(label: int, gesture_validation_split: List[float]) -> float:
    if label < 0 or label >= len(gesture_validation_split):
        return 0.5
    split = gesture_validation_split[label]
    return split if split >= 0.0 else 0.5


def get_session_validation_fraction(
    label: int,
    session_idx: int,
    session_masks: List[List[int]],
    gesture_validation_split: List[float],
) -> float:
    if not should_include_session(label, session_idx, session_masks):
        return 0.0

    session_mask = session_masks[label]
    if not session_mask or session_mask[0] == -1:
        cumulative_ones = session_idx + 1
    else:
        mask_len = len(session_mask)
        cumulative_ones = 0
        for idx in range(session_idx + 1):
            if session_mask[idx % mask_len] == 1:
                cumulative_ones += 1

    validation_split = get_gesture_validation_fraction(label, gesture_validation_split)

    if cumulative_ones <= validation_split:
        return 1.0
    if cumulative_ones - 1 < validation_split:
        return validation_split - (cumulative_ones - 1)
    return 0.0


def get_gesture_stride(label: int, gesture_stride: List[int]) -> int:
    if label < 0 or label >= len(gesture_stride):
        return 16
    stride = gesture_stride[label]
    return stride if stride > 0 else 16


def get_padding_config(label: int, padding_configs: List[List[Tuple[int, int, float]]]) -> List[Tuple[int, int, float]]:
    if label < 0 or label >= len(padding_configs):
        return []
    configs = padding_configs[label]
    # filter out disabled entries
    return [cfg for cfg in configs if cfg[1] > 0 and cfg[2] > 0.0]


def get_output_label_index(label: int) -> int:
    try:
        return OUTPUT_GESTURE_LABELS.index(label)
    except ValueError:
        return -1


def cartesian_to_polar(x: float, y: float, z: float) -> Tuple[float, float, float]:
    r = math.sqrt(x * x + y * y + z * z)
    theta = math.atan2(y, x)
    phi = math.atan2(z, math.sqrt(x * x + y * y))
    return r, theta, phi


def normalize_point(point_data: Tuple[float, float, float, float, float]) -> np.ndarray:
    x, y, z, doppler, power = point_data

    if USE_POLAR_COORDINATES:
        r, theta, phi = cartesian_to_polar(x, y, z)
        r_norm = normalize_value(r, 0.0, 20.0)
        theta_norm = normalize_value(theta, THETA_MIN, THETA_MAX)
        phi_norm = normalize_value(phi, PHI_MIN, PHI_MAX)
        doppler_norm = normalize_value(doppler, DOPPLER_MIN, DOPPLER_MAX)
        power_norm = normalize_value(power_to_db(power), POWER_DB_MIN, POWER_DB_MAX)
        return np.array([r_norm, theta_norm, phi_norm, doppler_norm, power_norm], dtype=np.float32)

    x_norm = normalize_value(x, X_MIN, X_MAX)
    y_norm = normalize_value(y, Y_MIN, Y_MAX)
    z_norm = normalize_value(z, Z_MIN, Z_MAX)
    doppler_norm = normalize_value(doppler, DOPPLER_MIN, DOPPLER_MAX)
    power_norm = normalize_value(power_to_db(power), POWER_DB_MIN, POWER_DB_MAX)
    return np.array([x_norm, y_norm, z_norm, doppler_norm, power_norm], dtype=np.float32)


def discover_pcr_files(input_dir: Path, label: int) -> List[Path]:
    """Return sorted PCR paths for a gesture label.

    Supports both legacy single-file naming ({label}.pcr) and set7+ variant
    naming ({label}-{variant}.pcr). Variant files take precedence when present.
    """
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


def load_and_normalize_pcr(
    pcr_path: Path,
    label: int,
    session_masks: List[List[int]],
    gesture_validation_split: List[float],
    session_idx_offset: int = 0,
) -> Tuple[List[np.ndarray], List[Tuple[int, float]], int]:
    log(f"  Loading PCR file: {pcr_path.name}")
    pcr = PCRFile(pcr_path)
    log(f"  Parsed metadata: {len(pcr.sessions)} sessions, {len(pcr.frame_infos)} frames")

    all_frames: List[np.ndarray] = []
    session_info: List[Tuple[int, float]] = []

    total_sessions = len(pcr.sessions)
    for local_session_idx in range(total_sessions):
        global_session_idx = session_idx_offset + local_session_idx
        if not should_include_session(label, global_session_idx, session_masks):
            log(f"    Session {local_session_idx + 1}/{total_sessions}: skipped by session_mask")
            continue

        session_frames = pcr.get_session_frames(local_session_idx)
        val_fraction = get_session_validation_fraction(
            label,
            global_session_idx,
            session_masks,
            gesture_validation_split,
        )
        session_info.append((len(session_frames), val_fraction))
        log(
            f"    Session {local_session_idx + 1}/{total_sessions} "
            f"(global={global_session_idx + 1}): {len(session_frames)} frames "
            f"(validation_split={val_fraction:.2f})"
        )

        total_frames = len(session_frames)
        for frame_idx, points in enumerate(session_frames, start=1):
            if len(points) == 0:
                normalized_frame = np.zeros((0, 5), dtype=np.float32)
            else:
                normalized_points = [
                    normalize_point((point.x, point.y, point.z, point.doppler, point.power))
                    for point in points
                ]
                normalized_frame = np.array(normalized_points, dtype=np.float32)

            all_frames.append(normalized_frame)

            if frame_idx == total_frames or frame_idx % 250 == 0:
                log(f"      normalized {frame_idx}/{total_frames} frames")

    next_session_idx_offset = session_idx_offset + total_sessions
    return all_frames, session_info, next_session_idx_offset


def split_session_frames(
    frames: List[np.ndarray],
    session_info: List[Tuple[int, float]],
) -> Tuple[List[List[np.ndarray]], List[List[np.ndarray]]]:
    train_segments: List[List[np.ndarray]] = []
    validation_segments: List[List[np.ndarray]] = []
    frame_idx = 0

    for session_frame_count, val_fraction in session_info:
        session_end = frame_idx + session_frame_count
        val_frame_count = int(session_frame_count * val_fraction)
        val_end = frame_idx + val_frame_count

        validation_frames = frames[frame_idx:val_end]
        train_frames = frames[val_end:session_end]

        if validation_frames:
            validation_segments.append(validation_frames)
        if train_frames:
            train_segments.append(train_frames)

        frame_idx = session_end

    return train_segments, validation_segments


def create_samples(
    frames: List[np.ndarray],
    session_info: List[Tuple[int, float]],
    label: int,
    train_stride: int,
    val_stride: int,
    trim_front: int = 0,
    trim_back: int = 0,
) -> Tuple[List[Tuple[List[int], int]], List[Tuple[List[int], int]]]:
    train_samples: List[Tuple[List[int], int]] = []
    val_samples: List[Tuple[List[int], int]] = []
    frame_idx = 0

    for session_frame_count, val_fraction in session_info:
        # apply trimming per session
        effective_total = session_frame_count - trim_front - trim_back
        if effective_total <= 0:
            frame_idx += session_frame_count
            continue

        session_start = frame_idx + trim_front
        session_end = session_start + effective_total

        val_frame_count = int(effective_total * val_fraction)
        val_end = session_start + val_frame_count

        current_idx = session_start
        while current_idx + SEQUENCE_LENGTH <= val_end:
            frame_indices = list(range(current_idx, current_idx + SEQUENCE_LENGTH))
            val_samples.append((frame_indices, label))
            current_idx += val_stride

        current_idx = val_end
        while current_idx + SEQUENCE_LENGTH <= session_end:
            frame_indices = list(range(current_idx, current_idx + SEQUENCE_LENGTH))
            train_samples.append((frame_indices, label))
            current_idx += train_stride

        frame_idx += session_frame_count

    return train_samples, val_samples


def build_sample_data(samples: List[Tuple[List[int], int]], all_frames: List[np.ndarray]) -> List[dict]:
    sample_data: List[dict] = []
    for frame_indices, _label in samples:
        frame_arrays = [all_frames[idx] for idx in frame_indices]
        sample_data.append({"frames": frame_arrays})
    return sample_data


def save_samples(sample_data: List[dict], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "wb") as file_handle:
        pickle.dump(sample_data, file_handle)


def pick_random_segment(segments: List[List[np.ndarray]], length: int) -> List[np.ndarray] | None:
    if length <= 0:
        return []

    candidates = [segment for segment in segments if len(segment) >= length]
    if not candidates:
        return None

    selected_segment = random.choice(candidates)
    max_start = len(selected_segment) - length
    start_idx = random.randint(0, max_start) if max_start > 0 else 0
    return selected_segment[start_idx:start_idx + length]


def select_sample_indices(total_count: int, ratio: float) -> List[int]:
    if total_count <= 0 or ratio <= 0.0:
        return []

    target_count = min(total_count, int(total_count * ratio))
    if target_count <= 0:
        return []

    return sorted(random.sample(range(total_count), target_count))


def make_front_padded_sample(
    sample_frames: List[np.ndarray],
    base_segments: List[List[np.ndarray]],
    max_prefix: int,
) -> List[np.ndarray] | None:
    if max_prefix <= 0 or not base_segments:
        return None

    prefix_cap = min(max_prefix, SEQUENCE_LENGTH - 1, len(sample_frames) - 1)
    if prefix_cap < 1:
        return None

    prefix_length = random.randint(1, prefix_cap)
    prefix_frames = pick_random_segment(base_segments, prefix_length)
    if prefix_frames is None:
        return None

    suffix_frames = sample_frames[:SEQUENCE_LENGTH - prefix_length]
    if len(prefix_frames) + len(suffix_frames) != SEQUENCE_LENGTH:
        return None

    return prefix_frames + suffix_frames


def make_back_padded_sample(
    sample_frames: List[np.ndarray],
    base_segments: List[List[np.ndarray]],
    max_suffix: int,
) -> List[np.ndarray] | None:
    if max_suffix <= 0 or not base_segments:
        return None

    suffix_cap = min(max_suffix, SEQUENCE_LENGTH - 1, len(sample_frames) - 1)
    if suffix_cap < 1:
        return None

    suffix_length = random.randint(1, suffix_cap)
    suffix_frames = pick_random_segment(base_segments, suffix_length)
    if suffix_frames is None:
        return None

    prefix_frames = sample_frames[:SEQUENCE_LENGTH - suffix_length]
    if len(prefix_frames) + len(suffix_frames) != SEQUENCE_LENGTH:
        return None

    return prefix_frames + suffix_frames


def augment_front_samples(
    sample_data: List[dict],
    source_segments: List[List[np.ndarray]],
    max_prefix: int,
    ratio: float,
) -> List[dict]:
    if max_prefix <= 0 or ratio <= 0.0 or not sample_data or not source_segments:
        return []

    chosen_indices = select_sample_indices(len(sample_data), ratio)
    augmented_samples: List[dict] = []
    for sample_idx in chosen_indices:
        padded_frames = make_front_padded_sample(sample_data[sample_idx]["frames"], source_segments, max_prefix)
        if padded_frames is not None:
            augmented_samples.append({"frames": padded_frames})
    return augmented_samples


def augment_back_samples(
    sample_data: List[dict],
    source_segments: List[List[np.ndarray]],
    max_suffix: int,
    ratio: float,
) -> List[dict]:
    if max_suffix <= 0 or ratio <= 0.0 or not sample_data or not source_segments:
        return []

    chosen_indices = select_sample_indices(len(sample_data), ratio)
    augmented_samples: List[dict] = []
    for sample_idx in chosen_indices:
        padded_frames = make_back_padded_sample(sample_data[sample_idx]["frames"], source_segments, max_suffix)
        if padded_frames is not None:
            augmented_samples.append({"frames": padded_frames})
    return augmented_samples


def process_gesture_file(
    pcr_path: Path,
    label: int,
    session_masks: List[List[int]],
    gesture_validation_split: List[float],
    train_stride: int,
    val_stride: int,
    session_idx_offset: int = 0,
) -> Tuple[List[dict], List[dict], List[List[np.ndarray]], List[List[np.ndarray]], int]:
    frames, session_info, next_session_idx_offset = load_and_normalize_pcr(
        pcr_path,
        label,
        session_masks,
        gesture_validation_split,
        session_idx_offset=session_idx_offset,
    )
    log(f"  Loaded {len(frames)} frames from {len(session_info)} sessions")

    trim_front, trim_back = SESSION_TRIM_CONFIGS[label] if 0 <= label < len(SESSION_TRIM_CONFIGS) else (0, 0)
    train_samples, val_samples = create_samples(frames, session_info, label, train_stride, val_stride, trim_front=trim_front, trim_back=trim_back)
    log(f"  Created {len(train_samples)} training + {len(val_samples)} validation samples")

    train_sample_data = build_sample_data(train_samples, frames)
    val_sample_data = build_sample_data(val_samples, frames)
    train_segments, validation_segments = split_session_frames(frames, session_info)

    return (
        train_sample_data,
        val_sample_data,
        train_segments,
        validation_segments,
        next_session_idx_offset,
    )


def process_gesture_label(
    pcr_paths: List[Path],
    label: int,
    session_masks: List[List[int]],
    gesture_validation_split: List[float],
    train_stride: int,
    val_stride: int,
) -> Tuple[List[dict], List[dict], List[List[np.ndarray]], List[List[np.ndarray]]]:
    train_sample_data: List[dict] = []
    val_sample_data: List[dict] = []
    train_segments: List[List[np.ndarray]] = []
    validation_segments: List[List[np.ndarray]] = []

    session_idx_offset = 0
    for pcr_path in pcr_paths:
        file_train, file_val, file_train_segments, file_val_segments, session_idx_offset = process_gesture_file(
            pcr_path=pcr_path,
            label=label,
            session_masks=session_masks,
            gesture_validation_split=gesture_validation_split,
            train_stride=train_stride,
            val_stride=val_stride,
            session_idx_offset=session_idx_offset,
        )
        train_sample_data.extend(file_train)
        val_sample_data.extend(file_val)
        train_segments.extend(file_train_segments)
        validation_segments.extend(file_val_segments)

    return train_sample_data, val_sample_data, train_segments, validation_segments


def preprocess_dataset(
    input_dir: Path,
    output_dir: Path,
    session_masks: List[List[int]],
    train_gesture_strides: List[int],
    val_gesture_strides: List[int],
    gesture_validation_split: List[float],
) -> None:
    train_dir = output_dir / "train"
    val_dir = output_dir / "validation"
    train_dir.mkdir(parents=True, exist_ok=True)
    val_dir.mkdir(parents=True, exist_ok=True)

    train_outputs: List[List[dict]] = [[] for _ in range(OUTPUT_GESTURE_COUNT)]
    validation_outputs: List[List[dict]] = [[] for _ in range(OUTPUT_GESTURE_COUNT)]
    train_segments_by_label: List[List[List[np.ndarray]]] = [[] for _ in range(GESTURE_COUNT)]
    validation_segments_by_label: List[List[List[np.ndarray]]] = [[] for _ in range(GESTURE_COUNT)]

    for label in OUTPUT_GESTURE_LABELS:
        pcr_paths = discover_pcr_files(input_dir, label)
        if not pcr_paths:
            log(f"Skipping label {label} - no PCR files found")
            continue

        file_names = ", ".join(path.name for path in pcr_paths)
        output_label = get_output_label_index(label)
        log(f"Processing label {label} -> output {output_label}: {file_names}")

        try:
            train_sample_data, val_sample_data, train_segments, validation_segments = process_gesture_label(
                pcr_paths=pcr_paths,
                label=label,
                session_masks=session_masks,
                gesture_validation_split=gesture_validation_split,
                train_stride=get_gesture_stride(label, train_gesture_strides),
                val_stride=get_gesture_stride(label, val_gesture_strides),
            )

            train_segments_by_label[label] = train_segments
            validation_segments_by_label[label] = validation_segments

            if output_label >= 0:
                train_outputs[output_label].extend(train_sample_data)
                validation_outputs[output_label].extend(val_sample_data)

        except Exception as exc:
            log(f"  ERROR: {exc}")
            raise

    for output_label, original_label in enumerate(OUTPUT_GESTURE_LABELS):
        front_configs = get_padding_config(original_label, BASE_PADDING_FRONT_CONFIGS)
        back_configs = get_padding_config(original_label, BASE_PADDING_BACK_CONFIGS)

        source_train_samples = train_outputs[output_label][:]
        source_val_samples = validation_outputs[output_label][:]

        any_augmented = False

        # apply front augmentations from each configured source
        for src_label, src_max, src_ratio in front_configs:
            src_train_segments = train_segments_by_label[src_label] if 0 <= src_label < GESTURE_COUNT else []
            src_val_segments = validation_segments_by_label[src_label] if 0 <= src_label < GESTURE_COUNT else []

            front_train = augment_front_samples(source_train_samples, src_train_segments, src_max, src_ratio)
            front_val = augment_front_samples(source_val_samples, src_val_segments, src_max, src_ratio)
            if front_train or front_val:
                any_augmented = True
            train_outputs[output_label].extend(front_train)
            validation_outputs[output_label].extend(front_val)

        # apply back augmentations from each configured source
        for src_label, src_max, src_ratio in back_configs:
            src_train_segments = train_segments_by_label[src_label] if 0 <= src_label < GESTURE_COUNT else []
            src_val_segments = validation_segments_by_label[src_label] if 0 <= src_label < GESTURE_COUNT else []

            back_train = augment_back_samples(source_train_samples, src_train_segments, src_max, src_ratio)
            back_val = augment_back_samples(source_val_samples, src_val_segments, src_max, src_ratio)
            if back_train or back_val:
                any_augmented = True
            train_outputs[output_label].extend(back_train)
            validation_outputs[output_label].extend(back_val)

        if any_augmented:
            log(
                f"  Augmented label {original_label} -> output {output_label} "
                f"train={len(train_outputs[output_label])}, val={len(validation_outputs[output_label])}"
            )

    for output_label, original_label in enumerate(OUTPUT_GESTURE_LABELS):
        train_path = train_dir / f"{output_label}.pkl"
        val_path = val_dir / f"{output_label}.pkl"
        save_samples(train_outputs[output_label], train_path)
        save_samples(validation_outputs[output_label], val_path)
        log(
            f"Saved label {original_label} as output {output_label}: "
            f"{len(train_outputs[output_label])} training samples to {train_path}"
        )
        log(
            f"Saved label {original_label} as output {output_label}: "
            f"{len(validation_outputs[output_label])} validation samples to {val_path}"
        )


def main() -> None:
    log("=" * 80)
    log("Preprocessing PCR data for gesture recognition (set7+ multi-file)")
    log("=" * 80)
    log(f"USE_POLAR_COORDINATES: {USE_POLAR_COORDINATES}")
    log(f"SEQUENCE_LENGTH: {SEQUENCE_LENGTH}")
    log(f"EXCLUDED_GESTURE_LABELS: {sorted(EXCLUDED_GESTURE_LABELS)}")
    log(f"OUTPUT_GESTURE_LABELS: {OUTPUT_GESTURE_LABELS}")
    log(f"GESTURE_VALIDATION_SPLIT: {GESTURE_VALIDATION_SPLIT}")
    log(f"BASE_PADDING_FRONT_CONFIGS: {BASE_PADDING_FRONT_CONFIGS}")
    log(f"BASE_PADDING_BACK_CONFIGS: {BASE_PADDING_BACK_CONFIGS}")
    log()

    log("Processing all data (training + validation split)...")
    log("-" * 80)
    preprocess_dataset(
        INPUT_SET_DIR,
        OUTPUT_DIR,
        SESSION_MASKS,
        TRAIN_GESTURE_STRIDES,
        VALIDATION_GESTURE_STRIDES,
        GESTURE_VALIDATION_SPLIT,
    )
    log()

    log("=" * 80)
    log("Preprocessing complete!")
    log("=" * 80)


if __name__ == "__main__":
    main()
