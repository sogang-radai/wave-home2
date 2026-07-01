include(FetchContent)

if(TARGET wave_stb)
    return()
endif()

FetchContent_Declare(
    wave_stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(wave_stb)

set(WAVE_STB_INCLUDE_DIR "${wave_stb_SOURCE_DIR}" CACHE INTERNAL
    "stb headers from FetchContent")

add_library(wave_stb INTERFACE)
add_library(wave::stb ALIAS wave_stb)

target_include_directories(wave_stb SYSTEM INTERFACE "${WAVE_STB_INCLUDE_DIR}")

if(WAVE_ARCH_AARCH64)
    target_compile_definitions(wave_stb INTERFACE STBI_NEON)
endif()

# Pin stb_image.cpp to FetchContent headers (ncnn also ships stb_image.h).
function(wave_use_wave_stb source_file)
    set_source_files_properties("${source_file}" PROPERTIES
        INCLUDE_DIRECTORIES "${WAVE_STB_INCLUDE_DIR}"
    )
    if(WAVE_ARCH_AARCH64)
        set_source_files_properties("${source_file}" PROPERTIES
            COMPILE_DEFINITIONS STBI_NEON
        )
    endif()
endfunction()
