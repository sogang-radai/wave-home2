#!/usr/bin/env bash
# Run wave-server test profile (docs/ports.txt).
# client-api :8520 · agent-api unused
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/bin"

if [[ ! -x "$BIN/wave-server" ]]; then
  echo "error: $BIN/wave-server not found. Run ./scripts/build/server.sh first." >&2
  exit 1
fi

cd "$BIN"
echo "Starting wave-server --profile test (client-api :8520)"
exec ./wave-server --profile test "$@"
