from __future__ import annotations

import csv
import datetime
import math
import pickle
import random
import sys
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, Dataset, Sampler
from tqdm import tqdm

SCRIPT_DIR = Path(__file__).resolve().parent
GESTURE_NET_DIR = SCRIPT_DIR.parent
sys.path.insert(0, str(GESTURE_NET_DIR))
sys.path.insert(0, str(SCRIPT_DIR))

from networks.PointNetV1nCNNV5 import (  # noqa: E402
    GESTURE_COUNT,
    GestureClassifierV5,
    SEQUENCE_LENGTH,
)

# ============================================================================
# Training Configuration
# ============================================================================

MODEL_TAG = "set7-3"
DATA_DIR: Optional[Path] = None  # override; default: dataset/processed7-3
BATCH_SIZE: int = 4
ACCUMULATION_STEPS: int = 4
EPOCHS: int = 50
LEARNING_RATE: float = 1e-3
SEED: int = 42
NUM_WORKERS: int = 4

DEFAULT_PREPROCESSED_DIRS = (
    GESTURE_NET_DIR / "dataset" / "processed7-3",
)

TRAIN_OUTPUT_DIR = GESTURE_NET_DIR / "training" / MODEL_TAG
RESUME_TRAINING: bool = False
CHECKPOINT_PATH: Path = TRAIN_OUTPUT_DIR / "checkpoint.pth"
BEST_MODEL_PATH: Path = TRAIN_OUTPUT_DIR / "best_model.pth"

LOG_TRAINING_CSV_PATH: Path = TRAIN_OUTPUT_DIR / "logs" / "training_log.csv"
LOG_VALIDATION_CSV_PATH: Path = TRAIN_OUTPUT_DIR / "logs" / "validation_log.csv"
LOG_SUMMARY_CSV_PATH: Path = TRAIN_OUTPUT_DIR / "logs" / "summary_log.csv"


class PickleGestureDataset(Dataset):
    """Loads per-label pickles written by preprocess_set7-3.py."""

    def __init__(self, split_dir: Path) -> None:
        self.samples: List[Dict[str, object]] = []
        if not split_dir.is_dir():
            raise FileNotFoundError(f"Missing split directory: {split_dir}")

        for pkl_path in sorted(split_dir.glob("*.pkl")):
            gesture = int(pkl_path.stem)
            with open(pkl_path, "rb") as handle:
                rows = pickle.load(handle)
            for row in rows:
                frames = row["frames"]
                self.samples.append({"frames": frames, "gesture": gesture})

        if not self.samples:
            raise FileNotFoundError(f"No samples under {split_dir}")

        self.indices_by_gesture: Dict[int, List[int]] = {
            gesture: [] for gesture in range(GESTURE_COUNT)
        }
        for index, sample in enumerate(self.samples):
            gesture = int(sample["gesture"])
            self.indices_by_gesture.setdefault(gesture, []).append(index)

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int) -> Dict[str, object]:
        sample = self.samples[index]
        return {
            "frames": sample["frames"],
            "gesture": int(sample["gesture"]),
            "batch_index": index,
        }


