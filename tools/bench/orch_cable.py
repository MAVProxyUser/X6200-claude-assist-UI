#!/usr/bin/env python3
"""The cable's features that had no test yet (FACILITATE.md "What the bench runs alone", 2026-11-25: the owner, "write
ALL the tests first, then we can debug them via the runs"): orch.py's tests over the 80 dB cable at 0.1 W, one radio
sending, the other listening, written against features.json's how_to_confirm.

orch.py imports this module and sets H to itself (its over(), its gates, its FT8 and TCI helpers, A, B, W); TESTS()
gives the Test rows and COVERS the features each covers (orch's coverage map). Every test:
  - both radios on one frequency and mode, any difference declared (bench.apart) for its time;
  - 0.1 W (H.W) over the pads, one radio on the air at a time, the Watch on both, the roles told to the sync guard;
  - the settings it changes through the remote's "cfg NAME VALUE" (by their database names, read back with "cfg?")
    and put back as found; what the radio says in its message box read from the remote's "msg" lines;
  - a main-screen button by its name ("btn NAME"), an app's F keys only in that app's own window (guarded);
  - ends with every app closed and both on the Sentry's page A, on the last frequency.
A picture alone is no proof: a feature only a picture shows is LOOK (the run keeps the picture), never PASS.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import collections
import contextlib
import json
import math
import os
import re
import subprocess
import threading
import time

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_cable.H = sys.modules[__name__])
HERE = os.path.dirname(os.path.abspath(__file__))
F_CW = 14040000
F_USB = 14200000
F_FT8 = 14074000
F_JS8 = 14078000
F_PSK = 14070000
F_SSTV = 14230000
F_FAX = 14100000
CW_WPM_FAST = 28                # the key speed the tests set for their longer sends (put back after)


# ---- the radios: settings, messages, windows, keys ----

def ssh(host, cmd, timeout=40):
    return bench.ssh(host, cmd, timeout=timeout)


def msgs_since(r, t0):
    with r.lock:
        return [t for at, t in r.msgs if at >= t0]


def wait_msg(r, t0, pattern, secs=6.0):
    """The first message box text since t0 matching pattern (a regex, any case); Refused when none comes"""
    rx = re.compile(pattern, re.I)
    end = time.time() + secs
    while time.time() < end:
        for t in msgs_since(r, t0):
            if rx.search(t):
                return t
        time.sleep(0.1)
    raise Refused(f"{r.name}: no message like {pattern!r} within {secs:.0f} s (it said: {msgs_since(r, t0)[-4:]})")


@contextlib.contextmanager
def cfg_kept(r, values):
    """r's settings set (through the remote's cfg, each read back), every one put back as found after"""
    was = {}
    try:
        for name, v in values.items():
            old, now = bench.cfg_set(r, name, v)
            was[name] = old
            if str(now) != str(v) and not _same_num(now, v):
                raise Refused(f"{r.name}: setting {name} reads {now!r} after asking {v!r}")
        yield was
    finally:
        for name, old in was.items():
            try:
                bench.cfg_set(r, name, old)
            except Refused:
                pass


def _same_num(a, b):
    try:
        return abs(float(a) - float(b)) < 1e-6
    except (TypeError, ValueError):
        return False


# radio_types.h's HW_CAP_* the tests judge the GUI by (the remote's read-only radio_caps): what the base does itself
CAP_CESSB, CAP_COMP, CAP_CW_PEAK, CAP_AGC_SHAPE = 1 << 7, 1 << 8, 1 << 21, 1 << 22


def caps_of(r):
    """r's base as its GUI judges it (the remote's radio_caps), None from a GUI before the remote had it"""
    v = str(bench.cfg_get(r, "radio_caps")).strip()
    return int(v) if v.isdigit() else None


def patched_rev(base):
    """The patched X6100 base's revision ("V1.1.6 ...,r13": 13), 0 for Xiegu's own and GoldCorpHF's gcfix bases"""
    m = re.search(r",r(\d+)", base or "")
    return int(m.group(1)) if m else 0


def buttons_of(r, timeout=4.0):
    """r's main-screen pages as its GUI made them (the remote's "buttons?"): [[page, name, ...], ...]"""
    t0 = time.time()
    r.send("buttons?")
    while time.time() - t0 < timeout:
        with r.lock:
            got = [x for when, x in list(r.lines) if when >= t0 and x.startswith("buttons ")]
        if got:
            return [p.split("; ") for p in got[-1][8:].split("|") if p]
        time.sleep(0.05)
    raise Refused(f"{r.name}: no answer to 'buttons?' (a GUI from before it)")


def refusal(r, cmd, timeout=3.0):
    """cmd to r's remote: the 'err' it answered naming cmd's subject (its second word) within timeout, None: taken"""
    subject = cmd.split()[1]
    r.take_control()
    t0 = time.time()
    r.send(cmd)
    while time.time() - t0 < timeout:
        with r.lock:
            got = [x for when, x in list(r.lines) if when >= t0 and x.startswith("err ") and subject in x]
        if got:
            return got[0][4:]
        time.sleep(0.05)
    return None


def lacks_check(r, res, cfgs=None, sets=None, buttons=(), cmds=()):
    """What r's base lacks offered nowhere (the owner, 2026-11-26: every FAIL fixed; radio_caps): each button named in
    buttons greyed in its place on its main-screen page ("buttons?" marks it "~"; the owner, 2026-11-26: "instead of
    hiding it, grey it out, we want 1:1 unity"), never live; each 'cfg NAME VALUE' and 'set NAME VALUE' refused with
    the setting as found after, each of cmds refused. What it found in res (pages, refused); returns what was wrong,
    [] for nothing. A set that was taken is put back"""
    bad = []
    pages = buttons_of(r)
    res["pages"] = {p[0].strip("()"): "; ".join(p[1:]) for p in pages if p}
    names = [b for p in pages for b in p[1:]]
    live = sorted({b for b in names if b in buttons})
    gone = sorted(b for b in buttons if "~" + b not in names)
    if live:
        bad.append(f"its main screen offers {live} (live, not greyed)")
    if gone:
        bad.append(f"its main screen has no greyed {gone} in its place (the pages not alike on both radios)")
    res["refused"] = {}
    for kind, items in (("cfg", cfgs or {}), ("set", sets or {})):
        for name, v in items.items():
            was = bench.cfg_get(r, name) if kind == "cfg" else r.state.get(name)
            why = refusal(r, f"{kind} {name} {v}")
            time.sleep(0.6)
            now = bench.cfg_get(r, name) if kind == "cfg" else r.state.get(name)
            res["refused"][f"{kind} {name} {v}"] = why
            if not why:
                bad.append(f"'{kind} {name} {v}' taken")
            if str(now) != str(was):
                bad.append(f"{name} {was} -> {now}")
                try:
                    if kind == "cfg":
                        bench.cfg_set(r, name, was)
                    elif was is not None:
                        r.send(f"set {name} {was}")
                except Refused:
                    pass
    for cmd in cmds:
        why = refusal(r, cmd)
        res["refused"][cmd] = why
        if not why:
            bad.append(f"'{cmd}' taken")
            if cmd.startswith("btn "):
                r.send(cmd)                     # a toggle pressed: pressed again
    return bad


def win(r):
    return r.state.get("win") or ""


def guard(r, names, what):
    names = (names,) if isinstance(names, str) else tuple(names)
    if not any(win(r).lower().startswith(n.lower()) for n in names):
        raise Refused(f"{r.name}: {what} not sent: the screen shows {win(r) or 'no window'!r}, not {names}")


def press(r, key, names, hold=False, rest=0.5):
    """KEY pressed (held 1.5 s) while one of the windows names is on the screen, else nothing (Refused)"""
    guard(r, names, f"key {key}")
    r.take_control()                    # a TCI client keying took it (its "take control first")
    r.send(f"key {key} 1")
    time.sleep(1.5 if hold else 0.15)
    r.send(f"key {key} 0")
    time.sleep(rest)


def turn(r, knob, n, names, rest=0.5):
    guard(r, names, f"knob {knob} {n}")
    r.take_control()
    r.send(f"knob {knob} {n}")
    time.sleep(rest)


def btn(r, name, hold=False, rest=0.6):
    """A main-screen button by its name (the remote's "btn"), as its F key on its page"""
    n0 = len(r.errs)
    r.take_control()
    r.send(f"btn {name}" + (" hold" if hold else ""))
    time.sleep(rest)
    if len(r.errs) > n0 and "btn" in r.errs[-1]:
        raise Refused(f"{r.name}: {r.errs[-1]}")


def open_app(r, action, name, secs=10.0):
    """App ACTION ("open ACTION") on the screen as window name; from the Sentry's A"""
    r.take_control()
    if (r.state.get("win") or "") == "Sentry" and r.state.get("pg") == "G":
        bench.open_page(r, "A")
    r.send(f"open {action}")
    r.wait(f"the {name} window", lambda: win(r).lower().startswith(name.lower()), secs)
    time.sleep(1.0)


def home(o, r):
    """Every app closed, r on the Sentry's page A (on the frequency it is on)"""
    try:
        bench.close_apps(r)
    except Refused:
        pass
    try:
        bench.open_page(r, "A")
    except Refused as e:
        o.st.ev(f"{r.name}: not back on A: {e}", "bad")


def snap(o, label):
    """Both screens photographed now, before the test goes on (o.snap takes them in a thread: a test that moves on at
    once had its next screen in the picture), kept with the running test as o.snap keeps them"""
    try:
        paths = bench.screenshots_both(list(o.r.values()), label.replace(" ", "_")[:60])
    except Exception as e:  # noqa: BLE001
        o.st.ev(f"pictures not taken: {e}", "radio")
        return {}
    with o.st.lock:
        if o.st.cur is not None:
            o.st.tests[o.st.cur]["shots"].append({"t": time.time(), "label": label, "paths": paths})
    o.st.ev(f"pictures: {label}: {', '.join(os.path.basename(p) for p in paths.values() if p)}", "conf")
    return paths


def classic(r):
    """r on the classic radio screen (from the Sentry's F SYSTEM > CLASSIC), on the dial and mode it was on: the
    classic screen has its own (VFO A's), which leaving the Sentry gives back"""
    import suite_cover_b as cb
    if cb.on_classic(r):
        return
    # the VFO too: the X6200's classic screen came up on B, and the Watch stopped the over for it (vfo 0/1, 11-26)
    keep = {k: int(r.state[k]) for k in ("vfo", "freq", "mode", "agc", "att", "pre", "rfg") if r.state.get(k) is not None}
    with bench.transition():
        r.take_control()
        cb.to_classic(r, r.host)
        bench.ensure(r, keep)


def settled(r, key="freq", quiet=1.0, limit=6.0):
    end = time.time() + limit
    last, since = r.state.get(key), time.time()
    while time.time() < end:
        v = r.state.get(key)
        if v != last:
            last, since = v, time.time()
        elif time.time() - since >= quiet:
            break
        time.sleep(0.05)
    v = r.state.get(key)
    return int(v) if isinstance(v, (int, float)) else v


def cw_settled(L, secs=6.0, quiet=1.5):
    """The listener's CW decoder print once it stopped growing for quiet s"""
    t0, last, t_last = time.time(), None, time.time()
    while time.time() - t0 < secs:
        if L.cw != last:
            last, t_last = L.cw, time.time()
        elif time.time() - t_last > quiet:
            break
        time.sleep(0.1)
    return " ".join(L.cw.split()).upper()


def tx_spans(r, t0, t1=None):
    """[(on, off)] of r's overs since t0 (its tx history: bench.Radio.tx_hist), an over still on: (on, None)"""
    with r.lock:
        hist = list(r.tx_hist)
    spans, on = [], None
    for at, v in hist:
        if v and on is None:
            on = at
        elif not v and on is not None:
            if on >= t0 - 0.5 and (t1 is None or on <= t1):
                spans.append((on, at))
            on = None
    if on is not None and on >= t0 - 0.5 and (t1 is None or on <= t1):
        spans.append((on, None))
    return spans


def wait_quiet(r, quiet=2.5, limit=40.0, watch=None):
    """r off the air for quiet s on end (CW's word gaps read off the air for a moment within one send)"""
    end = time.time() + limit
    since = None
    while time.time() < end:
        if watch is not None and watch.why:
            raise Refused(watch.why)
        if r.tx:
            since = None
        elif since is None:
            since = time.time()
        elif time.time() - since >= quiet:
            return
        time.sleep(0.1)
    raise Refused(f"{r.name}: still keying after {limit:.0f} s")


def t_frames(r, t0, t1=None):
    """[(when, W, SWR, ALC)] of r's TX meters since t0"""
    out = []
    with r.lock:
        lines = [(at, x) for at, x in list(r.lines) if at >= t0 and (t1 is None or at <= t1) and x.startswith("t ")]
    for at, x in lines:
        try:
            v = [float(y) for y in x[2:].split()]
            out.append((at, *v[:3]))
        except ValueError:
            pass
    return out


def meters(r, t0, t1):
    with r.lock:
        got = [x for at, x in list(r.lines) if t0 <= at <= t1 and x.startswith("m ")]
    vals = []
    for x in got:
        try:
            vals.append(float(x[2:]))
        except ValueError:
            pass
    return vals


# ---- on the air ----

@contextlib.contextmanager
def session(o, label, senders, f=None, w=None):
    """The gates for a test whose radios transmit from an app or the classic screen (not the Sentry's armed A, which
    over() arms and locks with A/B: on the classic screen A/B is the VFO key): the chain, each sender's TX gate, the
    listener's supply, the Watch on both. Yields the Watch; after: everything stopped, the roles back to RX"""
    w = w or H.W
    p, total = bench.chain_gate(o.c, "radios")
    for n in senders:
        bench.radio_tx_gate(o.r[n], o.c, w)
    for n in o.r:
        bench.listener_gate(o.r[n])
    watch = bench.Watch(list(o.r.values()), o.c, expect_dbm=10 * math.log10(w * 1000) - total, w=w, label=label)
    try:
        bench.tx_log({"who": ",".join(senders), "f": f or int(o.r[senders[0]].state.get("freq", 0)), "w": w,
                      "what": f"orch: {label}"})
        yield watch
    finally:
        watch.close()
        for r in o.r.values():
            r.stop()
        o.roles = {H.A: "RX", H.B: "RX"}
        if watch.why:
            o.st.ev(f"{label}: the watch stopped it: {watch.why}", "bad")
    # the test's body ended without its own error, but the Watch stopped the over: the test fails with it (only said
    # until 2026-11-26: tx-panel x6100 and long-press-busy x6100 came back LOOK, their overs cut 0.2 s in)
    if watch.why:
        raise Refused(f"the watch stopped the over: {watch.why}")


def role(o, sender):
    o.roles = {n: ("TX" if n == sender else "RX") for n in o.r}


def tci_cw(o, S, text, watch, max_s=30):
    """text keyed by S's own keyer through its TCI (tci_cw_tx.py on S, a client from 127.x: never asked); (on, off)"""
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_cw_tx.py"),
                    f"root@{S.host}:/tmp/tci_cw_tx.py"], check=True)
    t0 = time.time()
    proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                             f"python3 /tmp/tci_cw_tx.py '{text}' {max_s}"], stdout=subprocess.PIPE, text=True)
    while proc.poll() is None:
        if watch is not None and watch.why:
            S.stop()
            proc.kill()
            raise Refused(watch.why)
        time.sleep(0.05)
    return t0, time.time()


def cw_over(o, s, l, text, label, f=F_CW, mode=4, listener_cfg=None, sender_cfg=None, check_power=True,
            listener_set=None, info=None):
    """One CW over: both on f in mode (AGC fast, as cw-text), the listener's and the sender's settings set for it
    (put back after), the sender keys text through its TCI at 0.1 W (over(): the Sentry's A armed); the listener's
    decoder print since the over began, and the over's (on, off). listener_set: the listener's state set after over()'s
    own set-up (which puts both on the bench's receive profile; the caller declares the difference, bench.apart);
    info (a dict): gets "heard", how far the over rose in the listener's spectrum (dB, the Watch's), and "wav", the
    listener's audio of the over kept in the run's folder (its TCI: what its decoder heard), for tools/cw_replay"""
    S, L = o.r[s], o.r[l]
    got = {}

    def body(watch):
        if listener_set:
            bench.ensure(L, listener_set)
        with o.step(f"{s} keys {text!r}") as st:
            L.cw = ""
            rec = None
            try:
                from tones import TciRx
                rec = TciRx(L.host)
            except (Refused, OSError) as e:     # the evidence only: the over goes on without it
                o.st.ev(f"{l}: its audio not recorded for the over ({e})", "info")
            try:
                t0 = time.time()
                got["t"] = tci_cw(o, S, text, watch)
                time.sleep(1.0)
                if rec is not None:
                    x, _ = rec.audio(t0, time.time())
                    name = re.sub(r"[^A-Za-z0-9]+", "_", f"cw_{label}_{s}_to_{l}").strip("_")
                    got["wav"] = H.save_wav(o, x, f"{name}_{time.strftime('%H%M%S')}.wav") if len(x) else None
            finally:
                if rec is not None:
                    rec.close()
            if info is not None:
                info["wav"] = got.get("wav")
            if info is not None:
                info["heard"] = (watch.heard_db.get(s) or [None])[-1]
            if check_power and not watch.tx_peak.get(s) and not (watch.rx_peak or -200) > H.RF_ALARM_DBM:
                raise Refused(f"{s}'s meter showed no power")
            st.confirm(f"keyed {got['t'][1] - got['t'][0]:.1f} s")

    with contextlib.ExitStack() as ex:
        if listener_cfg:
            ex.enter_context(cfg_kept(L, listener_cfg))
        if sender_cfg:
            ex.enter_context(cfg_kept(S, sender_cfg))
        H.over(o, s, l, f, mode, label, body, agc=2)
        got["text"] = cw_settled(L)
    for r in (S, L):
        r.take_control()                # the TCI client that keyed took it
    return got["text"], got["t"]


def offset_over(o, s, l, f_s, mode, label, body, pair_ignore=(), rf_hz=None):
    """An over from s on f_s with the two radios where the test put them, apart as the test declared (bench.apart):
    over() would put both on one frequency. Both on the Sentry's page G first (the caller), s armed there, locked after"""
    S, L = o.r[s], o.r[l]
    p, total = bench.chain_gate(o.c, "radios")
    with o.step(f"{s} may send on {f_s / 1e6:.6f} MHz, {l} on {int(L.state.get('freq', 0)) / 1e6:.6f}") as st:
        bench.ensure(S, *bench.tx_profile(f_s, mode, H.W))
        bench.radio_tx_gate(S, o.c, H.W, rf_hz=rf_hz)
        sup = bench.listener_gate(L)
        st.confirm(f"{l}'s supply {sup}" if sup else "")
    o.roles = {s: "TX", l: "RX"}
    watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(H.W * 1000) - total, listener=L, w=H.W, label=label,
                        pair_ignore=pair_ignore)
    try:
        o.changing += 1
        try:
            bench.arm(S, True, mode=mode)
        finally:
            o.changing -= 1
        bench.tx_log({"who": s, "f": f_s, "w": S.pwr(), "what": f"orch: {label}"})
        body(watch)
    finally:
        watch.close()
        try:
            S.wait("off the air", lambda: not S.tx, 5)
        except Refused:
            S.stop()
        try:
            bench.arm(S, False)
        except Refused as e:
            o.st.ev(f"{s}: TX not locked again: {e}", "bad")
        o.roles = {H.A: "RX", H.B: "RX"}
    if watch.why:
        raise Refused(f"the watch stopped the over: {watch.why}")
    return watch


def on_g_alike(o, f, mode, agc=2, **more):
    """Both on the Sentry's page G, on f in mode, the receive profile over() sets (ATT, PRE, RF gain, AGC, filter,
    zoom) alike on both: an offset test's start (the Watch compares them)"""
    with o.step(f"both on page G, {f / 1e6:.4f} MHz mode {mode}, alike"):
        o.both_page("G")
        want, checks = bench.rx_profile(f, mode, pre=0, att=0, rfg=100, agc=agc, sql=0,
                                        filt=(100, 3000) if mode in (0, 1, 2, 3) else None)
        o.both(dict(want, pwr=H.W, atu=0, xit=0, rit=0, split=0, zoom=8, **more), checks)
        o.pair_alike()


def ensure_alike(o, f, mode, **more):
    """Both on f in mode at the bench's power, and every receiver setting the Watch compares alike (Orch.pair_alike, as
    over() does): a test keying from the classic screen went on the air with an earlier test's AGC left on one radio,
    and the Watch stopped its over 0.2 s in (agc 2/0: tx-panel x6100, long-press-busy x6100, 2026-11-25)"""
    with o.step(f"both on {f / 1e6:.4f} MHz mode {mode}, alike"):
        o.both(dict({"freq": f, "mode": mode, "pwr": H.W}, **more))
    with o.step("every receiver setting the Watch compares alike on both"):
        o.pair_alike()


# ---- CW: the decoder's settings and keys ----

CW_WEAK_RFG = {"x6100": 55, "x6200": 15}   # cw-dec-snr: the listener's RF gain for its weak overs, heard about 20 dB
                          # up (2026-11-26: the X6100 at 15 heard it 13.9 dB up, at 30 18.8, at 40 21.8, where Dec SNR 5
                          # still copied garble; the X6200 at 15, 23-25 dB, copied clean)
CW_DEC_SPEED = {"key_speed": 20}    # and the sender's speed: at its own 28 WPM the X6200 copied nothing of the
                                    # X6100's 'TEST TEST' even at full gain (2026-11-26; cw-his found it too)
CW_WEAK_MAX_DB = 30       # a weak over heard at most this far over the quiet: Dec SNR 30 (with its 5 dB margin) must
                          # be able to refuse it
CW_WEAK_STEP = 15         # the RF gain stepped down by this until the weak over is that weak (t_cw_dec_snr)


def t_cw_dec_snr(o, s, l):
    """Dec SNR (CW 2:2): with it at 5 dB the listener copies a signal its RF gain has brought down; at 30 dB the same
    signal prints nothing of it; at 5 dB again it copies (the threshold, not the signal, made the difference). The weak
    overs' RF gain is set after over()'s own set-up (which put it back to 100 before 2026-11-26: the "weak" overs were
    heard ~50 dB up and no threshold could show), and each is heard at most CW_WEAK_MAX_DB up or the test fails"""
    L = o.r[l]
    res = {}
    weak_rfg = CW_WEAK_RFG.get(l, 15)
    try:
        text, _ = cw_over(o, s, l, "TEST TEST", "dec-snr 5 dB full", listener_cfg={"cw_decoder_snr_3": 5},
                          sender_cfg=CW_DEC_SPEED)
        res["snr5_full"] = text
        if "TEST" not in text:
            raise Refused(f"at Dec SNR 5 dB and full RF gain {l} printed {text!r}")
        rfg0 = int(L.state.get("rfg", 100))
        i30, i5 = {}, {}
        tried = []
        try:
            with bench.apart({"rfg": bench.STEPPED}, f"cw-dec-snr: {l}'s RF gain brings the signal down for the "
                                                      f"threshold to show"):
                # the RF gain that leaves it at most CW_WEAK_MAX_DB up, stepped down from the table's: the same 55
                # left the X6100 28.9 dB up once and 33-36 dB the next run (2026-11-26)
                while True:
                    i30 = {}
                    text30, _ = cw_over(o, s, l, "TEST TEST", f"dec-snr 30 dB weak rfg {weak_rfg}",
                                        listener_cfg={"cw_decoder_snr_3": 30}, listener_set={"rfg": weak_rfg},
                                        info=i30, sender_cfg=CW_DEC_SPEED)
                    tried.append((weak_rfg, i30.get("heard")))
                    if i30.get("heard") is None or i30["heard"] <= CW_WEAK_MAX_DB or weak_rfg - CW_WEAK_STEP < 5:
                        break
                    weak_rfg -= CW_WEAK_STEP
                text5, _ = cw_over(o, s, l, "TEST TEST", f"dec-snr 5 dB weak rfg {weak_rfg}",
                                   listener_cfg={"cw_decoder_snr_3": 5}, listener_set={"rfg": weak_rfg}, info=i5,
                                   sender_cfg=CW_DEC_SPEED)
        finally:
            bench.ensure(L, {"rfg": rfg0})
        res["rfg_tried"] = tried
        res.update(snr30_weak=text30, snr5_weak=text5, heard30_db=i30.get("heard"), heard5_db=i5.get("heard"),
                   wav30=i30.get("wav"), wav5=i5.get("wav"))
        weak = [h for h in (i30.get("heard"), i5.get("heard")) if h is not None]
        if not weak:
            return None, f"the Watch kept no level for the weak overs: {res}", res
        if max(weak) > CW_WEAK_MAX_DB:
            return False, f"RF gain down to {weak_rfg} did not bring the signal down enough to test the threshold: " \
                          f"heard {weak} dB up (at most {CW_WEAK_MAX_DB} wanted; tried {tried})", res
        if "TEST" in text30:
            return False, f"Dec SNR 30 still copied {text30!r} from a signal heard {i30.get('heard')} dB up", res
        if "TEST" not in text5:
            return False, f"Dec SNR 30 printed {text30!r} (as it should), but 5 also missed ({text5!r}) at " \
                          f"{i5.get('heard')} dB", res
        return True, f"heard {weak} dB up: Dec SNR 5 copies {text5!r}, 30 prints {text30!r}", res
    except Refused as e:
        return False, str(e), res


CW_PARSE_TRIES = 3        # cw-parse: overs at most per setting for one the decoder copies right


def t_cw_parse(o, s, l):
    """Parse (CW 2:2) splits run-together CW in the decoder's panel (the remote's cw lines are the decoder's raw text):
    s keys 'CQCQDE<its call>K' (no spaces) once with Parse off, once on. The panel's own text (his?'s tail, the
    remote's view of it, from 2026-11-26): run together with Parse off; 'CQ CQ DE <call> K' with it on, and the call
    taken as the station's ({HIS}'s call). Off first, so a call the panel had before cannot pass for Parse's"""
    L = o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    try:
        for parse in (0, 1):
            # Parse is judged on an over the decoder copied right ('QCHTST' for Q1TST once, 2026-11-26: nothing for
            # Parse to split): up to CW_PARSE_TRIES overs
            for k in range(CW_PARSE_TRIES):
                text, _ = cw_over(o, s, l, f"CQCQDE{call}K", f"parse {parse}", listener_cfg={"cw_parse": parse},
                                  sender_cfg=CW_DEC_SPEED)   # 20 WPM: the X6100's own 28 came out garbled
                if f"DE{call}" in text.replace(" ", ""):
                    break
            h = his(L)
            tail = " ".join(str(h.get("tail", "")).split())
            res[f"parse_{parse}"] = {"raw": text, "tail": tail, "call": h.get("call"), "overs": k + 1}
            if f"DE{call}" not in text.replace(" ", ""):
                return False, f"Parse {parse}: {l}'s decoder never copied {call} in {CW_PARSE_TRIES} overs " \
                              f"(the last {text!r}): nothing for Parse to judge", res
        if "tail" not in str(his(L)):
            return None, f"the remote's his? has no tail (a GUI before it): {res}", res
        off, on = res["parse_0"], res["parse_1"]
        bad = []
        if f"DE{call}" not in off["tail"].replace(" ", "") or f"DE {call}" in off["tail"]:
            bad.append(f"Parse off: the panel ends {off['tail']!r} (run together wanted)")
        if f"DE {call}" not in on["tail"] or "CQ CQ" not in on["tail"]:
            bad.append(f"Parse on: the panel ends {on['tail']!r} ('CQ CQ DE {call} K' wanted)")
        if on["call"] != call:
            bad.append(f"Parse on: the call taken {on['call']!r}, not {call}")
        if bad:
            return False, "; ".join(bad), res
        return True, f"Parse off: {off['tail'][-20:]!r}; on: {on['tail'][-20:]!r}, the call {call} taken", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, L)


ZAP_OFF = 200


def t_cw_zap(o, s, l):
    """ZAP (CW 1:2): the listener ZAP_OFF Hz off the sender's CW (declared) tunes onto it within 10 Hz while the
    decoder follows it; in CW and in CW-R"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        for mode in (4, 5):
            on_g_alike(o, F_CW, mode, agc=2)
            got = {}
            with bench.apart({"freq": bench.STEPPED}, f"cw-zap: {l} {ZAP_OFF} Hz off {s}, then ZAP onto it (within "
                                                      f"10 Hz: what the test measures)"):
                with o.step(f"{l} {ZAP_OFF} Hz above {s} (declared)"):
                    bench.ensure(L, {"freq": F_CW + ZAP_OFF})

                def body(watch):
                    with o.step(f"{s} keys a long VVV; {l} presses ZAP while it hears it") as st, \
                            cfg_kept(S, {"key_speed": CW_WPM_FAST}):
                        # at the sender's own speed the X6200's VVV ran past the watch's 16 s (run 30)
                        th = threading.Thread(target=lambda: got.update(t=tci_cw(o, S, "VVV VVV VVV VVV", watch)))
                        th.start()
                        time.sleep(3.5)
                        btn(L, "cw_zap")
                        th.join(40)
                        f_l = settled(L)
                        got["f"] = f_l
                        st.confirm(f"{l} on {f_l}, {s} on {S.state.get('freq')}")
                offset_over(o, s, l, F_CW, mode, f"cw-zap mode {mode}", body)
                bench.ensure(L, {"freq": F_CW})
            res[f"mode{mode}"] = got.get("f")
            if got.get("f") is None or abs(int(got["f"]) - F_CW) > 10:
                raise Refused(f"mode {mode}: ZAP left {l} on {got.get('f')}, not within 10 Hz of {F_CW}")
        ensure_alike(o, F_CW, 4)
        return True, f"ZAP put {l} within 10 Hz of {s} in CW ({res['mode4']}) and CW-R ({res['mode5']})", res
    except Refused as e:
        return False, str(e), res


def t_cw_tuner(o, s, l):
    """Tuner (CW 2:2): the bars, one per 10 Hz, lit on the station's side; the listener 30 Hz off the sender in CW,
    then CW-R: pictures (LOOK: only a picture shows the bars)"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with cfg_kept(L, {"cw_tune": 1}):
            for mode in (4, 5):
                on_g_alike(o, F_CW, mode, agc=2)
                with bench.apart({"freq": 30}, f"cw-tuner: {l} 30 Hz off {s}"):
                    bench.ensure(L, {"freq": F_CW + 30})
                    classic(L)                          # the bars are the classic screen's (not on G)

                    def body(watch, mode=mode):
                        th = threading.Thread(target=lambda: tci_cw(o, S, "VVV VVV VVV", watch))
                        th.start()
                        time.sleep(2.5)
                        snap(o, f"cw-tuner {l} 30 Hz off, mode {mode}")
                        th.join(30)
                    offset_over(o, s, l, F_CW, mode, f"cw-tuner mode {mode}", body)
                    home(o, L)
        ensure_alike(o, F_CW, 4)
        return None, f"{l}'s tuning bars photographed 30 Hz off in CW and CW-R (the bars: look at the pictures)", res
    except Refused as e:
        return False, str(e), res


