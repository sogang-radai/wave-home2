#!/usr/bin/env bash
# Run an Ollama model with the thinking mode from scripts/ollama/.think
# (set by thinking-on.sh / thinking-off.sh). Default: think=false if unset.
# Usage: scripts/ollama/run.sh [MODEL] [PROMPT...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
STATE="$ROOT/.think"
MODEL="${1:-gemma4:12b-mlx}"
if [[ $# -ge 1 ]]; then shift; fi

THINK="false"
if [[ -f "$STATE" ]]; then
  THINK="$(tr -d '[:space:]' <"$STATE")"
fi
case "$THINK" in
  true|false|high|medium|low) ;;
  *) THINK="false" ;;
esac

echo "think=${THINK} model=${MODEL} host=${OLLAMA_HOST:-127.0.0.1:11434}" >&2
exec ollama run "$MODEL" --think="$THINK" "$@"
