#!/usr/bin/env python3
"""The bench harness: automated tests through attenuators, between the HackRF on this computer and a radio
("hackrf" profile, the radio receive only), or between two radios keying into each other ("radios" profile). Every
rule of README.md is enforced here: nothing is sent unless every gate passes, and every transmission is watched and
logged. The radios are worked through their browser remote (src/remote_server.c) with a session of the harness's
own; SSH (root) only for provisioning, the test callsigns and cleaning the logs.

  bench.py provision NAME rx|tx        a remote session for the harness on radio NAME (the GUI restarts once)
  bench.py status [NAME...]            frequency, mode, power, TX, app and S-meter of each radio
  bench.py chain-ok PROFILE "WORDS"    the owner's word that the cable of PROFILE is in place (12 hours)
  bench.py power-ok radios W "WORDS"   the owner's word for W over 5 W, up to each pad's own rating (12 hours)
  bench.py preflight PROFILE           every check before a transmission; nothing sent
  bench.py stop                        the emergency stop: the HackRF ended, every radio unkeyed, apps' TX stopped
  bench.py hackrf-ft8 [--band 20m] [--x 30] [--text "CQ Q9TST FN00"] [--audio 1500]
  bench.py hackrf-iq FILE --at HZ --offset HZ [--fs 2000000] [--mode 7] [--x 30] [--during "CMD"]
                                       an I/Q file into the radio tuned to HZ (7 NFM), CMD run beside it
  bench.py radios-ft8 TX RX [--band 20m]          (at 0.1 W, the first probe's power, throughout)
  bench.py radios-walkie A B [--bands 20m,40m] [--modes am,fm]
                                       G (Walkie) on both: each band's channel, each mode, A sends and B hears,
                                       then B sends and A hears (0.1 W)
  bench.py testcall NAME CALL|restore     the radio's callsign for the tests (Q<digit>TST), or back
  bench.py cleanlog NAME               the test callsigns' QSOs out of the radio's logs

The configuration is ~/.config/x6x00-bench/bench.json (bench.example.json says what goes in it); the tokens, the
owner's confirmations, the TX log and the runs are kept beside it, never in the repository.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import base64
import collections
import contextlib
import json
import math
import os
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import threading
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from remote_ws import Remote  # noqa: E402

STATE = os.path.expanduser("~/.config/x6x00-bench")
CONF = os.path.join(STATE, "bench.json")

# ---- The rules' numbers (README.md): the configuration can lower them, never raise them ----
HARD_MAX_W = 5.0            # no radio test above this, whatever the configuration says
FIRST_PROBE_W = 0.1         # a session's first transmission, whatever the test asks
HACKRF_MAX_X = 40           # the owner's approved maximum TX gain; the amp (-a 1) is never used
HACKRF_MIN_DB = 70          # the HackRF only into at least this much attenuation
RADIOS_MIN_DB = 60          # two radios only through at least this much
CHAIN_VALID_S = 12 * 3600   # the owner's word on the cable holds this long
MAX_KEY_S = 16              # longer than an FT8 transmission: a radio still on the air after it is stopped
MAX_SWR = 2.0               # higher on a radio's TX meter: the chain is not the load it should be
TOO_STRONG_DB = 20          # a receiver hearing this much more than the chain lets through: pads missing
STALE_S = 6.0               # a radio silent this long (its meters come every 100 ms; WiFi has held them back 2-3 s,
                            # 2026-11-19): the Watch cannot see it, stopped
STALE_HELD_S = 15.0         # ... unless it cannot go on the air (stale_limit): its stall said, the over goes on this long
                            # (the X6200's WiFi held its link 4 s under WeFax's Auto as the X6100 sent, 2026-11-24)
RX_APPS = ("WeFax", "NavTex", "Time Signal", "Airspace")   # a listener's app that keys only when told to (WeFax's TX
                            # sends a picture named to it; the others have none): nothing of an over tells it to
DEAF_DB = 15                # the listener's spectrum at the sender's frequency over its quiet: under it, an over unheard
DEAF_MIN_W = 0.05           # the sender's meter at least this for a pass of the Watch to count (SSB with no audio: 0)
DEAF_MIN_POWERED = 10       # passes (50 ms each) with power before an over can be called unheard
# Over HARD_MAX_W only with the owner's recorded word (power-ok; the owner, 2026-11-20: "Yes, run 10 W"): up to the
# pads' own rating (not twice it), never over FULL_RATING_MAX_W, each over at most FULL_RATING_KEY_S on the air and
# the radio then off at least as long as it was on before its next (the pad cools)
FULL_RATING_MAX_W = 10.0
FULL_RATING_KEY_S = 10
TEST_CALL = re.compile(r"^Q[0-9]TST$")

FT8_DIAL = {"40m": 7074000, "30m": 10136000, "20m": 14074000, "17m": 18100000, "15m": 21074000,
            "12m": 24915000, "10m": 28074000, "6m": 50313000}
HAM_HF = [(1800000, 2000000), (3500000, 4000000), (5330500, 5406500), (7000000, 7300000), (10100000, 10150000),
          (14000000, 14350000), (18068000, 18168000), (21000000, 21450000), (24890000, 24990000),
          (28000000, 29700000), (50000000, 54000000)]


class Refused(Exception):
    """A gate that did not pass: nothing was sent"""


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def load_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, doc, mode=0o600):
    os.makedirs(STATE, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=1)
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def conf():
    c = load_json(CONF, None)
    if not c:
        raise Refused(f"no configuration: copy tools/bench/bench.example.json to {CONF} and fill it in")
    lim = c.setdefault("limits", {})
    lim["radio_w"] = min(float(lim.get("radio_w", 1.0)), HARD_MAX_W)
    full = power_ok()
    if full:
        lim["radio_w"] = full
    lim["max_key_s"] = min(float(lim.get("max_key_s", MAX_KEY_S)), MAX_KEY_S)
    lim["hackrf_x"] = min(int(lim.get("hackrf_x", 30)), HACKRF_MAX_X)
    return c


def tx_log(entry):
    os.makedirs(STATE, exist_ok=True)
    entry = dict(entry, at=time.strftime("%Y-%m-%dT%H:%M:%S%z"))
    with open(os.path.join(STATE, "tx_log.jsonl"), "a") as f:
        f.write(json.dumps(entry) + "\n")


def ssh(host, cmd, timeout=60, input=None):
    """cmd on the radio; input: its standard input. -4: the radios' .local names have no IPv6 answer, each lookup of
    one cost about 5 s (2026-11-23)"""
    r = subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                       capture_output=True, text=True, timeout=timeout, input=input)
    if r.returncode:
        raise Refused(f"{host}: {cmd.split()[0]}: {(r.stderr or r.stdout).strip()[-200:]}")
    return r.stdout


# ---- A radio through its remote ----

class Radio:
    def __init__(self, name, rc, want_tx):
        self.name, self.host = name, rc["host"]
        self.simulated = "port" in rc       # sim_test.py's radios: another port, no SSH, no battery log
        tok = load_json(os.path.join(STATE, "tokens.json"), {}).get(name, {}).get("tx" if want_tx else "rx")
        if not tok:
            raise Refused(f"{name}: no {'tx' if want_tx else 'rx'} session: bench.py provision {name} "
                          f"{'tx' if want_tx else 'rx'}")
        self._rc, self._tok, self._want_tx = rc, tok, want_tx     # for reconnect()
        self.r = Remote(self.host, int(rc.get("port", 50002)))     # another port: the simulated radios' (sim_test.py)
        self.state, self.meter, self.txm, self.ft8, self.apps = {}, None, None, {}, None
        # reentrant: wait() holds it while its condition runs, and a condition may take it again (orch_cable's
        # tx_spans in a wait: run 29 hung 1 h 40 min on a plain Lock, 2026-11-25)
        self.rows, self.errs, self.lock = [], [], threading.RLock()
        self.tx, self.tx_since, self.ctl_mine, self.role = False, None, False, None
        self.ctl_who = None         # who has control, as the remote last said ("ctl WHO MINE"): none, local, tci, browser NAME
        self.tx_last = None         # (on, off) of the last over: the pad's rest above HARD_MAX_W
        self.tx_hist = []           # [(when, on)]: every change of tx, the session's whole
        self.tx_off_at = 0.0       # when it last left TX (the radios' gate: one on the air at a time)
        self.last_msg = time.time()
        self.freqs = set()          # every dial it reported since snapshot(): the bands a test tuned it into
        self.cw = ""                # the CW decoder's text since the session began (the remote's "cw" lines)
        self.msgs = []              # [(when, text)]: what the radio put in its message box (the remote's "msg" lines)
        self.cfgv = {}              # {name: value}: the settings the remote's "cfg? NAME" answered
        self.js8_hist = []          # [(when, message)]: every JS8 message line ("js8r"), the session's whole
        self.lines = collections.deque(maxlen=600)     # every text the radio sent, with when: the suite's protocol tests
        self.r.send(f"auth {tok}")
        self.r.send("panel 1")
        self.r.send("screen 0")
        threading.Thread(target=self._read, args=(self.r,), daemon=True).start()
        t = time.time()
        while time.time() - t < 8 and (self.role is None or not self.state):
            time.sleep(0.05)
        if self.role is None:
            raise Refused(f"{name}: the remote did not take the harness's session")
        while time.time() - t < 8 and self.role[0] == "view":
            time.sleep(0.05)        # the radio's first word can be the unpaired "view", before it has read the token
        if self.role[0] == "view":
            raise Refused(f"{name}: the harness's session is not paired any more (role view): provision it again")
        self.may_tx = self.role[1] == "1"

    def reconnect(self):
        """A new connection with the same session (a stalled or dropped link: the X6200's WiFi held its messages back
        6 s and more, 2026-11-22), its state read afresh and control taken again. Refused when it does not come"""
        try:
            self.r.close()
        except OSError:
            pass
        self.state, self.role, self.ctl_mine, self.ctl_who = {}, None, False, None
        self.r = Remote(self.host, int(self._rc.get("port", 50002)))
        self.last_msg = time.time()
        self.r.send(f"auth {self._tok}")
        self.r.send("panel 1")
        self.r.send("screen 0")
        threading.Thread(target=self._read, args=(self.r,), daemon=True).start()
        self.wait("its state again", lambda: self.role is not None and self.state and self.role[0] != "view", 10)
        self.take_control()

    def stale(self, secs=3.0):
        """No word from the radio for secs, or its connection gone"""
        return not self.r.alive or time.time() - self.last_msg > secs

    def _read(self, rem):
        while True:
            t = rem.recv()
            if t is None:
                return
            if rem is not self.r:
                return                          # a connection replaced by reconnect(): its reader stops
            k, _, rest = t.partition(" ")
            self.last_msg = time.time()        # anything at all from it: a stall is measured, not guessed
            self.lines.append((self.last_msg, t))
            with self.lock:
                if k == "state":
                    self.state = json.loads(rest)
                    self.freqs.add(int(self.state.get("freq", 0)))
                    self._tx(bool(self.state.get("tx")))
                elif k == "m":
                    self.meter = float(rest)
                elif k == "t":
                    self.txm = [float(v) for v in rest.split()]
                elif k == "tx":
                    self._tx(rest.strip() == "1")
                elif k == "ft8":
                    self.ft8 = json.loads(rest)
                elif k == "apps":
                    self.apps = json.loads(rest)      # [[ACTION, "Name"], ...] (the remote's "apps")
                elif k == "ft8r":
                    self.rows.append(dict(json.loads(rest), at=time.time()))
                elif k == "ctl":
                    self.ctl_mine = rest.rsplit(" ", 1)[-1] == "1"
                    self.ctl_who = rest.rsplit(" ", 1)[0] if " " in rest else rest
                elif k == "role":
                    self.role = rest.split(" ", 2)
                elif k == "err":
                    self.errs.append(rest)
                elif k == "cw":
                    try:
                        self.cw += json.loads(rest)
                    except ValueError:
                        pass
                elif k == "cfg":
                    name, _, val = rest.partition(" ")
                    self.cfgv[name] = val
                elif k == "js8r":
                    try:
                        self.js8_hist.append((self.last_msg, json.loads(rest)))
                        del self.js8_hist[:-2000]
                    except ValueError:
                        pass
                elif k == "msg":
                    try:
                        self.msgs.append((self.last_msg, json.loads(rest)))
                    except ValueError:
                        pass

    def _tx(self, on):
        if on != self.tx:
            self.tx_hist.append((time.time(), on))      # every change, kept (the lines' deque holds about a minute)
            del self.tx_hist[:-4000]
        if on and not self.tx:
            self.tx_since = time.time()
        if not on:
            if self.tx:
                self.tx_off_at = time.time()
                if self.tx_since:
                    self.tx_last = (self.tx_since, self.tx_off_at)
            self.tx_since = None
        self.tx = on

    def send(self, cmd):
        self.r.send(cmd)
        if cmd.startswith(DIAL_CMDS):
            _dial_moved()           # an app opening on its own memory, closing, a raw set: the other DIAL_FOLLOW_S

    def wait(self, what, cond, timeout):
        t = time.time()
        while time.time() - t < timeout:
            with self.lock:
                if cond():
                    return True
            time.sleep(0.05)
        raise Refused(f"{self.name}: {what} did not come within {timeout:.0f} s"
                      + (f" (the radio said: {self.errs[-1]})" if self.errs else ""))

    def take_control(self):
        """Control, taken only when it is free or the bench's own: its sessions (browser bench-tx, bench-rx) or a TCI
        client (the bench's tests' own). The radio's operator (local) or another browser keeps it: Refused, never
        taken from them (the owner's rule: the operator at the radio has the last word, 2026-11-23; cover_a found
        put_back taking it back at every test's end)"""
        if self.ctl_mine:
            return
        try:
            self.wait("who has control", lambda: self.ctl_who is not None, 2)
        except Refused:
            pass                                            # not said yet: free as far as is known
        who = self.ctl_who or "none"
        if who == "local" or (who.startswith("browser ") and not who.startswith("browser bench-")):
            raise Refused(f"{self.name}: control with {who}: not taken (the radio's operator keeps it)")
        self.send("control take")
        self.wait("control", lambda: self.ctl_mine, 10)     # the X6200 took 3-5 s now and then (2026-11-19)

    def pwr(self):
        return float(self.state.get("pwr", 99))

    def stop(self):
        """Everything that may send stopped, as far as this session may"""
        for cmd in ("ft8 stop", "key PTT 0", "tune 0"):
            try:
                self.send(cmd)
            except OSError:
                pass

    def close(self):
        try:
            self.send("control release")
        except OSError:
            pass
        self.r.close()


# ---- Each radio's state for a test: named whole, set, read back, proved, put back ----
# The owner, 2026-11-22: "very often during testing you do things like forget the preamp, forget to sync channels,
# forget you muted an audio source, or had attenuation on ... I need you to audit deeply the test harness and ensure
# this stops happening". So no test sets a radio by hand or takes one as it finds it: it names the whole state it
# needs (rx_profile, tx_profile: PRE, ATT, RF gain and AGC always said), ensure() sets each in order and reads every
# one back, path_proof() shows the listener hears the sender before anything is measured (that day tests ran for
# hours on a listener that heard nothing), the Watch says an over the listener did not hear, and finish() puts each
# radio back as it was, checked, on the Sentry's page A (the owner: "when tests finish they should always leave on
# screen A, regardless of what the state was before the testing").

# The keys "set" takes (src/remote_server.c panel_set), in the order they go: split and the VFO, then the dial and the
# mode (a band change loads the band's ATT, PRE and AGC, so those after it), the receiver's, the rest; LISTEN last
SET_ORDER = ("split", "vfo", "freq", "mode", "filter", "zoom", "att", "pre", "agc", "rfg", "sql", "nb", "nr", "rit",
             "xit", "ifs", "comp",
             "atu", "pwr", "tpwr", "vol", "txio", "txqo", "txig", "txip", "wch", "heard", "kt")
# Kept by the band (or the band's mode): put back only once the dial is on that band again
BAND_KEYS = {"mode", "filter", "zoom", "att", "pre", "agc", "txio", "txqo", "txig", "txip", "ifs"}
# What a test may change and finish() puts back ("heard" is not: the Sentry's page A puts its own LISTEN back)
RESTORED = SET_ORDER[:-1]
# The state the checks read (pstate_read): a GUI without these is older than them, and the harness will not guess
STATE_NEEDS = ("txf", "split", "rit", "xit", "play", "heard", "spk", "moni", "muted", "tcim", "mic", "flo", "fhi",
               "vol")
SET_WAIT_S = 6.0            # a "set" read back within this (sim_test.py's radios answer at once: less there)
PLAY_MODES = {0: "off", 1: "on (the speaker plays the processor's audio)", 2: "voice record", 3: "the hand mic"}


def _value(r, k):
    s = r.state
    if k == "filter":
        return (int(s.get("flo", -1)), int(s.get("fhi", -1)))
    if k == "wch":                      # the Sentry G's own channel (Hz, mode): a GUI without it says none
        return None if s.get("wch") is None else (int(s["wch"]), int(s.get("wchm", -1)))
    if k == "pwr":
        return round(float(s.get("pwr", 99)), 1)
    v = s.get(k)
    return None if v is None else int(v)


def _same(k, have, want):
    if have is None:
        return False
    if k == "pwr":
        return abs(have - want) < 0.05
    if k in ("filter", "wch"):
        return tuple(have) == tuple(want)
    return have == want


def _put(r, k, v):
    if k == "filter":
        r.send(f"set filter {int(v[0])} {int(v[1])}")
    elif k == "wch":
        r.send(f"set wch {int(v[0])} {int(v[1])}")
    elif k == "pwr":
        r.send(f"set pwr {round(v * 10)}")
    elif k == "vfo":
        r.send("set vfo 1")                 # the remote's VFO is a toggle (P_VFO: cfg_vfo_toggle)
    else:
        r.send(f"set {k} {int(v)}")


def state_ready(r):
    """The radio's GUI says every state the checks read: one older than them (2026-11-22) is refused, not guessed"""
    missing = [k for k in STATE_NEEDS if k not in r.state]
    if missing:
        raise Refused(f"{r.name}: its GUI does not report {', '.join(missing)} (older than the bench's state check of "
                      f"2026-11-22): install a newer one first")


# ---- The one framework that keeps the two radios in sync (the owner, 2026-11-24: "EVERY test should use the same
# unified framework to sync the radios or make exceptions for tests so that there is NO deviation") ----
# Every difference between the two radios that a test makes on purpose is declared with apart(), exactly, with its
# reason, and only that passes: bench.Watch (as a radio keys, every receiver setting compared) and orch's sync guard
# (the dial and mode, all the while) read the same declarations. A setting being changed is a transition, not a
# difference: ensure(), open_page() and arm() are one while they run, and a dial changed on one radio leaves the other
# DIAL_FOLLOW_S to follow. sync_pair() sets the two alike at once: orch's both() and ready(), the suite's sync_both
# and pair_align all go through it.

# The bench's own commands that may move one radio's dial or mode outside ensure() (an app opening on its own memory:
# PSK31 on 3.580 MHz, 2026-11-23; an app closing; a raw "set freq"): sent through Radio.send, each leaves the other
# radio DIAL_FOLLOW_S to follow, as ensure() does. Keys are not among them: a test that moves a dial with a key says so
DIAL_CMDS = ("open ", "close", "ft8 open", "ft8 close", "ft8 mode", "js8 open", "js8 close", "wefax open",
             "wefax close", "set freq", "set mode", "set band", "set vfo")
STEPPED = "stepped"         # a rule: the setting is what the test steps and measures (the reason says how)
DIAL_FOLLOW_S = 8.0         # a dial or mode set on one radio: the other set within this (one test step), else a
                            # difference
_sync_lock = threading.RLock()
_declared = []              # [{"spec": {key: rule}, "reason": str, "at": t}]
_transitions = 0
_dial_until = 0.0
_dial_apart = False         # the radios seen apart since the follow window opened (dial_settled)
# A bench program run as a child of another (suite.run_child: tones.py --bursts, bench.py radios-walkie) sets the radios
# in its own process: its transitions, follow window and declarations go to SYNC_DIR/<pid>.json, and the parent's
# changing() and declared() read them too, so the one framework covers it (orch's guard counted a child's set-ups as
# violations, air power-loop 2026-11-24). Every bench process owns the directory for its own children
if not os.environ.get("BENCH_SYNC_DIR"):
    os.environ["BENCH_SYNC_DIR"] = tempfile.mkdtemp(prefix="bench-sync-")
    os.environ["BENCH_SYNC_OWNER"] = str(os.getpid())
SYNC_DIR = os.environ["BENCH_SYNC_DIR"]
_SYNC_CHILD = os.environ.get("BENCH_SYNC_OWNER") != str(os.getpid())
_others_cache = {}          # file -> (mtime, doc)


def _enc_rule(rule):
    if isinstance(rule, tuple):
        return {"pair": list(rule)}
    if isinstance(rule, list):
        return {"pairs": [list(x) for x in rule]}
    return rule


def _dec_rule(rule):
    if isinstance(rule, dict) and "pair" in rule:
        return tuple(rule["pair"])
    if isinstance(rule, dict) and "pairs" in rule:
        return [tuple(x) for x in rule["pairs"]]
    return rule


def _publish():
    """A child's sync state for its parent (under _sync_lock)"""
    if not _SYNC_CHILD:
        return
    doc = {"pid": os.getpid(), "transitions": _transitions, "dial_until": _dial_until,
           "declared": [{"spec": {k: _enc_rule(v) for k, v in d["spec"].items()}, "reason": d["reason"]}
                        for d in _declared]}
    path = os.path.join(SYNC_DIR, f"{os.getpid()}.json")
    try:
        with open(path + ".tmp", "w") as fh:
            json.dump(doc, fh)
        os.replace(path + ".tmp", path)
    except OSError:
        pass


def _unpublish():
    if _SYNC_CHILD:
        try:
            os.remove(os.path.join(SYNC_DIR, f"{os.getpid()}.json"))
        except OSError:
            pass


import atexit                                       # noqa: E402
if _SYNC_CHILD:
    atexit.register(_unpublish)
else:                                               # the owner: its children's directory gone with it
    atexit.register(lambda: __import__("shutil").rmtree(SYNC_DIR, ignore_errors=True))


def _others():
    """The sync state of the bench programs this one runs as children (those still alive)"""
    out = []
    try:
        names = [n for n in os.listdir(SYNC_DIR) if n.endswith(".json") and n[:-5] != str(os.getpid())]
    except OSError:
        return out
    for n in names:
        path = os.path.join(SYNC_DIR, n)
        try:
            os.kill(int(n[:-5]), 0)
            mt = os.stat(path).st_mtime
            hit = _others_cache.get(path)
            if not hit or hit[0] != mt:
                with open(path) as fh:
                    d = json.load(fh)
                d["declared"] = [{"spec": {k: _dec_rule(v) for k, v in x["spec"].items()}, "reason": x["reason"]}
                                 for x in d.get("declared", [])]
                hit = _others_cache[path] = (mt, d)
            out.append(hit[1])
        except (OSError, ValueError, ProcessLookupError):
            continue
    return out


class apart:
    """with bench.apart({"freq": 1500}, "the listener 1500 Hz below, the image's place"): the radios that far apart
    on purpose while the block runs. A rule: an int, the two exactly that far apart (|a - b|); a tuple of two values,
    the two reading them (in either order); a list of such tuples (one for each phase of the test); STEPPED, a setting
    the test sets or steps itself as what it measures (the reason says how). "filter" covers the filter's edges (flo,
    fhi)"""

    def __init__(self, spec, reason):
        if not spec or not reason:
            raise ValueError("apart() needs what is apart and why")
        self.d = {"spec": dict(spec), "reason": str(reason), "at": time.time()}

    def __enter__(self):
        with _sync_lock:
            _declared.append(self.d)
            _publish()
        return self

    def __exit__(self, *a):
        with _sync_lock:
            if self.d in _declared:
                _declared.remove(self.d)
            _publish()
        return False


def _rule_ok(rule, a, b):
    if rule == STEPPED:
        return True
    if isinstance(rule, bool):
        return False
    if isinstance(rule, int):
        try:
            return abs(int(a) - int(b)) == rule
        except (TypeError, ValueError):
            return False
    if isinstance(rule, tuple) and len(rule) == 2:
        return (a, b) == rule or (b, a) == rule
    if isinstance(rule, list):                  # several pairs, one for each phase of the test
        return any(_rule_ok(tuple(x), a, b) for x in rule)
    return False


def declared(key, a, b):
    """The difference a/b of key is one a running test declared (apart)"""
    with _sync_lock:
        specs = [d["spec"] for d in _declared]
    specs += [d["spec"] for o in _others() for d in o["declared"]]     # a child's (a bench program it runs)
    for spec in specs:
        if key in spec and _rule_ok(spec[key], a, b):
            return True
        if key in ("flo", "fhi") and spec.get("filter") == STEPPED:
            return True
    return False


def undeclare(reason_prefix):
    """Every declaration whose reason starts with reason_prefix ended (a test's own, left by a refusal)"""
    with _sync_lock:
        _declared[:] = [d for d in _declared if not d["reason"].startswith(reason_prefix)]
        _publish()


def declarations():
    """What is declared apart now: [(spec, reason)] (orch's console says it)"""
    with _sync_lock:
        own = [(dict(d["spec"]), d["reason"]) for d in _declared]
    return own + [(dict(d["spec"]), f"{d['reason']} (a child, pid {o['pid']})") for o in _others()
                  for d in o["declared"]]


@contextlib.contextmanager
def transition():
    """A change under way (one or both radios being set): no difference counted meanwhile"""
    global _transitions
    with _sync_lock:
        _transitions += 1
        _publish()
    try:
        yield
    finally:
        with _sync_lock:
            _transitions -= 1
            _publish()


def changing():
    """A change under way, or a dial set on one radio still within DIAL_FOLLOW_S for the other to follow"""
    now = time.time()
    with _sync_lock:
        if _transitions > 0 or now < _dial_until:
            return True
    return any(o.get("transitions", 0) > 0 or now < o.get("dial_until", 0) for o in _others())   # a child's


def _dial_moved():
    global _dial_until, _dial_apart
    with _sync_lock:
        _dial_until = time.time() + DIAL_FOLLOW_S
        _dial_apart = False
        _publish()


def dial_apart():
    """The radios seen apart within the window (orch's guard): once alike again it may close"""
    global _dial_apart
    with _sync_lock:
        if time.time() < _dial_until:
            _dial_apart = True


def dial_settled(force=False):
    """The two radios are alike again: the window for the other to follow closed. By the guard only once it has seen
    them apart within it: an app asked to open has not tuned yet when the two still read alike, and closing then let
    its tune a moment later count as a violation (air tune-app, 2026-11-24). sync_pair closes it always (force)"""
    global _dial_until
    with _sync_lock:
        if force or _dial_apart:
            _dial_until = 0.0
            _publish()


def sync_pair(radios, want, checks=None):
    """want set on every radio at once, each read back (ensure), then compared: any key of want the radios read
    differently, unless declared apart, Refused. The one way the radios are set alike (orch both/ready, the suite's
    sync_both, pair_align). Returns the first radio's values"""
    errs = {}

    def one(r):
        try:
            ensure(r, want, checks)
        except Refused as e:
            errs[r.name] = str(e)
    with transition():
        th = [threading.Thread(target=one, args=(r,)) for r in radios]
        [t.start() for t in th]
        [t.join(60) for t in th]
    if errs:
        raise Refused("; ".join(errs.values()))
    first = radios[0]
    diff = {k: [_value(r, k) for r in radios] for k in want
            if len({repr(_value(r, k)) for r in radios}) > 1 and not (len(radios) == 2 and declared(
                k, _value(radios[0], k), _value(radios[1], k)))}
    if diff:
        raise Refused("the radios read differently after the set: " + ", ".join(
            f"{k} {'/'.join(str(v) for v in vals)}" for k, vals in diff.items()))
    dial_settled(force=True)
    return {k: _value(first, k) for k in want}


def ensure(r, want, checks=None):
    """want ({key: value}; "filter": (lo, hi), "pwr": W) set on r in SET_ORDER, each read back before the next; then
    all of them again at once (a band change must not have moved one set before it) and checks ({key: (test, words)},
    the state "set" does not take) held. Anything not as asked: Refused, naming it and what the radio reads. A
    transition while it runs; a dial or mode moved leaves the other radio DIAL_FOLLOW_S to follow"""
    with transition():
        got = _ensure(r, want, checks)
    if "freq" in want or "mode" in want:
        _dial_moved()
    return got


def _ensure(r, want, checks=None):
    state_ready(r)
    for k in SET_ORDER:
        if k not in want:
            continue
        v = want[k]
        if k == "heard" and int(r.state.get("heard", -1)) < 0:
            continue                        # no Sentry open: nothing of its on the speaker (play is checked below)
        for attempt in range(1 if k == "vfo" else 2):   # a toggle is never sent twice
            if _same(k, _value(r, k), v):
                break
            _put(r, k, v)
            try:
                r.wait(f"{k} {v}", lambda k=k, v=v: _same(k, _value(r, k), v), SET_WAIT_S)
                break
            except Refused:
                pass
        if not _same(k, _value(r, k), v):
            raise Refused(f"{r.name}: {k} {v} not taken (it reads {_value(r, k)})"
                          + (f"; the radio said: {r.errs[-1]}" if r.errs else ""))
    time.sleep(0.4)
    wrong = {k: (_value(r, k), v) for k, v in want.items()
             if not (k == "heard" and int(r.state.get("heard", -1)) < 0) and not _same(k, _value(r, k), v)}
    if wrong:
        raise Refused(f"{r.name}: moved again after being set: " + ", ".join(f"{k} {h} (asked {w})" for k, (h, w) in
                                                                             wrong.items()))
    for k, (test, words) in (checks or {}).items():
        if not test(r.state.get(k)):
            raise Refused(f"{r.name}: {words} ({k} {r.state.get(k)})")
    return {k: _value(r, k) for k in want}


def rx_profile(f, mode, pre, att, rfg=100, agc=0, sql=0, filt=None):
    """A listener for a test: its dial on f in mode, and nothing between the chain and its audio but what is named here.
    PRE and ATT have no default: each test says them. No split, RIT, NB or NR; the squelch open; its own receiver on
    the speaker, never an SDR (the base in play mode sends the processor no receiver audio: 2026-11-22, tones.py got
    nothing from an X6200 whose B played its stick); no TCI client's over muting it"""
    want = {"split": 0, "vfo": 0, "freq": f, "mode": mode, "att": att, "pre": pre, "agc": agc, "rfg": rfg, "sql": sql,
            "nb": 0, "nr": 0, "rit": 0, "heard": 0}
    if filt:
        want["filter"] = filt
    checks = {"play": (lambda v: v == 0, "its base is in play mode: its receiver's audio does not reach the processor"),
              "tcim": (lambda v: v == 0, "a TCI client's over has it muted")}
    return want, checks


def tx_profile(f, mode, w, mic=False):
    """A sender for a test: on f in mode at w, sending where it is tuned (no split, XIT or TX shift: txf, the frequency
    the radio says it transmits on, must be f), the ATU and TUNE off. mic: the over is the mic's, so the base must not
    be in play mode (it has both mics at 0 there: 2026-11-22, a PTT over while B played the stick went out silent)"""
    want = {"split": 0, "vfo": 0, "freq": f, "mode": mode, "xit": 0, "atu": 0, "pwr": w}
    checks = {"txf": (lambda v: v == f, f"it would transmit off its dial (split, XIT or a TX shift), not on {f} Hz"),
              "tune": (lambda v: not v, "TUNE for an external tuner is on")}
    if mic:
        checks["play"] = (lambda v: v == 0, "its base is in play mode: both mics at 0, a PTT over goes out silent")
    return want, checks


BAND_MEMS = ("vfoa_freq", "vfoa_mode", "vfoa_att", "vfoa_pre", "vfoa_agc",
             # each band's TX calibration (txcal.py's own: its search starts from these, puts them back on a failure)
             "tx_i_offset", "tx_q_offset", "tx_iq_gain", "tx_iq_phase")


def band_values(bm, f):
    """The memories of the band f is in (bm: band_memories), {} when not read or none"""
    if not bm:
        return {}
    for bid, (lo, hi) in sorted(bm["bands"].items()):
        if lo <= f < hi and bid in bm["mem"]:
            return bm["mem"][bid]
    return {}


def band_memories(r):
    """Each band's own memories on r (band_params: VFO A's frequency, mode, ATT, PRE, AGC) and the band table, read
    over SSH (read only). A test that tunes into a band rewrites them: the audit of 2026-11-22 found radios-walkie,
    image.py and txcal.py leaving their test frequency, mode, ATT and AGC in every band they visited, and every
    test's restore setting the frequency first, so the test's band kept the test's settings. None: not read (said)"""
    if r.simulated:
        return None
    q = ("select 'b',id,start_freq,stop_freq from bands; select 'p',bands_id,name,val from band_params where name in "
         "(" + ",".join(f"'{m}'" for m in BAND_MEMS) + ")")
    try:
        out = ssh(r.host, f'sqlite3 -separator "|" /mnt/params.db "{q}"', timeout=20)
    except (Refused, subprocess.SubprocessError) as e:
        say(f"{r.name}: its band memories not read ({e}): the bands a test visits are not put back")
        return None
    bands, mem = {}, {}
    for line in out.splitlines():
        p = line.split("|")
        if p[0] == "b" and len(p) == 4:
            bands[int(p[1])] = (int(p[2]), int(p[3]))
        elif p[0] == "p" and len(p) == 4 and p[1].lstrip("-").isdigit():
            mem.setdefault(int(p[1]), {})[p[2]] = int(p[3])
    return {"bands": bands, "mem": mem}


DB_LEARNED = re.compile(r"P\|tci_tx_level")   # rows the radio moves by itself (TCI's learned TX level): left
# The owner, 2026-11-23: "any time you are *done* simply leave the radio on the last frequency it was on. stop
# trying to reset it to some state it was previously on short of simply being on the same screen". finish() leaves
# the dial and the receiver where the test left them (TUNING), the bands' memories and the per-mode rows with them
# (DB_TUNING); it still stops, locks the transmitter, closes the apps, puts back the power, TUNE's power, the volume,
# the ATU and the compressor, and opens the Sentry's page A. False: the whole snapshot put back, as before (sim_test)
LEAVE_TUNING = True
TUNING = ("split", "vfo", "freq", "mode", "filter", "zoom", "att", "pre", "agc", "rfg", "sql", "nb", "nr", "rit", "xit",
          "ifs", "wch", "txio", "txqo", "txig", "txip", "_bands")
DB_TUNING = re.compile(r"[BM]\||P\|(band_id|vfo[ab]_\w+|rfgain|sql)$")
DB_SETTLE_S = 4.0       # the GUI writes what changed every 3 s (settings_manager.cpp start_flush_thread)
DB_QUERY = ('.separator "\\037" "\\036"\n'
            "select 'P|'||name, quote(val) from params;"
            "select 'B|'||bands_id||'|'||name, quote(val) from band_params;"
            "select 'M|'||mode||'|'||name, quote(val) from mode_params;\n")


def db_rows(r):
    """Every row of r's params.db (params, band_params, mode_params) as {"P|name", "B|band|name", "M|mode|name": its
    value as an SQL literal}, read only. Holds secrets (the callsign, passwords): never said, never written to a file.
    None: not read (said)"""
    if r.simulated:
        return None
    try:
        out = ssh(r.host, "sqlite3 -readonly -cmd '.timeout 3000' /mnt/params.db", timeout=30, input=DB_QUERY)
    except (Refused, subprocess.SubprocessError) as e:
        say(f"{r.name}: its settings database not read ({e}): its rows are not put back")
        return None
    rows = {}
    for rec in out.split("\x1e"):
        if "\x1f" in rec:
            k, v = rec.lstrip("\n").split("\x1f", 1)
            rows[k] = v
    return rows


def _db_where(k):
    """The table, the key's columns and its WHERE for a db_rows() key; None for one not understood"""
    p = k.split("|")
    q = lambda x: "'" + x.replace("'", "''") + "'"
    if p[0] == "P" and len(p) == 2:
        return "params", f"name={q(p[1])}", f"name", q(p[1])
    if p[0] in ("B", "M") and len(p) == 3 and re.fullmatch(r"-?\d+", p[1]):
        t, c = ("band_params", "bands_id") if p[0] == "B" else ("mode_params", "mode")
        return t, f"{c}={int(p[1])} and name={q(p[2])}", f"{c},name", f"{int(p[1])},{q(p[2])}"
    return None


def db_put_back(r, before, skip=None):
    """r's settings database as before (db_rows) found it: a row new since deleted (a setting set and set back leaves
    one: the split tests left VFO B's 14.230 USB in 20 m's memories on both radios, where VFO B had followed VFO A,
    2026-11-23, and finish put back VFO A's alone), a changed one's value written again, one gone inserted again; the
    rows the radio moves by itself (DB_LEARNED) left. Written live, after the live settings are back and DB_SETTLE_S:
    the GUI reads a band's rows only as it goes to that band and writes only what changes, so its database is then
    the whole truth of them. Read back; the rows not back returned. Only the rows' names are said"""
    if before is None:
        return {}
    now = db_rows(r)
    if now is None:
        return {"db": "not read"}
    diff = sorted(k for k in set(before) | set(now)
                  if before.get(k) != now.get(k) and not DB_LEARNED.match(k) and not (skip and skip.match(k)))
    if not diff:
        return {}
    sql = ["begin;"]
    for k in diff:
        w = _db_where(k)
        if not w:
            continue
        table, where, cols, vals = w
        sql.append(f"delete from {table} where {where};")
        if k in before:
            sql.append(f"insert into {table}({cols},val) values({vals},{before[k]});")
    sql.append("commit;")
    try:
        ssh(r.host, "sqlite3 -cmd '.timeout 3000' /mnt/params.db", timeout=30, input="\n".join(sql) + "\n")
    except (Refused, subprocess.SubprocessError) as e:
        say(f"{r.name}: its settings database not written ({e})")
    after = db_rows(r) or {}
    bad = [k for k in diff if after.get(k) != before.get(k)]
    new = [k for k in diff if k not in before]
    say(f"{r.name}: its settings database put back as found: {len(diff)} row(s) "
        f"({', '.join(k.replace('|', ' ') for k in diff[:12])}{' ...' if len(diff) > 12 else ''}"
        f"{f'; {len(new)} new since, deleted' if new else ''})" + (f", NOT back: {bad}" if bad else ""))
    return {"db": bad} if bad else {}


def snapshot(r):
    """What finish() puts back: every key a test may change, as the radio reads it now, each band's own memories
    ("_bands") and every row of its settings database ("_db": never written to a file); from here the radio's dials
    are noted (Radio.freqs). Refused for a radio a test left not as found"""
    left_wrong_gate(r)
    state_ready(r)
    snap = {k: _value(r, k) for k in RESTORED}
    snap["_bands"] = band_memories(r)
    snap["_db"] = db_rows(r)
    r.freqs = {int(r.state.get("freq", 0))}
    return snap


def restore_bands(r, bm, home_freq):
    """Every band r was tuned into since snapshot() but its home band: its memories as bm found them (its saved
    frequency, then its mode, ATT, PRE and AGC), each read back. What did not come back is returned"""
    if not bm:
        return {}
    def bands_of(f):
        return {bid for bid, (lo, hi) in bm["bands"].items() if lo <= f < hi}
    visited = set().union(*(bands_of(f) for f in r.freqs)) - bands_of(home_freq)
    bad = {}
    for bid in sorted(visited):
        m = bm["mem"].get(bid, {})
        if "vfoa_freq" not in m or not bands_of(m["vfoa_freq"]):
            continue
        want = {"freq": m["vfoa_freq"]}
        for k in ("mode", "att", "pre", "agc"):
            if f"vfoa_{k}" in m:
                want[k] = m[f"vfoa_{k}"]
        b = restore(r, want, quiet=True)
        if b:
            bad[f"band{bid}"] = b
    if visited:
        say(f"{r.name}: the memories of {len(visited)} band(s) it visited put back"
            + (f", not all: {sorted(bad)}" if bad else ""))
    return bad


def restore(r, snap, quiet=False):
    """r as snap found it, in SET_ORDER (the dial first: the band's own ATT, PRE and AGC come with it and are then set
    as they were), each read back. What did not come back is said and returned, never raised: the end of a test puts
    back all it can"""
    bad, off_band = {}, False
    for k in SET_ORDER:
        v = snap.get(k)
        if v is None or k == "heard" or _same(k, _value(r, k), v):
            continue
        if off_band and k in BAND_KEYS:
            continue                    # never onto another band's memory (2026-11-22: an app held the dial)
        _put(r, k, v)
        try:
            r.wait(f"{k} back", lambda k=k, v=v: _same(k, _value(r, k), v), SET_WAIT_S)
        except Refused:
            if k == "freq":
                off_band = True
    time.sleep(0.4)
    for k, v in snap.items():
        if v is not None and not k.startswith("_") and k != "heard" and not _same(k, _value(r, k), v):
            bad[k] = {"reads": _value(r, k), "was": v}
    if bad and not quiet:
        say(f"{r.name}: NOT put back: " + ", ".join(f"{k} {b['reads']} (was {b['was']})" for k, b in bad.items()))
    return bad


def tap(r, key="AB"):
    r.send(f"key {key} 1")
    time.sleep(0.15)
    r.send(f"key {key} 0")


def arm(r, on, mode=None):
    """_arm as a transition (arming puts CW and the data modes into the band's voice mode, then mode again)"""
    with transition():
        return _arm(r, on, mode)


def _arm(r, on, mode=None):
    """The Sentry's A/B on A or G: the transmitter armed (rxo 0) or locked again (rxo 1), checked, three tries. Arming
    puts CW and the data modes into the band's voice mode (app_sentry.c arm_voice_mode, 42279ea2): mode, given, is
    set again after it and read back (the audit, 2026-11-22: tones.py's USB-D went out as USB, nothing said)"""
    for _ in range(3):
        if (r.state.get("rxo") == 0) == on:
            break
        tap(r)
        try:
            r.wait("TX " + ("armed" if on else "locked"), lambda: (r.state.get("rxo") == 0) == on, 5)
            break
        except Refused:
            pass
    else:
        raise Refused(f"{r.name}: TX not {'armed' if on else 'locked'}")
    if (r.state.get("rxo") == 0) != on:
        raise Refused(f"{r.name}: TX not {'armed' if on else 'locked'}")
    if on and mode is not None and int(r.state.get("mode", -1)) != mode:
        ensure(r, {"mode": mode})


G_FIRST_HZ = 27185000           # the Sentry G's own channel the first time (CB 19)
G_ENTRY_S = 3.0                 # G's entry tune comes within this of the page saying G


def cfg_get(r, name, timeout=4.0):
    """A setting by its database name (the remote's "cfg? NAME"), as text"""
    r.cfgv.pop(name, None)
    r.send(f"cfg? {name}")
    r.wait(f"the setting {name}", lambda: name in r.cfgv, timeout)
    return r.cfgv[name]


def cfg_set(r, name, value, timeout=4.0):
    """A setting by its database name set (the remote's "cfg NAME VALUE", through its validator on the radio) and read
    back; returns (as it was, as it is now)"""
    was = cfg_get(r, name, timeout)
    r.take_control()                    # a set needs control: a TCI client that keyed took it (its "take control first")
    r.send(f"cfg {name} {value}")
    end = time.time() + timeout
    now = was
    while time.time() < end:
        now = cfg_get(r, name, timeout)
        if now != was or str(value) == now:
            break
        time.sleep(0.2)
    return was, now


def open_page(r, letter):
    """_open_page as a transition (G tunes its own channel, page A brings back A's dial): the other radio's page and
    dial follow within DIAL_FOLLOW_S"""
    with transition():
        got = _open_page(r, letter)
    _dial_moved()
    return got


def settled(r, still=1.5, timeout=8.0, keys=("freq", "mode", "app", "win", "pg")):
    """r's dial, mode, app and page unchanged for still seconds (an app closing puts its dial back a moment after it
    says it closed: text-log's X6100 went back to 14.2 MHz after it had been set to the app's 3.58, 2026-11-24), at
    most timeout; True when it settled"""
    t0 = time.time()
    last, since = None, t0
    while time.time() - t0 < timeout:
        cur = tuple(r.state.get(k) for k in keys)
        if cur != last:
            last, since = cur, time.time()
        elif time.time() - since >= still:
            return True
        time.sleep(0.1)
    return False


def open_page_both(radios, letter):
    """The Sentry's page letter on every radio as one transition, then the radios set alike again on the dial and mode
    they shared before: G tunes each to its own channel, the last each was on there, which differ once a test left one
    radio on G apart (air armed-ptt-a: the X6100's G on 14.2 AM against the X6200's USB, 11 s apart, 2026-11-24)"""
    want = {k: _value(radios[0], k) for k in ("freq", "mode")}
    alike = all(_value(r, k) == want[k] for r in radios[1:] for k in want)
    with transition():
        for r in radios:
            open_page(r, letter)
        if alike and len(radios) > 1:
            sync_pair(radios, want)


def _open_page(r, letter):
    """The Sentry on its page letter, asked twice (an app still closing can take the first ask). G entered: its own
    channel's tune waited for (a GUI with "wch": the page says G a moment before G tunes its channel, and a dial set
    meanwhile was moved again, 2026-11-23); a G that does not tune (TX armed, the dial held, on an SDR's channel)
    after G_ENTRY_S"""
    for wait in (5, 8):
        if r.state.get("pg") == letter:
            return
        r.send(f"open sentry {letter}")
        try:
            r.wait(f"the Sentry's page {letter}", lambda: r.state.get("pg") == letter, wait)
            break
        except Refused:
            if wait == 8:
                raise
    if letter == "G" and r.state.get("wch") is not None:
        want = int(r.state.get("wch") or 0) or G_FIRST_HZ
        try:
            r.wait("G on its own channel", lambda: abs(int(r.state.get("freq", 0)) - want) <= 3000, G_ENTRY_S)
        except Refused:
            pass                        # G stayed on the dial as found: its own message says why
        time.sleep(0.3)
    elif letter != "G":
        # Another page brings back its own dial a moment after it says it is there: waited for, or a dial set right
        # after was moved again (air cw-kbd 2026-11-25: the X6100 back on page A's 14.200 USB after it was set to CW
        # on 14.040, 'moved again after being set')
        settled(r, still=0.6, timeout=3.0, keys=("freq", "mode", "pg"))


def close_apps(r, keep_dial=True):
    """An app open closed (it holds the dial: a band's memories cannot be put back under it, 2026-11-22); the Sentry
    itself is no app. Said if it stays. keep_dial: r back on the dial and mode the app was on, one transition: closing
    puts it on the Sentry page's own dial (G's channel, 29.6 FM), apart from the other radio while the test goes on
    (psk, rtty, jtty, olivia, 2026-11-24)"""
    if not (r.state.get("app") or (r.state.get("win") or "Sentry") != "Sentry"):
        return True
    f, m = int(r.state.get("freq") or 0), int(r.state.get("mode", -1))
    with transition():
        for _ in range(3):
            r.send("close")
            try:
                r.wait("the app closed", lambda: not r.state.get("app") and (r.state.get("win") or "Sentry") == "Sentry",
                       4)
                break
            except Refused:
                pass
        else:
            say(f"{r.name}: its app {r.state.get('app') or r.state.get('win')} did not close")
            return False
        if keep_dial and f and m >= 0:
            settled(r)
            try:
                ensure(r, {"freq": f, "mode": m})
            except Refused as e:
                say(f"{r.name}: not back on its app's dial {f} mode {m} after closing it ({e})")
    return True


FINISH_TRIES = 3                # finish()'s attempts at a radio before it is recorded as left wrong
LEFT_WRONG = "left_wrong.json"  # the radios a test could not put back: no test touches them till bench.py restore


def stop_bench_programs(r):
    """The harness's own programs on the radio ended: a TCI sender holds control while it is connected (the X6200's
    dial stayed on 15 MHz under one, 2026-11-22), a recording runs on. BusyBox has no pkill"""
    if r.simulated:
        return
    try:
        ssh(r.host, "kill $(cat /tmp/tci_tone_tx.pid 2>/dev/null) 2>/dev/null; for p in $(ps | grep -E "
                    "'tci_(tone|cw)_tx|parecord' | grep -v grep | awk '{print $1}'); do kill $p; done; true", timeout=20)
    except (Refused, subprocess.SubprocessError) as e:
        say(f"{r.name}: its bench programs not stopped ({e})")


def put_back(r, snap, keep=(), before=None):
    """One attempt at r: its link renewed if it stalled, the bench's programs stopped, control, apps closed, the
    transmitter locked, then everything put back and read back, the Sentry's page A. What is not back, returned"""
    bad = {}
    if r.stale():
        r.reconnect()
    r.stop()
    stop_bench_programs(r)
    if not r.ctl_mine:
        r.take_control()                # an emergency stop's session, a TCI client had it; the operator: Refused,
                                        # the radio recorded as not put back (finish puts it back later)
    r.send("banner")
    if not close_apps(r):
        bad["app"] = r.state.get("app") or r.state.get("win")
    if r.state.get("rxo") == 0 and r.may_tx:
        try:
            arm(r, False)
        except Refused as e:
            bad["armed"] = str(e)
    # page A first: G left puts A's dial back as G found it (the Sentry's G has its own channel, 2026-11-23), which
    # would undo what is put back here if it came after
    open_page(r, "A")
    for want in before or []:
        bad.update(restore(r, want))
    snap = {k: v for k, v in snap.items() if k not in keep}
    if "freq" not in keep:
        bad.update(restore_bands(r, snap.get("_bands"), snap.get("freq") or int(r.state.get("freq", 0))))
    bad.update(restore(r, snap))           # its home band last: the dial, then that band's own settings
    open_page(r, "A")
    return bad


def finish(radios, snaps, keep=(), report=None, before=None):
    """The radios put back (_finish) as one transition: each set in turn, no difference counted meanwhile; a pair ends
    alike (the idle radio following), one radio put back leaves the other DIAL_FOLLOW_S to follow"""
    with transition():
        out = _finish(radios, snaps, keep, report, before)
    if len(radios) != 2:
        _dial_moved()               # one radio put back: the other follows; a pair ends alike (sync_pair)
    return out


def _finish(radios, snaps, keep=(), report=None, before=None):
    """The end of every test, passed or not: each radio stopped, its transmitter locked, put back as snaps found it
    (all but keep), read back, on the Sentry's page A, the harness's control released. before ({name: [want, ...]}):
    put back first what snaps cannot hold (a mode's own filter). A radio NEVER stays not as found (the owner,
    2026-11-22: "The X6200 was left on the wrong frequency ... fix the harness. this should NEVER happen"): up to
    FINISH_TRIES attempts, each renewing a stalled link, stopping the bench's own programs, taking control again; what
    is still not back then is recorded (LEFT_WRONG) and every test refuses that radio till bench.py restore puts it
    back. What did not come back is said and kept in report["not_restored"]"""
    if LEAVE_TUNING:                        # the dial and the receiver left where the test left them
        keep, before = tuple(keep) + TUNING, None
    bads = {}
    for r in radios:
        snap, bad = snaps.get(r.name, {}), {}
        for attempt in range(FINISH_TRIES):
            try:
                if attempt:
                    r.reconnect()
                bad = put_back(r, snap, keep, (before or {}).get(r.name, []))
            except (Refused, OSError) as e:
                bad = {"error": str(e)}
            if not bad:
                break
            say(f"{r.name}: not all put back (try {attempt + 1} of {FINISH_TRIES}): {bad}")
            time.sleep(2.0)
        bads[r.name] = bad
    # the dial left where the test left it, the idle radio following (the owner, 11-23): the second radio of a pair
    # set to the first's dial and mode, the two read back alike. Left as each was, a sender in AM on A and a listener
    # whose page A brought back A's own dial stayed apart into the next test (air armed-ptt-a, 2026-11-24)
    if LEAVE_TUNING and len(radios) == 2 and not any(bads.values()):
        try:
            sync_pair(radios, {k: _value(radios[0], k) for k in ("freq", "mode")})
        except Refused as e:
            bads[radios[1].name] = {"follow": str(e)}
    # then each radio's settings database (the rows its live settings cannot reach: other bands' VFO B, a row left
    # where none was), once the GUI has written what the put back changed
    if any(snaps.get(r.name, {}).get("_db") is not None and not bads[r.name] for r in radios):
        time.sleep(DB_SETTLE_S)
    for r in radios:
        snap, bad = snaps.get(r.name, {}), bads[r.name]
        if not bad and "_db" not in keep:
            bad = db_put_back(r, snap.get("_db"), DB_TUNING if LEAVE_TUNING else None)
        if bad:
            say(f"{r.name}: LEFT NOT AS FOUND after {FINISH_TRIES} tries: {bad}. No test touches it till "
                f"bench.py restore puts it back")
            doc = load_json(os.path.join(STATE, LEFT_WRONG), {})
            doc[r.name] = {"at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "bad": bad,
                           "snap": {k: v for k, v in snap.items() if k not in keep and k != "_db"}}
            save_json(os.path.join(STATE, LEFT_WRONG), doc)
        if report is not None and bad:
            report.setdefault("not_restored", {})[r.name] = bad
        r.close()


def left_wrong_gate(r):
    """No test on a radio a test could not put back (finish() recorded it): bench.py restore first"""
    doc = load_json(os.path.join(STATE, LEFT_WRONG), {})
    if r.name in doc:
        raise Refused(f"{r.name} was left not as found ({doc[r.name]['at']}: {doc[r.name]['bad']}): "
                      f"bench.py restore puts it back first")


def cmd_restore(c, a):
    """The radios a test left not as found, put back from the snapshot taken before that test, read back"""
    doc = load_json(os.path.join(STATE, LEFT_WRONG), {})
    if not doc:
        say("no radio was left not as found")
        return
    for name, ent in list(doc.items()):
        toks = load_json(os.path.join(STATE, "tokens.json"), {}).get(name, {})
        r = Radio(name, c["radios"][name], "tx" in toks)
        r.take_control()
        bad = {}
        for _ in range(FINISH_TRIES):
            try:
                bad = put_back(r, ent["snap"])
            except (Refused, OSError) as e:
                bad = {"error": str(e)}
            if not bad:
                break
            time.sleep(2.0)
            r.reconnect()
        r.close()
        if bad:
            raise Refused(f"{name}: still not as found: {bad}")
        doc.pop(name)
        save_json(os.path.join(STATE, LEFT_WRONG), doc)
        say(f"{name}: put back as it was before the test of {ent['at']}")


def spectrum_level(r, hz, t0, t1, half=3000):
    """r's spectrum frames between t0 and t1: the mean over the frames of the highest bin within half of hz, and how
    many frames; (None, 0) when none covered it"""
    tops = []
    for t, cen, wid, v in list(r.r.spectra):
        if not (t0 <= t <= t1) or not v:
            continue
        per = wid / len(v)
        a, b = int((hz - half - (cen - wid / 2)) / per), int((hz + half - (cen - wid / 2)) / per)
        seg = v[max(a, 0):min(b + 1, len(v))]
        if seg and a < len(v) and b >= 0:
            tops.append(max(seg))
    return (sum(tops) / len(tops), len(tops)) if tops else (None, 0)


# ---- The path proof: the listener hears the sender before a test measures anything ----
PROOF_HZ = 1000             # the tone above the dial (USB-D)
PROOF_S = 5.0             # long enough for both screens to be photographed in the middle of it (the owner, 2026-11-23)
PROOF_MIN_DB = 20           # the listener's spectrum at the tone over its quiet, at the least (0.1 W through 70-80 dB:
                            # 50-59 dB on the X6200, 2026-11-22)
PROOF_DROP_DB = 15          # weaker than the last proof of the same pair by this much: said


# ---- Both radios alike, and both pictured while one sends ----
# The owner, 2026-11-23: "when you TX on one radio screenshot them BOTH, you should see the RX happening! and they
# should have the same settings. IF they don't something is wrong". So before any over the listener is set first and
# read back, the sender is given exactly the listener's set-up, the two are compared (a difference refuses the test),
# and during the over both screens are photographed (SHOTS_DIR, named for the test, the over and the radio).
PAIR_KEYS = ("freq", "mode", "flo", "fhi", "pre", "att", "agc", "rfg", "sql", "nb", "nr", "rit", "split", "vfo", "ifs",
             "zoom")
PAIR_SET = ("freq", "mode", "filter", "pre", "att", "agc", "rfg", "sql", "nb", "nr", "rit", "split", "vfo", "zoom")
SHOTS_DIR = os.path.join(STATE, "shots")
PANEL_WH = (800, 480)                   # the screen's raw buffer: 480x800 portrait, saved landscape


def pair_diff(a, b):
    """The receiver settings a and b do not share: {key: (a's, b's)}"""
    return {k: (a.state.get(k), b.state.get(k)) for k in PAIR_KEYS if a.state.get(k) != b.state.get(k)}


def pair_align(sender, listener, f=None, mode=None):
    """The listener first (f and mode when given), read back; then the sender set exactly as the listener, read back;
    then the two compared. Refused, naming the keys, when they differ. Returns the set-up both have"""
    first = {k: v for k, v in (("freq", f), ("mode", mode)) if v is not None}
    if first:
        ensure(listener, first)
    want = {k: _value(listener, k) for k in PAIR_SET if _value(listener, k) is not None}
    sync_pair([listener, sender], want)         # the one way the two are set alike
    time.sleep(0.5)
    diff = {k: v for k, v in pair_diff(sender, listener).items() if not declared(k, *v)}
    if diff:
        raise Refused(f"{sender.name} and {listener.name} are not set alike before the over: " +
                      ", ".join(f"{k} {x}/{y}" for k, (x, y) in diff.items()))
    return {k: listener.state.get(k) for k in PAIR_KEYS}


# ssh -4: from Python each connection waited ~5 s on the name's IPv6 lookup (2026-11-23: 5.3 s against 0.33 s)
def screen_capture(radio, tag):
    """radio's screen kept on the radio now (/tmp/bench_shot_<tag>.raw): fast, so it can be taken during an over;
    screen_fetch brings it here later. True when it was kept"""
    if radio.simulated:
        return False
    cmd = (f"rm -f /tmp/screen.raw /tmp/bench_shot_{tag}.raw; kill -USR1 $(pidof x6100_gui); "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -f /tmp/screen.raw ] && break; sleep 0.1; done; "
           f"sleep 0.2; mv /tmp/screen.raw /tmp/bench_shot_{tag}.raw && echo kept")
    try:
        r = subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{radio.host}", cmd],
                           capture_output=True, text=True, timeout=30)
        return "kept" in r.stdout
    except (subprocess.TimeoutExpired, OSError):
        return False


