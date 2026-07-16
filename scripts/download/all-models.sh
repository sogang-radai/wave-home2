#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

"$ROOT/tts-model.sh"
"$ROOT/stt-model.sh"
"$ROOT/pose-model.sh"

echo "All models installed."
