#!/usr/bin/env bash
# Copy GLTF house model into CRA public/ for static serving.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/wave-home-front/demo"
DEST="$ROOT/wave-home-front/public/models"
mkdir -p "$DEST"
cp -f "$SRC/model_house.gltf" "$SRC/model_house.bin" "$DEST/"
echo "Copied model_house.gltf + .bin → $DEST"
