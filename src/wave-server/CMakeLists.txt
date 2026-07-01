add_executable(wave-server
    main.cpp
    app/app_state.cpp
    app/app_setting.cpp
    core/logger.cpp
    core/task_queue.cpp
    service/llm_client.cpp
    service/tts_service.cpp
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

include("${CMAKE_SOURCE_DIR}/cmake/ncnn_minimal.cmake")
add_subdirectory("${CMAKE_SOURCE_DIR}/thirdparty/ncnn" "${CMAKE_BINARY_DIR}/thirdparty/ncnn")

target_include_directories(wave-server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/common"
)

target_compile_definitions(wave-server PRIVATE
    WAVE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(wave-server PRIVATE
    drogon
    wave::asio
    wave::tuyapp
    ncnn
    sherpa-onnx-cxx-api
)

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/device/platform/tuya_ep2h.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src"
)

target_link_libraries(wave-server PRIVATE nlohmann_json::nlohmann_json)

if(EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/stb/stb_image.h")
    set(WAVE_STB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/stb")
else()
    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(stb)
    set(WAVE_STB_INCLUDE_DIR "${stb_SOURCE_DIR}")
endif()

target_include_directories(wave-server PRIVATE "${WAVE_STB_INCLUDE_DIR}")

set_target_properties(wave-server PROPERTIES
    OUTPUT_NAME "wave-server"
)
