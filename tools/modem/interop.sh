#!/bin/sh
# x6x00-modem against BinModem itself, at every rate, at 24 and 16 kHz: the C transmitter's samples
# against BinModem's, BinModem's receiver on the C transmitter, the C receiver on BinModem's.
# Needs a BinModem checkout with the harness example built (README.md, "Checked against BinModem").
#   interop.sh BINMODEM_DIR
set -e
B=${1:-../../build/binmodem}
R=$B/target/release/examples/faxpump
C=${BUILD:-build}/interop
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
[ -x "$R" ] || { echo "no $R: build BinModem's faxpump example first (README.md)"; exit 2; }
python3 -c "import random; random.seed(7); print(''.join(random.choice('01') for _ in range(9600)))" > "$D/bits.txt"
bad=0
for fs in 24000 16000; do
    for rate in v27_2400 v27_4800 v29_7200 v29_9600; do
        "$R" tx $rate "$D/bits.txt" "$D/rust.f64" $fs
        "$C" tx $rate "$D/bits.txt" "$D/c.f64" $fs
        "$C" rx $rate "$D/rust.f64" $fs > "$D/c_rx.txt" 2>/dev/null
        "$R" rx $rate "$D/c.f64" $fs > "$D/rust_rx.txt" 2>/dev/null
        python3 - "$D" $rate $fs <<'PY' || bad=1
import sys, struct
d, rate, fs = sys.argv[1:4]
bits = open(d + "/bits.txt").read().strip()
def samples(p):
    b = open(p, "rb").read()
    return struct.unpack("<%dd" % (len(b) // 8), b)
a, c = samples(d + "/rust.f64"), samples(d + "/c.f64")
diff = max(abs(x - y) for x, y in zip(a, c)) if len(a) == len(c) else float("inf")
c_ok = bits in open(d + "/c_rx.txt").read()
r_ok = bits in open(d + "/rust_rx.txt").read()
print("%5s Hz %-9s samples %s (largest difference %.1e) | C rx of BinModem: %s | BinModem rx of C: %s" % (
    fs, rate, "equal" if diff < 1e-5 else "DIFFER", diff, "exact" if c_ok else "FAILED", "exact" if r_ok else "FAILED"))
sys.exit(0 if diff < 1e-5 and c_ok and r_ok else 1)
PY
    done
done
exit $bad
