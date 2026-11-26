"""The 1:1 suite's "air_a" batch: the transmitter (power, meters, TX audio, TUNE, keying, the TX lock and its refusals,
the paths), over the cable between the radios (bench.py chain-ok radios: 80 dB of pads): one radio sends, the other
listens, and the result is what the listener heard (the owner, 2026-11-23: "One has to RX the other has to TX!").
Every over at 0.1 W (bench.FIRST_PROBE_W) but where the feature is a power level (then 1 W at most), the path proved
first, every over under bench.Watch, never both keyed, the sender locked again and off the air in a finally; both
radios in sync before each test (suite_more.sync_both) and put back as found after, on the Sentry's page A; each over
begins with the two receivers set alike (the sender takes the listener's settings, or the listener those of an app
open on the sender), what a test sets apart on purpose named to the watch's pair check. Both directions where both
radios can. The listener's hearing is judged on its own spectrum or S-meter (the TX meter reads whole tenths, cut
off). Nothing goes to the internet: a radio whose Wavelog live link is on is not used, the FT8 app's WSJT-X UDP is
off while it is open (/mnt/ft8_udp.conf put back byte for byte), no text sent names a call after "DE" (PSK
Reporter), the FT8 app sends only from a radio with a test call.

    suite.py run air_a [--only TEST,...]

The tests (the features each covers; "part of" where only part of the feature is tried):
  smeter-front-end  a steady 0.1 W tone; the listener's S-meter with PRE on, ATT on, RF gain 40 against as set
                    (settings-txcal-smeter-front-end)
  readings-zero     an over at 0.5 W, then a near-silent one: its first TX meter reading and TCI's first tx_sensors
                    not the last over's (tx-readings-start-zero)
  band-edges        a TCI tone at 7.299 and 14.349 MHz (LSB-D) and 28.001 and 50.001 MHz (USB-D): keyed and heard
                    (tx-band-edges-accepted)
  keys-with-ptt     PTT held (AM carrier): DFN tapped (NR) and held (NB) do their jobs, off the air the moment PTT
                    goes (radio-core-keys-with-ptt)
  armed-ptt-a       on the Sentry's A, armed: KEY and MSG send nothing, PTT sends at the power set and is heard
                    (part of sentry-armed-ptt)
  smeter-am         the X6100's AM carrier: the X6200's S-meter in AM and SAM against its USB reading, rising and
                    falling with the carrier (part of main-screen-x6200-smeter-am-wfm)
  comp              a tone 20 dB loud then soft, USB and USB-D, Comp 1:1 and 4:1: the soft-to-loud range on the
                    listener (settings-audio-compressor, tx-comp-ratio)
  split             split, VFO B 30 kHz up: TCI audio lands on B (part of tx-shift-apps-on-txfreq); the X6200's PTT
                    in split on B in the listen VFO's mode, the VFO back after (part of tx-x6200-ptt-split-txfreq)
  split-cw          the X6200 in split: TCI CW keyed on the listen VFO, decoded there, nothing on B
                    (part of tx-x6200-split-cw-on-listen-vfo)
  cw-alc            CW keyed through TCI: the ALC the base reports while it sets the power (part of alc-cw-grey)
  qsy-out           PTT on 14.320, the Panel's QSY to 14.360 under it: 0 W at once, nothing heard there
                    (tx-stopped-on-qsy-out)
  tune              TUNE at its own power, 0.1 W only (set for the test with the remote's "set tpwr" and put back
                    after; a GUI without it: set at the radio, Settings > TX Cal > EXTERNAL TUNER, else not run):
                    the lease, its lapse, tune 0,
                    LOCK; the state after; each change that stops it; refused on the air; split's carrier on
                    VFO B (ext-tune-panel-lease,
                    ext-tune-carrier-txfreq; part of ext-tune-restores-state, ext-tune-stops-on-change,
                    ext-tune-refused-on-air)
  tune-power        TUNE power (0.1 W) whatever PWR is and PWR changed under it (1 W at most); the time limit
                    (ext-tune-power-setting; part of settings-txcal-external-tuner, ext-tune-time-limit)
  tune-app          TUNE with FT8 open (TX at 1500 Hz) and with SSTV: the tone where the app sends
                    (part of ext-tune-app-button, ext-tune-restores-state)
  tune-key          'TUNE ext. tuner on/off' on FST's long press (set and put back): on, heard, off
                    (part of ext-tune-key-action, radio-core-key-tx-actions)
  text-log          PSK31 text into the listener's PSK app: its line in /mnt/digi_text (the file put back)
                    (part of text-log-file)
  codec-clock       the X6200's tone near the centre and near the edge of the X6100's spectrum: the scale
                    (tx-x6100-gcfix-codec-clock; judged on a gcfix3+ base)
  power-loop        tones.py --bursts both ways: the power loop after each pause (path-tx-power-loop-bursts;
                    tx-x6100-gcfix5-pause-hold on a gcfix4/5 base)
  power-steps       the X6100 in FM at 0.1 and 1 W on 40, 20, 10 and 6 m: 10 dB apart on the X6200
                    (tx-x6100-gcfix5-low-power; judged on a gcfix5 base)
  ft8-tx            FT8 CQs at 0.5 W from a radio with a test call: USB-D on the air and USB after, the ALC, the
                    level found, the X6200's convergence (part of digi-tx-data-mode, alc-digital-reading;
                    digi-tx-level-auto, digi-tx-level-x6200-power)

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import inspect
import itertools
import json
import math
import os
import re
import subprocess
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from suite_more import sync_both, unsync_both  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BATCH = "air_a"
F20 = 14200000                  # the tests' dial (20 m: both radios transmit there)
FIRST = bench.FIRST_PROBE_W     # every over's power but where the feature is a power level
POWER_MAX_W = 1.0               # a power feature's over: never more (the owner, 2026-11-23)
MAX_OVER_S = 14.0               # an over ends itself here, under the watch's 16 s
TUNE_W_OTHER = FIRST            # TUNE's own power for the tune tests that are not about a power level
TUNE_GUARD_CAP_W = 1.3          # a tune's TX meter over this (or over twice its power) ended at once
TUNE_MAX_S = 15.3               # a tune still on after this ended here (its limit is 30 s at most)
GUI_LOG = "/tmp/x6100_log.txt"
# What an over may have apart on purpose, for bench.Watch's pair check (each over begins with the two radios' receiver
# settings alike, the owner, 2026-11-23: "they should have the same settings"): TUNE sends as USB-D whatever the mode
TUNE_IGNORE = ("mode", "flo", "fhi")
ALIGN_KEYS = ("pre", "att", "agc", "rfg", "sql", "nb", "nr", "rit", "zoom")   # receiver keys a sender takes as is
HOLD = itertools.count(int(time.time() * 10) % 1000000000 + 1000)  # each PTT or TUNE press its own, rising number


class NotRun(Exception):
    """A test that cannot run as the radios are set (the owner's hands needed): a look, not a failure"""


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---- small helpers ----

def model(run, n):
    return (run.c["radios"][n].get("model") or n).lower()


def both_ways(run):
    a, b = run.names[0], run.names[1]
    return [(a, b), (b, a)]


def one_way(run, sender_model, listener_model):
    s = next((n for n in run.names if model(run, n) == sender_model), None)
    li = next((n for n in run.names if model(run, n) == listener_model), None)
    return [(s, li)] if s and li and s != li else []


def med(vals):
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    k = len(vals)
    return vals[k // 2] if k % 2 else (vals[k // 2 - 1] + vals[k // 2]) / 2


def fmt(v, nd=1):
    return "-" if v is None else f"{v:.{nd}f}"


def db(a, b):
    """a over b in dB (powers), None when either is missing or not above 0"""
    if a is None or b is None or a <= 0 or b <= 0:
        return None
    return 10 * math.log10(a / b)


def wait_for(cond, secs, step=0.05):
    """The seconds cond took to come true, None when it did not within secs"""
    t0 = time.time()
    while time.time() - t0 < secs:
        if cond():
            return time.time() - t0
        time.sleep(step)
    return time.time() - t0 if cond() else None


def lines_of(r, kind, t0, t1):
    """r's texts of a kind ("t", "m", "state", "tx", "err") that came between t0 and t1: [(when, the rest)]"""
    pre = kind + " "
    return [(at, x[len(pre):]) for at, x in list(r.lines) if t0 <= at <= t1 and x.startswith(pre)]


def tx_frames(r, t0, t1):
    """r's TX meter between t0 and t1: [(when, W, SWR, ALC)]"""
    out = []
    for at, rest in lines_of(r, "t", t0, t1):
        try:
            v = [float(p) for p in rest.split()]
        except ValueError:
            continue
        if len(v) >= 3:
            out.append((at, v[0], v[1], v[2]))
    return out


def meters(r, t0, t1):
    """r's S-meter between t0 and t1: [(when, dBm)]"""
    out = []
    for at, rest in lines_of(r, "m", t0, t1):
        try:
            out.append((at, float(rest)))
        except ValueError:
            pass
    return out


def state_vals(r, key, t0, t1):
    out = []
    for at, rest in lines_of(r, "state", t0, t1):
        try:
            out.append((at, json.loads(rest).get(key)))
        except ValueError:
            pass
    return out


def window(r, secs):
    """r's S-meter over the next secs, its median"""
    t0 = time.time()
    time.sleep(secs)
    return med([v for _, v in meters(r, t0, time.time())])


def med_w(r, t0, t1):
    return med([w for _, w, _, _ in tx_frames(r, t0, t1)])


def level(r, hz, t0, t1, half=1500):
    return bench.spectrum_level(r, hz, t0, t1, half=half)[0]


def rise(q, lv):
    return None if q is None or lv is None else lv - q


def spectrum_peak_hz(r, hz, t0, t1, span=3000):
    """The strongest bin within span of hz in r's spectrum frames t0..t1 (averaged, those of the last frame's centre
    and width), a parabola through its neighbours: (Hz, dB, the bin's width) or (None, None, None)"""
    import numpy as np
    fr = [(cen, wid, v) for t, cen, wid, v in list(r.r.spectra) if t0 <= t <= t1 and v]
    if not fr:
        return None, None, None
    cen, wid, n = fr[-1][0], fr[-1][1], len(fr[-1][2])
    avg = np.mean([np.array(v, float) for c_, w_, v in fr if c_ == cen and w_ == wid and len(v) == n], axis=0)
    per = wid / n
    lo, hi = int((hz - span - (cen - wid / 2)) / per), int((hz + span - (cen - wid / 2)) / per)
    lo, hi = max(lo, 1), min(hi, n - 2)
    if hi - lo < 3:
        return None, None, per
    k = lo + int(np.argmax(avg[lo:hi + 1]))
    a, b, c = avg[k - 1], avg[k], avg[k + 1]
    d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return cen - wid / 2 + (k + 0.5 + d) * per, float(b), per


def last_frame(r):
    fr = list(r.r.spectra)
    return fr[-1] if fr else None


# ---- the radio over SSH (reads; and the few files a test changes, put back) ----

def ssh(host, cmd, timeout=30):
    """bench.ssh over IPv4 (ssh -4: each connection otherwise waited ~5 s on the name's IPv6 lookup, 2026-11-23)"""
    r = subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise bench.Refused(f"{host}: {cmd.split()[0]}: {(r.stderr or r.stdout).strip()[-200:]}")
    return r.stdout


def gui_log_mark(host):
    out = ssh(host, f"wc -l < {GUI_LOG} 2>/dev/null || echo 0").strip().split()
    return int(out[0]) if out and out[0].isdigit() else 0


def gui_log_since(host, mark):
    return ssh(host, f"tail -n +{mark + 1} {GUI_LOG} 2>/dev/null | tail -n 400; true").splitlines()


def base_of(host):
    """The base's version as the GUI read it at its start ("radio: base V1.1.9 ..., line_in"), "" unread"""
    try:
        out = ssh(host, f"grep -m1 'radio: base ' {GUI_LOG} 2>/dev/null; true")
    except bench.Refused:
        return ""
    m = re.search(r"radio: base (.*?), line_in", out)
    return m.group(1).strip() if m else ""


def gcfix_of(base):
    m = re.search(r"gcfix(\d+)", base or "", re.I)
    return int(m.group(1)) if m else 0


def callsign(host):
    return ssh(host, "sqlite3 /mnt/params.db \"select val from params where name='callsign'\"").strip()


def tune_settings(host):
    """TUNE for an external tuner, as the radio keeps it (params: tune_pwr in tenths of a watt, 1.0 W when never set;
    tune_time, 15 s): (W, s). Anything else read: refused, never guessed (a misread power is an over too strong)"""
    out = ssh(host, "sqlite3 -separator '|' /mnt/params.db \"select name,val from params where name in "
                    "('tune_pwr','tune_time')\"")
    d = dict(x.split("|", 1) for x in out.split() if "|" in x)
    pw, tm = d.get("tune_pwr", "10").strip(), d.get("tune_time", "15").strip()
    if not re.fullmatch(r"\d+", pw) or not re.fullmatch(r"\d+", tm):
        raise bench.Refused(f"TUNE's settings not read as numbers (tune_pwr {pw!r}, tune_time {tm!r})")
    return int(pw) / 10.0, max(5, min(30, int(tm)))


CFG_PY = ("import base64,socket,sys\n"
          "s=socket.socket(socket.AF_UNIX);s.settimeout(6);s.connect('/tmp/x6100_cfg.sock')\n"
          "s.sendall(base64.b64decode(sys.argv[1])+b'\\n')\n"
          "b=b''\n"
          "while not b.endswith(b'\\n'):\n"
          " d=s.recv(65536)\n"
          " if not d: break\n"
          " b+=d\n"
          "print(b.decode(errors='replace').strip())\n")


def cfg_sock(host, req):
    """A request to the GUI's configurator socket (src/cfg_socket.cpp), as the web's pages make it: its JSON answer"""
    py = base64.b64encode(CFG_PY.encode()).decode()
    arg = base64.b64encode(json.dumps(req).encode()).decode()
    out = ssh(host, f"python3 -c \"import base64;exec(base64.b64decode('{py}'))\" {arg}", timeout=20)
    lines = out.strip().splitlines()
    if not lines:
        raise bench.Refused(f"{host}: no answer from the configurator socket to {req.get('cmd')}")
    return json.loads(lines[-1])


UDP_OFF = ("if [ -f /mnt/ft8_udp.conf ]; then cp -p /mnt/ft8_udp.conf /tmp/bench_air_ft8_udp.conf && "
           "sed -i 's/^enabled=1/enabled=0/' /mnt/ft8_udp.conf && echo udp=off; else echo udp=none; fi")
UDP_BACK = ("cp -p /tmp/bench_air_ft8_udp.conf /mnt/ft8_udp.conf && cmp /tmp/bench_air_ft8_udp.conf "
            "/mnt/ft8_udp.conf && rm /tmp/bench_air_ft8_udp.conf && echo udp_back")


def udp_off(ctx, r):
    """The FT8 app's WSJT-X UDP (to loggers) off before it opens; /mnt/ft8_udp.conf put back byte for byte after it
    closed (as suite_proto.py's Log test does)"""
    out = ssh(r.host, UDP_OFF)
    if "udp=off" in out:
        def back():
            if "udp_back" not in ssh(r.host, UDP_BACK):
                return f"{r.name}: /mnt/ft8_udp.conf NOT put back (/tmp/bench_air_ft8_udp.conf has it)"
        ctx.undo.append(back)
    elif "udp=none" not in out:
        raise bench.Refused(f"{r.name}: the FT8 app's UDP not switched off ({out.strip()[-80:]})")


def internet_gate(ctx):
    """Nothing to the internet: a radio whose Wavelog live link is on (it posts the frequency and mode the test
    tunes) is not used; switching it off and on would not start its sender again"""
    for r in (ctx.snd, ctx.lis):
        out = ssh(r.host, "grep -c '^live=1' /mnt/field/wavelog.conf 2>/dev/null; true").split()
        if out and out[0] not in ("0", ""):
            raise NotRun(f"{r.name}: Wavelog's live link is on (Operating > Wavelog): the test's frequencies would be "
                         f"posted to it")


# ---- one test, both ways ----

class Ctx:
    """A sender and a listener for one direction of a test: what they are, the rows it says, what to put back"""

    def __init__(self, run, total, snd, lis):
        self.run, self.c, self.total, self.snd, self.lis = run, run.c, total, snd, lis
        self.rep, self.undo, self.rows = {}, [], []
        self.before = {snd.name: [], lis.name: []}

    def row(self, label, ok, says, covers=(), partial=()):
        ok = ok if ok is None else bool(ok)
        self.rows.append((label, ok, says, tuple(covers), tuple(partial)))
        return ok

    def banner(self, text):
        for r in (self.snd, self.lis):
            try:
                r.send(f"banner BENCH {text}")
            except OSError:
                pass

    def run_undo(self):
        """What the test changed beyond finish()'s keys, put back, last first: what did not come back"""
        bad = []
        while self.undo:
            fn = self.undo.pop()
            try:
                why = fn()
                if why:
                    bad.append(str(why))
            except (bench.Refused, OSError, ValueError, subprocess.SubprocessError) as e:
                bad.append(f"{type(e).__name__}: {e}")
        return bad


def part(ctx, label, fn, covers=(), partial=()):
    """One row of a test of several: fn() -> (ok, says); a refusal is its row's, the test goes on"""
    try:
        ok, says = fn()
    except NotRun as e:
        ok, says = None, f"not run: {e}"
    except bench.Refused as e:
        ok, says = False, f"refused: {e}"
    return ctx.row(label, ok, says, covers, partial)


def one_pair(run, s_name, l_name, body, test, covers, partial):
    """body(ctx) with s_name sending and l_name listening: both taken, snapshot, gated, on G; then put back (the
    test's own changes first, then finish: page A). Its rows; a radio or a file not put back fails them"""
    c, radios, snaps, ctx, rows = run.c, [], {}, None, None
    try:
        _, total = bench.chain_gate(c, "radios")
        snd = bench.Radio(s_name, c["radios"][s_name], True)
        radios.append(snd)
        lis = bench.Radio(l_name, c["radios"][l_name], False)
        radios.append(lis)
        for r in radios:
            r.take_control()
            snaps[r.name] = bench.snapshot(r)
        ctx = Ctx(run, total, snd, lis)
        internet_gate(ctx)
        ctx.rep["supply"] = bench.listener_gate(lis)
        bench.open_page_both(radios, "G")       # G tunes each radio to its own channel: set alike again
        out = body(ctx)
        rows = list(out) if isinstance(out, list) else [(test, out[0] if out[0] is None else bool(out[0]), out[1],
                                                        tuple(covers), tuple(partial))]
    except NotRun as e:
        rows = (ctx.rows if ctx else []) + [(test, None, f"not run: {e}", tuple(covers), tuple(partial))]
    except bench.Refused as e:
        errs = ""
        if radios and radios[0].errs:
            errs = f" (the sender said: {radios[0].errs[-3:]})"
        rows = (ctx.rows if ctx else []) + [(test, False, f"refused: {e}{errs}", tuple(covers), tuple(partial))]
    except Exception as e:      # a test's own fault: said as its result, the radios put back all the same
        tb = traceback.format_exc().strip().splitlines()
        rows = (ctx.rows if ctx else []) + [(test, False, f"error: {type(e).__name__}: {e} ({tb[-3:]})",
                                             tuple(covers), tuple(partial))]
    finally:
        with bench.transition():        # the put-back, the test's own undo and finish, one transition
            bad = ctx.run_undo() if ctx else []
            rep = ctx.rep if ctx else {}
            if snaps:
                bench.finish([r for r in radios if r.name in snaps], snaps, report=rep,
                             before=ctx.before if ctx else None)
        for r in radios:
            if r.name not in snaps:
                r.close()
    if bad or rep.get("not_restored"):
        why = "; ".join(bad + ([str(rep["not_restored"])] if rep.get("not_restored") else []))
        rows = [(lb, False if ok else ok, f"{says} (NOT put back: {why})", cv, pt) for lb, ok, says, cv, pt in rows]
    return rows


def pair_test(run, test, body, covers=(), partial=(), pairs=None):
    """body on each (sender, listener) of pairs (both ways by default): both radios in sync first and put back as
    found after (the owner, 2026-11-23); a result per direction and row, and the 1:1 of the two"""
    pairs = both_ways(run) if pairs is None and len(run.names) >= 2 else (pairs or [])
    if not pairs:
        run.result(BATCH, test, None, f"not run: it needs both radios (here: {', '.join(run.names)})",
                   covers=list(covers), partial=list(partial))
        return
    snaps, seen, why = sync_both(run)
    oks = []
    try:
        for s, li in pairs:
            if why:
                rows = [(test, False, f"refused: the radios are not in sync before the test ({why})", tuple(covers),
                         tuple(partial))]
            else:
                rows = one_pair(run, s, li, body, test, covers, partial)
            for label, ok, says, cv, pt in rows:
                run.result(BATCH, f"{label} {s}->{li}", ok, says, covers=list(cv), partial=list(pt), radio=s)
                oks.append(ok)
    finally:
        unsync_both(run, snaps)
    if len(pairs) == 2 and oks:
        pair = False if False in oks else None if None in oks else True
        run.result(BATCH, f"{test} 1:1", pair, f"{len(oks)} results both ways: {oks.count(True)} pass, "
                   f"{oks.count(None)} to look at, {oks.count(False)} fail", covers=list(covers), partial=list(partial))


# ---- each radio set whole, the path proved, the overs ----

def listen(ctx, f, mode, agc=3):
    """The listener on f in mode: PRE and ATT off, RF gain 100, its own receiver on the speaker, read back"""
    return bench.ensure(ctx.lis, *bench.rx_profile(f, mode, pre=0, att=0, rfg=100, agc=agc))


def keep_mode_mem(ctx, r, f, mode):
    """r's own memory of mode (its filter and zoom: each mode's, which finish's snapshot does not hold), kept once in
    ctx.before before a test sets it as the other radio's, so finish puts it back (suite_more.align_over's way). r must
    be in that mode now"""
    mem = ctx.before.setdefault(r.name, [])
    if int(r.state.get("mode", -1)) == mode and not any(x.get("mode") == mode for x in mem):
        mem.append({"freq": f, "mode": mode, "filter": bench._value(r, "filter"), "zoom": bench._value(r, "zoom")})


def alike(ctx, apart=(), what="the sender's TX set-up"):
    """The two radios' receiver settings compared (bench.PAIR_KEYS, as bench.Watch compares them as an over begins);
    anything apart but what the over has apart on purpose (apart: what its watch's pair_ignore names) refused"""
    d = {k: v for k, v in bench.pair_diff(ctx.snd, ctx.lis).items() if k not in apart}
    if d:
        raise bench.Refused(f"after {what}: {ctx.snd.name} and {ctx.lis.name} not set alike: "
                            + ", ".join(f"{k} {x}/{y}" for k, (x, y) in d.items()))


def sender(ctx, f, mode, w, apart=()):
    """The sender set for an over, the two alike (the owner, 2026-11-23: "they should have the same settings. IF they
    don't something is wrong"). The listener is set first (listen()); the sender on f in mode (its own memory of the
    mode kept for finish), then, the listener on that dial and mode, given exactly its set-up (bench.pair_align:
    filter, zoom, PRE, ATT, AGC, RF gain, squelch, NB, NR, RIT, split, VFO), else (an over with the dial or the mode
    apart on purpose) its receiver keys as the listener's (align); then its TX set-up at w W (sending on its dial, ATU
    and TUNE off), read back; the two compared, nothing apart but apart (the over's own pair_ignore)"""
    if w > POWER_MAX_W:
        raise bench.Refused(f"{w} W asked: never over {POWER_MAX_W} W here")
    snd, lis = ctx.snd, ctx.lis
    bench.ensure(snd, {"split": 0, "vfo": 0, "freq": f, "mode": mode})
    keep_mode_mem(ctx, snd, f, mode)
    if (int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))) == (f, mode):
        bench.pair_align(snd, lis)
    else:
        align(ctx)
    got = bench.ensure(snd, *bench.tx_profile(f, mode, w))
    alike(ctx, apart)
    return got