def screen_fetch(radio, tag, label):
    """A screen screen_capture kept, here as SHOTS_DIR/<time>_<label>_<name>.png (landscape): its path, or None"""
    try:
        import gzip
        from PIL import Image
    except ImportError:
        return None
    try:
        r = subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{radio.host}",
                            f"gzip -1 -c /tmp/bench_shot_{tag}.raw && rm -f /tmp/bench_shot_{tag}.raw"],
                           capture_output=True, timeout=90)
        data = gzip.decompress(r.stdout)
    except (subprocess.TimeoutExpired, OSError, EOFError):
        return None
    if len(data) != PANEL_WH[0] * PANEL_WH[1] * 4:
        return None
    os.makedirs(SHOTS_DIR, exist_ok=True)
    im = Image.frombuffer("RGBA", (PANEL_WH[1], PANEL_WH[0]), data, "raw", "BGRA", 0, 1).convert("RGB")
    path = os.path.join(SHOTS_DIR, f"{time.strftime('%Y%m%d-%H%M%S')}_{re.sub(r'[^A-Za-z0-9]+', '_', label)}_"
                                   f"{radio.name}.png")
    im.rotate(-90, expand=True).save(path)
    return path


def screenshot(radio, label):
    """radio's screen now, landscape, into SHOTS_DIR: its path, or None"""
    tag = f"{os.getpid()}_{radio.name}"
    return screen_fetch(radio, tag, label) if screen_capture(radio, tag) else None


