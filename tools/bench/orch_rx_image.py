"""The listener's own images and spurs of a strong carrier (2026-11-28): air sentry-lock's HOLD step found the X6100's
A scan sitting on 14.0903 MHz with the X6200's carrier on 14.030 or 14.036 and nothing there without it (Birdie Cal,
run on the X6100, did not find it). Here the listener's dial is held and the sender's carrier (a 1000 Hz tone in USB at
0.1 W, its dial 1000 Hz under) is stepped across the listener's spectrum frames (the X6100's are 100 kHz wide on its
dial at every zoom): each peak that rises over the frames from just before, away from the carrier and its skirt, is
listed with its rise and its level against the carrier, and set against the carrier mirrored about the frames' centre
(an I/Q image) and the carrier plus or less the frames' width (an alias). Then the listener on 14.0903 itself and on
dials up to 125 kHz over the carrier, the carrier out of its frames. A measurement: it passes when every carrier in
the frames was heard, and says what it found (TODO.md: none of it an image)

The listener's dial differs from the sender's throughout: declared to the sync guard and the Watch (bench.apart, the
frequency stepped), both otherwise alike; both on page G (the Sentry's AUTO lock would follow the carrier on A).

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import json
import math
import os
import subprocess
import time

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_rx_image.H = sys.modules[__name__])

# (the listener's dial, the carrier): carriers across the 100 kHz picture on 14.060 and one carrier under two more dials
# (images; the outer parts' rise, which follows the dial); the HOLD step's own set-ups; then the dial 64 to 125 kHz over
# one carrier, out of the frames, where its alias (the carrier plus the frames' width) falls in A's view.
# Runs 51-52 (2026-11-28): no carrier's mirror rose anywhere; a carrier lifts the frames' outer parts (beyond about 27
# kHz from the dial, where the X6100's base IQ climbs) 15-20 dB, 14.0903 among them from 14.060, following the dial;
# from 14.0903 the carrier 54 kHz under came in at 14.1360, 55 dB over the quiet (11 dB under its own level in frame)
POINTS = [
    (14060000, 14020000), (14060000, 14036000), (14060000, 14072000),
    (14050000, 14036000), (14080000, 14036000),
    (14026000, 14036000),                       # HOLD's start: A 10 kHz under its carrier
    (14090300, 14036000), (14090300, 14030000),  # where A sat, the carrier out of its frames
    (14100000, 14036000), (14111000, 14036000), (14120000, 14036000), (14136000, 14036000), (14150000, 14036000),
    (14161000, 14036000),
]
WATCHED_HZ = (14090300,)            # where the HOLD step's scan sat
WATCHED_OFF = (30300,)              # the same from run 51's dial (14.060): the outer parts' rise
VIEW_HZ = 25000                     # the X6100's A looks this far either side of its dial (app_sentry.c view_hz)
ALIAS_HZ = 400                      # a peak this near the carrier plus or less the frames' width: its alias
ALIKE = ("mode", "agc", "pre", "att", "rfg", "nb", "nr", "sql", "zoom")   # the sender's receiver set as the listener's
TONE_HZ = 1000                  # the sender's tone over its dial (USB): its carrier
ON_S = 5.0                      # each carrier
AMP = 0.3                       # the tone's amplitude (image.py's)
QUIET_S = 2.5                   # the listener's frames before each carrier: its quiet
RISE_DB = 10                    # a peak this far over the quiet: one the carrier made
STAND_DB = 6                    # ... and this far over its own surroundings (2 kHz either side): not the skirt's slope
SKIRT_HZ = 3000                 # the carrier's own skirt: not looked at
MIRROR_HZ = 400                 # a peak this near the carrier mirrored about the centre: its image
SHOWN = 8                       # peaks said per point, the strongest rise first


def _frames(L, t0, t1, cen):
    """The listener's spectrum frames between t0 and t1 on centre cen"""
    return [v for t, c, w, v in list(L.r.spectra) if t0 <= t <= t1 and v and c == cen]


def _wid(L, cen):
    w = [w for t, c, w, v in list(L.r.spectra) if c == cen and v]
    return w[-1] if w else None


