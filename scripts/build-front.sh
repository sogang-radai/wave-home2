#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRONTEND_DIR="$ROOT/wave-home-front"
SITE_DIR="$ROOT/site"

if [[ ! -f "$FRONTEND_DIR/package.json" ]]; then
    echo "error: wave-home-front is not available at $FRONTEND_DIR" >&2
    echo "  git submodule update --init wave-home-front" >&2
    exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
    echo "error: npm is not installed" >&2
    exit 1
fi

cd "$FRONTEND_DIR"

install_deps() {
    if [[ -f package-lock.json ]]; then
        if npm ci >/dev/null 2>&1; then
            return
        fi
        echo "warning: package-lock.json is out of sync; falling back to npm install" >&2
    fi
    npm install
}

install_deps

npm run build

detect_out_dir() {
    if [[ -n "${WH_FRONTEND_OUT_DIR:-}" ]]; then
        echo "$WH_FRONTEND_OUT_DIR"
        return
    fi

    if command -v node >/dev/null 2>&1; then
        local from_node
        from_node="$(FRONTEND_DIR="$FRONTEND_DIR" node <<'NODE'
const fs = require("fs");
const path = require("path");

const root = process.env.FRONTEND_DIR;
const pkg = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));

if (pkg.build?.outDir) {
    console.log(pkg.build.outDir);
    process.exit(0);
}

const viteConfigCandidates = [
    "vite.config.ts",
    "vite.config.js",
    "vite.config.mts",
    "vite.config.mjs",
];
for (const name of viteConfigCandidates) {
    const file = path.join(root, name);
    if (!fs.existsSync(file)) continue;
    const text = fs.readFileSync(file, "utf8");
    const match = text.match(/outDir\s*:\s*['"]([^'"]+)['"]/);
    if (match) {
        console.log(match[1]);
        process.exit(0);
    }
}

for (const candidate of ["build", "dist", "out"]) {
    if (fs.existsSync(path.join(root, candidate))) {
        console.log(candidate);
        process.exit(0);
    }
}

process.exit(1);
NODE
)" || true
        if [[ -n "$from_node" ]]; then
            echo "$from_node"
            return
        fi
    fi

    for candidate in dist build out; do
        if [[ -d "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done

    echo "error: frontend build output not found (expected dist/, build/, or out/)" >&2
    exit 1
}

OUT_DIR="$(detect_out_dir)"
OUT_PATH="$FRONTEND_DIR/$OUT_DIR"

if [[ ! -f "$OUT_PATH/index.html" ]]; then
    echo "error: $OUT_PATH/index.html not found after npm run build" >&2
    exit 1
fi

rm -rf "$SITE_DIR"
mkdir -p "$SITE_DIR"
cp -a "$OUT_PATH"/. "$SITE_DIR/"

echo "Frontend deployed to $SITE_DIR (from $OUT_PATH)"
