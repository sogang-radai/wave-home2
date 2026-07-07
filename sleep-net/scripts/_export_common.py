"""Shared export logic for sleep-net ONNX + ncnn + model.json."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Callable, List

import torch

EMBEDDING_DIM = 256
POINTNET_SAMPLE_POINTS = 256
ONNX_OPSET = 13

_PNNX_CANDIDATES = []


def init_pnnx_candidates(workspace_dir: Path) -> None:
    global _PNNX_CANDIDATES
    _PNNX_CANDIDATES = [
        workspace_dir / "thirdparty" / "pnnx" / "pnnx.exe",
        workspace_dir / "thirdparty" / "pnnx" / "pnnx",
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


def export_onnx(
    module: torch.nn.Module,
    dummy: torch.Tensor,
    onnx_path: Path,
    input_name: str,
    output_name: str,
    dynamic_axis: dict,
) -> None:
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

    base = onnx_path.stem
    ncnn_param = onnx_path.parent / f"{base}.ncnn.param"
    ncnn_bin = onnx_path.parent / f"{base}.ncnn.bin"
    if not (ncnn_param.exists() and ncnn_bin.exists()):
        log("  ERROR: expected ncnn outputs missing")
        return False
    return True


_PNNX_JUNK_SUFFIXES = [
    ".pnnx.param", ".pnnx.bin", ".pnnx.onnx", ".pnnxsim.onnx",
    "_ncnn.py", "_pnnx.py", ".pnnx.py",
]


def cleanup_pnnx_junk(models_dir: Path, base: str) -> None:
    for suffix in _PNNX_JUNK_SUFFIXES:
        junk = models_dir / f"{base}{suffix}"
        if junk.exists():
            junk.unlink()
    for extra in models_dir.glob(f"{base}.pnnx*"):
        if extra.exists():
            extra.unlink()


def write_model_json(
    path: Path,
    model_tag: str,
    model_version: str,
    bed_window: int,
    toss_window: int,
    temporal_type: str,
    bed_head_name: str,
    toss_head_name: str,
) -> None:
    doc = {
        "model_name": "SleepNet",
        "model_version": model_version,
        "model_tag": model_tag,
        "toss_active_status": 2,
        "frame_encoder": {
            "name": "sleep_pointnet",
            "type": "pointnet",
            "output_size": EMBEDDING_DIM,
            "input_name": "in0",
            "output_name": "out0",
            "param_path": "pointnet.ncnn.param",
            "bin_path": "pointnet.ncnn.bin",
            "normalization": {
                "coordinate_mode": "cartesian",
                "power_mode": "db",
                "ranges": {
                    "x": [-0.8, 0.8],
                    "y": [-1.0, 1.0],
                    "z": [-2.1, 0.0],
                    "doppler": [-2.5, 2.5],
                    "power": [25.0, 60.0],
                },
            },
        },
        "bed_model": {
            "labels": ["absent", "awake", "asleep"],
            "temporal_aggregator": {
                "name": bed_head_name,
                "type": temporal_type,
                "sequence_length": bed_window,
                "embedding_size": EMBEDDING_DIM,
                "output_size": 3,
                "input_name": "in0",
                "output_name": "out0",
                "param_path": f"{bed_head_name}.ncnn.param",
                "bin_path": f"{bed_head_name}.ncnn.bin",
            },
        },
        "toss_model": {
            "toss_weight_vector": [0.0, 0.5, 1.0],
            "labels": ["calm", "slight", "moderate"],
            "temporal_aggregator": {
                "name": toss_head_name,
                "type": temporal_type,
                "sequence_length": toss_window,
                "embedding_size": EMBEDDING_DIM,
                "output_size": 3,
                "input_name": "in0",
                "output_name": "out0",
                "param_path": f"{toss_head_name}.ncnn.param",
                "bin_path": f"{toss_head_name}.ncnn.bin",
            },
        },
    }
    path.write_text(json.dumps(doc, indent=4) + "\n", encoding="utf-8")
    log(f"  wrote {path.name}")


def run_export(
    model_tag: str,
    model_version: str,
    train_output_dir: Path,
    models_dir: Path,
    model_factory: Callable[[], torch.nn.Module],
    bed_window: int,
    toss_window: int,
    temporal_type: str,
    bed_head_export_name: str,
    toss_head_export_name: str,
) -> None:
    input_pth = train_output_dir / "best_model.pth"

    log("=" * 70)
    log(f"Exporting SleepNet '{model_tag}' -> ONNX + ncnn (3 sub-models)")
    log("=" * 70)

    if not input_pth.exists():
        log(f"ERROR: trained model not found: {input_pth}")
        return

    pnnx_exe = find_pnnx()
    log(f"pnnx   : {pnnx_exe}")
    log(f"input  : {input_pth}")
    log(f"output : {models_dir}")
    log()

    models_dir.mkdir(parents=True, exist_ok=True)

    model = model_factory()
    model.load_state_dict(torch.load(input_pth, map_location="cpu", weights_only=True))
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
            "name": bed_head_export_name,
            "module": model.head_status,
            "dummy": torch.randn(1, bed_window, EMBEDDING_DIM),
            "input_name": "embeddings",
            "output_name": "logits",
            "dynamic_axis": {1: "seq"},
            "input_shape": f"[1,{bed_window},{EMBEDDING_DIM}]",
        },
        {
            "name": toss_head_export_name,
            "module": model.head_toss,
            "dummy": torch.randn(1, toss_window, EMBEDDING_DIM),
            "input_name": "embeddings",
            "output_name": "logits",
            "dynamic_axis": {1: "seq"},
            "input_shape": f"[1,{toss_window},{EMBEDDING_DIM}]",
        },
    ]

    succeeded: List[str] = []
    failed: List[str] = []
    for item in sub_models:
        name = item["name"]
        log(f"[{name}]")
        onnx_path = models_dir / f"{name}.onnx"

        with torch.no_grad():
            export_onnx(
                item["module"], item["dummy"], onnx_path,
                item["input_name"], item["output_name"], item["dynamic_axis"],
            )

        ok = convert_to_ncnn(pnnx_exe, onnx_path, item["input_shape"])
        cleanup_pnnx_junk(models_dir, name)

        if ok:
            log(f"  OK: {name}.onnx + {name}.ncnn.param/.bin")
            succeeded.append(name)
        else:
            failed.append(name)
        log()

    write_model_json(
        models_dir / "model.json",
        model_tag=model_tag,
        model_version=model_version,
        bed_window=bed_window,
        toss_window=toss_window,
        temporal_type=temporal_type,
        bed_head_name=bed_head_export_name,
        toss_head_name=toss_head_export_name,
    )

    log("=" * 70)
    log(f"Done. succeeded={succeeded} failed={failed}")
    log(f"Artifacts in: {models_dir}")
