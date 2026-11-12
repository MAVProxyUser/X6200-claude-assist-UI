#!/bin/bash
#
# The X6200 GUI cross-built on a Mac (or any PC) without the buildroot tree: a sysroot made of
#   - the cross toolchain's own C library (crosstool-NG's glibc 2.28, older than the radio's 2.36, so what is
#     linked against it runs there; Homebrew's arm-unknown-linux-gnueabihf, GCC 15.2),
#   - headers and pkg-config files from Ubuntu's armhf -dev packages (fetch_debs.py),
#   - the radio's own shared libraries, copied from the radio over ssh, so the libraries linked are the ones it runs,
#   - the headers of the radio's own liquid-dsp, ft8_lib and X6200 control library, at the versions it has.
# Run once (again after the radio's image changes); then build.sh builds the GUI.
#
#     tools/cross_x6200/setup.sh [root@192.168.0.224]
#
set -euo pipefail
RADIO=${1:-root@192.168.0.224}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
W=$TOP/build/x6200
SYS=$W/sysroot
MA=$SYS/usr/lib/arm-linux-gnueabihf
CC=arm-linux-gnueabihf-gcc
LIQUID_VER=1.4.0                                        # host/build.sh's, the radio's libliquid.so
FT8LIB_REV=73b0db861dc41c7ca9f3dedc73ee148daa06dd0e     # host/build.sh's, the radio's libft8.so
X6200_CONTROL_TAG=v0.1.1                                # tools/hw_x6200/Makefile: the image's

mkdir -p "$W/debs" "$W/src"
command -v $CC > /dev/null || { echo "no $CC: brew install arm-unknown-linux-gnueabihf"; exit 1; }
command -v zstd > /dev/null || { echo "no zstd: brew install zstd"; exit 1; }

echo "== the radio's libraries ($RADIO)"
mkdir -p "$W/radio-root"
ssh -o BatchMode=yes "$RADIO" 'tar -C / -cf - lib usr/lib usr/sbin/x6100_gui 2>/dev/null' | tar -C "$W/radio-root" -xf -

echo "== Ubuntu headers"
python3 -I "$TOP/tools/cross_x6200/fetch_debs.py" "$W/debs" > /dev/null

echo "== sysroot"
rm -rf "$SYS" && mkdir -p "$SYS"
cp -a "$($CC -print-sysroot)/." "$SYS/"
chmod -R u+w "$SYS"
for d in "$W"/debs/*.deb; do
    t=$(mktemp -d)
    (cd "$t" && ar x "$d" && for f in data.tar.*; do
        case $f in
            *.zst) zstd -dcq "$f" | tar -C "$SYS" -xf - ;;
            *)     tar -C "$SYS" -xf "$f" ;;
        esac
    done)
    rm -rf "$t"
done
# The -dev packages' lib*.so point at Ubuntu's runtime files, which are not here: the radio's take their place.
# glibc, libgcc and libstdc++ stay the toolchain's (the GUI takes libstdc++ static: GCC 15's is newer than the radio's)
find "$MA" -maxdepth 1 -type l ! -exec test -e {} \; -delete
for f in "$W"/radio-root/usr/lib/*.so* "$W"/radio-root/lib/*.so*; do
    case $(basename "$f") in
        libc.so*|libc-*|ld-linux*|libm.so*|libm-*|libpthread*|libdl.so*|libdl-*|librt.so*|librt-*|libresolv*|\
        libutil.so*|libanl*|libnsl*|libBrokenLocale*|libgcc_s*|libstdc++*|libmvec*|libthread_db*) continue ;;
    esac
    cp -a "$f" "$MA/"
done
# The radio's links into /lib (libudev.so -> ../../lib/libudev.so.1.6.3) point at the copy next to them now
for l in "$MA"/*; do
    if [ -L "$l" ] && [ ! -e "$l" ]; then
        t=$(basename "$(readlink "$l")")
        if [ -e "$MA/$t" ]; then ln -sf "$t" "$l"; else rm -f "$l"; fi
    fi
done
# A library the radio has only by its versioned name: the plain name the linker looks for
for f in "$MA"/lib*.so.[0-9]*; do
    b=$(basename "$f"); plain=${b%%.so.*}.so
    [ -e "$MA/$plain" ] || ln -sf "$b" "$MA/$plain"
done
# pkg-config: what a library itself links privately is not needed for a dynamic link, and its .pc files are not all
# here (libmount, libpcre, FLAC, ...)
for pc in "$MA"/pkgconfig/*.pc "$SYS"/usr/share/pkgconfig/*.pc; do
    [ -f "$pc" ] && [ ! -L "$pc" ] && sed -i '' -e '/^Requires.private:/d' -e '/^Libs.private:/d' "$pc"
done

echo "== liquid-dsp $LIQUID_VER, ft8_lib, X6200 control $X6200_CONTROL_TAG headers"
cd "$W/src"
[ -d liquid-dsp ] || git clone -q --depth 1 --branch v$LIQUID_VER https://github.com/jgaeddert/liquid-dsp.git
mkdir -p "$SYS/usr/include/liquid" && cp liquid-dsp/include/liquid.h "$SYS/usr/include/liquid/"
[ -d ft8_lib ] || { git clone -q https://github.com/gdyuldin/ft8_lib.git && git -C ft8_lib checkout -q $FT8LIB_REV; }
mkdir -p "$SYS/usr/include/ft8lib" && cp ft8_lib/ft8/*.h "$SYS/usr/include/ft8lib/"
[ -d X6200Control ] || git clone -q --depth 1 --branch $X6200_CONTROL_TAG https://github.com/gdyuldin/X6200Control.git
cp -R X6200Control/include/. "$SYS/usr/include/"

echo "== done: $SYS ($(du -sh "$SYS" | cut -f1)); next: tools/cross_x6200/build.sh"
