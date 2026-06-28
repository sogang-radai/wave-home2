# Python clients — no build step; run:
#   python3 src/test/test-iq/cli-tool.py --help
#   python3 src/test/test-iq/gui.py

add_custom_target(test-iq
    COMMAND "${CMAKE_COMMAND}" -E echo
        "Run: python3 ${CMAKE_CURRENT_SOURCE_DIR}/cli-tool.py --help  |  python3 ${CMAKE_CURRENT_SOURCE_DIR}/gui.py"
    VERBATIM
)
