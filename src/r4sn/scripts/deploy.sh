#!/usr/bin/env bash
set -euo pipefail

R4SN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCAL_BIN="${R4SN_ROOT}/deploy-aarch64/iq-server"
REMOTE_PATH="/user/bin/iq-server"
DEPLOY_ENV="${R4SN_ROOT}/scripts/deploy.env"

if [[ -f "${DEPLOY_ENV}" ]]; then
    # shellcheck disable=SC1090
    source "${DEPLOY_ENV}"
fi

SENSOR_HOST="${R4SN_SENSOR_HOST:-192.168.0.33}"
SENSOR_USER="${R4SN_SENSOR_USER:-root}"
SENSOR_PASS="${R4SN_SSH_PASS:-}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

if [[ -z "${SENSOR_PASS}" ]]; then
    echo "error: R4SN_SSH_PASS is not set"
    echo "  cp ${R4SN_ROOT}/scripts/deploy.env.example ${DEPLOY_ENV}"
    echo "  # edit deploy.env and set R4SN_SSH_PASS"
    exit 1
fi

if [[ ! -x "${LOCAL_BIN}" ]]; then
    echo "error: ${LOCAL_BIN} not found; run build-iq-server.sh first"
    exit 1
fi

if ! command -v sshpass >/dev/null 2>&1; then
    echo "error: sshpass not found"
    exit 1
fi

sshpass -p "${SENSOR_PASS}" scp "${SSH_OPTS[@]}" \
    "${LOCAL_BIN}" "${SENSOR_USER}@${SENSOR_HOST}:${REMOTE_PATH}"

sshpass -p "${SENSOR_PASS}" ssh "${SSH_OPTS[@]}" \
    "${SENSOR_USER}@${SENSOR_HOST}" "chmod +x ${REMOTE_PATH}"

echo "Deployed ${LOCAL_BIN} -> ${SENSOR_USER}@${SENSOR_HOST}:${REMOTE_PATH}"
