#!/bin/bash
#
# Puts what tools/bootvideo/build.sh built on the radio, /usr/bin/ffmpeg, with the boot video's start script
# (/etc/init.d/S94bootvideo), and a video made by tools/bootvideo/prepare.sh as /mnt/bootvideo.mp4 when one is named.
# Card images and updates bring both themselves; this is for a radio running an older one.
#
#     tools/bootvideo/install.sh [root@192.168.0.224] [VIDEO.mp4]
#
set -euo pipefail
RADIO=${1:-root@192.168.0.224}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200/ffmpeg/out
arm-linux-gnueabihf-strip -o "$B/ffmpeg.stripped" "$B/ffmpeg"
scp -q "$B/ffmpeg.stripped" "$RADIO:/usr/bin/ffmpeg.new"
scp -q "$TOP/rootfs/etc/init.d/S94bootvideo" "$RADIO:/etc/init.d/S94bootvideo.new"
[ $# -ge 2 ] && scp -q "$2" "$RADIO:/mnt/bootvideo.mp4"
ssh -o BatchMode=yes "$RADIO" 'chmod 755 /usr/bin/ffmpeg.new /etc/init.d/S94bootvideo.new
    mv /usr/bin/ffmpeg.new /usr/bin/ffmpeg; mv /etc/init.d/S94bootvideo.new /etc/init.d/S94bootvideo
    ls -la /usr/bin/ffmpeg /etc/init.d/S94bootvideo /mnt/bootvideo.mp4 2>&1'
