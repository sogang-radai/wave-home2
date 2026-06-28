cmake_minimum_required(VERSION 3.16)

project(wave-home2 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for IDE IntelliSense" FORCE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

include("${CMAKE_SOURCE_DIR}/cmake/deps.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/drogon_minimal.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/ncnn_minimal.cmake")

add_subdirectory(thirdparty/drogon)
add_subdirectory(thirdparty/ncnn)
add_subdirectory(src/wave-server)
add_subdirectory(src/test/test-llm)
add_subdirectory(src/test/test-ir)
add_subdirectory(src/test/test-iq)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC COPY_ON_ERROR
    )
endif()
