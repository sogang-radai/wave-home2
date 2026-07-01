set(TEST_LLM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/test")

add_executable(test-llm
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/wave-server/service/llm_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/core/task_queue.cpp"
    "${CMAKE_SOURCE_DIR}/src/wave-server/auxiliary/stb_image.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/spinner.cpp"
    "${CMAKE_SOURCE_DIR}/src/common/util/arg_parser.cpp"
)

target_include_directories(test-llm PRIVATE
    "${CMAKE_SOURCE_DIR}/src/wave-server"
    "${CMAKE_SOURCE_DIR}/src/common"
)

include(FetchContent)
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

target_include_directories(test-llm PRIVATE "${WAVE_STB_INCLUDE_DIR}")

target_link_libraries(test-llm PRIVATE drogon nlohmann_json::nlohmann_json)

set_target_properties(test-llm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_LLM_OUTPUT_DIR}"
)
