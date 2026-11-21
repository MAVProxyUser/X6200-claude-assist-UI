#!/usr/bin/env python3
"""hammer.py SECONDS: the capture's pointer read as fast as one core can (each /proc status read runs the driver's
pointer callback: sun6i-dma's residue); a hw_ptr jump over half a buffer between two reads microseconds apart is a wrap
ALSA counted from a position that stepped back. Prints the reads per second and every jump."""
import os, sys, time
P = "/proc/asound/card0/pcm0c/sub0/status"
end = time.time() + float(sys.argv[1])
fd = os.open(P, os.O_RDONLY)
n = jumps = 0
prev = None
t0 = time.time()
while time.time() < end:
    for _ in range(200):
        s = os.pread(fd, 1024, 0)
        i = s.find(b"hw_ptr")
        if i < 0:
            continue
        j = s.index(b":", i) + 1
        hw = int(s[j:s.index(b"\n", j)])
        if prev is not None and hw - prev > 44100:
            jumps += 1
            print(f"{time.strftime('%H:%M:%S')} jump {hw - prev:+d} frames ({(hw - prev) / 88200:+.3f} buffers) after {n} reads", flush=True)
        prev = hw
        n += 1
dt = time.time() - t0
print(f"{n} reads in {dt:.0f} s ({n / dt:.0f}/s), {jumps} jumps", flush=True)
