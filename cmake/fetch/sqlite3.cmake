include(FetchContent)

if(TARGET wave_sqlite3)
    return()
endif()

FetchContent_Declare(
    wave_sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
)

FetchContent_MakeAvailable(wave_sqlite3)

add_library(wave_sqlite3 STATIC "${wave_sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(wave_sqlite3 PUBLIC "${wave_sqlite3_SOURCE_DIR}")
target_compile_definitions(wave_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION=1
)
set_target_properties(wave_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(wave_sqlite3 PUBLIC Threads::Threads)
endif()
