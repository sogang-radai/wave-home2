#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v cloc >/dev/null 2>&1; then
    echo "error: cloc is not installed" >&2
    echo "  macOS: brew install cloc" >&2
    exit 1
fi

# Backend source roots (frontend, thirdparty, and resource assets are excluded).
BACKEND_PATHS=(
    "$ROOT/src/wave-server"
    "$ROOT/src/common"
    "$ROOT/src/r4sn"
    "$ROOT/src/test"
    "$ROOT/bed_net"
    "$ROOT/cmake"
    "$ROOT/scripts"
)

EXCLUDE_DIR=(
    thirdparty
    wave-home-front
    site
    build
    bin
    .git
    .cache
    resource
    thumbnails
    model
    models
    node_modules
    __pycache__
    .venv
)

exclude_arg="$(IFS=,; echo "${EXCLUDE_DIR[*]}")"

echo "Backend code statistics (excluding frontend, thirdparty, and resource assets)"
echo "  roots: ${BACKEND_PATHS[*]#$ROOT/}"
echo

cloc \
    --vcs=git \
    --exclude-dir="$exclude_arg" \
    "${BACKEND_PATHS[@]}" \
    "$@"
