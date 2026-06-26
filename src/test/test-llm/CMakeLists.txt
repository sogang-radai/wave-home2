set(TEST_LLM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-llm
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/common/llm/lm_studio_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/spinner.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
)

target_include_directories(test-llm PRIVATE
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(test-llm PRIVATE drogon)

set_target_properties(test-llm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_LLM_OUTPUT_DIR}"
)
