#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/../_lib/cmake-build.sh"

wave_run_build test-ir

ROOT="$(wave_build_root)"
echo
echo "Built: $ROOT/bin/test/test-ir"
