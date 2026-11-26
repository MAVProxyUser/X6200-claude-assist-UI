"""Receiver measurements over the cable (orch_rx.py, 2026-11-26, the owner: "RX first"): what each radio's receiver
does to a known signal, measured, not read back. The other radio sends through the pads at 0.1 W (its TCI audio: a
file of tones made here and played by tci_tone_tx.py on it); the listener's filter, RIT, PRE/ATT, AGC, DNF and NR act
on it, and its own TCI audio, S-meter and spectrum are judged. Every test both ways (PAIRS).

How the signal is read. A tone's level is taken against a pilot tone sent with it all through, in the same stretch of
audio: the listener's AGC moves both alike, so their ratio is the receiver's own doing (its filter), and the sender's
audio response comes out by the ratio of a narrow filter's run to a wide one's. The steps of a file are found in the
listener's audio by the pilot's start and end (the audio's rate is not exactly 24 kHz: the steps between are mapped
from those two).

What the listener changes on its own is declared apart (bench.apart: the Watch and the sync guard compare RIT, NR,
PRE, ATT and the filter) and put back to the sender's (the over's own set-up) before the declaration ends; DNF and the
levels, which nothing compares, go through cfg_kept.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import contextlib
import math
import os
import re
import subprocess
import tempfile
import threading
import time

import numpy as np

import bench
from bench import Refused
import orch_cable as oc

H = None                    # the running orch module (orch.py sets it)
FS = 24000
F_RX = 14200000             # 20 m USB, the bench's voice frequency
USB = 2
CAP_DNF_AUTO = 1 << 2       # radio_types.h HW_CAP_DNF_AUTO
CAP_XIT = 1 << 23           # radio_types.h HW_CAP_XIT

# rx-filter
PILOT_HZ = 1500             # inside every filter the test sets
PILOT_AMP = 0.12            # the pilot and the swept tone alike: 0.24 at most together (the bench's 0.3 for one tone)
SWEEP_AMP = 0.12
SWEEP_HZ = [f for f in range(150, 3151, 100) if abs(f - PILOT_HZ) >= 150]   # 12.8 s an over: the Watch's 16 s
STEP_S = 0.4                # each swept tone
LEAD_S = 0.8                # the pilot alone first (the AGC settles; its start found)
TAIL_S = 0.8                # and alone after (its end found)
FILTERS = (("wide", (100, 3000)), ("mid", (500, 2300)), ("narrow", (1250, 1750)))   # wide: over()'s own, the ref
FLAT_DB = 3.0               # inside the passband (150 Hz in from each edge) within this of its median
STOP_DB = 15.0              # 300 Hz or more outside an edge at least this far under the passband: the filter acts.
                            # Measured 2026-11-26: the X6200 60-100 dB down, the factory X6100 40-60 on its high side
                            # but 21-27 on its low side, alike with both filters (its base's own): 20 sat on that
REF_SNR_DB = 25.0           # a step judged only where the wide run heard it this far over the noise
WIDE_FLAT = (300, 2800)     # and inside the wide filter's own flat part (100-3000 less 200 Hz each side: the X6100's
                            # wide run was 10 dB down at 150 Hz, where a ratio to it means nothing)
EDGE_HZ = 150               # the -6 dB points within this of the edges set
LEVEL_BW = 20.0             # a tone looked for this near its own frequency (the radios 7 Hz apart; the X6100's own
                            # steady tones at 781 Hz and its harmonics kept out)

# the listener's audio clipped: samples this near full scale, more than this share of them (a measurement on clipped
# audio means nothing: the X6100's came in at full scale with its codec's +24 dB boost, 2026-11-26)
CLIP_AT = 0.985
CLIP_SHARE = 0.001

# rx-level
LEVEL_SEG_S = 2.4           # each AGC setting held this long in one over
LEVEL_PEAK_MAX_DBFS = -1.0  # the strong tone's audio peaks under this with every AGC setting

# rit-xit
RIT_TONE_HZ = 1000
RIT_STEPS = (200, -200)
PITCH_HZ = 25               # the tone heard within this of where it should be (the radios ~7 Hz apart)

# rx-pre-att
PREATT_TONE_HZ = 1000
PREATT_SEG_S = 3.0          # each setting held this long: off, the switch, off in one over (10 s: the Watch's 16)
PREATT_MIN_DB = 6.0         # the spectrum moves at least this, the right way, for each switch
SMETER_SAME_DB = 3.0        # the S-meter moves as the spectrum less its figure (the GUI takes the figure off) within
                            # this; a figure off the radio's own gain by more is said (Settings > TX Cal > S-METER >
                            # Measure sets it), not failed

# rx-agc: the step made at the listener (its ATT), not by the sender: a 30 dB step in the X6100's audio moved its RF
# 8-10 dB (its power loop holds the power), the X6200's first 10 dB nothing (it saturates), 2026-11-26
AGC_BEFORE_S = 3.0          # the tone steady this long, then the listener's ATT on
AGC_AFTER_S = 5.0           # and this long after (8.5 s an over)
AGC_MIN_STEP_DB = 8.0       # ATT stepped the input this much at least, as the spectrum at the tone shows
AGC_HELD_DB = 6.0           # with the AGC on, the step left in the settled audio at most
AGC_SETTLE_DB = 3.0         # settled: within this of the last second's level
AGC_FAST_MAX_S = 1.5        # Fast settles within this
AGC_SLOW_RATIO = 1.5        # and Slow takes this many times as long at least
AGC_SLOW_UP_DB = 3.0        # Slow has taken up this much of the step by the window's end at least

# rx-dnf
DNF_HZ = 1000               # the tone the notch is put on
DNF_OTHER_HZ = 1700         # a tone it must leave
DNF_DEPTH_DB = 20.0         # the notched tone down this much against the other at least
DNF_KEEP_DB = 3.0           # the other within this of where it was against the noise
DNF_SEG_S = 3.5
DNF_PITCH_S = 3.0           # an over first to hear where the tone lands (the radios 7 Hz apart: the X6100's notch at
                            # 1000 Hz took a tone heard at 1007 down 18 dB, 2026-11-26): the notch put there

# rx-nr: the noise sent with the tone (the receiver's own sits 50-80 dB under the pads' signal: nothing for NR to
# take, 2026-11-26), so the ratio is the sender's audio's, whatever its power loop does with the level
NR_TONE_HZ = 1000
NR_TONE_AMP = 0.12
NR_NOISE_RMS = 0.025        # white noise 300-2800 Hz: the tone some 20 dB over it in a 50 Hz band
NR_LEVEL = 30              # NR's threshold: at 60 the X6200 took the tone down with the noise, 26 dB, the ratio no
                            # better; 10/25/40 gave it +3/+10/+13 dB and the X6100 +17-21 (2026-11-26)
NR_SEG_S = 4.0
NR_GAIN_DB = 3.0            # the tone over the noise better by this much at least with NR on

# rx-smeter
SM_STEPS_DB = (-20, -30, -40)   # the tone's level in each stretch (from the bench's 0.3): under the X6200's knee (its
                                # 0 and -10 dB came out alike, 2026-11-26)
SM_SEG_S = 3.0
SM_TRACK_DB = 3.0           # the S-meter's step within this of the spectrum's, or SM_TRACK_FRAC of it
SM_TRACK_FRAC = 0.3
SM_MIN_STEP_DB = 4.0        # a step judged only where the spectrum moved this much (the sender's power loop may not)


# ---- the signal ----

def _db(v):
    return 20 * math.log10(max(float(v), 1e-12))


def _tone(hz, secs, amp):
    t = np.arange(int(round(secs * FS))) / FS
    return amp * np.sin(2 * np.pi * hz * t)


def _faded(x, ms=8):
    n = min(int(FS * ms / 1000), len(x) // 2)
    if n:
        w = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))
        x = x.copy()
        x[:n] *= w
        x[-n:] *= w[::-1]
    return x


def _spectrum(x):
    """|X| of x, Hann windowed, scaled so a sine of amplitude a reads a; and the bins' frequencies"""
    w = np.hanning(len(x))
    return np.abs(np.fft.rfft(x * w)) * 2 / np.sum(w), np.fft.rfftfreq(len(x), 1 / FS)


