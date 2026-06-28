#!/bin/sh
# Cross-build iq-server for S32R45 (aarch64 Linux) → src/r4sn/deploy-aarch64/iq-server
set -e

R4SN_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WAVE_HOME2_ROOT="$(cd "${R4SN_ROOT}/../.." && pwd)"
BUILD_DIR="${R4SN_ROOT}/iq-server/build-a64"
DEPLOY_DIR="${R4SN_ROOT}/deploy-aarch64"
TOOLCHAIN="${R4SN_ROOT}/cmake/aarch64-linux-gnu.cmake"
LOCAL_TOOLCHAIN="${WAVE_HOME2_ROOT}/.toolchain/prefix/usr/bin"
STATIC="${R4SN_STATIC:-ON}"
A64_GCC="${A64_GCC:-aarch64-linux-gnu-gcc}"
A64_GXX="${A64_GXX:-aarch64-linux-gnu-g++}"
JOBS="$(nproc 2>/dev/null || echo 4)"

err() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

if ! command -v "$A64_GXX" >/dev/null 2>&1; then
	if [ -x "${LOCAL_TOOLCHAIN}/aarch64-linux-gnu-g++-13" ]; then
		A64_GXX="${LOCAL_TOOLCHAIN}/aarch64-linux-gnu-g++-13"
		A64_GCC="${LOCAL_TOOLCHAIN}/aarch64-linux-gnu-gcc-13"
	fi
fi

command -v "$A64_GXX" >/dev/null 2>&1 ||
	err "missing $A64_GXX (apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu)"
command -v "$A64_GCC" >/dev/null 2>&1 ||
	err "missing $A64_GCC (apt install gcc-aarch64-linux-gnu)"

mkdir -p "${DEPLOY_DIR}"
echo "=== iq-server (aarch64, R4SN_STATIC=${STATIC}) ==="
echo "  CXX=${A64_GXX}"
cmake -S "${R4SN_ROOT}/iq-server" -B "${BUILD_DIR}" \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
	-DCMAKE_C_COMPILER="${A64_GCC}" \
	-DCMAKE_CXX_COMPILER="${A64_GXX}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DR4SN_STATIC="${STATIC}"
cmake --build "${BUILD_DIR}" --target iq-server -j"${JOBS}"

echo ""
echo "=== ${DEPLOY_DIR}/iq-server ==="
file "${DEPLOY_DIR}/iq-server"
