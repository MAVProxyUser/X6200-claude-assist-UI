#!/bin/bash
# dsd-fme (lwvmobile/dsd-fme, ISC) for the X6200, its voice through DVSI hardware only: built against libmbe-dvsi
# (tools/dsdfme/mbe_dvsi: mbelib's calls answered by an AMBE-3000, the SDR daemon's USB-3000 on 127.0.0.1:2460), never
# mbelib (the owner, 2026-11-16: "hard no on the mbelib, USB-3000 is hard requirement"). Cross-built on the Mac with
# tools/cross_x6200's sysroot (run its setup.sh first: it has libsndfile, PulseAudio and ncurses for this).
#
#     tools/dsdfme/build.sh          -> build/x6200/dsdfme/out/dsd-fme
set -euo pipefail
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200/dsdfme
SYS=$TOP/build/x6200/sysroot
MA=$SYS/usr/lib/arm-linux-gnueabihf
REV=33d6cdf                                     # audio_work, 2026-11-16
CC=arm-linux-gnueabihf-gcc
CF="-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
[ -f "$SYS/usr/include/curses.h" ] || { echo "no curses.h in the sysroot: tools/cross_x6200/setup.sh again"; exit 1; }
mkdir -p "$B/prefix/lib" "$B/prefix/include" "$B/obj" "$B/out"

# libmbe-dvsi as libmbe, with its mbelib.h: dsd-fme's FindMBE takes it as mbelib
$CC --sysroot="$SYS" $CF -Wall -Wextra -c -o "$B/obj/mbe_dvsi.o" "$TOP/tools/dsdfme/mbe_dvsi/mbe_dvsi.c"
arm-linux-gnueabihf-ar rcs "$B/prefix/lib/libmbe.a" "$B/obj/mbe_dvsi.o"
cp "$TOP/tools/dsdfme/mbe_dvsi/mbelib.h" "$B/prefix/include/"

# dsd-fme's sources at REV
if [ ! -d "$B/src/.git" ]; then
    git clone -q -b audio_work https://github.com/lwvmobile/dsd-fme.git "$B/src"
fi
git -C "$B/src" fetch -q origin audio_work 2>/dev/null || true
git -C "$B/src" checkout -q -f "$REV"
# Ours (patches/goldcorp.patch): the talkgroup table at 65536 rows, every write into it bounded (16778000 rows asked
# for 1.8 GB, NULL on a 32-bit system, then written into on a talker alias); stdin read a buffer at a time (a sample a
# read(2) was two thirds of its time); DSD_NO_DMR in the environment leaves DMR to the SDR daemon's own decoder
git -C "$B/src" apply "$TOP/tools/dsdfme/patches/goldcorp.patch"
# The radio's ncurses is the narrow one (libncurses.so.6.4, no ncursesw): dsd-fme asks for the wide one
sed -i '' 's/set(CURSES_NEED_WIDE TRUE)/set(CURSES_NEED_WIDE FALSE)/' "$B/src/CMakeLists.txt"

cat > "$B/toolchain.cmake" <<TC
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_SYSROOT $SYS)
set(CMAKE_C_FLAGS "$CF -isystem $SYS/usr/include/arm-linux-gnueabihf" CACHE STRING "")
set(CMAKE_CXX_FLAGS "$CF -isystem $SYS/usr/include/arm-linux-gnueabihf" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-L$MA -Wl,-rpath-link,$MA -Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc" CACHE STRING "")
set(CMAKE_FIND_ROOT_PATH $SYS $B/prefix)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
TC
cmake -S "$B/src" -B "$B/cmake" -DCMAKE_TOOLCHAIN_FILE="$B/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$B/prefix" -DMBE_INCLUDE_DIR="$B/prefix/include" -DMBE_LIBRARY="$B/prefix/lib/libmbe.a" \
    -DCURSES_INCLUDE_PATH="$SYS/usr/include" -DCURSES_LIBRARY="$MA/libncurses.so" \
    -DCURSES_NCURSES_LIBRARY="$MA/libncurses.so" -DCOLORS=OFF > "$B/cmake.log" 2>&1 || { tail -30 "$B/cmake.log"; exit 1; }
cmake --build "$B/cmake" -j8 > "$B/build.log" 2>&1 || { grep -E "error|Error" "$B/build.log" | head -30; exit 1; }
cp "$B/cmake/dsd-fme" "$B/out/dsd-fme"
arm-linux-gnueabihf-strip -o "$B/out/dsd-fme.stripped" "$B/out/dsd-fme"
ls -la "$B/out"
