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
# A text after a quiet (2026-11-25): the X6100's audio of the X6200's TCI CW, then 8 s with nothing on the air, then
# "TEST". In the quiet the lock went to a line of the X6100's noise at 297 Hz and held it; the station back, the lock
# stayed on the line, or went back to it in every gap, and the decoder printed "T ST" or "EEIT". The station held
# before now takes the lock back at once (tone_lock.cpp HELD_SNR): the text must end "VE TEST"
n=$((n + 1))
out=$(S16=1 ./cw_replay 300 1100 8 21 < x6200_text_after_quiet_on_x6100.s16 2> /dev/null)
case $out in
    *"Q1TST"*"VE TEST "*) echo "ok   a text after a quiet: $out" ;;
    *) echo "FAIL a text after a quiet: $out"; bad=$((bad + 1)) ;;
esac
echo "cw_replay: $n checks, $bad failed"
[ $bad -eq 0 ]
