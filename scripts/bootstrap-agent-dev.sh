#!/usr/bin/env bash
# 에이전트 로컬 개발 환경 준비 (.env, venv, 의존성)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AGENT_DIR="$ROOT/wave-home-agent"

cd "$AGENT_DIR"

if [[ ! -f README.md ]]; then
  echo "ERROR: wave-home-agent missing. Run: ./scripts/setup-agent-submodule.sh" >&2
  exit 1
fi

if [[ ! -f .env ]]; then
  cp .env.example .env
  # 백엔드 포트 맞춤 (wave-server 기본 8500)
  sed -i '' 's|WAVEHOME_CORE_API_BASE_URL=.*|WAVEHOME_CORE_API_BASE_URL=http://127.0.0.1:8500|' .env 2>/dev/null \
    || sed -i 's|WAVEHOME_CORE_API_BASE_URL=.*|WAVEHOME_CORE_API_BASE_URL=http://127.0.0.1:8500|' .env
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
echo "  Terminal 1 (backend): cd bin && sudo ./wave-server   # :8500"
echo "  Terminal 2 (agent):"
echo "    cd wave-home-agent && source .venv/bin/activate"
echo "    uvicorn app.main:app --reload --port 8501"
echo ""
echo "  Smoke test: ./scripts/test-agent-integration.sh"
