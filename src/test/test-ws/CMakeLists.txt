cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/cmake/opus.cmake")

set(TEST_WS_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-ws
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/radai_ws.cpp"
)

target_include_directories(test-ws PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_compile_definitions(test-ws PRIVATE
    WAVE_STANDALONE_DEVICE_TEST=1
    WAVE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test-ws PRIVATE
    wave::asio
    nlohmann_json::nlohmann_json
)

wave_link_opus(test-ws)

set_target_properties(test-ws PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_WS_OUTPUT_DIR}"
)
