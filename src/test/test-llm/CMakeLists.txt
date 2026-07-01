set(TEST_LLM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-llm
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/service/llm_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/task_queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/auxiliary/stb_image.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/spinner.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
)

wave_use_wave_stb("${CMAKE_SOURCE_DIR}/src/wave-server/auxiliary/stb_image.cpp")

target_include_directories(test-llm PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-llm PRIVATE
    drogon
    wave::stb
    nlohmann_json::nlohmann_json
)

set_target_properties(test-llm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_LLM_OUTPUT_DIR}"
)
