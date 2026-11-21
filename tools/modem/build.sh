#!/bin/sh
# x6x00-modem for the radios, cross-built on this Mac (the X6200 sysroot of tools/cross_x6200/setup.sh: the same
# armhf glibc serves both radios), and a hot install for testing (the image's own comes from buildroot,
# package x6x00-modem).
#     tools/modem/build.sh             build/x6x00-modem.arm
#     tools/modem/build.sh install     and to /tmp/x6x00-modem on both radios (gone at their next restart)
set -e
cd "$(dirname "$0")"
S=../../build/x6200/sysroot
[ -d "$S/usr/include" ] || { echo "no $S: tools/cross_x6200/setup.sh first"; exit 1; }
mkdir -p build
arm-linux-gnueabihf-gcc --sysroot=$S -isystem $S/usr/include/arm-linux-gnueabihf -B$S/usr/lib/arm-linux-gnueabihf \
    -L$S/usr/lib/arm-linux-gnueabihf -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -Wall -Wextra \
    -o build/x6x00-modem.arm x6x00-modem.c dsp.c v21.c v27ter.c v29.c qamrx.c pump.c hdlc.c link.c -lm -static-libgcc
arm-linux-gnueabihf-strip build/x6x00-modem.arm
echo "build/x6x00-modem.arm: $(wc -c < build/x6x00-modem.arm) bytes"
if [ "$1" = install ]; then
    for r in root@x6100.local root@x6200.local; do
        scp -q build/x6x00-modem.arm "$r:/tmp/x6x00-modem.new" && ssh "$r" 'mv /tmp/x6x00-modem.new /tmp/x6x00-modem' &&
            echo "$r: /tmp/x6x00-modem"
    done
fi
