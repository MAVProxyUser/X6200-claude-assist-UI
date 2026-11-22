#!/usr/bin/env python3
"""acoustic.py's judgement on made-up data, no radio: the staircase (every step's frames, their times in order), a
step missing, steps out of order, a spectrum frame's strongest step found over its quiet, and the mic radio's capture
read into its steps. SPDX-License-Identifier: LGPL-2.1-or-later"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import acoustic  # noqa: E402

TONES = [2000, 1700, 1400, 1100, 800, 500]
results = []


def check(ok, what):
    results.append(ok)
    print(("ok    " if ok else "WRONG ") + what)


def pts(order, frames=8):
    return [(k + i * 0.1, hz + 7, 30.0) for k, hz in enumerate(order) for i in range(frames)]


steps, heard, ordered = acoustic.staircase(pts(TONES), TONES, 60)
check(heard and ordered, "six steps, eight frames each, in order: heard in order")
steps, heard, ordered = acoustic.staircase(pts(TONES[:3] + TONES[4:]), TONES, 60)
check(not heard, "a step missing: not heard")
steps, heard, ordered = acoustic.staircase(pts([2000, 1400, 1700, 1100, 800, 500]), TONES, 60)
check(heard and not ordered, "two steps swapped: heard, not in order")
steps, heard, ordered = acoustic.staircase(pts(TONES, frames=2), TONES, 60)
check(not heard, f"two frames a step: under {acoustic.STEP_FRAMES}, not heard")


class FakeRemote:
    def __init__(self, spectra):
        self.spectra = spectra


class FakeRadio:
    def __init__(self, spectra):
        self.r = FakeRemote(spectra)


F, WID, NB = 14200000, 100000, 1024
per = WID / NB
quiet = [-120] * NB
frames = []
for k, hz in enumerate(TONES):
    v = list(quiet)
    v[int((F + hz - (F - WID / 2)) / per)] = -60
    frames.append((10.0 + k, F, WID, v))
frames.append((5.0, F, WID, list(quiet)))
r = FakeRadio(frames)
q = acoustic.quiet_bins(r, 4.0, 6.0)
found, binhz = acoustic.spectrum_points(r, F, 9.0, 16.0, q)
check(len(found) == 6 and all(abs(off - hz) <= binhz for (t, off, rise), hz in zip(found, TONES)),
      f"each frame's step found over its quiet, within a bin ({binhz:.0f} Hz)")
x = np.concatenate([0.3 * np.sin(2 * np.pi * hz * np.arange(44100) / 44100) for hz in TONES]) * 32767
apts = acoustic.audio_points(x, TONES)
steps, heard, ordered = acoustic.staircase(apts, TONES, 40)
check(heard and ordered, "the mic radio's capture: its steps read in order")
bad = results.count(False)
print(f"acoustic: {len(results) - bad} of {len(results)} as they should be")
sys.exit(1 if bad else 0)
