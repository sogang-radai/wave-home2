#!/usr/bin/env bash
# Agent local dev env (.env, venv, deps). Defaults to production ports (docs/ports.txt).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AGENT_DIR="$ROOT/wave-home-agent"

cd "$AGENT_DIR"

if [[ ! -f README.md ]]; then
  echo "ERROR: wave-home-agent missing. Clone it under wave-home-agent/ first." >&2
  exit 1
fi

if [[ ! -f .env ]]; then
  cp .env.example .env
  sed -i '' 's|WAVEHOME_CORE_API_BASE_URL=.*|WAVEHOME_CORE_API_BASE_URL=http://127.0.0.1:8500|' .env 2>/dev/null \
    || sed -i 's|WAVEHOME_CORE_API_BASE_URL=.*|WAVEHOME_CORE_API_BASE_URL=http://127.0.0.1:8500|' .env
  if grep -q '^WAVEHOME_AGENT_INTERNAL_BASE_URL=' .env; then
    sed -i '' 's|WAVEHOME_AGENT_INTERNAL_BASE_URL=.*|WAVEHOME_AGENT_INTERNAL_BASE_URL=http://127.0.0.1:8501/internal/v1|' .env 2>/dev/null \
      || sed -i 's|WAVEHOME_AGENT_INTERNAL_BASE_URL=.*|WAVEHOME_AGENT_INTERNAL_BASE_URL=http://127.0.0.1:8501/internal/v1|' .env
  else
    printf '\nWAVEHOME_AGENT_INTERNAL_BASE_URL=http://127.0.0.1:8501/internal/v1\n' >> .env
  fi
  echo "Created wave-home-agent/.env from .env.example"
  echo "  → GEMINI_API_KEY, OLLAMA_BASE_URL 등 필요 시 수정"
else
  echo "wave-home-agent/.env already exists"
fi

PY="${PYTHON:-python3.12}"
if ! command -v "$PY" &>/dev/null; then
  PY=python3
fi

if [[ ! -d .venv ]]; then
  echo "Creating venv ($PY)..."
  "$PY" -m venv .venv
fi

# shellcheck disable=SC1091
source .venv/bin/activate
pip install -q -U pip
pip install -q -r requirements.txt

echo ""
echo "Agent dev env ready."
echo "  Backend:  ./scripts/run/prod.sh          # client :8500, agent-api :8501"
echo "  Agent:    cd wave-home-agent && source .venv/bin/activate"
echo "            python -m app --reload         # .env WAVEHOME_AGENT_PORT (8502)"
echo ""
echo "  Demo:     ./scripts/configure/agent-demo.sh && ./scripts/run/demo.sh"
echo "            cd wave-home-agent && python -m app --reload   # :8512"
