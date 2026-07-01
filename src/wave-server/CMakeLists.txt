set(_wave_server_sources
    main.cpp
    app/app_state.cpp
    app/app_setting.cpp
    core/logger.cpp
    core/task_queue.cpp
    service/llm_client.cpp
    web/server.cpp
    web/http/api_controller.cpp
    web/http/push_notification.cpp
    device/device.cpp
    device/device_manager.cpp
    device/platform/srs_r4sn.cpp
    device/platform/tuya_ep2h.cpp
    device/platform/ir_device.cpp
    device/protocol/ir_remote.cpp
    nn/frame_encoder.cpp
    nn/frame_aggregator.cpp
    auxiliary/stb_image.cpp
    "${CMAKE_SOURCE_DIR}/src/common/util/exe_path.cpp"
)

if(WAVE_BUILD_TTS)
    list(APPEND _wave_server_sources service/tts_service.cpp)
endif()

add_executable(wave-server ${_wave_server_sources})

include("${CMAKE_SOURCE_DIR}/cmake/ncnn.cmake")

wave_use_wave_stb("${CMAKE_CURRENT_SOURCE_DIR}/auxiliary/stb_image.cpp")

target_include_directories(wave-server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_compile_definitions(wave-server PRIVATE
    WAVE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(WAVE_ARCH_AARCH64)
    target_compile_definitions(wave-server PRIVATE WAVE_ONNX_NEON=1)
endif()

set(_wave_server_libs drogon wave::asio wave::tuyapp ncnn nlohmann_json::nlohmann_json)
if(WAVE_BUILD_TTS)
    include("${CMAKE_SOURCE_DIR}/cmake/sherpa_onnx.cmake")
    list(APPEND _wave_server_libs sherpa-onnx-cxx-api)
endif()

target_link_libraries(wave-server PRIVATE ${_wave_server_libs})

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/device/platform/tuya_ep2h.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src"
)

set_target_properties(wave-server PROPERTIES
    OUTPUT_NAME "wave-server"
)