def t_cw_peak_q(o, s, l):
    """CW peak (CW 1:2) and its Q on the X6100 (its base's peaking filter at the key tone), nothing sent. The base
    read from the GUI's log (suite_air_a.base_of: the state has none). On a base with the filter (the patched base
    from r12, HW_CAP_CW_PEAK): l's receive noise (cw-noise-shape's measure) through an 800 Hz filter around the key
    tone with CW peak off, on at Q 16 and on at Q 1; PASS when Q 16 lifts the noise at the key tone 3 dB or more over
    the peak off and is narrower than Q 1. On any other (Xiegu's own: no peak at any Q, 2026-11-25): PASS when the GUI
    assumes none: its radio_caps without the bit (cw.cpp then tells the decoder of no peak), CW 1:2 without CW peak
    and its Q, the remote refusing cw_peak_on, cw_peak_q and 'btn cw_peak'. (Its first form keyed s 300 Hz off the
    tone at the default Q 1 and saw -1 dB: a Q 1 peak is the key tone wide)"""
    from tones import TciRx
    import suite_air_a as aa
    L = o.r[l]
    base = aa.base_of(L.host)
    res = {"base": base, "patched_rev": patched_rev(base)}
    try:
        caps = caps_of(L)
        res["caps"] = caps
        if caps is None:
            return False, f"{l}: no radio_caps from the remote (a GUI before it)", res
        has, want = bool(caps & CAP_CW_PEAK), res["patched_rev"] >= 12
        if has != want:
            return False, f"{l}'s base {base or '(not read)'}: the GUI {'assumes' if has else 'denies'} a CW peak " \
                          f"filter (radio_caps {caps}); only the patched base from r12 has one", res
        if not has:
            bad = lacks_check(L, res, cfgs={"cw_peak_on": 1, "cw_peak_q": 16}, buttons=("CW peak", "CW peak Q"),
                              cmds=("btn cw_peak",))
            if bad:
                return False, f"{l}'s base {base or '(not read)'} applies no CW peak, yet {'; '.join(bad)}", res
            return True, f"{l}'s base {base or '(not read)'} applies no CW peak and the GUI assumes none: radio_caps " \
                         f"{caps} without it (the decoder told of no peak), CW 1:2 '{res['pages'].get('CW 1:2')}', " \
                         f"refused: {', '.join(res['refused'])}", res
    except Refused as e:
        return False, str(e), res
    kt = int(L.state.get("kt") or 700)
    lo, hi = kt - 400, kt + 400
    at = int(round(kt / 50.0) * 50)
    try:
        on_g_alike(o, F_CW, 4)
        with o.step(f"both through {lo}-{hi} Hz (the key tone {kt} Hz)"):
            o.both({"filter": (lo, hi)})
        rec = TciRx(L.host)
        try:
            time.sleep(1.5)
            for key, vals in (("off", {"cw_peak_on": 0}), ("q16", {"cw_peak_on": 1, "cw_peak_q": 16}),
                              ("q1", {"cw_peak_on": 1, "cw_peak_q": 1})):
                with o.step(f"{l}: CW peak {key}, {H.CW_SHAPE_S:.0f} s of quiet") as st, cfg_kept(L, vals):
                    time.sleep(1.5)                     # the base's filter settled
                    t0 = time.time()
                    time.sleep(H.CW_SHAPE_S)
                    x, _ = rec.audio(t0, time.time())
                    if len(x) < H.FS * H.CW_SHAPE_S * 0.8:
                        raise Refused(f"{l}: {len(x) / H.FS:.1f} s of audio in {H.CW_SHAPE_S:.0f} s")
                    sh = H.noise_shape(x, H.FS, lo, hi)
                    res[key] = {"at_kt_db": sh["at"].get(at), "peak_hz": sh["peak_hz"], "peak_db": sh["peak_db"],
                                "width6_hz": sh["width6_hz"]}
                    st.confirm(f"{sh['at'].get(at)} dB at {at} Hz; peak {sh['peak_db']} dB at {sh['peak_hz']} Hz, "
                               f"{sh['width6_hz']} Hz wide")
        finally:
            rec.close()
    except Refused as e:
        return False, str(e), res
    off, q16, q1 = (res[k]["at_kt_db"] for k in ("off", "q16", "q1"))
    if None in (off, q16, q1):
        return False, f"no level at {at} Hz ({res})", res
    lift = round(q16 - off, 1)
    res["lift_q16_db"] = lift
    words = f"off {off} dB, Q 16 {q16} dB, Q 1 {q1} dB at {at} Hz; 6 dB widths Q 16 {res['q16']['width6_hz']} Hz, " \
            f"Q 1 {res['q1']['width6_hz']} Hz"
    if lift >= 3 and res["q16"]["width6_hz"] < res["q1"]["width6_hz"]:
        return True, f"CW peak lifts {l}'s noise at the key tone {lift} dB at Q 16, narrower than Q 1 ({words})", res
    # 2026-11-25, on Xiegu's factory base V1.1.9: no peak at any Q (0.4/0.5/0.9 dB, 485 Hz wide at all), the GUI
    # writing the peak and telling the decoder of it: radio_caps' HW_CAP_CW_PEAK since 2026-11-26, the check above
    return False, f"CW peak at Q 16 lifts {l}'s noise at the key tone {lift} dB on the base {base} ({words})", res


def _tone_db(x, fs, hz):
    import numpy as np
    x = np.asarray(x, dtype=float)
    if len(x) < fs // 4:
        return None
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n))) / n
    freqs = np.fft.rfftfreq(n, 1 / fs)
    band = (freqs > hz - 25) & (freqs < hz + 25)
    return float(20 * np.log10(spec[band].max() + 1e-12))


OWN_DECODE_TEXT = "VVV TEST"
OWN_DECODE_WPM = 25


def t_cw_own_decode(o, s, l):
    """The sender's own keying decoded (cw-decoder-own-keying): KEY Train on (sidetone only: the X6200's base keys at
    0 W, the X6100's sounds it), KEY Speed 25, more than 2 s since a key-down (the next sets the decoder to KEY
    Speed): its TCI CW 'VVV TEST' (its own keyer) read back by its own CW decoder from the first word. At the bench's
    VOL; its own TCI audio of the over kept (own_decode_<s>.raw, 24 kHz int16 mono: tools/cw_replay) with its peak,
    the decoder's only copy of it"""
    import numpy as np
    from tones import TciRx
    S = o.r[s]
    res = {}
    text, on, off = "", None, None
    try:
        with cfg_kept(S, {"key_train": 1, "key_speed": OWN_DECODE_WPM, "cw_decoder": 1}):
            ensure_alike(o, F_CW, 4, agc=2)
            res["vol"] = {n: r.state.get("vol") for n, r in o.r.items()}
            rx = TciRx(S.host)
            try:
                with session(o, f"cw-own-decode {s}", [s], F_CW) as watch:
                    watch.shots_on_tx = False   # no screen grab into the sidetone the decoder reads
                    role(o, s)
                    time.sleep(2.5)
                    n0 = len(S.cw)
                    on, off = tci_cw(o, S, OWN_DECODE_TEXT, watch)
                    time.sleep(2.5)             # the decoder's last character out
                    text = " ".join(S.cw[n0:].split()).upper()
                x, x0 = rx.audio()
            finally:
                rx.close()
        res["decoded"] = text
        if x0 is not None and len(x) and on:
            seg = x[max(int((on - 1.0 - x0) * 24000), 0):max(int((off + 2.5 - x0) * 24000), 0)]
            if len(seg):
                path = os.path.join(run_dir(o), f"own_decode_{s}.raw")
                (np.clip(seg, -1, 1) * 32767).astype("<i2").tofile(path)
                res["audio"] = path
                res["peak_dbfs"] = round(20 * math.log10(float(np.max(np.abs(seg))) + 1e-9), 1)
        if OWN_DECODE_TEXT not in text:
            return False, f"{s}'s own decoder read its {OWN_DECODE_TEXT!r} (KEY Train, {OWN_DECODE_WPM} WPM, VOL " \
                          f"{res['vol'].get(s)}) as {text!r}; its own audio of it peaked at " \
                          f"{res.get('peak_dbfs')} dBFS", res
        return True, f"{s}'s own decoder read {text!r} from the first word (KEY Train, {OWN_DECODE_WPM} WPM, VOL " \
                     f"{res['vol'].get(s)}; its own audio peaked at {res.get('peak_dbfs')} dBFS)", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


