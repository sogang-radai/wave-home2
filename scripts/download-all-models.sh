#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

"$ROOT/download-tts-model.sh"
"$ROOT/download-stt-model.sh"

echo "All models installed."
