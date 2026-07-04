cmake_minimum_required(VERSION 3.16)

set(TEST_REOLINK_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-reolink-e1pro
    main.cpp
    stb_impl.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/reolink_e1pro.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/service/go2rtc_service.cpp"
)

target_include_directories(test-reolink-e1pro PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_SOURCE_DIR}/thirdparty/stb"
)

target_compile_definitions(test-reolink-e1pro PRIVATE
    WAVE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test-reolink-e1pro PRIVATE
    nlohmann_json::nlohmann_json
)

set_target_properties(test-reolink-e1pro PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_REOLINK_OUTPUT_DIR}"
)