def t_cw_key_train(o, s, l):
    """KEY Train: the sender's TCI CW goes out as sidetone only (TX power held at 0 W, its message 'KEY Train...'),
    the listener hears nothing of it; Train off, the listener decodes it"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        t0 = time.time()
        with cfg_kept(S, {"key_train": 1}):
            text, (on, off) = cw_over(o, s, l, "TEST", "key-train on", check_power=False)
        w = [x[1] for x in t_frames(S, on, off + 1)]
        res.update(train_text=text, train_w=max(w) if w else None, said=msgs_since(S, t0)[-3:],
                   train_tx_spans=[round((b or time.time()) - a, 1) for a, b in tx_spans(S, on - 1, off + 1)])
        if w and max(w) > 0.05:
            raise Refused(f"Train on: {s} read {max(w)} W on the air")
        if "TEST" in text:
            raise Refused(f"Train on: {l} decoded {text!r}")
        with o.step(f"{s}'s MSG > CW 'TEST' under Train: ON AIR at 0 W, its message, {l} hears nothing") as st:
            mid, idx, n = _cw_msg_add(S, "TEST")
            try:
                with cfg_kept(S, {"key_train": 1}):
                    ensure_alike(o, F_CW, 4, agc=2)
                    with session(o, "key-train: MSG > CW", [s], F_CW) as watch:
                        role(o, s)
                        m = MsgCw(S, idx)
                        try:
                            L.cw = ""
                            t1 = time.time()
                            m.tx()
                            try:
                                S.wait("ON AIR (at 0 W)", lambda: S.tx or watch.why, 6)
                                S.wait("its end", lambda: not S.tx or watch.why, 20)
                            except Refused:
                                pass
                            time.sleep(4.0)             # the message's sidetone through
                        finally:
                            m.close()
                        w2 = [x[1] for x in t_frames(S, t1)]
                        res.update(msg_tx_spans=len(tx_spans(S, t1)), msg_w=max(w2) if w2 else None,
                                   msg_said=msgs_since(S, t1)[-3:], msg_heard=cw_settled(L, 4))
            finally:
                _cw_msg_del(S, mid)
                home(o, S)
            st.confirm(str({k: res[k] for k in ("msg_tx_spans", "msg_w", "msg_said", "msg_heard")}))
        if res["msg_w"] and res["msg_w"] > 0.05:
            raise Refused(f"Train on: MSG > CW put {res['msg_w']} W on the air")
        if "TEST" in res["msg_heard"]:
            raise Refused(f"Train on: {l} decoded MSG > CW's {res['msg_heard']!r}")
        # two words at 20 WPM: one 'TEST' at the X6200's own 28 WPM is a second on the air, its meter came 0 once
        # (round 2 2026-11-26: "x6200's meter showed no power")
        text2, _ = cw_over(o, s, l, "TEST TEST", "key-train off", sender_cfg=CW_DEC_SPEED)
        res["off_text"] = text2
        if "TEST" not in text2:
            raise Refused(f"Train off: {l} decoded {text2!r}, not TEST")
        return True, f"Train on: TCI CW and MSG > CW sidetone only (0 W, {l} heard nothing); off: {l} decoded " \
                     f"{text2!r}. ('ON AIR at 0.0 W' and its message come when the base keys itself: the KEY jack, a " \
                     f"paddle: the owner's hands)", res
    except Refused as e:
        return False, str(e), res


OWN_CARRIER_HZ = 300      # cw-own-carrier: the sender's own carrier looked for this far either side of its dial
OWN_CARRIER_DB = 15       # at least this over the frame's median, in a frame while the key is down (keyed T's leave
                          # most frames in a gap, and the X6100's base sends few on the air: 1 of 6, 36 dB, 11-26)


def _own_peaks(r, f, t0, t1):
    """Per spectrum frame of r in t0..t1: (Hz from f, dB over the frame's median) of its strongest bin within
    OWN_CARRIER_HZ of f, either side (spec_track looks only from 100 Hz on the mode's side: a carrier on the dial never
    counted)"""
    out = []
    for t, cen, wid, v in list(r.r.spectra):          # the remote's frames, as spec_track reads them
        if not (t0 <= t <= t1) or not v:
            continue
        per = wid / len(v)
        med = sorted(v)[len(v) // 2]
        cand = [(v[i], (cen - wid / 2 + (i + 0.5) * per) - f) for i in range(len(v))]
        cand = [c for c in cand if abs(c[1]) <= OWN_CARRIER_HZ]
        if cand:
            db, hz = max(cand)
            out.append((round(hz), round(db - med, 1)))
    return out


def t_cw_own_carrier(o, s, l):
    """While the sender keys CW from the classic screen its TX meters come (with an ALC reading on the X6100; the
    X6200's ALC stays about 0, HW_CAP_TX_NO_ALC): judged. Its carrier on the VFO line and the grey ALC bar are drawn by
    the GUI, not carried in the spectrum frames on the air (the X6100's base sends few, unzoomed; the X6200's showed
    none within 300 Hz of the dial, 2026-11-26): the classic screen photographed on the air, LOOK, with the frames'
    peaks near the dial kept as data"""
    S = o.r[s]
    res = {}
    try:
        got = {}
        ensure_alike(o, F_CW, 4, agc=2)
        classic(S)
        # session(), not over(): over() arms with the Sentry's A/B, which on the classic screen is the VFO key: the
        # X6200 went on the air on VFO B and the Watch stopped the over (vfo 0/1, 2026-11-26)
        with session(o, "cw-own-carrier", [s], F_CW) as watch:
            role(o, s)
            th = threading.Thread(target=lambda: got.update(t=tci_cw(o, S, "TTTTT TTTTT", watch)))
            th.start()
            time.sleep(2.0)
            snap(o, f"cw-own-carrier {s}'s classic screen sending CW (the VFO line, the ALC bar)")
            th.join(30)
        if "t" not in got:
            return False, f"{s}'s TCI CW did not finish", res
        on, off = got["t"]
        peaks = _own_peaks(S, F_CW, on + 0.5, off)
        frames = t_frames(S, on, off)
        res.update(frames=len(peaks), strong=sum(1 for p in peaks if p[1] >= OWN_CARRIER_DB),
                   best_db=max((p[1] for p in peaks), default=None), alc=[x[3] for x in frames][:5])
        if not frames:
            return False, f"no TX meter frames from {s} while it sent", res
        if s == H.A and not any((x or 0) > 0 for x in res["alc"]):
            return False, f"the X6100's TX meters carry no ALC reading ({res['alc']})", res
        return None, f"{s}'s TX meters came, ALC {res['alc'][:3]}; its classic screen photographed on the air (look: " \
                     f"the carrier on the VFO line, the ALC bar grey); its spectrum frames near the dial: " \
                     f"{res['strong']} of {res['frames']} over {OWN_CARRIER_DB} dB, the best {res['best_db']}", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


def t_cw_log_qso(o, s, l):
    """Log QSO (CW 1:2) after the listener's decoder confirmed the sender's call: the quick log opens with the call in
    it and logs it (the day's log, web Logs page: CALL Q1TST/Q2TST, CW); out of CW (SSB) the box is empty"""
    import suite_air_b as ab
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    ip = ab.web_ip(L.host)
    try:
        text, _ = cw_over(o, s, l, f"DE {call} {call} K", "cw-log-qso")
        res["decoded"] = text
        if call not in text:
            raise Refused(f"{l} decoded {text!r}: no {call} for Log QSO")
        with o.step(f"{l}'s classic screen, Log QSO (CW 1:2)") as st:
            classic(L)
            n0 = _log_count(ip, call)
            btn(L, "cw_log", rest=1.5)
            snap(o, f"cw-log-qso {l}: the quick log with the call")
            L.send("knob mfk 39")                   # the on-screen keyboard's last key, its tick (OK): 40 keys
            time.sleep(0.8)
            L.send("key MFK 1")
            time.sleep(0.15)
            L.send("key MFK 0")
            time.sleep(2.0)
            n1 = _log_count(ip, call)
            res["logged"] = n1 - n0
            st.confirm(f"{call} in the day's log {n0} -> {n1}")
        if n1 <= n0:
            return False, f"Log QSO did not log {call} ({n0} -> {n1} rows with it)", res
        return True, f"{l}'s decoder confirmed {call}; Log QSO logged it", res
    except Refused as e:
        return False, str(e), res
    finally:
        _log_forget(ip, call)
        home(o, L)


def _log_qsos(ip, call):
    """The QSOs with call in the radio's logs (the web Logs page: {file, n, id, call...})"""
    import suite_air_b as ab
    st, b = ab.http(ip, "/api/logs")
    if st != 200:
        return []
    try:
        rows = json.loads(b).get("qsos", [])
    except (ValueError, AttributeError):
        return []
    return [q for q in rows if str(q.get("call", "")).upper() == call]


def _log_count(ip, call):
    return len(_log_qsos(ip, call))


def _log_forget(ip, call):
    """The test's QSOs out of the logs again (the web Logs page's delete)"""
    import suite_air_b as ab
    qs = [{"file": q.get("file"), "n": q.get("n"), "id": q.get("id")} for q in _log_qsos(ip, call)]
    if qs:
        ab.http(ip, "/api/logs/delete", method="POST", body={"qsos": qs})


# ---- MSG > CW: messages, tokens, break-in, beacon, QRSS3 ----

def _cw_msg_add(S, text):
    """A CW message added on S's web page; (its id, its place in the list, how many)"""
    import suite_air_b as ab
    ip = ab.web_ip(S.host)
    st, b = ab.http(ip, "/api/cw_messages", method="POST", body={"text": text})
    if st != 200:
        raise Refused(f"{S.name}: the CW message not added ({st})")
    msgs = json.loads(b)["messages"]
    mid = max(x["id"] for x in msgs if x["text"] == text)
    order = [x["id"] for x in msgs]
    return mid, order.index(mid), len(order)


def _cw_msg_del(S, mid):
    import suite_air_b as ab
    ab.http(ab.web_ip(S.host), f"/api/cw_messages/{mid}", method="DELETE")


class MsgCw:
    """MSG > CW on S's classic screen (a dialog: the remote's state names no window for it), its page kept here; its
    runtime switches (Beacon, QRSS3: the GUI keeps them while it runs) read with "cfg? msg_cw_beacon|msg_cw_qrss" and
    put back Off by close(), which always closes the dialog"""

    def __init__(self, S, index=0):
        self.S, self.page = S, 1
        classic(S)
        S.take_control()
        S.send("key MSG 1")
        time.sleep(0.15)
        S.send("key MSG 0")
        time.sleep(1.5)                             # MSG again would step its page
        S.send("knob mfk -60")                      # the top row
        time.sleep(0.6)
        if index:
            S.send(f"knob mfk {index}")
            time.sleep(0.6)

    def key(self, k, rest=0.5):
        self.S.take_control()
        self.S.send(f"key {k} 1")
        time.sleep(0.15)
        self.S.send(f"key {k} 0")
        time.sleep(rest)

    def to(self, page):
        for _ in range((page - self.page) % 3):
            self.key("F1")
        self.page = page

    def tx(self):
        self.to(1)
        self.key("F2", rest=0.2)

    def beacon_s(self):
        return int(bench.cfg_get(self.S, "msg_cw_beacon"))

    def qrss(self):
        return bench.cfg_get(self.S, "msg_cw_qrss") == "1"

    def beacon(self, want_s):
        """Beacon (page 2 F2) stepped to want_s (0 off; 10, 30, 60, 120)"""
        self.to(2)
        for _ in range(6):
            if self.beacon_s() == want_s:
                return
            self.key("F2", rest=0.6)
        raise Refused(f"{self.S.name}: MSG > CW's beacon reads {self.beacon_s()} s, not {want_s}")

    def set_qrss(self, on):
        self.to(2)
        if self.qrss() != on:
            self.key("F4", rest=0.6)
        if self.qrss() != on:
            raise Refused(f"{self.S.name}: MSG > CW's QRSS3 not {'on' if on else 'off'}")

    def close(self):
        """Closed (F5 on every page: it stops a beacon or a QRSS3 send too), then Beacon and QRSS3 back Off in a fresh
        opening (they outlast the dialog while the GUI runs; their keys do nothing while it sends)"""
        S = self.S
        if S.tx:
            S.stop()
        self.key("F5", rest=1.5)
        if self.beacon_s() or self.qrss():
            S.send("key MSG 1")
            time.sleep(0.15)
            S.send("key MSG 0")
            time.sleep(1.5)
            self.page = 1
            try:
                if self.beacon_s():
                    self.beacon(0)
                if self.qrss():
                    self.set_qrss(False)
            finally:
                self.key("F5", rest=1.0)


def t_cw_msg(o, s, l):
    """MSG > CW: a stored message 'TEST {CALL} TEST' sent by TX (page 1 F2): the listener decodes it with the
    sender's call filled in; the sender's TX one over for the whole message (the break-in delay raised through its
    word gaps)"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    mid = None
    try:
        ensure_alike(o, F_CW, 4, agc=2)
        mid, idx, n = _cw_msg_add(S, "TEST {CALL} TEST")
        with session(o, "cw-msg", [s], F_CW) as watch:
            role(o, s)
            with o.step(f"{s}'s MSG > CW, message {idx + 1} of {n}, TX") as st:
                m = MsgCw(S, idx)
                try:
                    L.cw = ""
                    t0 = time.time()
                    m.tx()
                    S.wait("its keying", lambda: S.tx or watch.why, 6)
                    S.wait("the end of the message", lambda: not S.tx or watch.why, 25)
                    spans = tx_spans(S, t0)
                    res["overs"] = len(spans)
                finally:
                    m.close()
                st.confirm(f"{len(spans)} over(s)")
        text = cw_settled(L)
        res["decoded"] = text
        if f"TEST {call}" not in text and call not in text:
            return False, f"{l} decoded {text!r}, not 'TEST {call} TEST'", res
        if res["overs"] != 1:
            return False, f"{l} decoded {text!r}, but {s} keyed {res['overs']} overs, not one through the gaps", res
        return True, f"{l} decoded {text!r} ({{CALL}} filled in), {s} on the air once through the message", res
    except Refused as e:
        return False, str(e), res
    finally:
        if mid is not None:
            _cw_msg_del(S, mid)
        home(o, S)


def his(r, timeout=4.0):
    """The remote's "his?": what MSG > CW's {HIS} would send now ({call, chars, shown}: the decoder panel's text length
    and whether the panel shows)"""
    t0 = time.time()
    r.send("his?")
    while time.time() - t0 < timeout:
        with r.lock:
            got = [x for when, x in list(r.lines) if when >= t0 and x.startswith("his ")]
        if got:
            return json.loads(got[-1][4:])
        time.sleep(0.05)
    raise Refused(f"{r.name}: no answer to 'his?' (a GUI from before it)")


def t_cw_msg_his(o, s, l):
    """{HIS}: the call the decoder confirmed last. s sends 'CQ DE <s's call> x3' (TCI, 20 WPM); l's message
    'TU {HIS} UR 599' sent from its MSG > CW: s decodes '<s's call> UR 599'. The lead-in TU: the X6100's factory base
    keys an over's first elements 30-50 ms late, and run 30's X6200 decoded the call's first two letters as <?>"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    mid = None
    try:
        # three calls at 20 WPM: at 28 the X6200 copied the X6100's 'CQ DE Q1TST Q1TST K' as 'CODE Q1TST MT1TST K', no
        # DE and one clean call, so none confirmed and {HIS} rightly empty (2026-11-25)
        text, _ = cw_over(o, s, l, f"CQ DE {call} {call} {call} K", "cw-his: the call", sender_cfg={"key_speed": 20})
        res["l_decoded"] = text
        if call not in text:
            raise Refused(f"{l} decoded {text!r}: no {call} confirmed")
        # {HIS} read where it was decoded (the Sentry's G) and on the classic screen it is sent from: leaving G gives
        # A's dial back (here 14.200 USB), and back in CW every new mode had cleared the decoder's text and the call
        # with it (text_panel.cpp mode_cb, 2026-11-25). Read before the send: a wrong {HIS} never goes on the air
        res["his"] = {"decoded": his(L)}
        classic(L)
        time.sleep(1.0)
        res["his"]["classic"] = his(L)
        if res["his"]["classic"]["call"] != call:
            snap(o, f"cw-his {l}: on the classic screen before MSG, {{HIS}} not {call}")
            raise Refused(f"{l}'s {{HIS}} is {res['his']['classic']['call']!r} on the classic screen, not {call}: "
                          f"{res['his']}")
        mid, idx, n = _cw_msg_add(L, "TU {HIS} UR 599")
        # 20 WPM: run 31's X6200 copied the X6100's message at 28 as 'TA UR 5CH9' (the token is what is tested)
        with session(o, "cw-his", [l], F_CW) as watch, cfg_kept(L, {"key_speed": 20}):
            role(o, l)
            with o.step(f"{l}'s MSG > CW 'TU {{HIS}} UR 599', TX"):
                m = MsgCw(L, idx)
                try:
                    S.cw = ""
                    m.tx()
                    L.wait("its keying", lambda: L.tx or watch.why, 6)
                    L.wait("the end", lambda: not L.tx or watch.why, 25)
                finally:
                    m.close()
        back = cw_settled(S)
        res["s_decoded"] = back
        # the token is what is tested: the call in its place, between the message's own TU and UR. The 599 is the
        # message's literal text, and the X6100's factory base keys it late in places (its 5 copied as 'S 9', round 2
        # 2026-11-26; cw-timing x6100 measures that keying)
        words = back.split()
        at = [i for i, w in enumerate(words) if call in w]
        if f"{call} UR 599" in back or (at and "UR" in words[at[-1] + 1:at[-1] + 3]):
            return True, f"{l} sent {{HIS}} as {call}: {s} decoded {back!r}", res
        return False, f"{s} decoded {back!r}, not '{call} UR 599'", res
    except Refused as e:
        return False, str(e), res
    finally:
        if mid is not None:
            _cw_msg_del(L, mid)
        home(o, L)


def t_cw_beacon(o, s, l):
    """MSG > CW's beacon: pause 10 s, Until 2 sends: exactly two sends decoded by the listener, then 'Beacon over: 2
    sends'; a beacon stopped in its pause by another transmission (rigctl T 1 / T 0) sends no more"""
    S, L = o.r[s], o.r[l]
    res = {}
    mid = None
    try:
        ensure_alike(o, F_CW, 4, agc=2)
        mid, idx, n = _cw_msg_add(S, "BCN")
        with cfg_kept(S, {"cw_bcn_until": 1, "cw_bcn_sends": 2, "cw_bcn_start": -1}):
            with session(o, "cw-beacon", [s], F_CW) as watch:
                role(o, s)
                with o.step(f"{s}'s beacon: 10 s pause, Until 2 sends") as st:
                    m = MsgCw(S, idx)
                    try:
                        m.beacon(10)
                        L.cw = ""
                        t0 = time.time()
                        m.tx()
                        over = wait_msg(S, t0, r"beacon over", 45)
                        spans = tx_spans(S, t0)
                        res.update(spans=len(spans), said=over)
                    finally:
                        m.close()
                    st.confirm(f"{len(spans)} sends, then {over!r}")
            text = cw_settled(L)
            res["decoded"] = text
        if res["spans"] != 2 or "BCN" not in text:
            return False, f"{s} sent {res['spans']} times, {l} decoded {text!r} (two sends wanted)", res
        return True, f"two sends 10 s apart ({l} decoded {text!r}), then {res['said']!r}", res
    except Refused as e:
        return False, str(e), res
    finally:
        if mid is not None:
            _cw_msg_del(S, mid)
        home(o, S)


def t_cw_beacon_stops(o, s, l):
    """A beacon in its pause stopped by another transmission (the mic's PTT, the remote's lease, a second): no more
    sends in the next 15 s"""
    S, L = o.r[s], o.r[l]
    res = {}
    mid = None
    try:
        ensure_alike(o, F_CW, 4, agc=2)
        mid, idx, n = _cw_msg_add(S, "BCN")
        with cfg_kept(S, {"cw_bcn_until": 0, "cw_bcn_start": -1}):
            with session(o, "cw-beacon-stops", [s], F_CW) as watch:
                role(o, s)
                m = MsgCw(S, idx)
                try:
                    m.beacon(10)
                    m.tx()
                    S.wait("the first send", lambda: S.tx or watch.why, 6)
                    S.wait("its pause", lambda: not S.tx or watch.why, 20)
                    time.sleep(2.0)
                    hold = int(time.time())             # the mic's PTT, leased (rigctl's PTT is refused by design)
                    S.send(f"key PTT 1 {hold}")
                    S.wait("keyed in the pause", lambda: S.tx or watch.why, 3)
                    time.sleep(0.8)
                    S.send("key PTT 0")
                    S.wait("off again", lambda: not S.tx, 4)
                    t_stop = time.time()
                    time.sleep(15)
                    later = [sp for sp in tx_spans(S, t_stop + 1.0)]
                    res.update(sends_after=len(later))
                finally:
                    m.close()
        if later:
            return False, f"the beacon sent {len(later)} more time(s) after a PTT in its pause", res
        return True, "a PTT in the pause stopped the beacon: nothing sent after it in 15 s", res
    except Refused as e:
        return False, str(e), res
    finally:
        if mid is not None:
            _cw_msg_del(S, mid)
        home(o, S)


F_QRSS = 10140050


def t_qrss3(o, s, l):
    """QRSS3 (MSG > CW page 2 F4) on 30 m: TX of the message 'E' sends a 3 s dot on 10.140050 MHz (the sender moves
    there; the listener put there first); then a send stopped by a retune: 'QRSS3 stopped: tuned away'"""
    S, L = o.r[s], o.r[l]
    res = {}
    mid = None
    try:
        ensure_alike(o, F_QRSS, 4, agc=2)
        mid, idx, n = _cw_msg_add(S, "E")
        with session(o, "qrss3", [s], F_QRSS) as watch:
            role(o, s)
            m = MsgCw(S, idx)
            with o.step(f"{s}'s QRSS3 on, message 'E' sent") as st:
                t0 = time.time()
                m.set_qrss(True)
                m.tx()
                S.wait("the dot", lambda: S.tx or watch.why, 12)
                f_tx = int(S.state.get("freq", 0))
                S.wait("its end", lambda: not S.tx or watch.why, 8)
                time.sleep(5.0)                     # the send's end (its TX key reads Stop till then)
                sp = tx_spans(S, t0)
                res.update(freq=f_tx, spans=[round((b or time.time()) - a, 1) for a, b in sp])
                st.confirm(f"on {f_tx}, keyed {res['spans']} s")
            with o.step(f"{s}'s QRSS3 again, retuned mid-send: stopped") as st:
                t1 = time.time()
                m.tx()
                try:
                    S.wait("the dot", lambda: S.tx or watch.why, 4)
                except Refused:                         # that press stopped a send still in its gap: once more
                    t1 = time.time()
                    m.tx()
                    S.wait("the dot", lambda: S.tx or watch.why, 12)
                time.sleep(0.8)
                with bench.apart({"freq": bench.STEPPED}, "qrss3: the sender's dial turned a step to stop it"):
                    try:
                        S.send("knob main 1")           # the dial turned (a "set" is refused while on the air)
                        S.wait("off the air", lambda: not S.tx, 3)
                        said = wait_msg(S, t1, r"QRSS3 stopped", 4)
                        res["stopped"] = said
                    finally:
                        m.close()
                        bench.ensure(S, {"freq": F_QRSS})
                st.confirm(said)
        long_ = [x for x in res["spans"] if 2.4 <= x <= 4.0]
        if res["freq"] != F_QRSS or not long_:
            return False, f"QRSS3 sent on {res['freq']} for {res['spans']} s (a 3 s dot on {F_QRSS} wanted)", res
        return True, f"a 3 s dot on {F_QRSS} ({res['spans']} s); retuned mid-send: {res['stopped']!r}", res
    except Refused as e:
        return False, str(e), res
    finally:
        if mid is not None:
            _cw_msg_del(S, mid)
        home(o, S)


def t_qrss3_key_safety(o, s, l):
    """QRSS3's key-safety on a crash (the GUI killed mid-dash: the key let go at once) needs the owner's word: killing
    the GUI on the air is the owner's to allow (features.json). Refused without ~/.config/x6x00-bench/owner_ok_qrss_kill"""
    if not os.path.exists(os.path.join(bench.STATE, "owner_ok_qrss_kill")):
        raise Refused("needs the owner's word: a GUI killed mid-dash on the air (owner_ok_qrss_kill not there)")
    return None, "the owner's word is there; the kill test itself is not written yet", {}


# ---- the test beacon (the Bands app and Field Ops' BANDS: dialog_field_test_beacon), Field Ops' CQ ----

def _beacon_two_presses(o, S, L, s, l, names, label, third=False):
    """First press: S moves to the band's CW frequency, says 'Press again'; the listener put there too; second press
    within 10 s: 'TEST DE <call>' x3 (KEY Speed raised for it), the listener decodes TEST DE; S back where it was.
    third: a third press 1 s into the send stops it at once and S goes back"""
    res = {}
    f0 = int(S.state.get("freq", 0))
    t0 = time.time()
    press(S, "F2", names, rest=1.5)
    f_b = settled(S)
    res["moved_to"] = f_b
    said = wait_msg(S, t0, r"press again|again to send", 4)
    res["said"] = said
    with bench.apart({"freq": (f0, f_b)}, f"{label}: {s} on the band's CW frequency before {l} follows"):
        bench.ensure(L, {"freq": f_b, "mode": 4})
    with session(o, label, [s], f_b) as watch:
        role(o, s)
        L.cw = ""
        t1 = time.time()
        press(S, "F2", names, rest=0.2)
        S.wait("the beacon on the air", lambda: S.tx or watch.why, 6)
        if third:
            time.sleep(1.0)
            press(S, "F2", names, rest=0.2)
            S.wait("off the air at once", lambda: not S.tx, 2)
            res["stopped_s"] = round(time.time() - t1, 1)
        wait_quiet(S, watch=watch)
        res["spans"] = len(tx_spans(S, t1))
    back = settled(S, limit=8)
    res["back_on"] = back
    res["decoded"] = cw_settled(L)
    with bench.apart({"freq": (f0, f_b)}, f"{label}: {l} back to {s}"):
        bench.ensure(L, {"freq": back})
    return res


def t_bands_beacon(o, s, l):
    """The Bands app's Test beacon: two presses as Field Ops' (first moves and asks, second sends TEST DE <call>
    three times), the radio back where it was after; ESC while waiting: nothing sent within 11 s"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        ensure_alike(o, F_CW - 10000, 4)          # off the band's beacon frequency (14.040 on 20 m)
        with cfg_kept(S, {"key_speed": CW_WPM_FAST}):
            open_app(S, 119, "Bands")
            ensure_alike(o, F_CW - 10000, 4)            # the app (or a restart) may bring its own dial and mode
            r1 = _beacon_two_presses(o, S, L, s, l, ("Bands",), "bands-beacon")
            res.update(r1)
            if H.TEST_CALLS[s] not in r1["decoded"]:
                raise Refused(f"{l} decoded {r1['decoded']!r}: no {H.TEST_CALLS[s]} ('TEST DE {H.TEST_CALLS[s]}' x3 sent)")
            if r1["back_on"] != F_CW - 10000:
                raise Refused(f"{s} on {r1['back_on']} after the beacon, not back on {F_CW - 10000}")
            with o.step(f"{s}: first press, then ESC: nothing sent within 11 s") as st:
                t0 = time.time()
                press(S, "F2", ("Bands",), rest=1.0)
                f_b = settled(S)
                with bench.apart({"freq": (F_CW - 10000, f_b)}, "bands-beacon: the first press alone, then ESC"):
                    S.send("close")
                    time.sleep(11.5)
                    sent = tx_spans(S, t0)
                    res["esc_sent"] = len(sent)
                    bench.ensure(S, {"freq": F_CW - 10000, "mode": 4})
                st.confirm(f"{len(sent)} transmissions")
            if res["esc_sent"]:
                return False, f"after the first press and ESC {s} transmitted {res['esc_sent']} time(s)", res
        return True, f"first press to {r1['moved_to']} ({r1['said']!r}), second: {l} decoded {r1['decoded']!r}, " \
                     f"{s} back on {F_CW - 10000}; first press then ESC: nothing sent", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


def _reconnect(o, n, why):
    t0 = time.time()
    while True:
        try:
            o.r[n].reconnect()
            return
        except (Refused, OSError):
            if time.time() - t0 > 90:
                raise Refused(f"{n}: its GUI did not come back ({why})")
            time.sleep(3)


@contextlib.contextmanager
def field_staged(o, n, state_text, why):
    """n's Field Ops opening on a screen the test stages: Field Ops reads /mnt/field/state once a GUI run, so the
    file is kept aside and the test's written while n's GUI is stopped, then started (one command, a planned restart,
    said on the console); after: the owner's file put back the same way. No key pressed on a page the test has not
    put there (2026-11-25: keys pressed blind on Field Ops' pages started its 25 MB list download)"""
    r = o.r[n]

    def restart(cmd, what):
        o.st.ev(f"{n}: its GUI restarts ({what}, planned)", "radio")
        o.planned[n] = f"planned: {what}"
        o.changing += 1
        try:
            ssh(r.host, f"/etc/init.d/S95gui stop >/dev/null 2>&1; {cmd}; /etc/init.d/S95gui start >/dev/null 2>&1",
                120)
            time.sleep(8)
            _reconnect(o, n, what)
            r.take_control()
            # a GUI comes back on its own saved dial (run 30: the X6200 on 14.030 beside the X6100's 14.040, out of
            # sync before the test put it right): the other's dial and mode again, still inside the planned window
            other = next(x for m, x in o.r.items() if m != n)
            bench.ensure(r, {"freq": int(other.state.get("freq", 0)), "mode": int(other.state.get("mode", 0))})
        finally:
            o.changing -= 1
            o.planned.pop(n, None)

    text = state_text.replace("'", "")
    restart(f"cp /mnt/field/state /tmp/field_state.bench 2>/dev/null || : > /tmp/field_state.bench; "
            f"printf '%s' '{text}' > /mnt/field/state", f"Field Ops staged: {why}")
    try:
        yield r
    finally:
        restart("[ -s /tmp/field_state.bench ] && cp /tmp/field_state.bench /mnt/field/state || rm -f /mnt/field/state; "
                "rm -f /tmp/field_state.bench", "Field Ops put back as found")


FIELD_BANDS = "screen=2\nhome=BANDS\nprog=S\nactive=0\nstart=0\nlog=\n"


def field_sota(now):
    return (f"screen=1\nhome=SOTA\nprog=S\nactive=1\nstart={int(now)}\nlog=\n"
            "ref=S\tQ1/BT-001\tBench summit\t\t\t1000\t1\n")


def t_field_beacon(o, s, l):
    """Field Ops > BANDS > Test beacon: the two presses (sends TEST DE <call> x3, back after); a third press 1 s into
    the send stops it at once and the radio goes back"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        ensure_alike(o, F_CW - 10000, 4)          # off the band's beacon frequency
        with field_staged(o, s, FIELD_BANDS, "its BANDS screen"), \
                cfg_kept(S, {"key_speed": CW_WPM_FAST}):
            open_app(S, 118, "Field")
            ensure_alike(o, F_CW - 10000, 4)            # the restart brought the GUI's saved dial back
            snap(o, f"field-beacon {s}: Field Ops' BANDS screen")
            r1 = _beacon_two_presses(o, S, L, s, l, ("Field",), "field-beacon")
            res["send"] = r1
            if H.TEST_CALLS[s] not in r1["decoded"]:
                raise Refused(f"{l} decoded {r1['decoded']!r}: no {H.TEST_CALLS[s]} ('TEST DE {H.TEST_CALLS[s]}' x3 sent)")
            r2 = _beacon_two_presses(o, S, L, s, l, ("Field",), "field-beacon-stop", third=True)
            res["stop"] = r2
            if r2.get("stopped_s") is None or r2["stopped_s"] > 3.0:
                raise Refused(f"the third press did not stop the send at once ({r2.get('stopped_s')} s)")
            if r2["back_on"] != F_CW - 10000:
                raise Refused(f"after the stop {s} on {r2['back_on']}, not back on {F_CW - 10000}")
        return True, f"Field Ops' test beacon: {l} decoded {r1['decoded']!r}; a third press stopped it at " \
                     f"{r2['stopped_s']} s and {s} went back", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


def t_field_cq(o, s, l):
    """Field Ops in a SOTA activation (Select SOTA, Activate), CW: CQ sends 'CQ CQ SOTA DE <call> <call> K', the
    listener decodes it; CQ again mid-send stops it. In SSB: CQ plays the voice CQ ('CQ SOTA' in /mnt/msg) when there
    is one, else says 'No voice CQ yet'"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    try:
        ensure_alike(o, F_CW, 4, agc=2)
        with field_staged(o, s, field_sota(time.time()),
                          "a bench SOTA activation"), \
                cfg_kept(S, {"key_speed": CW_WPM_FAST}), session(o, "field-cq", [s], F_CW) as watch:
            role(o, s)
            ensure_alike(o, F_CW, 4, agc=2)
            open_app(S, 118, "Field")
            bench.ensure(S, {"freq": F_CW, "mode": 4})
            snap(o, f"field-cq {s}: the staged activation (ACT 1:2: F2 CQ)")
            L.cw = ""
            t1 = time.time()
            press(S, "F2", ("Field",), rest=0.3)    # CQ (ACT 1:2, F2)
            S.wait("CQ on the air", lambda: S.tx or watch.why, 6)
            S.wait("its end", lambda: not S.tx or watch.why, 25)
            text = cw_settled(L)
            res["decoded"] = text
            t2 = time.time()
            press(S, "F2", ("Field",), rest=0.2)
            S.wait("CQ again on the air", lambda: S.tx or watch.why, 6)
            time.sleep(1.0)
            press(S, "F2", ("Field",), rest=0.2)
            t_p = time.time()
            S.wait("stopped (the letter on the air finished)", lambda: not S.tx, 3.5)
            res["stop_s"] = round(time.time() - t_p, 1)
            if "CQ" not in text or call not in text:
                raise Refused(f"{l} decoded {text!r}: no CQ and {call} ('CQ CQ SOTA DE {call} {call} K' sent)")
            with o.step(f"{s} in USB: CQ plays the voice CQ or says there is none") as st:
                with bench.apart({"mode": (4, 2)}, "field-cq: the sender's SSB CQ check (nothing sent without a file)"):
                    bench.ensure(S, {"mode": 2})
                    t3 = time.time()
                    press(S, "F2", ("Field",), rest=1.5)
                    if S.tx:
                        S.stop()
                        res["ssb"] = "played a voice CQ (stopped)"
                    else:
                        res["ssb"] = wait_msg(S, t3, r"voice CQ|sending", 4)
                    bench.ensure(S, {"mode": 4})
                st.confirm(res["ssb"])
        return True, f"CQ in CW: {l} decoded {text!r}; CQ again stopped it ({res['stop_s']} s); SSB: {res['ssb']!r}", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


# ---- Propagation's beacon cells, the Scanner ----

NCDXF_20M = 14100000


BEACON_BANDS_N = 5        # the NCDXF round's bands (beacons.h); 20 m the first
PROP_BEACON_MIN_DB = 10   # prop-beacon: the slot's beacon heard at least this over the band's noise


def t_prop_beacon(o, s, l):
    """Propagation's Beacons (NCDXF, 10 s slots): l's app on Beacons, held on 20 m (Band: F4 until it listens on
    14.100 CW); s puts a 0.1 W carrier on 14.100000 itself (USB 1 kHz under it, a 1 kHz tone: in l's CW passband) for 7
    s of one slot. Once the next slot starts that slot is scored: l's beacons on 20 m (the remote's read-only
    beacons_heard_20m, from 2026-11-26) have the slot's beacon ((t mod 180) / 10) heard PROP_BEACON_MIN_DB or more. Before
    2026-11-26 the test never left the Conditions view and photographed it"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        ensure_alike(o, NCDXF_20M, 2)
        with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED, "filter": bench.STEPPED, "agc": bench.STEPPED},
                         f"prop-beacon: {l}'s Beacons listen in CW on 14.100, {s} USB 1 kHz under it puts its carrier "
                         f"there"):
            with o.step(f"{l}'s Propagation on Beacons, held on 20 m") as st:
                o.changing += 1
                try:
                    open_app(L, 126, "Propagation")
                    press(L, "F2", ("Propagation",), rest=1.5)         # Beacons
                    for _ in range(BEACON_BANDS_N):
                        press(L, "F4", ("Propagation",), rest=1.5)     # Band: the one it is on, then the next
                        if int(L.state.get("freq", 0)) == NCDXF_20M:
                            break
                    else:
                        raise Refused(f"{l}'s Beacons never held 20 m ({L.state.get('freq')})")
                finally:
                    o.changing -= 1
                st.confirm(f"{l} on {L.state.get('freq')} mode {L.state.get('mode')}")
            with o.step(f"{s} on page G"):
                o.changing += 1
                try:
                    S.send("open sentry G")
                    S.wait("page G", lambda: S.state.get("pg") == "G", 6)
                finally:
                    o.changing -= 1
            while int(time.time()) % 10 != 1:              # a slot's first half second is left out
                time.sleep(0.05)
            slot = int(time.time()) // 10 * 10
            res["slot"], res["expect"] = slot, (slot % 180) // 10

            def body(watch):
                bench.tci_send(S, [[1000, 7.0]], watch=watch)
            offset_over(o, s, l, NCDXF_20M - 1000, 2, "prop-beacon", body)
            while time.time() < slot + 12:                 # scored once the next slot starts
                time.sleep(0.2)
            res["heard"] = bench.cfg_get(L, "beacons_heard_20m")
            snap(o, f"prop-beacon {l}: the 20 m row after {s}'s carrier in a slot")
            o.changing += 1
            try:
                home(o, L)
                bench.ensure(S, {"freq": NCDXF_20M, "mode": 2})
            finally:
                o.changing -= 1
        if res["heard"] is None:
            return None, f"no beacons_heard_20m from {l}'s remote (a GUI before it)", res
        got = {}
        for e in str(res["heard"]).split(","):
            parts = e.split(":")
            if len(parts) == 3 and int(parts[2]) == slot:
                got[int(parts[0])] = int(parts[1])
        res["in_slot"] = got
        if res["expect"] not in got:
            return False, f"{l}'s Beacons scored no beacon {res['expect']} in the slot of {s}'s carrier ({got}; all: " \
                          f"{res['heard']!r})", res
        if got[res["expect"]] < PROP_BEACON_MIN_DB:
            return False, f"beacon {res['expect']} heard only {got[res['expect']]} dB (at least {PROP_BEACON_MIN_DB})", res
        return True, f"{s}'s carrier in beacon {res['expect']}'s slot: {l}'s Beacons scored it heard " \
                     f"{got[res['expect']]} dB over the noise", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, L)


# The carrier's channel between two in other bands (phone segments: the scanning radio keys on them): 20 kHz apart
# (14.210/230/250) the X6200's 0.1 W skirt put -94 dB into its neighbours, 20 over their quiet, and the X6100's
# scanner stopped on 14.210 (2026-11-25). A step across bands is also the X6100's meter's slowest (dsp_meter_lag_ms)
SCAN_CH = [7200000, 14230000, 21300000]


def _channels_add(r, freqs):
    import suite_air_b as ab
    ip = ab.web_ip(r.host)
    added = []
    for i, f in enumerate(freqs):
        st, b = ab.http(ip, "/api/channels", method="POST",
                        body={"name": f"BENCH {i + 1}", "frequency": f, "mode": "USB", "agc": "FAST", "pre": False,
                              "att": False})
        if st != 200:
            raise Refused(f"{r.name}: the bench channel {f} not added ({st} {b[:80]})")
        added.append(f)
    return ip


def _channels_drop(ip, names=("BENCH 1", "BENCH 2", "BENCH 3")):
    import suite_air_b as ab
    for _ in range(6):
        st, b = ab.http(ip, "/api/channels")
        try:
            chans = json.loads(b).get("channels", [])
        except (ValueError, AttributeError):
            return
        idx = next((i for i, c in enumerate(chans) if c.get("name") in names), None)
        if idx is None:
            return
        ab.http(ip, f"/api/channels/{idx}", method="DELETE", body={"was": chans[idx]})


def t_scanner(o, s, l):
    """The Scanner (subject l) through three bench channels (7.200/14.230/21.300 USB, added on its web page, taken out
    after): s keys a tone on 14.230: l stops there (its dial holds, its S-meter up) and goes on Hang seconds after s
    ends; l keyed itself (PTT 3 s at 0.1 W) while scanning: its dial frozen meanwhile, the scan on after"""
    S, L = o.r[s], o.r[l]
    res = {}
    ip = None
    try:
        ensure_alike(o, SCAN_CH[1], 2)
        ip = _channels_add(L, SCAN_CH)
        open_app(L, 127, "Scanner")
        with bench.apart({k: bench.STEPPED for k in ("freq", "mode", "flo", "fhi", "att", "pre", "agc", "zoom", "rfg")},
                         "scanner: the scanning radio steps through its channels in their modes and settings"):
            for _ in range(5):                                  # Source: the Channels (each press says its list)
                t_s = time.time()
                press(L, "F2", ("Scanner",), rest=1.0)
                said = " ".join(msgs_since(L, t_s))
                if "Channels" in said:
                    res["source"] = said
                    break
            else:
                raise Refused(f"{l}'s Scanner: Source never said Channels")
            t0 = time.time()
            press(L, "F3", ("Scanner",), rest=3.0)              # Start
            res["visits"] = len({f for _, f, _ in _visits(L, t0)})
            got = {}

            def body(watch):
                # the carrier once the scan has left its channel (its first look there, quiet, learned its level in
                # Auto), on long before the scan comes round again: a round of 21 (the radio's own 18 and the bench's
                # 3) takes about 11 s on the X6100 (dsp_meter_lag_ms), 6 s on the X6200. Started when the scan was on
                # the channel before, the carrier came up after the scan's look (2026-11-25: both passed it once and
                # stopped there a round later)
                L.wait(f"the scan off {SCAN_CH[1]}", lambda: int(L.state.get("freq", 0)) != SCAN_CH[1] or watch.why,
                       20)
                t1 = time.time()
                meters, done = [], threading.Event()

                def sample():                           # l's S-meter and dial, and s on the air, through the carrier
                    while not done.is_set():
                        meters.append((round(time.time() - t1, 1), L.meter, int(L.state.get("freq", 0))))
                        if S.tx and "air" not in got:
                            got["air"] = time.time()
                        done.wait(0.25)
                th_m = threading.Thread(target=sample, daemon=True)
                th_m.start()
                try:
                    bench.tci_send(S, [[1000, 14.5]], max_s=15.5, watch=watch)
                finally:
                    done.set()
                got["on"] = (t1, time.time())
                res["meter_through_carrier"] = [m for m in meters if m[2] == SCAN_CH[1]] or meters[:12]
            offset_over(o, s, l, SCAN_CH[1], 2, "scanner: the carrier", body,
                        pair_ignore=("mode", "flo", "fhi", "att", "pre", "agc", "zoom", "rfg"))
            on0, on1 = got["on"]
            if "air" not in got:
                raise Refused(f"{s} never on the air for the carrier")
            air = got["air"] + 1.0                      # the carrier up and steady
            # l's stays (from, to, f) from the Start on; the first on the carrier's channel still there once the
            # carrier was steady must last to its end
            vis = _visits(L, t0, on1)
            stays = [(at, nxt, f) for (at, f, _), nxt in zip(vis, [v[0] for v in vis[1:]] + [on1])]
            first = next((st for st in stays if st[2] == SCAN_CH[1] and st[1] > air), None)
            res.update(air_after_s=round(got["air"] - on0, 1),
                       stays=[(round(a - on0, 1), round(b - on0, 1), f) for a, b, f in stays if b > on0])
            if first is None:
                raise Refused(f"the scan never came to {SCAN_CH[1]} while {s} sent there 14.5 s")
            res["stopped_after_s"] = round(max(first[0], air) - on0, 1)
            if first[1] < on1 - 0.5:
                snap(o, f"scanner {l}: left {SCAN_CH[1]} while {s} still sent (its threshold on the screen)")
                raise Refused(f"the scan left {SCAN_CH[1]} {first[1] - on0:.1f} s into {s}'s carrier, "
                              f"{on1 - first[1]:.1f} s before its end")
            time.sleep(8.0)
            after = [f for _, f, _ in _visits(L, on1 + 0.5)]
            res["after"] = after[:6]
            if len(set(after)) < 2:
                raise Refused(f"after the carrier the scan stayed on {after}")
            with o.step(f"{l} keyed 3 s while scanning, on a bench channel: its dial frozen") as st:
                on_ham = lambda: int(L.state.get("freq", 0)) in SCAN_CH[:2]     # noqa: E731
                gated = contextlib.ExitStack()
                for k in range(8):          # the gates read the dial, and the scan moves on while they run
                    L.wait("the scan on 7.200 or 14.230", on_ham, 30)
                    try:
                        gated.enter_context(session(o, "scanner: keyed", [l]))
                        break
                    except Refused as e:
                        if "outside the amateur bands" not in str(e) or k == 7:
                            raise
                with gated:
                    role(o, l)
                    # keyed as the scan comes to 7.200 or 14.230: the next one is in an amateur band too if it moves
                    # on first. Waited for before the session's set-up (5 s), the X6200's scan was on 4.2085 by the
                    # key, which it refuses outside the bands, and scanned on (2026-11-25)
                    L.wait("the scan on 7.200 or 14.230", on_ham, 30)
                    t2, aired = time.time(), False
                    L.send(f"key PTT 1 {int(t2)}")
                    for _ in range(3):
                        for _ in range(10):
                            time.sleep(0.1)
                            aired = aired or L.tx
                        L.send(f"key PTT 1 {int(t2)}")
                    L.send("key PTT 0")
                    L.wait("off the air", lambda: not L.tx, 4)
                    t3 = time.time()
                if not aired:
                    said = L.errs[-1] if L.errs else "no error said"
                    raise Refused(f"{l} never on the air for its PTT ({said})")
                frozen = {f for _, f, _ in _visits(L, t2 + 0.5, t3)}
                res["frozen"] = sorted(frozen)
                st.confirm(f"on {sorted(frozen)} while keyed")
            if len(frozen) > 1:
                raise Refused(f"the scan moved while {l} was on the air: {res['frozen']}")
        return True, f"stopped on {SCAN_CH[1]} {res['stopped_after_s']} s into {s}'s carrier and held there, went on " \
                     f"after ({after[:3]}); frozen on {res['frozen']} while {l} was keyed", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, L)
        if ip:
            _channels_drop(ip)
        try:
            ensure_alike(o, int(L.state.get("freq", F_USB)), 2)
        except Refused:
            pass


def _visits(r, t0, t1=None):
    out = []
    with r.lock:
        lines = [(at, x) for at, x in list(r.lines) if at >= t0 and (t1 is None or at <= t1) and x.startswith("state ")]
    for at, x in lines:
        try:
            st = json.loads(x[6:])
        except ValueError:
            continue
        f, m = int(st.get("freq", 0)), int(st.get("mode", -1))
        if not out or out[-1][1] != f:
            out.append((at, f, m))
    return out


# ---- FT8 ----

TEST_GRID = "JO22"              # a test locator for the grid tests (not the station's own)


def run_dir(o):
    d = getattr(o.st, "run_dir", None) or os.path.join(bench.STATE, "runs", "orch_cable")
    os.makedirs(d, exist_ok=True)
    return d


def ft8_tci_msg(o, S, text, watch, hz=1500):
    """An FT8 message the app cannot be told to send (gen_ft8's, at hz) keyed through S's TCI into the next slot,
    by S's own clock (suite_air_b's way): (on, off)"""
    import numpy as np
    import digimodes
    import suite_air_b as ab
    x = ab.gen_ft8_24k(o.c, text, hz, run_dir(o))
    on = np.flatnonzero(np.abs(x) > 1e-3 * max(np.abs(x).max(), 1e-9))
    x = x[max(0, on[0] - int(ab.FT8_LEAD * ab.FAX_FS)):on[-1] + int(0.1 * ab.FAX_FS)]
    x = 0.45 * x / max(np.abs(x).max(), 1e-9)
    local = os.path.join(run_dir(o), f"ft8msg_{S.name}.raw")
    secs = digimodes.write_raw(x, local)
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:/tmp/bench_ft8msg.raw"], check=True)
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
                    f"root@{S.host}:/tmp/tci_tone_tx.py"], check=True)
    off = ab.radio_offset(S.host) or 0.0
    slot = math.ceil((time.time() + off + 3.0) / 15.0) * 15.0
    _, t0, t1 = ab.tci_play(S, "/tmp/bench_ft8msg.raw", secs + 2, watch,
                            radio_at=slot + 0.5 - ab.FT8_LEAD - ab.FT8_KEY_LAG)
    S.wait("off the air (the TCI tail)", lambda: not S.tx, 5)
    return t0, time.time()


@contextlib.contextmanager
def ft8_pair(o, label, dial=F_FT8, senders=None, w=None):
    """Both radios' FT8 apps open on dial (test calls, Auto off), the gates for senders (both by default), their power
    w (0.1 W unless said); after: stopped, closed, both on A"""
    try:
        with o.step("both on their test calls, the FT8 app open on both on the dial"):
            H.ft8_both(o, dial)
            if w:
                for n in (senders or list(o.r)):
                    bench.ensure(o.r[n], {"pwr": w})
        with session(o, label, senders or list(o.r), dial, w=w) as watch:
            yield watch
    finally:
        for r in o.r.values():
            r.send("ft8 stop")
        for r in o.r.values():
            r.send("ft8 close")
        try:
            o.apps_closed(dial, 3)
        except Refused:
            pass
        for r in o.r.values():
            home(o, r)


def _decoded(L, n0, want=None, secs=8):
    """L's FT8 rows since n0 (want: wait for one with want in its message)"""
    if want:
        try:
            L.wait(f"{want!r} decoded", lambda: any(want in bench.msg(r) for r in L.rows[n0:]), secs)
        except Refused:
            pass
    return [bench.msg(r) for r in L.rows[n0:]]


def t_ft8_cq_grid(o, s, l):
    """FT8's CQ with the bench's test calls (Q?TST: non-standard FT8 calls on purpose): the radio says 'no QTH or
    modifier with this call' and sends 'CQ <call>' alone, whatever the QTH (JO22ab) and the CQ Modifier (POTA). The
    4-character square and the modifier themselves need a standard call (not the bench's): partial"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    try:
        with cfg_kept(S, {"qth": TEST_GRID + "ab", "ft8_cq_modifier": "POTA"}):
            with ft8_pair(o, "ft8-cq-grid", senders=[s]) as watch:
                role(o, s)
                n0 = len(L.rows)
                t0 = time.time()
                sent = H.ft8_send(o, S, L, watch, "ft8 cq", "ft8-cq-grid", F_FT8)
                got = _decoded(L, n0, call)
                said = msgs_since(S, t0)
                res.update(sent=sent, decoded=got, said=said[-3:])
        if f"CQ {call}" not in got or any(TEST_GRID in g or "POTA" in g for g in got):
            return False, f"{l} decoded {got} (wanted 'CQ {call}' alone with a non-standard call)", res
        if not any("no QTH or modifier" in m for m in said):
            return None, f"{l} decoded 'CQ {call}' alone; the radio's note not seen ({said[-3:]})", res
        return True, f"a non-standard call's CQ: 'CQ {call}' alone, the radio said so ({[m for m in said if 'QTH' in m][0]!r})", res
    except Refused as e:
        return False, str(e), res


def t_ft8_retries(o, s, l):
    """Retries: l answers s's CQ and s never replies: l's reply goes out Retries times (3 set for the test), then its
    QSO is dropped; Auto off, nothing moves on by itself: s's answer row needs a press ('ft8 reply') to move l on"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with cfg_kept(L, {"ft8_max_repeats": 3}):
            with ft8_pair(o, "ft8-retries") as watch:
                role(o, s)
                n0 = len(L.rows)
                cq = H.ft8_send(o, S, L, watch, "ft8 cq", "ft8-retries: the CQ", F_FT8)
                rows = [r for r in L.rows[n0:] if bench.msg(r) == cq]
                if not rows:
                    L.wait("the CQ decoded", lambda: any(bench.msg(r) == cq for r in L.rows[n0:]), 8)
                    rows = [r for r in L.rows[n0:] if bench.msg(r) == cq]
                role(o, l)
                t0 = time.time()
                L.send(f"ft8 reply {rows[0]['id']}")
                deadline = time.time() + 3 * 30 + 20
                while time.time() < deadline and not watch.why:
                    if len(tx_spans(L, t0)) >= 3 and not L.tx and not L.ft8.get("qso"):
                        break
                    time.sleep(0.5)
                L.send("ft8 stop")
                sends = len(tx_spans(L, t0))
                res.update(replies=sends, qso_after=L.ft8.get("qso"))
        if sends != 3:
            return False, f"with Retries 3 and no answer {l} sent its reply {sends} times", res
        return True, f"{l}'s unanswered reply went out 3 times (Retries 3), then its QSO was dropped", res
    except Refused as e:
        return False, str(e), res


def t_ft8_auto_off(o, s, l):
    """Auto off: l answers s's CQ; s's report to l arrives; l does not move on by itself (its reply repeats) until
    'ft8 reply' on s's answer row moves it to the report"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with ft8_pair(o, "ft8-auto-off") as watch:
            role(o, s)
            n0 = len(L.rows)
            cq = H.ft8_send(o, S, L, watch, "ft8 cq", "ft8-auto-off: CQ", F_FT8)
            L.wait("the CQ", lambda: any(bench.msg(r) == cq for r in L.rows[n0:]), 8)
            row = next(r for r in L.rows[n0:] if bench.msg(r) == cq)
            role(o, l)
            n1 = len(S.rows)
            rep1 = H.ft8_send(o, L, S, watch, f"ft8 reply {row['id']}", "ft8-auto-off: reply", F_FT8)
            S.wait("the reply", lambda: any(bench.msg(r) == rep1 for r in S.rows[n1:]), 8)
            srow = next(r for r in S.rows[n1:] if bench.msg(r) == rep1)
            role(o, s)
            n2 = len(L.rows)
            rpt = H.ft8_send(o, S, L, watch, f"ft8 reply {srow['id']}", "ft8-auto-off: report", F_FT8)
            ca, cb = H.TEST_CALLS[l], H.TEST_CALLS[s]
            # the bench's test calls are non-standard: FT8 sends them hashed, the decoder prints "<Q2TST> <Q1TST> -10"
            plain = lambda m: m.replace("<", "").replace(">", "")      # noqa: E731
            try:
                L.wait("the report", lambda: any(plain(bench.msg(r)) == plain(rpt) or
                                                 (plain(bench.msg(r)).startswith(ca) and cb in bench.msg(r))
                                                 for r in L.rows[n2:]), 12)
            except Refused:
                raise Refused(f"{s} sent {H.mask(rpt)!r}; {l} decoded {[H.mask(bench.msg(r)) for r in L.rows[n2:]]}")
            nxt = bench.norm(L.ft8.get("next") or "")
            res.update(cq=H.mask(cq), reply=H.mask(rep1), report=H.mask(rpt), l_next_unpressed=H.mask(nxt))
            moved = nxt and nxt != rep1
            rrow = next(r for r in L.rows[n2:] if plain(bench.msg(r)) == plain(rpt) or
                        (plain(bench.msg(r)).startswith(ca) and cb in bench.msg(r)))
            L.send(f"ft8 reply {rrow['id']}")
            time.sleep(1.0)
            nxt2 = bench.norm(L.ft8.get("next") or "")
            res["l_next_pressed"] = H.mask(nxt2)
            L.send("ft8 stop")
        if moved:
            return False, f"Auto off, {l} moved on by itself to {res['l_next_unpressed']!r}", res
        if not nxt2 or nxt2 == rep1:
            return False, f"the press on the report row left {l}'s next message {res['l_next_pressed']!r}", res
        return True, f"Auto off: {l} kept {res['reply']!r} until the press, then {res['l_next_pressed']!r}", res
    except Refused as e:
        return False, str(e), res


def t_ft8_compound(o, s, l):
    """A compound own call (Q?TST/6Y): 'ft8 cq' sends 'CQ Q?TST/6Y' (no QTH); one that does not fit FT8 (12
    characters): 'No CQ: callsign ... does not fit FT8', nothing sent"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    res = {}
    try:
        with cfg_kept(S, {"callsign": call + "/6Y"}):
            with ft8_pair(o, "ft8-compound", senders=[s]) as watch:
                role(o, s)
                n0 = len(L.rows)
                sent = H.ft8_send(o, S, L, watch, "ft8 cq", "ft8-compound", F_FT8)
                got = _decoded(L, n0, f"{call}/6Y")
                res.update(sent=sent, decoded=got)
                if f"CQ {call}/6Y" not in got:
                    raise Refused(f"{l} decoded {got}, not 'CQ {call}/6Y'")
                bench.cfg_set(S, "callsign", call + "/6YZ/QRPX")
                t0 = time.time()
                S.send("ft8 cq")
                said = wait_msg(S, t0, r"does not fit|No CQ", 6)
                time.sleep(16)
                res.update(said=said, keyed=len(tx_spans(S, t0)))
                S.send("ft8 stop")
                if res["keyed"]:
                    raise Refused(f"the 12-character call keyed {s} ({said!r})")
        return True, f"'CQ {call}/6Y' decoded by {l}; a call too long refused: {said!r}", res
    except Refused as e:
        return False, str(e), res


def t_ft8_list_look(o, s, l):
    """The FT8 list's columns and the For you strip, from the rows the remote gets (ft8r: d the distance in km, mi
    shown in miles, me addressed to this radio): s's CQ decoded with its SNR; s's 'l s TEST_GRID' (to l, with a
    locator) decoded as l's (me), its distance worked out (d over 0) and shown in km with FT8/FT4 info 0; the same
    again with info 4 (Country + mi): the same distance, shown in miles. The strip turned off by Show held (F4 on page
    1: show_hold_cb, which says it), on again the same way. Before 2026-11-26 the test photographed these and set the
    strip off by its setting, which leaves the pinned row (only the button clears it); Q-calls have no country"""
    S, L = o.r[s], o.r[l]
    res = {}
    msg_l = f"{H.TEST_CALLS[l]} {H.TEST_CALLS[s]} {TEST_GRID}"

    def to_l(watch, info):
        bench.cfg_set(L, "ft8_info", info)
        time.sleep(0.5)
        n0 = len(L.rows)
        ft8_tci_msg(o, S, msg_l, watch)
        end = time.time() + 20
        while time.time() < end:
            row = next((r for r in L.rows[n0:] if bench.msg(r) == msg_l), None)
            if row:
                return {k: row.get(k) for k in ("snr", "d", "mi", "me")}
            time.sleep(0.3)
        raise Refused(f"{l} did not decode {H.mask(msg_l)!r} (info {info})")
    try:
        with cfg_kept(L, {"ft8_info": 0, "ft8_foryou": 1}):
            with ft8_pair(o, "ft8-list-look", senders=[s]) as watch:
                role(o, s)
                n0 = len(L.rows)
                cq = H.ft8_send(o, S, L, watch, "ft8 cq", "ft8-list-look: CQ", F_FT8)
                _decoded(L, n0, cq)
                row = next((r for r in L.rows[n0:] if bench.msg(r) == cq), None)
                res.update(cq=H.mask(cq), snr=row.get("snr") if row else None)
                res["km"] = to_l(watch, 0)
                res["mi"] = to_l(watch, 4)
                snap(o, "ft8-list-look: the row to l, info Country + mi, the For you strip")
                t0 = time.time()
                press(L, "F4", ("FT8",), hold=True)
                res["foryou_off"] = next((t for t in msgs_since(L, t0) if "For you strip" in t), None)
                t1 = time.time()
                press(L, "F4", ("FT8",), hold=True)
                res["foryou_on"] = next((t for t in msgs_since(L, t1) if "For you strip" in t), None)
        bad = []
        if not row or row.get("snr") is None:
            bad.append(f"the CQ not decoded with its SNR ({res.get('cq')})")
        km, mi = res["km"], res["mi"]
        if not km["me"] or not mi["me"]:
            bad.append(f"the message to {l} not marked as its own (me: {km['me']}, {mi['me']})")
        if not km["d"] or km["d"] <= 0 or abs((mi["d"] or 0) - km["d"]) > 2:
            bad.append(f"the distance {km['d']} km, then {mi['d']} (one figure over 0 wanted)")
        if km["mi"] != 0 or mi["mi"] != 1:
            bad.append(f"shown in miles: {km['mi']} with info 0, {mi['mi']} with info 4 (0, then 1 wanted)")
        if not (res["foryou_off"] or "").endswith("off") or not (res["foryou_on"] or "").endswith("on"):
            bad.append(f"Show held said {res['foryou_off']!r}, then {res['foryou_on']!r} (off, then on wanted)")
        if bad:
            return False, "; ".join(bad), res
        return True, f"{l}: the CQ at {res['snr']} dB; the message to it its own, {km['d']} km, shown in km then miles; " \
                     f"Show held: {res['foryou_off']!r}, {res['foryou_on']!r}", res
    except Refused as e:
        return False, str(e), res


def t_ft8_hound(o, s, l):
    """Hound (FT8 page 4): s plays the Fox (a CQ from Q?FOX through its TCI); l with Hound on calls it in the other
    period with an offset of 1000 Hz or more ('Q?FOX <l's call> <grid>')"""
    S, L = o.r[s], o.r[l]
    fox = "Q9FOX"
    res = {}
    try:
        with ft8_pair(o, "ft8-hound") as watch:
            role(o, s)
            n0 = len(L.rows)
            ft8_tci_msg(o, S, f"CQ {fox} {TEST_GRID}", watch)
            S.wait("the Fox off the air", lambda: not S.tx, 5)
            L.wait("the Fox's CQ", lambda: any(fox in bench.msg(r) for r in L.rows[n0:]), 10)
            row = next(r for r in L.rows[n0:] if fox in bench.msg(r))
            if not L.ft8.get("hound"):
                L.send("ft8 hound")                     # page 4's Hound (the remote's "ft8 hound")
                L.wait("Hound on", lambda: L.ft8.get("hound"), 4)
            role(o, l)
            n1 = len(S.rows)
            sent = H.ft8_send(o, L, S, watch, f"ft8 reply {row['id']}", "ft8-hound: the call", F_FT8)
            txf = int(L.ft8.get("txf") or 0)
            res.update(call=H.mask(sent), txf=txf)
        if txf < 1000:
            return False, f"{l}'s Hound called at {txf} Hz, under 1000", res
        return True, f"{l} as a Hound called the Fox: {res['call']!r} at {txf} Hz", res
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            if L.ft8.get("hound"):
                L.send("ft8 hound")
        except Exception:  # noqa: BLE001
            pass


# ---- JS8 ----

def _js8_last(r):
    import suite_air_b as ab
    return ab.last_json(r, "js8")


@contextlib.contextmanager
def js8_pair(o, label, dial=F_JS8, ghostnet=0):
    """The JS8 app open on both on dial (test calls), GhostNet mode as asked on both (on, the band keys step through
    GhostNet's list only: run 30's X6100, left with it on, never came to 14.078), the gates for both; after: stopped,
    closed, both on A, GhostNet mode as found"""
    import suite_air_b as ab
    kept = contextlib.ExitStack()
    try:
        for r in o.r.values():
            kept.enter_context(cfg_kept(r, {"js8_ghostnet": ghostnet}))
        with o.step("both on their test calls, the JS8 app open on both"):
            H.test_calls(o)
            o.changing += 1
            try:
                for r in o.r.values():
                    r.send("js8 open")
                for r in o.r.values():
                    r.wait("the JS8 app", lambda r=r: _js8_last(r).get("open") == 1 or
                           (r.state.get("app") or "").upper().startswith("JS8"), 10)
                time.sleep(1.5)
                for r in o.r.values():                  # one after the other (orch's js8_text: a thread left
                    ab.js8_to(r, dial)                  # stepping a band key moved the X6200 off mid-over)
            finally:
                o.changing -= 1
            o.both({"freq": dial, "pwr": H.W, "agc": 2})   # one AGC (the apps' own differed: 2/0)
        with session(o, label, list(o.r), dial) as watch:
            o.roles = {n: "TX" for n in o.r}            # either may send in JS8 (an answer, a heartbeat)
            yield watch
    finally:
        for r in o.r.values():
            r.send("js8 stop")
            r.send("js8 close")
        for r in o.r.values():
            home(o, r)
        kept.close()


JS8_TEXTS = "/mnt/js8_texts.txt"


@contextlib.contextmanager
def js8_texts(o, r, lines):
    """r's JS8 texts file (GROUPS=, ALERTS=: read at each opening of the app) with the test's lines, before the app
    opens; after it closed: the file as found (none: taken out)"""
    had = ssh(r.host, f"[ -f {JS8_TEXTS} ] && cp {JS8_TEXTS} /tmp/js8_texts.bench && echo had || echo none").strip()
    keys = {ln.split("=", 1)[0] for ln in lines}
    body = "\n".join(lines)
    ssh(r.host, f"touch {JS8_TEXTS}; grep -v -E '^({'|'.join(keys)})=' {JS8_TEXTS} > /tmp/js8_texts.new; "
                f"printf '%s\\n' '{body}' >> /tmp/js8_texts.new; mv /tmp/js8_texts.new {JS8_TEXTS}")
    try:
        yield
    finally:
        ssh(r.host, f"cp /tmp/js8_texts.bench {JS8_TEXTS} && rm -f /tmp/js8_texts.bench" if had == "had"
                    else f"rm -f {JS8_TEXTS}")


def js8_rows(r, t0):
    with r.lock:
        got = [(at, x) for at, x in list(r.lines) if at >= t0 and x.startswith("js8r ")]
    out = []
    for at, x in got:
        try:
            out.append(json.loads(x[5:]))
        except ValueError:
            pass
    return out


def js8_send(o, S, text, watch, secs=60):
    """S sends text (its JS8 app, "js8 send"), every frame of it (a frame a slot): (start, end) once no frame came
    for a slot"""
    t0 = time.time()
    bench.tx_log({"who": S.name, "f": int(S.state.get("freq", 0)), "w": S.pwr(), "what": f"orch: js8 {text}"})
    S.send(f"js8 send {text}")
    S.wait("its JS8 over", lambda: S.tx or watch.why, secs)
    end = time.time() + 90
    while time.time() < end and not watch.why:
        S.wait("the frame's end", lambda: not S.tx or watch.why, 20)
        try:
            S.wait("its next frame", lambda: S.tx or watch.why, 17)
        except Refused:
            break
    if watch.why:
        raise Refused(watch.why)
    return t0, time.time()


def _js8_heard(L, t0, needle, secs=20, fr=None):
    """A message L's JS8 list shows since t0 with needle in its text (bench.Radio.js8_hist: the remote's lines keep
    about a minute)"""
    end = time.time() + secs
    while True:
        with L.lock:
            got = [m for at, m in L.js8_hist if at >= t0]
        for m in got:
            if (not fr or str(m.get("fr", "")).upper() == fr.upper()) and needle.upper() in (m.get("x") or "").upper():
                return m
        if time.time() >= end:
            return None
        time.sleep(0.3)


def t_js8_speed(o, s, l):
    """Speed and Decode: s at Turbo; l at Normal with Decode All speeds hears it; with My speed (set before JS8 opens
    again: the app takes it at its opening or its own Decode key) it does not"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with cfg_kept(S, {"js8_speed": 2}), cfg_kept(L, {"js8_speed": 0, "js8_rx_all": 1}):
            with js8_pair(o, "js8-speed: all speeds") as watch:
                role(o, s)
                t0 = time.time()
                js8_send(o, S, f"{H.TEST_CALLS[l]} SPEEDTEST ALL", watch)
                heard_all = _js8_heard(L, t0, "SPEEDTEST")
            bench.cfg_set(L, "js8_rx_all", 0)
            with js8_pair(o, "js8-speed: my speed") as watch:
                role(o, s)
                t1 = time.time()
                js8_send(o, S, f"{H.TEST_CALLS[l]} SPEEDTEST MINE", watch)
                heard_mine = _js8_heard(L, t1, "SPEEDTEST", 15)
            res.update(all=bool(heard_all), mine=bool(heard_mine))
        if not heard_all:
            return False, f"{l} with All speeds did not hear {s}'s Turbo", res
        if heard_mine:
            return False, f"{l} with My speed (Normal, set before the app opened) heard {s}'s Turbo too", res
        return True, f"{s}'s Turbo heard by {l} with All speeds, not with My speed", res
    except Refused as e:
        return False, str(e), res


def t_js8_groups(o, s, l):
    """Groups: l's Groups hold @X6100 and @GSTFLASH (its JS8 texts' GROUPS=, before the app opens). s's '@X6100 HELLO'
    shows on l as a group message (its row's grp) and l never transmits for it, even with AUTO on; s's
    '@GSTFLASH TEST' puts l's 'EMERGENCY @GSTFLASH' notice up (a message the remote gets) and l stays off the air.
    Before 2026-11-26 the test left @GSTFLASH out of Groups, and the GUI rightly showed nothing for it"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        # AUTO on once the app is open: opening it turns AUTO, HB and HB ACK off (js8_window.c construct_cb)
        with js8_texts(o, L, ["GROUPS=@X6100 @GSTFLASH"]), js8_pair(o, "js8-groups") as watch, \
                cfg_kept(L, {"js8_auto": 1}):
            for text, key in (("@X6100 HELLO", "x6100"), ("@GSTFLASH TEST", "gstflash")):
                role(o, s)
                t0 = time.time()
                js8_send(o, S, text, watch)
                m = _js8_heard(L, t0, text.split()[1])
                snap(o, f"js8-groups {l}: {text}")
                time.sleep(16)
                notice = next((t for t in msgs_since(L, t0) if "EMERGENCY @GSTFLASH" in t.upper()), None)
                res[key] = {"heard": bool(m), "grp": (m or {}).get("grp"), "to": (m or {}).get("to"),
                            "notice": notice, "l_keyed": len(tx_spans(L, t0))}
                if not m:
                    raise Refused(f"{l} did not show {text!r}")
                if res[key]["l_keyed"]:
                    raise Refused(f"{l} transmitted after {text!r} (AUTO on)")
        bad = []
        if not res["x6100"]["grp"]:
            bad.append(f"'@X6100 HELLO' not shown as a group message ({res['x6100']})")
        if not res["gstflash"]["notice"]:
            bad.append(f"no 'EMERGENCY @GSTFLASH' notice for '@GSTFLASH TEST' ({res['gstflash']})")
        if bad:
            return False, "; ".join(bad), res
        return True, f"{l}: '@X6100 HELLO' a group message, '@GSTFLASH TEST' its notice " \
                     f"{res['gstflash']['notice'][:60]!r}; off the air both times (AUTO on)", res
    except Refused as e:
        return False, str(e), res


GHOSTNET_HZ = 7107000


def t_ghostnet(o, s, l):
    """GhostNet on 7.107: s's '@GHOSTNET SNR?' answered by l (AUTO on, in GhostNet's group); l's
    /mnt/ghostnet_log.txt keeps s's '@GHOSTNET TEST'"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with js8_texts(o, L, ["GROUPS=@GHOSTNET @X6100", "X6100GROUP=1"]):
            # AUTO on once the app is open: opening it turns AUTO off (js8_window.c construct_cb)
            with js8_pair(o, "ghostnet", dial=GHOSTNET_HZ, ghostnet=1) as watch, cfg_kept(L, {"js8_auto": 1}):
                # both may send: l answers in the slot right after the query's last frame, while js8_send still
                # waits for a next frame of s's (run 28: the guard stopped l's answer, its role still RX)
                o.roles = {s: "TX", l: "TX"}
                t0 = time.time()
                js8_send(o, S, "@GHOSTNET SNR?", watch)
                try:
                    L.wait("l's answer", lambda: tx_spans(L, t0) or watch.why, 30)
                    L.wait("its end", lambda: not L.tx or watch.why, 20)
                    res["answered"] = bool(tx_spans(L, t0))
                except Refused:
                    res["answered"] = False
                    snap(o, f"ghostnet {l}: after '@GHOSTNET SNR?' (an answer offered, not sent?)")
                res["decoded_query"] = bool(_js8_heard(L, t0, "SNR?", 2))
                ans = _js8_heard(S, t0, "SNR", 20, fr=H.TEST_CALLS[l]) if res["answered"] else None
                res["answer_heard_by_s"] = (ans or {}).get("x")
                role(o, s)
                t1 = time.time()
                js8_send(o, S, "@GHOSTNET TEST", watch)
                time.sleep(4)
                log = ssh(L.host, "tail -20 /mnt/ghostnet_log.txt 2>/dev/null")
                res["logged"] = "@GHOSTNET TEST" in log.upper() or "GHOSTNET TEST" in log.upper()
        if not res["answered"]:
            if res["decoded_query"] and res.get("logged"):
                return None, f"{l} decoded '@GHOSTNET SNR?' and logged '@GHOSTNET TEST', but sent no answer with AUTO " \
                             f"on (an answer offered on its screen? look at the picture)", res
            return False, f"{l} (AUTO on, GhostNet) did not answer '@GHOSTNET SNR?' (decoded it: " \
                          f"{res['decoded_query']})", res
        if not res["logged"]:
            return False, f"{l}'s ghostnet_log.txt has no '@GHOSTNET TEST'", res
        return True, f"{l} answered '@GHOSTNET SNR?' ({s} decoded {res['answer_heard_by_s']!r}) and logged " \
                     f"'@GHOSTNET TEST'", res
    except Refused as e:
        return False, str(e), res


def t_js8_look(o, s, l):
    """JS8: s's directed 'l TESTWORD HELLO' raises l's alert ('Alert TESTWORD: ...', TESTWORD in its ALERTS=); l
    answers s (the QSO two-way: the log offers only then), s's 73 puts up l's 'QSO with s ended - Save to log?' (Log
    prompt on), the MFK pressed on Save says 'Logged s' and js8call_log.adi gets the QSO (s's call, MFSK JS8) in what
    it wrote since the test began. Before 2026-11-26 l never answered, so no popup could come, and pictures were the
    proof"""
    S, L = o.r[s], o.r[l]
    call_s, call_l = H.TEST_CALLS[s], H.TEST_CALLS[l]
    res = {}
    adi = "/mnt/js8call_log.adi"
    try:
        adi0 = int(ssh(L.host, f"wc -c < {adi} 2>/dev/null || echo 0").strip() or 0)
        with cfg_kept(L, {"js8_log_prompt": 1, "js8_log_activation": 1}), js8_texts(o, L, ["ALERTS=TESTWORD"]):
            with js8_pair(o, "js8-look") as watch:
                role(o, s)
                t0 = time.time()
                js8_send(o, S, f"{call_l} TESTWORD HELLO", watch)
                _js8_heard(L, t0, "TESTWORD")
                res["alert"] = next((t for t in msgs_since(L, t0) if t.startswith("Alert TESTWORD")), None)
                role(o, l)
                t1 = time.time()
                js8_send(o, L, f"{call_s} HELLO", watch)
                res["s_heard_l"] = bool(_js8_heard(S, t1, "HELLO"))
                role(o, s)
                t2 = time.time()
                js8_send(o, S, f"{call_l} 73", watch)
                m73 = _js8_heard(L, t2, " 73") or {}
                res["row_73"] = {k: m73.get(k) for k in ("fr", "to", "lc", "chk", "part", "x")}
                end = time.time() + 8              # the offer, the popup or (l busy) the line saying Log QSO
                while time.time() < end and not any("ended" in t for t in msgs_since(L, t2)):
                    time.sleep(0.3)
                res["l_said"] = msgs_since(L, t2)[-6:]
                res["popup"] = next((t for t in msgs_since(L, t2) if "Save to log?" in t), None)
                snap(o, "js8-look: the log popup at 73 on l")
                if res["popup"]:
                    t3 = time.time()
                    L.take_control()
                    L.send("key MFK 1")
                    time.sleep(0.15)
                    L.send("key MFK 0")
                    time.sleep(2.0)
                    res["logged"] = next((t for t in msgs_since(L, t3) if t.startswith(("Logged", "Not logged"))), None)
                new = ssh(L.host, f"tail -c +{adi0 + 1} {adi} 2>/dev/null; true")
                res.update(adi_new=len(new), adi_has_call=call_s in new, adi_js8="JS8" in new.upper())
        bad = []
        if not res["alert"]:
            bad.append(f"no 'Alert TESTWORD' on {l} ({msgs_since(L, t0)[-3:]})")
        if not res["s_heard_l"]:
            bad.append(f"{s} did not hear {l}'s answer: the QSO not two-way")
        if not res["popup"]:
            bad.append(f"no 'Save to log?' on {l} after {call_s}'s 73 (it said {res['l_said']})")
        elif not (res.get("logged") or "").startswith(f"Logged {call_s}"):
            bad.append(f"Save said {res.get('logged')!r}")
        if not (res["adi_has_call"] and res["adi_js8"]):
            bad.append(f"js8call_log.adi's new {res['adi_new']} bytes: {call_s} {res['adi_has_call']}, JS8 "
                       f"{res['adi_js8']}")
        if bad:
            return False, "; ".join(bad), res
        return True, f"{l}: 'Alert TESTWORD' up; after the QSO {res['popup']!r}, Save: {res['logged']!r}, the ADIF " \
                     f"record written ({call_s}, JS8)", res
    except Refused as e:
        return False, str(e), res


def t_js8_time_sync(o, s, l):
    """Time Sync needs l's clock 1 s off with NTP stopped (then put back): the owner's word first (the radios' clock
    and NTP; gpsd is fed from them). Refused without ~/.config/x6x00-bench/owner_ok_clock"""
    if not os.path.exists(os.path.join(bench.STATE, "owner_ok_clock")):
        raise Refused("needs the owner's word: a radio's clock set 1 s off with NTP stopped (owner_ok_clock)")
    return None, "the owner's word is there; the clock test itself is not written yet", {}


def _same_dial(o, S, L, app):
    """Both apps opened where their memory (/mnt/app_freqs) says, which an app holds (the remote's set freq refused
    under it): on one dial, else Refused; the power set"""
    f, m = int(L.state.get("freq", 0)), int(L.state.get("mode", -1))
    if (int(S.state.get("freq", 0)), int(S.state.get("mode", -1))) != (f, m):
        raise Refused(f"the two {app} apps on two dials ({S.name} {S.state.get('freq')}, {L.name} {f}): each opens "
                      f"where its memory (/mnt/app_freqs) says")
    o.both({"pwr": H.W})


# ---- SSTV: the app's own TX (Robot 8: under the Watch's 16 s), the listener's pictures ----

SSTV_ROBOT8 = 1                 # sstv_mode.h: the TX Mode as params.sstv_tx_mode stores it


def _sstv_pics(r):
    return set(ssh(r.host, "ls /mnt/sstv/*.png 2>/dev/null; true").split())


def _sstv_tx_menu(S, row, watch, picks=6):
    """S's SSTV TX (page 1 F5) menu at row (0 CALL CQ, 3 TX IMAGE ONLY, 4 REPEAT LAST TX), then MFK presses through
    its steps (picture, colours, text) until S is on the air; the time it keyed"""
    press(S, "F5", ("SSTV",), rest=1.0)
    S.send("knob mfk -10")
    time.sleep(0.5)
    if row:
        S.send(f"knob mfk {row}")
        time.sleep(0.5)
    for _ in range(picks):
        S.send("key MFK 1")
        time.sleep(0.15)
        S.send("key MFK 0")
        try:
            S.wait("its picture on the air", lambda: S.tx or watch.why, 3)
            return time.time()
        except Refused:
            continue
    raise Refused(f"{S.name}'s SSTV TX did not start after {picks} presses through its menu")


@contextlib.contextmanager
def sstv_pair(o, label, s, l):
    """Both SSTV apps open on one dial (the listener's), the sender's TX Mode Robot 8; after: closed, both on A, the
    test's pictures taken off the listener's card"""
    import suite_more
    S, L = o.r[s], o.r[l]
    before = _sstv_pics(L)
    try:
        with cfg_kept(S, {"sstv_tx_mode": SSTV_ROBOT8}):
            with o.step("both SSTV apps open, one dial"):
                o.changing += 1
                try:
                    for r in (L, S):
                        if not suite_more.open_app(r, "sstv"):
                            raise Refused(f"{r.name}'s SSTV app did not open")
                        time.sleep(1.0)
                    _same_dial(o, S, L, "SSTV")
                finally:
                    o.changing -= 1
            with session(o, label, [s]) as watch:
                role(o, s)
                yield watch, before
    finally:
        new = _sstv_pics(L) - before
        if new:
            ssh(L.host, "rm -f " + " ".join(new) + " " + " ".join(p.replace(".png", "_dn.png") for p in new))
        for r in (S, L):
            home(o, r)


def t_sstv_rx(o, s, l):
    """Manual RX (Robot 8) started on l just before s's CQ picture: a picture saved, with Denoise on its _dn.png beside
    it; with Denoise off the next picture alone"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with cfg_kept(L, {"sstv_denoise": 1}):
            with sstv_pair(o, "sstv-rx", s, l) as (watch, before):
                press(L, "F4", ("SSTV",), rest=0.8)          # RX Mode: Auto -> Robot 8
                press(L, "F3", ("SSTV",), rest=0.8)          # Start Manual RX
                _sstv_tx_menu(S, 0, watch)
                S.wait("the picture's end", lambda: not S.tx or watch.why, 20)
                time.sleep(4)
                new1 = _sstv_pics(L) - before
                res["denoise_on"] = sorted(os.path.basename(p) for p in new1)
                bench.cfg_set(L, "sstv_denoise", 0)
                press(L, "F2", ("SSTV",), rest=0.8)          # Start Auto RX (Manual's key would stop it if it waits)
                _sstv_tx_menu(S, 4, watch)                   # REPEAT LAST TX
                S.wait("the picture's end", lambda: not S.tx or watch.why, 20)
                time.sleep(4)
                new2 = _sstv_pics(L) - before - new1
                res["denoise_off"] = sorted(os.path.basename(p) for p in new2)
        dn1 = [p for p in res["denoise_on"] if p.endswith("_dn.png")]
        if len(res["denoise_on"]) < 2 or not dn1:
            return False, f"Manual RX with Denoise on saved {res['denoise_on']} (a picture and its _dn wanted)", res
        if len(res["denoise_off"]) != 1:
            return False, f"Denoise off saved {res['denoise_off']} (one picture wanted)", res
        return True, f"Manual RX saved {res['denoise_on']} (Denoise on), then {res['denoise_off']} (off; the repeat)", res
    except Refused as e:
        return False, str(e), res


def t_sstv_image_only(o, s, l):
    """TX IMAGE ONLY, then REPEAT LAST TX: l (Auto RX) saves two pictures, alike"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with sstv_pair(o, "sstv-image-only", s, l) as (watch, before):
            press(L, "F2", ("SSTV",), rest=0.8)              # Start Auto RX
            for row in (3, 4):
                _sstv_tx_menu(S, row, watch)
                S.wait("the picture's end", lambda: not S.tx or watch.why, 20)
                time.sleep(4)
            new = sorted(p for p in _sstv_pics(L) - before if not p.endswith("_dn.png"))
            res["pictures"] = [os.path.basename(p) for p in new]
            if len(new) < 2:
                raise Refused(f"{l} saved {res['pictures']} (two wanted)")
            res["alike"] = _pngs_alike(L, new[-2], new[-1])
        if res["alike"] is False:
            return False, f"the image-only picture and its repeat differ ({res})", res
        return True, f"{l} saved the image-only picture and the repeat ({res['pictures'][-2:]}), alike", res
    except Refused as e:
        return False, str(e), res


def _pngs_alike(r, a, b):
    import io
    from PIL import Image
    import numpy as np
    try:
        ia = Image.open(io.BytesIO(subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", f"root@{r.host}", f"cat {a}"],
                                                  capture_output=True, timeout=30).stdout)).convert("L")
        ib = Image.open(io.BytesIO(subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", f"root@{r.host}", f"cat {b}"],
                                                  capture_output=True, timeout=30).stdout)).convert("L")
    except Exception:  # noqa: BLE001
        return None
    if ia.size != ib.size:
        ib = ib.resize(ia.size)
    d = np.abs(np.asarray(ia, dtype=float) - np.asarray(ib, dtype=float)).mean()
    return bool(d < 40)


def t_sstv_halt_hold(o, s, l):
    """While s's picture goes out: the main knob, BAND_UP and A/B leave its frequency, band and mode; Halt TX (page 1
    F5) takes it off the air at once"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        with sstv_pair(o, "sstv-halt-hold", s, l) as (watch, before):
            press(L, "F2", ("SSTV",), rest=0.8)
            f0, m0 = int(S.state.get("freq", 0)), int(S.state.get("mode", -1))
            _sstv_tx_menu(S, 0, watch)
            time.sleep(1.5)
            S.send("knob main 10")
            time.sleep(0.6)
            S.send("key BAND_UP 1")
            time.sleep(0.15)
            S.send("key BAND_UP 0")
            time.sleep(0.8)
            f1, m1 = int(S.state.get("freq", 0)), int(S.state.get("mode", -1))
            res.update(held=(f1, m1) == (f0, m0), during=(f1, m1))
            t_halt = time.time()
            press(S, "F5", ("SSTV",), rest=0.1)              # Halt TX
            S.wait("off the air at once", lambda: not S.tx, 2.0)
            res["halt_s"] = round(time.time() - t_halt, 2)
        if not res["held"]:
            return False, f"during the picture {s} moved to {res['during']} from {(f0, m0)}", res
        return True, f"{s}'s frequency and mode held through the knob and BAND_UP; Halt TX off the air in " \
                     f"{res['halt_s']} s", res
    except Refused as e:
        return False, str(e), res


# ---- WeFax: the fax drawn inverted on the screen, the PNG black on white; the tilt ----

def _png_mean(r, path):
    import io
    from PIL import Image
    import numpy as np
    data = subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", f"root@{r.host}", f"cat '{path}'"], capture_output=True,
                          timeout=30).stdout
    im = Image.open(io.BytesIO(data)).convert("L")
    return float(np.asarray(im, dtype=float).mean()), im


WEFAX_PAPER_MIN = 150            # the saved PNG's paper (its clean rows' mean, three quarters white) at least this light
WEFAX_INK_MIN_PX = 800           # the fax area's amber more than on the app's empty screen by this much (its ink
                                 # drawn amber: a 12 s chart is a strip about 14 px tall, its black quarter ~2500 px)
WEFAX_WHITE_MAX_PX = 100         # and near-white pixels in it at most this many (paper drawn white: not inverted)
WEFAX_AREA = (172, 392, 18, 778)  # the WeFax view's fax area in the 800x480 picture: rows, then columns


def _fax_edges(pic):
    """A saved fax's chart rows: (row, the edge's column) for each row of light paper (mean over WEFAX_PAPER_MIN - 30:
    the noise before the chart left out) whose white-to-black step (the steepest fall, 30 px from either border: the
    smoothing's own fall at the ends was taken for it) lies within 30 px of the rows' median; and their mean
    brightness"""
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(pic).convert("L"), dtype=float)
    k = np.ones(25) / 25
    edges = []
    for i, r in enumerate(a):
        if r.mean() < WEFAX_PAPER_MIN - 30:
            continue
        sm = np.convolve(r, k, mode="same")[30:-30]
        edges.append((i, 30 + int(np.argmin(np.diff(sm)))))
    if not edges:
        return [], None
    med = float(np.median([c for _, c in edges]))
    clean = [e for e in edges if abs(e[1] - med) <= 30]
    paper = float(np.mean([a[i].mean() for i, _ in clean])) if clean else None
    return clean, paper


def _screen_colours(pic):
    """(amber, near-white) pixel counts in the WeFax view's fax area (WEFAX_AREA)"""
    import numpy as np
    from PIL import Image
    y0, y1, x0, x1 = WEFAX_AREA
    a = np.asarray(Image.open(pic).convert("RGB"), dtype=int)[y0:y1, x0:x1]
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    amber = int(((r > 50) & (r - b > 40)).sum())     # the fax's ink is a dim amber (95, 70, 14 on 3, 3, 5 paper)
    white = int(((r > 200) & (g > 200) & (b > 200)).sum())
    return amber, white


def t_wefax_look(o, s, l):
    """WeFax's looks, judged (before 2026-11-26 photographed after the save had cleared the screen). s sends a chart
    three quarters white per line, l at Tilt 0:
    - the saved PNG is black on white: its chart rows' mean at least WEFAX_PAPER_MIN;
    - the screen shows it inverted, ink amber on dark paper (photographed before the save): amber up by
      WEFAX_INK_MIN_PX against l's screen before the fax, near-white at most WEFAX_WHITE_MAX_PX.
    Tilt is not judged here: at its +-400 ends it moves a 12 s chart's edge about 4 px over its 24 rows, under the
    rows' own +-6 px (2026-11-26). tools/wefax_plan's host test checks it, a known tilt found again within 2"""
    L = o.r[l]
    res = {}
    try:
        shots = {}

        def after_clear():
            shots["blank"] = snap(o, f"wefax-look {l}: its WeFax screen empty, before the fax").get(l)

        def before_save():
            shots["fax"] = snap(o, f"wefax-look {l}: the fax on its screen before the save").get(l)
        with cfg_kept(L, {"wefax_tilt": 0}):
            ok, says, data = H.wefax_chart(o, s, l, "wefax-look", kind="chart_q", before_save=before_save,
                                           after_clear=after_clear)
        pic = (data or {}).get("picture")
        if not ok or not pic or not os.path.exists(pic):
            raise Refused(f"no chart ({says})")
        clean, paper = _fax_edges(pic)
        res.update(chart_rows=len(clean), paper=round(paper, 1) if paper is not None else None, picture=pic)
        if shots.get("blank") and shots.get("fax"):
            res["screen"] = {"blank": _screen_colours(shots["blank"]), "fax": _screen_colours(shots["fax"])}
        bad = []
        if len(clean) < 6 or paper is None or paper < WEFAX_PAPER_MIN:
            bad.append(f"the saved PNG: {len(clean)} chart rows, paper {res['paper']} (6 rows, at least "
                       f"{WEFAX_PAPER_MIN} wanted)")
        sc = res.get("screen")
        if not sc:
            bad.append("the screen not photographed before and with the fax")
        else:
            ink = sc["fax"][0] - sc["blank"][0]
            res["ink_px"] = ink
            if ink < WEFAX_INK_MIN_PX or sc["fax"][1] > WEFAX_WHITE_MAX_PX:
                bad.append(f"the screen's fax not inverted: amber {sc['blank'][0]} -> {sc['fax'][0]}, near-white "
                           f"{sc['fax'][1]} (ink up {WEFAX_INK_MIN_PX}, white at most {WEFAX_WHITE_MAX_PX} wanted)")
        if bad:
            return False, "; ".join(bad), res
        return True, f"the PNG black on white ({len(clean)} chart rows, paper {res['paper']}); the screen inverted " \
                     f"(amber +{res['ink_px']}, near-white {sc['fax'][1]})", res
    except Refused as e:
        return False, str(e), res


# ---- PSK31: the DX call from 'DE <call>', the status and text buttons (pictures), the cursor ----

PSK_TX_HZ = 1000          # psk-look: the sender's PSK cursor (its audio frequency)
PSK_HZ_TOL = 15           # the listener's status within this of it (the two radios ~7 Hz apart)
PSK_Q_MIN = 70            # and its quality at least this over the cable


def t_psk_look(o, s, l):
    """PSK31: s's macro 'CQ CQ DE <call> <call> K' (added on its web page, taken out after); l's text log has it with
    the status's audio frequency (PSK_TX_HZ, within PSK_HZ_TOL) and quality (PSK_Q_MIN or better), and its DX call
    takes the call (the remote's read-only digi_dx, from 2026-11-26; the button is on page 2, never in the pictures)"""
    import suite_air_b as ab
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    text = f"CQ CQ DE {call} {call} K"
    res = {}
    ip = ab.web_ip(S.host)
    mid = None
    tl0 = ab.tl_sizes(L.host)
    try:
        st, b_ = ab.http(ip, "/api/macros/psk", method="POST", body={"name": "BENCH", "text": text})
        if st != 200:
            raise Refused(f"{s}: the PSK macro not added ({st})")
        listed = json.loads(b_)["macros"]
        i_m = max(i for i, m in enumerate(listed) if m.get("text") == text)
        mid = listed[i_m].get("id")
        with o.step("both PSK31 apps open on one dial"):
            o.changing += 1
            try:
                o.both_page("G")
                for r in (L, S):
                    ab.open_listed(r, "psk31", "psk")
                _same_dial(o, S, L, "PSK31")
                # every setting the Watch compares made alike: each app's filter is its own radio's (the X6100's
                # USB-D from 100 Hz, the X6200's from 200) and the Watch stopped the over for it (2026-11-26)
                bench.pair_align(S, L)
            finally:
                o.changing -= 1
        t0 = time.time()
        with session(o, "psk-look", [s]) as watch:
            role(o, s)
            ab.Pages(S).macro(i_m)
            S.wait("its PSK on the air", lambda: S.tx or watch.why, 8)
            time.sleep(3)
            snap(o, "psk-look: l's status button (audio frequency, quality) during s's PSK")
            S.wait("its end", lambda: not S.tx or watch.why, 30)
        time.sleep(2)
        snap(o, "psk-look: l's DX call button after 'DE <call>'")
        lines = H.textlog_lines(L.host, tl0)
        res["textlog"] = str(lines)[-120:]
        if call not in str(lines):
            return False, f"{l}'s text log has no {call} ({res['textlog']!r})", res
        # the status button's audio frequency and quality, as the text log keeps them with each line: '<date> <time>
        # <MHz> PSK31 RX Q<n>% <text>' (the dial plus the audio); the sender's cursor at PSK_TX_HZ
        dial = int(L.state.get("freq") or 0)
        m = next((re.search(r"\s(\d+\.\d{6})\s+PSK\w*\s+RX\s+Q(\d+)%", x) for x in lines if call in x), None)
        if not m:
            return False, f"{l}'s text log line has no frequency and quality: {res['textlog']!r}", res
        res.update(audio_hz=round(float(m.group(1)) * 1e6) - dial, quality=int(m.group(2)))
        res["dx"] = bench.cfg_get(L, "digi_dx")
        bad = []
        if abs(res["audio_hz"] - PSK_TX_HZ) > PSK_HZ_TOL:
            bad.append(f"the status's audio {res['audio_hz']} Hz, not within {PSK_HZ_TOL} of {PSK_TX_HZ}")
        if res["quality"] < PSK_Q_MIN:
            bad.append(f"the quality {res['quality']}% (at least {PSK_Q_MIN} wanted over the cable)")
        if str(res["dx"]).upper() != call:
            bad.append(f"the DX call {res['dx']!r}, not {call}")
        if bad:
            return False, "; ".join(bad), res
        return True, f"{l} decoded {call} from s's macro: the status {res['audio_hz']} Hz Q {res['quality']}%, the DX " \
                     f"call {res['dx']}", res
    except Refused as e:
        return False, str(e), res
    finally:
        H.textlog_back(L.host, tl0)
        if mid is not None:
            ab.http(ip, f"/api/macros/psk/{mid}", method="DELETE")
        for r in (S, L):
            home(o, r)


# ---- RADE: a recorded over (the hand-mic session's) sent by s's RADE app, decoded by l ----

def t_rade_rx(o, s, l):
    """RADE V1: s's RADE app sends its recorded over (Rec from the hand mic: the owner's hand-mic session makes one)
    at its own power; l's RADE shows Sync, SNR and 'last over <call>' (picture), its TCI audio is speech. Refused
    without a recording on s"""
    S = o.r[s]
    have = ssh(S.host, "ls /mnt/rade/*.raw /mnt/rade/*.wav /tmp/rade_over* 2>/dev/null | head -1").strip()
    if not have:
        raise Refused(f"{s} has no recorded RADE over: the hand-mic session (Rec) first")
    return None, f"a recording is there ({have}); the RADE send/decode steps are not written yet", {"file": have}


# ---- TX settings: the transverter's limit, the 0 W warning, AGC, CESSB, Moni, the mic, the TX panel ----

XV_DIAL = 144174000
XV_SHIFT = 116000000            # 144.174 on a 28.174 MHz IF


def t_transverter(o, s, l):
    """Transverter 0 (144-146 MHz, IF 28 MHz) with a 0.1 W limit, the sender's PWR 1 W: a TCI tone from 144.174 goes
    out at 0.1 W or less (its meters) and l hears it on the 28.174 IF; the ATU's long press there: 'No tuner: ...
    transverter', nothing keyed"""
    S, L = o.r[s], o.r[l]
    f_if = XV_DIAL - XV_SHIFT
    res = {}
    try:
        on_g_alike(o, f_if, 2)
        ref = {}

        def ref_body(watch):                        # the same tone at 0.1 W on the IF itself: the meters' reference
            _, t0, t1 = bench.tci_send(S, [[1000, 4.0]], watch=watch)
            ref["t"] = (t0, t1)
        offset_over(o, s, l, f_if, 2, "transverter: the 0.1 W reference on the IF", ref_body)
        rw = [x[1] for x in t_frames(S, *ref["t"])]
        rm = meters(L, ref["t"][0] + 1, ref["t"][1])
        res.update(ref_watts=max(rw) if rw else None, ref_l_meter=round(sum(rm) / len(rm), 1) if rm else None)
        with cfg_kept(S, {"transverter_0_from": 144000000, "transverter_0_to": 146000000,
                          "transverter_0_shift": XV_SHIFT, "transverter_0_pwr_max": 0.1}):
            with bench.apart({"freq": (XV_DIAL, f_if)}, f"transverter: {s} on 144.174 through its 28.174 IF"):
                p0 = S.pwr()
                try:
                    got = {}

                    def body(watch):
                        S.send("set pwr 10")            # 1 W asked: the limit holds PWR itself (pwr_xvtr)
                        time.sleep(1.0)
                        got["pwr_read"] = S.pwr()
                        _, t0, t1 = bench.tci_send(S, [[1000, 4.0]], watch=watch)
                        got["t"] = (t0, t1)
                    offset_over(o, s, l, XV_DIAL, 2, "transverter: the tone through the limit", body, rf_hz=f_if)
                    t0, t1 = got["t"]
                    w = [x[1] for x in t_frames(S, t0, t1 + 1)]
                    m = meters(L, t0 + 1, t1)
                    res.update(watts=max(w) if w else None, l_meter_max=max(m) if m else None,
                               l_meter=round(sum(m) / len(m), 1) if m else None,
                               pwr_asked_1w_reads=got.get("pwr_read"))
                    with o.step(f"{s}: the ATU's long press on the limited band") as st:
                        classic(S)
                        t2 = time.time()
                        S.send("key ATU 1")
                        time.sleep(1.5)
                        S.send("key ATU 0")
                        said = wait_msg(S, t2, r"No tuner", 4)
                        res.update(atu_said=said, atu_keyed=len(tx_spans(S, t2)))
                        st.confirm(said)
                finally:
                    bench.ensure(S, {"pwr": p0})
                    home(o, S)
                    bench.ensure(S, {"freq": f_if})
        if res["pwr_asked_1w_reads"] is None or res["pwr_asked_1w_reads"] > 0.15:
            return False, f"PWR 1 W asked on the limited band reads {res['pwr_asked_1w_reads']} W", res
        # the X6100's meter reads 0.2-0.5 W at a 0.1 W setting: judged against the reference over, the listener's
        # S-meter the measure
        if res["l_meter"] is None or res["ref_l_meter"] is None:
            return False, f"no S-meter from {l} ({res})", res
        if res["l_meter"] > res["ref_l_meter"] + 2.0:
            return False, f"through the 0.1 W limit {l} heard {res['l_meter']} dB, over the 0.1 W reference's " \
                          f"{res['ref_l_meter']} dB (meters {res['watts']} W against {res['ref_watts']} W)", res
        if res["atu_keyed"]:
            return False, f"the ATU keyed {s} on the limited band ({res['atu_said']!r})", res
        return True, f"limit held: PWR 1 W asked reads {res['pwr_asked_1w_reads']} W; {l} heard {res['l_meter']} dB " \
                     f"against the 0.1 W reference's {res['ref_l_meter']} dB (meters {res['watts']} W, reference " \
                     f"{res['ref_watts']} W); ATU: {res['atu_said']!r}", res
    except Refused as e:
        return False, str(e), res


ZERO_POWER_SEEN_W = 0.2   # zero-power: an over at least this strong is no 0 W over


def t_zero_power(o, s, l):
    """Line-in 0 (put back after), PWR 1 W, one FT8 CQ. Line-in is the codec's input gain, 0.5 dB a step: 0 is 5 dB
    under the default, not off, so power still goes out (0.4 W from the X6100, 1.0 from the X6200, 2026-11-25): no 'TX
    0 W' may be said for it (no false alarm). An over that did go out at 0 W must be said ('TX 0 W: level ..., Line-in
    ...'); a true 0 W over cannot be made safely on the air, so the warning itself is tools/txlevel's dead-PA case"""
    S = o.r[s]
    res = {}
    li0 = int(S.state.get("linein", 10))
    p0 = None
    try:
        S.send("set linein 0")
        S.wait("Line-in 0", lambda: int(S.state.get("linein", -1)) == 0, 4)
        p0 = S.pwr()
        with ft8_pair(o, "zero-power", senders=[s], w=1.0) as watch:
            role(o, s)
            t0 = time.time()
            S.send("ft8 cq")
            S.wait("its over", lambda: S.tx or watch.why, 32)
            S.wait("its end", lambda: not S.tx or watch.why, 18)
            S.send("ft8 stop")
            time.sleep(6)
            w = [x[1] for x in t_frames(S, t0)]
            said = [m for m in msgs_since(S, t0) if "TX 0 W" in m]
            res.update(said=said, watts=max(w) if w else 0.0)
        if res["watts"] >= ZERO_POWER_SEEN_W:
            if said:
                return False, f"{res['watts']} W went out at PWR 1 W, yet the radio said {said[0]!r}", res
            return True, f"Line-in 0 (5 dB down, not off) still sent {res['watts']} W at PWR 1 W, and no 'TX 0 W' was " \
                         f"said (the warning itself: tools/txlevel's dead-PA case)", res
        if not said:
            return False, f"a 0 W over ({res['watts']} W at PWR 1 W) and no 'TX 0 W' message", res
        return True, f"0 W over with Line-in 0, the radio said {said[0]!r}", res
    except Refused as e:
        return False, str(e), res
    finally:
        S.send(f"set linein {li0}")
        if p0 is not None:
            bench.ensure(S, {"pwr": p0})


def _rx_level_db(L, t0, t1):
    from tones import TciRx
    return None


AGC_STEP_MIN_DB = 8       # agc-knee: the listener's S-meter rises at least this from 0.1 to 1 W (the input stepped)
AGC_PEAK_MAX_DBFS = -3    # and its loudest step's audio peaks under this (room for a slope to show)
AGC_SLOPE_MIN_DB = 3      # Slope 0 and 10 grow the audio this differently at least (10x the 0.2-0.3 dB between runs)


def t_agc_knee(o, s, l):
    """AGC Knee and Slope (Settings > Audio): with the knee at -100 (every signal over it) the slope decides how the
    audio grows with the signal: s's tone stepped 0.1, 0.3, 1.0 W through the pads, l's TCI audio level at each with
    Slope 0 and Slope 10. Valid when l's S-meter shows the step (AGC_STEP_MIN_DB) and its audio has room
    (AGC_PEAK_MAX_DBFS): then the growth from 0.1 to 1 W differs by AGC_SLOPE_MIN_DB. A run that cannot show it
    fails saying so (2026-11-26: the X6100 listening sat at -5 dBFS, its AGC's own level; the X6200 grew 3.1 and 2.8).
    On a base whose Knee and Slope do nothing (radio_caps without HW_CAP_AGC_SHAPE: the X6200's stock 1.0.8 and
    Xiegu's own X6100 base from V1.1.9, which take register 27 and change nothing: Knee 0 and -100 within 1 dB, the
    X6100 at RF gain 100, 30 and 5, 2026-11-26): PASS when they are offered nowhere, Settings > Audio > AGC showing
    Hang alone (by that bit, settings_window.cpp) and the remote refusing agc_knee and agc_slope"""
    import numpy as np
    from tones import TciRx
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        caps = caps_of(L)
        res["caps"] = caps
        if caps is None:
            return False, f"{l}: no radio_caps from the remote (a GUI before it)", res
        if not caps & CAP_AGC_SHAPE:
            bad = lacks_check(L, res, cfgs={"agc_knee": -100, "agc_slope": 10})
            if bad:
                return False, f"{l}'s base does nothing with Knee and Slope, yet {'; '.join(bad)}", res
            return True, f"{l}'s base does nothing with Knee and Slope and the GUI offers them nowhere: radio_caps " \
                         f"{caps} without them (Settings > Audio > AGC: Hang alone; radio.c sends neither), refused: " \
                         f"{', '.join(res['refused'])}", res
        curves, meters, peaks = {}, {}, {}
        for slope in (0, 10):
            with cfg_kept(L, {"agc_knee": -100, "agc_slope": slope}):
                curve, mtr, pk = [], [], []
                for w in (0.1, 0.3, 1.0):
                    rx = TciRx(L.host)
                    try:
                        got = {}

                        def body(watch, w=w):
                            _, t0, t1 = bench.tci_send(S, [[1000, 2.5]], watch=watch)
                            got["t"] = (t0, t1)
                        wt = H.over(o, s, l, F_USB, 2, f"agc slope {slope} {w} W", body, w=w, agc=1)
                        x, _ = rx.audio(got["t"][0] + 1.0, got["t"][1] - 0.3)
                        curve.append(round(float(20 * np.log10(np.sqrt(np.mean(np.square(x))) + 1e-9)), 1)
                                     if len(x) else None)
                        pk.append(round(float(20 * np.log10(np.max(np.abs(x)) + 1e-9)), 1) if len(x) else None)
                        mtr.append(getattr(wt, "rx_peak", None))
                    finally:
                        rx.close()
                curves[slope], meters[slope], peaks[slope] = curve, mtr, pk
        bench.ensure(S, {"pwr": H.W})
        res.update(curves_db=curves, s_meter_dbm=meters, peaks_dbfs=peaks)
        if any(v is None for c in curves.values() for v in c):
            return False, f"no audio at some step: {curves}", res
        steps = [m[-1] - m[0] for m in meters.values() if m[0] is not None and m[-1] is not None]
        if not steps or min(steps) < AGC_STEP_MIN_DB:
            return False, f"{l}'s S-meter did not show the 0.1-1 W step ({meters} dBm; {AGC_STEP_MIN_DB} dB wanted): " \
                          f"the input never stepped", res
        loud = max(p for c in peaks.values() for p in c)
        if loud > AGC_PEAK_MAX_DBFS:
            return False, f"{l}'s audio peaked at {loud} dBFS (its AGC's own level; under {AGC_PEAK_MAX_DBFS} wanted): " \
                          f"no room for the slope to show on this listener ({curves})", res
        growth = {k: round(c[-1] - c[0], 1) for k, c in curves.items()}
        res["growth_db"] = growth
        if abs(growth[10] - growth[0]) < AGC_SLOPE_MIN_DB:
            return False, f"Slope 0 and 10 grew {l}'s audio alike from 0.1 to 1 W ({growth} dB, the input " \
                          f"{[round(x, 1) for x in steps]} dB up): Knee/Slope show no effect there", res
        return True, f"the AGC slope acts: from 0.1 to 1 W (the S-meter {round(min(steps), 1)} dB up) the audio grew " \
                     f"{growth[0]} dB with Slope 0, {growth[10]} dB with 10 (knee -100)", res
    except Refused as e:
        return False, str(e), res


def t_cessb(o, s, l):
    """CESSB (Settings > TX Cal): the same two-tone audio through s's TCI at 0.1 W: its average power higher with
    CESSB on (its meters, l's S-meter). On a base without it (radio_caps without HW_CAP_CESSB: the X6200's stock base,
    Xiegu's own X6100 base): PASS when TX Cal has no CESSB card (by that bit alone, settings_window.cpp; radio.c sends
    none) and the remote refuses cessb_on and cessb_power_up"""
    import numpy as np
    import digimodes
    import suite_air_b as ab
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        caps = caps_of(S)
        res["caps"] = caps
        if caps is None:
            return False, f"{s}: no radio_caps from the remote (a GUI before it)", res
        if not caps & CAP_CESSB:
            bad = lacks_check(S, res, cfgs={"cessb_on": 1, "cessb_power_up": 5.0})
            if bad:
                return False, f"{s}'s base has no CESSB, yet {'; '.join(bad)}", res
            return True, f"{s}'s base has no CESSB and the GUI offers none: radio_caps {caps} without it (no CESSB " \
                         f"card in TX Cal), refused: {', '.join(res['refused'])}", res
        fs = 24000
        t = np.arange(int(4.0 * fs)) / fs
        x = 0.25 * (np.sin(2 * np.pi * 700 * t) + np.sin(2 * np.pi * 1900 * t))
        local = os.path.join(run_dir(o), f"twotone_{s}.raw")
        secs = digimodes.write_raw(x, local)
        subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:/tmp/bench_twotone.raw"],
                       check=True)
        subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
                        f"root@{S.host}:/tmp/tci_tone_tx.py"], check=True)
        avg = {}
        for on in (0, 1):
            with cfg_kept(S, {"cessb_on": on}):
                got = {}

                def body(watch):
                    _, t0, t1 = ab.tci_play(S, "/tmp/bench_twotone.raw", secs + 2, watch)
                    got["t"] = (t0, t1)
                H.over(o, s, l, F_USB, 2, f"cessb {on}", body)
                w = [x[1] for x in t_frames(S, got["t"][0] + 1, got["t"][1])]
                m = meters(L, got["t"][0] + 1, got["t"][1] - 0.5)
                avg[on] = {"w": round(float(np.mean(w)), 3) if w else None,
                           "m": round(float(np.mean(m)), 1) if m else None}
        res["avg"] = avg
        up = (avg[1]["m"] or -200) - (avg[0]["m"] or -200)
        res["s_meter_up_db"] = round(up, 1)
        if up >= 0.5:
            return True, f"CESSB on raised the average: {l}'s S-meter {up:+.1f} dB ({avg})", res
        return None, f"CESSB on did not raise the average of a two-tone ({avg}): to look at (the stimulus, or the " \
                     f"base's CESSB)", res
    except Refused as e:
        return False, str(e), res
    finally:
        ssh(S.host, "rm -f /tmp/bench_twotone.raw")


