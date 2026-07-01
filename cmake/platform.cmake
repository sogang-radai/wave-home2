option(WAVE_TARGET_RPI5 "Tune compiler and deps for Raspberry Pi 5 (aarch64)" OFF)

if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/macos.cmake")
endif()

if(WAVE_TARGET_RPI5 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    include("${CMAKE_CURRENT_LIST_DIR}/platform_aarch64.cmake")
endif()
