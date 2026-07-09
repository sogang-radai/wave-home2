#!/usr/bin/env bash
# 기존 wave-home-agent 클론(또는 submodule)을 부모 repo에 정식 등록
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AGENT_DIR="$ROOT/wave-home-agent"
SUBMODULE_URL="https://github.com/sogang-radai/wave-home-agent.git"

cd "$ROOT"

die() { echo "ERROR: $*" >&2; exit 1; }

# 이미 submodule gitlink인지 확인
if git ls-files --stage wave-home-agent 2>/dev/null | awk '{print $1}' | grep -q '^160000$'; then
  echo "wave-home-agent is already registered as a submodule."
  git submodule status wave-home-agent
  exit 0
fi

# 유효한 git repo인지
if ! git -C "$AGENT_DIR" rev-parse HEAD &>/dev/null; then
  die "wave-home-agent has no valid HEAD. Run:\n  rm -rf wave-home-agent\n  git clone $SUBMODULE_URL wave-home-agent\n  ./scripts/register-agent-submodule.sh"
fi

COMMIT="$(git -C "$AGENT_DIR" rev-parse HEAD)"
REMOTE="$(git -C "$AGENT_DIR" config --get remote.origin.url || echo "$SUBMODULE_URL")"

echo "Registering wave-home-agent @ $COMMIT"
echo "  remote: $REMOTE"

# .gitmodules 항목 보장
if ! grep -q '\[submodule "wave-home-agent"\]' .gitmodules 2>/dev/null; then
  cat >> .gitmodules <<EOF
[submodule "wave-home-agent"]
	path = wave-home-agent
	url = $SUBMODULE_URL
EOF
fi

# nested .git → .git/modules/wave-home-agent
if [[ -d "$AGENT_DIR/.git" ]]; then
  mkdir -p .git/modules
  if [[ -d ".git/modules/wave-home-agent" ]]; then
    rm -rf ".git/modules/wave-home-agent"
  fi
  mv "$AGENT_DIR/.git" ".git/modules/wave-home-agent"
  git config -f .git/config submodule.wave-home-agent.url "$REMOTE"
  git config -f .git/config submodule.wave-home-agent.active true
  echo "gitdir: ../.git/modules/wave-home-agent" > "$AGENT_DIR/.git"
  git config -f ".git/modules/wave-home-agent/config" core.worktree "$AGENT_DIR"
elif [[ -f "$AGENT_DIR/.git" ]]; then
  : # already gitdir pointer
else
  die "wave-home-agent/.git missing"
fi

git submodule absorbgitdirs wave-home-agent 2>/dev/null || true
git update-index --add --cacheinfo 160000,"$COMMIT",wave-home-agent
git add .gitmodules

echo ""
echo "OK: staged submodule gitlink for wave-home-agent ($COMMIT)"
echo "  git status"
git status --short wave-home-agent .gitmodules
echo ""
echo "Commit when ready:"
echo "  git commit -m \"Add wave-home-agent submodule\""
