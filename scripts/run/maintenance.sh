#!/usr/bin/env bash
# Serve the maintenance page in place of wave-server (docs/ports.txt: demo client-api :8510).
# Use while backend/frontend demo servers are stopped for backend work.
set -euo pipefail

PORT="${1:-8510}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SITE="$ROOT/wave-home-front/maintenance-site"

if [[ ! -f "$SITE/index.html" ]]; then
  echo "error: $SITE/index.html not found." >&2
  exit 1
fi

cd "$SITE"
echo "Serving maintenance page on :$PORT (Ctrl+C to stop, then restart wave-server normally)"
exec python3 -m http.server "$PORT"
