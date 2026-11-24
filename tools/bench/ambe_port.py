#!/usr/bin/env python3
"""The radio's AMBE vocoder through its SDR daemon's port (127.0.0.1:2460, AMBEserver's way: a datagram one AMBE-3000
packet, the answer the chip's packet back; tools/rtlspec/ambe.c). Runs ON the radio, plain Python, no numpy.

    ambe_port.py id                                   the chip's product ID and version
    ambe_port.py parity [on|off]                      the chip's packets with or without their parity field
                                                      (PKT_PARITYMODE): set, then said as an answer shows it
    ambe_port.py enc RATE IN.s16 OUT.frames           8 kHz s16 (little endian) speech to channel frames
    ambe_port.py dec RATE IN.frames OUT.s16           channel frames to 8 kHz s16 speech

RATE: a rate index (33: DMR's AMBE+2 3600x2450, YSF's and NXDN's too) or dstar (rate words 0x0130 0x0763 0x4000 0 0
0x0048: D-STAR's AMBE 2400 with its FEC) or p25 (0x0558 0x086B 0x1030 0 0 0x0190: P25 phase 1's IMBE 7200x4400).
OUT.frames / IN.frames: one byte the frame's bit count, then its bytes. A frame the chip does not answer within
0.5 s is written as an empty frame (encoding) or 20 ms of silence (decoding), counted on stderr.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import socket
import struct
import sys

PORT = ("127.0.0.1", 2460)
DEBUG = bool(__import__("os").environ.get("AMBE_DEBUG"))
RATEP = {"dstar": (0x0130, 0x0763, 0x4000, 0, 0, 0x0048), "p25": (0x0558, 0x086B, 0x1030, 0, 0, 0x0190)}


def pkt(typ, fields):
    b = bytes([0x61]) + struct.pack(">H", len(fields) + 2) + bytes([typ]) + fields + b"\x2f"
    par = 0
    for x in b[1:]:
        par ^= x
    return b + bytes([par])


class Port:
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(0.5)
        self.missed = 0

    def ask(self, typ, fields, want=None):
        """One packet, the chip's answer: (type, fields without the parity field) or None"""
        for _ in range(2):
            self.s.sendto(pkt(typ, fields), PORT)
            try:
                while True:
                    r, _ = self.s.recvfrom(1024)
                    if len(r) < 5 or r[0] != 0x61:
                        continue
                    t, f = r[3], r[4:]
                    par = 0
                    for x in r[1:]:
                        par ^= x
                    if len(f) >= 2 and f[-2] == 0x2F and par == 0:  # a parity field (the DVMega's chip sends none
                        f = f[:-2]                                  # until asked: data may end in 0x2F)
                    if t == 0 and f[:1] == b"\x39":     # READY: the chip restarted (P25's full rate can make it)
                        sys.stderr.write("chip restarted (READY)\n")
                        return t, f
                    if want is None or t == want:
                        if DEBUG and (t == 1 and (len(f) < 2 or f[1] not in (72, 144)) or t == 2 and f[1:2] != b"\xa0"):
                            sys.stderr.write(f"odd answer ({len(r)} bytes): {r[:24].hex()} .. {r[-8:].hex()}\n")
                        return t, f
            except socket.timeout:
                continue
        self.missed += 1
        return None

    def rate(self, r):
        if r in RATEP:
            a = self.ask(0, b"\x0a" + struct.pack(">6H", *RATEP[r]), 0)
            ok = a and a[1][:2] == b"\x0a\x00"
        else:
            a = self.ask(0, bytes([0x09, int(r)]), 0)
            ok = a and a[1][:2] == b"\x09\x00"
        if not ok:
            sys.exit(f"rate {r} refused: {a}")


def main():
    p = Port()
    cmd = sys.argv[1]
    if cmd == "id":
        for f in (b"\x30", b"\x31"):
            a = p.ask(0, f, 0)
            print(a and a[1][1:].split(b"\0")[0].decode(errors="replace"))
        return
    if cmd == "parity":
        if len(sys.argv) > 2:
            a = p.ask(0, bytes([0x3F, 1 if sys.argv[2] == "on" else 0]), 0)
            if not a or a[1][:2] != b"\x3f\x00":
                sys.exit(f"parity mode refused: {a}")
        p.s.sendto(pkt(0, b"\x30"), PORT)          # an answer, whole: its parity field there or not
        r, _ = p.s.recvfrom(1024)
        par = 0
        for x in r[1:]:
            par ^= x
        print("parity on" if len(r) >= 6 and r[-2] == 0x2F and par == 0 else "parity off")
        return
    rate, src, dst = sys.argv[2], sys.argv[3], sys.argv[4]
    p.rate(rate)
    data = open(src, "rb").read()
    out = open(dst, "wb")
    n = 0
    def again(a, typ, fields, want):
        """After a READY (the chip restarted, at its power-on rate): the rate set again, the frame once more"""
        if a and a[0] == 0 and a[1][:1] == b"\x39":
            p.restarts += 1
            p.rate(rate)
            return p.ask(typ, fields, want)
        return a
    p.restarts = 0
    if cmd == "enc":
        # the encoder from fresh (PKT_INIT): the same speech gives the same frames whatever the chip did before (147
        # of 246 frames differed between two runs otherwise, 2026-11-24). Not at P25's full rate (its encoder's
        # reset restarts the chip there)
        if rate != "p25":
            p.ask(0, b"\x0b\x01", 0)
        for i in range(0, len(data) - 319, 320):
            pcm = struct.unpack("<160h", data[i:i + 320])
            fields = b"\x00\xa0" + struct.pack(">160h", *pcm)
            a = again(p.ask(2, fields, 1), 2, fields, 1)
            if a and a[1][:1] == b"\x01":
                bits = a[1][1]
                out.write(bytes([bits]) + a[1][2:2 + (bits + 7) // 8])
            else:
                out.write(b"\x00")
            n += 1
    else:
        i = 0
        while i < len(data):
            bits = data[i]
            fr = data[i + 1:i + 1 + (bits + 7) // 8]
            i += 1 + (bits + 7) // 8
            fields = b"\x01" + bytes([bits]) + fr
            a = again(p.ask(1, fields, 2), 1, fields, 2) if bits else None
            if a and a[1][:1] == b"\x00" and len(a[1]) >= 2 + 2 * a[1][1]:
                c = a[1][1]
                out.write(struct.pack("<%dh" % c, *struct.unpack(">%dh" % c, a[1][2:2 + 2 * c])))
            else:
                out.write(b"\0" * 320)
            n += 1
    out.close()
    sys.stderr.write(f"{cmd} {rate}: {n} frames, {p.missed} unanswered, {p.restarts} chip restarts\n")


if __name__ == "__main__":
    main()