def peaks(quiet, on, cen, wid, carrier):
    """quiet and on: lists of frames (dB per bin). The peaks the carrier made away from it: [{hz, rise_db, dbc,
    mirror, alias, view}], the carrier's rise (None outside the frames), the rises at WATCHED_HZ, at WATCHED_OFF from
    the centre and at the carrier's alias ({label: dB, None outside}), and the carrier's mirror"""
    import numpy as np
    q, a = np.median(np.array(quiet), axis=0), np.median(np.array(on), axis=0)
    n = len(a)
    per = wid / n
    f0 = cen - wid / 2
    hz = f0 + (np.arange(n) + 0.5) * per
    d = a - q
    ci = int((carrier - f0) / per)
    c_lvl = float(a[max(ci - 2, 0):ci + 3].max()) if 0 <= ci < n else None
    c_rise = float(d[max(ci - 2, 0):ci + 3].max()) if 0 <= ci < n else None
    mirror = 2 * cen - carrier
    aliases = [carrier + wid, carrier - wid]
    side = max(1, int(2000 / per))
    found, i = [], 0
    while i < n:
        if d[i] >= RISE_DB and abs(hz[i] - carrier) > SKIRT_HZ:
            j = i
            while j + 1 < n and d[j + 1] >= RISE_DB:
                j += 1
            k = i + int(np.argmax(a[i:j + 1]))
            around = np.concatenate([a[max(k - side, 0):max(k - 2, 0)], a[k + 3:k + side + 1]])
            stand = float(a[k] - np.median(around)) if len(around) else 0.0
            if stand >= STAND_DB:
                found.append({"hz": int(round(hz[k])), "rise_db": round(float(d[k]), 1),
                              "dbc": round(float(a[k]) - c_lvl, 1) if c_lvl is not None else None,
                              "mirror": bool(abs(hz[k] - mirror) <= MIRROR_HZ),
                              "alias": bool(any(abs(hz[k] - x) <= ALIAS_HZ for x in aliases)),
                              "view": bool(abs(hz[k] - cen) <= VIEW_HZ)})
            i = j + 1
        else:
            i += 1
    watched = {}
    spots = [(f"{w / 1e6:.4f}", w) for w in WATCHED_HZ] + [(f"dial{x:+d}", cen + x) for x in WATCHED_OFF]
    spots += [(f"alias {x / 1e6:.4f}", x) for x in aliases if abs(x - cen) < wid / 2]
    for label, at in spots:
        wi = int((at - f0) / per)
        watched[label] = round(float(d[max(wi - 3, 0):wi + 4].max()), 1) if 0 <= wi < n else None
    found.sort(key=lambda p: -p["rise_db"])
    return found, (round(c_rise, 1) if c_rise is not None else None), watched, mirror


