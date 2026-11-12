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
if [ ! -f "$B/CMakeCache.txt" ]; then
    # buildroot/build.sh's settings, for the X6200
    OPTS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TOP/tools/cross_x6200/toolchain.cmake"
          -DCMAKE_INSTALL_PREFIX=/usr -DHW_BACKEND=x6200
          -DLV_CONF_BUILD_DISABLE_EXAMPLES=ON -DLV_CONF_BUILD_DISABLE_DEMOS=ON)
    for o in BUILD_DOC BUILD_DOCS BUILD_EXAMPLE BUILD_EXAMPLES BUILD_TEST BUILD_TESTS BUILD_TESTING \
             BUILD_SHARED_LIBS BUILD_SHARED_LIB; do
        OPTS+=(-D$o=OFF)
    done
    cmake -S "$TOP" -B "$B" "${GEN[@]}" "${OPTS[@]}"
fi
cmake --build "$B" --target x6100_gui -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ls -la "$B/src/x6100_gui"
