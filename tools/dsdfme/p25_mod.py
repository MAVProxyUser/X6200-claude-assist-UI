#!/usr/bin/env python3
"""GoldCorpHF - p25_gen's (or ysf_gen's) dibits as 4-level FSK for cabled tests: 4800 symbols/s, dibit 01 +1800 Hz,
00 +600, 10 -600, 11 -1800. Shaped with a raised cosine (rolloff 0.2: P25's C4FM, which dsd-fme takes unfiltered), or
with SHAPE=rrc in the environment a root raised cosine (DMR's and YSF's, whose receivers, dsd-fme's among them, filter
with the other half: with a raised cosine sent, the voice of a YSF call over the cable was garbled, envelope r 0.44).
Half a second of carrier before and after.

    p25_mod.py disc CALL.dibits OUT.s16              48 kHz s16 FM discriminator audio (+-5 kHz = +-16000)
    p25_mod.py iq CALL.dibits OUT.iq OFFSET_HZ       2 Msps int8 IQ for hackrf_transfer -t, OFFSET_HZ off centre
"""
import os
import sys
import numpy as np

LEVEL = {0: 1.0, 1: 3.0, 2: -1.0, 3: -3.0}
BAUD = int(os.environ.get("BAUD", "4800"))      # NXDN48: BAUD=2400 DEV=350 (+-1050 Hz outer)
DEV = float(os.environ.get("DEV", "600"))        # Hz for a +-1 symbol


def freq(dibits, fs):
    sps = fs // BAUD
    imp = np.zeros(len(dibits) * sps)
    imp[::sps] = [LEVEL[d] * DEV for d in dibits]
    a, span = 0.2, 8
    t = np.arange(-span * sps, span * sps + 1) / sps
    with np.errstate(divide="ignore", invalid="ignore"):
        if os.environ.get("SHAPE") == "rrc":
            h = (np.sin(np.pi * t * (1 - a)) + 4 * a * t * np.cos(np.pi * t * (1 + a))) / \
                (np.pi * t * (1 - (4 * a * t) ** 2))
            h[t == 0] = 1 - a + 4 * a / np.pi
            edge = np.isclose(np.abs(t), 1 / (4 * a))
            h[edge] = a / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * a)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * a)))
            h /= np.sqrt(np.sum(h ** 2) / sps)  # the cascade with the receiver's half: unit gain at symbol centres
        else:
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
