#!/usr/bin/env bash
# wave-home-agent 클론 → submodule 등록
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AGENT_DIR="$ROOT/wave-home-agent"
SUBMODULE_URL="https://github.com/sogang-radai/wave-home-agent.git"

cd "$ROOT"
chmod +x scripts/register-agent-submodule.sh scripts/test-agent-integration.sh 2>/dev/null || true

# 이미 submodule
if git ls-files --stage wave-home-agent 2>/dev/null | awk '{print $1}' | grep -q '^160000$'; then
  echo "wave-home-agent submodule already registered."
  git submodule status wave-home-agent
  exit 0
fi

# 유효한 nested repo → register
if [[ -e "$AGENT_DIR/.git" ]] && git -C "$AGENT_DIR" rev-parse HEAD &>/dev/null; then
  echo "Found existing clone; registering as submodule..."
  exec "$ROOT/scripts/register-agent-submodule.sh"
fi

# 소스만 있고 .git 없음 — 삭제하지 않고 안내
if [[ -f "$AGENT_DIR/README.md" ]] && [[ ! -e "$AGENT_DIR/.git" ]]; then
  echo "ERROR: wave-home-agent has files but no .git (broken clone)."
  echo "  Fix manually (do NOT delete if unsure):"
  echo "    rm -rf wave-home-agent"
  echo "    git clone $SUBMODULE_URL wave-home-agent"
  echo "    ./scripts/register-agent-submodule.sh"
  exit 1
fi

if [[ ! -d "$AGENT_DIR" ]]; then
  echo "Cloning $SUBMODULE_URL ..."
  if ! git clone "$SUBMODULE_URL" "$AGENT_DIR"; then
    echo "ERROR: clone failed (GitHub auth?). Run in your terminal:"
    echo "  git clone $SUBMODULE_URL wave-home-agent"
    echo "  ./scripts/register-agent-submodule.sh"
    exit 1
  fi
fi

if ! git -C "$AGENT_DIR" rev-parse HEAD &>/dev/null; then
  echo "ERROR: wave-home-agent is not a valid git repo."
  exit 1
fi

"$ROOT/scripts/register-agent-submodule.sh"