def _level(x, hz, bw=30.0):
    """The level (dBFS) of the component at hz in x: its strongest bin within bw"""
    if len(x) < 1024:
        return None
    X, f = _spectrum(x)
    m = (f >= hz - bw) & (f <= hz + bw)
    return _db(X[m].max()) if m.any() else None


def _noise(x, lo, hi, avoid=()):
    """The median bin (dBFS, the same scale as _level for the same length) between lo and hi Hz, skipping 80 Hz round
    each tone of avoid"""
    X, f = _spectrum(x)
    m = (f >= lo) & (f <= hi)
    for a in avoid:
        m &= np.abs(f - a) > 80
    return _db(np.median(X[m])) if m.any() else None


def _pitch(x, hz, span=400.0):
    from tones import peak
    return peak(x, hz, span)


def _put_audio(S, tag, x):
    """x as the 16-bit 24 kHz file tci_tone_tx.py sends as it is, put on the sender: its path there"""
    path = f"/tmp/bench_rx_{tag}.raw"
    fd, local = tempfile.mkstemp(suffix=".raw")
    os.close(fd)
    try:
        (np.clip(x, -1.0, 1.0) * 32767).astype("<i2").tofile(local)
        subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:{path}"], check=True,
                       timeout=60)
    finally:
        os.unlink(local)
    return path


def _drop_audio(S, tag):
    subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}", f"rm -f /tmp/bench_rx_{tag}.raw"],
                   capture_output=True, timeout=30)


def _send_file(S, path, secs):
    def send(watch):
        _, t0, t1 = bench.tci_send(S, [], max_s=secs + 2.0, watch=watch, file=path)
        return t0, t1
    return send


def _send_plan(S, plan, amp=0.3):
    def send(watch):
        _, t0, t1 = bench.tci_send(S, plan, amp=amp, max_s=sum(s for _, s in plan) + 2.0, watch=watch)
        return t0, t1
    return send


# ---- the over ----

def _over(o, s, l, label, send, agc=2, during=None, apart=None, keep=(), sender_keep=(), f=F_RX, mode=USB):
    """One over from s while l listens, both set alike by over() (the bench's receive profile, AGC agc). during(S, L,
    marks, watch), when given, runs beside send once s is on the air: what it changes on l, with when, goes in marks
    [(what, epoch)]. apart (spec, reason) is declared for the over; keep: l's keys, sender_keep: s's, put back to the
    over's own set-up (the other radio's) before the declaration ends. Returns {"x": l's TCI audio of the over, "x0":
    its first sample's epoch, "t": send's (on, off), "marks": [...]}"""
    from tones import TciRx
    S, L = o.r[s], o.r[l]
    rx = TciRx(L.host)
    got = {"marks": []}

    def body(watch):
        if during is None:
            got["t"] = send(watch)
            return
        out = {}

        def run():
            try:
                out["t"] = send(watch)
            except BaseException as e:      # noqa: BLE001 - handed to the body's thread
                out["e"] = e
        th = threading.Thread(target=run, daemon=True)
        th.start()
        try:
            S.wait("on the air", lambda: S.tx, 6)
            during(S, L, got["marks"], watch)
        finally:
            th.join(90)
        if "e" in out:
            raise out["e"]
        got["t"] = out.get("t")

    try:
        with contextlib.ExitStack() as ex:
            if apart:
                ex.enter_context(bench.apart(*apart))
            try:
                H.over(o, s, l, f, mode, label, body, agc=agc)
            finally:
                if keep:
                    bench.ensure(L, {k: S.state.get(k) for k in keep if S.state.get(k) is not None})
                if sender_keep:
                    bench.ensure(S, {k: L.state.get(k) for k in sender_keep if L.state.get(k) is not None})
        time.sleep(0.6)
        x, x0 = rx.audio()
        got.update(x=x, x0=x0)
        if not len(x):
            raise Refused(f"{l}: no TCI audio of the over")
        _keep_audio(o, l, label, x)
        got["clip"] = float(np.mean(np.abs(x) >= CLIP_AT))
        return got
    finally:
        rx.close()


def _keep_audio(o, l, label, x):
    """l's audio of an over kept in the run's folder (24 kHz int16 .raw: the console plays it)"""
    try:
        name = re.sub(r"[^a-z0-9+-]+", "_", f"{label} {l}".lower()).strip("_")
        (np.clip(x, -1.0, 1.0) * 32767).astype("<i2").tofile(os.path.join(oc.run_dir(o), f"rx_{name}.raw"))
    except OSError:
        pass


def _unclipped(got, l, what):
    """Refused when l's audio of the over clipped (CLIP_SHARE of it at full scale): what was measured means nothing"""
    if got.get("clip", 0.0) > CLIP_SHARE:
        raise Refused(f"{l}'s audio clipped in {what} ({100 * got['clip']:.1f} % of it at full scale): its receive "
                      f"audio has no headroom, nothing measured on it holds (orch rx-level)")


def _span(got, t_a, t_b):
    """l's audio between two epochs (the over's own clock: frames as they came)"""
    x, x0 = got["x"], got["x0"]
    a, b = int((t_a - x0) * FS), int((t_b - x0) * FS)
    return x[max(a, 0):max(min(b, len(x)), 0)]


def _onoff(x, hz, block_s=0.05, under_db=12.0):
    """(first, last) sample of the stretch where the tone at hz stands within under_db of its strongest, in blocks"""
    n = int(block_s * FS)
    lv = np.array([_level(x[i:i + n], hz, 40.0) or -200.0 for i in range(0, len(x) - n, n)]) if len(x) > n else []
    if not len(lv):
        return None, None
    top = np.percentile(lv, 90)
    on = np.nonzero(lv >= top - under_db)[0]
    if not len(on):
        return None, None
    return int(on[0] * n), int((on[-1] + 1) * n)


