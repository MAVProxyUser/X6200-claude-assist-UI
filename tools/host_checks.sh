#!/bin/bash
# Every host check of the repository in one run (upstream #46): each tools/*/ with a Makefile (`make` there builds
# its test and runs it), and the few built here (tools/adsb, tools/rtlspec). Each ends as one of:
#   PASS   built and ran, every case as it should be
#   FAIL   built, and the test said a case was wrong (or it ran past its time)
#   BUILD  did not build: a harness that no longer compiles, told apart from a failing case (the compiler's or the
#          linker's own words in its output)
#   SKIP   not run here, and why (a library the machine lacks, a check that needs the radio)
# The exit status is 0 only with no FAIL and no BUILD. A Mac or Linux with a C compiler and make; CI runs it.
#
#   tools/host_checks.sh [-v] [name ...]     -v: each check's output; names: only those (ham_bands adsb ...)
#
# SPDX-License-Identifier: LGPL-2.1-or-later

cd "$(dirname "$0")/.." || exit 2
TOP=$PWD
VERBOSE=0
[ "$1" = "-v" ] && { VERBOSE=1; shift; }
ONLY=" $* "
TIME_LIMIT=${HOST_CHECK_TIME:-300}      # seconds each; a hung check is a FAIL, not a stuck run
OUT=$(mktemp -d "${TMPDIR:-/tmp}/host_checks.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}

# ft8_lib's headers, as the radio's image has them (<ft8lib/constants.h>): FT8LIB_INC, a folder holding ft8lib/ (CI
# makes one from gdyuldin/ft8_lib at tools/cross_x6200/setup.sh's FT8LIB_REV), else the ft8lib folder alone of a
# cross build's sysroot here (the whole of it would bring Linux's headers to a Mac). Through CPATH, which the
# compilers search after -I, so each check's Makefile stays as it is
if [ -z "$FT8LIB_INC" ] && [ -f build/x6200/sysroot/usr/include/ft8lib/constants.h ]; then
    mkdir -p "$OUT/inc"
    ln -s "$TOP/build/x6200/sysroot/usr/include/ft8lib" "$OUT/inc/ft8lib"
    FT8LIB_INC=$OUT/inc
fi
[ -n "$FT8LIB_INC" ] && export CPATH="$FT8LIB_INC${CPATH:+:$CPATH}"

declare -a ROWS
n_pass=0 n_fail=0 n_build=0 n_skip=0

# Checks not run here, and why. A name here is still listed, as SKIP
skip_why() {
    case $1 in
        cw_trainer) [ -n "$DEPS" ] || echo "needs liquid-dsp: DEPS=<dir with include/liquid and lib>";;
        update|base_update) echo "CI runs these in workflows of their own (updater.yml, base_update.yml)";;
        cross_x6200|manual_pdf|logo|icons|fonts|bootvideo|secrets|tailscale|data_partition|first_start|wifi_restore|wifi_steady|winlink|pat|channels|iambic_default|smeter_share|ft8_qso)
            # tools, not host checks: their Makefiles build images, files or programs for the radio
            [ -f "tools/$1/Makefile" ] && grep -q -E '^test:' "tools/$1/Makefile" || echo "a tool, not a host check";;
    esac
}

# Runs "$@" with the time limit; its output into $2's log. Prints the result word
run_one() {
    local name=$1 log=$OUT/$1.log
    shift
    ( "$@" ) > "$log" 2>&1 &
    local pid=$! t=0
    while kill -0 $pid 2> /dev/null; do
        sleep 1
        t=$((t + 1))
        if [ $t -ge "$TIME_LIMIT" ]; then
            kill -TERM $pid 2> /dev/null
            sleep 1
            kill -KILL $pid 2> /dev/null
            echo "ran past ${TIME_LIMIT}s" >> "$log"
            echo FAIL
            return
        fi
    done
    if wait $pid; then
        echo PASS
    elif why=$(missing "$log"); [ -n "$why" ]; then
        echo "SKIP:$why"
    elif grep -q -E ': error:|fatal error:|undefined reference|Undefined symbols|ld: (error|symbol)|collect2: error|No rule to make target|cannot find -l' "$log"; then
        echo BUILD
    else
        echo FAIL
    fi
}

