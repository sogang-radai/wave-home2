#!/usr/bin/env bash
# 백엔드(:8500) ↔ 에이전트(:8501) 연동 스모크 테스트
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BACKEND_URL="${BACKEND_URL:-http://127.0.0.1:8500}"
AGENT_URL="${AGENT_URL:-http://127.0.0.1:8501}"
CHECK_ONLY=false
VERBOSE=false

usage() {
  cat <<'EOF'
Usage: ./scripts/test-agent-integration.sh [options]

Options:
  --check-only   Exit after env/README checks (do not curl services)
  --verbose      Print response bodies
  -h, --help     Show this help

Env:
  BACKEND_URL   Default http://127.0.0.1:8500
  AGENT_URL     Default http://127.0.0.1:8501
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check-only) CHECK_ONLY=true ;;
    --verbose) VERBOSE=true ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

pass() { echo "  [OK] $*"; }
fail() { echo "  [FAIL] $*"; }
warn() { echo "  [WARN] $*"; }
skip() { echo "  [SKIP] $*"; }

http_code() {
  curl -s -o /dev/null -w "%{http_code}" --connect-timeout 2 --max-time 5 "$1" 2>/dev/null || echo "000"
}

http_get() {
  curl -s --connect-timeout 2 --max-time 8 "$1" 2>/dev/null || true
}

echo "=== WaveHome agent integration smoke test ==="
echo "Backend: $BACKEND_URL"
echo "Agent:   $AGENT_URL"
echo ""

# --- Submodule / README ---
AGENT_DIR="$ROOT/wave-home-agent"
if [[ -f "$AGENT_DIR/README.md" ]]; then
  pass "wave-home-agent/README.md found"
  echo ""
  echo "--- wave-home-agent README (first 40 lines) ---"
  head -n 40 "$AGENT_DIR/README.md" | sed 's/^/  /'
  echo "---"
  if [[ -f "$AGENT_DIR/.env.example" ]]; then
    pass ".env.example present — copy to .env and set BACKEND_URL=$BACKEND_URL/internal/v1"
  fi
  if [[ -f "$AGENT_DIR/pyproject.toml" ]]; then
    pass "pyproject.toml found (uv/poetry install per README)"
  fi
else
  fail "wave-home-agent not cloned — run: ./scripts/setup-agent-submodule.sh"
  echo "  (Private repo: GitHub SSH or HTTPS login required)"
fi
echo ""

if $CHECK_ONLY; then
  echo "(--check-only: skipping HTTP probes)"
  exit 0
fi

# --- Backend ---
echo "Backend probes:"
code="$(http_code "$BACKEND_URL/api/v1/health")"
if [[ "$code" == "200" ]]; then
  pass "GET /api/v1/health → $code"
else
  fail "GET /api/v1/health → $code (start: cd bin && ../build/wave-server or your run script)"
fi

for path in \
  "/internal/v1/db/query" \
  "/internal/v1/rag/search" \
  "/internal/v1/devices"
do
  if [[ "$path" == *db/query* || "$path" == *rag/search* ]]; then
    code="$(curl -s -o /dev/null -w '%{http_code}' --connect-timeout 2 --max-time 5 \
      -X POST "$BACKEND_URL$path" -H 'Content-Type: application/json' -d '{}' 2>/dev/null || echo 000)"
  else
    code="$(http_code "$BACKEND_URL$path")"
  fi
  if [[ "$code" == "000" ]]; then
    skip "$path — backend not reachable"
  elif [[ "$code" == "404" ]]; then
    warn "$path → 404 (not implemented yet — see docs/agent-tool-api.md)"
  else
    pass "$path → $code"
    $VERBOSE && http_get "$BACKEND_URL$path" | head -c 200 && echo ""
  fi
done
echo ""

# --- Agent ---
echo "Agent probes:"
agent_up=false
for path in "/docs" "/openapi.json" "/health" "/api/health"; do
  code="$(http_code "$AGENT_URL$path")"
  if [[ "$code" == "200" ]]; then
    pass "GET $path → $code"
    agent_up=true
    break
  fi
done
if ! $agent_up; then
  fail "Agent not responding on $AGENT_URL (see wave-home-agent/README.md to start :8501)"
fi

for path in "/llm/v1/models" "/chat/v1/turns"; do
  if [[ "$path" == *turns ]]; then
    code="$(curl -s -o /dev/null -w '%{http_code}' --connect-timeout 2 --max-time 5 \
      -X POST "$AGENT_URL$path" -H 'Content-Type: application/json' \
      -d '{"chatHistoryId":1,"userId":1,"messages":[{"role":"user","content":"ping"}],"stream":false}' 2>/dev/null || echo 000)"
  else
    code="$(http_code "$AGENT_URL$path")"
  fi
  if [[ "$code" == "000" ]]; then
    skip "$path"
  elif [[ "$code" =~ ^[45] ]]; then
    warn "$path → $code (may need API key / model config)"
  else
    pass "$path → $code"
  fi
done
echo ""

echo "=== Summary ==="
echo "Contract: docs/agent-api/README.md, docs/agent-api/device-tool-api.md"
echo "1. Backend :8500 — public /api/v1/* + internal /internal/v1/* (agent callbacks)"
echo "2. Agent :8501 — /chat/v1, /llm/v1, /sleep/v1, /power/v1; calls BACKEND_URL/internal/v1"
echo "3. Agent env: BACKEND_URL=$BACKEND_URL/internal/v1 (typical)"
echo ""
echo "Manual chat test (after both up):"
echo "  curl -N -X POST $AGENT_URL/chat/v1/turns \\"
echo "    -H 'Content-Type: application/json' \\"
echo "    -d '{\"chatHistoryId\":1,\"userId\":1,\"messages\":[{\"role\":\"user\",\"content\":\"안녕\"}],\"stream\":true}'"
