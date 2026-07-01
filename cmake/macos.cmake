# Homebrew hints for Apple Silicon / macOS (libomp is keg-only).
if(NOT APPLE)
    return()
endif()

find_program(BREW_EXECUTABLE brew)
if(NOT BREW_EXECUTABLE)
    message(STATUS "brew not found; install libomp manually for NCNN OpenMP support")
    return()
endif()

execute_process(
    COMMAND ${BREW_EXECUTABLE} --prefix libomp
    OUTPUT_VARIABLE HOMEBREW_LIBOMP_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT HOMEBREW_LIBOMP_PREFIX OR NOT EXISTS "${HOMEBREW_LIBOMP_PREFIX}/lib/libomp.dylib")
    message(STATUS "libomp not found via Homebrew; NCNN will build without OpenMP")
    return()
endif()

set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I${HOMEBREW_LIBOMP_PREFIX}/include")
set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${HOMEBREW_LIBOMP_PREFIX}/include")
set(OpenMP_C_LIB_NAMES "omp")
set(OpenMP_CXX_LIB_NAMES "omp")
set(OpenMP_omp_LIBRARY "${HOMEBREW_LIBOMP_PREFIX}/lib/libomp.dylib")
