set(TEST_TEXT_EMBED_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-text-embed
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/service/agent/embedding_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/task_queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/spinner.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
)

target_include_directories(test-text-embed PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-text-embed PRIVATE
    drogon
    nlohmann_json::nlohmann_json
)

set_target_properties(test-text-embed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_TEXT_EMBED_OUTPUT_DIR}"
)