def _both(fn, radios):
    out = {}
    ths = [threading.Thread(target=lambda r=r: out.__setitem__(r.name, fn(r))) for r in radios]
    for t in ths:
        t.start()
    for t in ths:
        t.join(90)
    return out


def screenshots_both(radios, label):
    """Every radio's screen at the same moment (captured together, fetched after): {name: path}"""
    tag = f"{os.getpid()}_{int(time.time() * 1000)}"
    kept = _both(lambda r: screen_capture(r, f"{tag}_{r.name}"), radios)
    return _both(lambda r: screen_fetch(r, f"{tag}_{r.name}", label) if kept.get(r.name) else None, radios)


def shots_during(radios, label, sender, settle_s=0.5, wait_s=15.0, tries=4):
    """Both screens captured while the sender is on the air (its remote state "tx"; a fixed delay missed the over,
    2026-11-23): once it goes on the air and settle_s more, both captured together on the radios (fast), kept when
    the sender was on the air from before the capture to after it, else captured again (tries) until the over ends;
    fetched afterwards. A started thread; once joined .paths ({name: path}), .on_air, .tries"""
    def run():
        t0 = time.time()
        while time.time() - t0 < wait_s and not sender.tx:
            time.sleep(0.05)
        if not sender.tx:
            return
        time.sleep(settle_s)
        tag = f"{os.getpid()}_{int(time.time() * 1000)}"
        for k in range(tries):
            if not sender.tx:
                break
            since = sender.tx_since
            kept = _both(lambda r: screen_capture(r, f"{tag}_{k}_{r.name}"), radios)
            th.tries = k + 1
            good = bool(sender.tx) and sender.tx_since == since and all(kept.values())
            if good or k == tries - 1 or not sender.tx:
                th.on_air = good
                th.paths = _both(lambda r: screen_fetch(r, f"{tag}_{k}_{r.name}", f"{label} {k + 1}")
                                 if kept.get(r.name) else None, radios)
                break
            _both(lambda r: subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{r.host}",
                                            f"rm -f /tmp/bench_shot_{tag}_{k}_{r.name}.raw"], capture_output=True,
                                           timeout=20), radios)
    th = threading.Thread(target=run)
    th.paths, th.on_air, th.tries = {}, False, 0
    th.start()
    return th


