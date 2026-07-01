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

find_path(JSONCPP_INCLUDE_DIR json/json.h
    HINTS /opt/homebrew/include /usr/local/include
)
find_library(JSONCPP_LIBRARY NAMES jsoncpp
    HINTS /opt/homebrew/lib /usr/local/lib
)

if(NOT JSONCPP_INCLUDE_DIR OR NOT JSONCPP_LIBRARY)
    message(FATAL_ERROR "jsoncpp not found. On macOS: brew install jsoncpp")
endif()

target_include_directories(wave_tuyapp SYSTEM PRIVATE ${JSONCPP_INCLUDE_DIR})
target_link_libraries(wave_tuyapp PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ${JSONCPP_LIBRARY}
)
