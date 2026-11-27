#!/usr/bin/env python3
"""IF shift on the X6100 (MFK 2:4; the remote's "set ifs"), through the cable: SENDER sends 1000 Hz over its TCI at
0.1 W (the path proved first in USB-D), LISTENER (the X6100) hears it with IF shift +2000, -2000 and 0, 3.4 s each (the
tone 2.5 s on before the first: its sender starts over SSH), in USB and then in USB-D, both radios alike in each. Its base is tuned the shift the other way and its audio stays as it was (the manual's "IF shift"), so:
  - the tone's pitch in LISTENER's TCI audio the same at every shift (within 20 Hz);
  - the tone where it is (F + 1000 Hz, within 250 Hz) in the browser's picture at every shift: its frames are placed
    where the base is tuned (up to 2026-11-23 on the dial: the tone drawn the shift off);
  - in those frames' own bins the tone moves by the shift (within 300 Hz): the base did retune.
Both radios put back and left on page A whatever happens.

    ifshift.py SENDER LISTENER [--f HZ]

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import math
import sys
import threading
import time

import numpy as np

import bench
from tones import TciRx

SHIFTS = (2000, -2000, 0)      # the last, 0, is the reference
STEP_S = 3.4


def measure(c, total, tx, rx, tci, F, mode, name):
    """One pass in mode (2 USB, 3 USB-D, both radios alike): the sender's 1000 Hz on while the listener's IF shift
    steps through SHIFTS; each step's pitch in the listener's TCI audio and the tone's place in its frames"""
    marks, rows = [], []

    def stepper():
        time.sleep(2.5)
        for s_ in SHIFTS:
            rx.send(f"set ifs {s_}")
            marks.append((time.time(), s_))
            time.sleep(STEP_S)
    watch = bench.Watch([tx, rx], c, expect_dbm=10 * math.log10(bench.FIRST_PROBE_W * 1000) - total, listener=rx,
                        w=bench.FIRST_PROBE_W, label=f"IF shift {tx.name} to {rx.name} {name}", pair_ignore=("ifs",))
    try:
        bench.arm(tx, True, mode=mode)
        bench.tx_log({"who": tx.name, "f": F, "w": tx.pwr(), "what": f"IF shift test {name}"})
        th = threading.Thread(target=stepper)
        th.start()
        bench.tci_send(tx, [[0, 0.3], [1000, 2.5 + STEP_S * len(SHIFTS) - 0.4]], max_s=16, watch=watch)
        th.join()
    finally:
        watch.close()
        try:
            tx.wait("off the air", lambda: not tx.tx, 5)
        except bench.Refused:
            pass
        bench.arm(tx, False)
    if watch.why:
        print(f"STOP {watch.why}")
    for t, s_ in marks:
        x, _ = tci.audio(t + 1.0, t + STEP_S - 0.3)
        fr = [(cen, wid, np.array(v, float)) for tt, cen, wid, v in list(rx.r.spectra) if t + 1.0 <= tt <= t + STEP_S - 0.3]
        if len(x) < 4096 or not fr:
            rows.append((s_, None, None, None))
            continue
        n = len(x)
        X = np.abs(np.fft.rfft(x * np.hanning(n)))
        hz = np.fft.rfftfreq(n, 1 / 24000)
        band = (hz > 100) & (hz < 4000)
        pitch = float(hz[np.argmax(np.where(band, X, 0))])
        cen, wid = fr[-1][0], fr[-1][1]
        avg = np.mean([v for c_, w_, v in fr if c_ == cen and w_ == wid], axis=0)
        per = wid / len(avg)
        i0 = int((F + 1000 - (F - wid / 2)) / per)
        lo, hi = max(0, i0 - 60), min(len(avg), i0 + 60)
        k = lo + int(np.argmax(avg[lo:hi]))
        rows.append((s_, pitch, cen - wid / 2 + k * per, (k - len(avg) // 2) * per))
    good = [r_ for r_ in rows if r_[1] is not None]
    ref = [r_ for r_ in good if r_[0] == 0]
    p0 = ref[-1][1] if ref else None
    rel0 = ref[-1][3] if ref else None
    for s_, pitch, at, rel in rows:
        if pitch is None:
            print(f"  {name} ifs {s_:+5d}: no audio or no frames")
            continue
        print(f"  {name} ifs {s_:+5d}: pitch {pitch:.1f} Hz; the picture puts it at {at:.0f} Hz (it is at {F + 1000}); "
              f"in the frame's own bins {rel - rel0:+.0f} Hz from the shift-0 place")
    ok = (not watch.why and len(good) == len(SHIFTS) and p0 is not None
          and all(abs(pitch - p0) <= 20 for _, pitch, _, _ in good)
          and all(abs(at - (F + 1000)) <= 250 for _, _, at, _ in good)
          and all(abs((rel - rel0) - s_) <= 300 for s_, _, _, rel in good)
          and int(rx.state.get("freq", 0)) == F)
    print(f"  {name}: {'right' if ok else 'WRONG'}")
    return {"ok": ok, "rows": rows}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sender")
    ap.add_argument("listener")
    ap.add_argument("--f", type=int, default=14200000)
    a = ap.parse_args()
    F = a.f
    c = bench.conf()
    p, total = bench.chain_gate(c, "radios")
    tx = bench.Radio(a.sender, c["radios"][a.sender], True)
    rx = bench.Radio(a.listener, c["radios"][a.listener], False)
    snaps, rep, ok = {}, {}, False
    for r in (tx, rx):
        r.take_control()
        snaps[r.name] = bench.snapshot(r)
    decl = None
    try:
        bench.listener_gate(rx)
        bench.open_page(rx, "G")
        bench.ensure(rx, *bench.rx_profile(F, 2, pre=0, att=0, rfg=100, agc=0, filt=(300, 2700)))
        rx.errs.clear()
        rx.send("set ifs 0")
        time.sleep(1.0)
        if any("ifs" in e for e in rx.errs):
            print(f"SKIP {rx.name} has no IF shift: {rx.errs[-1]}")
            return 0
        bench.path_proof(c, total, tx, rx, F, rep)
        tci = TciRx(rx.host)
        time.sleep(1.0)
        # the listener's IF shift is what this measures: declared in the one registry (bench.apart, the owner,
        # 2026-11-24: "NO deviation"), till it is back at 0 (2026-11-25: undeclared, the Watch stopped the over)
        decl = bench.apart({"ifs": bench.STEPPED}, f"if-shift: {rx.name}'s IF shift stepped through {SHIFTS} Hz, "
                                                   f"what the test measures")
        decl.__enter__()
        results = {}
        # USB as the manual has it (the path proof left both in USB-D: from 2026-11-24 to 11-27 this measured there,
        # and the tone left the X6100's audio at +-2000), then USB-D, where digital apps hear it
        for mode, name, filt in ((2, "USB", (300, 2700)), (3, "USB-D", (100, 3000))):
            bench.ensure(rx, *bench.rx_profile(F, mode, pre=0, att=0, rfg=100, agc=0, filt=filt))
            bench.pair_align(tx, rx, F, mode)
            bench.ensure(tx, *bench.tx_profile(F, mode, bench.FIRST_PROBE_W))
            results[name] = measure(c, total, tx, rx, tci, F, mode, name)
        tci.close()
        ok = all(r_["ok"] for r_ in results.values())
        print(("PASS" if ok else "FAIL") + f" if-shift {tx.name}->{rx.name}: the audio kept its pitch, the picture "
              f"placed the tone where it is, the base retuned by the shift: " +
              "; ".join(f"{n} {r_['ok']}" for n, r_ in results.items()))
    except bench.Refused as e:
        print(f"REFUSED {e}")
    finally:
        rx.send("set ifs 0")
        if decl is not None:
            end = time.time() + 5
            while rx.state.get("ifs") not in (0, None) and time.time() < end:
                time.sleep(0.1)
            decl.__exit__(None, None, None)
        bench.finish([tx, rx], snaps, report=rep)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