def _segments(x, hz, sent_s, marks_s):
    """The audio of each stretch of a sent file (marks_s: [(start, end)] in the file's seconds), found by the tone at hz
    that runs the whole file: its start and end in x give the mapping (the audio's own rate)"""
    a, b = _onoff(x, hz)
    if a is None or b - a < FS:
        raise Refused(f"the {hz} Hz tone that runs through the file not found in the listener's audio")
    scale = (b - a) / (sent_s * FS)
    return [x[a + int(s0 * FS * scale):a + int(s1 * FS * scale)] for s0, s1 in marks_s], round(scale, 4)


def _wait_until(t):
    while time.time() < t:
        time.sleep(0.02)


def _held(L, marks, items, seg_s, watch):
    """items [(what, {key: value} or a callable)], each set on L in turn and held seg_s, marked when set"""
    for what, change in items:
        t = time.time()
        if callable(change):
            change()
        elif change:
            bench.ensure(L, change)
        marks.append((what, time.time()))
        if watch.why:
            raise Refused(watch.why)
        _wait_until(t + seg_s)


def _seg_after(marks, i, total_end, lead=0.8, tail=0.3):
    t0 = marks[i][1] + lead
    t1 = (marks[i + 1][1] if i + 1 < len(marks) else total_end) - tail
    return t0, t1


# ---- the tests ----

def _level_med(x, hz, parts=3, bw=LEVEL_BW):
    """The tone's level (dBFS) as the median of parts stretches of x: one disturbed stretch (a frame lost, a click)
    does not move it"""
    n = len(x) // parts
    v = [_level(x[i * n:(i + 1) * n], hz, bw) for i in range(parts)]
    v = [y for y in v if y is not None]
    return float(np.median(v)) if v else None


def _noise_near(x, hz, avoid=(), half=250.0):
    """The median bin (dBFS, as _level for the same length) within half of hz, the tone itself and avoid kept out"""
    X, f = _spectrum(x)
    m = (np.abs(f - hz) <= half) & (np.abs(f - hz) > 40)
    for a in avoid:
        m &= np.abs(f - a) > 60
    return _db(np.median(X[m])) if m.any() else None


def _filter_run(o, s, l, name, lo, hi, path, total):
    """One over of the sweep with l's filter lo-hi (wide: the over's own): each step's level against the pilot (the
    median of three stretches) and over the noise round it"""
    S, L = o.r[s], o.r[l]
    spec = None if name == "wide" else ({"filter": bench.STEPPED}, f"rx-filter: {l}'s filter {lo}-{hi} Hz while "
                                                                   f"{s} keeps the bench's 100-3000")

    def send(watch):
        if name != "wide":
            bench.ensure(L, {"filter": (lo, hi)})
        return _send_file(S, path, total)(watch)
    got = _over(o, s, l, f"rx-filter {name} {lo}-{hi}", send, apart=spec, keep=("filter",) if spec else ())
    _unclipped(got, l, f"the {name} filter's over")
    marks_s = [(LEAD_S + k * STEP_S + 0.08, LEAD_S + (k + 1) * STEP_S - 0.05) for k in range(len(SWEEP_HZ))]
    segs, scale = _segments(got["x"], PILOT_HZ, total, marks_s)
    rel, snr = [], []
    for hz, seg in zip(SWEEP_HZ, segs):
        p, t = _level_med(seg, PILOT_HZ), _level_med(seg, hz)
        n = _noise_near(seg, hz, avoid=(PILOT_HZ,))
        rel.append(None if p is None or t is None else round(t - p, 1))
        snr.append(None if t is None or n is None else round(t - n, 1))
    return {"rel": rel, "snr": snr, "scale": scale, "flo": L.state.get("flo"), "fhi": L.state.get("fhi")}


def _edge(freqs, resp, level, rising):
    """Where resp crosses level (linear between steps): the first crossing going up (rising) from the low side, or
    the last going down from the passband; None if it never does"""
    pts = [(f, r) for f, r in zip(freqs, resp) if r is not None]
    for (f0, r0), (f1, r1) in zip(pts, pts[1:]):
        if rising and r0 < level <= r1:
            return round(f0 + (level - r0) * (f1 - f0) / (r1 - r0))
        if not rising and r0 >= level > r1:
            return round(f0 + (r0 - level) * (f1 - f0) / (r0 - r1))
    return None


