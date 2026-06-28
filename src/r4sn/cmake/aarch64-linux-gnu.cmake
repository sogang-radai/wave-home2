# Shared cross toolchain for S32R45 (aarch64 Linux).
# Compilers are passed by build-iq-server.sh via -DCMAKE_C/CXX_COMPILER.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

get_filename_component(R4SN_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(WAVE_HOME2_ROOT "${R4SN_CMAKE_DIR}/../../.." ABSOLUTE)
set(CMAKE_SYSROOT "${WAVE_HOME2_ROOT}/.toolchain/prefix")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Sysroot ships static libs only; avoid dynamic libm.so.6 probe during compiler check.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
