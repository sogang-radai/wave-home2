#!/usr/bin/env bash
# Cherry-pick dashboard-only commit from wave-home-front PR #3.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRONT="$ROOT/wave-home-front"
COMMIT=84a36437c5defc0ad318d22d7046653726db72b8

cd "$FRONT"

if ! git cat-file -e "$COMMIT^{commit}" 2>/dev/null; then
  echo "Fetching PR #3 head..."
  git fetch origin "pull/3/head:pr-3-dashboard"
fi

if git diff --quiet && [ -z "$(git status --porcelain)" ]; then
  STASHED=false
else
  git stash push -u -m "wip before dashboard cherry-pick $(date +%Y%m%d-%H%M%S)"
  STASHED=true
fi

set +e
git cherry-pick "$COMMIT"
status=$?
set -e

if [ "$status" -ne 0 ]; then
  echo "Cherry-pick failed. Resolve conflicts, then: git cherry-pick --continue" >&2
  exit "$status"
fi

# Drop schema docs from the submodule if the commit added them.
for stray in docs/db-schema.md docs/temp.md; do
  if [ -f "$FRONT/$stray" ]; then
    rm -f "$FRONT/$stray"
    cp "$ROOT/docs/db-schema.md" "$FRONT/docs/db-schema.md" 2>/dev/null || true
  fi
done

if [ "$STASHED" = true ]; then
  git stash pop || true
fi

echo "Cherry-picked $COMMIT on branch $(git branch --show-current)"
