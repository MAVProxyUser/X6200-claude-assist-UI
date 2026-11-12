#!/bin/bash
#
# OpenX6X00 - builds js8_play on this computer from the repository's own JS8 code: third-party/js8core and the
# receive path of src/js8. Needs a C++20 compiler, single-precision FFTW and Boost's headers (its CRC), as from
# Homebrew: brew install fftw boost.
#
#   tools/js8_play/build.sh && tools/js8_play/bin/js8_play --selftest
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$DIR/../.." && pwd)
CORE=$REPO/third-party/js8core
JS8=$REPO/src/js8
FFTW=${FFTW_PREFIX:-$(brew --prefix fftw 2> /dev/null || echo /usr/local)}
BOOST=${BOOST_PREFIX:-$(brew --prefix boost 2> /dev/null || echo /usr/local)}
OBJ=$DIR/bin/obj
mkdir -p "$OBJ"

CXX=${CXX:-c++}
FLAGS=(-std=c++20 -O2 -w -I"$CORE/include" -I"$CORE" -I"$JS8" -I"$FFTW/include" -I"$BOOST/include")
SRCS=(
    "$CORE"/src/engine/engine.cpp "$CORE"/src/decoder/legacy_decoder.cpp "$CORE"/src/dsp/flatten.cpp
    "$CORE"/src/dsp/resampler.cpp "$CORE"/src/tx/modulator.cpp "$CORE"/src/protocol/submode.cpp
    "$CORE"/src/protocol/costas.cpp "$CORE"/src/protocol/constants.cpp "$CORE"/src/protocol/time_utils.cpp
    "$CORE"/src/protocol/varicode.cpp "$CORE"/src/protocol/jsc.cpp "$CORE"/src/protocol/jsc_map.cpp
    "$CORE"/src/protocol/jsc_list.cpp
    "$JS8"/rx_engine.cpp "$JS8"/rate_conv.cpp "$JS8"/frame_text.cpp "$JS8"/frame_joiner.cpp "$JS8"/msg_kind.cpp
    "$JS8"/submodes.cpp "$JS8"/wav_file.cpp "$JS8"/test_signal.cpp
    "$DIR"/js8_play.cpp
)
objs=()
for s in "${SRCS[@]}"; do
    o="$OBJ/$(basename "${s%.cpp}").o"
    if [ ! -e "$o" ] || [ "$s" -nt "$o" ]; then
        echo "== $(basename "$s")"
        "$CXX" "${FLAGS[@]}" -c "$s" -o "$o" &
    fi
    objs+=("$o")
done
wait
"$CXX" -o "$DIR/bin/js8_play" "${objs[@]}" -L"$FFTW/lib" -lfftw3f -lpthread
echo "== $DIR/bin/js8_play"
