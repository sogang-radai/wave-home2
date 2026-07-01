option(WAVE_BUILD_SITE "Build wave-home-front into site/ before wave-server" OFF)

set(WAVE_SITE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/wave-home-front")
set(WAVE_SITE_DIR "${CMAKE_SOURCE_DIR}/site")

if(WAVE_BUILD_SITE AND EXISTS "${WAVE_SITE_SOURCE_DIR}/package.json")
    find_program(NPM_EXECUTABLE npm)
    if(NPM_EXECUTABLE)
        add_custom_target(wave-home-site
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-site.sh"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "npm build wave-home-front -> site/"
            VERBATIM
        )
        add_dependencies(wave-server wave-home-site)
    else()
        message(WARNING "npm not found; site will not be built")
    endif()
elseif(WAVE_BUILD_SITE)
    message(WARNING
        "wave-home-front (site source) not initialized. Run:\n"
        "  git submodule update --init wave-home-front\n"
        "site/ will not be rebuilt automatically.")
else()
    message(STATUS "WAVE_BUILD_SITE=OFF: skipping site npm build")
endif()
