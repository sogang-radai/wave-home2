#!/usr/bin/env bash
# 실사용 프로필(wave-server :8500)에 맞게 에이전트 .env 의 internal API 설정을 갱신한다.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AGENT_DIR="$ROOT/wave-home-agent"
ENV_FILE="$AGENT_DIR/.env"
REAL_INTERNAL_URL="${REAL_INTERNAL_URL:-http://127.0.0.1:8500/internal/v1}"
REAL_CORE_URL="${REAL_CORE_URL:-http://127.0.0.1:8500}"

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

set_kv WAVEHOME_CORE_API_BASE_URL "$REAL_CORE_URL"
set_kv WAVEHOME_AGENT_INTERNAL_BASE_URL "$REAL_INTERNAL_URL"
set_kv WAVEHOME_CORE_API_MOCK false

echo "Updated agent env for real backend:"
echo "  WAVEHOME_CORE_API_BASE_URL=$REAL_CORE_URL"
echo "  WAVEHOME_AGENT_INTERNAL_BASE_URL=$REAL_INTERNAL_URL"
echo "  WAVEHOME_CORE_API_MOCK=false"
echo ""
echo "Restart the agent (:8501) for changes to take effect."