def t_rx_filter_measured(o, s, l):
    """The listener's SSB filter as set, measured: a pilot at PILOT_HZ with a tone stepped 150-3450 Hz beside it, sent
    three times, l's filter wide (over()'s 100-3000, the reference), 500-2300 and 1250-1750. Each step's level against
    the pilot, the narrow runs' against the wide one's (the sender's own audio response and the AGC out). A step is
    judged only where the wide run heard it REF_SNR_DB over the noise round it, inside the wide filter's flat part
    (WIDE_FLAT); a stopband step only where the wide run's margin could show STOP_DB (REF_SNR_DB + STOP_DB - 10 over the
    noise). PASS when, in each narrow run, the passband is flat within FLAT_DB (150 Hz in from the edges), every
    judged step 300 Hz or more outside an edge is STOP_DB down or lost in the noise (within 6 dB of it), and the -6 dB
    points are within EDGE_HZ of the edges. Each side's depth said"""
    S = o.r[s]
    res = {"hz": SWEEP_HZ}
    total = LEAD_S + STEP_S * len(SWEEP_HZ) + TAIL_S
    sig = _tone(PILOT_HZ, total, PILOT_AMP)
    for k, hz in enumerate(SWEEP_HZ):
        a = int((LEAD_S + k * STEP_S) * FS)
        sig[a:a + int(STEP_S * FS)] += _faded(_tone(hz, STEP_S, SWEEP_AMP), 5)
    sig = _faded(sig, 20)
    tag = f"filter_{s}"
    try:
        path = _put_audio(S, tag, sig)
        runs = {name: _filter_run(o, s, l, name, lo, hi, path, total) for name, (lo, hi) in FILTERS}
        res["runs"] = runs
        ref = runs["wide"]
        ok_ref = [r is not None and (rs or 0) >= REF_SNR_DB and WIDE_FLAT[0] <= hz <= WIDE_FLAT[1]
                  for hz, r, rs in zip(SWEEP_HZ, ref["rel"], ref["snr"])]
        res["ref_judged_hz"] = [hz for hz, k in zip(SWEEP_HZ, ok_ref) if k]
        bad, said = [], []
        for name, (lo, hi) in FILTERS[1:]:
            run = runs[name]
            if (run["flo"], run["fhi"]) != (lo, hi):
                bad.append(f"{name}: {l}'s filter read {run['flo']}-{run['fhi']}, not {lo}-{hi}")
                continue
            resp = [None if not k or a is None else round(a - b, 1)
                    for a, b, k in zip(run["rel"], ref["rel"], ok_ref)]
            inside = [r for hz, r in zip(SWEEP_HZ, resp) if lo + 150 <= hz <= hi - 150 and r is not None]
            med = float(np.median(inside)) if inside else 0.0
            norm = [None if r is None else round(r - med, 1) for r in resp]
            res[f"{name}_resp_db"] = norm
            flat = [(hz, r) for hz, r in zip(SWEEP_HZ, norm) if lo + 150 <= hz <= hi - 150 and r is not None
                    and abs(r) > FLAT_DB]
            stop, depth = [], {"low": [], "high": []}
            for hz, r, sn, rs in zip(SWEEP_HZ, norm, run["snr"], ref["snr"]):
                if r is None or not (hz <= lo - 300 or hz >= hi + 300):
                    continue
                if (rs or 0) < REF_SNR_DB + STOP_DB - 10:   # too little margin to show the depth
                    continue
                depth["low" if hz < lo else "high"].append(r)
                if r > -STOP_DB and (sn is None or sn > 6):  # within 6 dB of the noise: lost in it
                    stop.append((hz, r))
            e_lo, e_hi = _edge(SWEEP_HZ, norm, -6.0, True), _edge(SWEEP_HZ, norm, -6.0, False)
            res[f"{name}_edges"] = (e_lo, e_hi)
            res[f"{name}_depth_db"] = {k: (max(v) if v else None) for k, v in depth.items()}   # the least deep
            if flat:
                bad.append(f"{name} {lo}-{hi}: not flat inside (dB from its median): {flat}")
            if stop:
                bad.append(f"{name} {lo}-{hi}: under {STOP_DB:.0f} dB down 300 Hz or more outside: {stop}")
            if lo > 200 and (e_lo is None or abs(e_lo - lo) > EDGE_HZ):
                bad.append(f"{name}: the low -6 dB point {e_lo} Hz, set {lo}")
            if e_hi is None or abs(e_hi - hi) > EDGE_HZ:
                bad.append(f"{name}: the high -6 dB point {e_hi} Hz, set {hi}")
            d = res[f"{name}_depth_db"]
            said.append(f"{name} {lo}-{hi}: -6 dB at {e_lo}/{e_hi} Hz"
                        + (f", flat within {max(abs(r - med) for r in inside):.1f} dB" if inside else "")
                        + ", down outside (low/high) at least " + "/".join(
                            "none judged" if d[k] is None else f"{-d[k]:.0f} dB" for k in ("low", "high")))
        said.append(f"judged {len(res['ref_judged_hz'])} of {len(SWEEP_HZ)} steps (where the wide run was flat "
                    f"and clear)")
        if bad:
            return False, f"{l}'s filter: " + "; ".join(bad), res
        return True, f"{l}'s filters measured as set: " + "; ".join(said), res
    except Refused as e:
        return False, str(e), res
    finally:
        _drop_audio(S, tag)


def t_rit_xit(o, s, l):
    """RIT moves what the listener hears, XIT what the sender sends: s's tone at RIT_TONE_HZ in USB; l's RIT +200 and
    -200 put the pitch at 1000 - RIT (the receiver tuned up hears it lower), s's XIT +200 and -200 at 1000 + XIT.
    Each heard within PITCH_HZ of that, against the over with neither (the two radios' own offset taken out)"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        plan = [[RIT_TONE_HZ, 4.0]]

        def pitch_of(got):
            t0, t1 = got["t"]
            seg = _span(got, t0 + 1.5, t1 - 0.5)
            f, lv = _pitch(seg, RIT_TONE_HZ, 450)
            return None if f is None else round(f, 1)
        base = pitch_of(_over(o, s, l, "rit-xit none", _send_plan(S, plan)))
        res["none_hz"] = base
        if base is None:
            return False, f"{l} heard no tone near {RIT_TONE_HZ} Hz with RIT and XIT off", res
        bad = []
        for rit in RIT_STEPS:
            def send(watch, rit=rit):
                bench.ensure(L, {"rit": rit})
                return _send_plan(S, plan)(watch)
            got = _over(o, s, l, f"rit {rit:+d}", send, apart=({"rit": (rit, 0)}, f"rit-xit: {l}'s RIT {rit:+d} Hz"),
                        keep=("rit",))
            p = pitch_of(got)
            want = base - rit
            res[f"rit_{rit:+d}_hz"] = p
            if p is None or abs(p - want) > PITCH_HZ:
                bad.append(f"RIT {rit:+d}: heard at {p} Hz, {want:.0f} wanted")
        caps = oc.caps_of(S)
        res["sender_caps"] = caps
        if caps is not None and not caps & CAP_XIT:
            # the X6200's base keys the dial whatever XIT says (measured: +-200 heard at the same pitch, SSB and
            # CW): XIT greyed in its place, refused, none kept
            lack = oc.lacks_check(S, res, sets={"xit": 200}, buttons=("XIT",))
            if S.state.get("xit") not in (0, None):
                lack.append(f"its XIT reads {S.state.get('xit')}, not 0")
            if lack:
                bad.append(f"{s}'s base has no XIT, yet " + "; ".join(lack))
            xit_said = f"{s}'s base has no XIT: greyed, refused ({'; '.join(v for v in res['refused'].values() if v)})"
        else:
            for xit in RIT_STEPS:
                def send(watch, xit=xit):
                    bench.ensure(S, {"xit": xit})
                    return _send_plan(S, plan)(watch)
                got = _over(o, s, l, f"xit {xit:+d}", send, sender_keep=("xit",))
                p = pitch_of(got)
                want = base + xit
                res[f"xit_{xit:+d}_hz"] = p
                if p is None or abs(p - want) > PITCH_HZ:
                    bad.append(f"{s}'s XIT {xit:+d}: heard at {p} Hz, {want:.0f} wanted")
            xit_said = f"{s}'s XIT +200/-200 to {res.get('xit_+200_hz')}/{res.get('xit_-200_hz')} Hz"
            bench.ensure(S, {"xit": 0})
        if bad:
            return False, f"with neither the tone at {base} Hz; " + "; ".join(bad), res
        return True, f"the tone at {base} Hz with neither; {l}'s RIT +200/-200 moved it to {res['rit_+200_hz']}/" \
                     f"{res['rit_-200_hz']} Hz; " + xit_said, res
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            bench.ensure(S, {"xit": 0})
        except Refused:
            pass


def _stretches(L, got, hz, lead=0.8):
    """Each held stretch of an over (got's marks): the spectrum at hz (L's frames) and the S-meter's median"""
    marks = got["marks"]
    end = got["t"][1] if got.get("t") else marks[-1][1] + PREATT_SEG_S
    rows = []
    for i, (what, _) in enumerate(marks):
        t0, t1 = _seg_after(marks, i, end, lead=lead)
        sp, nfr = bench.spectrum_level(L, hz, t0, t1, half=150)
        sm = oc.meters(L, t0, t1)
        rows.append({"what": what, "spectrum_db": None if sp is None else round(sp, 1), "frames": nfr,
                     "smeter": round(float(np.median(sm)), 1) if sm else None})
    return rows


