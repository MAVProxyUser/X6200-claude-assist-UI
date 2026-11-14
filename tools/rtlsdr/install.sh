#!/bin/bash
#
# Puts what tools/rtlsdr/build.sh built on the radio: /usr/bin/dump1090-fa (AIRSPACE starts it for an RTL-SDR),
# /usr/bin/rtl_test and /usr/bin/x6x00-rtlspec (the stick's daemon). A dump1090 running is stopped first; AIRSPACE
# starts the new one. The daemon is told to end once its helper is replaced (it closes the stick): the GUI starts the
# new one within seconds. A GUI started again on its own leaves the daemon, and the stick, as they are.
#
#     tools/rtlsdr/install.sh [root@192.168.0.224]
#
set -euo pipefail
RADIO=${1:-root@192.168.0.224}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
B=$TOP/build/x6200/rtlsdr/out/bin
for f in dump1090-fa rtl_test x6x00-rtlspec; do
    arm-linux-gnueabihf-strip -o "$B/$f.stripped" "$B/$f"
    scp -q "$B/$f.stripped" "$RADIO:/usr/bin/$f.new"
done
ssh -o BatchMode=yes "$RADIO" 'killall dump1090-fa 2>/dev/null; sleep 1
    for f in dump1090-fa rtl_test x6x00-rtlspec; do chmod 755 /usr/bin/$f.new; mv /usr/bin/$f.new /usr/bin/$f; done
    killall x6x00-rtlspec 2>/dev/null
    ls -la /usr/bin/dump1090-fa /usr/bin/rtl_test /usr/bin/x6x00-rtlspec'
