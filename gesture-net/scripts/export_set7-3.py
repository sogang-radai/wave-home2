from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import torch

SCRIPT_DIR = Path(__file__).resolve().parent
GESTURE_NET_DIR = SCRIPT_DIR.parent
WORKSPACE_DIR = GESTURE_NET_DIR.parent
sys.path.insert(0, str(GESTURE_NET_DIR))

from networks.PointNetV1nCNNV5 import GestureClassifierV5, SEQUENCE_LENGTH  # noqa: E402

MODEL_TAG = "set7-3"
TRAIN_OUTPUT_DIR = GESTURE_NET_DIR / "training" / MODEL_TAG
MODELS_DIR = GESTURE_NET_DIR / "models" / MODEL_TAG
INPUT_PTH = TRAIN_OUTPUT_DIR / "best_model.pth"

_PNNX_CANDIDATES = [
    WORKSPACE_DIR / "thirdparty" / "pnnx" / "pnnx",
    WORKSPACE_DIR / "thirdparty" / "pnnx" / "pnnx.exe",
]


def find_pnnx() -> Path:
    for candidate in _PNNX_CANDIDATES:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "pnnx executable not found. Checked:\n  "
        + "\n  ".join(str(c) for c in _PNNX_CANDIDATES)
    )


def get_unique_base_name(directory: Path, name: str) -> str:
    target_param = directory / f"{name}.param"
    if not target_param.exists():
        return name

    counter = 1
    while (directory / f"{name}{counter}.param").exists():
        counter += 1
    return f"{name}{counter}"


def main() -> None:
    pt_dir = SCRIPT_DIR / "pt_models"
    models_dir = MODELS_DIR
    pnnx_exe = find_pnnx()
    model_src = INPUT_PTH

    pt_dir.mkdir(parents=True, exist_ok=True)
    models_dir.mkdir(parents=True, exist_ok=True)

    if not model_src.exists():
        print(f"Error: model not found: {model_src}")
        return

    print(f"Loading PyTorch model: {model_src}")
    device = torch.device("cpu")

    model = GestureClassifierV5()
    model.load_state_dict(torch.load(model_src, map_location=device, weights_only=True))
    model.eval()

    sub_models = [
        {
            "name": "pointnet",
            "module": model.frame_encoder,
            "dummy": torch.randn(1, 5, 200),
            "input_shape": "[1,5,200]",
        },
        {
            "name": "lstm",
            "module": model.temporal_head,
            "dummy": torch.randn(1, SEQUENCE_LENGTH, 256),
            "input_shape": f"[1,{SEQUENCE_LENGTH},256]",
        },
    ]

    for item in sub_models:
        name = item["name"]
        pt_path = pt_dir / f"{name}.pt"
        final_param = models_dir / f"{name}.param"
        final_bin = models_dir / f"{name}.bin"

        print(f"Tracing {name} to TorchScript...")
        traced_module = torch.jit.trace(item["module"], item["dummy"])
        traced_module.save(pt_path)

        print(f"Converting {name}.pt to ncnn via pnnx...")
        try:
            subprocess.run(
                [
                    str(pnnx_exe),
                    str(pt_path),
                    f"inputshape={item['input_shape']}",
                ],
                check=True,
                cwd=str(pt_dir),
            )

            pnnx_param = pt_dir / f"{name}.ncnn.param"
            pnnx_bin = pt_dir / f"{name}.ncnn.bin"

            if not pnnx_param.exists():
                print(f"Error: pnnx output missing: {pnnx_param}")
                continue

            unique_name = get_unique_base_name(models_dir, name)
            final_param = models_dir / f"{unique_name}.param"
            final_bin = models_dir / f"{unique_name}.bin"

            shutil.move(str(pnnx_param), str(final_param))
            shutil.move(str(pnnx_bin), str(final_bin))

            print(f"Success: Saved to {final_param} and {final_bin}")

        except subprocess.CalledProcessError as e:
            print(f"pnnx conversion failed for {name}: {e}")

        for extra in pt_dir.glob(f"{name}*"):
            if extra.name not in {final_param.name, final_bin.name}:
                try:
                    os.remove(extra)
                except OSError:
                    pass

    try:
        pt_dir.rmdir()
    except OSError:
        pass

    print("\nAll conversion tasks finished.")


if __name__ == "__main__":
    main()
