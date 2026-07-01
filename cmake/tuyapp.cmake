if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src/tuyaAPI.cpp")
    message(FATAL_ERROR
        "Missing thirdparty/tuyapp. Run:\n"
        "  git submodule update --init thirdparty/tuyapp")
endif()

file(GLOB WAVE_TUYAPP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src/*.cpp"
)

add_library(wave_tuyapp STATIC ${WAVE_TUYAPP_SOURCES})
add_library(wave::tuyapp ALIAS wave_tuyapp)

target_compile_definitions(wave_tuyapp PRIVATE
    WITHOUT_API31
    WITHOUT_API35
)

target_include_directories(wave_tuyapp PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/tuyapp/src"
)

find_package(OpenSSL REQUIRED)

target_link_libraries(wave_tuyapp PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)
