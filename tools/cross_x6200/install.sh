#!/bin/bash
#
# Puts the GUI tools/cross_x6200/build.sh built on the radio and restarts it (x6100_daemon starts it again at once).
# The radio's first GUI is kept as /usr/sbin/x6100_gui.orig the first time.
#
#     tools/cross_x6200/install.sh [root@192.168.0.224]
#
set -euo pipefail
RADIO=${1:-root@192.168.0.224}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200
arm-linux-gnueabihf-strip -o "$B/x6100_gui.stripped" "$B/gui/src/x6100_gui"
scp -q "$B/x6100_gui.stripped" "$RADIO:/usr/sbin/x6100_gui.new"
ssh -o BatchMode=yes "$RADIO" '[ -f /usr/sbin/x6100_gui.orig ] || cp -p /usr/sbin/x6100_gui /usr/sbin/x6100_gui.orig
    mv /usr/sbin/x6100_gui.new /usr/sbin/x6100_gui; kill $(pidof x6100_gui) 2>/dev/null; sleep 5
    p=$(pidof x6100_gui) && echo "running, pid $p" || { echo "NOT running"; tail -20 /tmp/x6100_log.txt; exit 1; }'
