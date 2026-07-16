#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/../_lib/cmake-build.sh"

wave_run_build

ROOT="$(wave_build_root)"
echo
echo "Built:"
echo "  $ROOT/bin/wave-server"
echo "  $ROOT/bin/test/test-tuya-ep2h"
echo "  $ROOT/bin/test/test-srs"
echo "  $ROOT/bin/test/test-llm"
echo "  $ROOT/bin/test/test-tts"
echo "  $ROOT/bin/test/test-ir"
