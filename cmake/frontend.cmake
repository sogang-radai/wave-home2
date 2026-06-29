option(WH_BUILD_FRONTEND "Build wave-home-front into site/ before wave-server" ON)

set(WH_FRONTEND_DIR "${CMAKE_SOURCE_DIR}/wave-home-front")
set(WH_SITE_DIR "${CMAKE_SOURCE_DIR}/site")

if(WH_BUILD_FRONTEND AND EXISTS "${WH_FRONTEND_DIR}/package.json")
    find_program(NPM_EXECUTABLE npm)
    if(NPM_EXECUTABLE)
        add_custom_target(wave-home-site
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-front.sh"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "npm build wave-home-front -> site/"
            VERBATIM
        )
        add_dependencies(wave-server wave-home-site)
    else()
        message(WARNING "npm not found; wave-home-front will not be built")
    endif()
elseif(WH_BUILD_FRONTEND)
    message(WARNING
        "wave-home-front not initialized. Run:\n"
        "  git submodule update --init wave-home-front\n"
        "site/ will not be rebuilt automatically.")
else()
    message(STATUS "WH_BUILD_FRONTEND=OFF: skipping wave-home-front npm build")
endif()
