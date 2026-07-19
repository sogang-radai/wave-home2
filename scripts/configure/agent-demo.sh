#!/usr/bin/env bash
# Configure agent .env for demo backend (docs/ports.txt).
# Backend client-api :8510, agent-api :8511; agent server :8512.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AGENT_DIR="$ROOT/wave-home-agent"
ENV_FILE="$AGENT_DIR/.env"
DEMO_HOST="${DEMO_HOST:-127.0.0.1}"
DEMO_CLIENT_PORT="${DEMO_CLIENT_PORT:-8510}"
DEMO_AGENT_API_PORT="${DEMO_AGENT_API_PORT:-8511}"
DEMO_AGENT_PORT="${DEMO_AGENT_PORT:-8512}"
DEMO_INTERNAL_URL="${DEMO_INTERNAL_URL:-http://${DEMO_HOST}:${DEMO_AGENT_API_PORT}/internal/v1}"
DEMO_CORE_URL="${DEMO_CORE_URL:-http://${DEMO_HOST}:${DEMO_CLIENT_PORT}}"

if [[ ! -d "$AGENT_DIR" ]]; then
  echo "ERROR: wave-home-agent not found" >&2
  exit 1
fi

if [[ ! -f "$ENV_FILE" ]]; then
  cp "$AGENT_DIR/.env.example" "$ENV_FILE"
  echo "Created $ENV_FILE from .env.example"
fi

set_kv() {
  local key="$1" value="$2"
  if grep -q "^${key}=" "$ENV_FILE"; then
    sed -i '' "s|^${key}=.*|${key}=${value}|" "$ENV_FILE" 2>/dev/null \
      || sed -i "s|^${key}=.*|${key}=${value}|" "$ENV_FILE"
  else
    printf '\n%s=%s\n' "$key" "$value" >> "$ENV_FILE"
  fi
}

set_kv WAVEHOME_AGENT_HOST "$DEMO_HOST"
set_kv WAVEHOME_AGENT_PORT "$DEMO_AGENT_PORT"
set_kv WAVEHOME_BACKEND_HOST "$DEMO_HOST"
set_kv WAVEHOME_BACKEND_CLIENT_PORT "$DEMO_CLIENT_PORT"
set_kv WAVEHOME_BACKEND_AGENT_API_PORT "$DEMO_AGENT_API_PORT"
set_kv WAVEHOME_CORE_API_BASE_URL "$DEMO_CORE_URL"
set_kv WAVEHOME_AGENT_INTERNAL_BASE_URL "$DEMO_INTERNAL_URL"
set_kv WAVEHOME_CORE_API_MOCK false

echo "Updated agent env for demo:"
echo "  WAVEHOME_AGENT_PORT=$DEMO_AGENT_PORT"
echo "  WAVEHOME_BACKEND_CLIENT_PORT=$DEMO_CLIENT_PORT"
echo "  WAVEHOME_BACKEND_AGENT_API_PORT=$DEMO_AGENT_API_PORT"
echo "  WAVEHOME_CORE_API_BASE_URL=$DEMO_CORE_URL"
echo "  WAVEHOME_AGENT_INTERNAL_BASE_URL=$DEMO_INTERNAL_URL"
echo "  WAVEHOME_CORE_API_MOCK=false"
echo ""
echo "Start agent: cd wave-home-agent && source .venv/bin/activate && uvicorn app.main:app --reload --host 127.0.0.1 --port ${DEMO_AGENT_PORT}"