def tci_send(sender, plan, amp=0.3, max_s=10.0, fade=0.05, watch=None, file=None, listener=None, label="over"):
    """plan ([[hz, s], ...]) into the sender's transmitter through its own TCI server (tci_tone_tx.py run on it): the
    sender's own stop if the watch stops. Returns (its output lines, epoch it began, epoch it ended). With listener:
    both screens photographed halfway through the plan (screenshots_both; their paths in tci_send.last_shots)"""
    shots = None
    if listener is not None and plan and not sender.simulated:
        shots = shots_during([sender, listener], label, sender,
                             settle_s=min(1.0, max(0.3, sum(sec for _, sec in plan) / 3)))
    sender_py = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tci_tone_tx.py")
    if not sender.simulated:
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", sender_py, f"root@{sender.host}:/tmp/tci_tone_tx.py"],
                       check=True)
    t0 = time.time()
    if sender.simulated:
        plan = plan or [[1500, max_s - 4]]
        sender.send(f"sim tone {json.dumps(plan)}")           # sim_test.py's radio keys itself for the plan
        time.sleep(sum(s for _, s in plan) + 0.3)
        return [], t0, time.time()
    proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{sender.host}",
                             f"python3 /tmp/tci_tone_tx.py '{'@' + file if file else json.dumps(plan)}' {amp} "
                             f"{max_s:.1f} 50001 {fade}"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    while proc.poll() is None:
        if watch is not None and watch.why:
            subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{sender.host}",
                            "kill $(cat /tmp/tci_tone_tx.pid) 2>/dev/null"], capture_output=True)
            break
        time.sleep(0.05)
    out = proc.communicate(timeout=30)[0]
    t1 = time.time()
    tci_send.last_shots, tci_send.last_on_air = {}, False
    if shots is not None:
        shots.join(80)
        tci_send.last_shots, tci_send.last_on_air = shots.paths, shots.on_air
    return out.splitlines(), t0, t1


def path_proof(c, total, sender, listener, f, report=None, label=None):
    """1000 Hz over the sender's TCI at 0.1 W for 2 s (USB-D on f, its transmitter armed on G for it and locked
    after), and the listener's spectrum at f + 1000 Hz must rise PROOF_MIN_DB over its quiet. Refused when it does not,
    with what was seen; said when it is PROOF_DROP_DB under the last proof of the same pair. The listener must be set
    (ensure) before; the sender is left on f in USB-D at 0.1 W. label: its screenshots' name (the test's own)"""
    open_page(sender, "G")
    open_page(listener, "G")
    # both alike, the listener first: the same frequency and mode (USB-D, the sender's), filter, PRE, ATT, AGC... read
    # back on both and compared (the owner, 2026-11-23: "they should have the same settings")
    pair_align(sender, listener, f, 3)
    ensure(sender, *tx_profile(f, 3, FIRST_PROBE_W))
    if pair_diff(sender, listener):
        raise Refused(f"path proof: not alike after the sender's TX set-up: {pair_diff(sender, listener)}")
    radio_tx_gate(sender, c, FIRST_PROBE_W)
    hz = f + PROOF_HZ
    for r in (sender, listener):
        r.send(f"banner BENCH PATH PROOF: {sender.name.upper()} SENDS {PROOF_HZ} HZ {FIRST_PROBE_W} W, "
               f"{listener.name.upper()} MUST SEE IT ({total} DB OF PADS)")
    time.sleep(1.2)
    quiet, nq = spectrum_level(listener, hz, time.time() - 1.0, time.time(), half=150)
    watch = Watch([sender, listener], c, expect_dbm=10 * math.log10(FIRST_PROBE_W * 1000) - total, listener=listener,
                  w=FIRST_PROBE_W, label=label or f"path proof {sender.name} to {listener.name}")
    try:
        arm(sender, True, mode=3)
        tx_log({"who": sender.name, "f": f, "w": sender.pwr(), "what": "path proof"})
        tone_s = min(PROOF_S, c["limits"]["max_key_s"] - 1.5)     # always under the over's own limit
        _, t_on, t_off = tci_send(sender, [[0, 0.3], [PROOF_HZ, tone_s]], watch=watch)
    finally:
        watch.close()
        try:
            sender.wait("off the air", lambda: not sender.tx, 5)
        except Refused:
            pass
        arm(sender, False)
        for r in (sender, listener):
            r.send("banner")
    if watch.why:
        raise Refused(f"path proof stopped: {watch.why}")
    loud, nl = spectrum_level(listener, hz, t_on + 0.8, t_off - 0.2, half=150)
    peak = watch.tx_peak.get(sender.name)
    res = {"sender": sender.name, "listener": listener.name, "f": f, "pads_db": total, "quiet_db": quiet, "tone_db": loud,
           "shots": (watch.shots[-1]["paths"] if watch.shots else {}),
           "shots_on_air": bool(watch.shots and watch.shots[-1]["on_air"]),
           "rise_db": None if quiet is None or loud is None else round(loud - quiet, 1), "frames": [nq, nl],
           "sender_peak_w": peak, "at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "tone_hz": hz, "t_on": t_on,
           "t_off": t_off}
    if report is not None:
        report["path_proof"] = res
    if not nq or not nl:
        raise Refused(f"path proof: {listener.name} sent no spectrum covering {hz} Hz ({nq} frames before, {nl} during)")
    if not peak or peak < 0.03:
        raise Refused(f"path proof: {sender.name} put out no power (its meter's highest {peak} W)")
    say(f"path proof: {listener.name} sees {sender.name}'s 0.1 W tone {res['rise_db']} dB over its quiet "
        f"({quiet:.0f} -> {loud:.0f}; the sender's meter {peak} W)")
    if res["rise_db"] < PROOF_MIN_DB:
        raise Refused(f"path proof: {listener.name} hears {sender.name}'s 0.1 W tone only {res['rise_db']} dB over its "
                      f"quiet (at least {PROOF_MIN_DB}): it does not hear the sender. Its state: "
                      + ", ".join(f"{k} {listener.state.get(k)}" for k in ("freq", "mode", "att", "pre", "rfg", "agc",
                                                                           "play", "heard")))
    proofs = load_json(os.path.join(STATE, "proofs.json"), {})
    key = f"{sender.name}>{listener.name}@{total}"
    last = proofs.get(key)
    if last and res["rise_db"] < last["rise_db"] - PROOF_DROP_DB:
        say(f"path proof: {res['rise_db']} dB is {last['rise_db'] - res['rise_db']:.0f} dB under the last proof of "
            f"this pair ({last['rise_db']} dB, {last['at']}): the path or a receiver changed")
        res["weaker_than_last_db"] = round(last["rise_db"] - res["rise_db"], 1)
    proofs[key] = res
    save_json(os.path.join(STATE, "proofs.json"), proofs)
    return res


# ---- The gates ----

def power_ok():
    """The power the owner allowed over HARD_MAX_W in the last 12 hours (power-ok), else None"""
    ok = load_json(os.path.join(STATE, "power.json"), {}).get("radios")
    if not ok or time.time() - ok["at"] > CHAIN_VALID_S:
        return None
    return min(float(ok["w"]), FULL_RATING_MAX_W)


def chain_gate(c, profile):
    ok = load_json(os.path.join(STATE, "chain.json"), {}).get(profile)
    if not ok or time.time() - ok["at"] > CHAIN_VALID_S:
        raise Refused(f"the owner has not said the {profile} cable is in place in the last 12 hours: ask, then "
                      f"bench.py chain-ok {profile} \"their words\"")
    p = c.get("profiles", {}).get(profile)
    if not p:
        raise Refused(f"no profile {profile} in the configuration")
    pads = p.get("pads", [])
    total = sum(x["db"] for x in pads)
    if profile == "hackrf":
        if total < HACKRF_MIN_DB:
            raise Refused(f"the HackRF profile's pads add up to {total} dB, under {HACKRF_MIN_DB}")
    else:
        if total < RADIOS_MIN_DB:
            raise Refused(f"the radios profile's pads add up to {total} dB, under {RADIOS_MIN_DB}")
        for name in p.get("between", []):
            at = [x for x in pads if x.get("at") == name]
            w = c["limits"]["radio_w"]
            need = w if w > HARD_MAX_W else w * 2       # over 5 W: the owner's word for the pads' own rating
            if not at or at[0].get("watts", 0) < need:
                raise Refused(f"no pad rated for {'the' if w > HARD_MAX_W else 'twice the'} test power on "
                              f"{name}'s antenna jack")
    return p, total