# What the machine lacks, from a check's output: a library from outside the repository, or Linux itself (a Mac has
# no accept4, pipe2, SOCK_NONBLOCK or linux/input.h). "" when it lacks nothing it needs: a BUILD then is the check's own
missing() {
    local log=$1
    if grep -q -E "liquid/liquid\.h.*(not found|No such file)|cannot find -lliquid|library 'liquid' not found" "$log"; then
        echo "needs liquid-dsp (libliquid-dev)"
    elif grep -q -E "sqlite3\.h.*(not found|No such file)|cannot find -lsqlite3" "$log"; then
        echo "needs sqlite3 (libsqlite3-dev)"
    elif grep -q -E "xkbcommon.*(not found|No such file)|cannot find -lxkbcommon|library 'xkbcommon' not found" "$log"; then
        echo "needs libxkbcommon (libxkbcommon-dev)"
    elif grep -q -E "bottle not found|No module named 'bottle'" "$log"; then
        echo "needs Python's bottle (python3-bottle)"
    elif grep -q -E "ft8lib/[a-z_]+\.h.*(not found|No such file)" "$log"; then
        echo "needs ft8_lib's headers: FT8LIB_INC=<a folder holding ft8lib/>"
    elif grep -q "autoreconf is needed" "$log"; then
        echo "needs RADE built (tools/rade/build.sh: autotools)"
    elif grep -q -E "GPSD=.*needed" "$log"; then
        echo "needs a gpsd 3.25 build: GPSD=<its buildroot build folder>"
    elif grep -q "No X6200 control library sources" "$log"; then
        echo "needs the X6200 control library's sources: X6200_CONTROL=<folder>"
    elif grep -q -E "build-host/lib/liblvgl\.a" "$log"; then
        echo "needs the GUI's host build (build-host, cmake with SDL)"
    elif [ "$(uname -s)" != Linux ] && grep -q -E "linux/[a-z_]+\.h.*(not found|No such file)|SOCK_NONBLOCK|SOCK_CLOEXEC|accept4|pipe2|pthread_condattr_setclock|unknown options: --wrap|epoll|timerfd|eventfd|signalfd|MSG_NOSIGNAL|TCP_KEEPIDLE|SO_BINDTODEVICE|clock_nanosleep|ppoll|mkfs|losetup|timeout: command not found|sed: -e|sed: 1:|sfdisk: command not found|unshare: (command not found|No such file)" "$log"; then
        echo "Linux only (this is $(uname -s))"
    fi
}

# The last line of a check's output that says how many cases, for the table
summary() {
    grep -i -E '[0-9]+ (of [0-9]+ |checks|cases|tests|passed|failed)|all .*(pass|as they should)|ok$' "$OUT/$1.log" 2> /dev/null |
        tail -n 1 | cut -c1-70
}

record() {
    local name=$1 res=$2 note=$3
    if [[ $res == SKIP:* ]]; then        # run_one found what the machine lacks
        note=${res#SKIP:}
        res=SKIP
    fi
    case $res in
        PASS) n_pass=$((n_pass + 1));;
        FAIL) n_fail=$((n_fail + 1));;
        BUILD) n_build=$((n_build + 1));;
        SKIP) n_skip=$((n_skip + 1));;
    esac
    ROWS+=("$(printf '%-5s  %-16s %s' "$res" "$name" "$note")")
    printf '%-5s  %-16s %s\n' "$res" "$name" "$note"
    if [ "$VERBOSE" = 1 ] || { [ "$res" != PASS ] && [ "$res" != SKIP ]; }; then
        [ -f "$OUT/$name.log" ] && sed 's/^/        /' "$OUT/$name.log" | tail -n 25
    fi
}

wanted() { [ "$ONLY" = "  " ] || [[ "$ONLY" == *" $1 "* ]]; }

# The checks with a Makefile
for d in tools/*/; do
    name=$(basename "$d")
    wanted "$name" || continue
    [ -f "$d/Makefile" ] || continue
    why=$(skip_why "$name")
    if [ -n "$why" ]; then
        record "$name" SKIP "$why"
        continue
    fi
    make -C "$d" clean > /dev/null 2>&1
    res=$(run_one "$name" make -C "$d")
    record "$name" "$res" "$(summary "$name")"
done

# The checks built here: no Makefile of their own
if wanted adsb; then
    res=$(run_one adsb sh -c "$CC -std=gnu11 -Wall -Wextra -O1 -g -I src -o '$OUT/adsb_test' tools/adsb/adsb_test.c src/adsb.c -lm -lpthread && '$OUT/adsb_test'")
    record adsb "$res" "$(summary adsb)"
fi
if wanted acars; then
    res=$(run_one acars sh -c "$CC -O2 -Wall -Wextra -o '$OUT/acars_test' tools/rtlspec/acars_test.c tools/rtlspec/acars.c -lm && '$OUT/acars_test'")
    record acars "$res" "$(summary acars)"
fi
if wanted rds; then
    res=$(run_one rds sh -c "$CC -O2 -o '$OUT/rds_test' tools/rtlspec/rds_test.c -lm && '$OUT/rds_test'")
    record rds "$res" "$(summary rds)"
fi

echo
echo "host checks: $n_pass passed, $n_fail failed, $n_build did not build, $n_skip skipped"
[ $n_fail -eq 0 ] && [ $n_build -eq 0 ]
