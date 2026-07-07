cmake_minimum_required(VERSION 3.16)

find_package(OpenSSL REQUIRED)

set(TEST_SAMSUNG_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-samsung-tizen
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/samsung_tizen.cpp"
)

target_include_directories(test-samsung-tizen PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-samsung-tizen PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(APPLE)
    target_link_libraries(test-samsung-tizen PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

set_target_properties(test-samsung-tizen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_SAMSUNG_OUTPUT_DIR}"
)
