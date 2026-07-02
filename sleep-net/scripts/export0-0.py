"""
Export a trained SleepNet into three separate, independently accelerated models:

  1. pointnet   : PointNet frame encoder   (1, 5, P)      -> (1, 256)
  2. bed_cnn    : status head (bed-net)     (1, S_bed, 256)  -> (1, 3)
  3. toss_cnn   : toss head   (toss-net)    (1, S_toss, 256) -> (1, 3)

Pipeline per sub-model:  torch  ->  ONNX (kept)  ->  ncnn (via pnnx)

The intermediate ONNX files are NOT discarded; both the .onnx and the ncnn
(.param/.bin) are written to MODELS_DIR. pnnx byproducts are cleaned up.

Input : TRAIN_OUTPUT_DIR/best_model.pth   (from scripts/training0-0.py)
Output: MODELS_DIR/{name}.onnx, {name}.ncnn.param, {name}.ncnn.bin
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import torch

SCRIPT_DIR = Path(__file__).resolve().parent
SLEEP_NET_DIR = SCRIPT_DIR.parent
WORKSPACE_DIR = SLEEP_NET_DIR.parent
sys.path.insert(0, str(SLEEP_NET_DIR))

from networks.PointNetV1nCNNV1 import SleepNet, EMBEDDING_DIM  # noqa: E402

# ============================================================================
# CONFIG
# ============================================================================

MODEL_TAG = "sleep-net0-0"
TRAIN_OUTPUT_DIR: Path = SLEEP_NET_DIR / "training" / MODEL_TAG
MODELS_DIR: Path = SLEEP_NET_DIR / "models" / MODEL_TAG
INPUT_PTH: Path = TRAIN_OUTPUT_DIR / "best_model.pth"

# Representative window lengths (must match preprocessing; heads use adaptive
# pooling so the exported ncnn still accepts variable-length sequences).
BED_WINDOW = 160   # ~8 s @ 20 fps
TOSS_WINDOW = 40   # ~2 s @ 20 fps
POINTNET_SAMPLE_POINTS = 256  # representative point count for tracing

ONNX_OPSET = 13

_PNNX_CANDIDATES = [
    WORKSPACE_DIR / "thirdparty" / "pnnx" / "pnnx.exe",
    WORKSPACE_DIR / "thirdparty" / "pnnx" / "pnnx",
]

# pnnx byproducts to delete (we keep only .onnx and .ncnn.param/.bin)
_PNNX_JUNK_SUFFIXES = [
    ".pnnx.param", ".pnnx.bin", ".pnnx.onnx", ".pnnxsim.onnx",
    "_ncnn.py", "_pnnx.py", ".pnnx.py",
]


def log(message: str = "") -> None:
    print(message, flush=True)


def find_pnnx() -> Path:
    for candidate in _PNNX_CANDIDATES:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "pnnx executable not found. Checked:\n  "
        + "\n  ".join(str(c) for c in _PNNX_CANDIDATES)
    )


def export_onnx(module: torch.nn.Module, dummy: torch.Tensor, onnx_path: Path,
                input_name: str, output_name: str, dynamic_axis: dict) -> None:
    log(f"  exporting ONNX -> {onnx_path.name}")
    torch.onnx.export(
        module,
        dummy,
        str(onnx_path),
        input_names=[input_name],
        output_names=[output_name],
        dynamic_axes={input_name: dynamic_axis},
        opset_version=ONNX_OPSET,
        dynamo=False,
    )


def convert_to_ncnn(pnnx_exe: Path, onnx_path: Path, input_shape: str) -> bool:
    log(f"  converting to ncnn via pnnx (inputshape={input_shape})")
    try:
        subprocess.run(
            [str(pnnx_exe), onnx_path.name, f"inputshape={input_shape}"],
            check=True,
            cwd=str(onnx_path.parent),
        )
    except subprocess.CalledProcessError as exc:
        log(f"  ERROR: pnnx failed: {exc}")
        return False

    base = onnx_path.stem  # e.g. "pointnet"
    ncnn_param = onnx_path.parent / f"{base}.ncnn.param"
    ncnn_bin = onnx_path.parent / f"{base}.ncnn.bin"
    if not (ncnn_param.exists() and ncnn_bin.exists()):
        log("  ERROR: expected ncnn outputs missing")
        return False
    return True


def cleanup_pnnx_junk(models_dir: Path, base: str) -> None:
    for suffix in _PNNX_JUNK_SUFFIXES:
        junk = models_dir / f"{base}{suffix}"
        if junk.exists():
            junk.unlink()
    # pnnx also emits a debug bin/param in some versions
    for extra in models_dir.glob(f"{base}.pnnx*"):
        if extra.exists():
            extra.unlink()


def main() -> None:
    log("=" * 70)
    log(f"Exporting SleepNet '{MODEL_TAG}' -> ONNX + ncnn (3 sub-models)")
    log("=" * 70)

    if not INPUT_PTH.exists():
        log(f"ERROR: trained model not found: {INPUT_PTH}")
        return

    pnnx_exe = find_pnnx()
    log(f"pnnx   : {pnnx_exe}")
    log(f"input  : {INPUT_PTH}")
    log(f"output : {MODELS_DIR}")
    log()

    MODELS_DIR.mkdir(parents=True, exist_ok=True)

    device = torch.device("cpu")
    model = SleepNet()
    model.load_state_dict(torch.load(INPUT_PTH, map_location=device, weights_only=True))
    model.eval()

    sub_models = [
        {
            "name": "pointnet",
            "module": model.frame_encoder,
            "dummy": torch.randn(1, 5, POINTNET_SAMPLE_POINTS),
            "input_name": "points",
            "output_name": "embedding",
            "dynamic_axis": {2: "num_points"},
            "input_shape": f"[1,5,{POINTNET_SAMPLE_POINTS}]",
        },
        {
            "name": "bed_cnn",
            "module": model.head_status,
            "dummy": torch.randn(1, BED_WINDOW, EMBEDDING_DIM),
            "input_name": "embeddings",
            "output_name": "logits",
            "dynamic_axis": {1: "seq"},
            "input_shape": f"[1,{BED_WINDOW},{EMBEDDING_DIM}]",
        },
        {
            "name": "toss_cnn",
            "module": model.head_toss,
            "dummy": torch.randn(1, TOSS_WINDOW, EMBEDDING_DIM),
            "input_name": "embeddings",
            "output_name": "logits",
            "dynamic_axis": {1: "seq"},
            "input_shape": f"[1,{TOSS_WINDOW},{EMBEDDING_DIM}]",
        },
    ]

    succeeded, failed = [], []
    for item in sub_models:
        name = item["name"]
        log(f"[{name}]")
        onnx_path = MODELS_DIR / f"{name}.onnx"

        with torch.no_grad():
            export_onnx(
                item["module"], item["dummy"], onnx_path,
                item["input_name"], item["output_name"], item["dynamic_axis"],
            )

        ok = convert_to_ncnn(pnnx_exe, onnx_path, item["input_shape"])
        cleanup_pnnx_junk(MODELS_DIR, name)

        if ok:
            log(f"  OK: {name}.onnx + {name}.ncnn.param/.bin")
            succeeded.append(name)
        else:
            failed.append(name)
        log()

    log("=" * 70)
    log(f"Done. succeeded={succeeded} failed={failed}")
    log(f"Artifacts in: {MODELS_DIR}")


if __name__ == "__main__":
    main()
