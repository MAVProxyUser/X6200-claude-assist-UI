#!/bin/bash
#
# The host checks that only build on Linux (macOS's linker has no --wrap: tune, txlevel, beacon; TCI's server: tci), so
# that on a Mac CI was the first to run them: cross-built here against the radio's sysroot (tools/cross_x6200/setup.sh)
# and run on a radio, which is Linux, at the lowest priority. Run before pushing a change to what they test (their
# Makefiles' COPIED lists: tx_player, tx_level, tx_hold, tune_tx, ext_tune, swr_guard, cw_encoder, cw_beacon,
# tci_server, tci_audio, tci_ws, control_owner...): 2026-11-24/25 eight pushes in a row failed CI's host checks on them
# (tune and txlevel's stand-ins, then the TCI owner test), each a real break, none a flake.
#
#     tools/radio_checks.sh [check ...]        checks: tune txlevel beacon tci (all four when none is named)
#     RADIO=root@x6200.local tools/radio_checks.sh tci
#     git diff --name-only origin/main | tools/radio_checks.sh --changed [--dry]   only the checks those files touch
#                                                     (before a push); none: exit 0; --dry: only named
#     KEEP_OUT=DIR tools/radio_checks.sh tci       each check's whole output kept in DIR as CHECK.log
#
# The X6100 by default (the bench's second radio). TCI's test server listens on TCI_TEST_PORT 50091, not the radio's
# own 50001; its files go under /tmp/radio_checks on the radio, removed after. Exit status 0 when every check passed.
# macOS's bash 3.2: no associative arrays here.
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -uo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
RADIO=${RADIO:-root@x6100.local}
SR=$TOP/build/x6200/sysroot
[ -d "$SR/usr/include" ] || { echo "run tools/cross_x6200/setup.sh first (the radio's sysroot)"; exit 2; }
ALL=(tune txlevel beacon tci)

# A check's inputs, as paths from the top: its own folder, the sources its Makefile copies from src/ (COPIED, MORSE),
# and what its build runs (tune: tools/speech's silent.c; tci: tools/host_link's autolink.py)
inputs_of() {
    echo "tools/$1/"
    sed -n -E '/^(COPIED|MORSE) *:=/,/[^\\]$/p' "$TOP/tools/$1/Makefile" | sed 's/^[A-Z]* *:=//; s/\\$//' |
        tr ' ' '\n' | grep -E '\.(c|h)$' | sed 's|^|src/|'
    case $1 in tune) echo "tools/speech/";; tci) echo "tools/host_link/";; esac
}

if [ "${1:-}" = --changed ]; then
    changed=$(cat)
    CHECKS=()
    for c in "${ALL[@]}"; do
        for i in $(inputs_of "$c"); do
            if printf '%s\n' "$changed" | grep -q -F -x -e "$i" || { [ "${i%/}" != "$i" ] &&
                printf '%s\n' "$changed" | grep -q "^$i"; }; then
                CHECKS+=("$c")
                break
            fi
        done
    done
    [ ${#CHECKS[@]} -gt 0 ] || { echo "radio checks: nothing they test changed"; exit 0; }
    echo "radio checks: ${CHECKS[*]} (their sources changed)"
    [ "${2:-}" = --dry ] && exit 0
else
    CHECKS=("$@")
    [ ${#CHECKS[@]} -gt 0 ] || CHECKS=("${ALL[@]}")
fi
OUT=$(mktemp -d "${TMPDIR:-/tmp}/radio_checks.XXXXXX")
trap '[ -n "${KEEP_OUT:-}" ] && cp "$OUT"/*.log "$KEEP_OUT"/ 2>/dev/null; rm -rf "$OUT"' EXIT

# The compiler as each Makefile calls it ($(CC) ... -o file sources -lm): ARM, the radio's sysroot, and libatomic
# linked in statically when it links (ARM has no 8-byte atomics of its own; the radios have no libatomic.so)
cat > "$OUT/cc" <<EOF
#!/bin/bash
link=1
for a in "\$@"; do case \$a in -c|-E|-fsyntax-only) link=0;; esac; done
if [ \$link = 1 ]; then
    exec arm-linux-gnueabihf-gcc --sysroot=$SR "\$@" -Wl,-Bstatic -latomic -Wl,-Bdynamic
fi
exec arm-linux-gnueabihf-gcc --sysroot=$SR "\$@"
EOF
chmod +x "$OUT/cc"

# The binaries make builds for a check, and the files it also needs on the radio
bins_of() {
    case $1 in
        tune) echo "tune_test tune_test_x6200";;
        txlevel) echo "txlevel_test";;
        beacon) echo "beacon_test";;
        tci) echo "tci_audio_test tci_own_test tci_host";;
    esac
}
extra_of() {
    case $1 in tci) echo "tci/tci_client_test.py";; esac
}
# What runs them on the radio, in /tmp/radio_checks (as their Makefiles' test targets)
run_of() {
    case $1 in
        tune) echo './tune_test && ./tune_test_x6200';;
        txlevel) echo './txlevel_test';;
        beacon) echo './beacon_test';;
        tci) cat <<'EOF'
