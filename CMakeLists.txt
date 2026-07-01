cmake_minimum_required(VERSION 3.16)

project(wave-home2 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for IDE IntelliSense" FORCE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

include("${CMAKE_SOURCE_DIR}/cmake/deps.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/asio.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/tuyapp.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/macos.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/drogon_minimal.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/sherpa_onnx_minimal.cmake")

add_subdirectory(thirdparty/drogon)

set(WAVE_SAVED_CXX_STANDARD ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(thirdparty/sherpa-onnx "${CMAKE_BINARY_DIR}/thirdparty/sherpa-onnx")
set(CMAKE_CXX_STANDARD ${WAVE_SAVED_CXX_STANDARD})

add_subdirectory(src/wave-server)
include("${CMAKE_SOURCE_DIR}/cmake/site.cmake")
add_subdirectory(src/test/test-tuya)
add_subdirectory(src/test/test-srs)
add_subdirectory(src/test/test-llm)
add_subdirectory(src/test/test-tts)
add_subdirectory(src/test/test-ir)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC COPY_ON_ERROR
    )
endif()
