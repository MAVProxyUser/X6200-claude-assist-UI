#!/bin/bash
#
# The GUI for Xiegu's GSOC head driving a G90 body (cjlawson02's port, 2026-11-18: HW_BACKEND=g90, BOARD_GSOC's
# 1024 x 600 landscape panel, libg90 from third-party/libg90) compiled and linked on this computer with the X6200's
# sysroot (setup.sh): the GSOC is an Allwinner A20, ARMv7 hard-float like the X6200's A33, so this checks that the
# G90 backend and every landscape path build, not that it runs (that needs a GSOC and a G90 on the bench). Its
# GSOC-only options too: G90_FUSE_IQ, G90_AF_ANALOG, G90_IQ_MMAP.
#
#     tools/cross_x6200/check_g90.sh          errors and warnings said; the exit status the build's
#
set -u
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/g90chk
LOG=$B.log
RADE=$(grep -o 'RADE_PREFIX:PATH=.*' "$TOP/build/x6200/gui/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
OPTS="-DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TOP/tools/cross_x6200/toolchain.cmake -DCMAKE_INSTALL_PREFIX=/usr"
OPTS="$OPTS -DHW_BACKEND=g90 -DBOARD_GSOC=ON -DLV_CONF_BUILD_DISABLE_EXAMPLES=ON -DLV_CONF_BUILD_DISABLE_DEMOS=ON"
OPTS="$OPTS -DRADE_PREFIX=$RADE"
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
echo "GSOC/G90: $([ $rc = 0 ] && echo built || echo FAILED) (the full log: $LOG)"
exit $rc
