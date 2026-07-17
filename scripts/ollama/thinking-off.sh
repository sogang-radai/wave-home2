#!/usr/bin/env bash
# Disable thinking (default for scripts/ollama/run.sh and API helpers).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
STATE="$ROOT/.think"
echo "false" >"$STATE"
echo "Thinking OFF -> $STATE"
echo "  Interactive:  $ROOT/run.sh [MODEL] [PROMPT...]"
echo "  One-shot:     ollama run MODEL --think=false"
echo "  API:          {\"think\": false}"
