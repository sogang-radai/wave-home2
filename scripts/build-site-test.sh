#!/usr/bin/env bash
# Mock/demo static site: in-browser mock API → site-test/
set -euo pipefail

WAVE_SITE_DEPLOY_DIR=site-test \
WAVE_SITE_USE_MOCK=true \
source "$(cd "$(dirname "$0")" && pwd)/build-site-lib.sh"
