# Raspberry Pi 5 and other AArch64 Linux targets.
# ONNX Runtime is fetched by sherpa-onnx as linux-aarch64-static; MLAS uses NEON.

set(WAVE_ARCH_AARCH64 TRUE CACHE INTERNAL "Building for AArch64")

if(NOT CMAKE_CROSSCOMPILING OR WAVE_TARGET_RPI5)
    add_compile_options(-march=armv8.2-a)
endif()

message(STATUS "Wave aarch64: compiler NEON tuning enabled")
message(STATUS "Wave aarch64: TTS uses sherpa-onnx onnxruntime-linux-aarch64-static (ORT MLAS NEON)")
