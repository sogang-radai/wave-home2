if(NOT WAVE_BUILD_TTS)
    message(FATAL_ERROR "test-tts requires -DWAVE_BUILD_TTS=ON")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/sherpa_onnx.cmake")

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
    trantor
    nlohmann_json::nlohmann_json
    sherpa-onnx-cxx-api
)

if(WAVE_ARCH_AARCH64)
    target_compile_definitions(test-tts PRIVATE WAVE_ONNX_NEON=1)
endif()

set_target_properties(test-tts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_TTS_OUTPUT_DIR}"
)