def t_rx_pre_att(o, s, l):
    """PRE and ATT act in the listener's base, and its S-meter takes their figures off: s's steady tone; one over with
    l's ATT off, on, off and one with PRE off, on, off (PREATT_SEG_S each: the Watch's 16 s an over). The spectrum at
    the tone moves the right way by PREATT_MIN_DB at least against the two stretches round it; the S-meter moves as
    the spectrum less the figure (smeter_pre / smeter_att, Settings > TX Cal > S-METER) within SMETER_SAME_DB. A
    figure that is not the radio's own gain (the S-meter then off by the difference) is said with what sets it"""
    S, L = o.r[s], o.r[l]
    res = {"figures": {}}
    for k in ("smeter_pre", "smeter_att"):
        try:
            res["figures"][k] = bench.cfg_get(L, k)
        except Refused:
            res["figures"][k] = None
    try:
        bad, said = [], []
        for key, sign in (("att", -1), ("pre", 1)):
            steps = [("off", {key: 0}), (key, {key: 1}), ("off again", {key: 0})]

            def during(S_, L_, marks, watch, steps=steps):
                _held(L, marks, steps, PREATT_SEG_S, watch)
            got = _over(o, s, l, f"rx-pre-att {key}", _send_plan(S, [[PREATT_TONE_HZ, PREATT_SEG_S * 3 + 0.8]]),
                        during=during, apart=({key: (1, 0)}, f"rx-pre-att: {l}'s {key.upper()} on for the middle "
                                                             f"stretch, {s}'s off"), keep=(key,))
            hz = int(L.state.get("freq") or F_RX) + PREATT_TONE_HZ
            rows = _stretches(L, got, hz)
            res[key] = rows
            if len(rows) < 3 or any(r["spectrum_db"] is None or r["smeter"] is None for r in rows):
                bad.append(f"{key.upper()}: no spectrum or S-meter for some stretch: {rows}")
                continue
            around_sp = (rows[0]["spectrum_db"] + rows[2]["spectrum_db"]) / 2
            around_sm = (rows[0]["smeter"] + rows[2]["smeter"]) / 2
            d_sp = round(rows[1]["spectrum_db"] - around_sp, 1)
            d_sm = round(rows[1]["smeter"] - around_sm, 1)
            res[f"{key}_spectrum_db"], res[f"{key}_smeter_db"] = d_sp, d_sm
            if sign * d_sp < PREATT_MIN_DB:
                bad.append(f"{key.upper()} moved the spectrum {d_sp:+.1f} dB ({'up' if sign > 0 else 'down'} "
                           f"{PREATT_MIN_DB:.0f} at least wanted)")
            fig = res["figures"].get(f"smeter_{key}")
            try:
                fig = float(fig)
            except (TypeError, ValueError):
                fig = None
            if fig is None:
                bad.append(f"no S-meter figure for {key.upper()} from {l}")
                continue
            want_sm = round(d_sp - sign * fig, 1)       # the spectrum's move less the figure the GUI takes off
            res[f"{key}_smeter_want_db"] = want_sm
            if abs(d_sm - want_sm) > SMETER_SAME_DB:
                bad.append(f"the S-meter moved {d_sm:+.1f} dB with {key.upper()}, the spectrum {d_sp:+.1f} less its "
                           f"figure {fig:.0f} wants {want_sm:+.1f}: the GUI does not take the figure off")
            said.append(f"{key.upper()} {d_sp:+.1f} dB in the spectrum, the S-meter {d_sm:+.1f} (its figure {fig:.0f}"
                        + (f": the radio's own {abs(d_sp):.0f} dB, the S-meter {abs(want_sm):.0f} dB off with it: "
                           f"Settings > TX Cal > S-METER > Measure)" if abs(want_sm) > SMETER_SAME_DB else ")"))
        if bad:
            return False, f"{l}: " + "; ".join(bad), res
        return True, f"{l}: " + "; ".join(said), res
    except Refused as e:
        return False, str(e), res


def _agc_run(o, s, l, agc, name):
    """One over: s's steady tone, l's ATT put in AGC_BEFORE_S into it (the step), AGC agc. Round the moment ATT went in
    (its mark: the audio's low point within a second of it, 50 ms blocks): the level before, the low point, the level
    at the window's end and the time from the low point until half the step is taken up (0 when the AGC took it inside
    one block: the X6100's Fast, 1.5 dB of 15 left in the first 100 ms, 2026-11-26). The input's own step from the
    spectrum at the tone (the S-meter takes ATT's figure off)"""
    S, L = o.r[s], o.r[l]
    secs = AGC_BEFORE_S + AGC_AFTER_S

    def during(S_, L_, marks, watch):
        t = time.time()
        marks.append(("before", t))
        _wait_until(t + AGC_BEFORE_S)
        bench.ensure(L, {"att": 1})
        marks.append(("att", time.time()))
        _wait_until(time.time() + AGC_AFTER_S - 0.3)
    got = _over(o, s, l, f"rx-agc {name}", _send_plan(S, [[RIT_TONE_HZ, secs + 0.5]]), agc=agc, during=during,
                apart=({"att": (1, 0)}, f"rx-agc: {l}'s ATT in mid-over, the step its AGC levels"), keep=("att",))
    _unclipped(got, l, f"AGC {name}'s over")
    t_att = got["marks"][1][1]
    hz = int(L.state.get("freq") or F_RX) + RIT_TONE_HZ
    sp_before, _ = bench.spectrum_level(L, hz, t_att - 2.0, t_att - 0.3, half=150)
    sp_after, _ = bench.spectrum_level(L, hz, t_att + 0.8, t_att + AGC_AFTER_S - 1.0, half=150)
    input_db = None if sp_before is None or sp_after is None else round(sp_before - sp_after, 1)
    blk = 0.05
    n = int(blk * FS)
    x, x0 = got["x"], got["x0"]

    def lv_at(t0, t1):
        a_, b_ = int((t0 - x0) * FS), int((t1 - x0) * FS)
        return [(_level(x[i:i + n], RIT_TONE_HZ, 40.0) or -200.0) for i in range(max(a_, 0), min(b_, len(x)) - n, n)]
    pre_v = lv_at(t_att - 2.0, t_att - 0.3)
    near = lv_at(t_att - 0.2, t_att + 1.2)
    if not pre_v or not near:
        raise Refused(f"AGC {name}: no audio round ATT's step")
    pre = float(np.median(pre_v))
    k_low = int(np.argmin(near))
    low = float(near[k_low])
    after = near[k_low:] + lv_at(t_att + 1.2, t_att + AGC_AFTER_S - 0.4)
    end = float(np.median(after[-int(1.0 / blk):]))
    step = pre - low
    half = next((k * blk for k, v in enumerate(after) if v >= low + step / 2), None) if step >= 3 else 0.0
    return {"input_step_db": input_db, "pre_dbfs": round(pre, 1), "low_dbfs": round(low, 1),
            "audio_step_db": round(step, 1), "end_dbfs": round(end, 1), "held_db": round(pre - end, 1),
            "taken_up_db": round(end - low, 1), "half_s": half, "curve": [round(v, 1) for v in after[:60]]}


