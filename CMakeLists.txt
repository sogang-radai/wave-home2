cmake_minimum_required(VERSION 3.16)

project(wave-home2 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for IDE IntelliSense" FORCE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

include("${CMAKE_SOURCE_DIR}/cmake/wave.cmake")

add_subdirectory(src/wave-server)

if(WAVE_BUILD_TESTS)
    add_subdirectory(src/test/test-tuya)
    add_subdirectory(src/test/test-srs)
    add_subdirectory(src/test/test-llm)
    if(WAVE_BUILD_TTS)
        add_subdirectory(src/test/test-tts)
    endif()
    add_subdirectory(src/test/test-ir)
else()
    message(STATUS "WAVE_BUILD_TESTS=OFF: skipping src/test (use -DWAVE_BUILD_TESTS=ON to enable)")
endif()

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC COPY_ON_ERROR
    )
endif()
