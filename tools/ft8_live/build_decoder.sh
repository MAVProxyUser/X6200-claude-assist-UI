#!/bin/bash
#
# OpenX6X00 - builds ft8_lib's decode_ft8 (MIT) into bin/ beside this script, where ft8_live.py looks for it.
# Built from the commit below; again only when that changes. Nothing is installed or started.
#
#   tools/ft8_live/build_decoder.sh
#   python3 tools/ft8_live/ft8_live.py         (then http://127.0.0.1:8073/, until it is stopped)
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)
FT8_LIB_URL=https://github.com/kgoba/ft8_lib.git
FT8_LIB_REV=9fec6ca39886edbf96f4f5e71edc76da5074e871

if [ -x "$DIR/bin/decode_ft8" ] && [ "$(cat "$DIR/bin/.rev" 2> /dev/null)" = "$FT8_LIB_REV" ]; then
    echo "decode_ft8 at ${FT8_LIB_REV:0:7} is there: $DIR/bin/decode_ft8"
    exit 0
fi
echo "== Building ft8_lib's decode_ft8 at ${FT8_LIB_REV:0:7}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$tmp" init -q
git -C "$tmp" fetch -q --depth 1 "$FT8_LIB_URL" "$FT8_LIB_REV"
git -C "$tmp" checkout -q FETCH_HEAD
# Its demo calls snprintf without <stdio.h>, which macOS's clang refuses
make -s -C "$tmp" CC="cc -include stdio.h" decode_ft8
mkdir -p "$DIR/bin"
cp "$tmp/decode_ft8" "$DIR/bin/decode_ft8"
echo "$FT8_LIB_REV" > "$DIR/bin/.rev"
echo "== $DIR/bin/decode_ft8"
