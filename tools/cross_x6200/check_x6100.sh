#!/bin/bash
#
# The GUI's X6100 side compiled and linked on this computer: the X6200 sysroot (setup.sh) and the stub radio backend,
# so every shared source takes its X6100 paths (no BOARD_X6200), the X6100's own driver (src/hw/x6100) aside (its
# control library is not in the sysroot). A check after an upstream merge, before a tag builds the X6100's image.
#
#     tools/cross_x6200/check_x6100.sh          errors and warnings said; the exit status the build's
#
set -u
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6100chk
LOG=$B.log
RADE=$(grep -o 'RADE_PREFIX:PATH=.*' "$TOP/build/x6200/gui/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
OPTS="-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TOP/tools/cross_x6200/toolchain.cmake -DCMAKE_INSTALL_PREFIX=/usr"
OPTS="$OPTS -DHW_BACKEND=stub -DLV_CONF_BUILD_DISABLE_EXAMPLES=ON -DLV_CONF_BUILD_DISABLE_DEMOS=ON -DRADE_PREFIX=$RADE"
for o in BUILD_DOC BUILD_DOCS BUILD_EXAMPLE BUILD_EXAMPLES BUILD_TEST BUILD_TESTS BUILD_TESTING BUILD_SHARED_LIBS \
         BUILD_SHARED_LIB; do
    OPTS="$OPTS -D$o=OFF"
done
GEN=""
command -v ninja > /dev/null && GEN="-G Ninja"
[ -f "$B/CMakeCache.txt" ] || cmake -S "$TOP" -B "$B" $GEN $OPTS > "$LOG" 2>&1 || { cat "$LOG"; exit 1; }
cmake --build "$B" --target x6100_gui -j "$(sysctl -n hw.ncpu 2> /dev/null || nproc)" > "$LOG" 2>&1
rc=$?
grep -E "error:|warning:|undefined reference" "$LOG" | sed "s|$TOP/||" | sort -u
echo "X6100 side: $([ $rc = 0 ] && echo built || echo FAILED) (the full log: $LOG)"
exit $rc
