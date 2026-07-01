set(TEST_IR_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-ir
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/protocol/ir_remote.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
)

target_include_directories(test-ir PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

find_package(Threads REQUIRED)

target_link_libraries(test-ir PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

set_target_properties(test-ir PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_IR_OUTPUT_DIR}"
)