def align(ctx):
    """The sender's receiver set as the listener's, read back: PRE, ATT, AGC, RF gain, squelch, NB, NR, RIT, zoom, and
    the filter when both are in one mode (the owner, 2026-11-23: "they should have the same settings"; bench.Watch
    compares them as each over begins). What a test sets apart on purpose its over names (ignore)"""
    snd, lis = ctx.snd, ctx.lis
    want = {k: int(lis.state[k]) for k in ALIGN_KEYS if lis.state.get(k) is not None}
    if int(snd.state.get("mode", -1)) == int(lis.state.get("mode", -2)):
        want["filter"] = (int(lis.state.get("flo", 0)), int(lis.state.get("fhi", 0)))
    return bench.ensure(snd, want)


def mirror(ctx):
    """The listener's receiver set as the sender's, read back: for an app open on the sender, which holds its own mode,
    filter and AGC there (the listener on the same dial before)"""
    snd, lis = ctx.snd, ctx.lis
    m = int(snd.state.get("mode", -1))
    bench.ensure(lis, {"mode": m})
    keep_mode_mem(ctx, lis, int(lis.state.get("freq", 0)), m)     # its own filter and zoom of the mode, for finish
    want = {k: int(snd.state[k]) for k in ALIGN_KEYS + ("mode",) if snd.state.get(k) is not None}
    want["filter"] = (int(snd.state.get("flo", 0)), int(snd.state.get("fhi", 0)))
    got = bench.ensure(lis, want)
    alike(ctx, (), "the listener set as the app")
    return got


def prove(ctx, f, mode=2, agc=3):
    """The listener set on f, then the path proof (the sender's 0.1 W tone 20 dB over the listener's quiet)"""
    listen(ctx, f, mode, agc)
    return bench.path_proof(ctx.c, ctx.total, ctx.snd, ctx.lis, f, ctx.rep)


def arm_if_sentry(r, on, mode=None):
    """The Sentry's A/B on A or G (its lock); with an app open the Sentry is not there to lock anything (its A/B would
    be the main screen's VFO key): there only the radio's own hold is checked"""
    if r.state.get("pg") in ("A", "G"):
        bench.arm(r, on, mode=mode)
    elif on and r.state.get("rxo") == 1:
        raise bench.Refused(f"{r.name}: held receive only (rxo 1) with {r.state.get('app') or r.state.get('win')} open")
    elif on and mode is not None and int(r.state.get("mode", -1)) != mode:
        bench.ensure(r, {"mode": mode})


def arm_for_over(r, mode):
    """Armed for an over, the dial where it was set: on the Sentry's A its routine may tune the dial till the arming
    holds it (dial_held); a dial that moved is set back and the over refused if it will not, never sent elsewhere"""
    f0 = int(r.state.get("freq", 0))
    arm_if_sentry(r, True, mode=mode)
    if int(r.state.get("freq", 0)) != f0:
        bench.ensure(r, {"freq": f0})
    if int(r.state.get("freq", 0)) != f0:
        raise bench.Refused(f"{r.name}: the dial moved to {r.state.get('freq')} as it was armed ({f0} set)")


def make_watch(ctx, w, ignore=(), label=None):
    """bench.Watch over both radios for an over at w W; its pair check (a bench.py from 2026-11-23 on) told what the
    over has apart on purpose (ignore), and its screenshots labelled"""
    kw, params = {}, inspect.signature(bench.Watch.__init__).parameters
    if "pair_ignore" in params:
        kw["pair_ignore"] = tuple(ignore)
    if "label" in params and label:
        kw["label"] = f"air_a {label}"
    return bench.Watch([ctx.snd, ctx.lis], ctx.c, expect_dbm=10 * math.log10(w * 1000) - ctx.total, listener=ctx.lis,
                       w=w, **kw)


def off_air(r, secs=5.0):
    try:
        r.wait("off the air", lambda: not r.tx, secs)
        return True
    except bench.Refused:
        return False


def lock_again(r):
    """The sender locked again after an over: control back first (a TCI client had it while connected)"""
    if not r.ctl_mine:
        try:
            r.take_control()
        except bench.Refused:
            pass
    arm_if_sentry(r, False)


def near_w(w, T):
    """A TX meter reading of T W: within 3 dB; at 0.15 W and less, 0.25 W at most (the meter reads whole tenths cut
    off: a true 0.09 W reads 0.0, so a low power's reading says only that it is not more)"""
    if w is None:
        return False
    if T <= 0.15:
        return w <= 0.25
    d = db(w, T)
    return d is not None and abs(d) <= 3.0


def heard_ok(o):
    return o.get("deaf") is None


def ptt_over(ctx, mode, secs, during=None, w=FIRST, what="PTT", ignore=()):
    """The sender armed and keyed with the Panel's PTT (a lease renewed each half second) in mode at w, watched;
    during(o) runs while it is on the air (o["release"]() ends the over early), else secs on the air. The PTT
    released, the sender off the air and locked again whatever happens. o: on, released, off, peak_w, deaf"""
    snd, lis = ctx.snd, ctx.lis
    bench.radio_tx_gate(snd, ctx.c, w)
    lis.wait("the listener out of TX", lambda: not lis.tx, 5)
    watch = make_watch(ctx, w, ignore, what)
    stop = threading.Event()
    hold = next(HOLD)
    o = {"on": None, "released": None, "off": None, "watch": watch}

    def release():
        stop.set()
        try:
            snd.send("key PTT 0")
        except OSError:
            pass
        if o["released"] is None:
            o["released"] = time.time()
    o["release"] = release

    def renew():
        t0 = time.time()
        while not stop.is_set() and not watch.why:
            if time.time() - t0 > min(secs + 2.0, MAX_OVER_S):
                release()               # its own end, under the watch's
                break
            try:
                snd.send(f"key PTT 1 {hold}")
            except OSError:
                break
            stop.wait(0.5)
    th = None
    try:
        arm_for_over(snd, mode)
        bench.tx_log({"who": snd.name, "f": int(snd.state.get("freq", 0)), "w": snd.pwr(), "what": f"air_a {what}"})
        th = threading.Thread(target=renew, daemon=True)
        th.start()
        snd.wait("on the air with PTT", lambda: snd.tx or bool(watch.why), 6)
        if watch.why:
            raise bench.Refused(f"stopped: {watch.why}")
        o["on"] = snd.tx_since or time.time()
        if during:
            during(o)
        else:
            while time.time() - o["on"] < secs and not watch.why and not stop.is_set():
                time.sleep(0.05)
    finally:
        release()
        if th:
            th.join(2.0)
        off_air(snd)
        o["off"] = time.time()
        watch.close()
        lock_again(snd)
    if watch.why:
        raise bench.Refused(f"stopped: {watch.why}")
    o["deaf"] = watch.unheard(snd)
    o["peak_w"] = watch.tx_peak.get(snd.name)
    o["over_w"] = watch.over_power.get(snd.name)
    return o


def tci_over(ctx, plan=None, file=None, mode=3, w=FIRST, max_s=None, during=None, amp=0.3, fade=0.05, what="TCI",
             ignore=()):
    """_tci_over, once more after a link's stall stopped it (bench.once_more_after_stall)"""
    return bench.once_more_after_stall([ctx.snd, ctx.lis], lambda: _tci_over(ctx, plan, file, mode, w, max_s, during,
                                                                             amp, fade, what, ignore), what)


def _tci_over(ctx, plan=None, file=None, mode=3, w=FIRST, max_s=None, during=None, amp=0.3, fade=0.05, what="TCI",
              ignore=()):
    """The sender armed and fed through its own TCI (tci_tone_tx.py on it: plan [[hz, s], ...], or file, 16-bit 24 kHz
    audio already on the radio) in mode at w, watched; during(o) while it sends. The sender stopped, off the air,
    control back and locked again whatever happens. o: on, sent (its begin and end here), off, peak_w, deaf"""
    snd, lis = ctx.snd, ctx.lis
    if max_s is None:
        max_s = (sum(s for _, s in plan) if plan else 10.0) + 1.5
    max_s = min(max_s, MAX_OVER_S)
    bench.radio_tx_gate(snd, ctx.c, w)
    lis.wait("the listener out of TX", lambda: not lis.tx, 5)
    watch = make_watch(ctx, w, ignore, what)
    o = {"on": None, "off": None, "sent": None, "watch": watch}
    th = None

    def go():
        try:
            lines, t0, t1 = bench.tci_send(snd, plan, amp=amp, max_s=max_s, fade=fade, watch=watch, file=file)
            o["lines"], o["sent"] = lines, (t0, t1)
        except (OSError, subprocess.SubprocessError) as e:
            o["error"] = str(e)
    try:
        arm_for_over(snd, mode)
        bench.tx_log({"who": snd.name, "f": int(snd.state.get("freq", 0)), "w": snd.pwr(), "what": f"air_a {what}"})
        th = threading.Thread(target=go, daemon=True)
        th.start()
        try:
            snd.wait("on the air with TCI", lambda: snd.tx or bool(watch.why) or not th.is_alive(), 8)
        except bench.Refused:
            pass
        if not snd.tx:
            raise bench.Refused(f"{snd.name} did not go on the air with TCI audio"
                                + (f": {watch.why}" if watch.why else f" ({o.get('lines', [])[-2:]})"))
        o["on"] = snd.tx_since or time.time()
        if during:
            during(o)
        th.join(max_s + 30)
    finally:
        if th is not None and th.is_alive():
            bench.stop_bench_programs(snd)
            th.join(10)
        off_air(snd)
        o["off"] = time.time()
        watch.close()
        lock_again(snd)
    if watch.why:
        raise bench.Refused(f"stopped: {watch.why}")
    o["deaf"] = watch.unheard(snd)
    o["peak_w"] = watch.tx_peak.get(snd.name)
    o["over_w"] = watch.over_power.get(snd.name)
    if o["sent"] is None:
        o["sent"] = (o["on"], o["off"])
    return o


def tci_cw_over(ctx, text, max_s=12.0, ignore=()):
    """text keyed by the sender's own keyer through its TCI (tci_cw_tx.py on it), in CW at 0.1 W, watched; the
    keyer's program stopped, the sender off the air and locked again whatever happens"""
    snd, lis = ctx.snd, ctx.lis
    bench.radio_tx_gate(snd, ctx.c, FIRST)
    lis.wait("the listener out of TX", lambda: not lis.tx, 5)
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_cw_tx.py"),
                    f"root@{snd.host}:/tmp/tci_cw_tx.py"], check=True, timeout=30)
    watch = make_watch(ctx, FIRST, ignore, "CW")
    o = {"on": None, "off": None, "t0": None, "watch": watch}
    proc = None
    try:
        arm_for_over(snd, 4)
        bench.tx_log({"who": snd.name, "f": int(snd.state.get("freq", 0)), "w": snd.pwr(), "what": "air_a CW"})
        o["t0"] = time.time()
        proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{snd.host}",
                                 f"python3 /tmp/tci_cw_tx.py '{text}' {max_s:.0f}"], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        while proc.poll() is None and not watch.why and time.time() - o["t0"] < max_s + 15:
            if snd.tx and o["on"] is None:
                o["on"] = snd.tx_since or time.time()
            time.sleep(0.05)
    finally:
        if proc is not None and proc.poll() is None:
            bench.stop_bench_programs(snd)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        off_air(snd, 8)
        o["off"] = time.time()
        watch.close()
        lock_again(snd)
    if watch.why:
        raise bench.Refused(f"stopped: {watch.why}")
    o["out"] = (proc.stdout.read() if proc and proc.stdout else "").strip().splitlines()[-2:]
    o["peak_w"] = watch.tx_peak.get(snd.name)
    return o


