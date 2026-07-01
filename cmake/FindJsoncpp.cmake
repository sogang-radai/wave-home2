include("${CMAKE_CURRENT_LIST_DIR}/fetch/jsoncpp.cmake")

set(JSONCPP_INCLUDE_DIRS "${wave_jsoncpp_SOURCE_DIR}/include")
set(JSONCPP_LIBRARIES jsoncpp_static)

if(NOT TARGET Jsoncpp_lib)
    add_library(Jsoncpp_lib INTERFACE IMPORTED)
    set_target_properties(Jsoncpp_lib PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${JSONCPP_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES jsoncpp_static
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Jsoncpp
    DEFAULT_MSG
    JSONCPP_INCLUDE_DIRS
    JSONCPP_LIBRARIES
)
mark_as_advanced(JSONCPP_INCLUDE_DIRS JSONCPP_LIBRARIES)
