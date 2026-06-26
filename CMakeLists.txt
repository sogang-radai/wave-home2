cmake_minimum_required(VERSION 3.16)

project(wave-home2 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for IDE IntelliSense" FORCE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

include("${CMAKE_SOURCE_DIR}/cmake/deps.cmake")

set(BUILD_CTL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_ORM OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
set(COZ_PROFILING OFF CACHE BOOL "" FORCE)
set(USE_SUBMODULE ON CACHE BOOL "" FORCE)

add_subdirectory(thirdparty/drogon)
add_subdirectory(src/test/test-llm)
add_subdirectory(src/test/test-ir)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC COPY_ON_ERROR
    )
endif()