def tune_over(ctx, T, secs=3.0, during=None, renew=True, start=None, ignore=TUNE_IGNORE):
    """TUNE for an external tuner at its own power T: started (the Panel's "tune 1 HOLD", or start()), renewed each
    second while renew (o["renew"][0] False stops the renewals), watched, guarded (its meter over twice T or over
    TUNE_GUARD_CAP_W, or on past TUNE_MAX_S: "tune 0" at once); during(o) while it tunes, else secs. "tune 0", the
    sender off the air and locked again whatever happens. o: on, off, peak_w, deaf, over (the guard's reading)"""
    snd = ctx.snd
    if T > POWER_MAX_W:
        raise bench.Refused(f"TUNE power {T} W: never over {POWER_MAX_W} W here")
    bench.radio_tx_gate(snd, ctx.c, snd.pwr())         # its supply, ATU off, the dial on a band, PWR in the limit
    ctx.lis.wait("the listener out of TX", lambda: not ctx.lis.tx, 5)
    lm = int(ctx.lis.state.get("mode", 2))
    decl = bench.apart({"mode": [(3, lm)], "filter": bench.STEPPED},
                       f"TUNE: the sender as USB-D (its filter) for the carrier, the listener in mode {lm}")
    decl.__enter__()
    try:
        return _tune_over(ctx, T, secs, during, renew, start, ignore)
    finally:
        decl.__exit__(None, None, None)


def _tune_over(ctx, T, secs, during, renew, start, ignore):
    snd = ctx.snd
    watch = make_watch(ctx, T, ignore, "TUNE")
    halt = threading.Event()
    o = {"on": None, "off": None, "hold": next(HOLD), "renew": [renew], "over": None, "watch": watch,
         "last_renew": None, "errs0": len(snd.errs)}
    guard_w = min(max(2 * T, T + 0.3), TUNE_GUARD_CAP_W)

    def lease():
        last = 0.0
        while not halt.is_set() and not watch.why:
            now = time.time()
            if o["renew"][0] and now - last >= 1.0:
                try:
                    snd.send(f"tune 1 {o['hold']}")
                except OSError:
                    break
                last = o["last_renew"] = now
            t = tx_frames(snd, now - 0.4, now)
            since = snd.tx_since
            if snd.tx and since and now - since > 0.6 and t and min(x[1] for x in t) > guard_w:
                o["over"] = max(x[1] for x in t)
                snd.send("tune 0")
                say(f"{snd.name}: TUNE read {o['over']:.2f} W for {T} W: stopped")
                halt.set()
            if snd.tx and since and now - since > TUNE_MAX_S:
                snd.send("tune 0")
                o["over_s"] = now - since
                halt.set()
            halt.wait(0.1)
    th = None
    try:
        arm_for_over(snd, int(snd.state.get("mode", 2)))
        bench.tx_log({"who": snd.name, "f": int(snd.state.get("txf") or snd.state.get("freq") or 0), "w": T,
                      "what": "air_a TUNE"})
        if start:
            start()
        else:
            snd.send(f"tune 1 {o['hold']}")
            o["last_renew"] = time.time()
        t_ask = time.time()
        wait_for(lambda: snd.state.get("tune") == 1 or bool(watch.why) or len(snd.errs) > o["errs0"], 4.0)
        if snd.state.get("tune") != 1:
            raise bench.Refused(f"TUNE did not start (the radio said: {snd.errs[o['errs0']:][-2:] or 'nothing'})")
        o["on"] = time.time()
        o["asked_s"] = o["on"] - t_ask
        th = threading.Thread(target=lease, daemon=True)
        th.start()
        if during:
            during(o)
        else:
            while time.time() - o["on"] < secs and not watch.why and not halt.is_set():
                time.sleep(0.05)
    finally:
        halt.set()
        if th:
            th.join(2.0)
        try:
            snd.send("tune 0")
        except OSError:
            pass
        wait_for(lambda: snd.state.get("tune") == 0 and not snd.tx, 5.0)
        o["off"] = time.time()
        watch.close()
        lock_again(snd)
    if watch.why:
        raise bench.Refused(f"stopped: {watch.why}")
    if o["over"] is not None:
        raise bench.Refused(f"TUNE read {o['over']:.2f} W for a TUNE power of {T} W: stopped at once")
    o["deaf"] = watch.unheard(snd)
    o["peak_w"] = watch.tx_peak.get(snd.name)
    return o


def tune_gate(ctx, max_w):
    """TUNE's own power and limit for the test: the power set to max_w with the remote's "set tpwr" (the owner,
    2026-11-23: "Yes, set it"; the snapshot keeps the power as found and finish() puts it back, its settings row too)
    and read back from the radio's state. A GUI without the setter: both as its settings database keeps them, a power
    over max_w not run (the GUI is never restarted for a changed params.db)"""
    snd = ctx.snd
    if "tpwr" in snd.state:
        want = round(max_w * 10)
        if int(snd.state["tpwr"]) != want:
            snd.send(f"set tpwr {want}")
            snd.wait(f"TUNE power {max_w} W", lambda: int(snd.state.get("tpwr", -1)) == want, bench.SET_WAIT_S)
        T, L = int(snd.state["tpwr"]) / 10.0, int(snd.state.get("tune_s", 15))
        say(f"{snd.name}: TUNE power {T} W (set for the test, put back after), its limit {L} s")
    else:
        T, L = tune_settings(snd.host)
    if T > max_w + 1e-6:
        raise NotRun(f"{ctx.snd.name}'s TUNE power reads {T} W; these tune tests run at {max_w} W at most: set it at "
                     f"the radio (Settings > TX Cal > EXTERNAL TUNER, TUNE power {max_w} W)")
    if T < 0.05:
        raise NotRun(f"{ctx.snd.name}'s TUNE power reads {T} W: nothing to measure")
    return T, L


def vfo_b(ctx, f_b, mode_b):
    """The sender's VFO B on f_b in mode_b, VFO A's dial kept; VFO B as it was put back at the test's end (finish()
    keeps no VFO B). Leaves VFO A in use, split off"""
    snd = ctx.snd
    bench.ensure(snd, {"split": 0, "vfo": 0})
    bench.ensure(snd, {"vfo": 1})
    was = (int(snd.state.get("freq", 0)), int(snd.state.get("mode", -1)))

    def back():
        bench.ensure(snd, {"split": 0})
        bench.ensure(snd, {"vfo": 1})
        try:
            if was[0] and (int(snd.state.get("freq", 0)), int(snd.state.get("mode", -1))) != was:
                bench.ensure(snd, dict({"freq": was[0]}, **({"mode": was[1]} if was[1] >= 0 else {})))
            got = (int(snd.state.get("freq", 0)), int(snd.state.get("mode", -1)))
        finally:
            bench.ensure(snd, {"vfo": 0})
        if was[0] and got != was:
            return f"{snd.name}'s VFO B reads {got}, was {was}"
    ctx.undo.append(back)
    bench.ensure(snd, {"freq": f_b, "mode": mode_b})
    bench.ensure(snd, {"vfo": 0})
    return was


def split_on(ctx, f_b):
    snd = ctx.snd
    bench.ensure(snd, {"split": 1})
    snd.wait(f"the TX frequency on VFO B ({f_b} Hz)", lambda: int(snd.state.get("txf", 0)) == f_b, 4)


# ---- the tests ----

# The X6200's Measure takes a signal of S9 - 3 (-76 dBm) or more: the X6100's 0.1 W read -82 dBm there over the bench's
# 80 dB (2026-11-24), 1 W -72; the X6100's takes S5, its 0.1 W from the X6200 -79
SMC_W_X6200 = 1.0
SMC_S = 12.0                    # Measure's four steps, 3 s each (settings_window.cpp: SMC_SETTLE + SMC_SAMPLE)


def smc_put(r, fig):
    """The three S-meter figures (PRE, ATT, RF gain) set on r ("smc put PRE,ATT,RFG"), read back: what is not as asked
    said, None when they are"""
    t0 = time.time()
    r.send("smc put " + ",".join(str(int(v)) for v in fig))
    time.sleep(0.8)
    err = [x for at, x in list(r.lines) if at >= t0 and x.startswith("err smc")]
    now = smc_ask(r)["fig"]
    if err or now != tuple(fig):
        return f"S-meter figures {now}, {tuple(fig)} asked" + (f" ({err[-1]})" if err else "")
    return None


def smc_ask(r, timeout=4.0):
    """TX Cal > S-METER's Measure as the remote reads it ("smc?"): running, (PRE, ATT, RF gain), its last note"""
    t0 = time.time()
    r.send("smc?")
    while time.time() - t0 < timeout:
        for at, t in reversed(list(r.lines)):
            if at >= t0 and t.startswith("smc "):
                p = t.split(" ", 5)
                return {"run": p[1] == "1", "fig": (int(p[2]), int(p[3]), int(p[4])),
                        "note": json.loads(p[5]) if len(p) > 5 else ""}
        time.sleep(0.1)
    raise bench.Refused(f"{r.name}: no answer to 'smc?' (a GUI before the remote Measure, v0.3.12.49)")


def body_smeter_front_end(ctx):
    """settings-txcal-smeter-front-end: the listener's own Measure (TX Cal > S-METER) on a steady tone through the
    pads, then its S-meter stays put (within 3 dB) when PRE, ATT or RF gain change. Measure first: the figures are each
    radio's own (the defaults were one radio's; neither bench radio was ever measured, and PRE read 3 to 4 dB low, RF
    gain 40 4 to 6 dB high on both, 11-23 and 11-24). The figures as found put back after"""
    snd, lis = ctx.snd, ctx.lis
    w = SMC_W_X6200 if str(lis.state.get("fp", "")).startswith("x6200") else FIRST
    prove(ctx, F20)
    listen(ctx, F20, 3)
    time.sleep(1.5)
    quiet = med([v for _, v in meters(lis, time.time() - 1.2, time.time())])
    found = smc_ask(lis)
    if found["run"]:
        raise bench.Refused(f"{lis.name}: a Measure already running ({found['note']!r})")

    ctx.undo.append(lambda: smc_put(lis, found["fig"]))
    sender(ctx, F20, 3, w)
    meas = {}

    def measure(o):                 # Measure from the tone's start: its first 1.5 s are its own settling
        lis.send("smc start")
        t0 = time.time()
        while time.time() - t0 < SMC_S + 1.2:
            time.sleep(0.5)
            a = smc_ask(lis, 2.0)
            if not a["run"] and time.time() - t0 > 1.0:
                meas.update(a, secs=time.time() - t0)
                return
        lis.send("smc stop")
        meas.update(smc_ask(lis), secs=None)
    steps = [("as set", {}), ("PRE on", {"pre": 1}), ("ATT on", {"pre": 0, "att": 1}), ("RF gain 40", {"att": 0,
                                                                                                       "rfg": 40}),
             ("as set again", {"rfg": 100})]
    got = []

    def during(o):
        time.sleep(1.0)
        for name, want in steps:
            if want:
                bench.ensure(lis, want)
            time.sleep(0.3)
            got.append((name, window(lis, 1.2)))
    # the listener's PRE, ATT and RF gain changed under the overs on purpose (Measure's steps, then what is measured)
    with bench.apart({"pre": bench.STEPPED, "att": bench.STEPPED, "rfg": bench.STEPPED},
                     "smeter-front-end: the listener's PRE, ATT and RF gain stepped under the overs, by its own "
                     "Measure, then by the test"):
        ctx.banner(f"S-METER MEASURE: {snd.name.upper()} SENDS A STEADY TONE {w} W, {lis.name.upper()} MEASURES ITS "
                   f"PRE, ATT, RF GAIN (TX CAL)")
        o1 = tci_over(ctx, plan=[[0, 0.3], [1000, SMC_S + 1.4]], mode=3, w=w, max_s=SMC_S + 1.7, during=measure,
                      what="S-meter Measure", ignore=("pre", "att", "rfg"))
        done = str(meas.get("note", "")).startswith("Done")
        if not done:
            return False, (f"{lis.name}'s Measure did not finish ({meas.get('note')!r}; the figures "
                           f"{found['fig']} before, {meas.get('fig')} after); {snd.name}'s meter "
                           f"{fmt(o1['peak_w'], 2)} W")
        listen(ctx, F20, 3)         # PRE and ATT off, RF gain 100 again (Measure puts them back itself)
        ctx.banner(f"S-METER FRONT END: {snd.name.upper()} SENDS A STEADY TONE {w} W, {lis.name.upper()}'S S-METER "
                   f"WITH PRE, ATT, RF GAIN")
        o = tci_over(ctx, plan=[[0, 0.3], [1000, 12.6]], mode=3, w=w, max_s=13.6, during=during,
                     what="S-meter front end", ignore=("pre", "att", "rfg"))
    base = got[0][1] if got else None
    off = [(n, None if v is None or base is None else v - base) for n, v in got]
    ok = (len(got) == len(steps) and base is not None and quiet is not None and base - quiet >= 15
          and all(d is not None and abs(d) <= 3.0 for _, d in off) and heard_ok(o))
    return ok, (f"{lis.name}'s Measure ({fmt(meas.get('secs'), 1)} s): PRE, ATT, RF gain {found['fig']} -> "
                f"{meas.get('fig')} ({meas.get('note')!r}); its S-meter: quiet {fmt(quiet)} dBm, the tone "
                f"{fmt(base)} dBm as set; " + ", ".join(f"{n} {fmt(d, 1)} dB" for n, d in off[1:])
                + f" from it (3 dB at most); {snd.name}'s meter {fmt(o['peak_w'], 2)} W"
                + ("" if heard_ok(o) else "; the over NOT heard") + "; the figures put back after")


def t_smeter_front_end(run):
    pair_test(run, "smeter-front-end", body_smeter_front_end, covers=["settings-txcal-smeter-front-end"])


class TciText:
    """A TCI client from this computer that only listens (it never keys: nobody is asked about it): every text with
    when it came, each command of a frame on its own"""

    def __init__(self, host):
        from remote_ws import Remote
        self.c = Remote(host, 50001, timeout=5)
        self.texts, self.on = [], True
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while self.on:
            m = self.c.recv(timeout=0.5)
            if m is None:
                if not self.c.alive:
                    break
                continue
            now = time.time()
            for part_ in m.split(";"):
                if part_.strip():
                    self.texts.append((now, part_.strip()))

    def send(self, t):
        self.c.send(t)

    def after(self, t0, prefix):
        return [(at, m) for at, m in list(self.texts) if at >= t0 and m.lower().startswith(prefix)]

    def close(self):
        self.on = False
        self.c.close()


def body_readings_zero(ctx):
    """tx-readings-start-zero: an over at 0.5 W (a power feature: 1 W at most), then a near-silent one at a 0.1 W
    setting: the second's first TX meter frame and TCI's first tx_sensors after it keyed are 0 or a reading of its own,
    never the first over's last watts (its real start can be 0 W or a Xiegu base's surge to about 1 W: neither is
    0.5); TCI's SWR 1.0 or more"""
    snd, lis = ctx.snd, ctx.lis
    w_first = 0.5
    prove(ctx, F20)
    listen(ctx, F20, 3)
    tci = TciText(snd.host)
    ctx.undo.append(tci.close)
    tci.send("tx_sensors_enable:true;")
    time.sleep(0.5)
    ctx.banner(f"READINGS FROM ZERO: {snd.name.upper()} SENDS {w_first} W, THEN NEXT TO NOTHING, {lis.name.upper()} "
               f"LISTENS")
    sender(ctx, F20, 3, w_first)
    o1 = tci_over(ctx, plan=[[0, 0.3], [1000, 3.0]], mode=3, w=w_first, what=f"readings {w_first} W")
    f1 = tx_frames(snd, o1["on"], o1["off"])
    w1 = med([w for at, w, _, _ in f1 if at >= o1["on"] + 1.0])
    last1 = f1[-1][1] if f1 else None
    if last1 is None or not 0.3 <= last1 <= 0.8:
        raise NotRun(f"the {w_first} W over's last reading was {fmt(last1, 2)} W: too far from it to tell a reading "
                     f"left over from a new one")
    time.sleep(1.5)
    sender(ctx, F20, 3, FIRST)
    t_mid = time.time()
    o2 = tci_over(ctx, plan=[[0, 0.2], [1000, 1.2]], amp=0.01, mode=3, w=FIRST, max_s=3.0, what="readings near 0 W")
    f2 = tx_frames(snd, t_mid, o2["off"])
    sens = tci.after(t_mid, "tx_sensors:")
    first_s = None
    if sens:
        p = sens[0][1].split(":", 1)[1].split(",")
        try:
            first_s = (float(p[2]), float(p[4]))
        except (IndexError, ValueError):
            first_s = None
    first_t = f2[0][1] if f2 else None

    def left_over(w):
        return w is not None and w > 0.05 and abs(w - last1) < 0.05
    ok = (first_t is not None and not left_over(first_t) and first_s is not None and not left_over(first_s[0])
          and first_s[1] >= 1.0)
    return ok, (f"over 1 at {w_first} W read {fmt(w1, 2)} W, its last frame {fmt(last1, 2)} W; over 2 (next to "
                f"nothing): {len(f2)} TX meter frames, the first {fmt(first_t, 2)} W"
                + (" (THE LAST OVER'S)" if left_over(first_t) else "") + "; TCI's first tx_sensors after it keyed: "
                + (f"{first_s[0]:.1f} W, SWR {first_s[1]:.1f}" + (" (THE LAST OVER'S)" if left_over(first_s[0]) else "")
                   if first_s else f"none ({len(tci.texts)} texts from TCI)"))


def t_readings_zero(run):
    pair_test(run, "readings-zero", body_readings_zero, covers=["tx-readings-start-zero"])


EDGES = (("40 m top", 7299000, 1), ("20 m top", 14349000, 1), ("10 m bottom", 28001000, 3),
         ("6 m bottom", 50001000, 3))


