#!/bin/bash
#
# Builds a small ffmpeg for the X6200 on this computer, against the sysroot tools/cross_x6200/setup.sh made, into
# build/x6200/ffmpeg/out: what the boot video (rootfs/etc/init.d/S94bootvideo) needs and little more. It reads MP4,
# MKV and AVI, decodes H.264, HEVC, MPEG-4 and VP8/VP9 video, and scales, pads and turns the picture for the
# framebuffer (fbdev). No sound. Static.
# The image builds buildroot's own ffmpeg package instead (buildroot/local-build.sh); this is for trying it on a
# radio without a new card image. --enable-v4l2-m2m only because 4.4's configure looks for linux/fb.h (fbdev) there.
#
#     tools/bootvideo/build.sh           then tools/bootvideo/install.sh [root@radio]
#
set -euo pipefail
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200/ffmpeg
SR=$TOP/build/x6200/sysroot
VER=4.4.4        # buildroot 2022.11's ffmpeg major version
SHA256=e80b380d595c809060f66f96a5d849511ef4a76a26b76eacf5778b94c3570309
[ -d "$SR/usr/lib/arm-linux-gnueabihf" ] || { echo "run tools/cross_x6200/setup.sh first"; exit 1; }
mkdir -p "$B/out"

T=$B/ffmpeg-$VER.tar.xz
if [ ! -f "$T" ]; then
    curl -sSfL -o "$T.tmp" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
    echo "$SHA256  $T.tmp" | shasum -a 256 -c --quiet - || { echo "ffmpeg-$VER: not the pinned SHA-256"; exit 1; }
    mv "$T.tmp" "$T"
fi
[ -d "$B/ffmpeg-$VER" ] || tar xJf "$T" -C "$B"

cd "$B/ffmpeg-$VER"
./configure --prefix="$B/out" --enable-cross-compile --cross-prefix=arm-linux-gnueabihf- --arch=arm --cpu=cortex-a7 \
    --target-os=linux --sysroot="$SR" --pkg-config=false \
    --extra-cflags="-mfpu=neon-vfpv4 -mfloat-abi=hard -isystem $SR/usr/include/arm-linux-gnueabihf" \
    --extra-ldflags="-L$SR/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,$SR/usr/lib/arm-linux-gnueabihf -Wl,--allow-shlib-undefined" \
    --disable-everything --disable-autodetect --disable-doc --disable-debug --disable-ffplay --disable-ffprobe \
    --enable-ffmpeg --enable-small --enable-pthreads --enable-neon --enable-swscale --enable-swresample \
    --enable-protocol=file \
    --enable-v4l2-m2m \
    --enable-demuxer=mov,matroska,avi \
    --enable-decoder=h264,hevc,mpeg4,vp8,vp9 --enable-encoder=rawvideo \
    --enable-parser=h264,hevc,mpeg4video,vp8,vp9 \
    --enable-filter=scale,pad,transpose,format,fps,crop,setsar,null \
    --enable-outdev=fbdev > "$B/configure.log"
make -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" ffmpeg > "$B/make.log"
cp ffmpeg "$B/out/ffmpeg"
ls -la "$B/out/ffmpeg"
