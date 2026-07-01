option(WAVE_BUILD_TTS "Build sherpa-onnx TTS (wave-server + test-tts)" ON)
option(WAVE_BUILD_TESTS "Build device/integration test binaries under src/test" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release")
endif()
