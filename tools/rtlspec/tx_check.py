#!/usr/bin/env python3
"""Checks x6x00-rtlspec's transmitter with nothing on the air: its test device (-T FILE) writes what it would send.

On the radio (plain Python), beside the radio's own daemon:

    x6x00-rtlspec -F -S rtlspec-test -T /tmp/tx.iq 2>/tmp/tx_test.log &
    python3 tx_check.py drive rtlspec-test > /tmp/tx_drive.txt

It asks for transmissions the daemon must refuse (off the amateur bands, 0 Hz edges), one it must end by itself (no
voice: IDLE), and one of 3 s of a 1 kHz tone at full scale then 3 s of a 2.5 kHz one, ended by "t 0". The words the
daemon said each time are printed: RTX NO/ON/END. Then on a computer with numpy:

    python3 tx_check.py analyse tx.iq

It demodulates the FM the tone transmission made (2116800 S/s, signed 8-bit I/Q) and prints its peak deviation, the
tones' frequencies, the level ramp at each end and the power outside +-12.5 kHz.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import math
import socket
import struct
import sys
import time

RATE = 2116800


def drive(name):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.connect('\0' + name)
    s.settimeout(0.2)
    said = []

    def read(secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                m = s.recv(70000)
            except socket.timeout:
                continue
            if m.startswith(b'RTX'):
                said.append(m.decode())
                print('%.2f %s' % (time.time() - t0, m.decode()), flush=True)
            elif m.startswith(b'RTLSPEC'):
                print('%.2f %s' % (time.time() - t0, m.decode()), flush=True)

    def tone(hz, secs, amp=30000):
        n = int(44100 * secs)
        ph = 0.0
        sent = 0
        start = time.time()
        while sent < n:
            k = min(2205, n - sent)
            v = []
            for i in range(k):
                v.append(int(amp * math.sin(ph)))
                ph += 2 * math.pi * hz / 44100
            s.send(b'TAUD' + struct.pack('<I', k) + struct.pack('<%dh' % k, *v))
            sent += k
            ahead = sent / 44100 - (time.time() - start)
            if ahead > 0.1:
                read(ahead - 0.05)

    t0 = time.time()
    read(1.0)
    for hz in (156800000, 121500000, 162550000, 1090000000, 143999999, 148000001, 1575420000, 462562500):
        print('ask %d' % hz, flush=True)
        s.send(b't %d 10 0\n' % hz)
        read(0.5)
    print('ask 146520000 with no voice: it must end by itself (IDLE)', flush=True)
    s.send(b't 146520000 10 0\n')
    read(3.5)
    print('ask 146520000, 3 s of 1 kHz then 3 s of 2.5 kHz, then t 0', flush=True)
    s.send(b't 146520000 10 0\n')
    read(0.3)
    tone(1000, 3.0)
    tone(2500, 3.0)
    s.send(b't 0\n')
    read(1.5)
    print('done: %d RTX lines' % len(said))


def analyse(path):
    import numpy as np

    raw = np.fromfile(path, dtype=np.int8).astype(np.float32)
    iq = raw[0::2] + 1j * raw[1::2]
    n = len(iq)
    print('%d samples, %.2f s' % (n, n / RATE))
    mag = np.abs(iq)
    top = np.percentile(mag, 99)
    on = np.nonzero(mag > top * 0.5)[0]
    print('level: %.1f of 127 (99th percentile); peak %.1f' % (top, mag.max()))
    if len(on):
        first, last = on[0], on[-1]
        up = np.nonzero(mag[first:] > top * 0.95)[0]
        dn = np.nonzero(mag[:last + 1][::-1] > top * 0.95)[0]
        print('ramp up %.1f ms, ramp down %.1f ms' % (up[0] / RATE * 1e3 if len(up) else -1,
                                                      dn[0] / RATE * 1e3 if len(dn) else -1))
    # FM: the phase's step per sample, as Hz; decimated by 48 (back to 44.1 kHz) for the tones
    d = np.angle(iq[1:] * np.conj(iq[:-1])) * RATE / (2 * np.pi)
    d = d[mag[1:] > top * 0.9]
    f = d[: len(d) // 48 * 48].reshape(-1, 48).mean(axis=1)
    print('deviation: peak %.0f Hz, 99.9th percentile %.0f Hz' % (np.abs(f).max(), np.percentile(np.abs(f), 99.9)))
    for name, seg in (('first 3 s', f[: int(44100 * 2.5)]), ('last 3 s', f[int(44100 * 3.5): int(44100 * 5.5)])):
        if len(seg) < 4096:
            continue
        sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
        k = np.argmax(sp[10:]) + 10
        print('%s: the tone at %.0f Hz, its deviation %.0f Hz peak' % (name, k * 44100 / len(seg),
                                                                    np.percentile(np.abs(seg), 99.5)))
    # occupied bandwidth: power outside +-12.5 kHz of the carrier, over a second in the middle
    mid = iq[n // 2 - RATE // 2: n // 2 + RATE // 2]
    if len(mid) == RATE:
        ps = np.abs(np.fft.fftshift(np.fft.fft(mid * np.hanning(len(mid))))) ** 2
        fr = np.fft.fftshift(np.fft.fftfreq(len(mid), 1 / RATE))
        tot = ps.sum()
        for w in (8000, 12500, 25000, 100000):
            out = ps[np.abs(fr) > w].sum()
            print('power beyond +-%5.1f kHz: %.1f dB' % (w / 1e3, 10 * np.log10(out / tot + 1e-30)))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'drive':
        drive(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == 'analyse':
        analyse(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(1)
