#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../_lib/cmake-build.sh"

# Local drogon patch (sqlite-vec + SQLITE_CONFIG_MULTITHREAD). Idempotent.
"$SCRIPT_DIR/../patch/drogon.sh"

wave_run_build wave-server

ROOT="$(wave_build_root)"
echo
echo "Built: $ROOT/bin/wave-server"
