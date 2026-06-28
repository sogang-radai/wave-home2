cmake_minimum_required(VERSION 3.16)

project(iq-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(WAVE_HOME2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(IQ_SERVER_OUTPUT_DIR "${WAVE_HOME2_ROOT}/src/r4sn/deploy-aarch64")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${IQ_SERVER_OUTPUT_DIR}")

add_executable(iq-server
    main.cpp
    cube_mmap.cpp
    signal_processor.cpp
    frame_sync.cpp
    tcp_server.cpp
    "${WAVE_HOME2_ROOT}/src/common/r4sn/iq_protocol.cpp"
    "${WAVE_HOME2_ROOT}/src/common/util/arg_parser.cpp"
)

target_include_directories(iq-server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WAVE_HOME2_ROOT}/src/common"
)

find_package(Threads REQUIRED)

target_link_libraries(iq-server PRIVATE Threads::Threads m)

target_compile_options(iq-server PRIVATE -Wall -Wextra -Wpedantic)

option(R4SN_STATIC "Static link (BusyBox rootfs)" OFF)
if(R4SN_STATIC)
    target_link_options(iq-server PRIVATE -static)
endif()
