#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

removed=0

remove_path() {
    local path="$1"
    if [[ -e "$path" || -L "$path" ]]; then
        rm -rf "$path"
        echo "removed: ${path#$ROOT/}"
        removed=$((removed + 1))
    fi
}

remove_path "$ROOT/compile_commands.json"

if [[ -d "$ROOT/bin" ]]; then
    while IFS= read -r -d '' binary; do
        remove_path "$binary"
    done < <(find "$ROOT/bin" -type f -executable -print0)
fi

if [[ "$removed" -eq 0 ]]; then
    echo "nothing to clean"
else
    echo
    echo "cleaned $removed item(s)"
fi
