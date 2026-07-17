#!/usr/bin/env bash
# Shared frontend build + deploy. Source from build-site.sh / build-site-test.sh.
# Required: WAVE_SITE_DEPLOY_DIR (e.g. site or site-test)
# Optional: WAVE_SITE_USE_MOCK, WAVE_SITE_API_MODE, WAVE_SITE_ANCHOR_DATE

set -euo pipefail

: "${WAVE_SITE_DEPLOY_DIR:?WAVE_SITE_DEPLOY_DIR must be set}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SITE_SOURCE_DIR="$ROOT/wave-home-front"
SITE_DIR="$ROOT/$WAVE_SITE_DEPLOY_DIR"
USE_MOCK="${WAVE_SITE_USE_MOCK:-true}"

if [[ ! -f "$SITE_SOURCE_DIR/package.json" ]]; then
    echo "error: site source (wave-home-front) is not available at $SITE_SOURCE_DIR" >&2
    echo "  git submodule update --init wave-home-front" >&2
    exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
    echo "error: npm is not installed" >&2
    exit 1
fi

# Cursor/sandbox (and some npm wrappers) inject npm_config_devdir; npm 10+ prints
# "Unknown env config devdir" on every invocation. Drop it for this script only.
unset npm_config_devdir NPM_CONFIG_DEVDIR 2>/dev/null || true

cd "$SITE_SOURCE_DIR"

# Quiet install: hide CRA/react-scripts transitive deprecation spam; keep real errors.
npm_install_quiet() {
    # --loglevel=error still surfaces failures; audit/fund are noise for site deploy.
    npm "$@" --no-fund --no-audit --loglevel=error
}

needs_npm_install() {
    [[ ! -d node_modules ]] && return 0
    [[ ! -f package-lock.json ]] && return 0
    # Reinstall when the lockfile is newer than the install tree.
    [[ package-lock.json -nt node_modules ]] && return 0
    return 1
}

install_deps() {
    if ! needs_npm_install; then
        echo "Using existing node_modules"
        return
    fi

    if [[ -f package-lock.json ]]; then
        local ci_log
        ci_log="$(mktemp)"
        if npm_install_quiet ci >"$ci_log" 2>&1; then
            rm -f "$ci_log"
            return
        fi
        echo "warning: npm ci failed; falling back to npm install" >&2
        # Show the real failure reason (last non-deprecation lines).
        if [[ -s "$ci_log" ]]; then
            grep -Ev 'deprecated|Unknown env config|allow-scripts|funding|vulnerabilit' "$ci_log" >&2 || true
            tail -n 5 "$ci_log" >&2 || true
        fi
        rm -f "$ci_log"
    fi
    npm_install_quiet install
}

install_deps

API_MODE="${WAVE_SITE_API_MODE:-}"
if [[ -z "$API_MODE" ]]; then
    if [[ "$USE_MOCK" == "true" ]]; then
        API_MODE_FLAG=mock
    else
        API_MODE_FLAG=real
    fi
else
    API_MODE_FLAG="$API_MODE"
fi

if [[ "$API_MODE_FLAG" == "mock" ]]; then
    MOCK_FLAG=true
else
    MOCK_FLAG=false
fi

echo "Building wave-home-front (REACT_APP_API_MODE=$API_MODE_FLAG REACT_APP_USE_MOCK=$MOCK_FLAG) → $SITE_DIR"

# CRA keeps old hashed bundles; a stale index.html can point at the wrong API-mode bundle.
rm -rf "$SITE_SOURCE_DIR/build" "$SITE_SOURCE_DIR/dist" "$SITE_SOURCE_DIR/out"

# Filter CRA's post-success boilerplate (bundle-size / serve tips). Keep compile
# errors, eslint output, and the gzip size table.
filter_cra_noise() {
    awk '
        /Unknown env config/ { next }
        /The bundle size is significantly larger/ { skip = 1; next }
        skip == 1 {
            if ($0 ~ /goo\.gl|analyze the project/) next
            skip = 0
        }
        /The project was built assuming it is hosted/ { skip = 2; next }
        skip == 2 {
            if ($0 ~ /^[[:space:]]*$/) next
            if ($0 ~ /homepage field|build folder is ready|You may serve|npm install -g serve|serve -s build|Find out more|cra\.link/) next
            skip = 0
        }
        { print }
    '
}

set +e
REACT_APP_API_MODE="$API_MODE_FLAG" \
REACT_APP_USE_MOCK="$MOCK_FLAG" \
REACT_APP_ANCHOR_DATE="${WAVE_SITE_ANCHOR_DATE:-}" \
GENERATE_SOURCEMAP=false \
npm run build 2>&1 | filter_cra_noise
build_status=${PIPESTATUS[0]}
set -e
if [[ "$build_status" -ne 0 ]]; then
    echo "error: npm run build failed (exit $build_status)" >&2
    exit "$build_status"
fi

detect_out_dir() {
    if [[ -n "${WAVE_SITE_OUT_DIR:-}" ]]; then
        echo "$WAVE_SITE_OUT_DIR"
        return
    fi

    if command -v node >/dev/null 2>&1; then
        local from_node
        from_node="$(SITE_SOURCE_DIR="$SITE_SOURCE_DIR" node <<'NODE'
const fs = require("fs");
const path = require("path");

const root = process.env.SITE_SOURCE_DIR;
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

    echo "error: site build output not found (expected dist/, build/, or out/)" >&2
    exit 1
}

OUT_DIR="$(detect_out_dir)"
OUT_PATH="$SITE_SOURCE_DIR/$OUT_DIR"

if [[ ! -f "$OUT_PATH/index.html" ]]; then
    echo "error: $OUT_PATH/index.html not found after npm run build" >&2
    exit 1
fi

rm -rf "$SITE_DIR"
mkdir -p "$SITE_DIR"
cp -a "$OUT_PATH"/. "$SITE_DIR/"

echo "Site deployed to $SITE_DIR (from $OUT_PATH, api_mode=$API_MODE_FLAG)"
