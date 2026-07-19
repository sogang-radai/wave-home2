#!/usr/bin/env bash
# Run wave-server demo profile (docs/ports.txt).
# client-api :8510 · agent-api :8511 (loopback) · agent server expected on :8512
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/bin"

if [[ ! -x "$BIN/wave-server" ]]; then
  echo "error: $BIN/wave-server not found. Run ./scripts/build/server.sh first." >&2
  exit 1
fi

"$ROOT/scripts/configure/agent-demo.sh" || true

cd "$BIN"
echo "Starting wave-server --profile demo (client-api :8510, agent-api :8511)"
echo "  Agent should listen on :8512 (see scripts/configure/agent-demo.sh)"
exec ./wave-server --profile demo "$@"
