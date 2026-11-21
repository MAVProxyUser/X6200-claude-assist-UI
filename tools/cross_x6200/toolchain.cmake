# The X6200 GUI cross-built against build/x6200/sysroot (setup.sh): Homebrew's arm-unknown-linux-gnueabihf GCC,
# Ubuntu's armhf headers, the radio's own libraries.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
get_filename_component(_top "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_SYSROOT "${_top}/build/x6200/sysroot")
set(_ma "${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${_ma}/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
# Ubuntu's multiarch headers (jconfig.h, ...) are not on this GCC's own search path. And glibc's _FORTIFY_SOURCE=1, as
# buildroot builds the image (its default): a build here trips the same checks as the release. Without it a realpath()
# into 256 bytes ran fine here and ended v0.3.12.41's GUI on the X6100 at every start ("buffer overflow detected"),
# rolled back by its updater. Not -fstack-protector (the image's BR2_SSP_REGULAR): this toolchain takes it from GCC's
# libssp.so.0, which the radios do not have (the GUI did not start, 2026-11-21); buildroot's uses glibc's own
set(_x6_hardening "-D_FORTIFY_SOURCE=1")
set(CMAKE_C_FLAGS_INIT "-isystem ${CMAKE_SYSROOT}/usr/include/arm-linux-gnueabihf ${_x6_hardening}")
set(CMAKE_CXX_FLAGS_INIT "-isystem ${CMAKE_SYSROOT}/usr/include/arm-linux-gnueabihf ${_x6_hardening}")
# The radio's libraries are newer than this glibc 2.28 (they want GLIBC_2.3x): their own undefined symbols are the
# radio's glibc's business. libstdc++ and libgcc go in static: GCC 15's runtime is newer than the radio's GCC 12
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L${_ma} -Wl,-rpath-link,${_ma} -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L${_ma} -Wl,-rpath-link,${_ma}")
# glibc 2.28 keeps dlsym in libdl (2.34 moved it into libc; the radio's 2.36 still has libdl.so.2 for programs like this)
set(CMAKE_C_STANDARD_LIBRARIES "-ldl" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "-ldl" CACHE STRING "" FORCE)
