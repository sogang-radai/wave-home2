# Submodule checks for large third-party trees (kept as git submodules).

macro(wave_require_path path hint)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${path}")
        message(FATAL_ERROR
            "Missing ${path}. Run:\n"
            "  git submodule update --init ${hint}")
    endif()
endmacro()

wave_require_path(
    "thirdparty/drogon/trantor/CMakeLists.txt"
    "--recursive thirdparty/drogon")
wave_require_path(
    "thirdparty/asio/asio/include/asio.hpp"
    "thirdparty/asio")
wave_require_path(
    "thirdparty/tuyapp/src/tuyaAPI.cpp"
    "thirdparty/tuyapp")
wave_require_path(
    "thirdparty/ncnn/CMakeLists.txt"
    "thirdparty/ncnn")
if(WAVE_BUILD_TTS)
    wave_require_path(
        "thirdparty/sherpa-onnx/CMakeLists.txt"
        "thirdparty/sherpa-onnx")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/wave-home-front/.git")
    message(STATUS
        "wave-home-front (site source) submodule not initialized. "
        "Run: git submodule update --init wave-home-front")
endif()

if(APPLE)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} list --formula libomp
            RESULT_VARIABLE _brew_result
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(_brew_result)
            message(WARNING
                "Homebrew package 'libomp' not installed. "
                "Run: brew install libomp")
        endif()
    else()
        message(STATUS
            "Homebrew not found on macOS. Install libomp manually, "
            "or run: brew install cmake libomp")
    endif()
endif()