def t_moni(o, s, l):
    """Moni level (Settings > Audio > MICROPHONE): the setting reads back; the level the base gets (the state's moni,
    radio_moni_sent) follows it while the speaker is up and is 0 at VOL 0 (radio_vol_refresh: issue #1's rule; the
    bench runs at VOL 0). Until 2026-11-26 the test looked for Moni 30 at VOL 0, and for a TCI over on G's voiceprint,
    which TCI's mute keeps off by design"""
    S = o.r[s]
    res = {}
    try:
        vol0 = int(S.state.get("vol", 0))
        with cfg_kept(S, {"moni": 30}):
            res["setting"] = bench.cfg_get(S, "moni")
            try:
                for vol in (0, 1, 0):
                    bench.ensure(S, {"vol": vol})
                    time.sleep(0.8)
                    res[f"moni_at_vol_{vol}" + ("_again" if f"moni_at_vol_{vol}" in res else "")] = S.state.get("moni")
            finally:
                bench.ensure(S, {"vol": vol0})
        want = {"moni_at_vol_0": 0, "moni_at_vol_1": 30, "moni_at_vol_0_again": 0}
        bad = [f"{k} {res.get(k)} ({v} wanted)" for k, v in want.items() if str(res.get(k)) != str(v)]
        if str(res["setting"]) != "30":
            bad.insert(0, f"the setting read back {res['setting']!r}")
        if bad:
            return False, f"{s}: " + "; ".join(bad), res
        return True, f"{s}: Moni 30 read back; the base got 0 at VOL 0, 30 at VOL 1, 0 again at VOL 0", res
    except Refused as e:
        return False, str(e), res


