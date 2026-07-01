if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/drogon/trantor/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing thirdparty/drogon/trantor. Run:\n"
        "  git submodule update --init --recursive thirdparty/drogon")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/asio/asio/include/asio.hpp")
    message(FATAL_ERROR
        "Missing thirdparty/asio. Run:\n"
        "  git submodule update --init thirdparty/asio")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src/tuyaAPI.cpp")
    message(FATAL_ERROR
        "Missing thirdparty/tuyapp. Run:\n"
        "  git submodule update --init thirdparty/tuyapp")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/wave-home-front/.git")
    message(STATUS
        "wave-home-front (site source) submodule not initialized. "
        "Run: git submodule update --init wave-home-front")
endif()

if(APPLE)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        foreach(_pkg IN ITEMS jsoncpp libomp)
            execute_process(
                COMMAND ${BREW_EXECUTABLE} list --formula ${_pkg}
                RESULT_VARIABLE _brew_result
                OUTPUT_QUIET
                ERROR_QUIET
            )
            if(_brew_result)
                message(WARNING
                    "Homebrew package '${_pkg}' not installed. "
                    "Run: brew install ${_pkg}")
            endif()
        endforeach()
    else()
        message(STATUS
            "Homebrew not found on macOS. Install jsoncpp and libomp manually, "
            "or run: brew install cmake jsoncpp libomp")
    endif()
endif()
