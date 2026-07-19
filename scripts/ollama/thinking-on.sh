#!/usr/bin/env bash
# Enable thinking (default for scripts/ollama/run.sh and API helpers).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
STATE="$ROOT/.think"
echo "true" >"$STATE"
echo "Thinking ON -> $STATE"
echo "  Interactive:  $ROOT/run.sh [MODEL] [PROMPT...]"
echo "  One-shot:     ollama run MODEL --think=true"
echo "  API:          {\"think\": true}"
