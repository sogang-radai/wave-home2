include(FetchContent)

if(TARGET wave_sqlite_vec)
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/sqlite3.cmake")

FetchContent_Declare(
    wave_sqlite_vec
    URL https://github.com/asg017/sqlite-vec/releases/download/v0.1.9/sqlite-vec-0.1.9-amalgamation.zip
)

FetchContent_MakeAvailable(wave_sqlite_vec)

add_library(wave_sqlite_vec STATIC "${wave_sqlite_vec_SOURCE_DIR}/sqlite-vec.c")
target_include_directories(wave_sqlite_vec PUBLIC "${wave_sqlite_vec_SOURCE_DIR}")
target_compile_definitions(wave_sqlite_vec PRIVATE
    SQLITE_CORE
    SQLITE_VEC_STATIC
)
target_link_libraries(wave_sqlite_vec PUBLIC wave_sqlite3)
set_target_properties(wave_sqlite_vec PROPERTIES POSITION_INDEPENDENT_CODE ON)