class BalancedGestureBatchSampler(Sampler[List[int]]):
    def __init__(
        self,
        dataset: PickleGestureDataset,
        batch_size: int = BATCH_SIZE,
        steps_per_epoch: Optional[int] = None,
        seed: int = 42,
    ) -> None:
        self.dataset = dataset
        self.batch_size = batch_size
        self.steps_per_epoch = (
            steps_per_epoch if steps_per_epoch is not None else max(1, len(dataset) // batch_size)
        )
        self.seed = seed
        self._epoch = 0
        self._gesture_ids = [
            gesture
            for gesture in range(GESTURE_COUNT)
            if dataset.indices_by_gesture.get(gesture)
        ]
        self._gesture_weights = [1.0 for _ in self._gesture_ids]

    def set_epoch(self, epoch: int) -> None:
        self._epoch = epoch

    def __iter__(self) -> Iterator[List[int]]:
        rng = random.Random(self.seed + self._epoch)
        for _ in range(self.steps_per_epoch):
            batch_indices: List[int] = []
            for _ in range(self.batch_size):
                gesture = rng.choices(self._gesture_ids, weights=self._gesture_weights, k=1)[0]
                batch_indices.append(rng.choice(self.dataset.indices_by_gesture[gesture]))
            rng.shuffle(batch_indices)
            yield batch_indices

    def __len__(self) -> int:
        return self.steps_per_epoch


def _frame_to_array(frame_points) -> np.ndarray:
    if isinstance(frame_points, np.ndarray):
        return frame_points.astype(np.float32, copy=False)
    if not frame_points:
        return np.zeros((0, 5), dtype=np.float32)
    first = frame_points[0]
    if hasattr(first, "x"):
        return np.asarray(
            [[p.x, p.y, p.z, p.doppler, p.power] for p in frame_points],
            dtype=np.float32,
        )
    return np.asarray(frame_points, dtype=np.float32)


def collate_sequence_batch(batch: Sequence[Dict[str, object]]):
    batch_size = len(batch)
    frame_lists = [item["frames"] for item in batch]
    labels = torch.tensor([int(item["gesture"]) for item in batch], dtype=torch.long)

    computed_max_points = 1
    for sequence in frame_lists:
        for frame in sequence:
            computed_max_points = max(computed_max_points, len(frame))

    points = torch.zeros((batch_size, SEQUENCE_LENGTH, computed_max_points, 5), dtype=torch.float32)
    mask = torch.zeros((batch_size, SEQUENCE_LENGTH, computed_max_points), dtype=torch.bool)

    for batch_index, sequence in enumerate(frame_lists):
        for frame_index, frame_points in enumerate(sequence[:SEQUENCE_LENGTH]):
            frame_array = _frame_to_array(frame_points)
            point_count = min(len(frame_array), computed_max_points)
            if point_count == 0:
                continue
            points[batch_index, frame_index, :point_count] = torch.from_numpy(
                frame_array[:point_count]
            )
            mask[batch_index, frame_index, :point_count] = True

    return points, mask, labels


def _ensure_csv_header(path: Path, fieldnames: Sequence[str]):
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", newline='', encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()

def _append_csv_row(path: Path, row: Dict[str, object], fieldnames: Sequence[str]):
    with open(path, "a", newline='', encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writerow(row)

CSV_FIELDNAMES = [
    "timestamp",
    "epoch",
    "phase",
    "step",
    "step_in_epoch",
    "batch_index",
    "batch_size",
    "num_points_max",
    "batch_loss",
    "running_loss",
    "batch_acc",
    "running_acc",
    "num_samples_seen",
    "lr",
    "notes",
]

def find_preprocessed_dir() -> Path:
    for candidate in DEFAULT_PREPROCESSED_DIRS:
        if candidate.exists():
            return candidate

    raise FileNotFoundError(
        "Could not find a preprocessing output directory. Expected "
        f"{GESTURE_NET_DIR / 'dataset' / 'processed7-3'} "
        "(run scripts/preprocess_set7-3.py)."
    )


def build_loaders(
    preprocessed_dir: Path,
    batch_size: int = BATCH_SIZE,
    seed: int = 42,
    num_workers: int = NUM_WORKERS,
):
    train_dataset = PickleGestureDataset(preprocessed_dir / "train")
    val_dataset = PickleGestureDataset(preprocessed_dir / "validation")

    train_sampler = BalancedGestureBatchSampler(train_dataset, batch_size=batch_size, seed=seed)

    train_loader = DataLoader(
        train_dataset,
        batch_sampler=train_sampler,
        collate_fn=collate_sequence_batch,
        num_workers=num_workers,
        pin_memory=True,
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=batch_size,
        shuffle=False,
        collate_fn=collate_sequence_batch,
        num_workers=num_workers,
        pin_memory=True,
    )

    return train_loader, val_loader

def train_one_epoch(model, loader, optimizer, criterion, device, epoch: int, log_path: Optional[Path] = None):
    model.train()
    total_loss = 0.0
    total_correct = 0
    total_samples = 0

    pbar = tqdm(loader, desc=f"Epoch {epoch:03d} [Train]", leave=False)
    
    for step, (points, mask, labels) in enumerate(pbar):
        points = points.to(device)
        mask = mask.to(device)
        labels = labels.to(device)

        logits = model(points, mask)
        loss = criterion(logits, labels)
        
        scaled_loss = loss / ACCUMULATION_STEPS
        scaled_loss.backward()

        # 설정한 스텝 수에 도달했거나 마지막 배치일 때 가중치 업데이트
        if (step + 1) % ACCUMULATION_STEPS == 0 or (step + 1) == len(loader):
            optimizer.step()
            optimizer.zero_grad(set_to_none=True)

        batch_size = labels.size(0)
        # 통계 기록에는 스케일되지 않은 원래 loss 값 사용
        current_loss = float(loss.item())
        total_loss += current_loss * batch_size
        
        preds = logits.argmax(dim=1)
        total_correct += int((preds == labels).sum().item())
        total_samples += batch_size

        running_loss = total_loss / total_samples
        running_acc = total_correct / total_samples
        
        # 콘솔에 실시간 Loss 및 정확도 표시
        pbar.set_postfix({"loss": f"{running_loss:.4f}", "acc": f"{running_acc:.4f}"})

        # CSV logging per iteration (append mode)
        if log_path is not None:
            try:
                lr = None
                if optimizer is not None and len(optimizer.param_groups) > 0:
                    lr = float(optimizer.param_groups[0].get("lr", 0.0))

                batch_acc = float((preds == labels).sum().item()) / max(1, batch_size)
                row = {
                    "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "epoch": epoch,
                    "phase": "train",
                    "step": step,
                    "step_in_epoch": step + 1,
                    "batch_index": getattr(labels, "batch_index", None),
                    "batch_size": batch_size,
                    "num_points_max": points.shape[2] if hasattr(points, "shape") else None,
                    "batch_loss": current_loss,
                    "running_loss": running_loss,
                    "batch_acc": batch_acc,
                    "running_acc": running_acc,
                    "num_samples_seen": total_samples,
                    "lr": lr,
                    "notes": "",
                }
                _append_csv_row(LOG_TRAINING_CSV_PATH, row, CSV_FIELDNAMES)
            except Exception:
                # never fail training because of logging
                pass

    return {
        "loss": total_loss / max(1, total_samples),
        "accuracy": total_correct / max(1, total_samples),
    }

def evaluate(model, loader, criterion, device, mode="Val", log_path: Optional[Path] = None, epoch: Optional[int] = None):
    model.eval()
    total_loss = 0.0
    total_correct = 0
    total_samples = 0

    pbar = tqdm(loader, desc=f"[{mode}] Evaluating", leave=False)

    with torch.no_grad():
        for step, (points, mask, labels) in enumerate(pbar):
            points = points.to(device)
            mask = mask.to(device)
            labels = labels.to(device)

            logits = model(points, mask)
            loss = criterion(logits, labels)

            batch_size = labels.size(0)
            current_loss = float(loss.item())
            total_loss += current_loss * batch_size
            
            preds = logits.argmax(dim=1)
            total_correct += int((preds == labels).sum().item())
            total_samples += batch_size
            
            running_loss = total_loss / total_samples
            running_acc = total_correct / total_samples
            pbar.set_postfix({"loss": f"{running_loss:.4f}", "acc": f"{running_acc:.4f}"})

            # CSV logging per evaluation iteration
            if log_path is not None:
                try:
                    batch_size = labels.size(0)
                    preds = logits.argmax(dim=1)
                    batch_acc = float((preds == labels).sum().item()) / max(1, batch_size)
                    row = {
                        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                        "epoch": epoch,
                        "phase": mode,
                        "step": step,
                        "step_in_epoch": step + 1,
                        "batch_index": getattr(labels, "batch_index", None),
                        "batch_size": batch_size,
                        "num_points_max": points.shape[2] if hasattr(points, "shape") else None,
                        "batch_loss": current_loss,
                        "running_loss": running_loss,
                        "batch_acc": batch_acc,
                        "running_acc": running_acc,
                        "num_samples_seen": total_samples,
                        "lr": None,
                        "notes": "",
                    }
                    _append_csv_row(log_path, row, CSV_FIELDNAMES)
                except Exception:
                    pass

    return {
        "loss": total_loss / max(1, total_samples),
        "accuracy": total_correct / max(1, total_samples),
    }

def main() -> None:
    torch.manual_seed(SEED)
    random.seed(SEED)

    TRAIN_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    (TRAIN_OUTPUT_DIR / "logs").mkdir(parents=True, exist_ok=True)

    data_dir = DATA_DIR if DATA_DIR is not None else find_preprocessed_dir()
    print(f"Loading datasets from: {data_dir}")

    train_loader, val_loader = build_loaders(
        preprocessed_dir=data_dir,
        batch_size=BATCH_SIZE,
        seed=SEED,
        num_workers=NUM_WORKERS,
    )

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = GestureClassifierV5().to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LEARNING_RATE)
    criterion = nn.CrossEntropyLoss()

    print(f"Using device: {device} | DataLoader Workers: {NUM_WORKERS}")
    print(f"Physical Batch Size: {BATCH_SIZE} | Effective Batch Size: {BATCH_SIZE * ACCUMULATION_STEPS}")
    
    start_epoch = 1
    best_val_accuracy = -math.inf

    try:
        _ensure_csv_header(LOG_TRAINING_CSV_PATH, CSV_FIELDNAMES)
        _ensure_csv_header(LOG_VALIDATION_CSV_PATH, CSV_FIELDNAMES)
        _ensure_csv_header(LOG_SUMMARY_CSV_PATH, CSV_FIELDNAMES)
    except Exception:
        print(f"Warning: could not initialize CSV log at {LOG_TRAINING_CSV_PATH}")

    if RESUME_TRAINING and CHECKPOINT_PATH.exists():
        print(f"Resuming training from checkpoint: {CHECKPOINT_PATH}")
        checkpoint = torch.load(CHECKPOINT_PATH, map_location=device)
        model.load_state_dict(checkpoint["model_state_dict"])
        optimizer.load_state_dict(checkpoint["optimizer_state_dict"])
        start_epoch = checkpoint["epoch"] + 1
        if "best_val_accuracy" in checkpoint:
            best_val_accuracy = checkpoint["best_val_accuracy"]
        print(f"Resumed at epoch {start_epoch} with best val accuracy: {best_val_accuracy:.4f}")

    for epoch in range(start_epoch, EPOCHS + 1):
        # Training Loop
        train_metrics = train_one_epoch(model, train_loader, optimizer, criterion, device, epoch, log_path=LOG_TRAINING_CSV_PATH)

        # Evaluation Loop
        val_metrics = evaluate(model, val_loader, criterion, device, mode="Val", log_path=LOG_VALIDATION_CSV_PATH, epoch=epoch)

        # Epoch Summary Logging
        print(
            f"Epoch {epoch:03d} Completed | "
            f"Train Loss: {train_metrics['loss']:.4f}, Acc: {train_metrics['accuracy']:.4f} | "
            f"Val Loss: {val_metrics['loss']:.4f}, Acc: {val_metrics['accuracy']:.4f}"
        )

        checkpoint_data = {
            "epoch": epoch,
            "model_state_dict": model.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
            "best_val_accuracy": best_val_accuracy,
            "metrics": val_metrics,
        }
        torch.save(checkpoint_data, CHECKPOINT_PATH)

        try:
            summary_row = {
                "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "epoch": epoch,
                "phase": "epoch_summary",
                "step": None,
                "step_in_epoch": None,
                "batch_index": None,
                "batch_size": BATCH_SIZE,
                "num_points_max": None,
                "batch_loss": train_metrics.get("loss"),
                "running_loss": val_metrics.get("loss"),
                "batch_acc": train_metrics.get("accuracy"),
                "running_acc": val_metrics.get("accuracy"),
                "num_samples_seen": None,
                "lr": float(optimizer.param_groups[0].get("lr", 0.0)) if len(optimizer.param_groups) > 0 else None,
                "notes": "epoch_summary",
            }
            _append_csv_row(LOG_SUMMARY_CSV_PATH, summary_row, CSV_FIELDNAMES)
        except Exception:
            pass

        if val_metrics["accuracy"] > best_val_accuracy:
            best_val_accuracy = val_metrics["accuracy"]
            torch.save(model.state_dict(), BEST_MODEL_PATH)
            print(f"  -> ⭐ Saved new best model with Validation Accuracy: {best_val_accuracy:.4f}\n")
        else:
            print("") # 줄바꿈

    print("Training completed")

if __name__ == "__main__":
    main()