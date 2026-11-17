#!/usr/bin/env python3
"""GoldCorpHF - an NXDN conventional (RDCH) voice call as dibits, for cabled tests (a HackRF through an attenuator into
the RTL stick, never over the air): two frames of FACCH1 VCALL (LICH 0x50: FACCH1 in both halves), then voice frames
(LICH 0x56: four AMBE+2 frames each) with the VCALL message spread over the SACCH superframe, laid out as dsd-fme's
nxdn_frame.c, nxdn_deperm.c and nxdn_voice.c read them: its PERM_12_5 / PERM_16_9 interleaves and voice tables read from
its sources, K=5 code (1+D^3+D^4, 1+D+D^2+D^4) punctured as it depunctures, CRC-6 and CRC-12 as it checks them, the
frame scrambled by its pn95 (seed 0xE4). Checked against dsd-fme's own Viterbi on the Mac before any RF.

The voice: DMR AMBE+2 frames (72 bits as sent, 9 bytes each: the USB-3000's at rate 33), whose four codewords NXDN
carries in the same interleave as DMR (asserted from the tables).

    nxdn_gen.py FRAMES.ambe VOICE_FRAMES [RAN SRC DST] > call.dibits
    then BAUD=2400 DEV=350 SHAPE=rrc p25_mod.py ...   (NXDN48: 2400 symbols/s, +-1050 Hz outer)
    DSD_SRC=<dsd-fme's source tree>, build/x6200/dsdfme/src by default
"""
import os
import re
import sys

SRC = os.environ.get("DSD_SRC", os.path.join(os.path.dirname(__file__), "../../../build/x6200/dsdfme/src"))


def table(path, name):
    s = open(os.path.join(SRC, path)).read()
    s = re.sub(r"//[^\n]*", "", s)
    m = re.search(name + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", s)
    return [int(x) for x in re.findall(r"\d+", m.group(1))]


P12, P16 = table("include/nxdn_const.h", "PERM_12_5"), table("include/nxdn_const.h", "PERM_16_9")
NW, NX, NY, NZ = (table("src/nxdn_voice.c", n) for n in ("nW", "nX", "nY", "nZ"))
# DMR's AMBE interleave (dsd-fme's rW..rZ): NXDN's is the same, so DMR's 72 bits go across as they are
RW = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2]
RX = [23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6,
      7, 5, 6, 4]
RY = [0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3]
RZ = [5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12,
      1, 11, 0]
assert (NW[:36], NX[:36], NY[:36], NZ[:36]) == (RW, RX, RY, RZ), "NXDN's voice interleave is not DMR's"

FSW = 0xCDF59
SACCH_P = [1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0]
FACCH1_P = [1, 0, 1, 1]


def bits_of(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def crc_lfsr(bits, n, taps):
    """dsd-fme's crc6 / crc12f: an n-stage register of ones, a = bit ^ s[0], s[i] = s[i+1] (^ a where tapped)"""
    s = [1] * n
    for b in bits:
        a = b ^ s[0]
        s = [s[i + 1] ^ (a if i in taps else 0) for i in range(n - 1)] + [a]
    return s


def crc6(bits):
    return crc_lfsr(bits, 6, (0, 3, 4))


def crc12(bits):
    return crc_lfsr(bits, 12, (0, 8, 9, 10))


def conv(bits):
    sr, out = [0] * 4, []
    for b in bits + [0] * 4:
        out += [b ^ sr[2] ^ sr[3], b ^ sr[0] ^ sr[1] ^ sr[3]]
        sr = [b] + sr[:3]
    return out


def coded(info, crc, punct, perm):
    c = conv(info + crc(info))
    p = [c[i] for i in range(len(c)) if punct[i % len(punct)]]
    return [p[perm[i]] for i in range(len(p))]


def vcall(src, dst, nbits):
    m = bits_of(0x01, 8) + bits_of(0, 8) + bits_of(1, 3) + bits_of(0, 5) + bits_of(src, 16) + bits_of(dst, 16) + \
        bits_of(0, 2) + bits_of(0, 6)
    return (m + [0] * 80)[:nbits]


def to_dibits(bits):
    return [(bits[2 * k] << 1) | bits[2 * k + 1] for k in range(len(bits) // 2)]


def scramble(dibits):
    lfsr, out = 0xE4, list(dibits)
    for i in range(len(out)):
        if lfsr & 1:
            out[i] ^= 2
        bit = ((lfsr >> 4) ^ lfsr) & 1
        lfsr = (lfsr >> 1) | (bit << 8)
    return out


def lich(v):
    full = (v << 1) | ((v >> 6 ^ v >> 5 ^ v >> 4 ^ v >> 3) & 1)    # parity of bits 7..4 of lich<<1
    return [(b << 1) | 1 for b in bits_of(full, 8)]


def frame(lich_v, sacch_info, payload_dibits):
    body = lich(lich_v) + to_dibits(coded(sacch_info, crc6, SACCH_P, P12)) + payload_dibits
    assert len(body) == 182, len(body)
    return to_dibits(bits_of(FSW, 20)) + scramble(body)


def main():
    v = open(sys.argv[1], "rb").read()
    voice = [v[k * 9:(k + 1) * 9] for k in range(len(v) // 9)]
    nvoice = int(sys.argv[2])
    ran = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    src = int(sys.argv[4]) if len(sys.argv) > 4 else 1234
    dst = int(sys.argv[5]) if len(sys.argv) > 5 else 100
    msg72 = vcall(src, dst, 72)
    out, vi, sf = [], 0, 0

    def sacch():
        nonlocal sf
        info = bits_of([3, 2, 1, 0][sf], 2) + bits_of(ran, 6) + msg72[sf * 18:(sf + 1) * 18]
        sf = (sf + 1) % 4
        return info

    f1 = to_dibits(coded(vcall(src, dst, 80), crc12, FACCH1_P, P16))
    for _ in range(2):                      # call set-up: VCALL in both halves
        out += frame(0x50, sacch(), f1 + f1)
    sf = 0
    while vi < nvoice:
        pay = []
        for _ in range(4):
            b = voice[vi % len(voice)]
            pay += to_dibits([(b[k // 8] >> (7 - k % 8)) & 1 for k in range(72)])
            vi += 1
        out += frame(0x56, sacch(), pay)
    sys.stdout.buffer.write(bytes(out))
    sys.stderr.write("nxdn_gen: %d dibits, %d voice frames\n" % (len(out), vi))


main()
