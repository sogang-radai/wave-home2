cmake_minimum_required(VERSION 3.16)

set(TEST_WIZ_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-philips-wiz-e29
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/philips_wiz_e29.cpp"
)

target_include_directories(test-philips-wiz-e29 PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-philips-wiz-e29 PRIVATE
    nlohmann_json::nlohmann_json
)

set_target_properties(test-philips-wiz-e29 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_WIZ_OUTPUT_DIR}"
)
