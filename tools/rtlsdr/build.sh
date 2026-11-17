#!/bin/bash
#
# Builds what the Sentry's VHF/UHF screens and AIRSPACE need for an SDR on the HOST port (an RTL-SDR, a HackRF, a
# bladeRF) on this computer, against the sysroot tools/cross_x6200/setup.sh made, into build/x6200/rtlsdr:
#   - librtlsdr from rtl-sdr-blog (it knows the RTL-SDR Blog V4's R828D as well as the R820T sticks), static, with
#     DETACH_KERNEL_DRIVER so a DVB-T driver holding the stick is let go;
#   - libhackrf (Great Scott Gadgets' hackrf, the tag of the HackRF firmware it is used with), static: only the
#     library, none of its tools (hackrf_transfer transmits);
#   - libbladeRF (Nuand's), shared: x6x00-rtlspec opens it at run time, so it runs without it; with its
#     analogdevicesinc submodule for the bladeRF 2.0. No FPGA images: Nuand's, put by hand in /mnt/bladerf (or the
#     board's own flash, bladeRF-cli -L) as docs/MANUAL.md says;
#   - rtl_test, to say which stick and tuner is plugged in;
#   - x6x00-rtlspec, the daemon that owns the device (tools/rtlspec; the radio's zlib for Meteor's PNGs);
#   - dump1090-fa (FlightAware), headless use only: its curses display is declared against the radio's own
#     libncurses.so.6 (curses.h below), as the sysroot has the library and no header.
# The radio's libusb-1.0 and ncurses are linked dynamically; libusb.h comes from libusb's 1.0.26 tag.
#
#     tools/rtlsdr/build.sh              then tools/rtlsdr/install.sh [root@radio]
#
set -euo pipefail
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200/rtlsdr
SR=$TOP/build/x6200/sysroot
DUMP1090_TAG=v11.1
LIBUSB_TAG=v1.0.26
HACKRF_TAG=v2024.02.1               # the firmware of the HackRF it was made for (2024.02.1); buildroot's package the same
BLADERF_TAG=libbladeRF_v2.6.0
[ -d "$SR/usr/lib/arm-linux-gnueabihf" ] || { echo "run tools/cross_x6200/setup.sh first"; exit 1; }
mkdir -p "$B/inc/libusb-1.0" "$B/out/lib" "$B/out/include" "$B/out/bin"

# rtl-sdr-blog at the commit the release's buildroot package takes, with the same patches (a stick pulled out
# mid-stream left libusb looping for ever in libusb_close)
RTLSDR_MK=$TOP/buildroot/br2_external/package/rtlsdr-blog/rtlsdr-blog.mk
RTLSDR_REV=$(sed -n 's/^RTLSDR_BLOG_VERSION = //p' "$RTLSDR_MK")
if [ ! -d "$B/rtl-sdr-blog" ]; then
    git clone -q https://github.com/rtlsdrblog/rtl-sdr-blog.git "$B/rtl-sdr-blog"
    git -C "$B/rtl-sdr-blog" checkout -q "$RTLSDR_REV"
fi
for p in "$(dirname "$RTLSDR_MK")"/*.patch; do
    git -C "$B/rtl-sdr-blog" apply --reverse --check "$p" 2> /dev/null || git -C "$B/rtl-sdr-blog" apply "$p"
done
[ -d "$B/hackrf" ] || git clone -q --depth 1 --branch "$HACKRF_TAG" https://github.com/greatscottgadgets/hackrf.git "$B/hackrf"
[ -d "$B/bladeRF" ] || git clone -q --depth 1 --branch "$BLADERF_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/Nuand/bladeRF.git "$B/bladeRF"
[ -d "$B/dump1090" ] || git clone -q --depth 1 --branch "$DUMP1090_TAG" https://github.com/flightaware/dump1090.git "$B/dump1090"
[ -f "$B/inc/libusb-1.0/libusb.h" ] ||
    curl -sSfL -o "$B/inc/libusb-1.0/libusb.h" "https://raw.githubusercontent.com/libusb/libusb/$LIBUSB_TAG/libusb/libusb.h"

cat > "$B/inc/curses.h" << 'EOF'
/* The curses calls of dump1090-fa's interactive display, declared against the radio's libncurses.so.6 (ncurses 6.4,
 * chtype 32 bits); the display is never used there: AIRSPACE runs dump1090 headless */
#pragma once
typedef unsigned int chtype;
typedef struct _win_st WINDOW;
extern WINDOW *stdscr;
extern chtype acs_map[];
#define ACS_HLINE (acs_map[(unsigned char)'q'])
WINDOW *initscr(void);
int endwin(void);
int mvprintw(int y, int x, const char *fmt, ...);
int mvaddch(int y, int x, const chtype ch);
int mvhline(int y, int x, chtype ch, int n);
int refresh(void);
int move(int y, int x);
int clear(void);
int clrtobot(void);
int getmaxy(const WINDOW *win);
EOF

CC="arm-linux-gnueabihf-gcc --sysroot=$SR -isystem $SR/usr/include/arm-linux-gnueabihf"
LD="-L$SR/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,$SR/usr/lib/arm-linux-gnueabihf -Wl,--allow-shlib-undefined"

