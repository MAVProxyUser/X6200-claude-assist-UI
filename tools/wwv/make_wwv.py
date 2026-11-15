#!/usr/bin/env python3
"""Test audio for tools/wwv/wwv_test.c, 11025 Hz mono float32.
  make_wwv.py synth OUT YEAR YDAY HH:MM MINUTES [noise]  a WWV signal made here: the ticks (5 ms of 1000 Hz, none at :29
      and :59, the minute's 800 ms tone at :00), the 500/600 Hz tones, the 100 Hz BCD code 15 dB over its off level,
      noise; starting 12.3 s before the first minute so the lock comes first
  make_wwv.py capture OUT  two minutes made of this radio's own 62 s off-air capture (radio_captures/modes/
      wwv_10000.wav, 10 MHz, day 312 04:20 UTC): its seconds :07-:59 of 04:20 and :00-:06 of 04:21, each a real
      second of audio, put in the order of one whole minute, twice
SPDX-License-Identifier: LGPL-2.1-or-later"""
import subprocess, sys, os
import numpy as np

FS = 11025


def bcd_bits(year, yday, hour, minute, dut1=-0.1):
    b = ['0'] * 60
    b[0] = '-'
    for s in (9, 19, 29, 39, 49, 59):
        b[s] = 'M'
    def put(val, secs, weights):
        for s, wgt in zip(secs, weights):
            if val & wgt: b[s] = '1'
    y = year % 100
    put(y % 10, (4, 5, 6, 7), (1, 2, 4, 8)); put(y // 10, (51, 52, 53, 54), (1, 2, 4, 8))
    put(minute % 10, (10, 11, 12, 13), (1, 2, 4, 8)); put(minute // 10, (15, 16, 17), (1, 2, 4))
    put(hour % 10, (20, 21, 22, 23), (1, 2, 4, 8)); put(hour // 10, (25, 26), (1, 2))
    put(yday % 10, (30, 31, 32, 33), (1, 2, 4, 8)); put(yday // 10 % 10, (35, 36, 37, 38), (1, 2, 4, 8))
    put(yday // 100, (40, 41), (1, 2))
    if dut1 >= 0: b[50] = '1'
    put(int(round(abs(dut1) * 10)), (56, 57, 58), (1, 2, 4))
    return b


def synth(out, year, yday, hhmm, minutes, noise=0.05):
    h, m = map(int, hhmm.split(':'))
    lead = 12.3
    n = int((lead + minutes * 60 + 3) * FS)
    t = np.arange(n) / FS
    x = np.zeros(n)
    rng = np.random.default_rng(1)
    for k in range(int(lead) + 1 + minutes * 60 + 2):
        start = lead + (k - int(lead) - 1) * 1.0 + (1 - (lead % 1))      # the seconds, :00 of the first minute at lead
        start = lead + (k - int(lead) - 1) + 1 - 1   # whole seconds from lead
    # second s of minute j starts at lead + 60*j + s, negative j for the lead-in
    for sec_abs in range(-int(lead) - 1, minutes * 60 + 2):
        t0 = lead + sec_abs
        if t0 < 0: continue
        j, s = divmod(sec_abs, 60)
        mm = (m + j) % 60; hh = (h + (m + j) // 60) % 24
        bits = bcd_bits(year, yday, hh, mm)
        i0 = int(t0 * FS)
        # tick, or the minute tone
        if s == 0:
            L = int(0.8 * FS); x[i0:i0 + L] += 0.6 * np.sin(2 * np.pi * 1000 * t[:L])
        elif s not in (29, 59):
            L = int(0.005 * FS); x[i0:i0 + L] += 0.6 * np.sin(2 * np.pi * 1000 * t[:L])
        # a tone most of the second (500/600 Hz alternate minutes), quiet 40 ms around the tick
        if s != 0:
            a0, a1 = i0 + int(0.03 * FS), i0 + int(0.99 * FS)
            x[a0:a1] += 0.25 * np.sin(2 * np.pi * (500 if (mm % 2) else 600) * t[a0:a1])
        # the 100 Hz: on (1) from 30 ms to the drop, off (0.178, -15 dB) after
        sym = bits[s]
        drop = {'-': 0.03, '0': 0.2, '1': 0.5, 'M': 0.8}[sym]
        a0, a1, a2 = i0, i0 + int(drop * FS), i0 + FS
        amp = np.full(FS, 0.178); amp[int(0.03 * FS):int(drop * FS)] = 1.0
        if sym == '-': amp[:] = 0.178
        seg = slice(i0, min(i0 + FS, n))
        L = seg.stop - seg.start
        x[seg] += 0.15 * amp[:L] * np.sin(2 * np.pi * 100 * t[seg])
    x += noise * rng.standard_normal(n)
    x.astype(np.float32).tofile(out)


def capture(out):
    top = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', os.path.join(top, 'radio_captures/modes/wwv_10000.wav'),
                          '-ac', '1', '-ar', str(FS), '-f', 'f32le', '-'], capture_output=True, check=True).stdout
    x = np.frombuffer(raw, dtype=np.float32)
    ep = 5283                    # the second's start in the file: 0.4792 s (the ticks folded)
    sec = lambda k: x[ep + k * FS: ep + (k + 1) * FS]    # the file's k-th whole second
    # file second 0 is :07 of 04:20; file second 53 is :00 of 04:21
    minute = [sec(53 + s) if s <= 6 else sec(s - 7) for s in range(60)]
    lead = [sec(k) for k in range(40, 53)]               # 13 s of real signal to lock on first
    y = np.concatenate(lead + minute + minute + minute[:3])
    y.astype(np.float32).tofile(out)


if __name__ == '__main__':
    if sys.argv[1] == 'synth':
        synth(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], int(sys.argv[6]),
              float(sys.argv[7]) if len(sys.argv) > 7 else 0.05)
    else:
        capture(sys.argv[2])
