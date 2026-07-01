#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")/.." && pwd)/lib/cmake-build.sh"

wave_run_build test-tuya-ep2h

ROOT="$(wave_build_root)"
echo
echo "Built: $ROOT/bin/test/test-tuya-ep2h"
