set(TEST_TTS_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-tts
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/service/tts_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/task_queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/exe_path.cpp"
)

target_include_directories(test-tts PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_link_libraries(test-tts PRIVATE
    drogon
    nlohmann_json::nlohmann_json
    sherpa-onnx-cxx-api
)

set_target_properties(test-tts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_TTS_OUTPUT_DIR}"
)