def hackrf_gate(x):
    if x > HACKRF_MAX_X or x < 0:
        raise Refused(f"HackRF gain -x {x}: 0 to {HACKRF_MAX_X} only")
    r = subprocess.run(["hackrf_info"], capture_output=True, text=True)
    if "Found HackRF" not in r.stdout:
        raise Refused("no HackRF on this computer")


def rx_only_gate(radio):
    """The radio the HackRF feeds: a session that may not transmit, nothing on the air, no app answering by itself"""
    if radio.may_tx:
        raise Refused(f"{radio.name}: the HackRF profile works the radio with a session that may NOT transmit "
                      f"(bench.py provision {radio.name} rx), never one that may")
    if radio.tx:
        raise Refused(f"{radio.name}: on the air")
    if radio.ctl_mine and radio.state.get("rxo") != 1:
        # The radio's own hold (rxo: radio_rx_only), held while a session without TX controls it: every way to the
        # air refused there, its keys' too, the base at 0 W. A GUI without "rxo" is older than that hold (v0.3.12.28
        # and before: such a session's ATU or MSG key could send)
        raise Refused(f"{radio.name}: the radio does not hold itself receive only with this session in control "
                      f"(rxo {radio.state.get('rxo', 'missing: its GUI is older than the hold')})")
    if radio.ft8.get("sending") or radio.ft8.get("cq") or (radio.ft8.get("auto") and not radio.ft8.get("nocall")):
        # Auto on a radio without a callsign answers nothing: its FT8 app cannot send at all ("nocall")
        raise Refused(f"{radio.name}: its FT8 app is set to send (Auto or CQ, with a callsign): turn it off at the "
                      f"radio")


SUPPLY_HOLDS_V = 11.0       # the radio's DC in at or over this: its supply holds it (the stock 12 V chargers read 11.7-11.9)


def supply_gate(radio, w):
    """The radio's own battery log (/mnt/battery_log.csv, a row a minute: its battery's %, its DC in): no over on a
    radio running on a low battery because its supply does not hold it up. 2026-11-20 the X6100's stock adapter (a
    12 V charger) sagged to 0.2-3.9 V in most minutes it transmitted, the radio ran on its battery all evening
    (52 % -> 1 %) and switched off in the first 10 W over, with the owner away. Unreadable: said, not a stop"""
    if radio.simulated:
        return
    try:
        rows = [r.split(",") for r in ssh(radio.host, "tail -3 /mnt/battery_log.csv", timeout=20).split()]
        rows = [(int(r[0]), int(float(r[2])), float(r[3])) for r in rows if len(r) > 3 and r[0].isdigit()]
    except (Refused, subprocess.SubprocessError, ValueError) as e:
        say(f"{radio.name}: its battery log not read ({e}): its supply not checked")
        return
    if not rows or time.time() - rows[-1][0] > 300:
        say(f"{radio.name}: no battery log row in the last 5 minutes: its supply not checked")
        return
    _, bat, dc = rows[-1]
    low = min(r[2] for r in rows)
    if bat <= 10 and dc < SUPPLY_HOLDS_V:
        raise Refused(f"{radio.name}: on its battery at {bat} % (DC in {dc} V): charge it, or a supply that holds it")
    if low < SUPPLY_HOLDS_V and bat < 30:
        raise Refused(f"{radio.name}: its supply is not holding it up (DC in down to {low} V in the last minutes) and "
                      f"its battery is at {bat} %: a supply for transmitting, or charge it first")
    if w > HARD_MAX_W and low < SUPPLY_HOLDS_V:
        raise Refused(f"{radio.name}: over {HARD_MAX_W} W only on a supply that holds it up (DC in down to {low} V in "
                      f"the last minutes)")


LISTENER_MIN_PCT = 25       # a listener's battery at least this, unless its DC in holds it (12 V and over)
LISTENER_DC_HOLDS_V = 12.0


def listener_gate_read(radio):
    """The radio's last battery log row: {battery_pct, vbat, dc_in}, or None unread (simulated, no row in 5 min)"""
    if radio.simulated:
        return None
    try:
        rows = [r.split(",") for r in ssh(radio.host, "tail -2 /mnt/battery_log.csv", timeout=20).split()]
        rows = [(int(r[0]), float(r[1]), int(float(r[2])), float(r[3])) for r in rows if len(r) > 3 and r[0].isdigit()]
    except (Refused, subprocess.SubprocessError, ValueError):
        return None
    if not rows or time.time() - rows[-1][0] > 300:
        return None
    return {"battery_pct": rows[-1][2], "vbat": rows[-1][1], "dc_in": rows[-1][3]}


def listener_gate(radio):
    """The listener's own battery log: 2026-11-22 the X6200 heard nothing of the X6100 through the chain with its
    battery at 7.0-7.6 V (10-21 %, on its stock 12 V 1 A charger at 11.9 V), and heard it 50-59 dB over its floor again
    on a full battery; it also switched itself off under the tests. A listener under LISTENER_MIN_PCT on a supply
    under LISTENER_DC_HOLDS_V: refused. Returns (battery %, DC in V) for the report, or None unread"""
    if radio.simulated:
        return None
    try:
        rows = [r.split(",") for r in ssh(radio.host, "tail -2 /mnt/battery_log.csv", timeout=20).split()]
        rows = [(int(r[0]), float(r[1]), int(float(r[2])), float(r[3])) for r in rows if len(r) > 3 and r[0].isdigit()]
    except (Refused, subprocess.SubprocessError, ValueError) as e:
        say(f"{radio.name}: its battery log not read ({e}): its supply not checked")
        return None
    if not rows or time.time() - rows[-1][0] > 300:
        say(f"{radio.name}: no battery log row in the last 5 minutes: its supply not checked")
        return None
    _, vbat, bat, dc = rows[-1]
    if bat < LISTENER_MIN_PCT and dc < LISTENER_DC_HOLDS_V:
        raise Refused(f"{radio.name}: listening on a battery at {bat} % ({vbat} V, DC in {dc} V): its receiver went deaf "
                      f"like this (2026-11-22) and it switches itself off at 7.0 V. A full battery or a supply first")
    return {"battery_pct": bat, "vbat": vbat, "dc_in": dc}


def radio_tx_gate(radio, c, w, rf_hz=None):
    """rf_hz: what the base transmits on when it is not the dial (a transverter's IF: the dial 144.174 goes out on
    28.174), checked against the amateur bands in its place"""
    if not radio.may_tx:
        raise Refused(f"{radio.name}: the harness's session there may not transmit")
    supply_gate(radio, w)
    if w > c["limits"]["radio_w"] or (w > HARD_MAX_W and (power_ok() or 0) < w):
        raise Refused(f"{w} W is over the limit ({c['limits']['radio_w']} W)")
    if w > HARD_MAX_W and radio.tx_last:
        # the pad at its rating: off at least as long as the last over was on
        on_s = radio.tx_last[1] - radio.tx_last[0]
        rest = on_s - (time.time() - radio.tx_last[1])
        if rest > 0:
            say(f"{radio.name}: the pad rests {rest:.1f} s before {w} W again")
            time.sleep(rest)
    if radio.state.get("atu"):
        raise Refused(f"{radio.name}: the ATU is on (it tunes by transmitting): off first")
    freq = int(rf_hz if rf_hz is not None else radio.state.get("freq", 0))
    if not any(lo <= freq <= hi for lo, hi in HAM_HF):
        raise Refused(f"{radio.name}: {freq} Hz is outside the amateur bands")
    if abs(radio.pwr() - w) > 0.05:
        raise Refused(f"{radio.name}: its power reads {radio.pwr()} W, not the {w} W asked")


def gui_pids(radios):
    """Each radio's GUI process (pidof x6100_gui), None when it could not be read: a link's stall leaves it, a crash
    changes it"""
    out = {}
    for r in radios:
        try:
            out[r.name] = ssh(r.host, "pidof x6100_gui", timeout=15).strip() or None
        except Exception:               # noqa: BLE001 (the link to it down: unknown)
            out[r.name] = None
    return out


def once_more_after_stall(radios, fn, what):
    """fn() (an over); once more when a Watch stopped it for a radio's silent link and every GUI is the same process
    after: a WiFi stall (the X6200's, 3-6 s now and then: ORCH_FINDINGS 2026-11-23), no fault of what the over tests.
    Never after a crash (a new GUI process) or any other stop"""
    pids = gui_pids(radios)
    try:
        return fn()
    except Refused as e:
        if "no word from" not in str(e) or any(v is None for v in pids.values()):
            raise
        time.sleep(2.0)
        if gui_pids(radios) != pids:
            raise
        say(f"{what}: stopped by a stalled link ({e}); the GUIs the same processes: the over once more")
        return fn()


def stale_limit(r, listener=None):
    """How long r may go unheard before a Watch stops the over: STALE_HELD_S for a radio off the air at its last word
    that cannot go on it unseen (its TX locked, rxo 1; or the over's listener in an app that keys only when told to,
    RX_APPS); STALE_S for any other (a sender, a radio on the air, an app that may key by itself: FT8's, JS8's)"""
    if r.tx:
        return STALE_S
    if r.state.get("rxo") == 1:
        return STALE_HELD_S
    if r is listener and (r.state.get("app") or r.state.get("win") or "") in RX_APPS:
        return STALE_HELD_S
    return STALE_S


class Watch:
    """While a test runs: a radio on the air past max_key_s, on a bad SWR, or over the rating of the pad on its jack,
    or a receiver hearing far more than the chain lets through, stops everything at once. More power than asked, under
    the pad's rating, is logged (said, tx_log, over_power) and the over goes on"""

    def __init__(self, radios, c, hackrf=None, expect_dbm=None, listener=None, w=None, label="over", pair_ignore=(),
                 offsets=None):
        self.radios, self.c, self.hackrf, self.why = radios, c, hackrf, None
        # A difference a test makes on purpose is declared in the one registry (apart(), the owner, 2026-11-24: "NO
        # deviation"); offsets: {key: amount} declared there for this over's life (image.py: the listener 1500 Hz
        # below). pair_ignore names what the test keeps apart: each must be declared (apart) as the radio keys, else
        # the over stops
        self._own_apart = apart({k: abs(int(v)) for k, v in offsets.items()}, f"{label}: declared by its Watch") \
            if offsets else None
        if self._own_apart:
            self._own_apart.__enter__()
        self.offsets = {k: abs(int(v)) for k, v in (offsets or {}).items()}
        # Every over of one radio to another (the owner, 2026-11-23: "when you TX on one radio screenshot them BOTH,
        # you should see the RX happening! and they should have the same settings. IF they don't something is
        # wrong"): as a radio goes on the air the others' receiver settings are compared with its own (PAIR_KEYS
        # less pair_ignore, what a test changes on purpose) and a difference stops everything; both screens are
        # photographed while it is on the air (shots_during), their paths in self.shots after close()
        self.label, self.pair_ignore = label, set(pair_ignore)
        self._shot_threads, self._shot_seen, self.shots = [], {}, []
        # the pictures each over starts: off for a test whose radio decodes its own keying (the CW Trainer's sidetone:
        # a screen grab during it made the decoder misread the group and the answer, 2026-11-26)
        self.shots_on_tx = True
        self.expect_dbm, self.listener, self.w = expect_dbm, listener, w
        self.tx_peak = {}       # each radio's own forward-power meter, its highest while on the air (W)
        self.over_power = {}      # the radios that went over the power asked, and their highest reading (W)
        # the rating of the pad on each radio's jack (the radios profile), the most a reading may be: the watch's stop
        pads = c.get("profiles", {}).get("radios", {}).get("pads", [])
        self.pad_w = {x["at"]: float(x.get("watts", HARD_MAX_W)) for x in pads if x.get("at")}
        self.rx_peak = None     # the listener's S-meter, its highest while another radio is on the air (dBm)
        # Each over the listener did not hear (2026-11-22: hours of tests on a listener that heard nothing, nothing
        # said): its spectrum around the sender's TX frequency never rose DEAF_DB over its quiet from before the over
        # while the sender's meter showed power. Said and kept here; a test treats its run as failed, it does not stop
        self.deaf, self.heard_db, self._ov = [], {}, {}
        self._pad_frame, self._pad_n, self._pad_said = {}, {}, {}   # TX meter frames over the pad's rating in a row
        self._no_spec_said = False
        self._stall_said = {}             # a radio's stall said once: its last word then
        self.on = True
        threading.Thread(target=self._run, daemon=True).start()

    def _hearing(self, r, since, txm):
        """The listener's ear on r's over (called each pass while it runs): its spectrum around r's TX frequency now,
        against before the over; at the over's end, one where it never rose DEAF_DB over DEAF_MIN_POWERED passes with
        power on r's meter goes in self.deaf. No spectrum from the listener: said once, not counted as deaf"""
        lis = self.listener
        st = self._ov.get(r.name)
        hz = int(r.state.get("txf") or r.state.get("freq") or 0)
        if since:
            if st is None or st["since"] != since:
                q, _ = spectrum_level(lis, hz, since - 2.5, since - 0.2)
                st = self._ov[r.name] = {"since": since, "quiet": q, "powered": 0, "best": None, "frames": 0}
            if time.time() - since > 1.2 and txm and txm[0] >= DEAF_MIN_W:
                st["powered"] += 1
                lvl, n = spectrum_level(lis, hz, time.time() - 0.6, time.time())
                st["frames"] += n
                if lvl is not None and st["quiet"] is not None:
                    st["best"] = lvl - st["quiet"] if st["best"] is None else max(st["best"], lvl - st["quiet"])
        elif st is not None:
            self._judge(r.name, st, hz)

    def unheard(self, r, wait=1.5):
        """r's last over, judged (the Watch's thread judges it a pass after r leaves the air: waited for here, else
        judged now; a test asking right after the unkey raced it, 2026-11-22): its entry in self.deaf if the listener
        did not hear it, else None"""
        t = time.time()
        while time.time() - t < wait:
            st = self._ov.get(r.name)
            if st is None or st.get("done"):
                break
            time.sleep(0.05)
        st = self._ov.get(r.name)
        if st is not None and not st.get("done"):
            self._judge(r.name, st, int(r.state.get("txf") or r.state.get("freq") or 0))
        if st is None:
            return None
        return next((d for d in reversed(self.deaf) if d["sender"] == r.name and d.get("since") == st["since"]), None)

    def _judge(self, name, st, hz):
        """An over ended (or the Watch closed under it): heard or not, once"""
        if st.get("done"):
            return
        st["done"] = True
        lis = self.listener
        if st["powered"] < DEAF_MIN_POWERED:
            return
        if not st["frames"] or st["quiet"] is None:
            if not self._no_spec_said:
                self._no_spec_said = True
                say(f"{lis.name} sends no spectrum around {hz} Hz: whether it hears {name} is not checked")
            return
        self.heard_db.setdefault(name, []).append(round(st["best"], 1))
        if st["best"] < DEAF_DB:
            d = {"sender": name, "listener": lis.name, "hz": hz, "best_rise_db": round(st["best"], 1),
                 "at": time.strftime("%H:%M:%S"), "since": st["since"]}
            self.deaf.append(d)
            say(f"{lis.name} did NOT hear {name}'s over at {hz} Hz: its spectrum rose {d['best_rise_db']} dB at "
                f"most (a heard over: {DEAF_DB} and more). Nothing it measured of this over counts")

    def _run(self):
        while self.on and not self.why:
            for r in self.radios:
                quiet = time.time() - r.last_msg
                if quiet > stale_limit(r, self.listener) or not r.r.alive:
                    self.trip(f"no word from {r.name} for {quiet:.1f} s: it cannot be watched "
                              f"(its link to the harness stalled or dropped)")
                elif quiet > STALE_S and self._stall_said.get(r.name) != r.last_msg:
                    self._stall_said[r.name] = r.last_msg
                    say(f"{r.name}: no word for {quiet:.1f} s, off the air and unable to key unseen (TX locked, or the "
                        f"listener in {r.state.get('app') or r.state.get('win')}): the over goes on, stopped at "
                        f"{STALE_HELD_S:.0f} s")
            if self.hackrf is None and sum(1 for r in self.radios if r.tx) > 1:
                # the owner, 2026-11-19: "won't that damage the radios? don't do that again" (at 0.1 W through 70 dB
                # each hears the other at -50 dBm, no harm; never all the same): one on the air at a time
                self.trip("two radios on the air at once: " + ", ".join(r.name for r in self.radios if r.tx))
            for r in self.radios:
                with r.lock:
                    since, txm, meter = r.tx_since, r.txm, (self.listener.meter if self.listener else None)
                if since and self.hackrf is None and len(self.radios) > 1 and self._shot_seen.get(r.name) != since:
                    self._shot_seen[r.name] = since
                    for o in self.radios:
                        diff = {k: v for k, v in pair_diff(r, o).items() if not declared(k, *v)} if o is not r else {}
                        undeclared = sorted(k for k in diff if k in self.pair_ignore)
                        if diff:
                            self.trip(f"{r.name} went on the air not set as {o.name}: " +
                                      ", ".join(f"{k} {x}/{y}" for k, (x, y) in diff.items()) +
                                      (f" (kept apart by the test but never declared: {', '.join(undeclared)}: "
                                       f"bench.apart)" if undeclared else ""))
                    if self.shots_on_tx:
                        self._shot_threads.append((r.name, since, shots_during(self.radios,
                                                                               f"{self.label} {r.name} TX", r)))
                if self.hackrf is not None and r.tx:
                    self.trip(f"{r.name} went on the air while the HackRF fed it")
                elif self.hackrf is not None and (not r.ctl_mine or r.state.get("rxo") != 1):
                    self.trip(f"{r.name}: control taken from the harness or the receive-only hold gone while the "
                              f"HackRF fed it")
                elif since and time.time() - since > (FULL_RATING_KEY_S if (self.w or 0) > HARD_MAX_W else
                                                      self.c["limits"]["max_key_s"]):
                    self.trip(f"{r.name} on the air for over "
                              f"{FULL_RATING_KEY_S if (self.w or 0) > HARD_MAX_W else self.c['limits']['max_key_s']:.0f} s")
                elif since and txm and time.time() - since > 1.0:
                    w, swr = txm[0], txm[1]
                    pad = self.pad_w.get(r.name, HARD_MAX_W)
                    # over the pad's rating: two TX meter frames in a row. One is said and logged: the X6100's meter
                    # sent one 18.5 W frame as a 0.1 W over ended, its listener hearing -86 dBm, 0.1 W's (air comp,
                    # 2026-11-24); a real overload holds, and the next frame (a tenth of a second) stops it
                    if self._pad_frame.get(r.name) is not txm:
                        self._pad_frame[r.name] = txm
                        self._pad_n[r.name] = self._pad_n.get(r.name, 0) + 1 if w > pad else 0
                    if swr > MAX_SWR:
                        self.trip(f"{r.name}: SWR {swr:.2f} on the air: the chain is not a load")
                    elif self._pad_n.get(r.name, 0) >= 2:
                        self.trip(f"{r.name}: {w:.1f} W on the air, over the {pad:g} W its pad takes (two TX meter "
                                  f"frames in a row)")
                    elif w > pad:
                        if self._pad_said.get(r.name) is not txm:
                            self._pad_said[r.name] = txm
                            say(f"{r.name}: one TX meter frame of {w:.1f} W, over the {pad:g} W its pad takes: logged, "
                                f"stopped if the next says so too")
                            tx_log({"who": r.name, "one_frame_over_pad_w": w, "asked_w": self.w})
                    elif self.w is not None and w > max(self.w * 2, self.w + 0.5):
                        # the owner, 2026-11-21: "don't kill just log. we have attenuators" (the X6100's factory base
                        # starts a TCI over at 0.9-1.0 W for a 0.1 W setting, and the stop hid what came after it).
                        # Under the pad's rating: said once an over, its highest kept and logged, the over goes on
                        if r.name not in self.over_power:
                            say(f"{r.name}: {w:.1f} W on the air, {self.w} W asked: logged, the over goes on "
                                f"(its pad takes {self.pad_w.get(r.name, HARD_MAX_W):g} W)")
                        if w > self.over_power.get(r.name, 0.0):
                            self.over_power[r.name] = w
                            tx_log({"who": r.name, "over_power_w": w, "asked_w": self.w})
                if since and txm and time.time() - since > 1.0:
                    self.tx_peak[r.name] = max(self.tx_peak.get(r.name, 0.0), txm[0])
                if since and meter is not None and r is not self.listener and time.time() - since > 1.0:
                    self.rx_peak = meter if self.rx_peak is None else max(self.rx_peak, meter)
                if since and self.expect_dbm is not None and meter is not None and \
                        time.time() - since > 1.0 and meter > self.expect_dbm + TOO_STRONG_DB:
                    self.trip(f"{self.listener.name} hears {meter:.0f} dBm, the chain should let through about "
                              f"{self.expect_dbm:.0f}: pads missing?")
                if self.hackrf is None and self.listener is not None and r is not self.listener:
                    self._hearing(r, since, txm)
            time.sleep(0.05)

    def trip(self, why):
        if self.why:
            return
        self.why = why
        say("STOP:", why)
        if self.hackrf is not None and self.hackrf.poll() is None:
            self.hackrf.kill()
        for r in self.radios:
            r.stop()
        tx_log({"stop": why})

    def _declared(self, k, v):
        """The difference v (a's, b's) of key k is declared (the one registry)"""
        return declared(k, *v)

    def close(self):
        """The Watch ended: an over still being followed is judged now (a test closes it as its sender unkeys)"""
        if getattr(self, "_own_apart", None):
            self._own_apart.__exit__(None, None, None)
            self._own_apart = None
        self.on = False
        time.sleep(0.06)                    # its last pass
        if self.listener is not None:
            for r in self.radios:
                st = self._ov.get(r.name)
                if st is not None and not st.get("done"):
                    self._judge(r.name, st, int(r.state.get("txf") or r.state.get("freq") or 0))
        for name, since, th in self._shot_threads:
            th.join(90)
            self.shots.append({"sender": name, "since": since, "paths": th.paths, "on_air": th.on_air})
            if th.paths:
                say(f"over of {name}: both screens {'on the air' if th.on_air else 'NOT caught on the air'}: "
                    + ", ".join(os.path.basename(p) for p in th.paths.values() if p))
        self._shot_threads = []


