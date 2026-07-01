if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/asio/asio/include/asio.hpp")
    message(FATAL_ERROR
        "Missing thirdparty/asio. Run:\n"
        "  git submodule update --init thirdparty/asio")
endif()

add_library(wave_asio INTERFACE)
add_library(wave::asio ALIAS wave_asio)

target_include_directories(wave_asio SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/thirdparty/asio/asio/include"
)
target_compile_definitions(wave_asio INTERFACE ASIO_STANDALONE)
target_compile_features(wave_asio INTERFACE cxx_std_17)
