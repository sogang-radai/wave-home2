"""Export sleep-net0-2 (LSTM, bed=100, toss=40) to ONNX + ncnn."""

from __future__ import annotations

import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
SLEEP_NET_DIR = SCRIPT_DIR.parent
WORKSPACE_DIR = SLEEP_NET_DIR.parent
sys.path.insert(0, str(SLEEP_NET_DIR))
sys.path.insert(0, str(SCRIPT_DIR))

from _export_common import init_pnnx_candidates, run_export  # noqa: E402
from networks.PointNetV1nLSTMV1 import SleepNet  # noqa: E402

MODEL_TAG = "sleep-net0-2"
TRAIN_OUTPUT_DIR = SLEEP_NET_DIR / "training" / MODEL_TAG
MODELS_DIR = SLEEP_NET_DIR / "models" / MODEL_TAG

BED_WINDOW = 100
TOSS_WINDOW = 40

if __name__ == "__main__":
    init_pnnx_candidates(WORKSPACE_DIR)
    run_export(
        model_tag=MODEL_TAG,
        model_version="0.2",
        train_output_dir=TRAIN_OUTPUT_DIR,
        models_dir=MODELS_DIR,
        model_factory=SleepNet,
        bed_window=BED_WINDOW,
        toss_window=TOSS_WINDOW,
        temporal_type="lstm",
        bed_head_export_name="bed_lstm",
        toss_head_export_name="toss_lstm",
    )