def install_stop_signals(get_conf):
    """Ctrl-C or SIGTERM: the emergency stop at once, then KeyboardInterrupt, so the test's own finish() still puts the
    radios back (the audit, 2026-11-22: os._exit skipped every finally, and suite.py's timeout killed its children);
    a second one leaves at once"""
    hits = [0]

    def on_signal(*_):
        hits[0] += 1
        if hits[0] > 1:
            say("interrupted again: out without putting the radios back")
            os._exit(130)
        say("interrupted: the emergency stop, then the radios put back (again to leave at once)")
        emergency_stop(get_conf() or {})
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)


def emergency_stop(c):
    subprocess.run(["pkill", "-f", "hackrf_transfer"], capture_output=True)
    toks = load_json(os.path.join(STATE, "tokens.json"), {})
    for name, rc in c.get("radios", {}).items():
        for kind in ("tx", "rx"):
            if toks.get(name, {}).get(kind):
                try:
                    r = Radio(name, rc, kind == "tx")
                    try:
                        r.take_control()
                    except Refused as e:            # the operator has it: stopped all the same (operate is enough)
                        say(f"{name}: {e}; stopping without it")
                    r.stop()
                    time.sleep(0.5)
                    say(f"{name}: stopped ({'on the air' if r.tx else 'not on the air'})")
                    r.close()
                    break
                except (Refused, OSError) as e:
                    say(f"{name}: {e}")


# ---- Signals for the HackRF ----

def gen_ft8_wav(c, text, audio_hz, path):
    gen = c.get("gen_ft8") or "gen_ft8"
    r = subprocess.run([gen, text, path, str(audio_hz)], capture_output=True, text=True)
    if r.returncode or not os.path.exists(path):
        raise Refused(f"gen_ft8: {(r.stderr or r.stdout).strip()[-200:]}")


