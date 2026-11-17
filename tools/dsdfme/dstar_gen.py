#!/usr/bin/env python3
"""GoldCorpHF - a D-STAR voice transmission for cabled tests (a HackRF through an attenuator into the RTL stick, never
over the air) (JARL layout, as dsd-fme decodes it): preamble, frame sync, the radio
header (CRC-CCITT, convolutional K=3 rate 1/2, interleaved, scrambled), superframes of AMBE silence with the data sync
every 21st frame and a slow-data text message, then the terminator. GMSK BT 0.5 at 4800 bit/s, +-1200 Hz.

    dstar_gen.py disc OUT.s16 [seconds]      48 kHz s16 FM discriminator audio (+-5 kHz = +-16000), for dsd-fme's stdin
    dstar_gen.py iq OUT.iq OFFSET_HZ [seconds]   2 Msps int8 IQ for hackrf_transfer -t, the signal OFFSET_HZ off centre

DSTAR_VOICE=FILE in the environment: the voice frames from FILE (9 bytes each, as D-STAR carries them: an AMBE-3000
in D-STAR mode, rate words 0x0130 0x0763 0x4000 0 0 0x0048, gives them so), its length the transmission's; AMBE
silence else.
"""
import os
import sys
import numpy as np

MY, MY2, UR, RPT1, RPT2 = "N0CALL", "TEST", "CQCQCQ", "DIRECT", "DIRECT"
TEXT = "GOLDCORPHF CABLE TST"        # 20 characters: the slow data's message

SCR = [0,0,0,0,1,1,1,0,1,1,1,1,0,0,1,0,1,1,0,0,1,0,0,1,0,0,0,0,0,0,1,0,
       0,0,1,0,0,1,1,0,0,0,1,0,1,1,1,0,1,0,1,1,0,1,1,0,0,0,0,0,1,1,0,0,
       1,1,0,1,0,1,0,0,1,1,1,0,0,1,1,1,1,0,1,1,0,1,0,0,0,0,1,0,1,0,1,0,
       1,1,1,1,1,0,1,0,0,1,0,1,0,0,0,1,1,0,1,1,1,0,0,0,1,1,1,1,1,1,1,0]


def scr_bits(n):
    """x^7 + x^4 + 1, all ones to start: the 127-bit sequence dsd-fme's table holds"""
    s, out = [1] * 7, []
    for _ in range(n):
        b = s[3] ^ s[6]
        out.append(b)
        s = [b] + s[:6]
    return out


def lsb_bits(data):
    return [(b >> i) & 1 for b in data for i in range(8)]


def crc_ccitt(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return (~crc) & 0xFFFF


def header_bits():
    h = bytes([0, 0, 0]) + RPT2.ljust(8).encode() + RPT1.ljust(8).encode() + UR.ljust(8).encode() + \
        MY.ljust(8).encode() + MY2.ljust(4).encode()
    c = crc_ccitt(h)
    h += bytes([c & 0xFF, c >> 8])
    assert len(h) == 41
    u = lsb_bits(h) + [0, 0]                    # 330 bits, the encoder flushed
    d1 = d2 = 0
    coded = []
    for b in u:                                 # G1 = 111 first, G2 = 101 second
        coded += [b ^ d1 ^ d2, b ^ d2]
        d2, d1 = d1, b
    assert len(coded) == 660
    k, order = 0, []                            # dsd-fme's deinterleave: out[k] = in[loop], k += 24 ...
    for _ in range(660):
        order.append(k)
        k += 24
        if k >= 672:
            k -= 671
        elif k >= 660:
            k -= 647
    tx = [coded[order[i]] for i in range(660)]
    pn = scr_bits(720)
    return [tx[i] ^ pn[i] for i in range(660)]


def stream(seconds):
    pn = scr_bits(720)
    full = [0,0,0,0,1,1,1,0,1,1,1,1,0,0,1,0,1,1,0,0,1,0,0,1,0,0,0,0,0,0,1,0]
    assert pn[:32] == full, "scrambler"         # the table dsd-fme holds
    bits = lsb_bits([0x55] * 8)                 # preamble: 1010...10
    bits += [1,1,1,0,1,1,0,0,1,0,1,0,0,0,0]     # frame sync
    bits += header_bits()
    silence = bytes([0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8])
    txt = TEXT.ljust(20)[:20]
    sd = b""
    for i in range(4):                          # the message: four blocks of 0x40+i and five characters
        sd += bytes([0x40 + i]) + txt[i * 5:(i + 1) * 5].encode()
    frames = int(seconds / 0.02)
    voice = open(os.environ["DSTAR_VOICE"], "rb").read() if os.environ.get("DSTAR_VOICE") else None
    if voice:
        frames = len(voice) // 9
    for f in range(frames):
        bits += lsb_bits(voice[f * 9:f * 9 + 9] if voice else silence)
        n = f % 21
        if n == 0:
            bits += lsb_bits([0x55, 0x2D, 0x16])    # data sync
        else:
            j = (n - 1) * 3
            d = sd[j:j + 3] if j + 3 <= len(sd) else bytes([0x66, 0x66, 0x66])
            bits += lsb_bits([d[0] ^ 0x70, d[1] ^ 0x4F, d[2] ^ 0x93])
    bits += lsb_bits([0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A])     # terminator
    return bits


def gmsk_freq(bits, fs):
    """the instantaneous frequency, Hz: NRZ through a Gaussian of BT 0.5, +-1200 Hz"""
    sps = fs / 4800.0
    n = int(len(bits) * sps)
    t = np.arange(n) / sps
    nrz = np.array([1.0 if b else -1.0 for b in bits])[np.minimum(t.astype(int), len(bits) - 1)]
    bt = 0.5
    sigma = np.sqrt(np.log(2)) / (2 * np.pi * bt) * sps
    k = np.arange(-int(4 * sigma), int(4 * sigma) + 1)
    g = np.exp(-k ** 2 / (2 * sigma ** 2))
    g /= g.sum()
    return np.convolve(nrz, g, mode="same") * 1200.0


def main():
    mode = sys.argv[1]
    if mode == "disc":
        out, secs = sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 6
        fs = 48000
        f = gmsk_freq(stream(secs), fs)
        lead = np.zeros(int(0.5 * fs))
        a = np.concatenate([lead, f, lead]) / 5000.0 * 16000
        a += np.random.default_rng(1).normal(0, 300, len(a))
        np.clip(a, -32767, 32767).astype("<i2").tofile(out)
    else:
        out, off = sys.argv[2], float(sys.argv[3])
        secs = float(sys.argv[4]) if len(sys.argv) > 4 else 6
        fs = 2000000
        f = gmsk_freq(stream(secs), 48000)
        # 48 kHz frequency up to 2 Msps by holding (the Gaussian has already smoothed it far below 24 kHz)
        idx = (np.arange(int(len(f) * fs / 48000)) * 48000 / fs).astype(int)
        f2 = np.concatenate([np.zeros(fs // 2), f[idx], np.zeros(fs // 2)]) + off
        ph = 2 * np.pi * np.cumsum(f2) / fs
        iq = np.empty(2 * len(ph), dtype=np.int8)
        iq[0::2] = np.round(np.cos(ph) * 100).astype(np.int8)
        iq[1::2] = np.round(np.sin(ph) * 100).astype(np.int8)
        iq.tofile(out)


main()
