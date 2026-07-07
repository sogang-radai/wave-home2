cmake_minimum_required(VERSION 3.16)

set(TEST_SLEEP_NET_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-sleep-net
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/srs_r4sn.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/nn/frame_encoder.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/nn/frame_aggregator.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/nn/sleep_pipeline.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
)

target_include_directories(test-sleep-net PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-sleep-net PRIVATE
    wave::asio
    ncnn
    nlohmann_json::nlohmann_json
)

set_target_properties(test-sleep-net PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_SLEEP_NET_OUTPUT_DIR}"
)