def t_mic_gain(o, s, l):
    """Mic gain per microphone and Input: the state's mic follows Input (Auto/Hand/Built-in); the gains read back. Its
    level through the air needs sound at the microphone (acoustic.py's speaker by the radio: not set up here): the
    settings' half only"""
    S = o.r[s]
    res = {}
    try:
        for name, vals in (("hmic", (20, 35)), ("imic", (30, 45))):
            with cfg_kept(S, {name: vals[1]}):
                res[name] = bench.cfg_get(S, name)
        mic0 = int(S.state.get("mic", 0))
        seen = []
        with cfg_kept(S, {}):
            for v in (0, 1, 2):
                bench.cfg_set(S, "mic", v)
                time.sleep(0.6)
                seen.append(S.state.get("mic"))
            bench.cfg_set(S, "mic", mic0)
        res["mic_states"] = seen
        if [str(x) for x in seen] != ["0", "1", "2"]:
            return False, f"the state's mic did not follow Input 0/1/2 (read {seen})", res
        if str(res["hmic"]) != "35" or str(res["imic"]) != "45":
            return False, f"the gains read back {res['hmic']}/{res['imic']}, not 35/45 as set", res
        return True, f"gains read back {res['hmic']}/{res['imic']}, Input stepped the state's mic {seen} (the settings' " \
                     f"half: the level through the air is comp-mic's room path, not judged here)", res
    except Refused as e:
        return False, str(e), res


