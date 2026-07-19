if(NOT WAVE_BUILD_TTS)
    message(FATAL_ERROR "test-stt requires -DWAVE_BUILD_TTS=ON")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/sherpa_onnx.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/opus.cmake")

set(TEST_STT_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-stt
    main.cpp
    mic_capture.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/service/stt_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/task_queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/exe_path.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/device.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/device/platform/radai_ws.cpp"
)

target_include_directories(test-stt PRIVATE
    "${CMAKE_SOURCE_DIR}/src/test/test-stt"
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_compile_definitions(test-stt PRIVATE
    WAVE_STANDALONE_DEVICE_TEST=1
    WAVE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test-stt PRIVATE
    wave::asio
    trantor
    nlohmann_json::nlohmann_json
    sherpa-onnx-cxx-api
)

wave_link_opus(test-stt)

if(APPLE)
    target_link_libraries(test-stt PRIVATE
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
endif()

if(WAVE_ARCH_AARCH64)
    target_compile_definitions(test-stt PRIVATE WAVE_ONNX_NEON=1)
endif()

set_target_properties(test-stt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_STT_OUTPUT_DIR}"
)
