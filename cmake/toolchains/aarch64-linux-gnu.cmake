# Cross-compilation AArch64 (Raspberry Pi Zero 2 W, Pi OS Lite 64 bits).
# Usage : cmake -S . -B build-pi -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
# Le SDL2 aarch64 doit être dans le sysroot (PKG_CONFIG_SYSROOT_DIR / PKG_CONFIG_LIBDIR).
# Validation initiale recommandée : build NATIF sur le Pi (pas besoin de ce fichier).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -mcpu=cortex-a53")
