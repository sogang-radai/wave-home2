set(_wave_server_sources
    main.cpp
    app/app_state.cpp
    app/app_config.cpp
    app/app_setting.cpp
    core/logger.cpp
    core/task_queue.cpp
    core/time_util.cpp
    db/database.cpp
    service/llm_client.cpp
    service/go2rtc_service.cpp
    web/server.cpp
    web/http/api_controller.cpp
    web/http/v1/health_controller.cpp
    web/http/v1/session_controller.cpp
    web/http/v1/session_store.cpp
    web/http/v1/accounts_controller.cpp
    web/http/v1/push_controller.cpp
    web/http/v1/settings_controller.cpp
    web/http/v1/settings_store.cpp
    web/http/push_notification.cpp
    web/push/web_push.cpp
    web/push/push_store.cpp
    device/device.cpp
    device/device_manager.cpp
    device/platform/srs_r4sn.cpp
    device/platform/tuya_ep2h.cpp
    device/platform/reolink_e1pro.cpp
    device/platform/philips_wiz_e29.cpp
    device/platform/samsung_tizen.cpp
    device/platform/ir_device.cpp
    device/protocol/ir_remote.cpp
    nn/frame_encoder.cpp
    nn/frame_aggregator.cpp
    auxiliary/stb_image.cpp
    "${CMAKE_SOURCE_DIR}/src/common/util/exe_path.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/network/net_util.cpp"
)

if(WAVE_BUILD_TTS)
    list(APPEND _wave_server_sources service/tts_service.cpp)
endif()

add_executable(wave-server ${_wave_server_sources})

include("${CMAKE_SOURCE_DIR}/cmake/ncnn.cmake")

find_package(OpenSSL REQUIRED)

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

target_link_libraries(wave-server PRIVATE ${_wave_server_libs} OpenSSL::SSL OpenSSL::Crypto)
if(APPLE)
    target_link_libraries(wave-server PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/device/platform/tuya_ep2h.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src"
)

set_target_properties(wave-server PROPERTIES
    OUTPUT_NAME "wave-server"
)
