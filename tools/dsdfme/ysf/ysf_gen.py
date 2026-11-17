#!/usr/bin/env python3
"""GoldCorpHF - a YSF (Yaesu System Fusion) V/D type 2 voice call as dibits, for cabled tests (a HackRF through an
attenuator into the RTL stick, never over the air): a header (HC: CSD1 = destination and source callsigns, CSD2),
communication frames (CC: FICH, then five of a 20-dibit DCH slice and a 52-dibit VeCH), a terminator (TC). Laid out as
dsd-fme's ysf.c reads it: its pn95 whitening and VeCH interleave tables and its Golay (24,12) generator are read from
its sources; the convolutional code is K=5, G1 = 1+D^3+D^4, G2 = 1+D+D^2+D^4 (its soft Viterbi's), CRC-16
x^16+x^12+x^5+1 inverted (its crc16ysf).

The voice: 49-bit AMBE+2 vectors in dsd-fme's ambe_d order (one byte a bit, dmr_to_ambed's output), cycled.

    ysf_gen.py VOICE.ambed FRAMES > call.dibits      (CC frames, 100 ms each; then p25_mod.py modulates the dibits)
    DSD_SRC=<dsd-fme's source tree>, build/x6200/dsdfme/src by default
"""
import os
import re
import sys

SRC = os.environ.get("DSD_SRC", os.path.join(os.path.dirname(__file__), "../../../build/x6200/dsdfme/src"))
DST, SRCCALL, DOWN, UP = "CQCQCQ", "N0CALL", "TEST", "GOLDCORP"


def table(path, name, n):
    s = open(os.path.join(SRC, path)).read()
    s = re.sub(r"//[^\n]*", "", s)
    m = re.search(name + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", s)
    v = [int(x) for x in re.findall(r"\d+", m.group(1))]
    assert len(v) >= n, (name, len(v))
    return v[:n]


PN95 = table("src/ysf.c", "pn95", 512)
VD2 = table("src/ysf.c", "vd2Interleave", 104)
GOLAY_G = table("src/fec.c", "Golay_24_12_m_G", 288)
SYNC = 0xD471C9634D


def bits_of(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def crc_raw(bits):
    poly, crc = (1 << 12) | (1 << 5) | 1, 0
    for b in bits:
        crc = ((crc << 1) | b) & 0x1FFFF
        if crc & 0x10000:
            crc = (crc & 0xFFFF) ^ poly
    return crc


def with_crc(data):
    r = crc_raw(data + [0] * 16) ^ 0xFFFF
    out = data + bits_of(r, 16)
    assert crc_raw(out) ^ 0xFFFF == 0
    return out


def golay(b12):
    return [sum(b12[i] * GOLAY_G[24 * i + j] for i in range(12)) % 2 for j in range(24)]


def conv(bits):
    """K=5 from a zero state, 4 tail zeros: two coded bits a step, G1's first"""
    sr, out = [0] * 4, []
    for b in bits + [0] * 4:
        out += [b ^ sr[2] ^ sr[3], b ^ sr[0] ^ sr[1] ^ sr[3]]
        sr = [b] + sr[:3]
    return out


def interleave(coded, n_dibits, depth):
    """coded bits -> dibits as sent: dsd-fme reads buf[j + i*depth] = input[i + j*20] (20 columns)"""
    d = [(coded[2 * k] << 1) | coded[2 * k + 1] for k in range(n_dibits)]
    tx = [0] * n_dibits
    for i in range(20):
        for j in range(depth):
            tx[i + j * 20] = d[j + i * depth]
    return tx


def fich(fi, fn, ft, dt=2):
    info = bits_of(fi, 2) + bits_of(2, 2) + bits_of(0, 2) + bits_of(0, 2) + bits_of(0, 2) + bits_of(fn, 3) + \
        bits_of(ft, 3) + [0, 0] + bits_of(0, 3) + [0] + bits_of(dt, 2) + [0] + bits_of(0, 7)
    assert len(info) == 32
    b = with_crc(info)
    coded = conv(sum((golay(b[k * 12:(k + 1) * 12]) for k in range(4)), []))
    assert len(coded) == 200
    return interleave(coded, 100, 5)


def text_bits(s, n):
    return sum((bits_of(ord(c), 8) for c in s.ljust(n)[:n]), [])


def dch(data_bits):
    """80 bits (DCH, V/D 2) or 160 (CSD, header/terminator): whitened, CRC-16, K=5 coded, interleaved"""
    n = len(data_bits)
    w = [data_bits[i] ^ PN95[i] for i in range(n)]
    coded = conv(with_crc(w))
    return interleave(coded, len(coded) // 2, len(coded) // 40)


def vech(ambe_d):
    v = [0] * 104
    for k in range(27):
        v[3 * k] = v[3 * k + 1] = v[3 * k + 2] = ambe_d[k]
    for k in range(22):
        v[81 + k] = ambe_d[27 + k]
    out = []
    for j in range(52):
        m, l = VD2[2 * j], VD2[2 * j + 1]
        out.append(((v[m] ^ PN95[m]) << 1) | (v[l] ^ PN95[l]))
    return out


def frame_sync():
    b = bits_of(SYNC, 40)
    return [(b[2 * k] << 1) | b[2 * k + 1] for k in range(20)]


def header(fi):
    out = frame_sync() + fich(fi, 0, 0)
    c1 = dch(text_bits(DST, 10) + text_bits(SRCCALL, 10))
    c2 = dch(text_bits(DOWN, 10) + text_bits(UP, 10))
    for i in range(10):
        out += (c1 if i % 2 == 0 else c2)[(i // 2) * 36:(i // 2) * 36 + 36]
    return out


def main():
    v = open(sys.argv[1], "rb").read()
    voice = [list(v[k * 49:(k + 1) * 49]) for k in range(len(v) // 49)]
    frames, out, vi = int(sys.argv[2]), [], 0
    out += header(0) * 2
    fields = [DST, SRCCALL, DOWN, UP] + [""] * 3
    for f in range(frames):
        fn = f % 7
        out += frame_sync() + fich(1, fn, 6)
        d = dch(text_bits(fields[fn], 10))
        for blk in range(5):
            out += d[blk * 20:(blk + 1) * 20] + vech(voice[vi % len(voice)])
            vi += 1
    out += header(2)
    sys.stdout.buffer.write(bytes(out))
    sys.stderr.write("ysf_gen: %d dibits (%.2f s), %d voice frames\n" % (len(out), len(out) / 4800, vi))


main()
