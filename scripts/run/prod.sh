#!/usr/bin/env bash
# Run wave-server production profile (docs/ports.txt).
# client-api :8500 · agent-api :8501 (loopback) · agent server expected on :8502
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/bin"

if [[ ! -x "$BIN/wave-server" ]]; then
  echo "error: $BIN/wave-server not found. Run ./scripts/build/server.sh first." >&2
  exit 1
fi

"$ROOT/scripts/configure/agent-real.sh" || true

cd "$BIN"
echo "Starting wave-server --profile real (client-api :8500, agent-api :8501)"
echo "  Agent should listen on :8502 (see scripts/configure/agent-real.sh)"
exec ./wave-server --profile real "$@"