def body_band_edges(ctx):
    """tx-band-edges-accepted: a 1 kHz TCI tone at 0.1 W just inside four table edges (upper ones in LSB-D, the tone
    below the dial; lower ones in USB-D, above it): each keyed, the sender's meter up, the listener's spectrum 15 dB
    up at the tone"""
    snd, lis = ctx.snd, ctx.lis
    prove(ctx, F20)
    said, good = [], True
    for name, f, m in EDGES:
        rf = f + (1000 if m in (2, 3) else -1000)
        listen(ctx, f, m)                   # in the sender's own mode: the two alike
        sender(ctx, f, m, FIRST)
        ctx.banner(f"BAND EDGE {name.upper()}: {snd.name.upper()} SENDS 0.1 W ON {f / 1e6:.3f} MHZ, "
                   f"{lis.name.upper()} LISTENS")
        time.sleep(1.3)
        q = level(lis, rf, time.time() - 1.0, time.time(), half=150)
        try:
            o = tci_over(ctx, plan=[[0, 0.3], [1000, 2.5]], mode=m, what=f"band edge {f}")
        except bench.Refused as e:
            if str(e).startswith("stopped"):
                raise                       # the watch stopped everything: no more overs
            good = False
            said.append(f"{name} {f / 1e6:.3f}: REFUSED ({e})")
            continue
        lv = level(lis, rf, o["on"] + 0.8, min(o["sent"][1], o["off"]) - 0.2, half=150)
        rs = rise(q, lv)                    # keyed (tci_over refuses one that was not) and heard: the listener's own
        this = rs is not None and rs >= 15  # (the TX meter reads whole tenths cut off: 0.09 W reads 0.0)
        good &= this
        said.append(f"{name} {f / 1e6:.3f} {'LSB-D' if m == 1 else 'USB-D'}: {fmt(o['peak_w'], 2)} W, heard "
                    f"{fmt(rs)} dB up" + ("" if this else " (NOT)"))
    return good, "; ".join(said)


def t_band_edges(run):
    pair_test(run, "band-edges", body_band_edges, covers=["tx-band-edges-accepted"])


def body_keys_with_ptt(ctx):
    """radio-core-keys-with-ptt: PTT held (AM, a carrier at 0.1 W, heard): DFN tapped turns NR over, DFN held turns
    NB over, both while on the air; PTT let go: off the air within a second and staying off"""
    snd, lis = ctx.snd, ctx.lis
    prove(ctx, F20)
    listen(ctx, F20, 6)
    sender(ctx, F20, 6, FIRST)
    nr0, nb0 = int(snd.state.get("nr", 0)), int(snd.state.get("nb", 0))
    res = {}
    time.sleep(1.3)
    q = level(lis, F20, time.time() - 1.0, time.time(), half=500)

    def during(o):
        time.sleep(1.5)
        res["heard"] = rise(q, level(lis, F20, o["on"] + 0.6, time.time(), half=500))
        bench.tap(snd, "DFN")
        res["nr_s"] = wait_for(lambda: int(snd.state.get("nr", nr0)) != nr0, 2.5)
        res["tx_tap"] = snd.tx
        snd.send("key DFN 1")
        time.sleep(1.6)
        snd.send("key DFN 0")
        res["nb_s"] = wait_for(lambda: int(snd.state.get("nb", nb0)) != nb0, 2.5)
        res["tx_hold"] = snd.tx
        res["nr_after_hold"] = int(snd.state.get("nr", -1))
        time.sleep(0.5)
        o["release"]()
        res["off_s"] = wait_for(lambda: not snd.tx, 3.0)
        t1 = time.time()
        time.sleep(2.0)
        res["came_back"] = any(v for _, v in state_vals(snd, "tx", t1, time.time())) or snd.tx
    ctx.banner(f"KEYS WITH PTT: {snd.name.upper()} SENDS AM 0.1 W, DFN TAPPED AND HELD UNDER PTT, {lis.name.upper()} "
               f"LISTENS")
    with bench.apart({"nr": bench.STEPPED, "nb": bench.STEPPED},
                     "keys-with-ptt: DFN tapped and held under PTT turns the sender's NR and NB, what it tests"):
        o = ptt_over(ctx, 6, 8.0, during=during, what="keys with PTT", ignore=("nr", "nb"))
    nr1, nb1 = int(snd.state.get("nr", -1)), int(snd.state.get("nb", -1))
    ok = (res.get("nr_s") is not None and res.get("nb_s") is not None and res.get("tx_tap") and res.get("tx_hold")
          and res.get("nr_after_hold") == 1 - nr0 and nr1 == 1 - nr0 and nb1 == 1 - nb0
          and res.get("off_s") is not None and res["off_s"] <= 1.0 and not res.get("came_back")
          and (res.get("heard") or 0) >= 15)
    return ok, (f"under PTT ({fmt(o['peak_w'], 2)} W, heard {fmt(res.get('heard'))} dB up): DFN tapped: NR {nr0} -> "
                f"{res.get('nr_after_hold')} in {fmt(res.get('nr_s'), 2)} s, on the air {res.get('tx_tap')}; DFN held: "
                f"NB {nb0} -> {nb1} in {fmt(res.get('nb_s'), 2)} s, on the air {res.get('tx_hold')}; PTT let go: off "
                f"the air in {fmt(res.get('off_s'), 2)} s, on again after {bool(res.get('came_back'))}")


def t_keys_with_ptt(run):
    pair_test(run, "keys-with-ptt", body_keys_with_ptt, covers=["radio-core-keys-with-ptt"])


def body_armed_ptt_a(ctx):
    """sentry-armed-ptt (part: a carrier, not a voice; no SDR on B): on the Sentry's A, armed: KEY and MSG send
    nothing; PTT (AM) sends at the power set and the listener hears it"""
    snd, lis = ctx.snd, ctx.lis
    prove(ctx, F20)
    listen(ctx, F20, 6)
    bench.open_page(snd, "A")
    sender(ctx, F20, 6, FIRST)
    bench.radio_tx_gate(snd, ctx.c, FIRST)
    ctx.banner(f"ARMED PTT ON A: {snd.name.upper()} ARMED, KEY AND MSG MUST SEND NOTHING, THEN PTT AM 0.1 W")
    watch = make_watch(ctx, FIRST, label="armed on A, KEY and MSG")
    went = []
    try:
        bench.arm(snd, True, mode=6)
        if int(snd.state.get("freq", 0)) != F20:
            bench.ensure(snd, {"freq": F20, "mode": 6})
        for k in ("KEY", "MSG"):
            bench.tap(snd, k)
            t0 = time.time()
            while time.time() - t0 < 2.5 and not watch.why:
                if snd.tx:
                    went.append(k)
                    snd.send("key PTT 0")
                    break
                time.sleep(0.05)
    finally:
        watch.close()
        off_air(snd)
        lock_again(snd)
    if watch.why:
        raise bench.Refused(f"stopped: {watch.why}")
    bench.close_apps(snd)
    bench.open_page(snd, "A")
    sender(ctx, F20, 6, FIRST)
    time.sleep(1.3)
    q = level(lis, F20, time.time() - 1.0, time.time(), half=500)
    o = ptt_over(ctx, 6, 3.0, what="armed PTT on A")
    w = med_w(snd, o["on"] + 1.0, o["released"] - 0.2)
    heard = rise(q, level(lis, F20, o["on"] + 0.8, o["released"] - 0.2, half=500))
    page = snd.state.get("pg")
    # at the power set: not over it (the meter's whole tenths, cut off, read an AM carrier at 0.1 W as 0.0 or 0.1)
    ok = not went and (w is None or w <= 0.3) and heard is not None and heard >= 15 and page == "A"
    return ok, (f"armed on A: KEY and MSG {'sent NOTHING' if not went else 'WENT ON THE AIR: ' + ', '.join(went)}; PTT "
                f"(AM, 0.1 W set): {fmt(w, 2)} W by its meter, heard {fmt(heard)} dB up, the page {page}")


def t_armed_ptt_a(run):
    pair_test(run, "armed-ptt-a", body_armed_ptt_a, partial=["sentry-armed-ptt"])


# the listener reads the X6100's AM carrier in USB 1 kHz under it, then in AM and SAM on its dial: dial, mode and filter
# apart on purpose
SMETER_AM_APART = ("freq", "mode", "flo", "fhi")


def body_smeter_am(ctx):
    """main-screen-x6200-smeter-am-wfm (part: AM and SAM; WFM is broadcast, never sent on): the X6100's AM carrier at
    0.1 W; the X6200's S-meter in USB (the carrier at 1 kHz in its passband), then AM and SAM on it: within 6 dB of
    the USB reading, 15 dB over the quiet in AM, and back to the quiet after"""
    snd, lis = ctx.snd, ctx.lis
    if not str(lis.state.get("fp", "")).startswith("x6200"):
        raise NotRun(f"{lis.name} is not an X6200 (its fingerprint {lis.state.get('fp')!r})")
    prove(ctx, F20)
    listen(ctx, F20, 6)
    time.sleep(2.0)
    q_am = med([v for _, v in meters(lis, time.time() - 1.5, time.time())])
    decl = bench.apart({"freq": 1000, "mode": [(6, 2), (6, 8)], "filter": bench.STEPPED},
                       "smeter-am: the listener reads the AM carrier in USB 1 kHz under it, then AM and SAM on its "
                       "dial (each mode's own filter)")
    decl.__enter__()
    try:
        return _smeter_am(ctx, snd, lis, q_am)
    finally:
        decl.__exit__(None, None, None)


def raw_peak(r, t0, t1):
    """The X6200 base's own spectrum ("wide 1": 512 bins over 384 kHz) t0..t1: its strongest bin, dB (the median of
    the packets'), None without packets"""
    import struct
    pk = []
    for at, _n, data in list(r.r.frames):
        if t0 <= at <= t1 and data[:2] == b"\xfc\xff" and len(data) >= 16:
            n = struct.unpack_from("<H", data, 2)[0]
            pk.append(max(struct.unpack_from(f"<{n}b", data, 16)))
    return med(pk) if pk else None


def shown(r, t0, t1):
    """The spectrum as r shows it t0..t1: (its span Hz, its strongest bin dB) of the last frame, (None, None)"""
    fr = [(w, max(v)) for at, _c, w, v in list(r.r.spectra) if t0 <= at <= t1 and v]
    return fr[-1] if fr else (None, None)


def _smeter_am(ctx, snd, lis, q_am):
    listen(ctx, F20 - 1000, 2)
    sender(ctx, F20, 6, FIRST, apart=SMETER_AM_APART)     # the listener in USB 1 kHz under it, on purpose
    win, why = {}, {}

    def one(name):                  # the reading, and what it came from: the base's own bins, the line as shown
        t0 = time.time()
        win[name] = window(lis, 1.8)
        why[name] = (raw_peak(lis, t0, time.time()),) + shown(lis, t0, time.time())

    def during(o):                  # about 10 s: each mode's reading 1.8 s after 0.6 s to settle
        time.sleep(1.0)
        one("usb")
        bench.ensure(lis, {"freq": F20, "mode": 6})
        time.sleep(0.6)
        one("am")
        try:
            bench.ensure(lis, {"mode": 8})
            time.sleep(0.6)
            one("sam")
        except bench.Refused as e:
            win["sam_no"] = str(e)
        bench.ensure(lis, {"mode": 6})
        o["release"]()
    if str(lis.state.get("fp", "")).startswith("x6200"):
        lis.send("wide 1")
        ctx.undo.append(lambda: (lis.send("wide 0"), None)[1])
    ctx.banner(f"S-METER IN AM: {snd.name.upper()} SENDS AN AM CARRIER 0.1 W, {lis.name.upper()} READS IT IN USB, AM, "
               f"SAM")
    o = ptt_over(ctx, 6, 12.0, during=during, what="AM carrier for the S-meter",
                 ignore=SMETER_AM_APART)       # the listener reads it in USB, AM and SAM on purpose
    time.sleep(1.5)
    after = med([v for _, v in meters(lis, time.time() - 1.2, time.time())])
    usb, am, sam = win.get("usb"), win.get("am"), win.get("sam")
    ok = (None not in (q_am, usb, am, sam, after) and am - q_am >= 15 and abs(am - usb) <= 6 and after - q_am <= 6
          and abs(sam - usb) <= 6 and "sam_no" not in win and heard_ok(o))
    return ok, (f"{lis.name}'s S-meter: AM quiet {fmt(q_am)} dBm; the carrier ({fmt(o['peak_w'], 2)} W) in USB "
                f"{fmt(usb)}, AM {fmt(am)}, SAM {fmt(sam)}" + (f" (SAM refused: {win['sam_no']})" if "sam_no" in win
                                                              else "") + f"; AM after it {fmt(after)} dBm; from "
                + ", ".join(f"{k}: the base's peak bin {fmt(v[0])} dB, shown over {v[1]} Hz peak {fmt(v[2])}"
                            for k, v in why.items()))


def t_smeter_am(run):
    pair_test(run, "smeter-am", body_smeter_am, partial=["main-screen-x6200-smeter-am-wfm"],
              pairs=one_way(run, "x6100", "x6200"))


COMP_FILE = "/tmp/bench_air_comp.raw"
COMP_LOUD, COMP_SOFT = 0.3, 0.03        # 20 dB apart
# The range at 1:1 at least this, or Comp is not to be told from the sender's own levelling: 2026-11-23 both senders
# put the 20 dB out as 10-11 on the listener's spectrum at 1:1, at 0.3/0.03 and at 0.08/0.008 alike (the TCI audio's
# level set per transmission and the base's power loop), the X6200's meter 0.2 W in the loud part at 1:1, 0.1 at 4:1
COMP_LINEAR_DB = 16.0
COMP_SEG_S = 0.15           # loud and soft take turns this fast: under the power loops (each levels a stretch of 0.5 s
COMP_PAIRS = 16             # and more: 2 s stretches went out 10-11 dB apart at 1:1, 2026-11-23/24), over the base's
                            # compressor's attack (milliseconds)


def comp_audio(path):
    """1000 Hz, loud and 20 dB softer taking turns every COMP_SEG_S, COMP_PAIRS times, after 0.3 s of quiet, 16-bit 24
    kHz, the tone's phase kept across the steps (no click): its length (s)"""
    import numpy as np
    fs = 24000
    n = int(round(COMP_SEG_S * fs))
    k = np.arange(2 * COMP_PAIRS * n)
    amp = np.where((k // n) % 2 == 0, COMP_LOUD, COMP_SOFT)
    tone = amp * np.sin(2 * np.pi * 1000 * k / fs)
    x = np.concatenate([np.zeros(int(0.3 * fs)), tone, np.zeros(int(0.4 * fs))])
    r = int(0.02 * fs)
    a, b = int(0.3 * fs), int(0.3 * fs) + len(tone)
    x[a:a + r] *= np.linspace(0, 1, r)
    x[b - r:b] *= np.linspace(1, 0, r)
    (np.clip(x, -1, 1) * 32767).astype("<i2").tofile(path)
    return len(x) / fs


def two_levels(x):
    """The loud and the soft steps in received audio (24 kHz, the 1000 Hz band only, 10 ms frames): the median of each
    kind's middle frames in dBFS. The steps' phase is the one that sets the two kinds furthest apart over the whole
    over (an onset fell on a soft step when the first 0.15 s went out late, 2026-11-25, and swapped them), its first
    and last 0.6 s left out (the loops' and the compressor's start, the fade); (None, None) when not found"""
    import numpy as np
    fs, n = 24000, 240
    if len(x) < fs:
        return None, None
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / fs)
    X[(f < 900) | (f > 1100)] = 0
    y = np.fft.irfft(X, len(x))
    lv = np.array([20 * math.log10(np.sqrt(np.mean(y[i:i + n] ** 2)) + 1e-12) for i in range(0, len(y) - n, n)])
    if len(lv) < 60:
        return None, None
    quiet = float(np.median(lv[:20]))
    on = np.where(lv > quiet + 10)[0]
    if len(on) < 120:
        return None, None
    per = n / fs
    a, b = on[0] + int(0.6 / per), on[-1] - int(0.6 / per)
    step = int(round(COMP_SEG_S / per))         # frames a step
    idx = np.arange(a, b)
    best = None
    for ph in range(2 * step):
        k = (idx - ph) % (2 * step)
        mid = lambda lo: idx[(k >= lo + 4) & (k <= lo + step - 3)]       # noqa: E731 (a step's middle frames)
        if len(mid(0)) < 20 or len(mid(step)) < 20:
            continue
        loud, soft = float(np.median(lv[mid(0)])), float(np.median(lv[mid(step)]))
        if best is None or loud - soft > best[0] - best[1]:
            best = (loud, soft)
    return best if best else (None, None)


def body_comp(ctx):
    """settings-audio-compressor, tx-comp-ratio: a tone taking turns loud and 20 dB softer every 0.15 s through the
    sender's TCI, in USB and in USB-D, at Comp 1:1 and 4:1; the loud-to-soft range at 4:1 at least 3 dB under 1:1's in
    USB; in USB-D the same (the X6100's base leaves the data modes out). Judged on the listener's audio (AGC off, the
    1000 Hz band in 10 ms frames): its spectrum comes too seldom for 0.15 s steps (2 s steps, judged there, were
    levelled by both senders' power loops at 1:1 already, 2026-11-23/24). Comp put back by finish()"""
    from tones import TciRx
    import orch_cable as oc
    snd, lis = ctx.snd, ctx.lis
    caps = oc.caps_of(snd)
    if caps is not None and not caps & oc.CAP_COMP:
        # Xiegu's own X6100 base has no compressor (radio_caps without HW_CAP_COMP; at 4:1 it sent as at 1:1,
        # 2026-11-25): Comp greyed in its place and refused, as comp-mic judges it; nothing sent
        got = {"caps": caps}
        bad = oc.lacks_check(snd, got, cfgs={"comp": 4}, sets={"comp": 4}, buttons=("Comp",))
        if bad:
            return False, f"{snd.name}'s base has no compressor, yet {'; '.join(bad)}"
        return True, (f"{snd.name}'s base has no compressor (radio_caps {caps}): Comp greyed on MFK 2:4 "
                      f"'{got['pages'].get('MFK 2:4')}', refused: {', '.join(got['refused'])}; nothing sent")
    x6100 = str(snd.state.get("fp", "")).startswith("x6100")
    comp0 = snd.state.get("comp")
    local = os.path.join(ctx.run.dir, f"air_comp_{snd.name}.raw")
    secs = comp_audio(local)
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{snd.host}:{COMP_FILE}"], check=True,
                   timeout=30)
    ctx.undo.append(lambda: (ssh(snd.host, f"rm -f {COMP_FILE}"), None)[1])
    prove(ctx, F20)
    listen(ctx, F20, 2, agc=0)
    tci = TciRx(lis.host)
    ctx.undo.append(tci.close)
    time.sleep(1.0)
    res = {}
    for m in (2, 3):
        listen(ctx, F20, m, agc=0)
        for comp in (1, 4):
            sender(ctx, F20, m, FIRST)
            bench.ensure(snd, {"comp": comp})
            ctx.banner(f"COMP {comp}:1 {'USB' if m == 2 else 'USB-D'}: {snd.name.upper()} SENDS A LOUD AND A SOFT "
                       f"TONE 0.1 W, {lis.name.upper()} LISTENS (AGC OFF)")
            o = tci_over(ctx, file=COMP_FILE, mode=m, max_s=secs + 1.5, what=f"comp {comp}:1 mode {m}")
            time.sleep(0.8)
            x, _ = tci.audio(o["on"] - 1.0, o["off"] + 0.3)
            loud, soft = two_levels(x)
            w = med_w(snd, o["on"] + 0.8, o["off"] - 0.5)
            res[(m, comp)] = (loud, soft, w, heard_ok(o))

    def rng(k):
        a, b = res[k][0], res[k][1]
        return None if a is None or b is None else a - b
    r1, r4, d1, d4 = rng((2, 1)), rng((2, 4)), rng((3, 1)), rng((3, 4))
    voice = None not in (r1, r4) and r4 <= r1 - 3.0
    data = None not in (d1, d4) and abs(d4 - d1) <= 1.5
    heard = all(v[3] for v in res.values())
    says = (f"Comp as found {comp0}; the loud-to-soft range (20 dB sent, steps of {COMP_SEG_S} s) in {lis.name}'s "
            f"audio: USB 1:1 {fmt(r1)} dB, 4:1 {fmt(r4)} dB; USB-D 1:1 {fmt(d1)}, 4:1 {fmt(d4)}; {snd.name}'s meter "
            f"over the over: USB {fmt(res[(2, 1)][2], 2)} -> {fmt(res[(2, 4)][2], 2)} W at 4:1; all heard {heard}")
    if not heard or None in (r1, r4, d1, d4):
        return False, says + " (the steps not found in the audio)"
    if r1 < COMP_LINEAR_DB:
        # its power loop levelled the tone at 1:1 already: what Comp does is not to be told from it
        return None, says + (f"; at 1:1 the 20 dB went out as {r1:.1f} (under {COMP_LINEAR_DB:.0f}): the sender's own "
                             f"power loop levels it, Comp not measurable so")
    if x6100:
        if not voice:   # MANUAL: what Comp does with Xiegu's own X6100 base was never measured
            return None, says + ("; no compression seen in USB: on Xiegu's own X6100 base the manual leaves Comp "
                                 "unmeasured")
        return data, says + ("" if data else "; USB-D CHANGED with Comp (the X6100's base leaves the data modes out)")
    if not voice:
        # TCI's audio goes in as the modem's (radio_set_modem), and on the X6200 Comp left it alone in USB as in USB-D
        # (2026-11-25: 16.9 dB at 1:1, 17.4 at 4:1, steps the power loop does not level): its base keeps Comp for the
        # mic's voice (MANUAL: "It is for your voice"). Judged through the mic (the other radio's speaker into it,
        # acoustic.py's path) next: TODO
        return None, says + ("; Comp leaves TCI's audio alone, USB as USB-D (it goes in as the modem's; Comp is the "
                             "mic's voice processing): to be judged through the mic")
    return voice, says + ("" if data else " (USB-D changed too: said)")


