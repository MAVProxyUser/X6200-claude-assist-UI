#!/bin/bash
#
# The X6100 GUI (x6100_gui, its own driver src/hw/x6100) cross-built on this computer, into build/x6100/gui, for a
# hot install on an X6100 running our image (install.sh RADIO build/x6100). The X6200's sysroot (setup.sh) serves:
# the two images share their libraries. Added to it once, from the X6100 named:
#   - the X6100 control library's headers, from gdyuldin/X6100Control at X6100CTL_COMMIT (its public headers are the
#     same from v0.16.1 to it; the api.h its CMake writes made by configuring it, its build needing libgpiod is not
#     run),
#   - the X6100's own libaether_x6100_control.so, copied from the radio, so the GUI links the library the radio has.
#
#     tools/cross_x6200/build_x6100.sh [root@x6100.local]
#
set -euo pipefail
RADIO=${1:-root@x6100.local}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
SYS=$TOP/build/x6200/sysroot
MA=$SYS/usr/lib/arm-linux-gnueabihf
INC=$SYS/usr/include/aether_radio/x6100_control
CTL=$TOP/build/x6100ctl
B=$TOP/build/x6100/gui
X6100CTL_URL=https://github.com/gdyuldin/X6100Control.git
X6100CTL_COMMIT=7a5bb8e5367f59003ef03af221361e186da10bf0

[ -d "$SYS/usr/include/aether_radio" ] || { echo "run tools/cross_x6200/setup.sh first (the X6200's sysroot)"; exit 1; }
if [ ! -f "$INC/api.h" ] || [ ! -f "$INC/low/flow.h" ]; then
    [ -d "$CTL/src/.git" ] || git clone --quiet "$X6100CTL_URL" "$CTL/src"
    git -C "$CTL/src" -c advice.detachedHead=false checkout --quiet "$X6100CTL_COMMIT"
    cmake -S "$CTL/src" -B "$CTL/bld" -DCMAKE_TOOLCHAIN_FILE="$TOP/tools/cross_x6200/toolchain.cmake" \
          -DCMAKE_BUILD_TYPE=Release > "$CTL/cfg.log" 2>&1 || { cat "$CTL/cfg.log"; exit 1; }
    mkdir -p "$INC/low"
    cp "$CTL/src/include/aether_radio/x6100_control/"*.h "$INC/"
    cp "$CTL/src/include/aether_radio/x6100_control/low/"*.h "$INC/low/"
    cp "$CTL/bld/include/aether_radio/x6100_control/api.h" "$INC/"
fi
if [ ! -e "$MA/libaether_x6100_control.so" ]; then
    lib=$(ssh -o BatchMode=yes "$RADIO" 'readlink -f /usr/lib/libaether_x6100_control.so.0')
    scp -q "$RADIO:$lib" "$MA/"
    ln -sf "$(basename "$lib")" "$MA/libaether_x6100_control.so.0"
    ln -sf libaether_x6100_control.so.0 "$MA/libaether_x6100_control.so"
fi

GEN=(); command -v ninja > /dev/null && GEN=(-G Ninja)
# A build folder configured before the toolchain had the image's hardening flags is configured again
[ -f "$B/CMakeCache.txt" ] && ! grep -q "_FORTIFY_SOURCE" "$B/CMakeCache.txt" && rm -f "$B/CMakeCache.txt"
if [ ! -f "$B/CMakeCache.txt" ]; then
    # build.sh's settings, the X6100's driver; RADE as the X6200's build has it (build.sh builds it)
    RADE=$(grep -o 'RADE_PREFIX:PATH=.*' "$TOP/build/x6200/gui/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
    OPTS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TOP/tools/cross_x6200/toolchain.cmake"
          -DCMAKE_INSTALL_PREFIX=/usr -DHW_BACKEND=x6100
          -DLV_CONF_BUILD_DISABLE_EXAMPLES=ON -DLV_CONF_BUILD_DISABLE_DEMOS=ON -DRADE_PREFIX="$RADE")
    for o in BUILD_DOC BUILD_DOCS BUILD_EXAMPLE BUILD_EXAMPLES BUILD_TEST BUILD_TESTS BUILD_TESTING \
             BUILD_SHARED_LIBS BUILD_SHARED_LIB; do
        OPTS+=(-D$o=OFF)
    done
    cmake -S "$TOP" -B "$B" "${GEN[@]}" "${OPTS[@]}"
fi
cmake --build "$B" --target x6100_gui -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ls -la "$B/src/x6100_gui"
