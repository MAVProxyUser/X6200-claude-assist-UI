#!/bin/sh
# The two ends of the link must never be keyed at once (the owner, 2026-11-21: "don't do that"). The link's own
# turn-taking is checked here, apart from the bench's interlock on the radios: a sweep of simulated lines, clean to
# failing, one way and both, the radios' own lead and tails (bench.py MODEM_TAIL), each case its own noise. Any
# sample with both keyed fails the sweep, whatever else becomes of the link.
#     overlap.sh [BUILD_DIR]
# SPDX-License-Identifier: GPL-3.0-or-later
B=${1:-build}
fail=0
seed=0
# snr_db offset_hz ppm both
for c in 300:7:40:1 24:-12:60:1 16:20:-50:1 11:5:30:1 8:-9:30:1 14:-25:80:0 20:35:-100:1 6:3:20:1 \
         18:-40:120:1 13:9:-60:0 300:-60:-120:1 9:15:45:1; do
    seed=$((seed + 1))
    set -- $(echo "$c" | tr : ' ')
    out=$(SEED=$seed LEAD=0.8 TAIL_A=0.6 TAIL_B=1.8 OVERLAP_ONLY=1 "$B/test_link" "$1" "$2" "$3" 4096 "$4" | grep -- '^-- ')
    echo "  snr $1 offset $2 ppm $3 both $4: $out"
    echo "$out" | grep -q 'both keyed for 0 samples' || fail=1
done
if [ $fail = 0 ]; then
    echo "overlap: never both keyed"
else
    echo "overlap: BOTH KEYED in a case above"
    exit 1
fi
