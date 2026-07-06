#!/usr/bin/env bash
# Production static site: real API (REACT_APP_USE_MOCK=false) → site/
set -euo pipefail

WAVE_SITE_DEPLOY_DIR=site \
WAVE_SITE_USE_MOCK=false \
source "$(cd "$(dirname "$0")" && pwd)/build-site-lib.sh"