def audio_to_iq(wav_path, iq_path, fs, offset, lead_s):
    """A 12 kHz audio file as upper sideband: analytic, upsampled to fs (an integer multiple), moved up by offset Hz,
    into signed 8-bit I/Q for hackrf_transfer; lead_s of silence first. In one-second pieces (a 15 s file is 30
    million samples)"""
    import numpy as np
    with wave.open(wav_path) as w:
        rate = w.getframerate()
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    up = fs // rate
    if up * rate != fs:
        raise ValueError("fs must be a multiple of the audio rate")
    a = np.concatenate([np.zeros(int(lead_s * rate)), a])
    n = len(a)
    h = np.zeros(n)
    h[0] = 1
    h[1:(n + 1) // 2] = 2
    if n % 2 == 0:
        h[n // 2] = 1
    z = np.fft.ifft(np.fft.fft(a) * h)
    z /= max(np.abs(z).max(), 1e-9)
    src = np.arange(n)
    with open(iq_path, "wb") as f:
        total = n * up
        for s0 in range(0, total, fs):
            k = np.arange(s0, min(s0 + fs, total))
            x = (np.interp(k / up, src, z.real) + 1j * np.interp(k / up, src, z.imag)) * \
                np.exp(2j * np.pi * offset * k / fs) * 90.0
            out = np.empty(2 * len(k), dtype=np.int8)
            out[0::2] = np.clip(np.round(x.real), -127, 127)
            out[1::2] = np.clip(np.round(x.imag), -127, 127)
            f.write(out.tobytes())
    return n * up / fs


def next_slot(period=15.0, lead=0.25, margin=3.0):
    """The wall-clock time to start at: the next FT8 slot at least margin seconds away, minus the HackRF's lead"""
    now = time.time()
    t = math.ceil((now + margin) / period) * period
    return t - lead


def norm(s):
    return " ".join((s or "").upper().split())


def msg(row):
    """A decoded row's message: the FT8 list shows it after its slot's time ("07:29:00 CQ Q9TST FN00")"""
    return re.sub(r"^\d\d:\d\d:\d\d ", "", norm(row.get("x")))


# ---- Tests ----

def run_hackrf_ft8(c, args):
    p, total = chain_gate(c, "hackrf")
    hackrf_gate(args.x)
    name = p["to"]
    radio = Radio(name, c["radios"][name], want_tx=False)
    rx_only_gate(radio)
    dial = FT8_DIAL[args.band]
    text = norm(args.text)
    os.makedirs(os.path.join(STATE, "runs"), exist_ok=True)
    run = tempfile.mkdtemp(prefix="ft8-", dir=os.path.join(STATE, "runs"))
    wav, iq = os.path.join(run, "ft8.wav"), os.path.join(run, "ft8.s8")
    gen_ft8_wav(c, text, args.audio, wav)
    fs, offset = 2004000, 100000
    secs = audio_to_iq(wav, iq, fs, offset, lead_s=0.25)
    radio.take_control()
    snaps, report = {name: snapshot(radio)}, {}
    radio.send(f"banner BENCH HackRF into {name.upper()} through the pads, {name.upper()} RECEIVES ONLY: FT8 "
               f"'{text}' on {args.band}")
    found, watch, hk = None, None, None
    try:
        report["supply"] = listener_gate(radio)
        radio.send(f"set freq {dial}")      # first: the app holds the dial once open
        radio.send("ft8 open")
        radio.wait("the FT8 app", lambda: radio.ft8.get("open") == 1, 10)
        rx_only_gate(radio)
        ft8_to_band(radio, dial)
        radio.wait(f"the dial on {dial}", lambda: int(radio.state.get("freq", 0)) == dial, 5)
        # its receiver said whole, after the band change (the band's own PRE, ATT and AGC came with it)
        report["rx"] = ensure(radio, *rx_profile(dial, 3, pre=args.pre, att=args.att, rfg=100, agc=3))
        say(f"{name}: receiver {report['rx']}")
        start = next_slot()
        say(f"{name}: FT8 on {dial} Hz, receive only; the HackRF sends {text!r} at {args.audio} Hz, -x {args.x}, "
            f"{total} dB of pads, at {time.strftime('%H:%M:%S', time.localtime(start + 0.25))}")
        while time.time() < start:
            rx_only_gate(radio)
            time.sleep(0.05)
        rows0 = len(radio.rows)
        cmd = ["hackrf_transfer", "-t", iq, "-f", str(dial - offset), "-s", str(fs), "-a", "0", "-p", "0",
               "-x", str(args.x)]
        hk = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        tx_log({"who": "hackrf", "to": name, "f": dial - offset, "x": args.x, "text": text, "secs": round(secs, 1)})
        watch = Watch([radio], c, hackrf=hk)
        quiet = radio.meter
        peak = -999.0
        t_end = time.time() + secs + 10
        while hk.poll() is None and time.time() < t_end and not watch.why:
            if radio.meter is not None:
                peak = max(peak, radio.meter)
            time.sleep(0.05)
        if hk.poll() is None and not watch.why:
            watch.trip("hackrf_transfer ran past its file")
        hk_said = (hk.stderr.read() if hk.stderr else "").strip().splitlines()
        say(f"{name}: S-meter {quiet} dBm before, {peak:.1f} at most while the HackRF sent; hackrf_transfer: "
            f"{hk_said[-1] if hk_said else '-'} (exit {hk.returncode})")
        end = time.time() + 6
        while time.time() < end and not watch.why and not found:
            with radio.lock:
                found = next((r for r in radio.rows[rows0:] if msg(r) == text), None)
            time.sleep(0.1)
        others = [r.get("x") for r in radio.rows[rows0:]]
    finally:
        if watch:
            watch.close()
        if hk and hk.poll() is None:
            hk.kill()
        radio.send("ft8 close")             # the app opened for the test closed, refused or not (the X6200: the Sentry)
        time.sleep(0.5)
        finish([radio], snaps, report=report)       # its dial (never put back before) and bands back, on page A
    if report.get("not_restored"):
        raise Refused(f"NOT put back: {report['not_restored']}")
    if watch and watch.why:
        raise Refused(watch.why)
    if not found:
        raise Refused(f"{name} did not decode {text!r}" + (f" (it decoded {others})" if others else " (nothing)"))
    say(f"PASS: {name} decoded {found['x']!r}, SNR {found['snr']} dB, {found['f']} Hz")


def run_hackrf_iq(c, args):
    """A signal made elsewhere (int8 I/Q at args.fs, the signal args.offset Hz above the HackRF's centre) into the
    radio tuned to args.at in args.mode (7 NFM): the radio's dial, mode and squelch put back after. What the radio
    makes of it is looked at by the caller (args.during: a command run beside it, its output said)"""
    p, total = chain_gate(c, "hackrf")
    hackrf_gate(args.x)
    name = p["to"]
    radio = Radio(name, c["radios"][name], want_tx=False)
    rx_only_gate(radio)
    secs = os.path.getsize(args.file) / 2 / args.fs
    radio.take_control()
    radio.send(f"banner BENCH HackRF into {name.upper()} through the pads, {name.upper()} RECEIVES ONLY: "
               f"{os.path.basename(args.file)} at {args.at} Hz")
    snaps, report, before = {name: snapshot(radio)}, {}, {name: []}
    hk, watch, during = None, None, None
    try:
        report["supply"] = listener_gate(radio)
        # the whole receiver said (the dial first: a new band brings its own mode, PRE, ATT and AGC), each read back;
        # the mode's filter too when asked, its own edges put back first by finish()
        report["rx"] = ensure(radio, *rx_profile(args.at, args.mode, pre=args.pre, att=args.att, rfg=100, agc=3))
        if args.filter:
            before[name].append({"freq": args.at, "mode": args.mode, "filter": _value(radio, "filter")})
            report["rx"].update(ensure(radio, {"filter": tuple(args.filter)}))
        say(f"{name}: receiver {report['rx']}")
        rx_only_gate(radio)
        if args.during:
            during = subprocess.Popen(args.during, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True)
            time.sleep(args.lead)
        quiet = radio.meter
        cmd = ["hackrf_transfer", "-t", args.file, "-f", str(args.at - args.offset), "-s", str(args.fs), "-a", "0",
               "-p", "0", "-x", str(args.x)]
        say(f"{name}: {args.at} Hz mode {args.mode}, receive only; the HackRF sends {os.path.basename(args.file)} "
            f"({secs:.1f} s), -x {args.x}, {total} dB of pads")
        hk = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        tx_log({"who": "hackrf", "to": name, "f": args.at - args.offset, "x": args.x,
                "file": os.path.basename(args.file), "secs": round(secs, 1)})
        watch = Watch([radio], c, hackrf=hk)
        peak = -999.0
        t_end = time.time() + secs + 10
        while hk.poll() is None and time.time() < t_end and not watch.why:
            if radio.meter is not None:
                peak = max(peak, radio.meter)
            time.sleep(0.05)
        if hk.poll() is None and not watch.why:
            watch.trip("hackrf_transfer ran past its file")
        say(f"{name}: S-meter {quiet} dBm before, {peak:.1f} at most while the HackRF sent (exit {hk.returncode})")
    finally:
        if watch:
            watch.close()
        if hk and hk.poll() is None:
            hk.kill()
        finish([radio], snaps, report=report, before=before)
    if report.get("not_restored"):
        raise Refused(f"NOT put back: {report['not_restored']}")
    if during:
        out, _ = during.communicate(timeout=120)
        say("beside it:\n" + out.rstrip())
    if watch and watch.why:
        raise Refused(watch.why)


def ft8_to_band(r, dial):
    """The FT8 app opens on the band it had last and holds the dial there ("freq: locked"): the band keys move it,
    as at the radio (found 2026-11-18, the first run on two radios: both opened on 20 m, 7.074 MHz refused)"""
    for _ in range(12):
        f = int(r.state.get("freq", 0))
        if f == dial:
            return
        k = "BAND_DOWN" if f > dial else "BAND_UP"
        r.send(f"key {k} 1")
        time.sleep(0.15)
        r.send(f"key {k} 0")
        r.wait("the next band", lambda f=f: int(r.state.get("freq", 0)) != f, 5)
        time.sleep(0.3)
    raise Refused(f"{r.name}: the FT8 app did not come to {dial} Hz (on {r.state.get('freq')})")


def run_radios_ft8(c, args):
    """Untried until both radios are on the bench: A calls CQ once, B must decode it and reply once, A must decode
    the reply. The first transmission of the session at FIRST_PROBE_W whatever was asked"""
    p, total = chain_gate(c, "radios")
    for n in (args.tx, args.rx):
        if n not in p.get("between", []):
            raise Refused(f"{n} is not on the radios profile's cable")
    a, b = Radio(args.tx, c["radios"][args.tx], True), Radio(args.rx, c["radios"][args.rx], True)
    calls = {}
    for r in (a, b):
        call = ssh(r.host, "sqlite3 /mnt/params.db \"select val from params where name='callsign'\"").strip()
        if not TEST_CALL.match(call):
            raise Refused(f"{r.name}: its callsign is {call or 'none'}: a test call first (bench.py testcall "
                          f"{r.name} Q1TST)")
        calls[r.name] = call
    dial = FT8_DIAL[args.band]
    radios = [a, b]
    snaps, report = {}, {}
    for r in radios:
        r.take_control()
        snaps[r.name] = snapshot(r)        # put back by finish() (the first run left both at 0.1 W, 2026-11-18)
    result = {}
    try:
        for r in radios:
            report.setdefault("supply", {})[r.name] = listener_gate(r)     # each listens in turn
            # Both receivers alike, always (they had been only with --rx-alike: the first run had the X6200's preamp
            # on and the X6100's off, 13-18 dB apart): preamp and attenuator off, RF gain at its top (below 100 the
            # X6100's base switches in its stepped attenuator, 100 - rfg steps, strijar/x6100-base docs/protocol.md;
            # it had run at 63, its default), AGC auto as the FT8 app runs. The dial first: the app holds it once open
            report.setdefault("rx", {})[r.name] = ensure(r, *rx_profile(dial, 3, pre=0, att=0, rfg=100, agc=3))
        path_proof(c, total, a, b, dial, report, label=f"ft8 path proof {a.name} to {b.name}")
        report["path_proof_" + a.name] = report["path_proof"]
        path_proof(c, total, b, a, dial, report, label=f"ft8 path proof {b.name} to {a.name}")
        ensure(a, *rx_profile(dial, 3, pre=0, att=0, rfg=100, agc=3))      # the proof left it in its sender's state
    except Refused:
        finish(radios, snaps, report=report)
        raise
    for r in radios:
        r.send("ft8 open")
        r.wait("the FT8 app", lambda r=r: r.ft8.get("open") == 1, 10)
        ft8_to_band(r, dial)
        if r.ft8.get("auto"):
            r.send("ft8 auto")
            r.wait("Auto off", lambda r=r: not r.ft8.get("auto"), 5)
        r.send(f"set pwr {round(FIRST_PROBE_W * 10)}")
        r.wait("the dial and the power", lambda r=r: int(r.state.get("freq", 0)) == dial and
               abs(r.pwr() - FIRST_PROBE_W) < 0.05, 5)
        radio_tx_gate(r, c, FIRST_PROBE_W)
    expect = 10 * math.log10(FIRST_PROBE_W * 1000) - total
    watch = Watch(radios, c, expect_dbm=expect, listener=b, w=FIRST_PROBE_W, label=f"ft8 cq {a.name} to {b.name}")
    try:
        def one_tx(sender, listener, start_cmd, want, what):
            n0 = len(listener.rows)
            line = (f"BENCH FT8 radio to radio, {args.band}, {FIRST_PROBE_W} W through {total} dB: "
                    f"{sender.name.upper()} SENDS {what.upper()}, {listener.name.upper()} LISTENS")
            for r in (sender, listener):                # the line across both screens (bench_banner.c)
                r.send(f"banner {line}")
            quiet = listener.meter
            watch.tx_peak.pop(sender.name, None)
            watch.rx_peak = None
            sender.send(start_cmd)
            tx_log({"who": sender.name, "f": dial, "w": sender.pwr(), "what": what})
            sender.wait("its transmission", lambda: sender.tx or watch.why, 32)
            sender.wait("the end of its transmission", lambda: not sender.tx or watch.why, MAX_KEY_S + 2)
            sender.send("ft8 stop")
            # the levels, to tell a weak transmitter from a deaf receiver (TODO 12: the FT8 SNRs 14 dB apart)
            say(f"{sender.name} sent {watch.tx_peak.get(sender.name, 0):.2f} W by its own meter; {listener.name}'s "
                f"S-meter {quiet} dBm before, {watch.rx_peak} dBm at most while it sent (the chain: about "
                f"{expect:.0f})")
            d = watch.unheard(sender)
            if d:
                raise Refused(f"{listener.name} did not hear {sender.name}'s {what} (its spectrum rose "
                              f"{d['best_rise_db']} dB at most): nothing decoded or not proves nothing")
            try:
                listener.wait(f"{what} decoded", lambda: watch.why or any(want(r) for r in listener.rows[n0:]), 8)
            except Refused:
                say(f"{listener.name} decoded meanwhile: " + ("; ".join(repr(msg(r)) for r in listener.rows[n0:])
                                                             or "nothing"))
                raise
            if watch.why:
                raise Refused(watch.why)
            return next(r for r in listener.rows[n0:] if want(r))

        ca, cb = calls[a.name], calls[b.name]
        # A test call (Q<digit>TST) is a nonstandard call, as WSJT-X has it (ft8lib 0002): a call in a message is
        # then sent whole or as its hash, which reads "<Q1TST>" where the call has been heard
        def calls_are(r, first, second):
            w = msg(r).split()
            return len(w) >= 2 and w[0].strip("<>") == first and w[1].strip("<>") == second
        row = one_tx(a, b, "ft8 cq", lambda r: calls_are(r, "CQ", ca), "CQ")
        result["cq"] = row
        say(f"{b.name} decoded {row['x']!r}, SNR {row['snr']} dB")
        watch.close()                   # the CQ's over judged; a new watch with A listening
        watch = Watch(radios, c, expect_dbm=expect, listener=a, w=FIRST_PROBE_W,
                      label=f"ft8 reply {b.name} to {a.name}")
        row = one_tx(b, a, f"ft8 reply {row['id']}", lambda r: calls_are(r, ca, cb), "reply")
        result["reply"] = row
        say(f"{a.name} decoded {row['x']!r}, SNR {row['snr']} dB")
    finally:
        watch.close()
        for r in radios:
            r.stop()
        time.sleep(1)
        for r in radios:
            r.send("banner")
            r.send("ft8 close")
            try:        # the app gives the dial back as it closes: ours after it (it took the X6100 to 14.074, 11-19)
                r.wait("the FT8 app closed", lambda r=r: r.ft8.get("open") != 1, 5)
            except Refused:
                pass
        time.sleep(0.5)
        finish(radios, snaps, report=report)        # put back, checked, both on the Sentry's page A
        for r in radios:
            clean_log(r.host)
    if report.get("not_restored"):
        raise Refused(f"NOT put back: {report['not_restored']}")
    if watch.why:
        raise Refused(watch.why)
    say(f"PASS: {a.name} -> {b.name} -> {a.name} at {FIRST_PROBE_W} W through {total} dB")


# G's channels on the amateur bands (its band groups' channels, app_sentry.c bands[] and w_plan; 30 m's is WWV on
# 10.000, outside the band: its channel here in it), and the FM calling channels of 10 m and 6 m
WALKIE_BANDS = [("160m", 1900000), ("80m", 3850000), ("60m", 5357000), ("40m", 7150000), ("30m", 10120000),
                ("20m", 14200000), ("17m", 18120000), ("15m", 21300000), ("12m", 24940000), ("10m", 28400000),
                ("10m-fm", 29600000), ("6m", 50125000), ("6m-fm", 52525000)]
WALKIE_MODES = {"am": 6, "fm": 7}       # radio_mode_t: the modes with a carrier (SSB without a voice sends nothing)
WALKIE_KEY_S = 4.0                      # each over, on the air
WALKIE_KEY_WAIT_S = 8.0                 # for the radio to come on the air after PTT
WALKIE_RISE_DB = 15                     # the listener's spectrum at the dial over its quiet level: heard
WALKIE_SPAN_HZ = 3000                   # how far from the dial its spectrum is looked at
WALKIE_UNHEARD_STOP = 3                 # overs in a row unheard: the test stops


def spectrum_peak(r, hz, t0, t1, span=WALKIE_SPAN_HZ):
    """The strongest bin within span of hz in r's spectrum frames between t0 and t1 (dB), None without frames"""
    best = None
    for t, cen, wid, v in list(r.r.spectra):
        if not t0 <= t <= t1 or not v:
            continue
        per = wid / len(v)
        lo, hi = int((hz - span - (cen - wid / 2)) / per), int((hz + span - (cen - wid / 2)) / per)
        for i in range(max(lo, 0), min(hi + 1, len(v))):
            best = v[i] if best is None else max(best, v[i])
    return best


def run_radios_walkie(c, args):
    """G (Walkie) on both radios (the owner, 2026-11-19: "BOTH radios should be able to TX & RX to each other across
    like any band selected ... tested across the wire + PAD setup"): on each band's channel and each mode, A armed
    with A/B and keyed with PTT for WALKIE_KEY_S while B listens on G, then the other way round. Heard: B's S-meter
    WALKIE_RISE_DB over its level before. At FIRST_PROBE_W throughout, the radios profile's pads, every gate and the
    watch as radios-ft8's; CB and the other services' channels G offers are never keyed (outside the amateur bands:
    the harness's gate and the radios' TX ranges refuse them)"""
    p, total = chain_gate(c, "radios")
    for n in (args.a, args.b):
        if n not in p.get("between", []):
            raise Refused(f"{n} is not on the radios profile's cable")
    bands = [x for x in WALKIE_BANDS if not args.bands or x[0] in args.bands.split(",")]
    modes = [m for m in (args.modes or "am,fm").split(",") if m in WALKIE_MODES]
    if not bands or not modes:
        raise Refused("no band or mode to try")
    a, b = Radio(args.a, c["radios"][args.a], True), Radio(args.b, c["radios"][args.b], True)
    radios = [a, b]
    snaps, report = {}, {}
    for r in radios:
        r.take_control()
        snaps[r.name] = snapshot(r)
    # each radio put back as found, on the Sentry's page A: the owner's later rule (2026-11-22, "NEVER leave a radio
    # not as found") over 2026-11-19's both on the first radio's channel after
    expect = 10 * math.log10(FIRST_PROBE_W * 1000) - total
    results, hold = [], [int(time.time()) % 100000]
    before = {}             # each radio's own memory of a mode (filter, zoom) before the other's was copied over it
    try:
        for r in radios:
            report.setdefault("supply", {})[r.name] = listener_gate(r)
            open_page(r, "G")
        # before any over: each hears the other at the first channel (USB-D tone, 0.1 W)
        hz0 = bands[0][1]
        for s_, l_ in ((a, b), (b, a)):
            ensure(l_, *rx_profile(hz0, 2, pre=0, att=0, rfg=100, agc=3))
            path_proof(c, total, s_, l_, hz0, report, label=f"walkie path proof {s_.name} to {l_.name}")
            report[f"path_proof_{s_.name}"] = report.pop("path_proof")
    except Refused:
        finish(radios, snaps, report=report)
        raise
    watch = Watch(radios, c, expect_dbm=expect, listener=b, w=FIRST_PROBE_W, label="walkie")

    def caught_up(r, wait=10):
        """r's link to this computer current again: a message within the last half second, for a second"""
        t, fresh = time.time(), None
        while time.time() - t < wait:
            if time.time() - r.last_msg < 0.5:
                fresh = fresh or time.time()
                if time.time() - fresh >= 1.0:
                    return
            else:
                fresh = None
            time.sleep(0.1)

    def on_g(r):
        """r on G, the Walkie: there the Sentry's scans leave the dial on the channel (sentry_step); on any other page
        A's routine retunes it (the owner, 2026-11-19: "why aren't BOTH radios on the walkie screen?")"""
        if r.state.get("pg") != "G":
            r.send("open sentry G")
            r.wait("G, the Walkie", lambda r=r: r.state.get("pg") == "G", 6)

    def over(sender, listener, band, hz, mode):
        for r in radios:
            on_g(r)
        # the listener's receiver the same on every band (it had kept each band's own PRE, ATT and AGC, unrecorded),
        # the sender sending where it is tuned; each band's own memories put back by finish()
        ensure(listener, *rx_profile(hz, WALKIE_MODES[mode], pre=0, att=0, rfg=100, agc=3))
        # both alike for the over (the owner, 2026-11-23: "they should have the same settings. IF they don't something
        # is wrong"): the listener set first (above), then the sender given exactly its set-up (pair_align: its filter,
        # zoom, PRE, ATT, AGC, RF gain...), then its TX set-up, the two compared. The sender's own memory of the mode
        # (filter and zoom: each mode's, which the snapshot does not hold) kept first for finish's before
        m = WALKIE_MODES[mode]
        ensure(sender, {"freq": hz, "mode": m})
        mem = before.setdefault(sender.name, [])
        if not any(x.get("mode") == m for x in mem):
            mem.append({"freq": hz, "mode": m, "filter": _value(sender, "filter"), "zoom": _value(sender, "zoom")})
        pair_align(sender, listener, hz, m)
        ensure(sender, *tx_profile(hz, m, FIRST_PROBE_W))
        if pair_diff(sender, listener):
            raise Refused(f"walkie {band} {mode}: {sender.name} and {listener.name} not alike after the sender's TX "
                          f"set-up: {pair_diff(sender, listener)}")
        watch.label = f"walkie {band} {mode} {sender.name} to {listener.name}"     # its screenshots' name
        line = (f"BENCH WALKIE (G) radio to radio, {band} {mode.upper()} {hz / 1e6:.3f} MHZ, {FIRST_PROBE_W} W through "
                f"{total} DB: {sender.name.upper()} SENDS, {listener.name.upper()} LISTENS")
        for r in radios:
            r.send(f"banner {line}")
        radio_tx_gate(sender, c, FIRST_PROBE_W)
        watch.listener = listener
        caught_up(listener)                           # what it held back (WiFi) in, not counted as quiet
        q0 = time.time()
        time.sleep(1.5)                               # the listener settled on the channel
        quiet, sq = listener.meter, spectrum_peak(listener, hz, q0, time.time())
        watch.tx_peak.pop(sender.name, None)
        watch.rx_peak = None
        watch.over_power.pop(sender.name, None)
        listener.wait("the other radio out of TX for a second",
                      lambda: not listener.tx and time.time() - listener.tx_off_at >= 1.0, 10)
        n_shots = len(watch._shot_threads)          # this over's pictures: the Watch's threads from here
        arm(sender, True, mode=WALKIE_MODES[mode])
        hold[0] += 1
        n = hold[0]
        tx_log({"who": sender.name, "f": hz, "w": sender.pwr(), "what": f"walkie {mode}"})
        t0 = time.time()
        on_at = None
        ltx = []                                      # the listener's own TX flags and hold through the over
        gap = 0.0                                     # the longest the listener sent nothing at all, through it
        # the lease renewed each half second till the radio has been on the air WALKIE_KEY_S (the X6200 came on the air
        # after 4 s on 17 m once, 2026-11-19), WALKIE_KEY_WAIT_S at most for it to come
        while not watch.why and (on_at is None and time.time() - t0 < WALKIE_KEY_WAIT_S or
                                 on_at is not None and time.time() - on_at < WALKIE_KEY_S):
            if listener.tx:                                         # the other on the air: never key over it
                break
            sender.send(f"key PTT 1 {n}")
            if on_at is None and sender.tx:
                on_at = time.time()
            ltx.append((int(listener.tx), listener.state.get("tx"), listener.state.get("rxo")))
            gap = max(gap, time.time() - listener.last_msg)
            time.sleep(0.5)
        keyed = on_at is not None
        stayed = listener.state.get("pg") == "G" and int(listener.state.get("freq", 0)) == hz
        sender.send("key PTT 0")
        t_end = time.time()
        if gap >= 1.5:                                # its frames of the over come late, in a burst: waited for
            caught_up(listener, 10)
        sp = spectrum_peak(listener, hz, (on_at or t0) + 1.0, t_end if gap < 1.5 else time.time())
        try:
            sender.wait("the end of its over", lambda: not sender.tx or watch.why, 8)
        finally:
            if not args.keep_armed:
                arm(sender, False)
        if watch.why:
            raise Refused(watch.why)
        # both screens while it was on the air (the Watch's own pictures of this over, fetched now)
        pic = next((th for name, _, th in watch._shot_threads[n_shots:] if name == sender.name), None)
        if pic is not None:
            pic.join(90)
        peak, w = watch.rx_peak, watch.tx_peak.get(sender.name, 0.0)
        # heard: the listener's own spectrum at the dial up WALKIE_RISE_DB (its S-meter beside it: the X6200's did not
        # follow an AM signal, 2026-11-19)
        # no quiet reading (the listener's link silent before the over): the signal against what the chain lets through
        # (no quiet reading: not counted; the spectrum's bins are a display level, never dBm to set against the
        # chain's, the 2026-11-22 audit)
        spec_rise = keyed and sp is not None and sq is not None and sp - sq >= WALKIE_RISE_DB
        meter_rise = keyed and quiet is not None and peak is not None and peak - quiet >= WALKIE_RISE_DB
        # either is the listener's receiver (the X6100's own spike at the dial in AM raises its spectrum's quiet level;
        # the X6200's S-meter in AM stays)
        heard = stayed and (spec_rise or meter_rise)
        if sender.name in watch.over_power:
            say(f"FINDING {band} {mode}: {sender.name} sent {watch.over_power[sender.name]:.1f} W asked for "
                f"{FIRST_PROBE_W} W")
        mute = keyed and sp is None and peak == quiet  # nothing came from the listener while it was sent to
        if gap >= 2.0:
            say(f"{listener.name} sent nothing at all for {gap:.1f} s during the over (its link to this computer)")
        res = {"band": band, "hz": hz, "mode": mode, "from": sender.name, "to": listener.name, "keyed": keyed,
               "w": round(w, 2), "quiet": quiet, "peak": peak, "spec_quiet": sq, "spec_peak": sp, "heard": heard,
               "meter_rose": meter_rise, "spec_rose": spec_rise,
               "key_s": round(on_at - t0, 1) if on_at else None, "listener_stayed": stayed,
               "listener_tx": sorted(set(ltx)), "listener_silent": mute,
               "over_power_w": watch.over_power.get(sender.name), "listener_silent_s": round(gap, 1),
               "pictured_on_air": bool(pic and pic.on_air), "shots": dict(pic.paths) if pic else {}}
        results.append(res)
        where = f"page {listener.state.get('pg')}, {listener.state.get('freq')} Hz"
        say(f"{'PASS' if heard else 'FAIL'} {band} {mode} {sender.name} -> {listener.name}: keyed {keyed}"
            f"{f' after {on_at - t0:.1f} s' if on_at else ''}, "
            f"{w:.2f} W by its meter; {listener.name}'s spectrum at the dial {sq} -> {sp} dB, its S-meter {quiet} -> "
            f"{peak} dBm{'' if meter_rise else ' (the S-meter did not follow)'}"
            f"{'' if stayed else ' (the listener left G or the channel: ' + where + ')'}"
            f" (the chain: about {expect:.0f}); the listener's tx/hold {sorted(set(ltx))}; both screens "
            f"{'on the air' if res['pictured_on_air'] else 'NOT caught on the air'}: "
            + ", ".join(os.path.basename(x) for x in res["shots"].values() if x))

    try:
        unheard = 0
        for band, hz in bands:
            for mode in modes:
                for r in radios:
                    arm(r, False)                     # an armed radio holds its dial (dial_held): locked to retune
                for s_, l_ in ((a, b), (b, a)):
                    if args.senders and s_.name not in args.senders.split(","):
                        continue
                    over(s_, l_, band, hz, mode)
                    last = results[-1]
                    if not watch.why and (not last["keyed"] or last["listener_silent"]):
                        say(f"{s_.name} did not key on {band} {mode}: once more" if not last["keyed"] else
                            f"{l_.name} sent nothing (no spectrum, the meter still) while {s_.name} sent: once more")
                        results.pop()
                        over(s_, l_, band, hz, mode)
                    # overs in a row unheard: the path or a receiver is gone, not the band (it went through every
                    # band before, 2026-11-22 audit)
                    unheard = 0 if results[-1]["heard"] else unheard + 1
                    if unheard >= WALKIE_UNHEARD_STOP:
                        raise Refused(f"{unheard} overs in a row not heard: stopped (the path or a receiver, not the "
                                      f"bands; the path proof passed at the start)")
    finally:
        watch.close()
        report["deaf"] = watch.deaf
        report["shots"] = watch.shots
        # each band's memories, power, ATU back; each mode's memory the other's was copied over first; both on page A
        finish(radios, snaps, report=report, before=before)
        out = os.path.join(STATE, "runs", time.strftime("%Y%m%d-%H%M%S-walkie.json"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w") as f:
            json.dump({"overs": results, "report": report}, f, indent=1)
        say(f"the overs: {out}")
    if report.get("not_restored"):
        raise Refused(f"NOT put back: {report['not_restored']}")
    bad = [x for x in results if not x["heard"]]
    still = [x for x in results if x["heard"] and not x["meter_rose"]]
    if still:
        say("heard but the S-meter did not follow: " + ", ".join(f"{x['band']} {x['mode']} {x['from']}->{x['to']}"
                                                                 for x in still))
    say(f"{len(results) - len(bad)} of {len(results)} overs heard" + (f"; not: " + ", ".join(
        f"{x['band']} {x['mode']} {x['from']}->{x['to']}" for x in bad) if bad else ""))
    if bad:
        raise Refused(f"{len(bad)} overs not heard")


def clean_log(host):
    py = ("import re,sqlite3\nn=0\ntry:\n d=sqlite3.connect('/mnt/qso_log.db')\n"
          " n=d.execute(\"delete from qso_log where remote_callsign glob 'Q[0-9]TST*' or local_callsign glob "
          "'Q[0-9]TST*'\").rowcount;d.commit()\nexcept sqlite3.Error:\n pass\n"
          "p='/mnt/ft_log.adi'\ntry:\n s=open(p).read()\nexcept OSError:\n s=''\n"
          # the header (up to <eoh>) kept as it is: it was part of the first record, and went with a test QSO there
          "h=re.search(r'(?i)<eoh>',s);e=h.end() if h else 0\n"
          "parts=re.split(r'(?i)(<eor>)',s[e:]);keep=[s[:e]];m=0\n"
          "for i in range(0,len(parts)-1,2):\n"
          " r=parts[i]+parts[i+1]\n"
          " if re.search(r'(?i)<(call|station_callsign|operator):\\d+>Q\\dTST',r): m+=1\n"
          " else: keep.append(r)\n"
          "keep.append(parts[-1] if len(parts)%2 else '')\n"
          "open(p,'w').write(''.join(keep)) if m else None\nprint(n,m)")
    # base64: the script's newlines and quotes through two shells came out as "\n" and broke it (2026-11-18)
    b64 = base64.b64encode(py.encode()).decode()
    out = ssh(host, f"python3 -c \"import base64;exec(base64.b64decode('{b64}'))\"").split()
    if out and out != ["0", "0"]:
        say(f"{host}: test QSOs out of the logs: {out[0]} from the database, {out[1]} from the ADIF")


# ---- Commands ----

def cmd_provision(c, a):
    rc = c["radios"][a.name]
    tok = secrets.token_hex(16)
    tx = 1 if a.kind == "tx" else 0
    now = int(time.time())
    # the token in the clear (the GUI hashes it as it loads the file, sess_load), operate, TX or not; the session of
    # the same name made before replaced; then the GUI restarted to load it (printf's \t: no tab through the shell)
    line = f"{tok}\\tbench-{a.kind}\\t1\\t{tx}\\t{now}\\t{now}\\t-\\t0\\n"
    ssh(rc["host"], f"f=/mnt/remote_sessions.conf; touch $f; awk -F'\\t' '$2 != \"bench-{a.kind}\"' $f > /tmp/rs.$$ "
                    f"&& printf '{line}' >> /tmp/rs.$$ && cat /tmp/rs.$$ > $f && rm /tmp/rs.$$ && "
                    f"kill $(pidof x6100_gui)")
    toks = load_json(os.path.join(STATE, "tokens.json"), {})
    toks.setdefault(a.name, {})[a.kind] = tok
    save_json(os.path.join(STATE, "tokens.json"), toks)
    say(f"{a.name}: session bench-{a.kind} ({'may' if tx else 'may NOT'} transmit); the GUI restarts")
    time.sleep(15)
    r = Radio(a.name, rc, a.kind == "tx")
    say(f"{a.name}: role {' '.join(r.role)}")
    r.close()


def state_words(r):
    """A radio's whole state as a test sees it, and what in it would spoil a test unless the test sets it itself"""
    s = r.state
    line = (f"{s.get('freq')} Hz (TX on {s.get('txf')}) mode {s.get('mode')} VFO {s.get('vfo')} split {s.get('split')} "
            f"RIT {s.get('rit')} XIT {s.get('xit')} | ATT {s.get('att')} PRE {s.get('pre')} RFG {s.get('rfg')} "
            f"AGC {s.get('agc')} SQL {s.get('sql')} NB {s.get('nb')} NR {s.get('nr')} filter {s.get('flo')}-"
            f"{s.get('fhi')} | play {s.get('play')} LISTEN {s.get('heard')} speaker {s.get('spk')} muted "
            f"{s.get('muted')} TCI mute {s.get('tcim')} monitor {s.get('moni')} mic {s.get('mic')} | page "
            f"{s.get('pg')} rxo {s.get('rxo')} {s.get('pwr')} W ATU {s.get('atu')} TUNE {s.get('tune')}")
    warn = []
    if s.get("play"):
        warn.append(f"its base in play mode ({PLAY_MODES.get(s.get('play'), s.get('play'))}): its receiver's audio "
                    f"does not reach the processor")
    if s.get("heard") in (1, 2):
        warn.append("LISTEN on an SDR")
    if s.get("txf") is not None and s.get("txf") != s.get("freq"):
        warn.append("it transmits off its dial")
    for k, why in (("att", "ATT on"), ("split", "split on"), ("rit", "RIT on"), ("nb", "NB on"), ("nr", "NR on"),
                   ("tcim", "a TCI client's over mutes it"), ("atu", "ATU on")):
        if s.get(k):
            warn.append(why)
    if s.get("rfg") is not None and s.get("rfg") < 100:
        warn.append(f"RF gain {s.get('rfg')}")
    if s.get("sql"):
        warn.append(f"squelch {s.get('sql')}")
    return line, warn


def cmd_status(c, a):
    for name in a.names or list(c["radios"]):
        try:
            toks = load_json(os.path.join(STATE, "tokens.json"), {}).get(name, {})
            r = Radio(name, c["radios"][name], "rx" not in toks)
            time.sleep(1.5)
            line, warn = state_words(r)
            say(f"{name}: {line}; tx {int(r.tx)} app {r.state.get('app') or '-'} S {r.meter} dBm; session may "
                f"transmit: {r.may_tx}")
            if warn:
                say(f"{name}: as it is (each test sets its own, these noted): " + "; ".join(warn))
            r.close()
        except (Refused, OSError) as e:
            say(f"{name}: {e}")


def cmd_chain_ok(c, a):
    p = c.get("profiles", {}).get(a.profile)
    if not p:
        raise Refused(f"no profile {a.profile}")
    doc = load_json(os.path.join(STATE, "chain.json"), {})
    doc[a.profile] = {"at": time.time(), "words": a.words, "pads": p.get("pads")}
    save_json(os.path.join(STATE, "chain.json"), doc)
    say(f"{a.profile}: the owner's word recorded ({sum(x['db'] for x in p.get('pads', []))} dB), good for 12 hours")


def cmd_power_ok(c, a):
    p = c.get("profiles", {}).get("radios")
    if not p:
        raise Refused("no profile radios")
    if not HARD_MAX_W < a.w <= FULL_RATING_MAX_W:
        raise Refused(f"power-ok is for {HARD_MAX_W} to {FULL_RATING_MAX_W} W")
    for name in p.get("between", []):
        at = [x for x in p.get("pads", []) if x.get("at") == name]
        if not at or at[0].get("watts", 0) < a.w:
            raise Refused(f"the pad on {name}'s jack is not rated for {a.w} W")
    doc = load_json(os.path.join(STATE, "power.json"), {})
    doc["radios"] = {"at": time.time(), "w": a.w, "words": a.words}
    save_json(os.path.join(STATE, "power.json"), doc)
    say(f"radios: the owner's word for {a.w} W recorded (each over at most {FULL_RATING_KEY_S} s, the pad resting as "
        f"long after), good for 12 hours")


def cmd_preflight(c, a):
    p, total = chain_gate(c, a.profile)
    say(f"{a.profile}: cable confirmed, {total} dB of pads")
    if a.profile == "hackrf":
        hackrf_gate(c["limits"]["hackrf_x"])
        say("HackRF found")
        r = Radio(p["to"], c["radios"][p["to"]], False)
        rx_only_gate(r)
        state_ready(r)
        say(f"{r.name}: receive-only session, not on the air, FT8 not set to send; {state_words(r)[0]}")
        r.close()
    else:
        for n in p.get("between", []):
            r = Radio(n, c["radios"][n], True)
            state_ready(r)
            call = ssh(r.host, "sqlite3 /mnt/params.db \"select val from params where name='callsign'\"").strip()
            say(f"{n}: may transmit {r.may_tx}, ATU {r.state.get('atu')}, {r.pwr()} W, callsign {call or 'none'}"
                f"{'' if TEST_CALL.match(call) else ' (NOT a test call)'}; {state_words(r)[0]}")
            supply = listener_gate(r)
            if supply:
                say(f"{n}: battery {supply['battery_pct']} % ({supply['vbat']} V), DC in {supply['dc_in']} V")
            r.close()
    say("preflight passed: nothing was sent")


def cmd_testcall(c, a):
    """A test call set (its GUI restarted to read it), the radio's own kept in calls.json; restore puts it back. With
    a test call PSK Reporter is off (its default is on: every FT8/JS8 decode would be spotted on the internet with
    the radio's locator): its row as found is kept beside the call and put back with it"""
    rc = c["radios"][a.name]
    saved = load_json(os.path.join(STATE, "calls.json"), {})
    cur = ssh(rc["host"], "sqlite3 /mnt/params.db \"select val from params where name='callsign'\"").strip()
    pskr = ssh(rc["host"], "sqlite3 /mnt/params.db \"select val from params where name='pskreporter'\"").strip()
    if a.call in ("restore", "--restore"):
        old = saved.pop(a.name, None)
        if old is None:
            raise Refused(f"{a.name}: no callsign kept to go back to")
        q = (f"insert or replace into params(name,val) values('callsign','{old}')" if old else
             "delete from params where name='callsign'")
        old_pskr = saved.pop(a.name + ".pskreporter", "")
        if re.fullmatch(r"-?\d+", old_pskr):
            q += f"; insert or replace into params(name,val) values('pskreporter',{int(old_pskr)})"
        else:
            q += "; delete from params where name='pskreporter'"      # none: the default (on)
    else:
        if not TEST_CALL.match(a.call):
            raise Refused("a test callsign is Q<digit>TST")
        if not TEST_CALL.match(cur):
            saved[a.name] = cur
            saved[a.name + ".pskreporter"] = pskr
        q = (f"insert or replace into params(name,val) values('callsign','{a.call}'); "
             f"insert or replace into params(name,val) values('pskreporter',0)")
    save_json(os.path.join(STATE, "calls.json"), saved)
    ssh(rc["host"], f"/etc/init.d/S95gui stop >/dev/null 2>&1; sqlite3 /mnt/params.db \"{q}\"; "
                    f"/etc/init.d/S95gui start >/dev/null 2>&1")
    back = a.call in ("restore", "--restore")
    say(f"{a.name}: callsign {'(its own)' if not TEST_CALL.match(cur) else cur} -> "
        f"{'its own' if back else a.call}; PSK Reporter {'as found' if back else 'off'}; the GUI restarted")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("provision"); s.add_argument("name"); s.add_argument("kind", choices=["rx", "tx"])
    s = sub.add_parser("status"); s.add_argument("names", nargs="*")
    s = sub.add_parser("chain-ok"); s.add_argument("profile", choices=["hackrf", "radios"]); s.add_argument("words")
    s = sub.add_parser("power-ok"); s.add_argument("profile", choices=["radios"]); s.add_argument("w", type=float)
    s.add_argument("words")
    s = sub.add_parser("preflight"); s.add_argument("profile", choices=["hackrf", "radios"])
    sub.add_parser("stop")
    sub.add_parser("restore", help="the radios a test left not as found, put back from its snapshot")
    s = sub.add_parser("hackrf-ft8")
    s.add_argument("--band", default="20m", choices=FT8_DIAL); s.add_argument("--x", type=int, default=None)
    s.add_argument("--text", default="CQ Q9TST FN00"); s.add_argument("--audio", type=int, default=1500)
    s.add_argument("--pre", type=int, default=0, choices=(0, 1)); s.add_argument("--att", type=int, default=0)
    s = sub.add_parser("hackrf-iq"); s.add_argument("file"); s.add_argument("--at", type=int, required=True)
    s.add_argument("--offset", type=int, required=True); s.add_argument("--fs", type=int, default=2000000)
    s.add_argument("--mode", type=int, default=7); s.add_argument("--x", type=int, default=None)
    s.add_argument("--during", default=None); s.add_argument("--lead", type=float, default=3.0)
    s.add_argument("--filter", type=int, nargs=2, default=None, metavar=("LO", "HI"))
    s.add_argument("--pre", type=int, default=0, choices=(0, 1)); s.add_argument("--att", type=int, default=0)
    s = sub.add_parser("radios-walkie"); s.add_argument("a"); s.add_argument("b")
    s.add_argument("--bands", default=""); s.add_argument("--modes", default="am,fm")
    s.add_argument("--senders", default="", help="only these send (the others listen): A,B")
    s.add_argument("--keep-armed", action="store_true", help="each radio armed through a channel's overs, as an "
                   "operator keeps it (locked again only to retune), not locked after each over")
    s = sub.add_parser("radios-ft8"); s.add_argument("tx"); s.add_argument("rx")
    s.add_argument("--rx-alike", action="store_true", help="kept for old command lines: both receivers are always "
                   "set alike now (PRE and ATT off, RF gain 100, AGC auto)")
    s.add_argument("--band", default="20m", choices=FT8_DIAL)
    s = sub.add_parser("testcall"); s.add_argument("name")
    s.add_argument("call", help="Q<digit>TST, or restore (argparse took --restore for an option: 2026-11-18)")
    s = sub.add_parser("cleanlog"); s.add_argument("name")
    a = ap.parse_args()
    c = None

    install_stop_signals(lambda: c)
    try:
        c = conf()
        if a.cmd in ("hackrf-ft8", "hackrf-iq") and a.x is None:
            a.x = c["limits"]["hackrf_x"]
        {"provision": cmd_provision, "status": cmd_status, "chain-ok": cmd_chain_ok, "power-ok": cmd_power_ok,
         "preflight": cmd_preflight, "restore": cmd_restore,
         "stop": lambda c, a: emergency_stop(c), "hackrf-ft8": run_hackrf_ft8, "hackrf-iq": run_hackrf_iq, "radios-ft8": run_radios_ft8,
         "radios-walkie": run_radios_walkie,
         "testcall": cmd_testcall, "cleanlog": lambda c, a: clean_log(c["radios"][a.name]["host"])}[a.cmd](c, a)
    except Refused as e:
        say(f"REFUSED: {e}")
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
