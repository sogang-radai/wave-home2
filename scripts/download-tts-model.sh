#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR="$ROOT/bin/models/tts/ko-kr"
ARCHIVE="sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2"
URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/$ARCHIVE"
TMP_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

mkdir -p "$DEST_DIR"

if command -v curl >/dev/null 2>&1; then
    curl -L -o "$TMP_DIR/$ARCHIVE" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$TMP_DIR/$ARCHIVE" "$URL"
else
    echo "error: curl or wget is required" >&2
    exit 1
fi

tar -xjf "$TMP_DIR/$ARCHIVE" -C "$DEST_DIR" --strip-components=1

echo "TTS model installed to $DEST_DIR"
