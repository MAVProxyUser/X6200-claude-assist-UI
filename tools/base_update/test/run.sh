#!/bin/bash
#
# base_flash's tests, on this computer (a Mac or a Linux PC; a C compiler and Python 3): the unit tests (units.c)
# and every scenario of the simulated base (sim.py), the slow ones side by side; about two minutes. Uses Xiegu's base
# files from build/base_update when they are there (tools/base_update/fetch.sh), else a stand-in of the same size
# for the transfers. Work files in build/base_update/test.
#
#     tools/base_update/test/run.sh
#
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$T/../../.." && pwd)
W=$TOP/build/base_update/test
CC=${HOST_CC:-cc}
mkdir -p "$W"
cd "$W" || exit 1
X8=$TOP/build/base_update/x6200_mcu_20260820141208.xgf
X7=$TOP/build/base_update/x6200_mcu_20260116092100.xgf
KNOWN=()
[ -f "$X8" ] && KNOWN+=("$X8")
[ -f "$X7" ] && KNOWN+=("$X7")

"$CC" -O1 -w -I "$T/stub" -o units "$T/units.c" || exit 1
./units "${KNOWN[@]}" || fails=1

# The transfers: Xiegu's 1.0.8 file, or a stand-in of its size that only the simulated base sees (base_flash takes
# only Xiegu's files: the stand-in is let through by building the scenarios with its hash in place of 1.0.8's)
if [ -f "$X8" ]; then
    F=$X8
    SRC=$T/../base_flash.c
else
    F=$W/standin.xgf
    head -c 524288 /dev/urandom > "$F"
    H=$(shasum -a 256 "$F" 2> /dev/null || sha256sum "$F")
    sed "s/9a19f97a5ad9e141a3663d222f984073bf0b18b38d4662604372a0036f9daa23/${H%% *}/" "$T/../base_flash.c" > "$W/base_flash.c"
    SRC=$W/base_flash.c
    echo "no Xiegu file in build/base_update: the transfers use a stand-in"
fi

pids=()
for mode in ok silent stream dead noerase nostart nak can garbage eot; do
    python3 "$T/sim.py" "$CC" "$SRC" "$T/stub" "$W" "$F" "$mode" > "$W/result_$mode" 2>&1 &
    pids+=($!)
done
[ -f "$X7" ] && { python3 "$T/sim.py" "$CC" "$T/../base_flash.c" "$T/stub" "$W" "$X7" ok ok107 > "$W/result_ok107" 2>&1 & pids+=($!); }
for p in "${pids[@]}"; do wait "$p" || fails=1; done
cat "$W"/result_*
[ -z "${fails:-}" ] && echo "all base_flash tests pass" || { echo "BASE_FLASH TESTS FAILED (logs in $W)"; exit 1; }