def t_rx_agc(o, s, l):
    """The AGC levels a step down in the signal, Fast sooner than Slow: s's steady tone, l's ATT put in mid-over (a
    step of its figure at the receiver's input, whatever the sender's power loop does; the spectrum shows it
    AGC_MIN_STEP_DB at least), heard with AGC Slow and with Fast. Fast: back within AGC_HELD_DB of where it was, half
    the step taken up within AGC_FAST_MAX_S. Slow: AGC_SLOW_UP_DB taken up by the window's end at least (or the step
    taken inside one block) and half of it taken up AGC_SLOW_RATIO times as slowly as Fast at least"""
    res = {}
    try:
        runs = {name: _agc_run(o, s, l, agc, name) for agc, name in ((1, "slow"), (2, "fast"))}
        res.update(runs)
        bad = []
        for name, r in runs.items():
            if r["input_step_db"] is None or r["input_step_db"] < AGC_MIN_STEP_DB:
                bad.append(f"{name}: ATT stepped the input {r['input_step_db']} dB in the spectrum "
                           f"({AGC_MIN_STEP_DB:.0f} at least)")
            if r["half_s"] is None:
                bad.append(f"{name}: never took up half the {r['audio_step_db']} dB step")
        f, sl = runs["fast"], runs["slow"]
        if f["held_db"] > AGC_HELD_DB:
            bad.append(f"fast: {f['held_db']} dB of the step left at the window's end ({AGC_HELD_DB:.0f} at most)")
        if f["half_s"] is not None and f["half_s"] > AGC_FAST_MAX_S:
            bad.append(f"fast: half the step taken up in {f['half_s']:.2f} s ({AGC_FAST_MAX_S} s at most)")
        if sl["audio_step_db"] >= 3 and sl["taken_up_db"] < AGC_SLOW_UP_DB:
            bad.append(f"slow: took up {sl['taken_up_db']} dB of its {sl['audio_step_db']} dB step "
                       f"({AGC_SLOW_UP_DB:.0f} at least)")
        if f["half_s"] is not None and sl["half_s"] is not None and \
                sl["half_s"] < AGC_SLOW_RATIO * max(f["half_s"], blk_s()):
            bad.append(f"Slow took up half its step in {sl['half_s']:.2f} s, Fast in {f['half_s']:.2f}: Slow not slower")
        if bad:
            return False, f"{l}'s AGC: " + "; ".join(bad), res
        return True, f"{l}'s AGC: ATT stepped the input {f['input_step_db']} dB; Slow left {sl['audio_step_db']} dB " \
                     f"in the audio and took half up in {sl['half_s']:.2f} s ({sl['taken_up_db']} dB by the end), " \
                     f"Fast left {f['audio_step_db']} dB and took half up in {f['half_s']:.2f} s ({f['held_db']} dB " \
                     f"left)", res
    except Refused as e:
        return False, str(e), res


def blk_s():
    return 0.05


def t_rx_dnf(o, s, l):
    """DNF (manual: its centre and width) notches the tone at its centre and leaves one beside it: s sends DNF_HZ and
    DNF_OTHER_HZ together. An over first hears where the DNF_HZ tone lands in l's audio (the radios a few Hz apart)
    and the notch is centred there (width 50); then one over with DNF off and on. The notched tone falls DNF_DEPTH_DB
    at least against the other, the other within DNF_KEEP_DB of where it was against the noise. Where the base has
    the auto notch (radio_caps HW_CAP_DNF_AUTO) a third stretch with it on, the centre elsewhere: the two steady
    tones' ratio moved as far; where it has none, the remote refuses dnf_auto"""
    S, L = o.r[s], o.r[l]
    res = {}
    caps = oc.caps_of(L)
    res["caps"] = caps
    auto = bool(caps and caps & CAP_DNF_AUTO)
    total = DNF_SEG_S * (3 if auto else 2) + 1.0
    sig = _faded(_tone(DNF_HZ, total, 0.12) + _tone(DNF_OTHER_HZ, total, 0.12), 20)
    tag = f"dnf_{s}"
    try:
        if not auto:
            ref = oc.refusal(L, "cfg dnf_auto 1")
            res["auto_refused"] = ref
            if not ref:                     # taken where it should not be: off again
                bench.cfg_set(L, "dnf_auto", 0)
        path = _put_audio(S, tag, sig)
        first = _over(o, s, l, "rx-dnf pitch", _send_file(S, path, DNF_PITCH_S + 0.5))
        t0, t1 = first["t"]
        f_heard, _ = _pitch(_span(first, t0 + 1.0, t1 - 0.3), DNF_HZ, 60)
        if f_heard is None:
            return False, f"{l} heard no tone near {DNF_HZ} Hz", res
        centre = int(round(f_heard))
        res["heard_hz"] = round(f_heard, 1)
        steps = [("off", lambda: bench.cfg_set(L, "dnf", 0)), ("on", lambda: bench.cfg_set(L, "dnf", 1))]
        if auto:
            steps.append(("auto", lambda: (bench.cfg_set(L, "dnf_center", 2500), bench.cfg_set(L, "dnf_auto", 1))))
        kept = {"dnf": 0, "dnf_center": centre, "dnf_width": 50}
        if auto:
            kept["dnf_auto"] = 0
        with oc.cfg_kept(L, kept):
            def during(S_, L_, marks, watch):
                _held(L, marks, steps, DNF_SEG_S, watch)
            got = _over(o, s, l, "rx-dnf", _send_file(S, path, total), during=during)
        _unclipped(got, l, "the DNF over")
        marks = got["marks"]
        end = got["t"][1] if got.get("t") else marks[-1][1] + DNF_SEG_S
        rows = {}
        for i, (what, _) in enumerate(marks):
            seg = _span(got, *_seg_after(marks, i, end, lead=1.2))
            a, b = _level(seg, f_heard, 20.0), _level(seg, DNF_OTHER_HZ + (f_heard - DNF_HZ), 20.0)
            n = _noise(seg, 300, 2800, avoid=(f_heard, DNF_OTHER_HZ))
            rows[what] = {"notched_db": None if a is None or b is None else round(a - b, 1),
                          "other_snr_db": None if b is None or n is None else round(b - n, 1)}
        res["rows"] = rows
        bad = []
        off, on = rows.get("off", {}), rows.get("on", {})
        if None in (off.get("notched_db"), on.get("notched_db")):
            return False, f"the tones not found: {rows}", res
        depth = round(off["notched_db"] - on["notched_db"], 1)
        keep = round(on["other_snr_db"] - off["other_snr_db"], 1) if None not in (on["other_snr_db"],
                                                                                   off["other_snr_db"]) else None
        res.update(depth_db=depth, other_moved_db=keep)
        if depth < DNF_DEPTH_DB:
            bad.append(f"DNF centred on the tone ({centre} Hz) took it down {depth} dB against the other "
                       f"({DNF_DEPTH_DB:.0f} wanted)")
        if keep is not None and keep < -DNF_KEEP_DB:
            bad.append(f"the tone at {DNF_OTHER_HZ} Hz fell {keep} dB against the noise")
        if auto:
            au = rows.get("auto", {})
            d_auto = None if au.get("notched_db") is None else round(abs(au["notched_db"] - off["notched_db"]), 1)
            res["auto_depth_db"] = d_auto
            if d_auto is None or d_auto < DNF_DEPTH_DB:
                bad.append(f"the auto notch moved the two steady tones' ratio {d_auto} dB ({DNF_DEPTH_DB:.0f} wanted)")
        elif not res.get("auto_refused"):
            bad.append(f"{l}'s base has no auto notch, yet 'cfg dnf_auto 1' was not refused")
        if bad:
            return False, f"{l}: " + "; ".join(bad), res
        return True, f"{l}: DNF centred on the tone ({centre} Hz) notched it {depth} dB, the tone at " \
                     f"{DNF_OTHER_HZ} Hz moved {keep} dB against the noise" + (
                         f"; the auto notch {res['auto_depth_db']} dB" if auto else
                         f"; no auto notch on its base, refused: {res['auto_refused']}"), res
    except Refused as e:
        return False, str(e), res
    finally:
        _drop_audio(S, tag)


