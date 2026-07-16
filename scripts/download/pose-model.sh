#!/usr/bin/env bash
# Installs YOLO11n-pose ncnn weights into bin/models/pose/.
#
# Priority:
#   1. Skip if yolo11n_pose.ncnn.{param,bin} already exist.
#   2. Download WAVE_POSE_MODEL_URL tarball when set (release artifact).
#   3. Fall back to scripts/build/pose-ncnn-model.sh:
#        - downloads yolo11n-pose.pt from Ultralytics assets (v8.3.0)
#        - exports to ncnn via ultralytics
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST_DIR="$ROOT/bin/models/pose"
PARAM_FILE="$DEST_DIR/yolo11n_pose.ncnn.param"
BIN_FILE="$DEST_DIR/yolo11n_pose.ncnn.bin"

# Official Ultralytics YOLO11n-pose PyTorch weights (used by build/pose-ncnn-model.sh).
export WAVE_POSE_PT_URL="${WAVE_POSE_PT_URL:-https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n-pose.pt}"

mkdir -p "$DEST_DIR"

if [[ -f "$PARAM_FILE" && -f "$BIN_FILE" ]]; then
    echo "Pose model already installed in $DEST_DIR"
    exit 0
fi

WAVE_POSE_MODEL_URL="${WAVE_POSE_MODEL_URL:-}"
WAVE_POSE_MODEL_ARCHIVE="${WAVE_POSE_MODEL_ARCHIVE:-wave-yolo11n-pose-ncnn.tar.bz2}"

if [[ -n "$WAVE_POSE_MODEL_URL" ]]; then
    TMP_DIR="$(mktemp -d)"
    cleanup() { rm -rf "$TMP_DIR"; }
    trap cleanup EXIT

    echo "==> Downloading pose model archive..."
    if command -v curl >/dev/null 2>&1; then
        curl -L -o "$TMP_DIR/$WAVE_POSE_MODEL_ARCHIVE" "$WAVE_POSE_MODEL_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$TMP_DIR/$WAVE_POSE_MODEL_ARCHIVE" "$WAVE_POSE_MODEL_URL"
    else
        echo "error: curl or wget is required" >&2
        exit 1
    fi

    tar -xjf "$TMP_DIR/$WAVE_POSE_MODEL_ARCHIVE" -C "$DEST_DIR" --strip-components=1
    echo "Pose model installed from archive to $DEST_DIR"
    exit 0
fi

echo "WAVE_POSE_MODEL_URL not set; building locally with Ultralytics..."
"$ROOT/scripts/build/pose-ncnn-model.sh"
