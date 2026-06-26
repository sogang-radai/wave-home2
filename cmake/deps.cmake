if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/drogon/trantor/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing thirdparty/drogon/trantor. Run:\n"
        "  git submodule update --init --recursive thirdparty/drogon")
endif()