def t_comp(run):
    pair_test(run, "comp", body_comp, covers=["settings-audio-compressor", "tx-comp-ratio"])


def nothing_there(r_other, r_sent):
    """Nothing sent on the other frequency: its rise under 10 dB, or 20 dB and more under the over's own (a skirt, not
    a signal: the X6100's spectrum spreads a strong signal across its span, pictured 2026-11-23: the X6200's 0.1 W tone
    65 dB up on B lifted it 15 dB on A, 30 kHz away, its CW 40 dB up on A 13.5 dB on B, 10 kHz away; the X6200's own
    spectrum 0 dB there the other way. The same sender at the same power there would read as high as on its own)"""
    return r_other is None or r_other < 10 or (r_sent is not None and r_other <= r_sent - 20)


# the listener between A and B, the sender in split (its listen VFO moves to B for each TX), the X6200 in AM there
SPLIT_IGNORE = ("freq", "split", "vfo", "mode", "flo", "fhi")


def body_split(ctx):
    """In split (VFO B 30 kHz up): the sender's TCI audio lands on B + 1 kHz, nothing on A (tx-shift-apps-on-txfreq,
    part: TCI); the X6200's PTT (AM) goes out on B, nothing on A, and the dial is A again after
    (tx-x6200-ptt-split-txfreq, part: no knob under PTT, no new B under it)"""
    snd, lis = ctx.snd, ctx.lis
    A, B = F20, F20 + 30000
    prove(ctx, A)
    with bench.apart({"freq": 15000, "split": (1, 0), "vfo": (1, 0), "mode": [(6, 3)], "filter": bench.STEPPED},
                     "split: the sender on A with its TX on B (VFO B, split), the listener between them, 15 kHz from "
                     "each; the X6200's PTT in AM (its own filter)"):
        return _split(ctx, snd, lis, A, B)


def _split(ctx, snd, lis, A, B):
    listen(ctx, A + 15000, 3)
    sender(ctx, A, 3, FIRST, apart=("freq",))     # the listener between A and B, on purpose
    vfo_b(ctx, B, 3)
    split_on(ctx, B)
    time.sleep(1.3)
    qa = level(lis, A + 1000, time.time() - 1.0, time.time(), half=300)
    qb = level(lis, B + 1000, time.time() - 1.0, time.time(), half=300)
    ctx.banner(f"SPLIT: {snd.name.upper()} ON {A / 1e6:.3f}, TX ON {B / 1e6:.3f}: TCI TONE 0.1 W, {lis.name.upper()} "
               f"LISTENS")

    def check(label, o, fa, fb, covers):
        t0, t1 = o["on"] + 0.8, min(o["sent"][1] if "sent" in o and o["sent"] else o["off"], o["off"]) - 0.2
        ra, rb = rise(qa, level(lis, fa, t0, t1, half=300)), rise(qb, level(lis, fb, t0, t1, half=300))
        back = (int(snd.state.get("freq", 0)), int(snd.state.get("split", -1)), int(snd.state.get("vfo", -1)))
        ok = rb is not None and rb >= 15 and nothing_there(ra, rb) and back == (A, 1, 0) and heard_ok(o)
        return ctx.row(label, ok, f"on B {fb / 1e6:.4f} MHz {fmt(rb)} dB up, on A {fa / 1e6:.4f} {fmt(ra)} dB ("
                                  f"{'nothing of its own' if nothing_there(ra, rb) else 'SENT THERE TOO'}); "
                                  f"{fmt(o['peak_w'], 2)} W; after it dial {back[0]} split {back[1]} VFO {back[2]} "
                                  f"(A {A}, 1, 0 wanted)", (), covers)
    o = tci_over(ctx, plan=[[0, 0.3], [1000, 3.0]], mode=3, what="split TCI", ignore=SPLIT_IGNORE)
    check("split-tci", o, A + 1000, B + 1000, ["tx-shift-apps-on-txfreq"])
    if str(snd.state.get("fp", "")).startswith("x6200"):
        bench.ensure(snd, {"mode": 6})
        keep_mode_mem(ctx, snd, A, 6)       # AM's own filter and zoom, for finish
        align(ctx)                          # its receiver keys as the listener's again: AM brings its own zoom
        time.sleep(1.3)
        qa = level(lis, A, time.time() - 1.0, time.time(), half=300)
        qb = level(lis, B, time.time() - 1.0, time.time(), half=300)
        o = ptt_over(ctx, 6, 4.0, what="split PTT", ignore=SPLIT_IGNORE)
        o["sent"] = (o["on"], o["released"])
        check("split-ptt", o, A, B, ["tx-x6200-ptt-split-txfreq"])
    return ctx.rows


def t_split(run):
    pair_test(run, "split", body_split, partial=["tx-shift-apps-on-txfreq", "tx-x6200-ptt-split-txfreq"])


def body_split_cw(ctx):
    """tx-x6200-split-cw-on-listen-vfo (part: the GUI's keyer through TCI; not the base's own keying): the X6200 in
    split, A 14.050 CW, B 14.060: CW text keyed through its TCI goes out on A, the listener on A decodes it, nothing
    on B"""
    snd, lis = ctx.snd, ctx.lis
    A, B = 14050000, 14060000
    prove(ctx, A)
    with bench.apart({"freq": B - A, "split": (1, 0), "vfo": (1, 0)},
                     "split-cw: the sender in split, A 14.050 and B 14.060, keyed on its listen VFO A; the listener "
                     "on A"):
        return _split_cw(ctx, snd, lis, A, B)


def _split_cw(ctx, snd, lis, A, B):
    listen(ctx, A, 4, agc=2)
    sender(ctx, A, 4, FIRST)
    vfo_b(ctx, B, 4)
    split_on(ctx, B)
    time.sleep(1.3)
    qa = level(lis, A, time.time() - 1.0, time.time(), half=1000)
    qb = level(lis, B, time.time() - 1.0, time.time(), half=1000)
    ctx.banner(f"SPLIT CW: {snd.name.upper()} ON {A / 1e6:.3f}, B {B / 1e6:.3f}: CW 0.1 W ON THE LISTEN VFO, "
               f"{lis.name.upper()} DECODES ON A")
    lis.cw = ""
    o = tci_cw_over(ctx, "TEST TEST", 12, ignore=("freq", "split", "vfo"))
    time.sleep(1.5)
    t0, t1 = (o["on"] or o["t0"]) + 0.5, o["off"]
    ra, rb = rise(qa, level(lis, A, t0, t1, half=1000)), rise(qb, level(lis, B, t0, t1, half=1000))
    pk, _, per = spectrum_peak_hz(lis, A, t0, t1, span=1500)     # where on A it went: the listener's CW passband
    got = " ".join(lis.cw.split())
    ok = "TEST" in got.upper() and ra is not None and ra >= 15 and nothing_there(rb, ra)
    return ok, (f"{snd.name} keyed 'TEST TEST' ({fmt(o['peak_w'], 2)} W, {o['out']}); {lis.name}'s decoder on A: "
                f"{got[-60:]!r}; the carrier {'at ' + format(pk - A, '+.0f') + ' Hz from A' if pk else 'not found'}"
                f" (bins {fmt(per, 0)} Hz; {lis.name}'s CW filter {lis.state.get('flo')}-{lis.state.get('fhi')} Hz, "
                f"key tone {lis.state.get('kt')}); A {fmt(ra)} dB up, B {fmt(rb)} dB ("
                f"{'nothing of its own' if nothing_there(rb, ra) else 'SENT THERE TOO'})")


def t_split_cw(run):
    pair_test(run, "split-cw", body_split_cw, partial=["tx-x6200-split-cw-on-listen-vfo"],
              pairs=one_way(run, "x6200", "x6100"))


def body_cw_alc(ctx):
    """alc-cw-grey (part: the reading, not the bar's colour): CW keyed through TCI at 0.1 W: the base sets the power
    itself, its ALC reading 2 to 10 (the manual: about 5) while the power stays near the setting"""
    snd, lis = ctx.snd, ctx.lis
    f = 14040000
    prove(ctx, f)
    listen(ctx, f, 4, agc=2)
    sender(ctx, f, 4, FIRST)
    ctx.banner(f"ALC IN CW: {snd.name.upper()} KEYS CW 0.1 W, {lis.name.upper()} LISTENS")
    o = tci_cw_over(ctx, "TEST TEST", 12)
    # key down: a reading of power or of the ALC (the meter reads whole tenths cut off: 0.09 W reads 0.0)
    down = [(w, alc) for _, w, _, alc in tx_frames(snd, (o["on"] or o["t0"]) + 0.3, o["off"])
            if w >= 0.05 or alc >= 0.5]
    alc, w = med([a for _, a in down]), med([x for x, _ in down])
    # each base's own: the X6100's ALC brings the carrier down to the power set (2 to 10, the manual's "about 5");
    # the X6200's reads about 0 from 0.03 to 5 W, whatever the drive (HW_CAP_TX_NO_ALC, hw_x6200.c; 0.0 at 0.1 W in
    # CW, 2026-11-24), its power held all the same
    x6200 = str(snd.state.get("fp", "")).startswith("x6200")
    lo, hi, want = (0.0, 1.0, "about 0 on the X6200") if x6200 else (2.0, 10.0, "about 5")
    ok = len(down) >= 5 and alc is not None and lo <= alc <= hi and w is not None and w <= 0.3
    return ok, (f"{len(down)} key-down TX meter frames: ALC {fmt(alc)} ({want} wanted), {fmt(w, 2)} W at a 0.1 W "
                f"setting ({o['out']})")


def t_cw_alc(run):
    pair_test(run, "cw-alc", body_cw_alc, partial=["alc-cw-grey"])


def keep_out_of_band(ctx, r):
    """r's memory of the frequencies outside the bands (band_params' row -1: no row of the band table, so finish's
    restore_bands does not cover it) put back by finish: the dial set there first as found, then the rest put back
    (suite_more.digi_over's way). A QSY out of a band writes it: 2026-11-23 the X6200's went 10 MHz -> 14.36 MHz. Always
    put first (the params file is written a while after a change: read right after the test it can be the old)"""
    m = ((bench.band_memories(r) or {}).get("mem") or {}).get(-1, {})
    if not m.get("vfoa_freq"):
        return
    want = {"freq": m["vfoa_freq"]}
    want.update({k: m[f"vfoa_{k}"] for k in ("mode", "att", "pre", "agc") if f"vfoa_{k}" in m})
    ctx.before.setdefault(r.name, []).insert(0, want)


def body_qsy_out(ctx):
    """tx-stopped-on-qsy-out (the Panel's QSY under PTT): PTT in AM on 14.320 (heard), then 'set freq 14360000', off
    the amateur band (not CB, not broadcast): stopped (off the air: no TX meter at all) or 0 W within a second, not on
    again under the PTT still held, nothing heard at 14.360; a radio still putting out power there a second after the
    QSY is unkeyed at once (the test failed)"""
    snd, lis = ctx.snd, ctx.lis
    f_in, f_out, f_lis = 14320000, 14360000, 14340000
    keep_out_of_band(ctx, snd)          # the QSY to 14.36 MHz writes its memory of the dials outside the bands
    prove(ctx, F20)
    with bench.apart({"freq": 20000}, "qsy-out: the listener on 14.340, between the sender's 14.320 and the 14.360 "
                                      "the Panel tunes it to under PTT: 20 kHz from each"):
        return _qsy_out(ctx, snd, lis, f_in, f_out, f_lis)


def _qsy_out(ctx, snd, lis, f_in, f_out, f_lis):
    listen(ctx, f_lis, 6)
    # its spectrum over both dials, 20 kHz each side: AM's own zoom (8 on both radios, 12.5 kHz) shows neither (the
    # 1:1 suite, 2026-11-24); AM's own filter and zoom kept for finish, the sender takes the zoom from it (align)
    keep_mode_mem(ctx, lis, f_lis, 6)
    bench.ensure(lis, {"zoom": 1})
    sender(ctx, f_in, 6, FIRST, apart=("freq",))    # the listener between the two dials, on purpose
    time.sleep(1.3)
    q_in = level(lis, f_in, time.time() - 1.0, time.time())
    q_out = level(lis, f_out, time.time() - 1.0, time.time())
    res = {}

    def during(o):
        time.sleep(2.0)
        res["w_in"] = med_w(snd, o["on"] + 0.8, time.time())
        res["lv_in"] = level(lis, f_in, o["on"] + 0.8, time.time())
        n0 = len(snd.errs)
        res["t_q"] = t_q = time.time()
        snd.send(f"set freq {f_out}")
        while time.time() - t_q < 3.0:
            if not snd.tx and res.get("off_s") is None:
                res["off_s"] = time.time() - t_q            # stopped: the feature's "until it stops"
            elif snd.tx and res.get("off_s") is not None:
                res["on_again"] = True                      # keyed again under the PTT still renewed: unkeyed now
                break
            late = tx_frames(snd, t_q + 1.0, time.time())
            if late and late[-1][1] > 0.05:
                res["guard"] = late[-1][1]
                break
            time.sleep(0.05)
        o["release"]()
        res["errs"] = snd.errs[n0:]
        res["freq"] = int(snd.state.get("freq", 0))
    ctx.banner(f"QSY OUT: {snd.name.upper()} PTT AM 0.1 W ON {f_in / 1e6:.3f}, THEN THE PANEL TUNES IT TO "
               f"{f_out / 1e6:.3f} (OUTSIDE 20 M): IT MUST GO TO 0 W")
    o = ptt_over(ctx, 6, 8.0, during=during, what="QSY out under PTT", ignore=("freq",))
    t_q = res.get("t_q", o["released"])
    late = tx_frames(snd, t_q + 1.0, o["released"])
    late_max = max([w for _, w, _, _ in late], default=None)
    r_in, r_out = rise(q_in, res.get("lv_in")), rise(q_out, level(lis, f_out, t_q + 0.8, o["released"]))
    taken = res.get("freq") == f_out
    if not taken:
        return None, (f"the QSY to {f_out} was not taken under PTT (the dial {res.get('freq')}; the radio said "
                      f"{res.get('errs')}): nothing to judge")
    # 0 W until it stops (features: "is stopped: ... 0 W until it stops"): off the air within a second (the TX meter
    # comes only on the air: none after it, 2026-11-23, both radios), or on with 0 W read from a second on
    off_s = res.get("off_s")
    stopped = off_s is not None and off_s <= 1.0 and not res.get("on_again")
    zero = bool(late) and (late_max or 0) <= 0.02
    ok = (r_in is not None and r_in >= 15 and "guard" not in res and (late_max or 0) <= 0.02 and (stopped or zero)
          and not res.get("on_again") and (r_out is None or r_out < 10))
    return ok, (f"on {f_in / 1e6:.3f}: {fmt(res.get('w_in'), 2)} W, heard {fmt(r_in)} dB up; after the QSY to "
                f"{f_out / 1e6:.3f}: off the air in {fmt(off_s, 2)} s" + (" (ON AGAIN under the PTT)"
                                                                          if res.get("on_again") else "")
                + f", {len(late)} TX meter frames from a second on, the most {fmt(late_max, 2)} W"
                + (f", STILL {res['guard']:.2f} W: unkeyed by the test" if "guard" in res else "")
                + f"; {lis.name} at {f_out / 1e6:.3f}: {fmt(r_out)} dB"
                + ("" if q_out is not None else " (not in its spectrum's span)")
                + f"; the radio said {res.get('errs')}")


