cmake_minimum_required(VERSION 3.16)

set(TEST_TUYA_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-tuya-ep2h
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/tuya_ep2h.cpp"
)

target_include_directories(test-tuya-ep2h PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/tuya_ep2h.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src"
)

target_link_libraries(test-tuya-ep2h PRIVATE
    wave::tuyapp
    nlohmann_json::nlohmann_json
)

set_target_properties(test-tuya-ep2h PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_TUYA_OUTPUT_DIR}"
)
