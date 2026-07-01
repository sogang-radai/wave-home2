# Prefer bundled sqlite amalgamation; fall back to system libsqlite3.

include("${CMAKE_CURRENT_LIST_DIR}/fetch/sqlite3.cmake")

if(TARGET wave_sqlite3)
    set(SQLite3_FOUND TRUE)
    set(SQLITE3_INCLUDE_DIRS "${wave_sqlite3_SOURCE_DIR}")
    set(SQLITE3_LIBRARIES wave_sqlite3)

    if(NOT TARGET SQLite3_lib)
        add_library(SQLite3_lib INTERFACE IMPORTED)
        set_target_properties(SQLite3_lib PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES wave_sqlite3
        )
    endif()

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(SQLite3
        DEFAULT_MSG
        SQLITE3_LIBRARIES
        SQLITE3_INCLUDE_DIRS
    )
else()
    include("${CMAKE_SOURCE_DIR}/thirdparty/drogon/cmake_modules/FindSQLite3.cmake")
endif()