# The classic screen as photographed (800x480): the S-meter box's frame and the TX popup's border, TX red while on the
# air (amber, the theme's ink, receiving); the popup's place is the screen's dark ground without it (2026-11-26, from
# the X6200's pictures of tx-panel)
TX_RED = (248, 113, 113)
SCREEN_DARK = (8, 9, 12)
METER_FRAME_XY = (366, 45)
TX_POPUP_XY = (151, 220)


def _px(path, xy):
    from PIL import Image
    try:
        return Image.open(path).convert("RGB").getpixel(xy)
    except (OSError, ValueError):
        return None


def _near(rgb, want, tol=30):
    return rgb is not None and all(abs(a - b) <= tol for a, b in zip(rgb, want))


def t_tx_panel(o, s, l):
    """Popups TX (Settings > Display): s keyed 3 s at 0.1 W from the classic screen with it on and off, photographed on
    the air (the PTT renewed first, s on the air when taken): with it on, the TX panel's red border; either way the
    S-meter box framed TX red; more than 10 't' frames (the panel's values) in each over"""
    S = o.r[s]
    res = {}
    try:
        ensure_alike(o, F_USB, 2)
        classic(S)
        bad = []
        for on in (1, 0):
            with cfg_kept(S, {"mag_alc": on}):
                with session(o, f"tx-panel {on}", [s], F_USB) as watch:
                    role(o, s)
                    hold = int(time.time())
                    t0 = time.time()
                    S.send(f"key PTT 1 {hold}")
                    S.wait("on the air", lambda: S.tx or watch.why, 4)
                    time.sleep(1.2)
                    S.send(f"key PTT 1 {hold}")     # renewed: taking the pictures outlasted the lease (11-25)
                    on_air = bool(S.tx)
                    paths = snap(o, f"tx-panel {s}: Popups TX {'on' if on else 'off'}")
                    S.send(f"key PTT 1 {hold}")
                    time.sleep(1.0)
                    S.send("key PTT 0")
                    S.wait("off the air", lambda: not S.tx, 4)
                    frames = len(t_frames(S, t0))
                    frame, popup = _px(paths.get(s), METER_FRAME_XY), _px(paths.get(s), TX_POPUP_XY)
                    res[f"popups_{on}"] = {"on_air": on_air, "frames": frames, "meter_frame": frame, "popup": popup}
                    what = f"Popups TX {'on' if on else 'off'}"
                    if not on_air:
                        bad.append(f"{what}: {s} off the air when photographed")
                    if not _near(frame, TX_RED):
                        bad.append(f"{what}: the S-meter box's frame {frame}, not TX red")
                    if not _near(popup, TX_RED if on else SCREEN_DARK):
                        bad.append(f"{what}: the TX popup's border {popup}, " + ("no panel" if on else "a panel shown"))
                    if frames <= 10:
                        bad.append(f"{what}: {frames} 't' frames")
        if bad:
            return False, "; ".join(bad), res
        return True, f"Popups TX on: the TX panel and a red S-meter frame; off: no panel, the S-meter box red " \
                     f"({res['popups_1']['frames']}/{res['popups_0']['frames']} 't' frames)", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


@contextlib.contextmanager
def disp_kept(r, values):
    """r's Display settings set through the web configurator's Display page (POST /api/display: the GUI's disp_set,
    which re-reads the idle minutes; the remote's cfg did not before 2026-11-25), each read back, put back after"""
    import suite_cover_a as ca
    found = ca.disp(r.host)
    if not found:
        raise Refused(f"{r.name}: the Display page did not answer")
    if found.get("screen_off_always") or found.get("screen_off_min") == 1:
        raise Refused(f"{r.name}: its screen goes dark at the minute the saver would come (Screen off): left alone")
    try:
        for k, v in values.items():
            st = ca.disp_set(r.host, k, v)
            if st != 200:
                raise Refused(f"{r.name}: the Display page answered {st} to {k} {v}")
        now = ca.disp(r.host)
        if any(now.get(k) != v for k, v in values.items()):
            raise Refused(f"{r.name}: Display reads {({k: now.get(k) for k in values})} after asking {values}")
        yield found
    finally:
        ca.disp_back(r.host, found, list(values))


def t_screensaver_tx(o, s, l):
    """The screensaver (1 min) ends within a second of any TX. Every remote client is a viewer and keeps the saver
    away (remote_server.c tick_cb: nclients > 0), the bench's own link too (run 27: the saver never came): s armed
    first (the arming's key would wake the screen), its screen's still parts taken, the bench's link to s closed (a
    planned silence; the Watch on l alone meanwhile), s left alone till a dump shows the saver; then keyed by TCI (no
    key pressed; TCI's client is no viewer) at 0.1 W for 4 s: 1 s after l hears it, the screen dumped again: the GUI's
    still parts back. The link opened again after. PASS when the saver came and the GUI was back 1 s into the over"""
    import suite_cover_a as ca
    S, L = o.r[s], o.r[l]
    res = {}
    im0 = im1 = None
    try:
        on_g_alike(o, F_USB, 2)
        # through the remote's cfg: the GUI re-reads the idle minutes after it since 2026-11-25 (remote_server.c
        # cfg_legacy's after: backlight_settings_changed); the Display page's path is the suite's screensaver test
        with cfg_kept(S, {"screen_saver_min": 1, "screen_saver": 1}):
            p, total = bench.chain_gate(o.c, "radios")
            bench.ensure(S, *bench.tx_profile(F_USB, 2, H.W))
            bench.radio_tx_gate(S, o.c, H.W)
            bench.listener_gate(L)
            o.changing += 1
            try:
                bench.arm(S, True, mode=2)
            finally:
                o.changing -= 1
            o.roles = {s: "TX", l: "RX"}
            pre = ca.dump(None, S.host)
            time.sleep(1.0)
            pre2 = ca.dump(None, S.host)
            if pre is None or pre2 is None:
                raise Refused(f"{s}'s screen could not be dumped")
            mask = ca.stable(pre, pre2)
            o.planned[s] = "planned: the bench's link closed for the screensaver (a remote client is a viewer)"
            S.r.close()
            watch = None
            try:
                time.sleep(2.0)
                res["peers"] = ca.remote_peers(S.host)
                if res["peers"]:
                    raise Refused(f"{res['peers']} other remote connections to {s} (a browser, another bench): a "
                                  f"client keeps the screensaver away")
                t_set = time.time()
                while time.time() - t_set < 85 and im0 is None:
                    im = ca.dump(None, S.host)
                    if im is not None and ca.kept(mask, pre, im) < 0.35:
                        im0 = im
                        res["saver_after_s"] = round(time.time() - t_set)
                    else:
                        time.sleep(2.0)
                if im0 is None:
                    raise Refused(f"no screensaver on {s} in 85 s with no remote client (Screensaver 1 min)")
                res["kept_saver"] = round(ca.kept(mask, pre, im0), 2)
                watch = bench.Watch([L], o.c, listener=L, w=H.W, label="screensaver-tx")
                th = threading.Thread(target=lambda: bench.tci_send(S, [[1000, 4.0]], max_s=8.0, watch=watch),
                                      daemon=True)
                th.start()
                L.wait(f"{s}'s over heard", lambda: (L.meter is not None and L.meter > H.RF_ALARM_DBM) or watch.why, 10)
                time.sleep(1.0)
                im1 = ca.dump(None, S.host)
                th.join(20)
            finally:
                if watch is not None:
                    watch.close()
                try:
                    S.reconnect()
                finally:
                    o.planned.pop(s, None)
                try:
                    S.wait("off the air", lambda: not S.tx, 5)
                except Refused:
                    S.stop()
                bench.arm(S, False)
                o.roles = {H.A: "RX", H.B: "RX"}
        if im1 is None:
            return False, f"{s}'s screen could not be dumped on the air", res
        res["kept_on_air"] = round(ca.kept(mask, pre, im1), 2)
        try:
            im0.save(os.path.join(run_dir(o), f"screensaver_{s}_saver.png"))
            im1.save(os.path.join(run_dir(o), f"screensaver_{s}_on_air.png"))
        except OSError:
            pass
        if res["kept_on_air"] < 0.6:
            return False, f"1 s into TCI's TX {s}'s screen kept {res['kept_on_air']:.0%} of the GUI's still parts " \
                          f"(the saver {res['kept_saver']:.0%}): the saver not ended ({res})", res
        return True, f"the saver came {res['saver_after_s']} s after the bench let go (the GUI's still parts " \
                     f"{res['kept_saver']:.0%}); 1 s into TCI's TX the GUI was back ({res['kept_on_air']:.0%})", res
    except Refused as e:
        return False, str(e), res


def t_update_check_off(o, s, l):
    """While a run goes, both radios' update check and its question are off (the owner, 2026-11-25: "Yes, during
    tests"): /mnt/update/no-check on each, the run's own (marked) or one the owner made, and no update question or
    power-on box open on either (cfg? upd_state: the X6200's question, fetched 6 min before a run made no-check, stayed
    over its tests 3 h, 2026-11-26; the GUI now puts it away when no-check appears); nothing sent"""
    res, boxes = {}, {}
    for n, r in o.r.items():
        res[n] = ssh(r.host, f"[ -e {H.NO_CHECK} ] && {{ cat {H.NO_CHECK}; echo .; }} || echo none").strip()
        try:
            boxes[n] = str(bench.cfg_get(r, "upd_state", timeout=3))
        except Refused as e:
            boxes[n] = f"unread ({e})"
    res["upd_state"] = boxes
    missing = [n for n in o.r if res[n] == "none"]
    if missing:
        return False, f"no {H.NO_CHECK} on {', '.join(missing)} during the run: an update question could take " \
                      f"the MFK mid-test", res
    open_ = [f"{n} {v}" for n, v in boxes.items() if v in ("asking", "restart") or v.startswith("unread")]
    if open_:
        return False, f"an update box over the tests ({', '.join(open_)}) with {H.NO_CHECK} there: the MFK and " \
                      f"keys go to it, not to the test", res
    return True, "the update check off on both while the run goes (" + \
        ", ".join(f"{n}: {'the run' + chr(39) + 's' if res[n].startswith(H.NO_CHECK_MARK) else 'the owner' + chr(39) + 's'}"
                  f", upd_state {boxes[n]}" for n in o.r) + ")", res


def t_stuck_tx_log(o, s, l):
    """The stuck-TX guard's lines in the base logs of every cabled run so far (passive): each 'TX power set to 0'
    followed by 'restored'"""
    res = {}
    bad = []
    for n, r in o.r.items():
        out = ssh(r.host, "grep -h -i 'TX power set to 0\\|TX power restored\\|PTT off resent\\|register write failed' "
                          "/mnt/app_logs/*.log /mnt/app_logs/*.txt 2>/dev/null | tail -200")
        lines = out.splitlines()
        zero = sum(1 for x in lines if "set to 0" in x.lower())
        back = sum(1 for x in lines if "restored" in x.lower())
        res[n] = {"set_to_0": zero, "restored": back, "resends": sum(1 for x in lines if "resent" in x.lower())}
        if zero > back:
            bad.append(f"{n}: {zero} 'TX power set to 0' but {back} 'restored'")
    if bad:
        return False, "; ".join(bad), res
    return True, f"every 'TX power set to 0' restored ({res})", res


def t_long_press_busy(o, s, l):
    """A key held 1.5 s while the GUI is busy (s on the air, 0.1 W) is a long press by its own events' times: the
    classic screen's AB held copies VFO A to B (A=B: the VFOs read alike after), never its tap's VFO swap"""
    S = o.r[s]
    res = {}
    try:
        ensure_alike(o, F_USB, 2)
        classic(S)
        vfo0 = S.state.get("vfo")
        with session(o, "long-press-busy", [s], F_USB) as watch:
            role(o, s)
            hold = int(time.time())
            S.send(f"key PTT 1 {hold}")
            S.wait("on the air", lambda: S.tx or watch.why, 4)
            t_ab = time.time()
            S.send("key AB 1")
            res["tx_at_ab_down"] = bool(S.tx)
            time.sleep(1.5)
            S.send(f"key PTT 1 {hold}")
            res["tx_at_ab_up"] = bool(S.tx)
            S.send("key AB 0")
            time.sleep(0.8)
            S.send("key PTT 0")
            S.wait("off the air", lambda: not S.tx, 4)
        vfo1 = S.state.get("vfo")
        # the hold's own word (screen_main.c ab_key: "Clone VFO A->B" on A), which the remote gets as a message
        said = next((t for t in msgs_since(S, t_ab) if re.search(r"Clone VFO", t, re.I)), None)
        res.update(vfo_before=vfo0, vfo_after=vfo1, said=said)
        if not (res["tx_at_ab_down"] and res["tx_at_ab_up"]):
            return False, f"{s} was not on the air through AB's hold ({res}): the long press not tried while busy", res
        if vfo1 != vfo0:
            return False, f"AB held 1.5 s on the air acted as a tap: the VFO {vfo0} -> {vfo1}", res
        if not said:
            return False, f"AB held 1.5 s on the air: no 'Clone VFO' (it said {msgs_since(S, t_ab)[-4:]})", res
        return True, f"AB held 1.5 s on the air was a long press: {said!r}, the VFO kept ({vfo0})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


# ---- the Sentry: G's walkie (PTT, ATU), G's lamp, A's TX rows, SCAN CFG's priority and skip ----

def t_walkie(o, s, l):
    """Walkie Talkie mode (Settings > Walkie: the portrait screen, cfg "walkie" 1) on s's classic screen: PTT (the
    remote's lease, 3 s at 0.1 W on 28.400 USB) sends with the walkie microphone (the state's mic) and a red border
    (picture); Walkie off: the Audio tab's microphone back. (Its ATU held tunes with the base's own carrier, which
    PWR does not hold: t_walkie_atu, on the owner's word)"""
    S, L = o.r[s], o.r[l]
    res = {}
    tp0 = S.state.get("tpwr")
    try:
        ensure_alike(o, 28400000, 2)
        classic(S)
        mic0 = S.state.get("mic")
        with cfg_kept(S, {"walkie": 1}):
            time.sleep(1.5)
            with session(o, "walkie", [s], 28400000) as watch:
                role(o, s)
                hold = int(time.time())
                S.send(f"key PTT 1 {hold}")
                S.wait("on the air", lambda: S.tx or watch.why, 4)
                time.sleep(1.0)
                res["mic_on_air"] = S.state.get("mic")
                snap(o, f"walkie {s}: PTT in Walkie Talkie mode (red border)")
                S.send(f"key PTT 1 {hold}")
                time.sleep(1.0)
                S.send("key PTT 0")
                S.wait("off the air", lambda: not S.tx, 4)
        time.sleep(1.5)
        res.update(mic_before=mic0, mic_after=S.state.get("mic"))
        if res["mic_on_air"] == mic0:
            return False, f"Walkie PTT: the state's mic stayed {mic0} (the walkie microphone wanted)", res
        if res["mic_after"] != mic0:
            return False, f"after Walkie Talkie the mic reads {res['mic_after']}, not {mic0}", res
        return True, f"Walkie PTT at 0.1 W: mic {mic0} -> {res['mic_on_air']} -> {res['mic_after']}", res
    except Refused as e:
        return False, str(e), res
    finally:
        if tp0 is not None:
            S.send(f"set tpwr {int(round(float(tp0) * 10))}")
        home(o, S)


def t_walkie_atu(o, s, l):
    """Walkie Talkie's ATU held tunes: the radio's own tuner keys the base's own carrier, which PWR does not hold (over
    the owner's 1 W on the cable): the owner's word first. Refused without ~/.config/x6x00-bench/owner_ok_atu_tune"""
    if not os.path.exists(os.path.join(bench.STATE, "owner_ok_atu_tune")):
        raise Refused("needs the owner's word: the ATU's tune is the base's own carrier, over 1 W (owner_ok_atu_tune)")
    return None, "the word is there; the tune steps themselves are not written yet", {}


G_LAMP_SETTLE_S = 1.0     # the listener's lamp lit this soon into the over (its meter and audio smoothed)
G_LAMP_QUIET_S = 3.0      # and dark again this soon after it (the hiss after an FM over read SIGNAL for long, 11-26)


def t_g_lamp(o, s, l):
    """G's lamp: l on G on 29.600 FM; s's FM over (a tone) at 0.1 W: s's lamp ON AIR, l's RX SIGNAL from
    G_LAMP_SETTLE_S into it to its end, RX QUIET within G_LAMP_QUIET_S after it and from then on. Judged on the lamp's
    word in each radio's state (glamp, from 2026-11-26), sampled every 0.1 s; pictures of both kept"""
    S, L = o.r[s], o.r[l]
    res = {}
    seen, stop = [], threading.Event()

    def sample():
        while not stop.is_set():
            seen.append((time.time(), bool(S.tx), S.state.get("glamp"), L.state.get("glamp")))
            time.sleep(0.1)
    try:
        t0 = time.time()
        th_s = threading.Thread(target=sample, daemon=True)
        th_s.start()

        def body(watch):
            th = threading.Thread(target=lambda: bench.tci_send(S, [[1000, 4.0]], watch=watch))
            th.start()
            time.sleep(2.0)
            snap(o, f"g-lamp: {s} ON AIR, {l} RX SIGNAL")
            th.join(15)
        try:
            H.over(o, s, l, 29600000, 7, "g-lamp", body)
            time.sleep(G_LAMP_QUIET_S + 3.0)
            snap(o, f"g-lamp: after the over, {l} RX QUIET")
        finally:
            stop.set()
            th_s.join(2)
        if not any(x[2] or x[3] for x in seen):
            return None, "no lamp word in the radios' state (a GUI before glamp): G's lamps photographed (look)", res
        spans = tx_spans(S, t0)
        if not spans:
            raise Refused(f"{s} never went on the air")
        on, off = spans[0]
        during = [x for x in seen if on + G_LAMP_SETTLE_S <= x[0] <= off - 0.2]
        after = [x for x in seen if off + G_LAMP_QUIET_S <= x[0]]
        first_quiet = next((x[0] - off for x in seen if x[0] > off and x[3] == "QUIET"), None)
        res.update(over_s=round(off - on, 1), sender_on_air=sum(1 for x in seen if on <= x[0] <= off and x[2] == "ON AIR"),
                   l_during=dict(collections.Counter(x[3] for x in during)),
                   l_after=dict(collections.Counter(x[3] for x in after)),
                   l_quiet_after_s=None if first_quiet is None else round(first_quiet, 1))
        bad = []
        if not res["sender_on_air"]:
            bad.append(f"{s}'s lamp never read ON AIR on the air")
        if not during or sum(1 for x in during if x[3] == "SIGNAL") < 0.9 * len(during):
            bad.append(f"{l}'s lamp during the over: {res['l_during']} (SIGNAL wanted)")
        if not after or any(x[3] != "QUIET" for x in after):
            bad.append(f"{l}'s lamp from {G_LAMP_QUIET_S:.0f} s after the over: {res['l_after']} (QUIET wanted)")
        if bad:
            return False, "; ".join(bad), res
        return True, f"{s} ON AIR on the air; {l} SIGNAL through the over ({res['l_during']}), QUIET " \
                     f"{res['l_quiet_after_s']} s after it and from then on", res
    except Refused as e:
        return False, str(e), res


A_TX_ROWS_MIN_PER_S = 2   # a-tx-rows: A's history keeps moving on the air at least this many rows a second (none before
                          # 2026-11-20); it draws them at its own receive pace (app_sentry.c wf_tx_row)


def t_a_tx_rows(o, s, l):
    """A's picture keeps time while transmitting: s keyed 5 s from A (armed there, 0.1 W); A's history draws its dim
    rows all through the over (the remote's read-only a_tx_rows counts them, from 2026-11-26: at least
    A_TX_ROWS_MIN_PER_S a second) and none after it; a picture kept"""
    S = o.r[s]
    res = {}
    try:
        with o.step("both on A, 14.200 USB, one receive profile"):
            o.both_page("A")
            want, checks = bench.rx_profile(F_USB, 2, pre=0, att=0, rfg=100, agc=2, sql=0, filt=(100, 3000))
            o.both(dict(want, pwr=H.W, atu=0, xit=0, rit=0, split=0, zoom=8), checks)

        n0 = bench.cfg_get(S, "a_tx_rows")
        if n0 in (None, ""):
            raise Refused(f"{s}: no a_tx_rows from the remote (a GUI before it)")
        t0 = time.time()

        def body(watch):
            bench.tci_send(S, [[1000, 5.0]], watch=watch)
        offset_over(o, s, l, F_USB, 2, "a-tx-rows", body)
        n1 = int(bench.cfg_get(S, "a_tx_rows"))
        time.sleep(3)
        n2 = int(bench.cfg_get(S, "a_tx_rows"))
        snap(o, f"a-tx-rows: {s}'s A after its 5 s over")
        spans = tx_spans(S, t0)
        over_s = sum(b - a for a, b in spans)
        res.update(rows=n1 - int(n0), rows_after=n2 - n1, over_s=round(over_s, 1),
                   per_s=round((n1 - int(n0)) / over_s, 1) if over_s else None)
        if not over_s:
            raise Refused(f"{s} never went on the air")
        if res["rows"] < A_TX_ROWS_MIN_PER_S * over_s:
            return False, f"A drew {res['rows']} rows through {over_s:.1f} s on the air (at least " \
                          f"{A_TX_ROWS_MIN_PER_S} a second wanted: the picture stopped)", res
        if res["rows_after"]:
            return False, f"A went on drawing on-air rows after the over ({res['rows_after']} in 3 s)", res
        return True, f"A drew {res['rows']} dim rows through {over_s:.1f} s on the air ({res['per_s']} a second), none " \
                     f"after", res
    except Refused as e:
        return False, str(e), res


def t_scancfg_priority(o, s, l):
    """SCAN CFG's PRIORITY set on l's A at 14.200 (a push on its row: s.prio), A scanning 20 m: s keys a 0.1 W tone on
    14.200: l's dial goes to 14.200 within ~3 s; then F3 SKIP passes it over (the scan moves on while s still sends).
    After: A's scan off and PRIORITY cleared (one more push on its row), read back from gui_state"""
    import suite_cover_b as cb
    S, L = o.r[s], o.r[l]
    res = {}
    keep = ("freq", "mode", "flo", "fhi", "att", "pre", "agc", "zoom", "rfg")
    scanning = False                                    # A's scan this test turned on (no state says it)
    try:
        with o.step("both on A, 14.200 USB"):
            o.both_page("A")
            o.both({"freq": F_USB, "mode": 2, "pwr": H.W})
        cb.cfg_open(L, "A")
        try:
            cur, _ = cb.cfg_find(L, L.host)
            cb.cfg_goto(L, cur, cb.CR["PRIO"])
            bench.ensure(L, {"freq": F_USB})
            cb.cfg_push(L)
            prio = cb.cfg_wait(L.host, lambda a: a.get("prio"), 4).get("prio")
            res["prio"] = prio
        finally:
            cb.cfg_close(L)
        if not prio:
            raise Refused(f"{l}'s PRIORITY not set by the push ({cb.cfg_look(L.host)})")
        with bench.apart({k: bench.STEPPED for k in keep}, "scancfg-priority: l's A scans 20 m"):
            bench.tap(L, "F1")                          # SCAN
            scanning = True
            time.sleep(4)
            got = {}

            def body(watch):
                t0 = time.time()
                th = threading.Thread(target=lambda: bench.tci_send(S, [[1000, 9.0]], watch=watch))
                th.start()
                try:
                    L.wait("the dial on the priority", lambda: int(L.state.get("freq", 0)) == F_USB, 5)
                    got["to_prio_s"] = round(time.time() - t0, 1)
                    time.sleep(1.0)
                    cb.cfg_open(L, "A")                 # SCAN CFG's F3 "A SKIP" (A's own F3 is VIEW)
                    bench.tap(L, "F3")
                    time.sleep(3.0)
                    got["after_skip"] = int(L.state.get("freq", 0))
                    bench.open_page(L, "A")
                finally:
                    th.join(15)
            offset_over(o, s, l, F_USB, 2, "scancfg-priority", body, pair_ignore=keep)
            bench.open_page(L, "A")
            bench.tap(L, "F1")                          # the scan off while the difference is still declared
            scanning = False
            time.sleep(1.0)
            bench.ensure(L, {"freq": F_USB, "mode": 2})
        res.update(got)
        if got.get("to_prio_s") is None:
            return False, f"{l}'s scan did not go to the priority 14.200 while {s} sent", res
        if got.get("after_skip") == F_USB:
            return False, f"F3 SKIP left {l} on 14.200 while {s} still sent", res
        return True, f"to the priority in {got['to_prio_s']} s; SKIP moved on ({got['after_skip']})", res
    except (Refused, cb.Unsure) as e:
        return False, str(e), res
    finally:
        try:
            bench.open_page(L, "A")
            if scanning:
                bench.tap(L, "F1")                      # the scan this test started: off
                time.sleep(1.0)
            if cb.cfg_look(L.host).get("prio"):         # PRIORITY's row is the one chosen: a push clears it
                cb.cfg_open(L, "A")
                cb.cfg_push(L)
                cb.cfg_wait(L.host, lambda a: not a.get("prio"), 4)
                bench.open_page(L, "A")                 # (SCAN CFG's F5 from the remote did not take, 2026-11-25)
            res["after"] = {"prio": cb.cfg_look(L.host).get("prio")}
            if res["after"]["prio"]:
                o.st.ev(f"{l}: PRIORITY not put back ({res['after']})", "bad")
        except Exception as e:  # noqa: BLE001
            o.st.ev(f"{l}: SCAN CFG not put back: {e}", "bad")
        home(o, L)


def t_iradio_tx(o, s, l):
    """Internet radio stops the moment the radio transmits (its player ends, the header Stopped) and does not resume:
    needs the internet (the owner's OK for the read-only internet session: owner_ok_internet)"""
    if not os.path.exists(os.path.join(bench.STATE, "owner_ok_internet")):
        raise Refused("needs the owner's OK for the internet apps (owner_ok_internet)")
    return None, "the OK is there; the player steps are not written yet", {}


# ---- MSG > Voice: a message played on the air, refused out of the bands ----

VOICE_NAME = "0 BENCH VOICE"