export TCI_TEST_PORT=50091 TCI_TEST_PLAYED=/tmp/radio_checks/played.raw TCI_TEST_REVOKED=/tmp/tci-test.conf.revoked
export TCI_TEST_ASK=/tmp/tci-test.conf.ask TCI_TEST_ANSWER=/tmp/tci-test.conf.answer
export TCI_TEST_ALLOWED=/tmp/tci-test.conf.allowed TCI_TEST_KEYED=/tmp/tci-test.conf.keyed
rm -f /tmp/tci-test.conf.*
./tci_audio_test || exit 1
./tci_own_test || exit 1
rm -f /tmp/tci-test.conf.*
./tci_host & p=$!
sleep 0.5
python3 -I tci_client_test.py; r=$?
kill $p
exit $r
EOF
        ;;
    esac
}

SSH=(ssh -4 -o BatchMode=yes -o ConnectTimeout=10 "$RADIO")
"${SSH[@]}" 'rm -rf /tmp/radio_checks && mkdir -p /tmp/radio_checks' || { echo "$RADIO: not reachable"; exit 2; }

fails=()
for c in "${CHECKS[@]}"; do
    bins=$(bins_of "$c")
    [ -n "$bins" ] || { echo "no such check here: $c (tune txlevel beacon tci)"; fails+=("$c"); continue; }
    make -s -C "$TOP/tools/$c" clean > /dev/null 2>&1
    # shellcheck disable=SC2086
    if ! make -s -C "$TOP/tools/$c" CC="$OUT/cc" $bins > "$OUT/$c.build" 2>&1; then
        echo "BUILD  $c"; sed 's/^/        /' "$OUT/$c.build" | tail -20; fails+=("$c"); continue
    fi
    run_of "$c" > "$OUT/run_$c.sh"
    files=("$OUT/run_$c.sh")
    for b in $bins; do files+=("$TOP/tools/$c/$b"); done
    for e in $(extra_of "$c"); do files+=("$TOP/tools/$e"); done
    if ! scp -4 -q -o BatchMode=yes "${files[@]}" "$RADIO:/tmp/radio_checks/"; then
        echo "COPY   $c"; fails+=("$c"); continue
    fi
    t0=$(date +%s)
    if "${SSH[@]}" "cd /tmp/radio_checks && nice -n 19 sh ./run_$c.sh" > "$OUT/$c.log" 2>&1; then
        echo "PASS   $c ($(( $(date +%s) - t0 )) s): $(grep -E 'OK|passed|cases|as they should' "$OUT/$c.log" | tail -1)"
    else
        echo "FAIL   $c"
        grep -E '^ *(FAIL|FAILED|Traceback|Error)' "$OUT/$c.log" | head -12 | sed 's/^/        /'
        tail -15 "$OUT/$c.log" | sed 's/^/        /'
        fails+=("$c")
    fi
    make -s -C "$TOP/tools/$c" clean > /dev/null 2>&1     # the ARM binaries not left for a Linux make to run
done
"${SSH[@]}" 'rm -rf /tmp/radio_checks /tmp/tci-test.conf.*'
if [ ${#fails[@]} -eq 0 ]; then
    echo "radio checks: all ${#CHECKS[@]} passed on $RADIO"
    exit 0
fi
echo "radio checks: ${#fails[@]} of ${#CHECKS[@]} did not pass: ${fails[*]}"
exit 1
