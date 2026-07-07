#!/usr/bin/env bash
# Build lzav shared library for the current platform.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [[ "$(uname -m)" == "arm64" ]]; then
  OUT_DIR="bin/arm64"
  OUT_NAME="lzav_lib.dylib"
else
  OUT_DIR="bin/x64"
  OUT_NAME="lzav_lib.so"
fi

mkdir -p "$OUT_DIR"
clang++ -std=c++17 -O2 -fPIC -shared -DLZAV_LIB_EXPORTS \
  -o "$OUT_DIR/$OUT_NAME" lzav_lib.cpp -I.

echo "built $OUT_DIR/$OUT_NAME"
