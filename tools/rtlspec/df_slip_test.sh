#!/bin/bash
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# GoldCorpHF - Phase 0 test 0a of docs/plans/kraken_df_plan.md: how often the radio's USB loses a sample with the
# KrakenSDR's five RTLs streaming at once. Run from the Mac; on the radio it runs rtl_test on each of the Kraken's
# sticks (serials 1000-1004) in test mode (the RTL2832U sends a counter instead of samples, so every gap is a loss),
# one opened at a time, all streaming together, then counts the loss EVENTS per channel per hour. Receive only:
# nothing is transmitted, nothing on the radio is changed but files in /tmp/df_slip (removed at the end).
#
#     tools/rtlspec/df_slip_test.sh [-h root@radio] [-s rate] [-m minutes]      (defaults 1024000, 30)
#
# Before: the Kraken on its own power on the HOST port; no watch channel and no AIRSPACE receiver on its sticks (the
# script stops if one of them is held). B on another SDR may keep running (that is the realistic case).

HOST=root@x6200
RATE=1024000
MIN=30
while getopts "h:s:m:" o; do
    case $o in
        h) HOST=$OPTARG;;
        s) RATE=$OPTARG;;
        m) MIN=$OPTARG;;
        *) sed -n '3,15p' "$0"; exit 2;;
    esac
done

ssh -o ConnectTimeout=5 "$HOST" RATE="$RATE" SECS=$((MIN * 60)) sh -s << 'EOF'
SER="1000 1001 1002 1003 1004"
D=/tmp/df_slip
# each serial on USB, and nobody holding it
for s in $SER; do
    dev=
    for d in /sys/bus/usb/devices/*; do
        [ "$(cat "$d/serial" 2> /dev/null)" = "$s" ] && grep -q '283[28]' "$d/idProduct" 2> /dev/null && dev=$d
    done
    [ -n "$dev" ] || { echo "no RTL with serial $s: is the Kraken plugged in and powered?"; exit 1; }
    node=$(printf '/dev/bus/usb/%03d/%03d' "$(cat "$dev/busnum")" "$(cat "$dev/devnum")")
    for p in /proc/[0-9]*; do
        if ls -l "$p/fd" 2> /dev/null | grep -q "$node"; then
            echo "serial $s ($node) is held by $(tr '\0' ' ' < "$p/cmdline"): stop its watch channel / AIRSPACE first"
            exit 1
        fi
    done
done
rm -rf $D && mkdir -p $D
echo "rate $RATE S/s, $SECS s, started $(date -u +%H:%M:%S) UTC"
for s in $SER; do
    ( rtl_test -d "$s" -s "$RATE" 2>&1 | while IFS= read -r l; do echo "$(date +%s) $l"; done > $D/$s.log ) &
    sleep 2                                         # one at a time: opening five at once left one with no samples
done
sleep "$SECS"
killall -INT rtl_test 2> /dev/null
sleep 3
killall rtl_test 2> /dev/null
echo "serial  loss-events  events/hour  bytes-lost(min)  rtl_test's summary"
for s in $SER; do
    ev=$(grep -c 'lost at least' $D/$s.log)
    by=$(grep 'lost at least' $D/$s.log | awk '{s += $5} END {print s + 0}')
    sum=$(grep -i 'per million' $D/$s.log | tail -1 | cut -d' ' -f2-)
    echo "$s  $ev  $(awk -v e="$ev" -v t="$SECS" 'BEGIN {printf "%.1f", e * 3600 / t}')  $by  ${sum:-?}"
done
echo "first events:"
grep -h 'lost at least\|error\|Error' $D/*.log | head -10
rm -rf $D
EOF
