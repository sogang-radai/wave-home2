#!/usr/bin/env bash
# Demo static site: client-side demo API layer → site-demo/
set -euo pipefail

WAVE_SITE_DEPLOY_DIR=site-demo \
WAVE_SITE_USE_MOCK=false \
WAVE_SITE_API_MODE=demo \
WAVE_SITE_ANCHOR_DATE=2026-06-30 \
source "$(cd "$(dirname "$0")" && pwd)/site-lib.sh"
