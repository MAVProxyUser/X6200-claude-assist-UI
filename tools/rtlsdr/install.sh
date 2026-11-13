#!/bin/bash
#
# Puts what tools/rtlsdr/build.sh built on the radio: /usr/bin/dump1090-fa (AIRSPACE starts it for an RTL-SDR) and
# /usr/bin/rtl_test. A dump1090 running is stopped first; AIRSPACE starts the new one.
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
    ls -la /usr/bin/dump1090-fa /usr/bin/rtl_test /usr/bin/x6x00-rtlspec'
