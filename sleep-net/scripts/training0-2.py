"""Train sleep-net0-2: LSTM heads, bed_window=100 (set0-1.pkl)."""

from __future__ import annotations

import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
SLEEP_NET_DIR = SCRIPT_DIR.parent
sys.path.insert(0, str(SLEEP_NET_DIR))
sys.path.insert(0, str(SCRIPT_DIR))

from _training_common import TrainConfig, run_training  # noqa: E402
from networks.PointNetV1nLSTMV1 import SleepNet  # noqa: E402

DATA_PKL = SLEEP_NET_DIR / "samples" / "set0-1.pkl"
TRAIN_OUTPUT_DIR = SLEEP_NET_DIR / "training" / "sleep-net0-2"

if __name__ == "__main__":
    run_training(TrainConfig(
        data_pkl=DATA_PKL,
        train_output_dir=TRAIN_OUTPUT_DIR,
        model_factory=SleepNet,
    ))