def _noise_300_2800(secs, rms, seed=7):
    """White noise band-limited to 300-2800 Hz with the given rms (the same each run: seeded)"""
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(int(secs * FS))
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / FS)
    X[(f < 300) | (f > 2800)] = 0
    y = np.fft.irfft(X, len(x))
    return y * (rms / (np.sqrt(np.mean(y ** 2)) + 1e-12))


def t_rx_nr(o, s, l):
    """NR lowers the noise round a tone: s sends NR_TONE_HZ with white noise 300-2800 Hz under it (NR_NOISE_RMS: the
    ratio is the sender's audio's, whatever its power loop makes of the level), l with NR off, on at level NR_LEVEL,
    off again in one over. The tone over the noise near it (the median bin within 250 Hz) better by NR_GAIN_DB at
    least with NR on than with it off on both sides"""
    S, L = o.r[s], o.r[l]
    res = {}
    total = NR_SEG_S * 3 + 0.8
    sig = _faded(_tone(NR_TONE_HZ, total, NR_TONE_AMP) + _noise_300_2800(total, NR_NOISE_RMS), 20)
    tag = f"nr_{s}"
    steps = [("off", {"nr": 0}), ("on", {"nr": 1}), ("off again", {"nr": 0})]
    try:
        path = _put_audio(S, tag, sig)
        with oc.cfg_kept(L, {"nr_level": NR_LEVEL}):
            def during(S_, L_, marks, watch):
                _held(L, marks, steps, NR_SEG_S, watch)
            got = _over(o, s, l, "rx-nr", _send_file(S, path, total), agc=1, during=during,
                        apart=({"nr": (1, 0)}, f"rx-nr: {l}'s NR on for the middle stretch"), keep=("nr",))
        _unclipped(got, l, "the NR over")
        marks = got["marks"]
        end = got["t"][1] if got.get("t") else marks[-1][1] + NR_SEG_S
        snr = {}
        for i, (what, _) in enumerate(marks):
            seg = _span(got, *_seg_after(marks, i, end, lead=1.5))
            t = _level_med(seg, NR_TONE_HZ, bw=25.0)
            n = _noise_near(seg, NR_TONE_HZ)
            snr[what] = None if t is None or n is None else round(t - n, 1)
        res["snr_db"] = snr
        if None in snr.values() or len(snr) < 3:
            return False, f"the tone not found in some stretch: {snr}", res
        gain = round(snr["on"] - max(snr["off"], snr["off again"]), 1)
        res["gain_db"] = gain
        if gain < NR_GAIN_DB:
            return False, f"{l}'s NR (level {NR_LEVEL}) left the tone {snr} dB over the noise near it (off, on, " \
                          f"off): {gain:+.1f} dB, {NR_GAIN_DB:.0f} wanted", res
        return True, f"{l}'s NR at level {NR_LEVEL}: the tone {snr['off']} dB over the noise near it off, " \
                     f"{snr['on']} on ({gain:+.1f} dB), {snr['off again']} off again", res
    except Refused as e:
        return False, str(e), res
    finally:
        _drop_audio(S, tag)


def t_rx_smeter(o, s, l):
    """The S-meter follows the signal as the receiver's spectrum does: s's tone in stretches SM_STEPS_DB (its audio
    stepped down 10 dB at a time, under the X6200's knee), l's S-meter and spectrum at the tone in each. Each step the
    spectrum shows SM_MIN_STEP_DB or more (the sender's power loop may hold some) is judged: the S-meter's step within
    SM_TRACK_DB, or SM_TRACK_FRAC of the step, of the spectrum's. At least one step judged"""
    S, L = o.r[s], o.r[l]
    res = {}
    total = SM_SEG_S * len(SM_STEPS_DB) + 0.3
    sig = np.concatenate([_tone(RIT_TONE_HZ, SM_SEG_S, 0.3 * 10 ** (d / 20)) for d in SM_STEPS_DB])
    sig = _faded(np.concatenate([sig, np.zeros(int(0.3 * FS))]), 10)
    tag = f"smeter_{s}"
    try:
        path = _put_audio(S, tag, sig)
        got = _over(o, s, l, "rx-smeter", _send_file(S, path, total), agc=2)
        if not got.get("t"):
            return False, "the over's times not kept", res
        t0 = got["t"][0]
        quiet = oc.meters(L, t0 - 3.0, t0 - 0.2)
        q = float(np.median(quiet)) if quiet else None
        with L.lock:
            ms = []
            for at, x in list(L.lines):
                if at >= t0 and x.startswith("m "):
                    try:
                        ms.append((at, float(x[2:])))
                    except ValueError:
                        pass
        on = next((at for at, v in ms if q is not None and v > q + 15), None)
        if on is None:
            return False, f"{l}'s S-meter never rose 15 dB over its quiet ({q})", res
        hz = int(L.state.get("freq") or F_RX) + RIT_TONE_HZ
        rows = []
        for k, d in enumerate(SM_STEPS_DB):
            a, b = on + k * SM_SEG_S + 0.8, on + (k + 1) * SM_SEG_S - 0.4
            sm = [v for at, v in ms if a <= at <= b]
            sp, _ = bench.spectrum_level(L, hz, a, b, half=150)
            rows.append({"sent_db": d, "smeter": round(float(np.median(sm)), 1) if sm else None,
                         "spectrum": None if sp is None else round(sp, 1)})
        res["rows"] = rows
        if any(r["smeter"] is None or r["spectrum"] is None for r in rows):
            return False, f"no S-meter or spectrum for some stretch: {rows}", res
        bad, said, judged = [], [], 0
        for r0, r1 in zip(rows, rows[1:]):
            d_sm, d_sp = r1["smeter"] - r0["smeter"], r1["spectrum"] - r0["spectrum"]
            said.append(f"{r0['sent_db']}->{r1['sent_db']} dB: S-meter {d_sm:+.1f}, spectrum {d_sp:+.1f}")
            if abs(d_sp) < SM_MIN_STEP_DB:
                said[-1] += " (not judged: the signal barely moved)"
                continue
            judged += 1
            if abs(d_sm - d_sp) > max(SM_TRACK_DB, SM_TRACK_FRAC * abs(d_sp)):
                bad.append(f"{r0['sent_db']}->{r1['sent_db']} dB sent: the S-meter moved {d_sm:+.1f} dB, the "
                           f"spectrum {d_sp:+.1f}")
        if not judged:
            return False, f"no step moved the signal {SM_MIN_STEP_DB:.0f} dB at {l}: " + "; ".join(said), res
        if bad:
            return False, f"{l}'s S-meter does not follow the signal: " + "; ".join(bad), res
        return True, f"{l}'s S-meter follows: " + "; ".join(said), res
    except Refused as e:
        return False, str(e), res
    finally:
        _drop_audio(S, tag)


