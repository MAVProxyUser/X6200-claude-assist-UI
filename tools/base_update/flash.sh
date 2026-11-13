#!/bin/bash
#
# Writes Xiegu's base firmware 1.0.8 (or 1.0.7 again) into the X6200's base from OpenX6X00, without Xiegu's system:
# base_flash (tools/base_update/base_flash.c) does what Xiegu's MENU > SYSTEM > UPGRADE does, step for step. The base
# file comes from tools/base_update/fetch.sh, the program from tools/base_update/build.sh; both are copied to the
# radio. More in tools/base_update/README.md.
#
#     tools/base_update/flash.sh [root@192.168.0.224] --check     reads only: the file, the base's version, who holds
#                                                                  its port and bus. Nothing is sent to the base.
#     tools/base_update/flash.sh [root@192.168.0.224] --flash     writes the base (asks first; --yes does not ask)
#     tools/base_update/flash.sh [root@192.168.0.224] --recover   after a write that failed past the erase
#     ... --to 1.0.7                                               base 1.0.7 again in place of 1.0.8, from
#                                                                  build/base_update/x6200_mcu_20260116092100.xgf (the
#                                                                  copy on the radio's internal memory)
#
# As Xiegu asks for its own update: the radio on a 13.8 V supply of 3 A or more, the battery out, for the whole
# write (about half a minute). The GUI is stopped for it and started again after; meanwhile /var/run/S95gui.lock is
# held, so a GUI start waits for the end of the write. The write runs on the radio by itself (run_on_radio.sh under
# setsid, its log in /tmp/base_update/flash.log, then appended to /mnt/base_update.log): a lost connection does not
# stop it, and this script follows the log until it ends. Do not switch the radio off while it runs. After it the
# base does not start the new firmware by itself: the radio switches itself off a minute or two later, and is
# switched on again by hand; then --check shows the new version.
#
set -euo pipefail
RADIO=root@192.168.0.224
MODE=
YES=
TO=1.0.8
while [ $# -gt 0 ]; do
    case "$1" in
        --check | --flash | --recover) MODE=$1 ;;
        --yes) YES=1 ;;
        --to) TO=${2:-}; shift ;;
        -*) echo "unknown option $1"; exit 1 ;;
        *) RADIO=$1 ;;
    esac
    shift
done
[ -n "$MODE" ] || { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 1; }
TOP=$(cd "$(dirname "$0")/../.." && pwd)
case "$TO" in
    1.0.8) XGF=x6200_mcu_20260820141208.xgf; GET="run tools/base_update/fetch.sh first" ;;
    1.0.7) XGF=x6200_mcu_20260116092100.xgf; GET="copy it from the radio's /usr/firmware on /dev/mmcblk2p2" ;;
    *) echo "--to 1.0.8 or 1.0.7"; exit 1 ;;
esac
BIN=$TOP/build/x6200/base_update/base_flash
F=$TOP/build/base_update/$XGF
[ -f "$F" ] || { echo "no $F: $GET"; exit 1; }
[ -x "$BIN" ] || { echo "no $BIN: run tools/base_update/build.sh first"; exit 1; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$RADIO")
D=/tmp/base_update

"${SSH[@]}" "mkdir -p $D"
scp -q "$BIN" "$F" "$RADIO:$D/"

if [ "$MODE" = --check ]; then
    "${SSH[@]}" "$D/base_flash --check $D/$XGF"
    exit
fi

cat << EOF
This writes Xiegu's base firmware $TO into the radio's base on $RADIO.
  - Xiegu's conditions: a 13.8 V supply of 3 A or more, the battery out, for the whole write.
  - The base's application is erased and written again. A write that fails after the erase leaves the base without
    one until this is run again with --recover (its bootloader is never erased; not yet tried on an X6200).
  - Do not switch the radio off until this says it is done.
EOF
if [ -z "$YES" ]; then
    read -r -p "Type FLASH to go on: " answer
    [ "$answer" = FLASH ] || { echo "not written"; exit 1; }
fi

# On the radio, apart from this session (tools/base_update/run_on_radio.sh): GUI off, the write, the GUI back unless
# the base was left without its application (exit 3), the log kept on the card
EXTRA=
[ "$MODE" = --recover ] && EXTRA=--recover
scp -q "$TOP/tools/base_update/run_on_radio.sh" "$RADIO:$D/"
"${SSH[@]}" "rm -f $D/flash.log; setsid sh $D/run_on_radio.sh $D $D/$XGF $EXTRA > /dev/null 2>&1 &"

# Follow the log until its END line, through dropped connections; 20 minutes at most (a normal write takes about one)
shown=0
for _ in $(seq 600); do
    sleep 2
    out=$("${SSH[@]}" "cat $D/flash.log 2> /dev/null" 2> /dev/null) || { echo "(no connection to the radio, trying again)"; continue; }
    total=$(printf '%s\n' "$out" | wc -l)
    if [ "$total" -gt "$shown" ]; then
        printf '%s\n' "$out" | tail -n +"$((shown + 1))"
        shown=$total
    fi
    case "$out" in *"END "*) break ;; esac
done
rc=$(printf '%s\n' "$out" | sed -n 's/^END //p' | tail -1)
[ -n "$rc" ] || { echo "no end of the write seen in 20 minutes: its log is $D/flash.log on the radio"; exit 4; }
[ "$rc" = 0 ] && echo "Written. If the radio switches itself off, switch it on again; then $0 $RADIO --check shows the version."
exit "$rc"
