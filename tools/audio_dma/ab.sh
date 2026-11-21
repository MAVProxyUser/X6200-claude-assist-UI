#!/bin/sh
# ab.sh HOST SECONDS KO: x6x00_dmafix A/B on HOST (a 6.1.82 kernel without 9105): fix=0 (6.1.82's single read) for
# SECONDS, then fix=1 (mainline's), two hammers each; the module unloaded at the end (the built-in function back).
# KO: x6x00_dmafix.ko built for HOST's kernel (README.md)
H=$1; T=$2; KO=$3; D=$(dirname "$0")
r() { ssh -o ConnectTimeout=8 -o ServerAliveInterval=15 root@$H "$@"; }
scp -q "$KO" $D/hammer.py root@$H:/tmp/ || exit 1
r 'insmod /tmp/x6x00_dmafix.ko fix=0 && dmesg | grep x6x00_dmafix | tail -1' || exit 1
cnt() { r 'cd /sys/module/x6x00_dmafix/parameters && echo "fix=$(cat fix) calls=$(cat calls) crossed=$(cat crossed) retries=$(cat retries)"'; }
for mode in 0 1; do
    r "echo $mode > /sys/module/x6x00_dmafix/parameters/fix"
    echo "== fix=$mode $(date +%H:%M:%S), before: $(cnt)"
    r "python3 /tmp/hammer.py $T > /tmp/ham_a 2>&1 & python3 /tmp/hammer.py $T > /tmp/ham_b 2>&1; wait; cat /tmp/ham_a /tmp/ham_b"
    echo "   after: $(cnt)"
done
r 'rmmod x6x00_dmafix; dmesg | grep x6x00_dmafix | tail -1'
