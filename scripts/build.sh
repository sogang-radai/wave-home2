#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

if [[ "$(uname -s)" == "Darwin" ]]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required on macOS. See https://brew.sh" >&2
        exit 1
    fi
    for pkg in cmake jsoncpp libomp; do
        if ! brew list --formula "$pkg" >/dev/null 2>&1; then
            echo "Installing $pkg via Homebrew..."
            brew install "$pkg"
        fi
    done
    export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
fi

mkdir -p "$BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j"$JOBS"

echo
echo "Built:"
echo "  $ROOT/bin/wave-server"
echo "  $ROOT/bin/test/test-llm"
echo "  $ROOT/bin/test/test-ir"
