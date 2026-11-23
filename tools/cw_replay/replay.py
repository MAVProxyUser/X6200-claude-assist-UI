#!/usr/bin/env python3
"""A WAV recording (the radio's RX audio, e.g. a bench capture from its TCI) through the GUI's CW decoder, offline:
resampled to the receiver's 4 kHz and fed to cw_replay (make first). Prints the text, the WPM and the tone locked.

    python3 replay.py RECORDING.wav [LO HI [FLOOR_DB]]      (the CW filter's edges, default 0 3000; Dec SNR, 8)
"""
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lo, hi = (sys.argv[2], sys.argv[3]) if len(sys.argv) > 3 else ("0", "3000")
    floor = sys.argv[4] if len(sys.argv) > 4 else "8"
    with wave.open(sys.argv[1]) as f:
        rate, ch, n = f.getframerate(), f.getnchannels(), f.getnframes()
        x = np.frombuffer(f.readframes(n), "<i2").astype(np.float32).reshape(-1, ch)[:, 0] / 32768
    y = resample_poly(x, 4000, rate).astype(np.float32)
    with tempfile.NamedTemporaryFile(suffix=".f32") as t:
        y.tofile(t.name)
        with open(t.name, "rb") as fin:
            out = subprocess.run([os.path.join(HERE, "cw_replay"), lo, hi, floor], stdin=fin, capture_output=True,
                                 text=True)
    print(f"{os.path.basename(sys.argv[1])}: peak {20 * np.log10(np.max(np.abs(x)) + 1e-9):.1f} dBFS, band {lo}-{hi} "
          f"Hz: {out.stdout.strip()}")


if __name__ == "__main__":
    main()