def t_qsy_out(run):
    pair_test(run, "qsy-out", body_qsy_out, covers=["tx-stopped-on-qsy-out"])


# ---- TUNE for an external tuner ----

def tune_lease(ctx, T, L):
    snd, lis = ctx.snd, ctx.lis
    listen(ctx, F20, 2)
    sender(ctx, F20, 2, FIRST)
    res = {}
    n0 = len(snd.errs)

    def during(o):
        time.sleep(3.0)
        res["tune"], res["tune_s"] = snd.state.get("tune"), snd.state.get("tune_s")
        res["w"] = med_w(snd, o["on"] + 0.8, time.time())
        res["heard"] = rise(res["q"], level(lis, F20, o["on"] + 0.8, time.time()))
        o["renew"][0] = False
        res["lapse"] = wait_for(lambda: snd.state.get("tune") == 0, 6.0)
        res["lapse_from_renew"] = time.time() - o["last_renew"] if res["lapse"] is not None else None
    time.sleep(1.2)
    res["q"] = level(lis, F20, time.time() - 1.0, time.time())
    tune_over(ctx, T, during=during)
    lapsed = [e for e in snd.errs[n0:] if "no renewal" in e]

    def stop_now(o):
        time.sleep(2.0)
        t0 = time.time()
        snd.send("tune 0")
        res["stop_s"] = wait_for(lambda: snd.state.get("tune") == 0, 3.0)
        res["stop_at"] = t0
    tune_over(ctx, T, during=stop_now)
    snd.send("set lock 1")
    ctx.undo.append(lambda: (snd.send("set lock 0"), None)[1])
    snd.wait("LOCK on", lambda: snd.state.get("ulock") in (1, True), 5)

    def locked(o):
        time.sleep(2.0)
        res["lock_tune"] = snd.state.get("tune")
        res["lock_w"] = med_w(snd, o["on"] + 0.8, time.time())
        res["lock_heard"] = rise(res["q"], level(lis, F20, o["on"] + 0.8, time.time()))
    try:
        tune_over(ctx, T, during=locked)
    finally:
        snd.send("set lock 0")
        snd.wait("LOCK off", lambda: snd.state.get("ulock") in (0, False), 5)
    near = near_w(res.get("w"), T)
    lapse = res.get("lapse_from_renew")
    ok = (res.get("tune") == 1 and res.get("tune_s") == L and near and (res.get("heard") or 0) >= 15
          and lapse is not None and 2.5 <= lapse <= 5.0 and bool(lapsed)
          and res.get("stop_s") is not None and res["stop_s"] <= 1.0
          and res.get("lock_tune") == 1 and (res.get("lock_heard") or 0) >= 15)
    return ok, (f"tune 1, renewed: tune {res.get('tune')}, its limit {res.get('tune_s')} s ({L} set), "
                f"{fmt(res.get('w'), 2)} W ({T} W set), heard {fmt(res.get('heard'))} dB up; renewals stopped: off "
                f"{fmt(lapse, 1)} s after the last ({lapsed[-1] if lapsed else 'NO word of it'}); tune 0: off in "
                f"{fmt(res.get('stop_s'), 2)} s; with LOCK on: tune {res.get('lock_tune')}, "
                f"{fmt(res.get('lock_w'), 2)} W, heard {fmt(res.get('lock_heard'))} dB up")


def tune_restores(ctx, T):
    snd = ctx.snd
    listen(ctx, F20, 2)
    sender(ctx, F20, 2, FIRST)
    keys = ("freq", "mode", "split", "vfo", "pwr", "txf", "atu", "flo", "fhi")
    before = {k: snd.state.get(k) for k in keys}
    mark = gui_log_mark(snd.host)
    tune_over(ctx, T, secs=3.0)
    time.sleep(1.0)
    after = {k: snd.state.get(k) for k in keys}
    moved = {k: (before[k], after[k]) for k in keys if before[k] != after[k]}
    lvl = [x for x in gui_log_since(snd.host, mark) if "tx_player: level" in x]
    return not moved and not lvl, (f"after a 3 s tune: " + (f"MOVED {moved}" if moved else
                                                              "the dial, mode, split, VFO, PWR, TX frequency, ATU and "
                                                              "filter as before")
                                   + f"; the GUI's log: {len(lvl)} 'tx_player: level' lines" + (f" ({lvl[-1][-80:]})"
                                                                                                if lvl else ""))


def tune_stops(ctx, T):
    snd = ctx.snd
    changes = [("the frequency", f"set freq {F20 + 1000}", {"freq": F20}),
               ("the mode", "set mode 3", {"mode": 2}),
               ("the VFO", "set vfo 1", {"vfo": 0}),
               ("split", "set split 1", {"split": 0})]
    said, good = [], True
    listen(ctx, F20, 2)
    decl = bench.apart({"freq": 1000, "vfo": (1, 0), "split": (1, 0)},
                       "tune-stops: the sender's dial 1 kHz up, its VFO B and split set under TUNE on purpose, each "
                       "put back after (the mode: TUNE's own USB-D, declared by its over)")
    decl.__enter__()
    try:
        return _tune_stops(ctx, T, snd, changes, said, good)
    finally:
        decl.__exit__(None, None, None)


def _tune_stops(ctx, T, snd, changes, said, good):
    for name, cmd, back in changes:
        sender(ctx, F20, 2, FIRST)
        res = {}

        def during(o, cmd=cmd, res=res):
            time.sleep(1.5)
            snd.send(cmd)
            res["s"] = wait_for(lambda: snd.state.get("tune") == 0, 3.0)
            if res["s"] is None:
                snd.send("tune 0")          # it should have stopped by itself: stopped here
            o["renew"][0] = False
        try:
            # the change that stops TUNE, made under it on purpose
            tune_over(ctx, T, during=during, ignore=TUNE_IGNORE + ("freq", "vfo", "split"))
        finally:
            bench.ensure(snd, back)
        this = res.get("s") is not None and res["s"] <= 1.5
        good &= this
        said.append(f"{name} changed: off in {fmt(res.get('s'), 2)} s" + ("" if this else " (NOT stopped)"))
    return good, "; ".join(said)


def tune_refused(ctx, T):
    snd = ctx.snd
    listen(ctx, F20, 6)
    sender(ctx, F20, 6, FIRST)
    res = {}

    def during(o):
        time.sleep(1.5)
        n0 = len(snd.errs)
        t0 = time.time()
        snd.send(f"tune 1 {next(HOLD)}")
        wait_for(lambda: any("tune" in e for e in snd.errs[n0:]), 2.5)
        res["err"] = [e for e in snd.errs[n0:] if "tune" in e]
        time.sleep(0.5)
        res["tuned"] = any(v == 1 for _, v in state_vals(snd, "tune", t0, time.time())) or snd.state.get("tune") == 1
        snd.send("tune 0")
    o = ptt_over(ctx, 6, 5.0, during=during, what="TUNE asked on the air")
    ok = any("on the air" in e for e in res.get("err", [])) and not res.get("tuned") and heard_ok(o)
    return ok, (f"PTT on the air (AM {fmt(o['peak_w'], 2)} W), then tune 1: the radio said {res.get('err')}, TUNE "
                f"{'STARTED' if res.get('tuned') else 'not started'}")


def tune_txfreq(ctx, T):
    snd, lis = ctx.snd, ctx.lis
    A, B = F20, F20 + 30000
    with bench.apart({"freq": 15000, "mode": [(4, 2)], "split": (1, 0), "vfo": (1, 0), "filter": bench.STEPPED},
                     "tune-txfreq: the sender in split, A in CW and B 30 kHz up, the listener in USB midway, 15 kHz "
                     "from each (TUNE's own USB-D declared by its over)"):
        return _tune_txfreq(ctx, T, snd, lis, A, B)


def _tune_txfreq(ctx, T, snd, lis, A, B):
    listen(ctx, A + 15000, 2)
    sender(ctx, A, 4, FIRST, apart=TUNE_IGNORE + ("freq",))     # between A and B in USB, on purpose
    vfo_b(ctx, B, 4)
    split_on(ctx, B)
    time.sleep(1.3)
    qa = level(lis, A, time.time() - 1.0, time.time(), half=600)
    qb = level(lis, B, time.time() - 1.0, time.time(), half=600)
    res = {}

    def during(o):
        time.sleep(2.5)
        res["ra"] = rise(qa, level(lis, A, o["on"] + 0.8, time.time(), half=600))
        res["rb"] = rise(qb, level(lis, B, o["on"] + 0.8, time.time(), half=600))
        res["pk"] = spectrum_peak_hz(lis, B, o["on"] + 0.8, time.time(), span=1500)[0]
    tune_over(ctx, T, during=during, ignore=TUNE_IGNORE + ("freq", "split", "vfo"))
    time.sleep(1.0)
    mode, split = int(snd.state.get("mode", -1)), int(snd.state.get("split", -1))
    pk = res.get("pk")
    rb, ra = res.get("rb") or 0, res.get("ra")
    on_b = rb >= 15 and pk is not None and abs(pk - B) <= 600 and mode == 4 and split == 1
    # A carrier sent on A would rise there as much as on B; 30 kHz from a carrier 60 dB up the listener's own skirts
    # or the sender's noise lift A (the X6200 at the X6100: 14.8-15.9 dB at A, 48-49 dB under B, 11-23 and 11-24; the
    # X6100 at the X6200: none): judged against B, A 30 dB or more under it is no carrier there, its rise said
    ok = on_b and (ra is None or ra < 10 or rb - ra >= 30)
    return ok, (f"split, A {A / 1e6:.3f} CW, B {B / 1e6:.3f}: the carrier on B {fmt(res.get('rb'))} dB up (its peak "
                f"{fmt(pk, 0)} Hz), on A {fmt(ra)} dB" + (f" ({rb - ra:.0f} dB under B: no carrier there, the "
                                                         f"skirts 30 kHz from it)" if ra is not None and ra >= 10
                                                         else "")
                + f"; after it mode {mode} (CW 4 wanted), split {split}")


def body_tune(ctx):
    snd = ctx.snd
    T, L = tune_gate(ctx, TUNE_W_OTHER)
    if L > TUNE_MAX_S:
        say(f"{snd.name}: TUNE's limit {L} s: each tune here is ended by the test well before it")
    prove(ctx, F20)
    listen(ctx, F20, 2)
    ctx.banner(f"TUNE: {snd.name.upper()} TUNES AT {T} W (ITS TUNE POWER), {ctx.lis.name.upper()} LISTENS")
    part(ctx, "tune-lease", lambda: tune_lease(ctx, T, L), covers=["ext-tune-panel-lease"])
    part(ctx, "tune-restores", lambda: tune_restores(ctx, T), partial=["ext-tune-restores-state"])
    part(ctx, "tune-stops", lambda: tune_stops(ctx, T), partial=["ext-tune-stops-on-change"])
    part(ctx, "tune-refused", lambda: tune_refused(ctx, T), partial=["ext-tune-refused-on-air"])
    part(ctx, "tune-txfreq", lambda: tune_txfreq(ctx, T), covers=["ext-tune-carrier-txfreq"])
    return ctx.rows


def t_tune(run):
    pair_test(run, "tune", body_tune, covers=["ext-tune-panel-lease", "ext-tune-carrier-txfreq"],
              partial=["ext-tune-restores-state", "ext-tune-stops-on-change", "ext-tune-refused-on-air"])


def body_tune_power(ctx):
    """ext-tune-power-setting: TUNE goes out at its own power T (1 W at most: a power feature) whatever PWR is, and a
    PWR changed under it changes nothing; the limit: on renewed, TUNE ends by itself at its time limit (15 s at most
    here: the watch's 16)"""
    snd, lis = ctx.snd, ctx.lis
    # TUNE only at a TUNE power of 0.1 W, this test too (the owner's bench rule, 2026-11-23: the TUNE tests run only
    # when the sender's TUNE power is 0.1 W); PWR is still set to 0.5 and 1 W under it (1 W at most: a power level)
    T, L = tune_gate(ctx, TUNE_W_OTHER)
    cand = [0.1, 0.5, 1.0]
    P1 = max(cand, key=lambda p: abs(db(p, T)))
    P2 = max([p for p in cand if p != P1], key=lambda p: abs(db(p, T)))
    prove(ctx, F20)
    listen(ctx, F20, 2)
    sender(ctx, F20, 2, P1)
    ctx.banner(f"TUNE POWER: {snd.name.upper()} TUNES AT {T} W WITH PWR {P1} W, THEN {P2} W UNDER IT")
    time.sleep(1.3)
    q = level(lis, F20, time.time() - 1.0, time.time())
    res = {}

    def during(o):
        time.sleep(2.0)
        res["w1"] = med_w(snd, o["on"] + 0.8, time.time())
        res["heard"] = rise(q, level(lis, F20, o["on"] + 0.8, time.time()))
        snd.send(f"set pwr {round(P2 * 10)}")
        try:
            snd.wait(f"PWR {P2} W", lambda: abs(snd.pwr() - P2) < 0.05, 3)
        except bench.Refused:
            pass
        t0 = time.time()
        time.sleep(2.0)
        res["w2"] = med_w(snd, t0 + 0.5, time.time())
    tune_over(ctx, T, during=during)
    pw = snd.pwr()

    def follows_tune(w, P):
        """TUNE's own power read, not PWR's (P at least 3 dB from T: chosen so)"""
        if not near_w(w, T):
            return False
        if T <= 0.15:
            return True                     # 0.25 W at most, PWR 0.5 W or more
        dt, dp = db(w, T), db(w, P)
        return dt is not None and dp is not None and abs(dt) <= 2.0 and abs(dt) < abs(dp)
    ok = (follows_tune(res.get("w1"), P1) and follows_tune(res.get("w2"), P2) and abs(pw - P2) < 0.05
          and (res.get("heard") or 0) >= 15)
    ctx.row("tune-power", ok, f"TUNE power {T} W: with PWR {P1} W it read {fmt(res.get('w1'), 2)} W (heard "
                              f"{fmt(res.get('heard'))} dB up), PWR {P2} W set under it: {fmt(res.get('w2'), 2)} W "
                              f"(TUNE's {T} W wanted, not PWR's); PWR after it {pw} W",
            ["ext-tune-power-setting"], ["settings-txcal-external-tuner"])
    if L > 15:
        ctx.row("tune-limit", None, f"not run: the TUNE time limit is {L} s, over the 16 s an over may last here",
                (), ["settings-txcal-external-tuner"])
        return ctx.rows
    sender(ctx, F20, 2, FIRST)
    res = {}

    def to_limit(o):
        res["tune_s"] = snd.state.get("tune_s")
        left = L + 0.8 - (time.time() - o["on"])
        res["end"] = wait_for(lambda: snd.state.get("tune") == 0, max(left, 0.5))
        res["dur"] = time.time() - o["on"]      # from TUNE seen on to TUNE seen off: the same delay both ends
        if res["end"] is None:
            snd.send("tune 0")              # past its limit: ended here
    tune_over(ctx, T, during=to_limit)
    dur = res.get("dur")
    ok = res.get("tune_s") == L and res.get("end") is not None and dur is not None and L - 1.0 <= dur <= L + 0.8
    ctx.row("tune-limit", ok, f"renewed all along: its limit {res.get('tune_s')} s ({L} set), ended by itself after "
                              f"{fmt(dur, 1)} s" + ("" if res.get("end") is not None else " (NOT: ended by the test)"),
            (), ["settings-txcal-external-tuner"] + (["ext-tune-time-limit"] if T <= TUNE_W_OTHER + 1e-6 else []))
    return ctx.rows


def t_tune_power(run):
    pair_test(run, "tune-power", body_tune_power, covers=["ext-tune-power-setting"],
              partial=["settings-txcal-external-tuner", "ext-tune-time-limit"])


def ft8_open(ctx, r, dial):
    """r's FT8 app open on dial in FT8 (its UDP to loggers off first), Auto and CQ off; its protocol stepped back as it
    was found and the app closed again at the test's end. The app opens in the protocol it was left in (the X6100's in
    FT4, 2026-11-23: "did not come to 14074000", FT4's 30 m dial) and its MODE steps FT8, FT4, FT2 round: the steps
    taken are stepped on round to where it was, live (the settings put-back fixes the database row only)"""
    udp_off(ctx, r)
    r.send("ft8 open")
    r.wait("the FT8 app", lambda: r.ft8.get("open") == 1, 10)
    time.sleep(1.0)
    steps = [0]

    def close():
        r.send("ft8 stop")
        for _ in range((3 - steps[0] % 3) % 3):
            p = r.ft8.get("proto")
            r.send("ft8 mode")
            if wait_for(lambda p=p: r.ft8.get("proto") != p, 5.0) is None:
                break
            time.sleep(0.5)
        r.send("ft8 close")
        wait_for(lambda: r.ft8.get("open") != 1, 5.0)
    ctx.undo.append(close)
    while r.state.get("app") != "FT8" and steps[0] < 3:
        p = r.ft8.get("proto")
        r.send("ft8 mode")
        r.wait("FT8's mode stepped", lambda p=p: r.ft8.get("proto") != p, 5)
        time.sleep(0.8)
        steps[0] += 1
    if r.state.get("app") != "FT8":
        raise bench.Refused(f"{r.name}: its FT8 app not in FT8 ({r.state.get('app')})")
    bench.ft8_to_band(r, dial)
    if r.ft8.get("auto"):
        r.send("ft8 auto")
        r.wait("Auto off", lambda: not r.ft8.get("auto"), 5)
    if r.ft8.get("cq"):
        r.send("ft8 stop")
        r.wait("CQ off", lambda: not r.ft8.get("cq"), 5)


