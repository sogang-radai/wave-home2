from __future__ import annotations

import csv
import datetime
import pickle
import random
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, Dataset, Sampler
from tqdm import tqdm

import sys

# ============================================================================
# CONFIG  (edit these for Colab: point DATA_PKL / OUTPUT to your Drive path)
# ============================================================================

DATA_PKL = Path("/content/drive/MyDrive/sleep-net/samples/set0-0.pkl")
TRAIN_OUTPUT_DIR = Path("/content/drive/MyDrive/sleep-net/training/sleep-net0-0")

# Batch sizes per head (they are sampled independently each step).
BED_BATCH: int = 16
TOSS_BATCH: int = 16
EVAL_BATCH: int = 16

EPOCHS: int = 50
LEARNING_RATE: float = 1e-3
WEIGHT_DECAY: float = 0.0
LAMBDA_TOSS: float = 0.5          # weight of toss loss relative to status loss
SEED: int = 42
NUM_WORKERS: int = 2              # set 0 if RAM is tight (frame pool is large)

RESUME_TRAINING: bool = False
SAVE_EPOCH_MODELS: bool = True   # keep a checkpoint for every epoch (disk heavy)

CHECKPOINT_PATH: Path = TRAIN_OUTPUT_DIR / "checkpoint.pth"
BEST_MODEL_PATH: Path = TRAIN_OUTPUT_DIR / "best_model.pth"
LOG_DIR: Path = TRAIN_OUTPUT_DIR / "logs"
SUMMARY_CSV_PATH: Path = LOG_DIR / "summary_log.csv"
CM_DIR: Path = LOG_DIR / "confusion_matrices"


# ============================================================================
# Dataset
# ============================================================================

class WindowDataset(Dataset):
    """Materializes fixed-length frame windows from the shared frame pool."""

    def __init__(self, frames: List[np.ndarray], samples: List[dict], split: str):
        self.frames = frames
        self.samples = [s for s in samples if s["split"] == split]
        self.indices_by_class: Dict[int, List[int]] = defaultdict(list)
        for local_idx, sample in enumerate(self.samples):
            self.indices_by_class[sample["label"]].append(local_idx)

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int) -> Dict[str, object]:
        sample = self.samples[index]
        frame_list = [self.frames[frame_idx] for frame_idx in sample["frame_indices"]]
        return {"frames": frame_list, "label": sample["label"]}


def make_collate(seq_len: int):
    def collate(batch: Sequence[Dict[str, object]]):
        batch_size = len(batch)
        max_points = 0
        for item in batch:
            for frame in item["frames"]:
                max_points = max(max_points, frame.shape[0])
        max_points = max(max_points, 1)

        points = torch.zeros((batch_size, seq_len, max_points, 5), dtype=torch.float32)
        mask = torch.zeros((batch_size, seq_len, max_points), dtype=torch.bool)
        labels = torch.tensor([int(item["label"]) for item in batch], dtype=torch.long)

        for b, item in enumerate(batch):
            for t, frame in enumerate(item["frames"]):
                if t >= seq_len:
                    break
                n = frame.shape[0]
                if n == 0:
                    continue
                points[b, t, :n] = torch.from_numpy(frame)
                mask[b, t, :n] = True

        return points, mask, labels

    return collate


class BalancedBatchSampler(Sampler[List[int]]):
    """Uniformly samples a class, then a random sample of that class."""

    def __init__(
        self,
        indices_by_class: Dict[int, List[int]],
        batch_size: int,
        steps_per_epoch: int,
        seed: int = 42,
    ) -> None:
        self.indices_by_class = indices_by_class
        self.classes = [c for c, idxs in indices_by_class.items() if idxs]
        self.batch_size = batch_size
        self.steps_per_epoch = max(1, steps_per_epoch)
        self.seed = seed
        self._epoch = 0

    def set_epoch(self, epoch: int) -> None:
        self._epoch = epoch

    def __iter__(self) -> Iterator[List[int]]:
        rng = random.Random(self.seed + self._epoch)
        for _ in range(self.steps_per_epoch):
            batch: List[int] = []
            for _ in range(self.batch_size):
                cls = rng.choice(self.classes)
                batch.append(rng.choice(self.indices_by_class[cls]))
            yield batch

    def __len__(self) -> int:
        return self.steps_per_epoch