# librtlsdr and rtl_test
R=$B/rtl-sdr-blog
CF="-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DDETACH_KERNEL_DRIVER -I $R/include -I $B/inc/libusb-1.0"
for f in librtlsdr tuner_e4k tuner_fc0012 tuner_fc0013 tuner_fc2580 tuner_r82xx; do
    $CC $CF -c -o "$B/out/$f.o" "$R/src/$f.c"
done
arm-linux-gnueabihf-ar rcs "$B/out/lib/librtlsdr.a" "$B/out/librtlsdr.o" "$B"/out/tuner_*.o
cp "$R/include/rtl-sdr.h" "$R/include/rtl-sdr_export.h" "$B/out/include/"
$CC $CF -c -o "$B/out/convenience.o" "$R/src/convenience/convenience.c"
$CC $CF -o "$B/out/bin/rtl_test" "$R/src/rtl_test.c" "$B/out/convenience.o" "$B/out/lib/librtlsdr.a" $LD -lusb-1.0 -lpthread -lm
# libhackrf, static
H=$B/hackrf/host/libhackrf/src
$CC $CF -std=gnu11 -I "$H" -c -o "$B/out/hackrf.o" "$H/hackrf.c"     # its own "typedef int bool": not C23
arm-linux-gnueabihf-ar rcs "$B/out/lib/libhackrf.a" "$B/out/hackrf.o"
cp "$H/hackrf.h" "$B/out/include/"
# libbladeRF, shared (its own CMake, cross): libbladeRF.so.2 for /usr/lib on the radio
BL=$B/bladeRF/host
if [ ! -f "$B/bladerf-build/output/libbladeRF.so.2" ]; then
    # the library alone: its tools (bladeRF-cli, bladeRF-update with curl) are not wanted, and have no switch
    sed -i.orig '/^add_subdirectory(utilities)$/d' "$BL/CMakeLists.txt"
    mkdir -p "$B/bladerf-build"
    cmake -S "$BL" -B "$B/bladerf-build" -DCMAKE_TOOLCHAIN_FILE="$TOP/tools/cross_x6200/toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCUMENTATION=OFF -DENABLE_BACKEND_LIBUSB=ON -DENABLE_BACKEND_CYAPI=OFF \
        -DENABLE_BACKEND_DUMMY=OFF -DTEST_LIBBLADERF=OFF -DBUILD_BLADERF_CLI=OFF -DBUILD_BLADERF_UTILITIES=OFF \
        -DENABLE_LIBTECLA=OFF -DINSTALL_UDEV_RULES=OFF -DTREAT_WARNINGS_AS_ERRORS=OFF \
        -DLIBUSB_HEADER_FILE="$B/inc/libusb-1.0/libusb.h" -Dusb_LIBRARY="$SR/usr/lib/arm-linux-gnueabihf/libusb-1.0.so" \
        -DLIBUSB_SKIP_VERSION_CHECK=Yes -DCMAKE_C_FLAGS="-I$B/inc/libusb-1.0 -std=gnu11" > "$B/bladerf-build.log" 2>&1 ||
        { tail -30 "$B/bladerf-build.log"; exit 1; }
    make -C "$B/bladerf-build/libraries/libbladeRF" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >> "$B/bladerf-build.log" 2>&1 ||
        { tail -30 "$B/bladerf-build.log"; exit 1; }
fi
cp -L "$B/bladerf-build/output/libbladeRF.so.2" "$B/out/lib/libbladeRF.so.2"
# x6x00-rtlspec, the Sentry's VHF/UHF spectrum (tools/rtlspec): librtlsdr and libhackrf in it, libbladeRF opened at
# run time (its header only here)
RS=$TOP/tools/rtlspec
$CC $CF -O3 -DSDR_BLADERF -I "$RS" -I "$H" -I "$BL/libraries/libbladeRF/include" -o "$B/out/bin/x6x00-rtlspec" \
    "$RS/rtlspec.c" "$RS/same.c" "$RS/rds.c" "$RS/acars.c" "$RS/ambe.c" "$RS/dmr.c" "$RS/dsd.c" "$RS/lrpt.c" "$RS/sdr.c" "$RS/sdr_rtl.c" "$RS/sdr_hackrf.c" "$RS/sdr_bladerf.c" "$RS/sdr_file.c" \
    "$B/out/lib/librtlsdr.a" "$B/out/lib/libhackrf.a" $LD -lusb-1.0 -lz -lpthread -lm -ldl

# dump1090-fa: ARCH armv7l picks its NEON DSP flavours for the radio's Cortex-A7
rm -f "$B/dump1090/dump1090"      # linked again: its Makefile does not see librtlsdr.a change
make -C "$B/dump1090" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" dump1090 CC="$CC" UNAME=Linux ARCH=armv7l \
    RTLSDR=yes RTLSDR_PREFIX="$B/out" STATIC=yes BLADERF=no HACKRF=no LIMESDR=no SOAPYSDR=no \
    CPPFLAGS="-I$B/inc -I$B/inc/libusb-1.0" CFLAGS="-O3 -g" LDFLAGS="$LD"
cp "$B/dump1090/dump1090" "$B/out/bin/dump1090-fa"
ls -la "$B/out/bin"