def body_tune_app(ctx):
    """ext-tune-app-button (part: the Panel's TUNE with the app open, not the app's own button) and
    ext-tune-restores-state (part: the app and its TX offset): with FT8 open on 14.074 and its TX at 1500 Hz the tone
    on 14.0755 MHz, FT8 still open and at 1500 after; with SSTV open, 1900 Hz from its dial on its sideband"""
    import suite_more
    from tones import TciRx, peak as audio_peak
    snd, lis = ctx.snd, ctx.lis
    T, L = tune_gate(ctx, TUNE_W_OTHER)
    dial = 14074000
    # the listener's audio, the path proof's 1000 Hz tone and then TUNE's in it, both in USB-D on the dial: the two
    # 500 Hz apart whatever the two radios' references (the X6100 reads the X6200's carriers 150-170 Hz high at
    # 14 MHz) and the X6200's 750 Hz spectrum bins (its carrier's peak read 404 Hz off, 2026-11-24)
    tci = TciRx(lis.host)
    ctx.undo.append(tci.close)
    proof = prove(ctx, dial)
    x, _ = tci.audio(proof["t_on"] + 0.8, proof["t_off"] - 0.2)
    a_proof = audio_peak(x, 1000, 400)[0]
    listen(ctx, dial, 2)
    sender(ctx, dial, 2, FIRST)
    ft8_open(ctx, snd, dial)
    mirror(ctx)                         # the app holds its own mode, filter, AGC: the listener as it
    txf0 = int(snd.ft8.get("txf") or 0)

    def txf_back():
        """FT8's own TX offset as found (the app open: it takes it only then)"""
        if txf0 and snd.ft8.get("open") == 1 and int(snd.ft8.get("txf") or 0) != txf0:
            snd.send(f"ft8 txfreq {txf0}")
            if wait_for(lambda: int(snd.ft8.get("txf") or 0) == txf0, 5.0) is None:
                return f"{snd.name}: FT8's TX offset reads {snd.ft8.get('txf')}, was {txf0}"
    ctx.undo.append(txf_back)
    snd.send("ft8 txfreq 1500")
    snd.wait("FT8's TX at 1500 Hz", lambda: int(snd.ft8.get("txf") or 0) == 1500, 5)
    app0 = snd.state.get("app")
    want = dial + 1500
    res = {}
    time.sleep(1.3)
    q = level(lis, want, time.time() - 1.0, time.time(), half=300)

    def during(o):
        time.sleep(2.5)
        res["pk"], res["db"], res["bin"] = spectrum_peak_hz(lis, want, o["on"] + 0.8, time.time(), span=2500)
        xa, _ = tci.audio(o["on"] + 0.8, time.time())
        res["audio"] = audio_peak(xa, 1500, 600)[0]
    ctx.banner(f"TUNE IN AN APP: {snd.name.upper()} FT8 OPEN, TX 1500 HZ: TUNE {T} W, {lis.name.upper()} LISTENS")
    o = tune_over(ctx, T, during=during)
    time.sleep(1.0)
    pk, ta = res.get("pk"), res.get("audio")
    after = (snd.state.get("app"), int(snd.ft8.get("txf") or 0), int(snd.state.get("freq", 0)))
    up = rise(q, res.get("db"))
    rest = (up or 0) >= 15 and after == (app0, 1500, dial) and heard_ok(o)
    # TUNE's carrier 500 Hz over the proof's 1000 Hz tone in the listener's audio: at the app's TX offset (1500), not
    # the dial's nor any other; both from the same sender on the same dial, so the references cancel
    apart = None if ta is None or a_proof is None else ta - a_proof
    ok = rest and apart is not None and abs(apart - 500.0) <= 10.0
    why_back = txf_back()               # before the app closes: it takes its offset only while open
    if why_back:
        ok = False
    ctx.row("tune-app-ft8", ok, f"FT8 open, TX 1500 Hz: in {lis.name}'s audio TUNE's carrier at {fmt(ta, 1)} Hz, "
                                f"the path proof's 1000 Hz tone at {fmt(a_proof, 1)} Hz: {fmt(apart, 1)} Hz apart "
                                f"(500 wanted, 10 at most off); its spectrum {fmt(up)} dB up at {fmt(pk, 0)} Hz "
                                f"({want} sent, {fmt(res.get('bin'), 0)} Hz bins); after it the app {after[0]!r} "
                                f"(was {app0!r}), its TX {after[1]} Hz, the dial {after[2]}"
                                + (f"; {why_back}" if why_back else f"; its offset back to {txf0}"),
            (), ["ext-tune-app-button", "ext-tune-restores-state"])
    snd.send("ft8 close")
    wait_for(lambda: snd.ft8.get("open") != 1, 5.0)
    bench.close_apps(snd)
    if not suite_more.open_app(snd, "sstv"):
        ctx.row("tune-app-sstv", None, "not run: SSTV did not open", (), ["ext-tune-app-button"])
        return ctx.rows
    ctx.undo.append(lambda: (bench.close_apps(snd), None)[1])
    time.sleep(1.5)
    f, m = int(snd.state.get("freq", 0)), int(snd.state.get("mode", -1))
    if not any(lo <= f <= hi for lo, hi in bench.HAM_HF) or m not in (0, 1, 2, 3):
        ctx.row("tune-app-sstv", None, f"not run: SSTV opened on {f} Hz mode {m}, no amateur sideband frequency",
                (), ["ext-tune-app-button"])
        return ctx.rows
    lsb = m in (0, 1)
    want = f - 1900 if lsb else f + 1900
    listen(ctx, f, 0 if lsb else 2)
    mirror(ctx)
    res = {}
    time.sleep(1.3)
    q = level(lis, want, time.time() - 1.0, time.time(), half=300)

    def during_sstv(o):
        time.sleep(2.5)
        res["pk"], res["db"], res["bin"] = spectrum_peak_hz(lis, want, o["on"] + 0.8, time.time(), span=2500)
    o = tune_over(ctx, T, during=during_sstv)
    tol = max(100.0, 1.5 * (res.get("bin") or 0))
    pk = res.get("pk")
    up = rise(q, res.get("db"))
    ok = (pk is not None and abs(pk - want) <= tol and (up or 0) >= 15 and heard_ok(o)
          and (snd.state.get("app") or "").upper().startswith("SSTV"))
    ctx.row("tune-app-sstv", ok, f"SSTV open on {f} Hz {'LSB' if lsb else 'USB'}: the tone at {fmt(pk, 0)} Hz, "
                                 f"{fmt(up)} dB up ({want} "
                                 f"wanted: 1900 Hz {'under' if lsb else 'over'} the dial); the app after it "
                                 f"{snd.state.get('app')!r}", (), ["ext-tune-app-button"])
    return ctx.rows


def t_tune_app(run):
    pair_test(run, "tune-app", body_tune_app, partial=["ext-tune-app-button", "ext-tune-restores-state"])


def body_tune_key(ctx):
    """ext-tune-key-action, radio-core-key-tx-actions (part: TUNE ext. tuner on a front key's long press; not a hand
    mic key, not with an app open; the ATU's tune never: an antenna tuner): FST's long press set to it through the
    configurator socket (as the web's Keys page sets it, put back after and read back), held 1.5 s: TUNE on, heard;
    held again: off"""
    snd, lis = ctx.snd, ctx.lis
    T, L = tune_gate(ctx, TUNE_W_OTHER)
    if L > 15:
        raise NotRun(f"the TUNE time limit is {L} s: a tune the key failed to stop would last over the 16 s an over "
                     f"may last")
    keys = cfg_sock(snd.host, {"cmd": "keys_get"})
    act = next((a for a, label in keys.get("actions", []) if re.search(r"tune\s*ext", label, re.I)), None)
    top = next((k for k in keys.get("top", []) if k.get("id") == "fst"), None)
    if act is None or top is None:
        raise NotRun(f"no 'TUNE ext. tuner' action ({act}) or no FST key ({bool(top)}) in Settings > Keys")
    h0 = int(top["hold"])
    at = {}                     # the step as found in the test's own mode (each mode keeps its own: read on G's AM
                                # channel first, it was AM's 1000 against USB's 500 after, and "set back" wrote 1000
                                # into USB's, 2026-11-23)

    def back():
        cfg_sock(snd.host, {"cmd": "keys_set", "key": "hold_fst", "val": h0})
        time.sleep(0.5)
        now = next((int(k["hold"]) for k in cfg_sock(snd.host, {"cmd": "keys_get"}).get("top", [])
                    if k.get("id") == "fst"), None)
        if now != h0:
            return f"FST's long press reads {now}, was {h0}"
        if at and int(snd.state.get("mode", -1)) == at["mode"] and snd.state.get("step") != at["step"]:
            step = snd.state.get("step")
            snd.send(f"set step {at['step']}")
            return f"the tuning step moved in mode {at['mode']} ({step}, was {at['step']}): set back"
    ctx.undo.append(back)
    r = cfg_sock(snd.host, {"cmd": "keys_set", "key": "hold_fst", "val": act})
    time.sleep(0.5)
    now = next((int(k["hold"]) for k in cfg_sock(snd.host, {"cmd": "keys_get"}).get("top", [])
                if k.get("id") == "fst"), None)
    if now != act:
        raise bench.Refused(f"FST's long press not set to TUNE ext. tuner ({r}; it reads {now})")
    prove(ctx, F20)
    listen(ctx, F20, 2)
    sender(ctx, F20, 2, FIRST)
    time.sleep(1.2)
    at.update(mode=int(snd.state.get("mode", -1)), step=snd.state.get("step"))
    q = level(lis, F20, time.time() - 1.0, time.time())

    def press():
        snd.send("key FST 1")
        time.sleep(1.5)
        snd.send("key FST 0")
    res = {}

    def during(o):
        time.sleep(2.0)
        res["w"] = med_w(snd, o["on"] + 0.8, time.time())
        res["heard"] = rise(q, level(lis, F20, o["on"] + 0.8, time.time()))
        press()
        res["off"] = wait_for(lambda: snd.state.get("tune") == 0, 3.0)
    ctx.banner(f"TUNE ON A KEY: {snd.name.upper()} FST HELD: TUNE {T} W ON, HELD AGAIN: OFF; {lis.name.upper()} "
               f"LISTENS")
    tune_over(ctx, T, during=during, renew=False, start=press)
    ok = res.get("off") is not None and (res.get("heard") or 0) >= 15 and near_w(res.get("w"), T)
    return ok, (f"FST's long press set to TUNE ext. tuner (action {act}, was {h0}): held: TUNE on, "
                f"{fmt(res.get('w'), 2)} W, heard {fmt(res.get('heard'))} dB up; held again: off in "
                f"{fmt(res.get('off'), 2)} s" + ("" if res.get("off") is not None else " (NOT: ended by the test)"))


def t_tune_key(run):
    pair_test(run, "tune-key", body_tune_key, partial=["ext-tune-key-action", "radio-core-key-tx-actions"])


TEXT_LOG_TEXT = "TEXT LOG CHECK K"     # no "DE" and no call in it: nothing for PSK Reporter
TEXT_LOG_LINE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\dZ) (\d+\.\d{6}) (\S+) (RX|TX) (\S+) (.*)$")


def body_text_log(ctx):
    """text-log-file (part: a received line; the sender's TX line needs its app to send): PSK31 through the sender's
    TCI at 0.1 W into the listener's PSK app: in /mnt/digi_text/<UTC date>.txt a line 'date time MHz mode RX Q% text'
    with the text, the frequency where the signal is, ASCII only. The day's file put back as it was after"""
    import digimodes
    import suite_more
    from suite_cover import keep
    snd, lis = ctx.snd, ctx.lis
    day = time.strftime("%Y-%m-%d", time.gmtime())
    path = f"/mnt/digi_text/{day}.txt"
    had_dir = ssh(lis.host, "[ -d /mnt/digi_text ] && echo yes || echo no").strip()
    k0 = keep(lis.host, path, "save")

    def back():
        k1 = keep(lis.host, path, "back")
        if had_dir == "no":
            ssh(lis.host, "rmdir /mnt/digi_text 2>/dev/null; true")
        if k1 not in ("same", "restored", "removed"):
            return f"{path}: {k1!r}"
    ctx.undo.append(back)
    size0 = ssh(lis.host, f"[ -f {path} ] && wc -c < {path} || echo 0").strip().split()
    size0 = int(size0[0]) if size0 and size0[0].isdigit() else 0
    # where the PSK app listens: opened once to see, closed for the path proof (which tunes the listener itself),
    # opened again for the over
    ctx.undo.append(lambda: (bench.close_apps(lis), None)[1])
    with bench.transition():            # the listener's app tunes it alone: the sender follows before it ends
        if not suite_more.open_app(lis, "psk"):
            raise bench.Refused(f"{lis.name}: its PSK app did not open")
        time.sleep(2.0)
        f, m = int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))
        bench.close_apps(lis)
        bench.settled(lis)              # the app's dial put back as it closes, a moment after
        if m not in (2, 3, 0, 1) or not any(lo <= f <= hi for lo, hi in bench.HAM_HF):
            raise NotRun(f"the PSK app opened on {f} Hz mode {m}: no amateur sideband frequency")
        bench.sync_pair([snd, lis], {"freq": f, "mode": m})
    x = digimodes.psk31_audio(TEXT_LOG_TEXT)
    local = os.path.join(ctx.run.dir, f"air_psk_{snd.name}.raw")
    secs = digimodes.write_raw(x, local)
    if secs + 1.5 > MAX_OVER_S:
        raise bench.Refused(f"the PSK31 text lasts {secs:.1f} s: too long for one over")
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{snd.host}:/tmp/bench_air_psk.raw"],
                   check=True, timeout=30)
    ctx.undo.append(lambda: (ssh(snd.host, "rm -f /tmp/bench_air_psk.raw"), None)[1])
    bench.open_page(lis, "G")
    prove(ctx, f, mode=m)
    if not suite_more.open_app(lis, "psk"):
        raise bench.Refused(f"{lis.name}: its PSK app did not open again")
    time.sleep(2.0)
    if (int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))) != (f, m):
        raise bench.Refused(f"{lis.name}'s PSK app opened on {lis.state.get('freq')} mode {lis.state.get('mode')} the "
                            f"second time ({f} mode {m} the first)")
    bench.ensure(lis, {"att": 0, "pre": 0, "rfg": 100, "sql": 0, "nb": 0, "nr": 0, "rit": 0})
    sender(ctx, f, m, FIRST)            # its receiver as the listener's with the app open
    ctx.banner(f"TEXT LOG: {snd.name.upper()} SENDS PSK31 0.1 W, {lis.name.upper()}'S PSK APP WRITES ITS LINE")
    o = tci_over(ctx, file="/tmp/bench_air_psk.raw", mode=m, max_s=secs + 1.5, what="PSK31 for the text log")
    time.sleep(2.0)
    bench.close_apps(lis)               # PSK31's line never pauses on noise: the app's closing ends it
    got, t0 = "", time.time()
    while time.time() - t0 < 30 and "LOG CHECK" not in got.upper():
        time.sleep(3.0)
        got = ssh(lis.host, f"[ -f {path} ] && tail -c +{size0 + 1} {path}; true")
    lines = [ln for ln in got.splitlines() if "LOG" in ln.upper()]
    mt = TEXT_LOG_LINE.match(lines[-1]) if lines else None
    sig_hz = f + 1000 if m in (2, 3) else f - 1000
    ok = (mt is not None and mt.group(4) == "RX" and "PSK" in mt.group(3).upper()
          and re.fullmatch(r"Q\d+%", mt.group(5)) is not None and abs(float(mt.group(2)) * 1e6 - sig_hz) <= 3000
          and all(ord(ch) < 128 for ch in lines[-1]) and "LOG CHECK" in mt.group(6).upper() and heard_ok(o))
    return ok, (f"{snd.name} sent PSK31 {TEXT_LOG_TEXT!r} at {f} Hz ({fmt(o['peak_w'], 2)} W); {lis.name}'s "
                f"{path}: " + (repr(lines[-1][-120:]) if lines else f"NO line of it ({' '.join(got.split())[-80:]!r})")
                + f" (the signal at {sig_hz / 1e6:.6f} MHz; the file {k0} before, put back after)")


def t_text_log(run):
    pair_test(run, "text-log", body_text_log, partial=["text-log-file"])


