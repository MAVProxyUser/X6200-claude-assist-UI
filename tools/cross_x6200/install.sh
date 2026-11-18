#!/bin/bash
#
# Puts the GUI tools/cross_x6200/build.sh built on the radio and restarts it (x6100_daemon starts it again at once).
# No copy of the GUI it replaces is kept (13.7 MB on a full root filesystem; the user had the old ones deleted,
# 2026-11-15): a release's GUI comes back with the updater.
#
#     tools/cross_x6200/install.sh [root@x6200.local]
#     tools/cross_x6200/install.sh root@x6100.local build/x6100     the X6100's (build_x6100.sh)
#
set -euo pipefail
RADIO=${1:-root@x6200.local}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/${2:-build/x6200}
arm-linux-gnueabihf-strip -o "$B/x6100_gui.stripped" "$B/gui/src/x6100_gui"
scp -q "$B/x6100_gui.stripped" "$RADIO:/usr/sbin/x6100_gui.new"
ssh -o BatchMode=yes "$RADIO" 'mv /usr/sbin/x6100_gui.new /usr/sbin/x6100_gui; kill $(pidof x6100_gui) 2>/dev/null; sleep 5
    p=$(pidof x6100_gui) && echo "running, pid $p" || { echo "NOT running"; tail -20 /tmp/x6100_log.txt; exit 1; }'
