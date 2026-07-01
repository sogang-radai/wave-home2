include(FetchContent)

file(GLOB _stale_jsoncpp_configs
    "${CMAKE_BINARY_DIR}/_deps/*jsoncpp*/jsoncppConfig.cmake"
)
foreach(_config IN LISTS _stale_jsoncpp_configs)
    file(REMOVE "${_config}")
endforeach()

if(TARGET jsoncpp_static)
    return()
endif()

FetchContent_Declare(
    wave_jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG 1.9.6
    GIT_SHALLOW TRUE
)

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(wave_jsoncpp)
