#!/usr/bin/env bash
# Converts Ultralytics yolo11n-pose.pt to ncnn weights under bin/models/pose/.
#
# Weights: https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-pose.pt
# Requires: python3, pip package ultralytics (pulls pnnx for ncnn export).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR="$ROOT/bin/models/pose"
EXPORT_DIR="$DEST_DIR/.export_work"
IMGSZ="${WAVE_POSE_IMGSZ:-640}"

# Official Ultralytics YOLO11n-pose release (v8.3.0).
WAVE_POSE_PT_URL="${WAVE_POSE_PT_URL:-https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-pose.pt}"
WAVE_POSE_PT_FILE="${WAVE_POSE_PT_FILE:-yolo11n-pose.pt}"

mkdir -p "$DEST_DIR"
rm -rf "$EXPORT_DIR"
mkdir -p "$EXPORT_DIR"

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required" >&2
    exit 1
fi

VENV_DIR="${WAVE_POSE_VENV_DIR:-$ROOT/.deps/pose-export-venv}"
if [[ ! -d "$VENV_DIR" ]]; then
    echo "==> Creating pose export venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
if ! python3 -c "import ultralytics" >/dev/null 2>&1; then
    echo "==> Installing ultralytics into pose export venv..."
    python3 -m pip install -q --upgrade pip
    python3 -m pip install -q ultralytics
fi

PT_PATH="$EXPORT_DIR/$WAVE_POSE_PT_FILE"
if [[ ! -f "$PT_PATH" ]]; then
    echo "==> Downloading ${WAVE_POSE_PT_FILE} from Ultralytics..."
    if command -v curl >/dev/null 2>&1; then
        curl -L -o "$PT_PATH" "$WAVE_POSE_PT_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$PT_PATH" "$WAVE_POSE_PT_URL"
    else
        echo "error: curl or wget is required" >&2
        exit 1
    fi
fi

export WAVE_POSE_DEST_DIR="$DEST_DIR"
export WAVE_POSE_EXPORT_DIR="$EXPORT_DIR"
export WAVE_POSE_IMGSZ="$IMGSZ"
export WAVE_POSE_PT_PATH="$PT_PATH"

echo "==> Exporting ${WAVE_POSE_PT_FILE} to ncnn (imgsz=${IMGSZ})..."
python3 - <<'PY'
import os
import shutil
import sys
from pathlib import Path

dest = Path(os.environ["WAVE_POSE_DEST_DIR"])
work = Path(os.environ["WAVE_POSE_EXPORT_DIR"])
imgsz = int(os.environ.get("WAVE_POSE_IMGSZ", "640"))
pt_path = Path(os.environ["WAVE_POSE_PT_PATH"])

try:
    from ultralytics import YOLO
except ImportError:
    print("error: install ultralytics: pip install ultralytics", file=sys.stderr)
    sys.exit(1)

if not pt_path.exists():
    print(f"error: weights not found: {pt_path}", file=sys.stderr)
    sys.exit(1)

print(f"==> Loading {pt_path}...")
model = YOLO(str(pt_path))

out_dir = work / f"{pt_path.stem}_ncnn_model"
if out_dir.exists():
    shutil.rmtree(out_dir)

model.export(format="ncnn", imgsz=imgsz, batch=1, device="cpu", optimize=False)

if not out_dir.exists():
    print(f"error: export did not create {out_dir}", file=sys.stderr)
    sys.exit(1)

param_src = out_dir / "model.ncnn.param"
bin_src = out_dir / "model.ncnn.bin"
if not param_src.exists() or not bin_src.exists():
    print("error: model.ncnn.param / model.ncnn.bin not found in export output", file=sys.stderr)
    sys.exit(1)

param_dst = dest / "yolo11n_pose.ncnn.param"
bin_dst = dest / "yolo11n_pose.ncnn.bin"
shutil.copy2(param_src, param_dst)
shutil.copy2(bin_src, bin_dst)

metadata = out_dir / "metadata.yaml"
if metadata.exists():
    shutil.copy2(metadata, dest / "metadata.yaml")

# Keep a copy of source weights for traceability.
weights_dst = dest / pt_path.name
if not weights_dst.exists():
    shutil.copy2(pt_path, weights_dst)

print(f"==> Installed {param_dst.name} and {bin_dst.name}")
PY

rm -rf "$EXPORT_DIR"

if [[ ! -f "$DEST_DIR/yolo11n_pose.ncnn.param" || ! -f "$DEST_DIR/yolo11n_pose.ncnn.bin" ]]; then
    echo "error: ncnn model files missing under $DEST_DIR" >&2
    exit 1
fi

echo "Pose ncnn model ready in $DEST_DIR"
