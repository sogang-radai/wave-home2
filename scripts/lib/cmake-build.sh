#!/usr/bin/env bash

wave_build_root() {
    cd "$(dirname "${BASH_SOURCE[1]}")/../.." && pwd
}

wave_build_jobs() {
    echo "${JOBS:-4}"
}

wave_ensure_macos_deps() {
    if [[ "$(uname -s)" != "Darwin" ]]; then
        return
    fi

    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required on macOS. See https://brew.sh" >&2
        exit 1
    fi

    for pkg in cmake libomp; do
        if ! brew list --formula "$pkg" >/dev/null 2>&1; then
            echo "Installing $pkg via Homebrew..."
            brew install "$pkg"
        fi
    done

    export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
}

wave_enable_ccache() {
    if command -v ccache >/dev/null 2>&1; then
        export CMAKE_C_COMPILER_LAUNCHER=ccache
        export CMAKE_CXX_COMPILER_LAUNCHER=ccache
    fi
}

wave_cmake_configure() {
    local root="$1"
    local build_dir="$2"

    wave_enable_ccache
    mkdir -p "$build_dir"
    cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" "${@:3}"
}

wave_cmake_build() {
    local build_dir="$1"
    local jobs="$2"
    shift 2

    if [[ $# -gt 0 ]]; then
        cmake --build "$build_dir" -j"$jobs" --target "$@"
    else
        cmake --build "$build_dir" -j"$jobs"
    fi
}

wave_run_build() {
    local target="${1:-}"

    local root
    root="$(wave_build_root)"
    local build_dir="${BUILD_DIR:-$root/build}"
    local jobs
    jobs="$(wave_build_jobs)"

    wave_ensure_macos_deps
    wave_cmake_configure "$root" "$build_dir"
    if [[ -n "$target" ]]; then
        wave_cmake_build "$build_dir" "$jobs" "$target"
    else
        wave_cmake_build "$build_dir" "$jobs"
    fi
}
