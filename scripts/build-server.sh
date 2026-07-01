#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib/cmake-build.sh"

wave_run_build wave-server

ROOT="$(wave_build_root)"
echo
echo "Built: $ROOT/bin/wave-server"
