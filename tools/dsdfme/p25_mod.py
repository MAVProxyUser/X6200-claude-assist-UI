#!/usr/bin/env python3
"""GoldCorpHF - p25_gen's dibits as C4FM (P25 phase 1) for cabled tests: 4800 symbols/s, dibit 01 +1800 Hz, 00 +600,
10 -600, 11 -1800, a raised cosine (rolloff 0.2) between symbols. Half a second of carrier before and after.

    p25_mod.py disc CALL.dibits OUT.s16              48 kHz s16 FM discriminator audio (+-5 kHz = +-16000)
    p25_mod.py iq CALL.dibits OUT.iq OFFSET_HZ       2 Msps int8 IQ for hackrf_transfer -t, OFFSET_HZ off centre
"""
import sys
import numpy as np

LEVEL = {0: 1.0, 1: 3.0, 2: -1.0, 3: -3.0}


def freq(dibits, fs):
    sps = fs // 4800
    imp = np.zeros(len(dibits) * sps)
    imp[::sps] = [LEVEL[d] * 600.0 for d in dibits]
    a, span = 0.2, 8
    t = np.arange(-span * sps, span * sps + 1) / sps
    with np.errstate(divide="ignore", invalid="ignore"):
        h = np.sinc(t) * np.cos(np.pi * a * t) / (1 - (2 * a * t) ** 2)
    h[np.isinf(h) | np.isnan(h)] = np.pi / 4 * np.sinc(1 / (2 * a))
    return np.convolve(imp, h, mode="same")


def main():
    mode, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
    dibits = list(open(src, "rb").read())
    f = freq(dibits, 48000)
    lead = np.zeros(24000)
    f = np.concatenate([lead, f, lead])
    if mode == "disc":
        a = f / 5000.0 * 16000 + np.random.default_rng(1).normal(0, 300, len(f))
        np.clip(a, -32767, 32767).astype("<i2").tofile(out)
    else:
        fs, off = 2000000, float(sys.argv[4])
        idx = (np.arange(int(len(f) * fs / 48000)) * 48000 / fs).astype(int)
        ph = 2 * np.pi * np.cumsum(f[idx] + off) / fs
        iq = np.empty(2 * len(ph), dtype=np.int8)
        iq[0::2] = np.round(np.cos(ph) * 100).astype(np.int8)
        iq[1::2] = np.round(np.sin(ph) * 100).astype(np.int8)
        iq.tofile(out)


main()
