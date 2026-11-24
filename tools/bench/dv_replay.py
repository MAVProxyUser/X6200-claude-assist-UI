#!/usr/bin/env python3
"""B's digital voice with no SDR plugged in: a recorded transmission (int8 I/Q, as the HackRF sends it) replayed into a
test SDR daemon on the radio (x6x00-rtlspec -S x6x00-dvtest -i, beside B's own daemon), which runs B's decoders on it:
its own DMR and dsd-fme's P25, D-STAR, YSF and NXDN, their voice through the USB-3000 that B's daemon holds
(127.0.0.1:2460). What the test daemon says of the call, and the audio it makes, are counted; the audio kept as a WAV.

    dv_replay.py HOST FILE.iq [--fs 2000000] [--offset 200000] [--at 441000000] [--secs 20] [--daemon PATH]
                 [--wav OUT.wav] [--rate 2116800]

FILE.iq is resampled to RATE, 2.1168 MS/s or 1.0584 (the daemon listens only at multiples of 264.6 kHz: the second
half the file to copy over) as unsigned 8-bit I/Q with its
signal OFFSET above centre. The test daemon is the radio's own /usr/bin/x6x00-rtlspec unless --daemon names a build to
copy over (it must leave the vocoder's serial port to B's daemon and reach the chip through its port: ambe.c's
"another daemon's" rule, DMR's frames too from 2026-11-18). Receive only: nothing goes on the air.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import os
import subprocess
import sys
import tempfile
import wave

CLIENT = r'''
import socket, struct, sys, time
name, hz, secs, out, off = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4], int(sys.argv[5])
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect("\0" + name)
s.settimeout(0.5)
s.send(b"f %d\n" % (hz - off)); s.send(b"l %d 1\n" % hz)
end = time.time() + secs; aud = 0; peak = 0; f = open(out, "wb"); calls = {}
while time.time() < end:
    try:
        m = s.recv(70000)
    except socket.timeout:
        continue
    if m[:4] == b"RDSD":
        l = struct.unpack("<I", m[4:8])[0]; t = m[8:8 + l].decode(errors="replace").split("|")
        if len(t) > 3 and t[1] and t[3]: calls[t[1] + " " + t[3]] = calls.get(t[1] + " " + t[3], 0) + 1
    elif m[:4] == b"RDMR":      # Hz|slot|cc|voice|lc|flco|src|dst|bs|vocoder
        l = struct.unpack("<I", m[4:8])[0]; t = m[8:8 + l].decode(errors="replace").split("|")
        if len(t) > 9 and t[3] == "1" and t[4] == "1":
            k = "DMR TS%s CC%s %s %s FROM %s (vocoder %s)" % (t[1], t[2], "TO" if t[5] == "3" else "TG", t[7], t[6], t[9])
            calls[k] = calls.get(k, 0) + 1
    elif m[:4] == b"RAUD":
        c = struct.unpack("<I", m[4:8])[0]; d = m[8:8 + 2 * c]; f.write(d); aud += 1
        v = struct.unpack("<%dh" % c, d); peak = max(peak, max(abs(x) for x in v) if v else 0)
f.close()
for k, n in sorted(calls.items(), key=lambda kv: -kv[1]): print("CALL", n, k)
print("AUDIO", aud, peak)
'''


def resample(src, dst, fs_in, fs_out, off):
    import numpy as np
    x = np.fromfile(src, dtype=np.int8).astype(np.float32)
    z = (x[0::2] + 1j * x[1::2]).astype(np.complex64)
    n = np.arange(len(z))
    z *= np.exp(-2j * np.pi * off * n / fs_in).astype(np.complex64)
    taps, fc = 255, 60e3 / fs_in
    k = np.arange(taps) - (taps - 1) / 2
    h = (2 * fc * np.sinc(2 * fc * k) * np.hamming(taps)).astype(np.float32)
    y = (np.convolve(z.real, h, "same") + 1j * np.convolve(z.imag, h, "same"))[::8].astype(np.complex64)
    fs_mid = fs_in / 8
    t_out = np.arange(int(len(y) / fs_mid * fs_out)) / fs_out
    with open(dst, "wb") as f:
        for s0 in range(0, len(t_out), int(fs_out)):
            t = t_out[s0:s0 + int(fs_out)]
            i = t * fs_mid
            v = (np.interp(i, np.arange(len(y)), y.real) + 1j * np.interp(i, np.arange(len(y)), y.imag))
            v *= np.exp(2j * np.pi * off * t)
            out = np.empty(2 * len(v), dtype=np.uint8)
            out[0::2] = np.clip(np.round(v.real + 127.5), 0, 255)
            out[1::2] = np.clip(np.round(v.imag + 127.5), 0, 255)
            f.write(out.tobytes())


def ssh(host, cmd, timeout=120):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", f"root@{host}", cmd], capture_output=True, text=True,
                          timeout=timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("host"); ap.add_argument("file")
    ap.add_argument("--fs", type=int, default=2000000); ap.add_argument("--offset", type=int, default=200000)
    ap.add_argument("--at", type=int, default=441000000); ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--daemon", default=None); ap.add_argument("--wav", default=None)
    ap.add_argument("--rate", type=int, default=2116800, choices=(1058400, 2116800))
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for the test daemon (its dsd-fme too)")
    a = ap.parse_args()
    fs_out = a.rate
    with tempfile.TemporaryDirectory() as td:
        u8 = os.path.join(td, "dv.u8")
        resample(a.file, u8, a.fs, fs_out, a.offset)
        cl = os.path.join(td, "dvclient.py")
        open(cl, "w").write(CLIENT)
        files = [u8, cl] + ([a.daemon] if a.daemon else [])
        r = subprocess.run(["scp", "-q"] + files + [f"root@{a.host}:/tmp/"], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"scp: {r.stderr.strip()}")
    daemon = "/tmp/" + os.path.basename(a.daemon) if a.daemon else "/usr/bin/x6x00-rtlspec"
    env = " ".join(e for e in a.env if "=" in e and " " not in e and ";" not in e)
    r = ssh(a.host, f"chmod 755 {daemon}; {env} RTLSPEC_DSD_PORT=23480 {daemon} -F -S x6x00-dvtest -i /tmp/dv.u8 -s {fs_out} "
                    f"-f {a.at - a.offset} 2> /tmp/dvtest.log & sleep 3; python3 /tmp/dvclient.py x6x00-dvtest {a.at} "
                    f"{a.secs} /tmp/dvaud.raw {a.offset}; kill $! 2>/dev/null; sleep 1; "
                    f"grep -c 'another daemon' /tmp/dvtest.log; rm -f /tmp/dv.u8 /tmp/dvclient.py"
                    + (f" {daemon}" if a.daemon else ""), timeout=a.secs + 60)
    print(r.stdout.strip())
    if a.wav:
        raw = os.path.join(tempfile.gettempdir(), "dvaud.raw")
        subprocess.run(["scp", "-q", f"root@{a.host}:/tmp/dvaud.raw", raw], check=True)
        with open(raw, "rb") as f, wave.open(a.wav, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(f.read())
        print("audio in", a.wav)
    ssh(a.host, "rm -f /tmp/dvaud.raw /tmp/dvtest.log")
    calls = [l for l in r.stdout.splitlines() if l.startswith("CALL")]
    aud = [l for l in r.stdout.splitlines() if l.startswith("AUDIO")]
    ok = calls and aud and int(aud[0].split()[1]) > 0 and int(aud[0].split()[2]) > 1000
    print("PASS: a call decoded and its voice heard" if ok else "FAIL: no call or no voice")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
