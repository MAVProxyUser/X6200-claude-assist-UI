#!/bin/bash
#
# Makes a boot video for the radio (rootfs/etc/init.d/S94bootvideo) from any video, on this computer, with its
# ffmpeg: fitted into the 800x480 screen with black around it, 30 frames a second, H.264 that the radio decodes
# with half its processor, and no sound (the boot video plays none). CROP first, as ffmpeg's crop=W:H:X:Y, keeps
# the part that matters of a tall or wide one (a phone's portrait video, say) so it fills more of the screen.
#
# With --portrait it also makes OUT's portrait twin (OUT-portrait.mp4: the same turned to the X6200's 480x800 panel),
# which the X6200's video engine decodes and the display shows with no processor at all (S94bootvideo plays it
# when the image has GStreamer's stateless decoder; the landscape one stays for ffmpeg).
#
#     tools/bootvideo/prepare.sh [--portrait] IN.mp4 OUT.mp4 [W:H:X:Y]
#     scp OUT.mp4 root@<radio>:/mnt/bootvideo.mp4        (plays from the next start)
#     scp OUT-portrait.mp4 root@<radio>:/mnt/bootvideo-portrait.mp4
#
set -euo pipefail
PORTRAIT=0
[ "${1:-}" = --portrait ] && { PORTRAIT=1; shift; }
[ $# -ge 2 ] || { echo "usage: tools/bootvideo/prepare.sh [--portrait] IN OUT [crop W:H:X:Y]"; exit 2; }
VF="scale=800:480:force_original_aspect_ratio=decrease,pad=800:480:(ow-iw)/2:(oh-ih)/2:black,fps=30,format=yuv420p"
[ $# -ge 3 ] && VF="crop=$3,$VF"
ffmpeg -v error -y -i "$1" -vf "$VF" -an -c:v libx264 -profile:v baseline -preset slow -crf 22 -movflags +faststart "$2"
ls -la "$2"
if [ $PORTRAIT = 1 ]; then      # turned as the panel is: landscape x, y is the panel's y, 799 - x (transpose=2)
    P="${2%.mp4}-portrait.mp4"
    ffmpeg -v error -y -i "$2" -vf "transpose=2,format=yuv420p" -an -c:v libx264 -profile:v baseline -preset slow \
        -crf 22 -movflags +faststart "$P"
    ls -la "$P"
fi
