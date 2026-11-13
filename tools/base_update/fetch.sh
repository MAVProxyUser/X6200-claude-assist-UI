#!/bin/bash
#
# Fetches the X6200's base firmware 1.0.8 from Xiegu's own update on xiegu.eu (x6200udimg-260820001.zip, 371 MB) into
# build/base_update, and takes the base's file out of it: the zip holds a card image (sdcard.img) whose third partition
# (FAT) holds rootfs.tar, the system Xiegu's update writes to the radio's internal memory, with the base's file in
# its /usr/firmware. The zip and the file are checked against the sizes and SHA-256 sums below; the big intermediate
# files are removed after. Base 1.0.8 fixes the S-meter that sticks at -255 dBm (shown as S1, -121.0) on 1.0.7.
# Needs 7-Zip (7zz or 7z, "brew install sevenzip") for the card image and its FAT partition.
#
#     tools/base_update/fetch.sh           then tools/base_update/install.sh [root@radio]
#
set -euo pipefail
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/base_update
URL=https://xiegu.eu/wp-content/uploads/sites/4/2026/10/x6200udimg-260820001.zip
ZIP=x6200udimg-260820001.zip
ZIP_SIZE=371310845
ZIP_SHA=ca0226c7ffbfa38a5655bd9fc4bbe4e7995055cb3e2b35433a2ad5d39b4100a1
XGF=x6200_mcu_20260820141208.xgf
XGF_SIZE=524288
XGF_SHA=9a19f97a5ad9e141a3663d222f984073bf0b18b38d4662604372a0036f9daa23

sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }
size() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }
SZ=$(command -v 7zz || command -v 7z || true)
[ -n "$SZ" ] || { echo "needs 7-Zip: brew install sevenzip (or apt install 7zip)"; exit 1; }
mkdir -p "$B"

if [ -f "$B/$XGF" ] && [ "$(sha "$B/$XGF")" = "$XGF_SHA" ]; then
    echo "$B/$XGF is there already (sha256 $XGF_SHA)"
    exit 0
fi

# The zip: the direct link from Xiegu's downloads page (its download counter link turns curl away)
if [ ! -f "$B/$ZIP" ] || [ "$(size "$B/$ZIP")" != "$ZIP_SIZE" ]; then
    echo "downloading $ZIP (371 MB) from xiegu.eu"
    curl -fL --retry 3 -C - -A "Mozilla/5.0 (compatible; OpenX6X00 base_update)" -o "$B/$ZIP.part" "$URL"
    mv "$B/$ZIP.part" "$B/$ZIP"
fi
[ "$(size "$B/$ZIP")" = "$ZIP_SIZE" ] || { echo "$ZIP: $(size "$B/$ZIP") bytes, not $ZIP_SIZE"; exit 1; }
[ "$(sha "$B/$ZIP")" = "$ZIP_SHA" ] || { echo "$ZIP: not the file this was written for (sha256)"; exit 1; }

# zip -> sdcard.img -> its third partition (2.fat) -> rootfs.tar -> ./usr/firmware/$XGF
W=$B/work
rm -rf "$W"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT
unzip -q -o -j "$B/$ZIP" '*/sdcard.img' -d "$W"
"$SZ" x -y -o"$W" "$W/sdcard.img" 2.fat > /dev/null
rm -f "$W/sdcard.img"
"$SZ" x -y -o"$W" "$W/2.fat" rootfs.tar > /dev/null
rm -f "$W/2.fat"
tar -xf "$W/rootfs.tar" -C "$W" "./usr/firmware/$XGF"
[ "$(size "$W/usr/firmware/$XGF")" = "$XGF_SIZE" ] && [ "$(sha "$W/usr/firmware/$XGF")" = "$XGF_SHA" ] ||
    { echo "$XGF in the image is not the one expected"; exit 1; }
mv "$W/usr/firmware/$XGF" "$B/$XGF"
echo "$B/$XGF: $XGF_SIZE bytes, sha256 $XGF_SHA (Xiegu's base 1.0.8)"
