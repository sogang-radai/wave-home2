add_executable(wave-server
    main.cpp
    wave_server.cpp
    api_controller.cpp
    ncnn_context.cpp
)

target_include_directories(wave-server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/thirdparty/ncnn/src"
)

target_compile_definitions(wave-server PRIVATE
    WH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(wave-server PRIVATE
    drogon
    ncnn
)

set_target_properties(wave-server PROPERTIES
    OUTPUT_NAME "wave-server"
)

add_custom_command(TARGET wave-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
        "$<TARGET_FILE_DIR:wave-server>/config.json"
    COMMENT "Copy wave-server config.json to output directory"
)
