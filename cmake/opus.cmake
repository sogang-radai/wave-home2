# libopus (MicComp / SpkComp). Optional: PCM-only build when not found.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(WAVE_OPUS QUIET opus)
endif()

if(NOT WAVE_OPUS_FOUND)
    find_path(WAVE_OPUS_INCLUDE_DIR NAMES opus/opus.h
        HINTS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(WAVE_OPUS_LIBRARY NAMES opus
        HINTS /opt/homebrew/lib /usr/local/lib)
    if(WAVE_OPUS_INCLUDE_DIR AND WAVE_OPUS_LIBRARY)
        set(WAVE_OPUS_FOUND TRUE)
        set(WAVE_OPUS_INCLUDE_DIRS "${WAVE_OPUS_INCLUDE_DIR}")
        if(WAVE_OPUS_INCLUDE_DIRS MATCHES "/opus$")
            get_filename_component(WAVE_OPUS_INCLUDE_DIRS "${WAVE_OPUS_INCLUDE_DIRS}/.." ABSOLUTE)
        endif()
        set(WAVE_OPUS_LIBRARIES "${WAVE_OPUS_LIBRARY}")
    endif()
endif()

if(WAVE_OPUS_FOUND AND WAVE_OPUS_INCLUDE_DIRS MATCHES "/opus$")
    get_filename_component(WAVE_OPUS_INCLUDE_DIRS "${WAVE_OPUS_INCLUDE_DIRS}/.." ABSOLUTE)
endif()

function(wave_link_opus target)
    if(WAVE_OPUS_FOUND)
        set(_opus_include_dirs "${WAVE_OPUS_INCLUDE_DIRS}")
        if(_opus_include_dirs MATCHES "/opus$")
            get_filename_component(_opus_include_dirs "${_opus_include_dirs}/.." ABSOLUTE)
        endif()
        target_compile_definitions(${target} PRIVATE WAVE_HAS_OPUS=1)
        target_include_directories(${target} PRIVATE ${_opus_include_dirs})
        target_link_libraries(${target} PRIVATE ${WAVE_OPUS_LIBRARIES})
        message(STATUS "libopus found — WaveStation Opus audio enabled")
    else()
        message(WARNING
            "libopus not found — RadaiWs will use PCM audio only. "
            "Install: brew install opus (macOS) or apt install libopus-dev (Linux)")
    endif()
endfunction()
