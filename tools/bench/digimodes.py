#!/usr/bin/env python3
"""RTTY and PSK31 audio made on the computer, for a radio to transmit through its TCI (tci_tone_tx.py @FILE) and the
other radio's app to decode into its text log (/mnt/digi_text, digi_textlog.c): the suite's keyboard-mode paths.
Made as the radios' own apps make them: RTTY as rtty.c's tx_start (ITA2, 1 start, 5 data LSB first, 1.5 stop bits,
LTRS first, the defaults 800 Hz centre, 450 Hz shift, 50 baud, reverse on: in USB the mark is the lower tone), PSK31
from psk_tx.c's own varicode table (BPSK, 31.25 baud, a raised-cosine reversal for each 0 bit, "00" between
characters). 16-bit mono at 24 kHz, the TCI's rate.

    digimodes.py rtty|psk31 TEXT OUT.raw
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import math
import os
import re
import sys

import numpy as np

FS = 24000
HERE = os.path.dirname(os.path.abspath(__file__))

# ITA2: the letters' and the figures' codes (US TTY figures)
LTRS = {"E": 1, "A": 3, "S": 5, "I": 6, "U": 7, "D": 9, "R": 10, "J": 11, "N": 12, "F": 13, "C": 14, "K": 15, "T": 16,
        "Z": 17, "L": 18, "W": 19, "H": 20, "Y": 21, "P": 22, "Q": 23, "O": 24, "B": 25, "G": 26, "M": 28, "X": 29,
        "V": 30}
FIGS = {"3": 1, "-": 3, "8": 6, "7": 7, "4": 10, ",": 12, "!": 13, ":": 14, "(": 15, "5": 16, "+": 17, ")": 18,
        "2": 19, "6": 21, "0": 22, "1": 23, "9": 24, "?": 25, ".": 28, "/": 29, "=": 30}
SPACE, CR, LF, LTRS_SHIFT, FIGS_SHIFT = 4, 8, 2, 31, 27


def rtty_codes(text):
    codes, case = [LTRS_SHIFT], "L"
    for ch in text.upper():
        if ch == " ":
            codes.append(SPACE)
        elif ch in LTRS:
            if case != "L":
                codes.append(LTRS_SHIFT)
                case = "L"
            codes.append(LTRS[ch])
        elif ch in FIGS:
            if case != "F":
                codes.append(FIGS_SHIFT)
                case = "F"
            codes.append(FIGS[ch])
    return codes + [CR, LF]


def rtty_audio(text, center=800.0, shift=450.0, baud=50.0, mark_low=True, amp=0.5):
    """FSK with its phase kept across the bits: 1 s of mark, the codes, 3 bits of mark after"""
    mark = center - shift / 2 if mark_low else center + shift / 2
    space = center + shift / 2 if mark_low else center - shift / 2
    bits = [(1, 1.0 * baud)]                        # (mark?, bits) — the idle mark before the first character
    for code in rtty_codes(text):
        bits.append((0, 1))                         # start
        bits += [((code >> i) & 1, 1) for i in range(5)]
        bits.append((1, 1.5))                       # stop
    bits.append((1, 3))
    out, phase = [], 0.0
    spb = FS / baud
    for m, n in bits:
        k = int(round(n * spb))
        w = 2 * math.pi * (mark if m else space) / FS
        out.append(np.sin(phase + w * np.arange(k)))
        phase = math.fmod(phase + w * k, 2 * math.pi)
    return amp * np.concatenate(out)


def varicode():
    """psk_tx.c's own table: the radios' PSK31 and this one alike"""
    src = open(os.path.join(HERE, "..", "..", "src", "psk_tx.c")).read()
    body = src[src.index("varicode[128]"):]
    body = body[body.index("{") + 1:body.index("};")]
    codes = re.findall(r'"([01]+)"', body)
    if len(codes) != 128:
        raise ValueError(f"psk_tx.c's varicode: {len(codes)} codes, not 128")
    return codes


def psk31_audio(text, center=1000.0, amp=0.5):
    """BPSK31: 64 reversals to lock on, each character's code and "00", 32 bits of steady carrier, then off; each 0
    bit a reversal shaped by a cosine through zero, each 1 bit the carrier held"""
    vc = varicode()
    bits = "0" * 64 + "".join(vc[ord(c)] + "00" for c in text if ord(c) < 128) + "1" * 32
    spb = int(FS / 31.25)                               # 768 samples a bit at 24 kHz
    t = np.arange(spb)
    sign, env = 1.0, []
    for b in bits:
        if b == "0":                                    # a reversal: from +sign to -sign through zero
            env.append(sign * np.cos(np.pi * t / spb))
            sign = -sign
        else:
            env.append(np.full(spb, sign))
    env = np.concatenate(env)
    car = np.sin(2 * np.pi * center * np.arange(len(env)) / FS)
    ramp = int(0.02 * FS)                               # no click at the start or the end
    env[:ramp] *= np.linspace(0, 1, ramp)
    env[-ramp:] *= np.linspace(1, 0, ramp)
    return amp * env * car


def write_raw(x, path):
    (np.clip(x, -1, 1) * 32767).astype("<i2").tofile(path)
    return len(x) / FS


if __name__ == "__main__":
    mode, text, out = sys.argv[1], sys.argv[2], sys.argv[3]
    x = rtty_audio(text) if mode == "rtty" else psk31_audio(text)
    print(f"{mode}: {write_raw(x, out):.1f} s into {out}")
