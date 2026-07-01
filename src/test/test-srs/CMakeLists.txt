cmake_minimum_required(VERSION 3.16)

set(TEST_SRS_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-srs
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/srs_r4sn.cpp"
)

target_include_directories(test-srs PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-srs PRIVATE
    wave::asio
    nlohmann_json::nlohmann_json
)

set_target_properties(test-srs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_SRS_OUTPUT_DIR}"
)