def t_voice_msg(o, s, l):
    """A voice message (a 3 s tone made here as MP3, put in /mnt/msg, taken out after) played by MSG > Voice's TX
    (page 1 F4) at 0.1 W on 28.400 USB: s on the air, l hears it (its S-meter up); on 27.185 MHz refused (its message),
    nothing sent"""
    S, L = o.r[s], o.r[l]
    res = {}
    local = os.path.join(run_dir(o), "bench_voice.mp3")
    try:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=1000:duration=3",
                        "-ar", "16000", "-ac", "1", local], check=True)
        if not os.path.getsize(local):
            raise Refused("the test's MP3 came out empty (ffmpeg)")
        made = ssh(S.host, "[ -d /mnt/msg ] && echo had || { mkdir -p /mnt/msg && echo made; }").strip()
        res["msg_dir"] = made
        subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:/mnt/msg/bench_voice.mp3"],
                       check=True)
        out = ssh(S.host, f"mv /mnt/msg/bench_voice.mp3 '/mnt/msg/{VOICE_NAME}.mp3' && ls -l '/mnt/msg/{VOICE_NAME}.mp3'")
        if VOICE_NAME not in out:
            raise Refused(f"{s}: the voice message not in /mnt/msg ({out.strip()[:100]})")
        ensure_alike(o, 28400000, 2)
        classic(S)
        with session(o, "voice-msg", [s], 28400000) as watch:
            role(o, s)
            S.send("key MSG 1")
            time.sleep(0.15)
            S.send("key MSG 0")
            time.sleep(1.5)                     # MSG > Voice: a dialog, no window name in the state
            res["dialog"] = True
            S.send("knob mfk -40")
            time.sleep(0.6)
            t0 = time.time()
            S.take_control()
            S.send("key F4 1")
            time.sleep(0.15)
            S.send("key F4 0")                  # TX (MSG 1:2, F4)
            S.wait("the message on the air", lambda: S.tx or watch.why, 5)
            S.wait("its end", lambda: not S.tx or watch.why, 12)
            t1 = time.time()
            m = meters(L, t0, t1)
            res.update(sent_s=round(t1 - t0, 1), l_meter_max=max(m) if m else None)
            with bench.apart({"freq": (28400000, 27185000)}, "voice-msg: s on 27.185 for the refusal"):
                bench.ensure(S, {"freq": 27185000})
                t2 = time.time()
                S.send("key F4 1")
                time.sleep(0.15)
                S.send("key F4 0")
                time.sleep(1.5)
                res["refused"] = msgs_since(S, t2)[-2:]
                res["keyed_on_cb"] = len(tx_spans(S, t2))
                S.stop()
                bench.ensure(S, {"freq": 28400000})
        if res["keyed_on_cb"]:
            return False, f"the voice message keyed {s} on 27.185 MHz", res
        if not res["l_meter_max"]:
            return False, f"no S-meter from {l} while {s} played the message", res
        return True, f"played {res['sent_s']} s on the air ({l}'s S-meter up to {res['l_meter_max']}); on 27.185 " \
                     f"refused ({res['refused']})", res
    except (Refused, subprocess.CalledProcessError) as e:
        return False, str(e), res
    finally:
        if res.get("dialog"):                   # closed by its own keys: page 2's F5 Close
            for k in ("F1", "F5"):
                S.send(f"key {k} 1")
                time.sleep(0.15)
                S.send(f"key {k} 0")
                time.sleep(0.8)
        ssh(S.host, f"rm -f '/mnt/msg/{VOICE_NAME}.mp3' /mnt/msg/bench_voice.mp3")
        if res.get("msg_dir") == "made":
            ssh(S.host, "rmdir /mnt/msg 2>/dev/null; true")
        home(o, S)


# ---- Winlink over the cable: Pat's ARDOP call to the other radio (it never answers), Mercury, XARA ----

def _winlink(r, body=None, timeout=90):
    """The web configurator's Winlink page (GET, or POST body): (status, its JSON). Its own timeout: a switch starts or
    stops Pat and the modem, which took longer than the suite's 10 s (run 28: on timed out, and so did the put-back,
    which left the X6200's Winlink on with Mercury)"""
    import urllib.error
    import urllib.request
    import suite_air_b as ab
    ip = ab.web_ip(r.host)
    h = {"Host": ip}
    data = None
    if body is not None:
        data, h["Content-Type"] = json.dumps(body).encode(), "application/json"
    req = urllib.request.Request(f"http://{ip}/api/winlink", data=data, headers=h, method="POST" if body else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as f:
            st, b = f.status, f.read(1 << 16).decode(errors="replace")
    except urllib.error.HTTPError as e:
        st, b = e.code, e.read(1 << 16).decode(errors="replace")
    except OSError as e:
        raise Refused(f"{r.name}: the Winlink page did not answer in {timeout} s ({e})")
    try:
        return st, json.loads(b)
    except ValueError:
        return st, {}


PAT_ASK = r"""
import sys, urllib.request
req = urllib.request.Request("http://127.0.0.1:8080" + sys.argv[1], method=sys.argv[2],
                             headers={"X-Requested-With": "XMLHttpRequest"})
try:
    with urllib.request.urlopen(req, timeout=float(sys.argv[3]) if len(sys.argv) > 3 else 20) as f:
        print(f.status, f.read(300).decode(errors="replace"))
except Exception as e:
    print(getattr(e, "code", 0), e)
"""


def _pat(r, path, method="GET", timeout=20):
    """Pat's own HTTP API, asked on the radio itself (127.0.0.1:8080) as Pat's own page asks it: what keys or changes
    something needs jQuery's X-Requested-With, else 403 (tools/pat/patches/0001-http-guard.patch)"""
    out = subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", f"root@{r.host}", f"python3 - '{path}' {method} {timeout}"],
                         input=PAT_ASK, capture_output=True, text=True, timeout=timeout + 20).stdout.strip()
    code, _, body = out.partition(" ")
    try:
        return int(code), body
    except ValueError:
        return 0, out


WL_DIAL = 14105000
WL_RUN = "/run/winlink"        # x6100-winlink's logs (pat.log, pat-own.log, ardopcf.log...): gone when Winlink stops


def _pat_logs(r):
    """Pat's and the sender's modem's last log lines on r, and the supervisor's word on it, read while Winlink is
    still on (its supervisor takes the logs away when it stops)"""
    try:
        out = ssh(r.host, f"tail -n 12 {WL_RUN}/pat.log {WL_RUN}/pat-own.log {WL_RUN}/ardopcf.log 2>/dev/null; "
                          f"tail -c 600 {WL_RUN}/eventlog.json 2>/dev/null", 20)[-3500:]
    except Refused as e:
        out = str(e)
    try:
        st, conf = _winlink(r, timeout=20)
        out += "\nsupervisor: " + json.dumps({k: v for k, v in conf.items()
                                               if k in ("state", "text", "pat", "ardop", "modem", "last_result",
                                                        "last_time", "last_freq", "busy", "radio")})
    except Refused as e:
        out += f"\nsupervisor: {e}"
    return out


WL_RECENT = "/mnt/winlink/mail_recent.json"   # APP > Mail's recent gateways: a session puts its call on top


class PatSession:
    """A connection as APP > Mail makes it: x6100-winlink-mail session URL on r (Pat's /api/connect, its log, its
    questions and the outcome, a JSON object a line), in the background. Pat's question about an account it cannot
    confirm is answered Continue anyway, as the app's button answers it (src/winlink_mail.c; MANUAL, APP > Mail): Pat
    asks it before anything else for a call the Winlink server does not know, as the test calls are, waits 2 minutes
    for the answer and says nothing in its log meanwhile (runs 27-33: 'its first frame did not come within 60 s', 7
    of 7). The answer holds 100 days (Pat's .account-confirmed mark). Every other question is left to the test"""

    def __init__(self, r, url):
        self.r, self.url, self.t0 = r, url, time.time()
        self.log, self.prompts, self.answered, self.done, self.other = [], [], [], None, []
        self.lock = threading.Lock()
        self.p = subprocess.Popen(["ssh", "-4", "-o", "BatchMode=yes", f"root@{r.host}",
                                   f"x6100-winlink-mail session '{url}'"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                  bufsize=1)
        self.th = threading.Thread(target=self._read, daemon=True)
        self.th.start()

    def _read(self):
        for line in self.p.stdout:
            line = line.strip()
            try:
                m = json.loads(line)
            except ValueError:
                m = None
            if not isinstance(m, dict):
                if line:
                    self.other.append(line[:200])
                continue
            at = round(time.time() - self.t0, 1)
            if "log" in m:
                self.log.append(f"{at} {str(m['log'])[:160]}")
            elif "prompt" in m and isinstance(m["prompt"], dict):
                p = m["prompt"]
                with self.lock:
                    self.prompts.append({"at": at, "kind": p.get("kind"), "message": str(p.get("message"))[:100]})
                if p.get("kind") == "pre-account-activation":
                    self.answer(p.get("id"), "confirmed")
            elif "done" in m:
                self.done = m["done"]
            elif "error" in m:
                self.other.append(str(m["error"])[:200])

    def answer(self, pid, value):
        """One of Pat's questions answered, as the app's buttons answer them"""
        try:
            self.p.stdin.write(json.dumps({"prompt": {"id": str(pid), "value": value}}) + "\n")
            self.p.stdin.flush()
            self.answered.append(value)
        except (OSError, ValueError):
            pass

    def kinds(self):
        with self.lock:
            return [p["kind"] for p in self.prompts]

    def alive(self):
        return self.p.poll() is None

    def close(self, wait=20):
        """Abort, as the Connection page's (a busy channel's wait ends with it at once); then at once if it is still
        on; the session's own process gone either way. What Pat did: the result's keys"""
        for line in ("abort", "abort!"):
            if not self.alive():
                break
            try:
                self.p.stdin.write(line + "\n")
                self.p.stdin.flush()
            except (OSError, ValueError):
                break
            end = time.time() + wait / 2
            while self.alive() and time.time() < end:
                time.sleep(0.2)
        if self.alive():
            _pat(self.r, "/api/disconnect?dirty=true")
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                self.p.kill()
        self.th.join(3)

    def result(self):
        return {"prompts": self.prompts, "answered": self.answered, "done": self.done,
                "log": self.log[-14:], "other": self.other[-4:]}


@contextlib.contextmanager
def winlink_on(o, modem):
    """Winlink on, on both, with modem (ardop, mercury, xara); every setting it had put back after, and APP > Mail's
    recent gateways (a PatSession puts the test call on top)"""
    kept, recent = {}, {}
    try:
        for n, r in o.r.items():
            recent[n] = ssh(r.host, f"cat {WL_RECENT} 2>/dev/null || echo NONE", 20)
            st, conf = _winlink(r)
            kept[n] = {k: conf.get(k) for k in ("enabled", "modem_choice") if k in conf}
            st, _ = _winlink(r, {"enabled": True, "modem_choice": modem})
            if st != 200:
                raise Refused(f"{n}: Winlink not switched on ({st})")
        time.sleep(6)
        yield
    finally:
        for n, r in o.r.items():
            if kept.get(n):
                body = {k: (v in (True, "1", 1)) if k == "enabled" else v for k, v in kept[n].items() if v is not None}
                if body:
                    try:
                        st, now = _winlink(r, body)
                        if any(now.get(k) != v for k, v in body.items()):
                            o.st.ev(f"{n}: Winlink NOT as found after the test: {body} asked, "
                                    f"{({k: now.get(k) for k in body})} now", "bad")
                    except Refused as e:
                        o.st.ev(f"{n}: Winlink NOT put back ({e}): {body} to set by hand", "bad")
            if n in recent:
                was = recent[n]
                try:
                    if was.strip() == "NONE":
                        ssh(r.host, f"rm -f {WL_RECENT}", 20)
                    else:
                        bench.ssh(r.host, f"cat > {WL_RECENT}.t && mv {WL_RECENT}.t {WL_RECENT}", 20, input=was)
                except Refused as e:
                    o.st.ev(f"{n}: APP > Mail's recent gateways NOT put back ({e})", "bad")


def t_winlink_ardop(o, s, l):
    """Pat on s calls l over ARDOP on 14.105 from APP > Mail's path (PatSession; l never answers: its P2P listening
    off): s's dial goes there in USB-D, it keys at 0.1 W (meters near 0.1 W frame after frame: the shared TX level),
    l's ardopcf logs frames; the dial turned mid-call stops its TX at once ('Winlink: TX stopped, it left ...'); after
    the session s goes back"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[l]
    res = {}
    try:
        ensure_alike(o, WL_DIAL, 3)
        classic(S)          # the Sentry holds the radio receive only until A/B arms it: Winlink keys from classic
        with winlink_on(o, "ardop"):
            with session(o, "winlink-ardop", [s], WL_DIAL) as watch:
                role(o, s)
                n0 = ssh(L.host, "wc -c < /run/winlink/ardopcf.log 2>/dev/null || echo 0").strip() or "0"
                t0 = time.time()
                ps = PatSession(S, f"ardop:///{call}?freq={WL_DIAL // 1000}")
                try:
                    try:
                        S.wait("its first frame", lambda: S.tx or watch.why or not ps.alive(), 60)
                        if not S.tx and not ps.alive():
                            raise Refused(f"Pat ended the connection without keying: {ps.done or ps.other}")
                    except Refused:
                        res["pat_log"] = _pat_logs(S)        # run 31: Pat closed the connect without an answer
                        raise
                    res["first_frame_s"] = round(time.time() - t0, 1)
                    res["dial"] = (int(S.state.get("freq", 0)), int(S.state.get("mode", -1)))
                    S.wait("the frame's end", lambda: not S.tx or watch.why, 15)
                    S.wait("its next frame", lambda: S.tx or watch.why, 20)
                    with bench.apart({"freq": bench.STEPPED}, "winlink-ardop: the sender's dial turned a step "
                                                              "mid-frame to stop it"):
                        try:
                            S.send("knob main 1")
                            t_k = time.time()
                            S.wait("off the air at once", lambda: not S.tx, 2.0)
                            res["stop_s"] = round(time.time() - t_k, 2)
                            try:
                                res["stop_said"] = wait_msg(S, t_k, r"Winlink: TX stopped", 3)
                            except Refused:
                                res["stop_said"] = None
                        finally:
                            ps.close()                       # Pat's tune back follows the end of the session
                            res["pat"] = ps.result()
                            bench.ensure(S, {"freq": WL_DIAL})
                finally:
                    ps.close()
                    res["pat"] = ps.result()
                time.sleep(5)
                # each key's first half second is its key-up (the X6100's factory base read 0.5 W there at 0.1 W,
                # 2026-11-26): kept apart; the rest of the key is the level judged
                spans = [(a, b or time.time()) for a, b in tx_spans(S, t0)]
                fr = t_frames(S, t0)
                res["keyup"] = [round(x[1], 2) for x in fr if any(a <= x[0] < a + 0.5 for a, b in spans)][:12]
                res["watts"] = [round(x[1], 2) for x in fr if any(a + 0.5 <= x[0] <= b for a, b in spans)][:24]
                n1 = ssh(L.host, "wc -c < /run/winlink/ardopcf.log 2>/dev/null || echo 0").strip() or "0"
                res["l_log_grew"] = int(n1) > int(n0)
        if res["dial"] != (WL_DIAL, 3):
            return False, f"Pat's call left {s} on {res['dial']}, not 14.105 USB-D", res
        lit = sorted(x for x in res["watts"] if x > 0)
        if max(res["watts"] + res["keyup"] + [0]) > 1.0:
            return False, f"{s} over 1 W on the cable: {res['keyup']} at key-up, {res['watts']} after", res
        if not lit or not 0.03 <= lit[len(lit) // 2] <= 0.15:
            return False, f"{s}'s meters over the frames {res['watts']} (key-up {res['keyup']}): not near 0.1 W", res
        if not res["l_log_grew"]:
            return False, f"{l}'s ardopcf.log did not grow while {s} called", res
        return True, f"{s} called over ARDOP on 14.105 USB-D at {lit[len(lit) // 2]} W (median; key-up " \
                     f"{max(res['keyup'] + [0])} W at most) {res['first_frame_s']} s after Connect (Pat asked " \
                     f"{[p['kind'] for p in res['pat']['prompts']] or 'nothing'}), {l}'s modem heard it; the dial " \
                     f"turned: off the air in {res['stop_s']} s ({res['stop_said']!r})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


def t_winlink_mercury_xara(o, s, l):
    """Mercury and XARA (Pat's VARA HF transport, varahf://, station to station) from APP > Mail's path: s calls l,
    keys at 0.1 W (no key over 40/30 s), l hears it (its S-meter 10 dB up while s keys; its xara.log grows, a Mercury
    that does not listen logs nothing of a CALL); neither answers"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[l]
    res = {}
    try:
        for modem, log in (("mercury", "mercury.log"), ("xara", "xara.log")):
            ensure_alike(o, WL_DIAL, 3)
            classic(S)      # the Sentry holds the radio receive only until A/B arms it: Winlink keys from classic
            with winlink_on(o, modem):
                with session(o, f"winlink-{modem}", [s], WL_DIAL) as watch:
                    role(o, s)
                    n0 = int(ssh(L.host, f"wc -c < /run/winlink/{log} 2>/dev/null || echo 0").strip() or 0)
                    t0 = time.time()
                    ps = PatSession(S, f"varahf:///{call}?freq={WL_DIAL // 1000}")
                    try:
                        try:
                            S.wait("its call on the air", lambda: S.tx or watch.why or not ps.alive(), 60)
                            if not S.tx and not ps.alive():
                                raise Refused(f"{modem}: Pat ended the connection without keying: "
                                              f"{ps.done or ps.other}")
                        except Refused:
                            res[f"{modem}_pat_log"] = _pat_logs(S)
                            raise
                        first = round(time.time() - t0, 1)
                        S.wait("its end", lambda: not S.tx or watch.why, 16)
                        try:                                 # the call once more: one CALL may go unheard
                            S.wait("its call again", lambda: S.tx or watch.why, 20)
                            S.wait("its end", lambda: not S.tx or watch.why, 16)
                        except Refused:
                            pass
                    finally:
                        ps.close()
                        res[f"{modem}_pat"] = ps.result()
                        time.sleep(4)
                    n1 = int(ssh(L.host, f"wc -c < /run/winlink/{log} 2>/dev/null || echo 0").strip() or 0)
                    # l heard the call: its S-meter while s keyed, against its quiet before the first key (a Mercury
                    # that does not listen drops a CALL without a line in its log, arq_fsm.c: its log is no witness)
                    spans = [(a, b or time.time()) for a, b in tx_spans(S, t0)]
                    quiet = meters(L, t0, spans[0][0] - 0.3) if spans else []
                    heard = [v for a, b in spans for v in meters(L, a + 0.3, b)]
                    up = round(sum(heard) / len(heard) - sum(quiet) / len(quiet), 1) if heard and quiet else None
                    res[modem] = {"keyed": len(spans), "first_s": first, "l_up_db": up, "l_log_grew": n1 > n0,
                                  "l_keyed": len(tx_spans(L, t0))}
                    if n1 <= n0:
                        res[f"{modem}_l_log"] = ssh(L.host, f"tail -n 8 /run/winlink/{log} 2>/dev/null", 20)[-800:]
        both = {m: res[m] for m in ("mercury", "xara")}
        bad = [m for m in both if not res[m]["keyed"] or res[m]["l_keyed"]]
        if bad:
            return False, f"{bad}: {both}", res
        deaf = [m for m in both if (res[m]["l_up_db"] or 0) < 10]
        logs = [m for m in both if res[m]["l_log_grew"]]
        said = f"Mercury and XARA called (keyed {res['mercury']['first_s']} and {res['xara']['first_s']} s after " \
               f"Connect, {l} silent); {l}'s S-meter up {res['mercury']['l_up_db']} and {res['xara']['l_up_db']} dB " \
               f"while they keyed; {l}'s logs grew for {logs}"
        return (None if deaf else True), said + (f"; {l} did not hear {deaf}" if deaf else ""), res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


WL_TONE_S = 13.0       # winlink-busy's tone: over Pat's account question (its server asked, up to 10 s) and its dial


def t_winlink_busy(o, s, l):
    """Pat listens before it calls: l sends a tone on 14.105 while s's Pat connects from APP > Mail's path: s does not
    key over it; Pat waits for the channel to clear and asks (the busy-channel question, left unanswered); Abort,
    while l still sends, ends the wait at once and nothing goes out after. The bandwidth cap: a call asking 2000MAX
    with the page at 500 goes at 500 (Pat's log). Should s key over the tone after all: off the air at once (its dial
    turned) and the test fails"""
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[l]
    res = {}
    try:
        ensure_alike(o, WL_DIAL, 3)
        classic(S)          # s may key here (the Sentry holds it receive only): Pat's holding off is tested
        with winlink_on(o, "ardop"):
            st, _ = _winlink(S, {"bandwidth": 500})
            got = {}

            def body(watch):
                t0 = time.time()
                th = threading.Thread(target=lambda: bench.tci_send(L, [[1500, WL_TONE_S]], max_s=WL_TONE_S + 1.5,
                                                                    watch=watch))
                th.start()
                time.sleep(1.0)
                ps = PatSession(S, f"ardop:///{call}?freq={WL_DIAL // 1000}&bw=2000MAX")
                try:
                    asked = None
                    end = t0 + WL_TONE_S - 2.5        # Abort while l still sends
                    while time.time() < end and ps.alive() and not watch.why:
                        if S.tx:                      # s on the air over l's tone: off at once
                            S.send("knob main 1")
                            got["keyed_over_s"] = round(time.time() - t0, 1)
                            break
                        if asked is None and "busy-channel" in ps.kinds():
                            asked = time.time()
                            got["busy_asked_s"] = round(asked - t0, 1)
                        if asked is not None and time.time() - asked > 2.0:
                            break                     # 2 s of the wait seen without a key
                        time.sleep(0.05)
                finally:
                    t_a = time.time()
                    ps.close()
                    got["abort_s"] = round(time.time() - t_a, 1)
                    got["pat"] = ps.result()
                th.join(20)
                time.sleep(2.0)                       # the channel clear: no call goes out after the Abort
                got["s_keyed"] = len(tx_spans(S, t0, time.time()))
            H.over(o, l, s, WL_DIAL, 3, "winlink-busy: l's tone", body, pages=False)
            plog = ssh(S.host, "tail -60 /run/winlink/pat-own.log 2>/dev/null")
            lines = got.get("pat", {}).get("log", []) + plog.splitlines()
            res.update(got, cap_line=next((x for x in lines if "500MAX" in x), None),
                       waited=any("Waiting for clear channel" in x for x in lines))
        if res.get("keyed_over_s") is not None or res["s_keyed"]:
            return False, f"{s}'s Pat keyed over {l}'s tone (at {res.get('keyed_over_s')} s, " \
                          f"{res['s_keyed']} overs)", res
        if res.get("busy_asked_s") is None and not res["waited"]:
            return False, f"{s}'s Pat neither keyed nor waited for the channel: {res['pat']['prompts']} " \
                          f"{res['pat']['done']}", res
        if res["abort_s"] > 6:
            return None, f"{s}'s Pat held off while {l} sent, but Abort took {res['abort_s']} s to end its wait", res
        said = f"{s}'s Pat held off while {l} sent (asked about the busy channel {res.get('busy_asked_s')} s in), " \
               f"Abort ended it in {res['abort_s']} s; cap: {res['cap_line']!r}"
        return (True if res["cap_line"] else None), said, res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


# ---- the browser's Panel (headless Chromium, the bench's own session): PTT states, Tap, a stalled link ----

def _page_control(page):
    """Control taken by the page: its session's control button (radio_session.js $ctl: "Control: free", "At the radio
    · Take control", "TCI client · Take control"...); any click but on "In control" sends 'control take'"""
    ctl = page.locator("button.ses", has_text="ontrol")
    for _ in range(3):
        t = ctl.first.inner_text(timeout=3000) if ctl.count() else ""
        if not t or t.startswith("In control"):
            break
        ctl.first.click()
        time.sleep(1.5)


@contextlib.contextmanager
def panel_page(r, path="/panel", wav=None):
    """r's Panel page in headless Chromium with the bench's tx session (its key in localStorage), control taken"""
    from playwright.sync_api import sync_playwright
    tok = json.load(open(os.path.join(bench.STATE, "tokens.json")))[r.name]["tx"]
    args = ["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--autoplay-policy=no-user-gesture-required"]
    if wav:
        args.append(f"--use-file-for-fake-audio-capture={wav}")
    pw = sync_playwright().start()
    br = pw.chromium.launch(headless=True, args=args)
    try:
        ctx = br.new_context()
        ctx.add_init_script(f"try{{localStorage.setItem('x6100session','{tok}');localStorage.setItem('x6100name','bench-tx')}}catch(e){{}}")
        page = ctx.new_page()
        page.goto(f"http://{r.host}{path}", wait_until="domcontentloaded", timeout=20000)
        time.sleep(3)
        _page_control(page)
        # the PTT options are in a closed <details>: opened, so its selects can be chosen
        page.evaluate("document.querySelectorAll('details.ptto').forEach(d => d.open = true)")
        yield page, ctx
    finally:
        br.close()
        pw.stop()


def _ptt_text(page):
    try:
        return page.locator("#pttbtn").inner_text(timeout=1000)
    except Exception:  # noqa: BLE001
        return ""


def _page_why(page):
    """What the Panel says about control and the PTT: the control button, the PTT's text (its refusal on its second
    line), an open popup's text (a refused press opens Take control over the buttons)"""
    try:
        return page.evaluate("""() => {
            const t = s => { const e = document.querySelector(s); return e ? e.innerText.replace(/\\s+/g, ' ').trim() : null; };
            const pop = document.querySelector('.spop:not([hidden]) .sbox');
            const ses = [...document.querySelectorAll('button.ses')].map(b => b.innerText.trim());
            return {ptt: t('#pttbtn'), popup: pop ? pop.innerText.replace(/\\s+/g, ' ').trim().slice(0, 200) : null,
                    control: ses.filter(x => /ontrol/.test(x))};
        }""")
    except Exception as e:  # noqa: BLE001
        return {"err": str(e)[:100]}


def t_panel_ptt(o, s, l):
    """The Panel's PTT (s on the classic screen, 0.1 W, l listening): held: 'Keying...' then 'ON AIR' within 1.5 s,
    released: back; Tap on, tap off: a tap keys, the next unkeys, the option kept after a reload; keyed from another
    client: 'TX (not this browser)'; a stalled link (the page offline) ends the over within 3 s"""
    S = o.r[s]
    res = {}
    try:
        ensure_alike(o, F_USB, 2, agc=2)                # the Watch compares AGC too (an earlier test left 0)
        classic(S)
        with session(o, "panel-ptt", [s], F_USB) as watch:
            role(o, s)
            with panel_page(S) as (page, ctx):
                btn_ = page.locator("#pttbtn")
                # in view first: the mouse presses where the box is in the window, and the PTT sits below the
                # 720-pixel fold (runs 28-30: the press reached nothing, no refusal, the page in control)
                btn_.scroll_into_view_if_needed()
                box = btn_.bounding_box()
                page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
                t0 = time.time()
                page.mouse.down()
                texts = []
                while time.time() - t0 < 2.0:
                    texts.append(_ptt_text(page))
                    time.sleep(0.1)
                on_air_s = next((round(i * 0.1, 1) for i, t in enumerate(texts) if "ON AIR" in t), None)
                res.update(states=sorted(set(t.replace("\n", " / ") for t in texts if t)), on_air_s=on_air_s,
                           keyed=bool(S.tx), page=_page_why(page))
                page.mouse.up()
                S.wait("off the air", lambda: not S.tx, 4)
                page.select_option('[data-o="tap"]', "1")
                btn_.click()
                S.wait("on the air by a tap", lambda: S.tx or watch.why, 4)
                time.sleep(1.0)
                btn_.click()
                S.wait("off by the next tap", lambda: not S.tx, 4)
                page.reload()
                time.sleep(3)
                page.evaluate("document.querySelectorAll('details.ptto').forEach(d => d.open = true)")
                res["tap_kept"] = page.eval_on_selector('[data-o="tap"]', "e => e.value") if \
                    page.locator('[data-o="tap"]').count() else None
                page.select_option('[data-o="tap"]', "0")
                hold = int(time.time())
                S.send(f"key PTT 1 {hold}")
                S.wait("keyed by the bench's other client", lambda: S.tx, 4)
                time.sleep(1.0)
                res["other_client"] = _ptt_text(page)
                S.send("key PTT 0")
                S.wait("off", lambda: not S.tx, 4)
                _page_control(page)                     # the bench's key took control from the page
                btn_.scroll_into_view_if_needed()
                box = btn_.bounding_box()
                page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
                page.mouse.down()
                S.wait("held from the page", lambda: S.tx or watch.why, 4)
                ctx.set_offline(True)
                t_off = time.time()
                S.wait("the lease ends", lambda: not S.tx, 5)
                res["stall_unkey_s"] = round(time.time() - t_off, 1)
                ctx.set_offline(False)
                page.mouse.up()
        bad = []
        if res["on_air_s"] is None or res["on_air_s"] > 1.5:
            bad.append(f"ON AIR at {res['on_air_s']} s ({res['states']})")
        if res["tap_kept"] != "1":
            bad.append(f"Tap option after reload {res['tap_kept']!r}")
        if "not this browser" not in (res["other_client"] or ""):
            bad.append(f"keyed by another client the button read {res['other_client']!r}")
        if res["stall_unkey_s"] > 3.5:
            bad.append(f"stalled link unkeyed after {res['stall_unkey_s']} s")
        if bad:
            return False, "; ".join(bad), res
        return True, f"PTT states {res['states']}, ON AIR at {res['on_air_s']} s; Tap kept; 'TX (not this browser)'; " \
                     f"a stalled page unkeyed in {res['stall_unkey_s']} s", res
    except Exception as e:  # noqa: BLE001
        return False, f"{type(e).__name__}: {e}", res
    finally:
        home(o, S)


def t_panel_3min(o, s, l):
    """Three minutes on the air at most whatever happens: a 3-minute key-down at 0.1 W is past the Watch's 16 s, the
    owner's word first (owner_ok_3min)"""
    if not os.path.exists(os.path.join(bench.STATE, "owner_ok_3min")):
        raise Refused("needs the owner's word: a 3-minute key-down at 0.1 W into the pads (owner_ok_3min)")
    return None, "the word is there; the long key test itself is not written yet", {}


# ---- PSK Reporter: its report caught on the radio itself ----

PSKR_HOST = "report.pskreporter.info"
PSKR_CATCH = r'''
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 4739))
s.settimeout(float(sys.argv[1]))
end = time.time() + float(sys.argv[1])
with open("/tmp/pskr_catch.bin", "wb") as f:
    while time.time() < end:
        try:
            d, a = s.recvfrom(65535)
        except socket.timeout:
            break
        f.write(len(d).to_bytes(4, "big") + d)
        f.flush()
'''


def t_pskreporter(o, s, l):
    """PSK Reporter (Report decodes) with the report kept on the radio: l's /etc/hosts points report.pskreporter.info
    at itself and its real addresses are routed nowhere (both taken back after), a catcher on 127.0.0.1:4739; s sends
    FT8 CQs over the cable; within its 5-minute batch l's report arrives at the catcher with l's test call and s's"""
    S, L = o.r[s], o.r[l]
    res = {}
    import socket
    real = sorted({a[4][0] for a in socket.getaddrinfo(PSKR_HOST, 4739, socket.AF_INET)})
    hosts0 = ssh(L.host, "cat /etc/hosts")
    try:
        ssh(L.host, f"cp /etc/hosts /tmp/hosts.bench && echo '127.0.0.1 {PSKR_HOST}' >> /etc/hosts && "
                    + " && ".join(f"ip route add blackhole {ip}/32" for ip in real))
        # the name as the GUI's getaddrinfo finds it (BusyBox has no getent: run 30 refused on that): /etc/hosts first
        check = ssh(L.host, f"python3 -c \"import socket; print(socket.gethostbyname('{PSKR_HOST}'))\"; "
                            f"ip route show | grep blackhole")
        if "127.0.0.1" not in check:
            raise Refused(f"{l}: {PSKR_HOST} not pointed at itself ({check!r}): nothing sent")
        subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", f"root@{L.host}", "cat > /tmp/pskr_catch.py"],
                       input=PSKR_CATCH, text=True, check=True)
        ssh(L.host, "rm -f /tmp/pskr_catch.bin; (python3 /tmp/pskr_catch.py 420 >/dev/null 2>&1 &)")
        with cfg_kept(L, {"pskreporter": 1}):
            with ft8_pair(o, "pskreporter", senders=[s]) as watch:
                role(o, s)
                n0 = len(L.rows)
                end = time.time() + 400
                cqs = 0
                while time.time() < end and not watch.why:
                    H.ft8_send(o, S, L, watch, "ft8 cq", "pskreporter: CQ", F_FT8)
                    cqs += 1
                    got = ssh(L.host, "ls -l /tmp/pskr_catch.bin 2>/dev/null | awk '{print $5}'").strip()
                    if got and int(got) > 0:
                        break
                    time.sleep(30)
                res["cqs"] = cqs
                res["decoded"] = sum(1 for r in L.rows[n0:] if H.TEST_CALLS[s] in bench.msg(r))
        raw = subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", f"root@{L.host}", "cat /tmp/pskr_catch.bin"],
                             capture_output=True, timeout=30).stdout
        res["caught_bytes"] = len(raw)
        txt = raw.decode("latin-1")
        res.update(has_receiver=H.TEST_CALLS[l] in txt, has_sender=H.TEST_CALLS[s] in txt, has_ft8="FT8" in txt)
        if not raw:
            return False, f"no report caught in {res['cqs']} CQs ({res['decoded']} decoded by {l})", res
        if not (res["has_receiver"] and res["has_sender"]):
            return False, f"a report caught ({len(raw)} bytes) without both test calls ({res})", res
        return True, f"{l}'s report caught on itself ({len(raw)} bytes): receiver {H.TEST_CALLS[l]}, heard " \
                     f"{H.TEST_CALLS[s]}, FT8 {res['has_ft8']}; nothing left the radio", res
    except Refused as e:
        return False, str(e), res
    finally:
        ssh(L.host, "pkill -f pskr_catch.py; [ -f /tmp/hosts.bench ] && cp /tmp/hosts.bench /etc/hosts; "
                    + "; ".join(f"ip route del blackhole {ip}/32 2>/dev/null" for ip in real)
                    + "; rm -f /tmp/hosts.bench /tmp/pskr_catch.py /tmp/pskr_catch.bin")
        if ssh(L.host, "cat /etc/hosts") != hosts0:
            o.st.ev(f"{l}: /etc/hosts not as found after the PSK Reporter test", "bad")


