#!/bin/bash
#
# Builds the X6200 GUI (x6100_gui) on this computer, against the sysroot setup.sh made, into build/x6200/gui.
#
#     tools/cross_x6200/build.sh           # configure (once) and build
#     tools/cross_x6200/build.sh clean
#
set -euo pipefail
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200/gui
[ "${1:-}" = clean ] && { rm -rf "$B"; exit 0; }
[ -d "$TOP/build/x6200/sysroot/usr/include/aether_radio" ] || { echo "run tools/cross_x6200/setup.sh first"; exit 1; }
GEN=(); command -v ninja > /dev/null && GEN=(-G Ninja)
# RADE, the RADE app's modem (tools/rade/build.sh, built once and cached): without it the app has no decoder, as every
# GUI built here had until 2026-11-17. It needs autoconf, automake and libtool (brew install autoconf automake libtool)
RADE=""
if command -v autoreconf > /dev/null && command -v automake > /dev/null \
        && { command -v libtoolize > /dev/null || command -v glibtoolize > /dev/null; }; then
    RADE=$(CC=arm-linux-gnueabihf-gcc "$TOP/tools/rade/build.sh") || { echo "tools/rade/build.sh failed"; exit 1; }
else
    echo "WARNING: no autoconf/automake/libtoolize: the GUI is built WITHOUT RADE (the RADE app has no decoder)" >&2
fi
if [ -f "$B/CMakeCache.txt" ] && ! grep -qx "RADE_PREFIX:PATH=$RADE" "$B/CMakeCache.txt"; then
    cmake -S "$TOP" -B "$B" -DRADE_PREFIX="$RADE"
fi
# A build folder configured before the toolchain had the image's hardening flags is configured again
[ -f "$B/CMakeCache.txt" ] && ! grep -q "_FORTIFY_SOURCE" "$B/CMakeCache.txt" && rm -f "$B/CMakeCache.txt"
if [ ! -f "$B/CMakeCache.txt" ]; then
    # buildroot/build.sh's settings, for the X6200
    OPTS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TOP/tools/cross_x6200/toolchain.cmake"
          -DCMAKE_INSTALL_PREFIX=/usr -DHW_BACKEND=x6200
          -DLV_CONF_BUILD_DISABLE_EXAMPLES=ON -DLV_CONF_BUILD_DISABLE_DEMOS=ON -DRADE_PREFIX="$RADE")
    for o in BUILD_DOC BUILD_DOCS BUILD_EXAMPLE BUILD_EXAMPLES BUILD_TEST BUILD_TESTS BUILD_TESTING \
             BUILD_SHARED_LIBS BUILD_SHARED_LIB; do
        OPTS+=(-D$o=OFF)
    done
    cmake -S "$TOP" -B "$B" "${GEN[@]}" "${OPTS[@]}"
fi
cmake --build "$B" --target x6100_gui -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ls -la "$B/src/x6100_gui"
