#!/usr/bin/env python3
"""Exact repeats in a radio's TCI receive audio: RECORD seconds of it (int16 24 kHz mono, PINGs answered), then every
0.25 s block compared with the audio 2.000 s before (48000 samples, +-64 searched): a correlation over 0.99 is the
same audio sent twice (PulseAudio reading its 2 s ALSA ring again). Receive only.

    rep2s.py HOST SECONDS [NAME]
"""
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench"))
from tones import TciRx, FS

host, sec = sys.argv[1], float(sys.argv[2])
name = sys.argv[3] if len(sys.argv) > 3 else host.split(".")[0]
here = os.path.dirname(os.path.abspath(__file__))
rx = TciRx(host)
time.sleep(1)
t0 = time.time()
while time.time() - t0 < sec:
    time.sleep(5)
x, start = rx.audio(t0, time.time())
with rx.lock:
    arr = [t for t, _ in rx.frames if t >= t0]
rx.close()
np.save(os.path.join(here, f"{name}.npy"), x)
print(f"{name}: {len(x) / FS:.1f} s of audio in {time.time() - t0:.1f} s, rms {20*np.log10(np.std(x)+1e-12):.1f} dBFS", flush=True)
B, L = FS // 4, 2 * FS
hits = []
for i in range(L + 64, len(x) - B - 64, B):
    a = x[i:i + B]
    if np.std(a) < 1e-5:
        continue
    best = (0, 0)
    for d in range(-64, 65):
        b = x[i - L + d:i - L + d + B]
        c = float(np.dot(a - a.mean(), b - b.mean()) / (np.std(a) * np.std(b) * B + 1e-20))
        if c > best[0]:
            best = (c, d)
    if best[0] > 0.99:
        hits.append((i / FS, best[0], best[1]))
print(f"  blocks repeated from 2.000 s before: {len(hits)}")
for t, c, d in hits[:40]:
    print(f"    at {t:7.2f} s: corr {c:.4f}, lag {2 + d / FS:.5f} s")
# a jump in arrival: frames' wall-clock gaps over 0.5 s
g = np.diff(arr)
big = np.flatnonzero(g > 0.5)
print(f"  arrival gaps over 0.5 s: {len(big)}" + "".join(f"; {arr[k] - t0:.1f} s: {g[k]:.2f} s" for k in big[:10]))