# ---- the X6100's TX Cal: IQ offsets per band; the patched-base rows hidden on Xiegu's factory base ----

def _bin_db(L, hz, t0, t1, tol=25):
    """The listener's spectrum frames in t0..t1: the strongest bin within tol Hz of hz, dB over each frame's median,
    averaged (the carrier left at the dial, under the tone's level)"""
    vals = []
    for t, cen, wid, v in list(L.r.spectra):
        if not (t0 <= t <= t1) or not v:
            continue
        per = wid / len(v)
        med = sorted(v)[len(v) // 2]
        near = [v[i] for i in range(len(v)) if abs((cen - wid / 2 + (i + 0.5) * per) - hz) <= max(tol, per)]
        if near:
            vals.append(max(near) - med)
    return round(sum(vals) / len(vals), 1) if vals else None


def t_iq_offsets(o, s, l):
    """The X6100's IQ offsets (TX Cal, per band): set on 20 m and read back; a carrier at 0.1 W into the X6200: the
    carrier at the dial measured with the offsets as found and with them moved (its level changes); on a band never
    calibrated (17 m here, if so) they read 0. Put back after"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        ensure_alike(o, F_USB, 2)
        io0, qo0 = int(S.state.get("txio", 0)), int(S.state.get("txqo", 0))
        levels = {}
        for io, qo in ((io0, qo0), (io0 + 40, qo0 - 40)):
            S.send(f"set txio {io}")
            S.send(f"set txqo {qo}")
            S.wait("the offsets", lambda: (int(S.state.get("txio", 9999)), int(S.state.get("txqo", 9999))) == (io, qo), 4)
            got = {}

            def body(watch):
                _, t0, t1 = bench.tci_send(S, [[1000, 3.0]], watch=watch)
                got["t"] = (t0, t1)
            H.over(o, s, l, F_USB, 2, f"iq-offsets {io},{qo}", body)
            levels[(io, qo)] = _bin_db(L, F_USB, got["t"][0] + 0.5, got["t"][1])
        S.send(f"set txio {io0}")
        S.send(f"set txqo {qo0}")
        res["carrier_db"] = {f"{k[0]},{k[1]}": v for k, v in levels.items()}
        vals = list(levels.values())
        if None in vals:
            return None, f"the carrier at the dial not seen in {l}'s spectrum ({res})", res
        if abs(vals[0] - vals[1]) < 2:
            return False, f"moving the offsets by 40 did not change the carrier ({res})", res
        return True, f"the X6100's IQ offsets act: carrier {res['carrier_db']} dB at the dial in {l}'s spectrum", res
    except Refused as e:
        return False, str(e), res


# The rows of Settings > TX Cal and Audio > TX AUDIO only a patched base has, each shown by its own capability
# (settings_window.cpp: radio_caps() & HW_CAP_*; radio_types.h's values)
PATCHED_ROW_CAPS = {"TX IQ balance": 1 << 20, "band output gain": 1 << 5, "codec gain": 1 << 6,
                    "compressor threshold/makeup": 1 << 9, "TX filter": 1 << 10, "FM emphasis": 1 << 11}


def t_patched_rows(o, s, l):
    """On Xiegu's factory base (the bench X6100 since 2026-11-21) the patched-base rows (codec and band output gain,
    compressor threshold/makeup, TX filter, FM emphasis, IQ balance) are hidden. Each row is shown by its own
    capability alone (settings_window.cpp), so the GUI's capabilities (the remote's read-only radio_caps, from
    2026-11-26) judge it: on a factory base none of them; on a patched one all"""
    S = o.r[s]
    res = {"base": ssh(S.host, "grep -h -o 'base[^,]*V1[.0-9a-z_-]*' /mnt/app_logs/start_trace.txt 2>/dev/null | tail -1").strip()}
    try:
        caps = bench.cfg_get(S, "radio_caps")
        if caps in (None, ""):
            return None, f"{s}: no radio_caps from the remote (a GUI before it); base {res['base'] or '?'}", res
        caps = int(caps)
        res["caps"] = caps
        res["rows"] = {k: bool(caps & v) for k, v in PATCHED_ROW_CAPS.items()}
        factory = "V1.1.9" in res["base"]
        shown = [k for k, v in res["rows"].items() if v]
        if factory and shown:
            return False, f"{s}'s factory base ({res['base']}): the GUI would show {shown}", res
        if not factory and len(shown) < len(PATCHED_ROW_CAPS):
            return False, f"{s}'s base {res['base'] or '(not read)'} is not the factory one, yet hides " \
                          f"{[k for k in PATCHED_ROW_CAPS if k not in shown]}", res
        return True, (f"{s}'s factory base ({res['base']}): none of the patched rows ({', '.join(PATCHED_ROW_CAPS)})"
                      if factory else f"{s}'s patched base {res['base']}: all the patched rows"), res
    except Refused as e:
        return False, str(e), res


# ---- Field Ops' reference lists from the internet (the owner, 2026-11-25: a download is no issue, test it) ----

FIELD_MTIME = "python3 -c \"import os;print(int(os.path.getmtime('/mnt/field/refs.tsv')))\" 2>/dev/null || echo 0"
# x6100-field holds /mnt/field/.update.lock with flock while it runs and leaves the empty file: free when it can be taken
FIELD_LOCK_HELD = ("python3 -c \"import fcntl;f=open('/mnt/field/.update.lock','a');"
                   "fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB);print('free')\" 2>/dev/null || echo held")


def t_field_update(o, s, l):
    """Update list (Field Ops page 2, or the web Field Ops page) on s: the web's POST /api/field/update, followed by
    GET /api/field to its end ('<list> (n/10): x / y MB' while it runs); then its result no error, the list there
    (refs.tsv with parks and summits counted, its time new), no lock or part file left. Nothing tuned, nothing sent"""
    import suite_air_b as ab
    S = o.r[s]
    ip = ab.web_ip(S.host)
    res = {"progress": []}
    try:
        with o.step(f"{s}: the lists as found") as st:
            st0, b0 = ab.http(ip, "/api/field")
            if st0 != 200:
                raise Refused(f"{s}'s /api/field answered {st0}")
            before = json.loads(b0)
            t_file0 = ssh(S.host, FIELD_MTIME).strip() or "0"
            res["before"] = {"refs": before.get("refs"), "mtime": int(t_file0)}
            st.confirm(str(res["before"]))
        with o.step(f"{s}: Update list started (the web page's), followed to its end") as st:
            t0 = time.time()
            code, b1 = ab.http(ip, "/api/field/update", method="POST", body={})
            if code == 409:
                res["already_running"] = True
            elif code != 200:
                raise Refused(f"{s}: the update not started ({code} {b1[:80]})")
            end = time.time() + 1500
            upd = {}
            while time.time() < end:
                time.sleep(3)
                code, b = ab.http(ip, "/api/field")
                if code != 200:
                    continue
                upd = json.loads(b).get("update") or {}
                p = upd.get("progress")
                if p and (not res["progress"] or res["progress"][-1] != p):
                    res["progress"].append(p)
                if not upd.get("running"):
                    break
            else:
                raise Refused(f"{s}: the update still running after {time.time() - t0:.0f} s ({res['progress'][-1:]})")
            res.update(result=upd.get("result"), secs=round(time.time() - t0))
            st.confirm(f"{res['secs']} s, {len(res['progress'])} progress steps, result {res['result']!r}")
        after = json.loads(ab.http(ip, "/api/field")[1])
        refs = after.get("refs") or {}
        t_file1 = int(ssh(S.host, FIELD_MTIME).strip() or 0)
        left = ssh(S.host, "ls -a /mnt/field | grep -E '^\\.update\\.download$|\\.tmp$' || true").split()
        held = ssh(S.host, FIELD_LOCK_HELD).strip()
        if held != "free":
            left.append(f".update.lock ({held})")
        res.update(refs=refs, mtime=t_file1, left=left)
        if (res["result"] or "").lower().startswith("error"):
            return False, f"{s}'s update ended with {res['result']!r} after {res['secs']} s", res
        if not refs or not refs.get("parks") or not refs.get("summits"):
            return False, f"{s}: no lists after the update ({refs})", res
        if left:
            return False, f"{s}: the update left {left} behind", res
        new = t_file1 > res["before"]["mtime"]
        return True, f"{s}: lists updated in {res['secs']} s ({len(res['progress'])} progress steps, " \
                     f"{res['progress'][-1] if res['progress'] else 'no download needed'}), result {res['result']!r}: " \
                     f"{refs.get('parks')} parks, {refs.get('summits')} summits, file {'new' if new else 'unchanged (the same lists)'}", res
    except Refused as e:
        return False, str(e), res


# ---- the CW Keyboard's on-screen keyboard and Live ----

def t_cwkbd_live(o, s, l):
    """The CW Keyboard without a keyboard: an MFK press with the line empty opens the on-screen keyboard; with Live on
    (its last page, off by default) each key pressed is keyed at once, no OK: four presses on '5' key '5555', the
    listener decodes at least two of them (one character alone is too little for its decoder to print)"""
    S, L = o.r[s], o.r[l]
    res = {}
    try:
        ensure_alike(o, F_CW, 4, agc=2)
        with cfg_kept(S, {"cw_live": 1}):
            open_app(S, 117, "CW Keyboard")
            bench.ensure(S, {"freq": F_CW, "mode": 4})
            with session(o, "cwkbd-live", [s], F_CW) as watch:
                role(o, s)
                L.cw = ""
                press(S, "MFK", ("CW Keyboard",), rest=1.0)       # the on-screen keyboard (its digits first)
                snap(o, f"cwkbd-live {s}: the on-screen keyboard")
                turn(S, "mfk", 4, ("CW Keyboard",), rest=0.8)     # '5': five dits
                t0 = time.time()
                for _ in range(4):
                    press(S, "MFK", ("CW Keyboard",), rest=1.0)
                time.sleep(3.0)
                res["keyed"] = len(tx_spans(S, t0))
            res["decoded"] = cw_settled(L)
        letters = re.sub(r"[^A-Z0-9]", "", res["decoded"])
        top = max((letters.count(c) for c in set(letters)), default=0)
        if res["keyed"] < 2 or top < 2:
            return False, f"Live: four key presses keyed {res['keyed']} over(s), {l} decoded {res['decoded']!r}", res
        return True, f"Live keyed each on-screen key at once ({res['keyed']} overs): {l} decoded {res['decoded']!r} " \
                     f"with no OK", res
    except Refused as e:
        return False, str(e), res
    finally:
        home(o, S)


# ---- what each test covers (features.json ids), its rows ----

COVERS = {
    "cw-dec-snr": (["cw-decoder-dec-snr"], []),
    "cw-parse": (["cw-parse"], []),
    "cw-zap": (["cw-zap"], []),
    "cw-tuner": ([], ["cw-tuning-indicator"]),
    "cw-peak-q": (["cw-peak-q"], []),
    "update-check-off": ([], ["system-update-daily-check"]),   # its off switch only: no check is made to see it
    "cw-key-train": ([], ["cw-key-train"]),          # its message and ON AIR at 0 W: the KEY jack, a paddle
    "cw-own-carrier": ([], ["cw-own-carrier-on-vfo-line", "cw-alc-grey"]),
    "cw-own-decode": (["cw-decoder-own-keying"], []),
    "cw-log-qso": (["cw-log-qso-button"], ["log-quick-qso-decoder-call"]),
    "cw-msg": (["cw-msg-tx", "cw-msg-breakin-raised"], ["cw-msg-tokens"]),
    "cw-his": (["cw-msg-tokens"], []),
    "cw-beacon": (["cw-beacon"], ["cw-beacon-start-until"]),
    "cw-beacon-stops": (["cw-beacon-stops"], []),
    "qrss3": (["qrss3-send", "qrss3-stops"], []),
    "qrss3-key-safety": (["qrss3-key-safety"], []),
    "cwkbd-live": (["cwkbd-live"], ["cwkbd-onscreen-keyboard"]),
    "bands-beacon": (["bands-test-beacon"], []),
    "field-beacon": (["field-test-beacon-send", "field-test-beacon-stop"], []),
    "field-cq": (["field-cq-cw"], ["field-cq-ssb-voice"]),
    "prop-beacon": (["prop-beacons-heard"], []),
    "scanner": (["scanner-stop-and-hang", "scanner-holds-on-tx"], []),
    "ft8-cq-grid": ([], ["ft8-grid-4char", "ft8-cq-modifier", "ft8-compound-call"]),   # a standard call: not the bench's
    "ft8-retries": (["ft8-retries"], []),
    "ft8-auto-off": (["ft8-auto-off-manual"], []),
    "ft8-compound": (["ft8-compound-call"], []),
    "ft8-list-look": (["ft8-for-you-strip"], ["ft8-list-info-display"]),
    "ft8-hound": ([], ["ft8-hound-mode"]),
    "js8-speed": (["js8-speed-decode"], []),
    "js8-groups": (["ghostnet-gstflash", "js8-x6100-group"], []),
    "ghostnet": (["ghostnet-auto-group-queries", "ghostnet-message-log"], []),
    "js8-look": (["js8-alerts", "js8-log-qso"], []),
    "js8-time-sync": (["js8-time-sync"], []),
    "sstv-rx": (["sstv-manual-rx", "sstv-denoise"], []),
    "sstv-image-only": (["sstv-tx-image-only-repeat"], []),
    "sstv-halt-hold": (["sstv-tx-halt", "sstv-tx-freq-locked", "radio-core-sstv-tx-hold"], []),
    "wefax-look": (["wefax-display-inverted"], []),
    "psk-look": (["psk-status-button"], ["psk-dx-call-rst"]),
    "rade-rx": (["rade-rx-decode"], []),
    "transverter": (["tx-transverter-power-limit", "transverter-no-internal-atu"], ["transverter-limit-held-in-tx"]),
    "zero-power": (["digi-tx-zero-power-warning", "digital-tx-0w-message"], []),
    "agc-knee": (["settings-audio-agc"], []),
    "cessb": (["settings-txcal-cessb"], []),
    "moni": ([], ["tx-moni-level"]),
    "mic-gain": ([], ["tx-mic-gain-input"]),
    "tx-panel": (["settings-display-tx-panel"], []),
    "screensaver-tx": ([], ["display-screensaver-tx"]),
    "stuck-tx-log": (["system-stuck-tx-watchdog"], []),
    "long-press-busy": (["keys-long-press-by-event-time"], []),
    "walkie": (["app-walkie-ptt"], []),
    "walkie-atu": (["app-walkie-atu-tune"], []),
    "g-lamp": (["g-lamp-carrier", "g-lamp"], []),
    "a-tx-rows": (["sentry-a-tx-rows"], []),
    "scancfg-priority": (["scancfg-priority", "scancfg-f3-skip"], []),
    "iradio-tx": (["iradio-stops-on-tx"], []),
    "voice-msg": (["voice-msg-tx"], []),
    "winlink-ardop": (["winlink-ardop-call-keys", "winlink-tx-stopped-when-left", "winlink-tx-level-shared"], []),
    "winlink-mercury-xara": (["winlink-mercury-call"], ["winlink-xara-call-and-id"]),
    "winlink-busy": (["winlink-ardop-bandwidth-cap"], ["winlink-busy-and-password-prompts"]),
    "panel-ptt": (["panel-ptt-ui-states", "ptt-option-tap", "link-stall-ptt-ends"], []),
    "panel-3min": (["panel-ptt-3min"], []),
    "pskreporter": (["pskreporter-upload", "settings-cluster-psk-reporter"], []),
    "field-update-lists": (["field-update-lists"], []),
    "iq-offsets": (["tx-iq-offsets", "settings-txcal-iq-offsets"], []),
    "patched-rows": ([], ["tx-patched-base-levels", "tx-patched-base-processing", "tx-iq-balance",
                          "tx-x6100-gcfix5-pause-hold", "settings-audio-tx-audio-x6100", "settings-txcal-iq-balance",
                          "settings-txcal-output-codec-gain"]),
}

# (name, fn, est s, both ways?, "what"): both ways = "name x6100>x6200" and "name x6200>x6100", else as named
PAIRS = [
    ("cw-dec-snr", t_cw_dec_snr, 90, "Dec SNR 5 copies a weak signal, 30 misses it, 5 again copies"),
    ("cw-parse", t_cw_parse, 60, "run-together CW printed split with Parse on, run together off"),
    ("cw-zap", t_cw_zap, 90, "200 Hz off, ZAP tunes onto the station within 10 Hz, in CW and CW-R"),
    ("cw-tuner", t_cw_tuner, 70, "the tuning bars 30 Hz off in CW and CW-R (pictures)"),
    ("cw-key-train", t_cw_key_train, 60, "KEY Train: TCI CW at 0 W, nothing heard; Train off: decoded"),
    ("cw-own-carrier", t_cw_own_carrier, 50, "the sender's TX meters and ALC; its classic screen on the air (picture)"),
    ("cw-own-decode", t_cw_own_decode, 40, "KEY Train, 25 WPM: the sender's TCI CW 'VVV TEST' read back by its "
     "own CW decoder from the first word, at the bench's VOL; its own audio of it kept"),
    ("cw-log-qso", t_cw_log_qso, 70, "the decoder's confirmed call logged by Log QSO (CW 1:2)"),
    ("cw-msg", t_cw_msg, 60, "MSG > CW 'TEST {CALL} TEST': decoded with the call, one over through the gaps"),
    ("cw-his", t_cw_msg_his, 80, "{HIS}: the call the decoder confirmed sent back"),
    ("cw-beacon", t_cw_beacon, 90, "the beacon: 10 s pause, Until 2 sends, then 'Beacon over: 2 sends'"),
    ("cw-beacon-stops", t_cw_beacon_stops, 70, "a beacon stopped in its pause by rigctl's T 1 / T 0"),
    ("qrss3", t_qrss3, 70, "QRSS3 on 30 m: a 3 s dot on 10.140050; retuned mid-send: stopped"),
    ("cwkbd-live", t_cwkbd_live, 60, "CW Keyboard's on-screen keyboard with Live: each key keyed at once"),
    ("bands-beacon", t_bands_beacon, 90, "the Bands app's Test beacon: two presses, decoded, back; ESC: nothing"),
    ("field-beacon", t_field_beacon, 120, "Field Ops' Test beacon: decoded; a third press stops it at once"),
    ("field-cq", t_field_cq, 90, "Field Ops' CQ in a SOTA activation: decoded, CQ again stops it; SSB's voice CQ"),
    ("prop-beacon", t_prop_beacon, 75, "Propagation's Beacons on 20 m score a carrier in a beacon's slot"),
    ("scanner", t_scanner, 120, "the Scanner stops on a carrier and goes on after Hang; frozen while keyed"),
    ("ft8-cq-grid", t_ft8_cq_grid, 120, "FT8 CQ: the 4-character square, none from 2, the POTA modifier"),
    ("ft8-retries", t_ft8_retries, 150, "an unanswered reply sent Retries (3) times, then dropped"),
    ("ft8-auto-off", t_ft8_auto_off, 120, "Auto off: nothing moves on until the press"),
    ("ft8-compound", t_ft8_compound, 90, "a compound call's CQ; a call too long refused"),
    ("ft8-list-look", t_ft8_list_look, 120, "the list's distance in km and miles, the For you rows and Show held"),
    ("ft8-hound", t_ft8_hound, 90, "Hound calls a Fox at 1000 Hz or more"),
    ("js8-speed", t_js8_speed, 120, "Turbo heard with All speeds, not with My speed"),
    ("js8-groups", t_js8_groups, 120, "@X6100 and @GSTFLASH shown, never answered"),
    ("ghostnet", t_ghostnet, 150, "@GHOSTNET SNR? answered (AUTO); @GHOSTNET TEST logged"),
    ("js8-look", t_js8_look, 180, "JS8's alert word, a two-way QSO's log popup at 73, Save, the ADIF"),
    ("sstv-rx", t_sstv_rx, 90, "Manual RX Robot 8: picture and _dn with Denoise, one without"),
    ("sstv-image-only", t_sstv_image_only, 90, "TX IMAGE ONLY then REPEAT LAST TX: two pictures alike"),
    ("sstv-halt-hold", t_sstv_halt_hold, 60, "knob and BAND_UP held off during TX; Halt TX at once"),
    ("wefax-look", t_wefax_look, 60, "the saved PNG black on white, the screen's fax inverted (Tilt: tools/wefax_plan)"),
    ("psk-look", t_psk_look, 90, "PSK31: the status's audio and quality, the DX call from 'DE <call>'"),
    ("transverter", t_transverter, 70, "the transverter's 0.1 W limit at PWR 1 W; ATU refused there"),
    ("zero-power", t_zero_power, 60, "Line-in 0 still sends: no false 'TX 0 W' (the warning: tools/txlevel)"),
    ("agc-knee", t_agc_knee, 120, "the AGC Knee changes the audio over 0.1/0.3/1 W; where the base ignores them "
     "(the X6200's, Xiegu's own X6100 base), Knee and Slope offered nowhere"),
    ("walkie", t_walkie, 70, "Walkie Talkie mode's PTT with the walkie mic at 0.1 W, the mic back after"),
    ("g-lamp", t_g_lamp, 45, "G's lamps on the air and after, judged on their words in the state"),
    ("scancfg-priority", t_scancfg_priority, 90, "SCAN CFG's PRIORITY catches a carrier; F3 SKIP passes it"),
    ("voice-msg", t_voice_msg, 60, "MSG > Voice on the air on 28.400; refused on 27.185"),
    ("winlink-ardop", t_winlink_ardop, 120, "Pat's ARDOP call keys at 0.1 W on 14.105 USB-D; the dial turned stops it"),
    ("winlink-mercury-xara", t_winlink_mercury_xara, 150, "Mercury and XARA calls keyed, the other silent"),
    ("winlink-busy", t_winlink_busy, 90, "Pat holds off a busy frequency; the 500 bandwidth cap"),
    ("panel-ptt", t_panel_ptt, 90, "the Panel's PTT states, Tap, another client, a stalled link (headless browser)"),
    ("pskreporter", t_pskreporter, 450, "PSK Reporter's report caught on the radio itself, nothing sent out"),
]
SINGLES = [
    # (name, fn, est, (sender, listener) by role names, "what")
    ("cw-peak-q x6100", t_cw_peak_q, 50, (None, "A"), "the X6100's CW peak and its Q in its receive noise, "
     "nothing sent; on a base without the filter (Xiegu's own), the GUI assumes none and offers none"),
    ("cessb x6200>x6100", t_cessb, 60, ("B", "A"), "the X6200's CESSB raises the average power of the same audio; "
     "on a base without it (the X6200's stock one), no CESSB card and the remote refuses it"),
    ("iq-offsets x6100>x6200", t_iq_offsets, 70, ("A", "B"), "the X6100's IQ offsets change its carrier"),
    ("patched-rows x6100", t_patched_rows, 40, ("A", "B"), "the patched-base rows hidden on the factory base"),
    ("moni x6100", t_moni, 20, ("A", "B"), "Moni 30: the base gets it with the speaker up, 0 at VOL 0"),
    ("moni x6200", t_moni, 20, ("B", "A"), "Moni 30: the base gets it with the speaker up, 0 at VOL 0"),
    ("mic-gain x6100", t_mic_gain, 20, ("A", "B"), "the mic gains read back, Input steps the state's mic"),
    ("mic-gain x6200", t_mic_gain, 20, ("B", "A"), "the mic gains read back, Input steps the state's mic"),
    ("tx-panel x6100", t_tx_panel, 40, ("A", "B"), "Popups TX on and off while keyed: the panel and the red frames, judged on the pictures"),
    ("tx-panel x6200", t_tx_panel, 40, ("B", "A"), "Popups TX on and off while keyed: the panel and the red frames, judged on the pictures"),
    ("screensaver-tx x6100", t_screensaver_tx, 100, ("A", "B"), "a TX ends the screensaver within a second"),
    ("screensaver-tx x6200", t_screensaver_tx, 100, ("B", "A"), "a TX ends the screensaver within a second"),
    ("stuck-tx-log", t_stuck_tx_log, 10, ("A", "B"), "every 'TX power set to 0' restored in the base logs"),
    ("update-check-off", t_update_check_off, 5, (None, "A"), "both radios' update check off while a run goes"),
    ("long-press-busy x6100", t_long_press_busy, 30, ("A", "B"), "AB held 1.5 s on the air is a long press"),
    ("long-press-busy x6200", t_long_press_busy, 30, ("B", "A"), "AB held 1.5 s on the air is a long press"),
    ("a-tx-rows x6100", t_a_tx_rows, 30, ("A", "B"), "A's on-air rows through a 5 s over and none after, counted"),
    ("a-tx-rows x6200", t_a_tx_rows, 30, ("B", "A"), "A's on-air rows through a 5 s over and none after, counted"),
    ("qrss3-key-safety", t_qrss3_key_safety, 30, ("B", "A"), "QRSS3's key let go on a crash (the owner's word)"),
    ("js8-time-sync", t_js8_time_sync, 30, ("A", "B"), "JS8's Time Sync with a clock 1 s off (the owner's word)"),
    ("iradio-tx", t_iradio_tx, 30, ("A", "B"), "Internet radio stops on TX (the owner's OK for the internet)"),
    ("walkie-atu", t_walkie_atu, 30, ("A", "B"), "Walkie Talkie's ATU held tunes (the base's own carrier: the "
     "owner's word)"),
    ("rade-rx x6100>x6200", t_rade_rx, 60, ("A", "B"), "RADE: a recorded over decoded (the hand-mic session's)"),
    ("panel-3min", t_panel_3min, 200, ("A", "B"), "three minutes on the air at most (the owner's word)"),
    ("field-update-lists x6100", t_field_update, 300, ("A", "B"), "Field Ops' Update list through the web page: "
     "the ten lists downloaded, counted, nothing left behind"),
    ("field-update-lists x6200", t_field_update, 300, ("B", "A"), "Field Ops' Update list through the web page: "
     "the ten lists downloaded, counted, nothing left behind"),
]


def tests(Test, A, B):
    """orch's Test rows of this module"""
    out = []
    # Every test on the test calls first (once a run, orch.test_calls): run 30 ran after the own calls were put back,
    # and the X6200 has none of its own ("Set your callsign in Settings": the beacons, MSG, Field Ops' CQ all failed)
    def on_calls(fn, s, l):
        def run(o):
            H.test_calls(o)
            return fn(o, s, l)
        return run
    for name, fn, est, what in PAIRS:
        for s, l in ((A, B), (B, A)):
            label = f"{name} {s}>{l}"
            out.append(Test(label, f"{s.upper()} TX, {l.upper()} RX", what, on_calls(fn, s, l), est))
    for name, fn, est, (sr, lr), what in SINGLES:
        s, l = (A if sr == "A" else B if sr else None), (A if lr == "A" else B)
        out.append(Test(name, f"{s.upper()} TX, {l.upper()} RX" if s else f"{l.upper()} RX", what,
                        on_calls(fn, s, l), est))
    return out