def t_rx_level(o, s, l):
    """The listener's receive audio has headroom with every AGC setting: s's tone at the bench's level (S9 and more
    through the pads), l's AGC Off, Slow, Fast and Auto in one over (LEVEL_SEG_S each). In each the audio's peak under
    LEVEL_PEAK_MAX_DBFS and nothing at full scale. The X6100 on Xiegu's own V1.1.9 base came in at full scale with
    every setting, AGC Fast some 11 dB over it, while the GUI set its codec's MIC1 boost to +24 dB (audio.c); the
    boost the X6100 has now is read and said"""
    S, L = o.r[s], o.r[l]
    res = {}
    steps = [("off", {"agc": 0}), ("slow", {"agc": 1}), ("fast", {"agc": 2}), ("auto", {"agc": 3})]
    try:
        try:
            res["mic1_boost"] = subprocess.run(
                ["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{L.host}", "amixer sget 'Mic1 Boost' | tail -1"],
                capture_output=True, text=True, timeout=20).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            res["mic1_boost"] = None

        def during(S_, L_, marks, watch):
            _held(L, marks, steps, LEVEL_SEG_S, watch)
        got = _over(o, s, l, "rx-level", _send_plan(S, [[RIT_TONE_HZ, LEVEL_SEG_S * len(steps) + 0.6]]),
                    during=during, apart=({"agc": bench.STEPPED}, f"rx-level: {l}'s AGC stepped through its four"),
                    keep=("agc",))
        marks = got["marks"]
        end = got["t"][1] if got.get("t") else marks[-1][1] + LEVEL_SEG_S
        rows, bad = {}, []
        for i, (what, _) in enumerate(marks):
            seg = _span(got, *_seg_after(marks, i, end, lead=0.6, tail=0.2))
            if len(seg) < 2048:
                bad.append(f"{what}: no audio")
                continue
            pk = round(_db(np.max(np.abs(seg))), 1)
            clip = float(np.mean(np.abs(seg) >= CLIP_AT))
            tone = _level(seg, RIT_TONE_HZ, 40.0)
            rows[what] = {"tone_dbfs": None if tone is None else round(tone, 1), "peak_dbfs": pk,
                          "clipped_pct": round(100 * clip, 2)}
            if tone is None or tone < -60:
                bad.append(f"AGC {what}: the tone not heard ({tone})")
            elif pk > LEVEL_PEAK_MAX_DBFS or clip > CLIP_SHARE:
                bad.append(f"AGC {what}: peaks at {pk} dBFS, {100 * clip:.1f} % at full scale")
        res["rows"] = rows
        said = ", ".join(f"{k} {v['tone_dbfs']} ({v['peak_dbfs']} peak)" for k, v in rows.items())
        if bad:
            return False, f"{l}'s receive audio has no headroom (MIC1 boost {res['mic1_boost']}): " + "; ".join(bad) + \
                f"; the tone in dBFS: {said}", res
        return True, f"{l}'s receive audio with headroom, the tone in dBFS with AGC {said} (MIC1 boost " \
                     f"{res['mic1_boost']})", res
    except Refused as e:
        return False, str(e), res


# ---- what orch runs ----

COVERS = {
    "rx-level": (["path-rx-audio-headroom"], []),
    "rx-filter": (["path-rx-filter-passband"], []),
    "rit-xit": (["path-rx-rit-xit"], []),
    "rx-pre-att": (["path-rx-pre-att"], []),
    "rx-agc": (["path-rx-agc-speeds"], []),
    "rx-dnf": (["path-rx-dnf"], []),
    "rx-nr": (["path-rx-nr"], []),
    "rx-smeter": (["path-rx-smeter-steps"], []),
}

PAIRS = [
    # (name, fn, est s, "what")
    ("rx-level", t_rx_level, 35, "the listener's receive audio has headroom with AGC Off, Slow, Fast and Auto: a strong "
     "tone's peaks under -1 dBFS, nothing at full scale"),
    ("rx-filter", t_rx_filter_measured, 110, "the listener's SSB filter measured: a pilot and a swept tone through "
     "100-3000, 500-2300 and 1250-1750; flat inside, 20 dB down 300 Hz out, the -6 dB points at the edges set"),
    ("rit-xit", t_rit_xit, 120, "the listener's RIT +-200 Hz moves the tone it hears 200 Hz down/up; the sender's XIT "
     "+-200 Hz moves it up/down"),
    ("rx-pre-att", t_rx_pre_att, 70, "PRE and ATT move the listener's spectrum the right way; its S-meter reads the "
     "signal alike through both (their figures)"),
    ("rx-agc", t_rx_agc, 70, "the listener's ATT put in mid-over: the step levelled by AGC Slow and Fast, Fast "
     "settling sooner"),
    ("rx-dnf", t_rx_dnf, 70, "DNF centred where the tone lands notches it 20 dB against one beside it; the auto "
     "notch where the base has one, refused where not"),
    ("rx-nr", t_rx_nr, 45, "NR raises a tone over the noise sent with it by 3 dB at least"),
    ("rx-smeter", t_rx_smeter, 40, "the S-meter follows 10 dB steps of the signal as the spectrum does"),
]


def tests(Test, A, B):
    """orch's Test rows of this module: each both ways"""
    out = []

    def run_one(fn, s, l):
        return lambda o: fn(o, s, l)
    for name, fn, est, what in PAIRS:
        for s, l in ((A, B), (B, A)):
            out.append(Test(f"{name} {s}>{l}", f"{s.upper()} TX, {l.upper()} RX", what, run_one(fn, s, l), est))
    return out