# ============================================================================
# Data loading
# ============================================================================

class SleepData:
    def __init__(self, pkl_path: Path):
        print(f"Loading dataset: {pkl_path}")
        with open(pkl_path, "rb") as f:
            raw = pickle.load(f)

        self.meta = raw["meta"]
        self.frames = raw["frames"]
        self.bed_samples = raw["bed_samples"]
        self.toss_samples = raw["toss_samples"]

        self.bed_window = int(self.meta["bed_window"])
        self.toss_window = int(self.meta["toss_window"])
        self.bed_class_count = int(self.meta["bed_class_count"])
        self.toss_class_count = int(self.meta["toss_class_count"])

        print(
            f"  frames={len(self.frames)} bed_samples={len(self.bed_samples)} "
            f"toss_samples={len(self.toss_samples)} "
            f"bed_window={self.bed_window} toss_window={self.toss_window}"
        )


def build_loaders(data: SleepData, seed: int) -> Dict[str, DataLoader]:
    bed_train = WindowDataset(data.frames, data.bed_samples, "train")
    bed_val = WindowDataset(data.frames, data.bed_samples, "val")
    toss_train = WindowDataset(data.frames, data.toss_samples, "train")
    toss_val = WindowDataset(data.frames, data.toss_samples, "val")

    steps_per_epoch = max(1, len(bed_train) // BED_BATCH)
    print(f"  steps_per_epoch={steps_per_epoch}")

    bed_collate = make_collate(data.bed_window)
    toss_collate = make_collate(data.toss_window)

    bed_train_loader = DataLoader(
        bed_train,
        batch_sampler=BalancedBatchSampler(bed_train.indices_by_class, BED_BATCH, steps_per_epoch, seed),
        collate_fn=bed_collate,
        num_workers=NUM_WORKERS,
        pin_memory=True,
    )
    toss_train_loader = DataLoader(
        toss_train,
        batch_sampler=BalancedBatchSampler(toss_train.indices_by_class, TOSS_BATCH, steps_per_epoch, seed + 1),
        collate_fn=toss_collate,
        num_workers=NUM_WORKERS,
        pin_memory=True,
    )
    bed_val_loader = DataLoader(
        bed_val, batch_size=EVAL_BATCH, shuffle=False,
        collate_fn=bed_collate, num_workers=NUM_WORKERS, pin_memory=True,
    )
    toss_val_loader = DataLoader(
        toss_val, batch_size=EVAL_BATCH, shuffle=False,
        collate_fn=toss_collate, num_workers=NUM_WORKERS, pin_memory=True,
    )

    return {
        "bed_train": bed_train_loader,
        "toss_train": toss_train_loader,
        "bed_val": bed_val_loader,
        "toss_val": toss_val_loader,
        "steps_per_epoch": steps_per_epoch,
    }


# ============================================================================
# Model helpers
# ============================================================================

def encode_sequence(encoder: nn.Module, points: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    """(B, S, P, 5) + (B, S, P) -> per-frame embeddings (B, S, EMBEDDING_DIM)."""
    batch_size, seq_len, max_points, features = points.shape
    flat_points = points.view(batch_size * seq_len, max_points, features).transpose(1, 2).contiguous()
    flat_mask = mask.view(batch_size * seq_len, max_points)
    embeddings = encoder(flat_points, flat_mask)
    return embeddings.view(batch_size, seq_len, -1)


def head_logits(model: SleepNet, head: nn.Module, points: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    embeddings = encode_sequence(model.frame_encoder, points, mask)
    return head(embeddings)


# ============================================================================
# Train / eval
# ============================================================================

def train_one_epoch(model, loaders, optimizer, criterion, device, epoch) -> Dict[str, float]:
    model.train()
    totals = {"loss": 0.0, "status_correct": 0, "toss_correct": 0, "count": 0}

    steps = loaders["steps_per_epoch"]
    pbar = tqdm(
        zip(loaders["bed_train"], loaders["toss_train"]),
        total=steps, desc=f"Epoch {epoch:03d} [Train]", leave=False,
    )

    for (bed_points, bed_mask, bed_labels), (toss_points, toss_mask, toss_labels) in pbar:
        bed_points, bed_mask, bed_labels = bed_points.to(device), bed_mask.to(device), bed_labels.to(device)
        toss_points, toss_mask, toss_labels = toss_points.to(device), toss_mask.to(device), toss_labels.to(device)

        status_logits = head_logits(model, model.head_status, bed_points, bed_mask)
        toss_logits = head_logits(model, model.head_toss, toss_points, toss_mask)

        loss_status = criterion(status_logits, bed_labels)
        loss_toss = criterion(toss_logits, toss_labels)
        loss = loss_status + LAMBDA_TOSS * loss_toss

        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        optimizer.step()

        totals["loss"] += float(loss.item())
        totals["count"] += 1
        totals["status_correct"] += int((status_logits.argmax(1) == bed_labels).sum().item()) / bed_labels.size(0)
        totals["toss_correct"] += int((toss_logits.argmax(1) == toss_labels).sum().item()) / toss_labels.size(0)

        pbar.set_postfix({
            "loss": f"{totals['loss'] / totals['count']:.3f}",
            "bed_acc": f"{totals['status_correct'] / totals['count']:.3f}",
            "toss_acc": f"{totals['toss_correct'] / totals['count']:.3f}",
        })

    n = max(1, totals["count"])
    return {
        "loss": totals["loss"] / n,
        "bed_acc": totals["status_correct"] / n,
        "toss_acc": totals["toss_correct"] / n,
    }


@torch.no_grad()
def evaluate_head(model, head, loader, criterion, device, desc) -> Dict[str, object]:
    model.eval()
    total_loss, total_correct, total = 0.0, 0, 0
    all_labels: List[int] = []
    all_preds: List[int] = []

    for points, mask, labels in tqdm(loader, desc=desc, leave=False):
        points, mask, labels = points.to(device), mask.to(device), labels.to(device)
        logits = head_logits(model, head, points, mask)
        loss = criterion(logits, labels)

        batch_size = labels.size(0)
        total_loss += float(loss.item()) * batch_size
        preds = logits.argmax(1)
        total_correct += int((preds == labels).sum().item())
        total += batch_size
        all_labels.extend(labels.cpu().tolist())
        all_preds.extend(preds.cpu().tolist())

    return {
        "loss": total_loss / max(1, total),
        "acc": total_correct / max(1, total),
        "labels": all_labels,
        "preds": all_preds,
    }


# ============================================================================
# Logging
# ============================================================================

SUMMARY_FIELDS = [
    "timestamp", "epoch", "lr",
    "train_loss", "train_bed_acc", "train_toss_acc",
    "val_bed_loss", "val_bed_acc", "val_toss_loss", "val_toss_acc", "val_mean_acc",
]


def ensure_csv_header(path: Path, fields: Sequence[str]) -> None:
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", newline="", encoding="utf-8") as f:
            csv.DictWriter(f, fieldnames=fields).writeheader()


def append_csv_row(path: Path, row: Dict[str, object], fields: Sequence[str]) -> None:
    with open(path, "a", newline="", encoding="utf-8") as f:
        csv.DictWriter(f, fieldnames=fields).writerow(row)


def save_confusion_matrix(labels: List[int], preds: List[int], class_count: int, path: Path) -> None:
    matrix = np.zeros((class_count, class_count), dtype=np.int64)
    for true_label, pred_label in zip(labels, preds):
        if 0 <= true_label < class_count and 0 <= pred_label < class_count:
            matrix[true_label, pred_label] += 1

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["True \\ Pred"] + [f"Class_{i}" for i in range(class_count)])
        for i, row in enumerate(matrix):
            writer.writerow([f"Class_{i}"] + row.tolist())


# ============================================================================
# Main
# ============================================================================

def main() -> None:
    torch.manual_seed(SEED)
    random.seed(SEED)
    np.random.seed(SEED)

    TRAIN_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    CM_DIR.mkdir(parents=True, exist_ok=True)

    if not DATA_PKL.exists():
        raise FileNotFoundError(f"Dataset not found: {DATA_PKL}")

    data = SleepData(DATA_PKL)
    loaders = build_loaders(data, SEED)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = SleepNet().to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LEARNING_RATE, weight_decay=WEIGHT_DECAY)
    criterion = nn.CrossEntropyLoss()

    print(f"Device: {device} | LAMBDA_TOSS={LAMBDA_TOSS} | workers={NUM_WORKERS}")

    start_epoch = 1
    best_mean_acc = -1.0

    if RESUME_TRAINING and CHECKPOINT_PATH.exists():
        print(f"Resuming from {CHECKPOINT_PATH}")
        checkpoint = torch.load(CHECKPOINT_PATH, map_location=device)
        model.load_state_dict(checkpoint["model_state_dict"])
        optimizer.load_state_dict(checkpoint["optimizer_state_dict"])
        start_epoch = checkpoint["epoch"] + 1
        best_mean_acc = checkpoint.get("best_mean_acc", -1.0)

    ensure_csv_header(SUMMARY_CSV_PATH, SUMMARY_FIELDS)

    for epoch in range(start_epoch, EPOCHS + 1):
        for key in ("bed_train", "toss_train"):
            loaders[key].batch_sampler.set_epoch(epoch)

        train_metrics = train_one_epoch(model, loaders, optimizer, criterion, device, epoch)
        bed_eval = evaluate_head(model, model.head_status, loaders["bed_val"], criterion, device, "[Val bed]")
        toss_eval = evaluate_head(model, model.head_toss, loaders["toss_val"], criterion, device, "[Val toss]")
        mean_acc = (bed_eval["acc"] + toss_eval["acc"]) / 2.0

        print(
            f"Epoch {epoch:03d} | train loss {train_metrics['loss']:.4f} "
            f"(bed {train_metrics['bed_acc']:.3f} / toss {train_metrics['toss_acc']:.3f}) | "
            f"val bed {bed_eval['acc']:.3f} / toss {toss_eval['acc']:.3f} | mean {mean_acc:.3f}"
        )

        save_confusion_matrix(bed_eval["labels"], bed_eval["preds"], data.bed_class_count,
                              CM_DIR / f"bed_epoch_{epoch:03d}.csv")
        save_confusion_matrix(toss_eval["labels"], toss_eval["preds"], data.toss_class_count,
                              CM_DIR / f"toss_epoch_{epoch:03d}.csv")

        append_csv_row(SUMMARY_CSV_PATH, {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "epoch": epoch,
            "lr": optimizer.param_groups[0]["lr"],
            "train_loss": train_metrics["loss"],
            "train_bed_acc": train_metrics["bed_acc"],
            "train_toss_acc": train_metrics["toss_acc"],
            "val_bed_loss": bed_eval["loss"],
            "val_bed_acc": bed_eval["acc"],
            "val_toss_loss": toss_eval["loss"],
            "val_toss_acc": toss_eval["acc"],
            "val_mean_acc": mean_acc,
        }, SUMMARY_FIELDS)

        torch.save({
            "epoch": epoch,
            "model_state_dict": model.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
            "best_mean_acc": best_mean_acc,
        }, CHECKPOINT_PATH)

        if SAVE_EPOCH_MODELS:
            torch.save(model.state_dict(), TRAIN_OUTPUT_DIR / f"model_{epoch:03d}.pth")

        if mean_acc > best_mean_acc:
            best_mean_acc = mean_acc
            torch.save(model.state_dict(), BEST_MODEL_PATH)
            print(f"  -> saved best model (mean_acc={best_mean_acc:.4f})")

    print("Training completed.")


if __name__ == "__main__":
    main()
