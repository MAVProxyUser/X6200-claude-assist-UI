#!/bin/sh
# The CW decoder's tone floor (src/cw_decoder/rx_defs.h TONE_MIN_HZ), on a recording: the X6100's audio of the
# X6200 keying "VVV TEST DE Q1TST" at 15 WPM, 0.1 W over the bench's 80 dB cable (2026-11-23), 4 kHz, 16-bit. The
# X6100's CW filter from 0 Hz locked the decoder on the keying's 20 Hz thumps and it printed every element as E;
# from 150 Hz up it copies. Also with the filter from 300 Hz, as it always did. And the X6200's of the X6100 (below).
cd "$(dirname "$0")"
bad=0 n=0
check() {   # LO HI [FILE]: the text must hold TEST DE Q1TST (its word spaces too), the tone locked at 600-800 Hz
    n=$((n + 1))
    out=$(S16=1 ./cw_replay "$1" "$2" 8 < "${3:-x6200_cw_on_x6100.s16}" 2> /dev/null)
    lock=$(echo "$out" | sed -n 's/.*lock \([0-9]*\) Hz.*/\1/p')
    case $out in
        *"TEST DE Q1TST"*) [ "${lock:-0}" -ge 600 ] && [ "${lock:-0}" -le 800 ] && { echo "ok   filter $1-$2 ${3:-}: $out"; return; } ;;
    esac
    echo "FAIL filter $1-$2 ${3:-}: $out"
    bad=$((bad + 1))
}
check 0 3000
check 300 3000
# The other way (2026-11-24): the X6200's audio, AGC slow, of the X6100 keying the same at 0.1 W. Its carrier between
# the elements (62 dB under its marks) came up with the X6200's AGC through each word gap, the decoder keyed on it
# ~200 ms early and printed "VVTESTZEQ1TST"; a key down must now be a rise (key_detector.cpp RISE_DB). The radios'
# CW filter, 400-6000 Hz
check 400 6000 x6100_cw_on_x6200.s16
echo "cw_replay: $n checks, $bad failed"
[ $bad -eq 0 ]
