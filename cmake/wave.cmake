# Wave Home CMake entry: third-party setup and toolchain tuning.

set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH
    "FetchContent download cache")
set(FETCHCONTENT_QUIET OFF CACHE BOOL "" FORCE)

include("${CMAKE_CURRENT_LIST_DIR}/options.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deps.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/platform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fetch/jsoncpp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/fetch/stb.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/asio.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tuyapp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drogon.cmake")