def body_codec_clock(ctx):
    """tx-x6100-gcfix-codec-clock: the X6200's 0.1 W tone near the centre and near the edge of the X6100's spectrum;
    the X6100's peak (a parabola through its bins) against where it was sent: the edge's error less the centre's (the
    two radios' oscillators apart) is the scale's, 0.07 % on Xiegu's base (about 28 Hz 40 kHz out), within 10 Hz on
    gcfix3 and later"""
    snd, lis = ctx.snd, ctx.lis
    base = base_of(lis.host)
    g = gcfix_of(base)
    prove(ctx, F20)
    listen(ctx, F20, 3)
    time.sleep(1.0)
    fr = last_frame(lis)
    if not fr:
        raise bench.Refused(f"{lis.name} sends no spectrum")
    cen, wid, per0 = fr[1], fr[2], fr[2] / len(fr[3])
    d = min(0.42 * wid, 14349500 - cen)
    if d < 10000:
        raise NotRun(f"{lis.name}'s spectrum {wid} Hz wide at {cen}: no room for a tone near its edge on 20 m")
    # both tones at the same place within a bin, whole bins apart: the parabola's own bias the same for both, gone in
    # their difference
    off_c = round(2000 / per0) * per0
    off_e = off_c + int((d - off_c) // per0) * per0
    got = []
    for off in (off_c, off_e):
        rf = cen + int(round(off))
        with bench.apart({"freq": abs((rf - 1000) - int(lis.state.get("freq") or F20))},
                         f"codec-clock: the sender's tone at {rf / 1e6:.4f} MHz, across the listener's span"):
            sender(ctx, rf - 1000, 3, FIRST, apart=("freq",))     # the tone placed across the listener's span
            time.sleep(1.3)
            q = level(lis, rf, time.time() - 1.0, time.time(), half=300)
            ctx.banner(f"SPECTRUM SCALE: {snd.name.upper()} SENDS A TONE 0.1 W AT {rf / 1e6:.4f} MHZ, "
                       f"{lis.name.upper()}'S SPECTRUM PLACES IT")
            o = tci_over(ctx, plan=[[0, 0.3], [1000, 3.0]], mode=3, what=f"codec clock {off:.0f}", ignore=("freq",))
            pk, pdb, per = spectrum_peak_hz(lis, rf, o["on"] + 0.8, min(o["sent"][1], o["off"]) - 0.2, span=400)
            got.append((off, rf, pk, per, (rise(q, pdb) or 0) >= 15))
    (o_c, rf_c, pk_c, per, h_c), (o_e, rf_e, pk_e, _, h_e) = got
    if pk_c is None or pk_e is None or not (h_c and h_e):
        return False, f"the tone not found on {lis.name}'s spectrum (centre {pk_c}, edge {pk_e}, heard {h_c}/{h_e})"
    e_c, e_e = pk_c - rf_c, pk_e - rf_e
    scale = e_e - e_c
    ppm = scale / (o_e - o_c) * 1e6
    says = (f"{lis.name}'s base {base or 'not read'}; bins of {fmt(per, 1)} Hz; the tone {o_c:.0f} Hz from its centre "
            f"read {e_c:+.1f} Hz off, {o_e:.0f} Hz out {e_e:+.1f} Hz: the scale {scale:+.1f} Hz ({ppm:+.0f} ppm; "
            f"Xiegu's base about +700)")
    if g < 3:
        return None, says + ": not gcfix3 or later, the fix is not on this base"
    return abs(scale) <= 10.0, says


def t_codec_clock(run):
    pair_test(run, "codec-clock", body_codec_clock, covers=["tx-x6100-gcfix-codec-clock"],
              pairs=one_way(run, "x6200", "x6100"))


BURSTS = "0.05,0.1,0.2,0.4,0.8,1.6,3.2"


def t_power_loop(run):
    """path-tx-power-loop-bursts (tx-x6100-gcfix5-pause-hold from an X6100 on gcfix4/5, at 0.1 and 1 W): tones.py
    --bursts as a child, each way: every burst after its pause within 1 dB of its steady level (overshoot and dip);
    on Xiegu's own X6100 base its loop overshoots by design (gcfix4/5 hold it): a look there"""
    from suite import run_child
    if len(run.names) < 2:
        run.result(BATCH, "power-loop", None, "not run: it needs both radios", covers=["path-tx-power-loop-bursts"])
        return
    run.close()
    run.radios = {}
    snaps, seen, why = sync_both(run)
    oks = []
    try:
        for s, li in both_ways(run):
            covers = ["path-tx-power-loop-bursts"]
            if why:
                run.result(BATCH, f"power-loop {s}->{li}", False, f"refused: the radios are not in sync ({why})",
                           covers=covers, radio=s)
                oks.append(False)
                continue
            x6100 = model(run, s) == "x6100"
            base = base_of(run.host(s)) if x6100 else ""
            g = gcfix_of(base)
            if x6100 and g >= 4:
                covers.append("tx-x6100-gcfix5-pause-hold")
            # the listener's ATT stays off: through 80 dB at 0.1 W the X6100's audio comes at -4 dBFS, the bursts'
            # peaks clipped (tones.py says CLIPPED: said in the result), but with its ATT on its own audio noise
            # hid the tones' onset ("no tone found", 2026-11-23): the noise is after the attenuator
            # a child (its TCI audio read in a process of its own: in orch's, two bursts' windows slipped, 11-24);
            # its set-ups count in the one sync framework through bench's SYNC_DIR
            # the X6100 listener 7.5 dB lower (its RF gain 85, the base's attenuator at 0.50 dB a step as its own
            # Measure found, 11-24): at 100 the X6200's bursts came at -4 dBFS and 7 of 8 clipped; its ATT (15 dB and
            # more) buried their onsets in its own noise (11-23)
            cmd = [sys.executable, "-B", os.path.join(HERE, "tones.py"), s, li, "--bursts", BURSTS, "--tag",
                   "suite-air"] + (["--w", str(POWER_MAX_W)] if x6100 and g >= 4 else []) + (
                ["--rx-rfg", "85"] if model(run, li) == "x6100" else [])
            r = run_child(cmd, 420)
            out = (r.stdout + r.stderr).strip().splitlines()
            with open(os.path.join(run.dir, f"power_loop_{s}_{li}.txt"), "w") as fo:
                fo.write("\n".join(out) + "\n")
            summ = None
            for x in out:
                if " summary: " in x:
                    try:
                        summ = json.loads(x.split(" summary: ", 1)[1])
                    except ValueError:
                        pass
            passes = {k: v for k, v in (summ or {}).items() if isinstance(v, dict) and "overshoot_max_db" in v}
            said = "; ".join(f"{k}: overshoot {v['overshoot_max_db']} dB, dip {v['dip_min_db']} dB, "
                             f"{v['bursts_found']}/{v['bursts']} bursts" for k, v in passes.items())
            good = (r.returncode == 0 and bool(passes)
                    and all(v["overshoot_max_db"] is not None and v["overshoot_max_db"] <= 1.0
                            and v["dip_min_db"] is not None and v["dip_min_db"] >= -1.0
                            and v["bursts_found"] == v["bursts"] for v in passes.values()))
            clip = sum(1 for x in out if " after " in x and "CLIPPED" in x)
            if clip:
                said += f" ({clip} bursts CLIPPED in {li}'s audio: their overshoot may read low)"
            refused = next((x.split(" ", 1)[-1] for x in out if "REFUSED" in x or "STOP" in x), "")
            ok = good
            if not good and x6100 and g < 4 and passes and r.returncode == 0:
                # the pause-hold is gcfix4/5's: on Xiegu's own base its loop overshoots by design, measured and said
                ok, said = None, (f"not this base's feature: the pause-hold is gcfix4/5's, this X6100's base is "
                                  f"{base or 'not read'} (Xiegu's loop, which overshoots by design); measured: " + said)
            elif not good and not x6100 and passes and r.returncode == 0:
                # the X6200's base is Xiegu's alone (closed firmware, no gcfix): its loop measured and said, as the
                # factory X6100's; under 1 W the GUI's level does not move (tx_level.c coarse), so it is the base's
                # (1.6-1.7 dB over after a pause, 11-23 and 11-24, its peaks clipped in the X6100's audio: more)
                ok, said = None, ("not this base's feature: the X6200's base is Xiegu's own loop (no gcfix for it), "
                                  "the GUI's level fixed under 1 W; measured: " + said)
            run.result(BATCH, f"power-loop {s}->{li}", ok, (said or refused or (out[-1] if out else "no output"))
                       + f" (rc {r.returncode})", covers=covers, radio=s)
            oks.append(ok)
    finally:
        unsync_both(run, snaps)
    if len(oks) == 2:
        pair = False if False in oks else None if None in oks else True
        run.result(BATCH, "power-loop 1:1", pair, f"{oks}", covers=["path-tx-power-loop-bursts"])


POWER_BANDS = (("40 m", 7150000), ("20 m", 14200000), ("10 m", 29600000), ("6 m", 52525000))


def body_power_steps(ctx):
    """tx-x6100-gcfix5-low-power: the X6100 in FM at 0.1 W then 1 W (a power feature: 1 W at most) on 40, 20, 10 and
    6 m: its meter 0.2 W at most (whole tenths, cut off) and 0.8-1.2 W, and on the X6200 10 dB between them (8 to 12,
    its spectrum or its
    S-meter). Judged on gcfix5; on Xiegu's own base (0.9 W out at 0.1 W on 6 m) a look"""
    snd, lis = ctx.snd, ctx.lis
    base = base_of(snd.host)
    g = gcfix_of(base)
    prove(ctx, F20)
    said, good = [], True
    for name, f in POWER_BANDS:
        listen(ctx, f, 7)
        got = {}
        for w in (FIRST, POWER_MAX_W):
            sender(ctx, f, 7, w)
            ctx.banner(f"POWER STEPS {name.upper()}: {snd.name.upper()} SENDS FM {w} W, {lis.name.upper()} MEASURES")
            time.sleep(1.2)
            q = level(lis, f, time.time() - 1.0, time.time(), half=3000)
            o = ptt_over(ctx, 7, 3.0, w=w, what=f"power step {w} W")
            t0, t1 = o["on"] + 1.0, o["released"] - 0.2
            got[w] = (med_w(snd, t0, t1), level(lis, f, t0, t1, half=3000), med([v for _, v in meters(lis, t0, t1)]),
                      q, heard_ok(o))
        (w1, s1, m1, q1, h1), (w2, s2, m2, q2, h2) = got[FIRST], got[POWER_MAX_W]
        ds = None if s1 is None or s2 is None else s2 - s1
        dm = None if m1 is None or m2 is None else m2 - m1
        this = (w1 is not None and w1 <= 0.2 and w2 is not None and 0.8 <= w2 <= 1.2 and h1 and h2
                and any(d is not None and 8 <= d <= 12 for d in (ds, dm)))
        good &= this
        said.append(f"{name}: {fmt(w1, 2)} W and {fmt(w2, 2)} W by its meter; on {lis.name} {fmt(ds)} dB apart (its "
                    f"spectrum), {fmt(dm)} dB (its S-meter)" + ("" if this else " (NOT as asked)"))
    says = f"{snd.name}'s base {base or 'not read'}: " + "; ".join(said)
    if g < 5:
        return (None if not good else True), says + " (not gcfix5: the feature is that base's)"
    return good, says


def t_power_steps(run):
    pair_test(run, "power-steps", body_power_steps, covers=["tx-x6100-gcfix5-low-power"],
              pairs=one_way(run, "x6100", "x6200"))


FT8_W = 0.5                 # the digital level's features are the power it reaches: 1 W at most


def body_ft8_tx(ctx):
    """FT8 CQs at 0.5 W from a radio with a test call (the listener only hears, its FT8 app shut: nothing decoded,
    nothing reported): on the air in USB-D and USB again after (rigctl); the TX meter through each over: the level
    finds the setting with the ALC about 1 (X6100: 3 at most) or, on the X6200, by the power alone; the GUI's
    'tx_player: level' lines"""
    from suite_more import rig
    snd, lis = ctx.snd, ctx.lis
    call = callsign(snd.host)
    if not bench.TEST_CALL.match(call):
        raise NotRun(f"{snd.name}'s callsign is not a test call (Q<digit>TST): bench.py testcall {snd.name} Q1TST "
                     f"first "
                     f"(it restarts the GUI, which this batch never does)")
    x6200 = str(snd.state.get("fp", "")).startswith("x6200")
    n_overs = 6 if x6200 else 4
    dial = bench.FT8_DIAL["20m"]
    prove(ctx, dial)
    listen(ctx, dial, 2)
    sender(ctx, dial, 2, FIRST)
    ft8_open(ctx, snd, dial)
    mirror(ctx)
    bench.ensure(snd, {"pwr": FT8_W})
    bench.radio_tx_gate(snd, ctx.c, FT8_W)
    if snd.state.get("rxo") == 1:
        raise bench.Refused(f"{snd.name}: held receive only with FT8 open")
    mark = gui_log_mark(snd.host)
    ctx.banner(f"FT8 TX LEVEL: {snd.name.upper()} CALLS CQ {n_overs} TIMES AT {FT8_W} W, {lis.name.upper()} LISTENS")
    watch = make_watch(ctx, FT8_W, ("mode", "flo", "fhi"), "FT8 CQ")    # it sends in USB-D
    overs, rig_mid, restarts = [], None, 0
    try:
        bench.tx_log({"who": snd.name, "f": dial, "w": snd.pwr(), "what": f"air_a FT8 CQ x{n_overs}"})
        snd.send("ft8 cq")
        while len(overs) < n_overs and not watch.why:
            try:
                snd.wait("its next CQ", lambda: snd.tx or bool(watch.why), 35)
            except bench.Refused:
                if restarts < 2 and not snd.ft8.get("cq"):
                    restarts += 1
                    snd.send("ft8 cq")
                    continue
                raise
            if watch.why:
                break
            on = snd.tx_since or time.time()
            if rig_mid is None:             # asked on the air: an over begun late in its slot may end first
                time.sleep(2.5)
                try:
                    q = rig(snd.host, ["m"])[0]
                except OSError as e:
                    q = f"no answer ({e})"
                if snd.tx:
                    rig_mid = q
            snd.wait("the end of its CQ", lambda: not snd.tx or bool(watch.why), bench.MAX_KEY_S + 2)
            off = time.time()
            tf = [x for x in tx_frames(snd, on + 3.0, off - 0.3)]
            time.sleep(0.5)
            lines = gui_log_since(snd.host, mark)       # this over's own lines: the level it left for the next
            mark += len(lines)
            step = next((tuple(float(v) for v in m_.groups()) for m_ in
                         (re.search(r"tx_player: level (-?[\d.]+) -> (-?[\d.]+) dB", x) for x in lines) if m_), None)
            overs.append({"w": med([x[1] for x in tf]), "alc": med([x[3] for x in tf]), "n": len(tf),
                          "deaf": watch.unheard(snd) is not None, "step": step,
                          "zero": next((x for x in lines if "tx_player: 0 W" in x), None)})
    finally:
        snd.send("ft8 stop")
        off_air(snd, 16)
        watch.close()
    if watch.why:
        raise bench.Refused(f"stopped: {watch.why}")
    time.sleep(2.0)
    try:
        rig_after = rig(snd.host, ["m"])[0]
    except OSError as e:
        rig_after = f"no answer ({e})"
    mode_after = int(snd.state.get("mode", -1))
    full = [o for o in overs if o["n"] >= 5]       # an over begun late in its slot (the first) is not judged
    ws = [o["w"] for o in full]
    last = full[-1] if full else {}
    heard = bool(overs) and not any(o["deaf"] for o in overs)
    near = last.get("w") is not None and 0.3 <= last["w"] <= 0.7
    data_tx = (rig_mid or "").split()[:1] in (["PKTUSB"], ["PKTLSB"])
    # FT8 is in USB-D for as long as the app is open, between its overs too (the manual: "FT8, JS8, WSPR, PSK, Olivia
    # and JTTY were in USB-D already"; SSTV, RTTY and WeFax are the ones back in LSB or USB after each over); the
    # mode as found once the app closes is finish()'s check
    data_back = (rig_after or "").split()[:1] == ["PKTUSB"] and mode_after == 3
    ctx.row("ft8-data-mode", data_tx and data_back and heard,
            f"rigctl's mode on the air {(rig_mid or '').split()[:1]}, after it {(rig_after or '').split()[:1]}, the "
            f"state's mode after {mode_after} (FT8 keeps USB-D, 3, while it is open)", (), ["digi-tx-data-mode"])
    alc_ok = x6200 or (last.get("alc") is not None and last["alc"] <= 3.0)
    ctx.row("ft8-alc", alc_ok and near and heard,
            f"the last CQ: {fmt(last.get('w'), 2)} W at {FT8_W} W set, ALC {fmt(last.get('alc'))}"
            + (" (the X6200's ALC changes nothing: not judged)" if x6200 else " (about 1, 3 at most)"), (),
            ["alc-digital-reading"])
    zero = [o["zero"] for o in overs if o["zero"]]
    ctx.row("ft8-level", near and heard and len(full) >= 2 and not zero,
            f"{len(overs)} CQs ({len(full)} whole): {', '.join(fmt(w, 2) for w in ws)} W ({FT8_W} W set); the level "
            f"each left (the GUI's log): {[o['step'] for o in overs]}"
            + (f"; '0 W the whole TX' said: {zero[-1][-90:]!r}" if zero else ""), ["digi-tx-level-auto"])
    if x6200:
        conv = len(ws) >= 2 and all(w is not None and 0.35 <= w <= 0.65 for w in ws[-2:])
        # reached as tx_level.c holds it: within the meter's 0.1 W step of the setting (power_hold)
        reached = next((i for i, o in enumerate(overs) if o["w"] is not None and o["w"] >= FT8_W - 0.1 - 0.005), None)
        creep = [round(o["step"][1] - o["step"][0], 2) for o in overs[reached + 1:] if o["step"]] \
            if reached is not None else []
        ctx.row("ft8-level-x6200", conv and heard and reached is not None and not any(c > 0.5 for c in creep),
                f"the X6200's last two whole CQs {', '.join(fmt(w, 2) for w in ws[-2:])} W ({FT8_W} W set); the "
                f"power reached in CQ {None if reached is None else reached + 1}; the level's changes after it: "
                f"{creep or 'none'}", ["digi-tx-level-x6200-power"])
    return ctx.rows


def t_ft8_tx(run):
    pair_test(run, "ft8-tx", body_ft8_tx, covers=["digi-tx-level-auto", "digi-tx-level-x6200-power"],
              partial=["digi-tx-data-mode", "alc-digital-reading"])


TESTS = (("smeter-front-end", t_smeter_front_end), ("readings-zero", t_readings_zero), ("band-edges", t_band_edges),
         ("keys-with-ptt", t_keys_with_ptt), ("armed-ptt-a", t_armed_ptt_a), ("smeter-am", t_smeter_am),
         ("comp", t_comp), ("split", t_split), ("split-cw", t_split_cw), ("cw-alc", t_cw_alc),
         ("qsy-out", t_qsy_out), ("tune", t_tune), ("tune-power", t_tune_power), ("tune-app", t_tune_app),
         ("tune-key", t_tune_key), ("text-log", t_text_log), ("codec-clock", t_codec_clock),
         ("power-loop", t_power_loop), ("power-steps", t_power_steps), ("ft8-tx", t_ft8_tx))


def batch(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE air_a: {name} (over the cable, 0.1 W through 80 dB: one sends, one listens)")
            try:
                fn(run)
            except bench.Refused as e:
                run.result(BATCH, name, False, f"refused: {e}")
