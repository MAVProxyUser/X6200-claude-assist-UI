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
# The first word whole (2026-11-25): the tone finder moved the lock 40-70 ms into a station's first mark and its first
# dit was lost (the receiver now gives the lock the audio it skipped: Receiver::rewind); a line of the noise the lock
# sat on kept it half a second into a station 40 dB over it (tone_lock.cpp ELSEWHERE_LINE); and a decoder just started
# keyed nothing for a second (KeyDetector::muted; that second given again, Receiver::detector_step). Cold, as recorded,
# and warm: the recording's first 0.9 s of the band's noise five times first, a decoder that has run on the band. Each
# read "VV TEST" or "UV TEST" before
whole() {   # LO HI FILE WARM
    n=$((n + 1))
    out=$( { [ "$4" = warm ] && for i in 1 2 3 4 5; do head -c 7200 "$3"; done; cat "$3"; } |
        S16=1 ./cw_replay "$1" "$2" 8 15 2> /dev/null)
    case $out in
        *"text: 'VVV TEST DE Q1TST "*) echo "ok   the first word, $4, filter $1-$2 $3: $out" ;;
        *) echo "FAIL the first word, $4, filter $1-$2 $3: $out"; bad=$((bad + 1)) ;;
    esac
}
whole 300 3000 x6200_cw_on_x6100.s16 cold
whole 400 6000 x6100_cw_on_x6200.s16 cold
whole 400 3000 x6200_cw_on_x6100.s16 warm
whole 400 3000 x6100_cw_on_x6200.s16 warm
# The X6200's base colours CW's receive audio itself: one resonance at the key tone, Q about 10, its noise 17-21 dB
# over the band's floor there (2026-11-25, orch cw-noise-shape), against which the decoder judged every tone: in a
# quiet it keyed on that noise and printed it (orch cw-quiet: 'ESI I I EI E T E I' with nothing on the air). src/cw.cpp
# gives the X6200's decoder the resonance (BASE_PEAK here), taken out of the spectrum and put back into the noise at
# the lock. Then: 12 s of its quiet print nothing (' EE EE E ' before); the X6100's TCI 'VV TEST' whole, its lock no
# longer on the noise's hump when the station began (the carrier floor measured over the station's first second
# raised the detector's noise 17 dB at once and its levels stayed in the old units: 'VS TES', KeyDetector::rescale);
# 'VVV' whole, the lock that keyed on the first dit's leak 39 Hz off moving to it (its filters filled from the audio
# before, not empty: 'VVU', Receiver::rewind)
colour() {  # FILE WANT: the text must be WANT (an empty one for a quiet)
    n=$((n + 1))
    out=$(BASE_PEAK=700:10 S16=1 ./cw_replay 300 1100 8 20 < "$1" 2> /dev/null)
    case $out in
        *"text: '$2'"*) echo "ok   the X6200's resonance taken out, $1: $out" ;;
        *) echo "FAIL the X6200's resonance taken out, $1: $out"; bad=$((bad + 1)) ;;
    esac
}
colour x6200_quiet.s16 ""
colour x6100_tci_vv_on_x6200.s16 "VV TES"
colour x6100_vvv_on_x6200.s16 "VVV IT"
colour x6100_cw_on_x6200.s16 "VVV TEST DE Q1TST "
# The radio's peak filter (APF) colours the noise as the X6200's resonance does (Q 8: +5.6 dB at the tone, -9 to -12
# at the band's edges), and its colour was taken out of the spectrum alone: the X6100's audio of the X6200's TCI
# "VV TEST", put through the APF's own filter at Q 8 (12.5 kHz, tone_finder.cpp apf_gain2's), read 'VI TEST'
n=$((n + 1))
out=$(APF=700:8 S16=1 ./cw_replay 300 1100 8 20 < x6200_tci_vv_on_x6100_apf8.s16 2> /dev/null)
case $out in
    *"text: 'VV TEST'"*) echo "ok   the APF's colour taken out at the lock too: $out" ;;
    *) echo "FAIL the APF's colour taken out at the lock too: $out"; bad=$((bad + 1)) ;;
esac
echo "cw_replay: $n checks, $bad failed"
[ $bad -eq 0 ]
