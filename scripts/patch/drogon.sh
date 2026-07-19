#!/usr/bin/env bash
# Apply wave-home local patches to thirdparty/drogon (idempotent).
#
# Needed because wave-server registers sqlite-vec via sqlite3_auto_extension
# after setting SQLITE_CONFIG_MULTITHREAD. That initializes SQLite early, so
# Drogon's later sqlite3_config() returns SQLITE_MISUSE and would otherwise
# LOG_FATAL on first DB connection.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DROGON_DIR="$ROOT/thirdparty/drogon"
PATCH="$ROOT/patches/drogon/sqlite3-multithread-misuse.patch"
TARGET="$DROGON_DIR/orm_lib/src/sqlite3_impl/Sqlite3Connection.cc"

if [[ ! -d "$DROGON_DIR" ]]; then
    echo "drogon not found at $DROGON_DIR (run: git submodule update --init --recursive)" >&2
    exit 1
fi

if [[ ! -f "$PATCH" ]]; then
    echo "missing patch: $PATCH" >&2
    exit 1
fi

if [[ ! -f "$TARGET" ]]; then
    echo "missing drogon source: $TARGET" >&2
    exit 1
fi

# Already applied (marker from our patch comment / tolerant SQLITE_MISUSE path).
if grep -q 'ret == SQLITE_MISUSE && sqlite3_threadsafe()' "$TARGET"; then
    echo "drogon: sqlite3 multithread patch already applied"
    exit 0
fi

echo "drogon: applying sqlite3 multithread SQLITE_MISUSE patch..."
if command -v git >/dev/null 2>&1 && [[ -d "$DROGON_DIR/.git" || -f "$DROGON_DIR/.git" ]]; then
    git -C "$DROGON_DIR" apply --whitespace=nowarn "$PATCH"
else
    patch -p1 -d "$DROGON_DIR" < "$PATCH"
fi

if ! grep -q 'ret == SQLITE_MISUSE && sqlite3_threadsafe()' "$TARGET"; then
    echo "drogon: patch did not apply as expected" >&2
    exit 1
fi

echo "drogon: patch applied"