def t_rx_image(o, sender, listener):
    S, L = o.r[sender], o.r[listener]
    W = H.W
    H.run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(H.HERE, "tci_tone_tx.py"),
              f"root@{S.host}:/tmp/tci_tone_tx.py"])
    rows = []
    plan = [[0, 0.3], [TONE_HZ, ON_S], [0, 0.3]]
    key_s = sum(x for _, x in plan) + 1.0
    tag = f"rx-image {sender}>{listener}:"
    decl = None
    try:
        with o.step(f"both on page G, {POINTS[0][0] / 1e6:.4f} USB, the receiver profile alike"):
            o.both_page("G")
            want, checks = bench.rx_profile(POINTS[0][0], 2, pre=0, att=0, rfg=100, agc=3, sql=0, filt=(100, 3000))
            o.both(dict(want, pwr=W, atu=0, xit=0, rit=0, split=0), checks)
        decl = bench.apart({"freq": bench.STEPPED}, f"{tag} {listener}'s dial held, {sender}'s carrier stepped across "
                                                    f"its spectrum frames (the frequency is what this test steps)")
        decl.__enter__()
        o.st.ev(f"declared: {listener}'s and {sender}'s dials apart for this test (stepped)", "conf")
        for d, c in POINTS:
            label = f"{listener} on {d / 1e6:.4f}, carrier {c / 1e6:.4f}"
            with o.step(f"{label}: {sender} on {(c - TONE_HZ) / 1e6:.4f} USB (declared apart)") as s:
                bench.ensure(L, {"freq": d})
                bench.ensure(S, *bench.tx_profile(c - TONE_HZ, 2, W))
                # a dial into another part of the band can load its own receiver settings (run 51: the X6200's AGC
                # 2 on 14.071, the X6100's 3, and the Watch stopped the over): the listener's again
                fix = {k: L.state.get(k) for k in ALIKE if L.state.get(k) is not None and S.state.get(k) != L.state.get(k)}
                if fix:
                    bench.ensure(S, fix)
                    o.st.ev(f"{sender} set as {listener} again after its move: {fix}", "conf")
                s.confirm(f"{sender} {S.state.get('freq')}, {listener} {L.state.get('freq')}")
            with o.step(f"{label}: {sender} sends its carrier {ON_S:.0f} s at {W} W, {listener}'s frames kept") as s:
                _, total = bench.chain_gate(o.c, "radios")
                bench.radio_tx_gate(S, o.c, W)
                bench.listener_gate(L)
                time.sleep(QUIET_S + 0.5)           # the listener's quiet frames on its dial
                cen = int(L.state.get("freq") or d)
                o.roles = {sender: "TX", listener: "RX"}
                watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W,
                                    label=f"rx-image {label}")
                t0 = time.time()
                try:
                    o.changing += 1
                    try:
                        bench.arm(S, True, mode=2)
                    finally:
                        o.changing -= 1
                    bench.tx_log({"who": sender, "f": c - TONE_HZ, "w": S.pwr(), "what": f"orch: rx-image {label}"})
                    t0 = time.time()
                    proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                             f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {AMP} {key_s:.1f} "
                                             f"50001 0.05"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    while proc.poll() is None and not watch.why:
                        time.sleep(0.05)
                    if proc.poll() is None:
                        bench.stop_bench_programs(S)
                    try:
                        S.wait("off the air", lambda: not S.tx, 5)
                    except Refused:
                        S.stop()
                finally:
                    watch.close()
                    o.changing += 1
                    try:
                        bench.arm(S, False)
                    finally:
                        o.changing -= 1
                    o.roles = {H.A: "RX", H.B: "RX"}
                if watch.why:
                    raise Refused(f"the watch stopped the over: {watch.why}")
                on, off = S.tx_last if S.tx_last and S.tx_last[0] >= t0 - 1 else (None, None)
                if on is None:
                    raise Refused(f"{sender} was not seen on the air")
                quiet = _frames(L, t0 - QUIET_S, t0 - 0.2, cen)
                live = _frames(L, on + 1.0, off - 0.3, cen)
                wid = _wid(L, cen)
                if len(quiet) < 3 or len(live) < 3 or not wid:
                    raise Refused(f"{listener}'s frames on {cen}: {len(quiet)} quiet, {len(live)} on the air")
                found, c_rise, watched, mirror = peaks(quiet, live, cen, wid, c)
                row = {"dial": d, "carrier": c, "centre": cen, "width": wid, "carrier_rise_db": c_rise,
                       "watched_rise_db": watched, "mirror_hz": mirror, "peaks": found[:SHOWN],
                       "frames": [len(quiet), len(live)]}
                rows.append(row)
                im = next((p for p in found if p["mirror"]), None)
                s.confirm(f"the carrier {'+' + str(c_rise) + ' dB' if c_rise is not None else 'outside its frames'}; "
                          f"its mirror {mirror / 1e6:.4f} " + (f"{im['rise_db']:+} dB, {im['dbc']} dBc" if im else
                                                              "not risen") +
                          "; watched " + ", ".join(f"{k} {'%+.0f dB' % v if v is not None else 'outside'}"
                                                   for k, v in watched.items()) +
                          "; peaks (MHz rise/dBc, A: its alias, V: in A's view): " +
                          (", ".join(f"{p['hz'] / 1e6:.4f} {p['rise_db']:+}/{p['dbc']}" +
                                     ("A" if p["alias"] else "") + ("V" if p["view"] else "")
                                     for p in found[:SHOWN]) or "none"))
    except Refused as e:
        return False, str(e), {"points": rows}
    finally:
        if decl is not None:
            decl.__exit__(None, None, None)
        bench.undeclare(tag)
        try:
            o.both({"freq": POINTS[0][0], "mode": 2})
        except Refused as e:
            o.st.ev(f"not alike after rx-image: {e}", "bad")
    heard = [r for r in rows if r["carrier_rise_db"] is not None]
    deaf = [r for r in heard if r["carrier_rise_db"] < bench.DEAF_DB]
    images = [(r, p) for r in rows for p in r["peaks"] if p["mirror"]]
    aliased = [(r, p) for r in rows for p in r["peaks"] if p["alias"]]
    outer = [v for r in rows for k, v in r["watched_rise_db"].items() if k.startswith("dial") and v is not None]
    says = (f"{len(rows)} carriers; images (mirrored about the frames' centre): " +
            (", ".join(f"carrier {r['carrier'] / 1e6:.4f} on {r['dial'] / 1e6:.4f}: {p['hz'] / 1e6:.4f} "
                       f"{p['dbc']} dBc" for r, p in images) or "none") +
            "; aliases (the carrier plus or less the frames' width): " +
            (", ".join(f"carrier {r['carrier'] / 1e6:.4f} on {r['dial'] / 1e6:.4f}: {p['hz'] / 1e6:.4f} "
                       f"{p['rise_db']:+} dB" + (" IN A'S VIEW" if p["view"] else "") for r, p in aliased) or "none") +
            (f"; the frames' outer parts ({WATCHED_OFF[0]} Hz over the dial) rose {min(outer):+.0f} to "
             f"{max(outer):+.0f} dB" if outer else ""))
    if deaf:
        return False, says + f"; the carrier not heard in {listener}'s frames at " + \
            ", ".join(f"{r['carrier'] / 1e6:.4f}" for r in deaf), {"points": rows}
    return True, says, {"points": rows}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("rx-image x6200>x6100", "X6200 TX, X6100 RX (dials apart, declared)", "what a 0.1 W carrier makes in the "
             "X6100's 100 kHz spectrum frames, the carrier stepped across them and then 54 to 125 kHz under its dial: "
             "images (none, 11-28), aliases, spurs, the frames' outer parts (air sentry-lock's HOLD scan sat on one)", lambda o: t_rx_image(o, B, A), 200),
    ]


COVERS = {
    "rx-image": ([], ["sentry-a-scan-stops-on-signal"]),
}
