"""The 1:1 suite's "air_b" batch: CW and the digital modes over the air, over the cable between the radios (bench.py chain-ok radios:
80 dB of pads): one radio sends at 0.1 W (1 W at most), the other listens; the path proved first, every over watched,
never both keyed; each radio put back and left on the Sentry's page A. Both directions where both radios can do it.

    suite.py run air_b [--only TEST,...]
    AIR_B_WAYS="x6100>x6200" (one direction only), AIR_B_PROTOS="FT4" (ft8-qso's protocols): a round again on
    the part not yet right

The owner, 2026-11-23: "One has to RX the other has to TX!" In every test here one radio sends and the other confirms
what it heard: its CW decoder (the remote's "cw" lines), its spectrum and TCI audio, its apps' own text log
(/mnt/digi_text), its WeFax picture (the state's rows, the PNG it saves), its FT8 and JS8 lists (the remote's "ft8r"
and "js8r" lines). Before each test both radios are put on one receiver set-up (suite_more.sync_both) and after it back
as found (unsync_both); inside it each direction takes both radios' snapshots and finish() puts them back, page A.

The tests (the features each covers in brackets; S sends, L listens; both ways):
  cw-copy          S keys "VVV ÅÄÖÜÉÑ Q1TST" with its own keyer through its TCI (CW, 0.1 W): L's decoder gives
                   the six letters back, never <?> [cw-decoder-extended-chars]; L's spectrum puts the station on its
                   dial and L's TCI audio has it at L's key tone [cw-station-on-vfo-line]
  cw-carrier       L's decoder: 12 s of the cable's quiet, then S's steady carrier on L's dial for 10 s (at L's key
                   tone, L's spectrum 15 dB up at least): no text either time; then S keys CW and L copies it (its
                   decoder was listening) [cw-decoder-no-noise]
  cw-kbd           S's CW Keyboard app: two messages added on S's web page (taken out after); the short one keyed by
                   its page key and copied by L, {CALL} filled when S has a test call [cwkbd-message-pages]; the long
                   one stopped with Stop: S off the air within 3 s [cwkbd-send-stop-clear: Send and Stop; Clear is the
                   screen's log, not read]
  rtty             RTTY made here to L's own RTTY settings (digimodes.py), sent by S's TCI; L's RTTY panel (opened
                   from the Modes menu) writes it into its text log as an "RTTY RX" line [rtty-rx-decode]
  psk              S's and L's PSK31 apps: a macro added on S's web page (the file put back after) sent by its key, L's
                   app writes it in a "PSK31 RX" line [psk-macros-tx]; a long macro stopped with Stop TX: S off the
                   air within 2 s [psk-stop-tx]
  jtty             S's and L's JTTY apps: a macro added on S's web page sent by its F-key, L's text log has a "JTTY RX"
                   line with its dB and the text [jtty-txrx-macros: a macro of the test's own; the CQ key's locator
                   and PSK Reporter spot are not sent]
  olivia           S's and L's Olivia apps through all eleven variants (Olivia 8/250 ... Contestia 32/1000), the Mode
                   key pressed on both together, round to where they were: a macro sent in each, L's text log has it in
                   the variant S sent [olivia-modes-txrx, contestia-modes-txrx]
  wefax-cont       L's WeFax in Cont: S sends 12 s of a chart, half white, half black; L takes rows, "wefax save"
                   mid-fax writes a PNG with the chart's two halves in it, "wefax clear" drops the rows
                   [wefax-save-clear, wefax-rx-auto-cont: Cont]
  wefax-auto       L's WeFax in Auto: S sends the APT start tone and the black band, then the chart over several overs
                   (Auto saves a fax of 200 rows: 100 s; each over is 13 s), then the stop tone: L takes the start,
                   keeps the rows and at the stop saves a PNG with the chart in it [wefax-rx-auto-cont: Auto]
  ft8-multistream  S sends two FT8 messages at once (gen_ft8's, at 1000 and 1600 Hz) through its TCI in an FT8 slot;
                   L's FT8 app decodes both [ft8-multistream-decode]
  ft8-qso          (test calls on both) FT8, FT4 and FT2: both with Auto, S calls CQ, L answers it once ("ft8 reply"),
                   the rest by itself: logged on both, one record each in ft_log.adi in the mode's ADIF, S quiet
                   after [ft8-auto-sequence, ft8-auto-log, ft4-tx-rx, ft2-tx-rx]; in FT4 L saves the QSO by hand
                   (Force QSO save) once both reports went: its record there before S's RR73, no second one after
                   [ft8-force-qso-save]
  ft8-stop         (a test call on S) S's CQ stopped with "ft8 stop" 2 s into its slot: off the air within a second,
                   not on again, nothing of it decoded by L [ft8-tx-call-stop]
  ft8-hold         (test calls) S's CQ at 1500 Hz, L's answer from TX Hz 2000: with Hold heard at 2000 and L's TX Hz
                   kept; without, heard at the CQ's and L's TX Hz moved there [ft8-hold-freq]
  ft8-late         (test calls) L's CQ, S answers; L calls CQ again once its report went: S's R-report still logged
                   by L (one record) and L's RR73 heard by S [ft8-late-qso-kept]
  js8              (test calls) S's CQ and heartbeat in L's list; S asks L "SNR?" and L, Auto on, answers; S's MSG in
                   L's inbox file; S's long message stopped with "js8 stop": off the air at once, the app still open
                   [js8-cq, js8-heartbeat, js8-directed-queries, js8-inbox, js8-stop-sending]

Nothing here reaches the internet. PSK Reporter spots every FT8/JS8 decode (on by default): those tests run only where
it is off; the PSK and Olivia texts carry no "DE <call>" (the only thing those apps spot); the FT8 UDP to loggers is
switched off for the FT8 and JS8 tests and put back byte for byte, Wavelog on refuses them, and their test QSOs are
taken out of ft_log.adi, qso_log.db and the day's Field Ops log after. Test calls (Q1TST, Q2TST) are not set here: that
restarts the GUI, which this batch never does (bench.py testcall NAME Q1TST first); without them the tests that send
from the FT8 and JS8 apps are refused, never run with a real call. No callsign or locator of the radios is printed.
Every file an app writes on the card is put back after: the apps' frequency memories (/mnt/app_freqs), the text log
lines of the test, the WeFax pictures it saved, the macros and CW messages it added, JS8's inbox and log.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import calendar
import collections
import json
import math
import os
import re
import shutil
import socket
import subprocess
import threading
import time
import wave

import bench
from suite_more import F_CW, say, shots_words, sync_both, unsync_both
from suite_proto import LOG_PREP, adif_records, http, pair_ok

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src")
B = "air_b"
W = bench.FIRST_PROBE_W             # every over here: 0.1 W
CW_MAX_S = 12.0                     # a CW text's time on the air at most (the watch's limit is 16 s)
F_RTTY = 14080000                   # 20 m, USB: the path proof's dial before an app takes the radio
F_PSK = 14070000
F_OLIVIA = 14072000
F_JTTY = 14090000
F_FAX = 14100000                    # the WeFax tests' dial (USB): S's 1075-1925 Hz lands in L's passband
F_JS8 = 14078000
FT_DIAL = {"FT8": 14074000, "FT4": 14080000, "FT2": 14084000}  # the FT8 app's 20 m dials (suite_cover FT8_FREQS)
ACTION_APP_MODES = 114              # params.h press_action_t: the Modes menu (RTTY is its first row)
APP_FREQS = "/mnt/app_freqs"        # app_window.c: each app's last frequency, written as it closes
TEST_CALL = bench.TEST_CALL
GRID = re.compile(r"\b(?!RR73\b)(?!FN00\b)[A-R]{2}[0-9]{2}(?:[A-Xa-x]{2})?\b")
# The receiver keys a sender takes as the listener has them when the dial or the mode is apart on purpose (suite_air_a's
# ALIGN_KEYS): the rest of bench.PAIR_KEYS the Watch compares as each over begins
ALIGN_KEYS = ("pre", "att", "agc", "rfg", "sql", "nb", "nr", "rit", "zoom")


# ---- small helpers ----

def scrub(text):
    """A decoded text as it may be printed: any locator but the test grid FN00 left out (a radio's own locator is the
    owner's location)"""
    return GRID.sub("<grid>", " ".join((text or "").split()))


def wait_for(cond, secs, step=0.1):
    end = time.time() + secs
    while time.time() < end:
        if cond():
            return True
        time.sleep(step)
    return bool(cond())


def sh(host, cmd, timeout=40):
    """An SSH command on a radio: its output ("" when it failed)"""
    try:
        r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout
    except subprocess.SubprocessError:
        return ""


def scp_to(local, host, remote):
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", local, f"root@{host}:{remote}"], check=True, timeout=60)


def param(host, name):
    """A value of the radio's settings (params.db, read only), None when it has no row"""
    out = sh(host, f"sqlite3 /mnt/params.db \"select val from params where name='{name}'\"", 20).strip()
    return out or None


def test_call(host):
    """The radio's callsign when it is a test call (Q<digit>TST), else None (a real one is never printed)"""
    call = param(host, "callsign") or ""
    return call if TEST_CALL.match(call) else None


def no_spots(host):
    """Whether the radio's decodes stay off the internet: PSK Reporter off (its default is on: no row means on)"""
    return param(host, "pskreporter") == "0"


def key_wpm(host):
    """The radio's KEY Speed (its keyer's, TCI's cw_macros too), 15 when it has no row (its default)"""
    v = param(host, "key_speed")
    return int(v) if v and v.isdigit() else 15


MORSE = {"A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....", "I": "..",
         "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
         "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..",
         "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....", "6": "-....",
         "7": "--...", "8": "---..", "9": "----.", "Å": ".--.-", "Ä": ".-.-", "Ö": "---.", "Ü": "..--", "É": "..-..",
         "Ñ": "--.--", "?": "..--..", "/": "-..-.", "=": "-...-"}


def morse_secs(text, wpm):
    """How long text takes keyed at wpm (PARIS: a dot 1.2/wpm s; a dash 3 dots, 1 between elements, 3 between letters,
    7 between words)"""
    units = 0
    for wi, word in enumerate(w for w in text.upper().split() if w):
        units += 7 if wi else 0
        codes = [MORSE[c] for c in word if c in MORSE]
        for ci, code in enumerate(codes):
            units += (3 if ci else 0) + sum(1 if e == "." else 3 for e in code) + len(code) - 1
    return units * 1.2 / max(wpm, 1)


def cw_speedup(text, wpm, most=CW_MAX_S, top=30):
    """text with TCI's ">" steps in front (5 WPM each, KEY Speed back at its end: tci_server.c cw_text) so it takes at
    most `most` seconds, never over `top` WPM: (the text to send, its WPM, its seconds); Refused when it cannot"""
    steps = 0
    while morse_secs(text, wpm + 5 * steps) > most and wpm + 5 * (steps + 1) <= top:
        steps += 1
    secs = morse_secs(text, wpm + 5 * steps)
    if secs > most:
        raise bench.Refused(f"{text!r} takes {secs:.1f} s at {wpm + 5 * steps} WPM, over the {most:.0f} s of an over")
    return ">" * steps + text, wpm + 5 * steps, secs


def settle(r, watch, quiet=1.5, most=16.0):
    """r off the air `quiet` s on end (an app's over must not be taken as done in a gap of its own), within `most` s;
    False when it was not, or the watch stopped"""
    end, off_since = time.time() + most, None
    while time.time() < end and not watch.why:
        if r.tx:
            off_since = None
        elif off_since is None:
            off_since = time.time()
        elif time.time() - off_since >= quiet:
            return True
        time.sleep(0.05)
    return False


def lines_json(r, prefix, since=0.0):
    """Every '<prefix> {json}' text r's session got since `since`: [(when, dict)]"""
    items = list(r.lines)
    out = []
    for t, x in items:
        if t >= since and x.startswith(prefix + " {"):
            try:
                out.append((t, json.loads(x[len(prefix) + 1:])))
            except ValueError:
                pass
    return out


def last_json(r, prefix):
    """The newest '<prefix> {json}' r's session got, {} for none"""
    head = prefix + " {"
    for t, x in reversed(list(r.lines)):
        if x.startswith(head):
            try:
                return json.loads(x[len(prefix) + 1:])
            except ValueError:
                return {}
    return {}


def fmt(v, spec=""):
    """A number for the result's words: "-" for none"""
    return "-" if v is None else format(v, spec)


def ctl_who(r):
    """Who has r's control as its remote last said ("ctl WHO MINE": none, local, tci, browser NAME): its WHO"""
    for _, x in reversed(list(r.lines)):
        if x.startswith("ctl "):
            return x.split()[1] if len(x.split()) > 1 else ""
    return None


def ctl_back(r, where):
    """r's control still the harness's, else the test stops here, never taken back while it runs: the radio's own keys
    and knobs give control to the operator at the radio, who has the last word (the owner may be standing at it). A
    GUI before v0.3.12.48 read the remote's own keys late under load (an FT8 app decoding) and took them for the
    operator's (2026-11-23, both radios): its control_owner now matches them by the kernel's event time"""
    if not r.ctl_mine:
        r.__dict__.setdefault("ctl_lost", []).append(where)
        raise bench.Refused(f"{r.name}: control went to {ctl_who(r) or 'someone'} ({where}): the test stops, control "
                            "not taken back while it runs")


def tap(r, key, hold=0.15):
    r.send(f"key {key} 1")
    time.sleep(hold)
    r.send(f"key {key} 0")
    time.sleep(0.45)
    ctl_back(r, f"after the remote's key {key}")


def app_name(r):
    return r.state.get("app") or r.state.get("win") or ""


def open_listed(r, *names):
    """r's app whose name starts with one of names (any case), from the remote's apps list only (never E's types: each
    of those it opens on the way sets its own memory, and the FT8 ones the FT8 app's protocol)"""
    r.apps = None
    r.send("apps")
    r.wait("the apps list", lambda: r.apps is not None, 5)
    app = next((a for a in r.apps if a[1].lower().startswith(names)), None)
    if not app:
        raise bench.Refused(f"{r.name}: no {names[0]} app in its list")
    r.send(f"open {app[0]}")
    r.wait(f"its {app[1]}", lambda: app_name(r).lower().startswith(names), 10)
    time.sleep(1.5)
    return app_name(r)


def keep_file(o, r, path):
    """A file on r kept aside now (suite_cover's keep) and put back byte for byte after the test (a cleanup: register
    it before the cleanups that close apps, which run first)"""
    from suite_cover import keep
    got = keep(r.host, path, "save")
    if got not in ("saved", "none"):
        raise bench.Refused(f"{r.name}: {path} not kept aside ({got!r})")

    def back():
        if path == APP_FREQS:
            bench.close_apps(r)             # an app still open writes its frequency there as it closes: first
        b = keep(r.host, path, "back")
        if b not in ("same", "restored", "removed"):
            raise bench.Refused(f"{r.name}: {path} not put back ({b!r})")
    o.later(f"{r.name}: {path} as found", back)


def web_ip(host):
    return socket.gethostbyname(host)


# ---- the text log of the keyboard modes (/mnt/digi_text): read, and the test's lines taken out after ----

def tl_sizes(host):
    """{file: bytes} of the radio's text log folder"""
    out = sh(host, "for f in /mnt/digi_text/*.txt; do [ -f \"$f\" ] && echo \"$f $(wc -c < \"$f\")\"; done; true", 20)
    got = {}
    for ln in out.splitlines():
        p = ln.rsplit(" ", 1)
        if len(p) == 2 and p[1].strip().isdigit():
            got[p[0]] = int(p[1])
    return got


def tl_new(host, before):
    """The text log's lines since before (tl_sizes)"""
    now = tl_sizes(host)
    text = []
    for f in sorted(now):
        if now[f] > before.get(f, 0):
            text.append(sh(host, f"tail -c +{before.get(f, 0) + 1} {f}", 20))
    return [ln for ln in "".join(text).splitlines() if ln.strip()]


def tl_wait(host, before, cond, secs=30.0):
    """The text log's new lines, read every 3 s till cond(lines) or secs"""
    lines, end = [], time.time() + secs
    while True:
        lines = tl_new(host, before)
        if cond(lines) or time.time() > end:
            return lines
        time.sleep(3.0)


def tl_keep(o, r):
    """r's text log as it is now; the test's lines taken out after (each file cut back to its size, a file the test
    began taken out). Register before the cleanups that close apps: their last lines go onto the card as they close"""
    before = tl_sizes(r.host)

    def back():
        for attempt in range(3):            # a line an app writes as it closes can land just after a cut (jtty,
            if attempt:                     # 2026-11-24): cut again
                time.sleep(2.0)
            now = tl_sizes(r.host)
            cmds = [f"rm -f {f}" if f not in before else f"python3 -c \"import os;os.truncate('{f}',{before[f]})\""
                    for f, n in now.items() if f not in before or n > before[f]]
            if cmds:
                sh(r.host, "; ".join(cmds), 30)
            after = tl_sizes(r.host)
            if not (any(after.get(f) != n for f, n in before.items()) or set(after) - set(before)):
                return
        raise bench.Refused(f"{r.name}: its text log not as before")
    o.later(f"{r.name}: the test's lines out of its text log", back)
    return before


# ---- one direction of a test: the sender's and the listener's sessions, put back whatever happens ----

class Over:
    """S (a session that may transmit) and L (receive only unless listener_tx): both taken, their snapshots kept, L's
    supply checked, both on G. self.res(name, ok, says) records a result; self.cleanups run (the last registered
    first) before finish() puts both back, read back, on page A"""

    def __init__(self, run, s_name, l_name, total, listener_tx=False, test=""):
        self.run, self.c, self.total, self.listener_tx, self.test = run, run.c, total, listener_tx, test
        self.s_name, self.l_name = s_name, l_name
        self.snd = self.lis = None
        self.radios, self.snaps, self.rep, self.out, self.before = [], {}, {}, {}, {}
        self.cleanups, self.unclean, self.watches = [], [], []

    def open(self):
        self.snd = bench.Radio(self.s_name, self.c["radios"][self.s_name], True)
        self.radios.append(self.snd)
        self.lis = bench.Radio(self.l_name, self.c["radios"][self.l_name], self.listener_tx)
        self.radios.append(self.lis)
        for r in self.radios:
            r.lines = collections.deque(r.lines, maxlen=40000)    # the whole test's texts (js8r, wefax...)
            r.take_control()
            self.snaps[r.name] = bench.snapshot(r)
        bench.listener_gate(self.lis)
        if self.listener_tx:
            bench.listener_gate(self.snd)       # each listens in turn
        bench.open_page_both(self.radios, "G")  # the Walkie holds the dial where it is put (A's scan retunes); G tunes
                                                # each to its own channel: set alike again

    def res(self, name, ok, says):
        self.out[name] = (None if ok is None else bool(ok), says)

    def later(self, what, fn):
        self.cleanups.append((what, fn))

    def banner(self, text):
        for r in self.radios:
            r.send(f"banner BENCH {text}"[:500])

    def label(self, what):
        """The name of an over's pictures (bench.SHOTS_DIR): the batch, the test, the direction, the over"""
        return f"air_b {self.test} {self.s_name} to {self.l_name} {what}"

    def keep_mem(self, r, f, mode):
        """r's own memory of mode (its filter and zoom: each mode's, which finish's snapshot does not hold), kept once
        before the other radio's set-up is copied over it, so finish(before=) puts it back (suite_more.align_over's
        way). r must be in that mode now"""
        mem = self.before.setdefault(r.name, [])
        if int(r.state.get("mode", -1)) == mode and not any(x.get("mode") == mode for x in mem):
            mem.append({"freq": f, "mode": mode, "filter": bench._value(r, "filter"),
                        "zoom": bench._value(r, "zoom")})

    def prove(self, f, both=False):
        """The path proof (bench.path_proof: 0.1 W, 1000 Hz, L's spectrum 20 dB up at least, both screens pictured) at
        f, S to L (and L to S with both); the proof's sender takes its listener's USB-D set-up, its own kept first for
        finish. L must be tuned near f already"""
        res = None
        for s, l in [(self.snd, self.lis)] + ([(self.lis, self.snd)] if both else []):
            bench.ensure(s, {"split": 0, "vfo": 0, "freq": f, "mode": 3})
            self.keep_mem(s, f, 3)
            got = bench.path_proof(self.c, self.total, s, l, f, self.rep,
                                   label=self.label(f"path proof {s.name} to {l.name}"))
            say(f"path proof {s.name} to {l.name}: both screens "
                f"{'on the air' if got.get('shots_on_air') else 'NOT caught on the air'}: "
                + ", ".join(os.path.basename(x) for x in (got.get("shots") or {}).values() if x))
            res = res or got
        return res

    def watch(self, what, listener=None, expect=True, ignore=(), both=False):
        """bench.Watch over both radios for the over `what` (its pictures labelled so), at 0.1 W; listener L unless
        given; both: the two take turns (an FT8 or JS8 QSO), so no one listener (one Watch only: two would picture
        each over twice at once, and the screen grabs on a radio would take each other's file); ignore: what the over
        has apart on purpose (the Watch stops an over whose radios are not set alike in anything else)"""
        lis = None if both else (listener or self.lis)
        w = bench.Watch(self.radios, self.c, w=W, listener=lis, label=self.label(what), pair_ignore=tuple(ignore),
                        expect_dbm=(10 * math.log10(W * 1000) - self.total) if expect and lis else None)
        self.watches.append(w)
        return w

    def alike(self, what, apart=()):
        """S and L compared (bench.PAIR_KEYS, as the Watch compares them when an over begins): anything apart but
        what the over has apart on purpose refused"""
        d = {k: v for k, v in bench.pair_diff(self.snd, self.lis).items() if k not in apart}
        if d:
            raise bench.Refused(f"{what}: {self.snd.name} and {self.lis.name} not set alike: "
                                + ", ".join(f"{k} {x}/{y}" for k, (x, y) in d.items()))

    def sender(self, f, mode, what, apart=(), page="G"):
        """S set for an over on G, the two alike (the owner, 2026-11-23: "they should have the same settings. IF they
        don't something is wrong"). L is set first (its dial and mode, read back); S on f in mode (its own memory of
        the mode kept for finish), then given exactly L's set-up (bench.pair_align: filter, zoom, PRE, ATT, AGC, RF
        gain, squelch, NB, NR, RIT, split, VFO), else (an over with the dial or the mode apart on purpose) L's
        receiver keys (ALIGN_KEYS); then its TX set-up at 0.1 W, read back; the two compared, nothing apart but
        apart (the over's watch's pair_ignore)"""
        snd, lis = self.snd, self.lis
        bench.open_page(snd, page)          # A for an app opened after it: G keeps its own channel, and an app opened
                                            # from G came up on the main dial (cw-kbd: 14.2 USB, not CW on 14.040)
        if apart:                           # the mode first: a 1 kHz move in CW was snapped back (cw-carrier)
            bench.ensure(snd, {"split": 0, "vfo": 0, "mode": mode})
        bench.ensure(snd, {"split": 0, "vfo": 0, "freq": f, "mode": mode})
        self.keep_mem(snd, f, mode)
        if not apart:
            bench.pair_align(snd, lis)
        else:
            want = {k: int(lis.state[k]) for k in ALIGN_KEYS if lis.state.get(k) is not None}
            if mode == int(lis.state.get("mode", -2)):
                want["filter"] = (int(lis.state.get("flo", 0)), int(lis.state.get("fhi", 0)))
            bench.ensure(snd, want)
        bench.ensure(snd, *bench.tx_profile(f, mode, W))
        self.alike(f"{what}: after {snd.name}'s TX set-up", apart)

    def apps_alike(self, what):
        """S's app open (L on its dial in its own app, or on G): S given L's set-up where they differ (pair_align: the
        app holds the dial and the mode, so those must already match), its own memory of the mode kept for finish;
        compared. An app that will not take it: Refused, naming what differs"""
        snd, lis = self.snd, self.lis
        self.keep_mem(snd, int(snd.state.get("freq", 0)), int(snd.state.get("mode", -1)))
        if bench.pair_diff(snd, lis):
            say(f"{what}: {snd.name} takes {lis.name}'s set-up: " + ", ".join(
                f"{k} {x}/{y}" for k, (x, y) in bench.pair_diff(snd, lis).items()))
            bench.pair_align(snd, lis)
        self.alike(what)

    def off_air(self, r, secs=5.0):
        """r off the air: waited for, else stopped (its remote, the bench's programs on it), then its app closed (an
        app's own keyer, the CW Keyboard's, JS8's, goes on past the remote's stop: closing the app ends it), else
        Refused"""
        if wait_for(lambda: not r.tx, secs, 0.05):
            return True
        r.stop()
        bench.stop_bench_programs(r)
        if wait_for(lambda: not r.tx, 3.0, 0.05):
            return True
        r.send("close")
        if wait_for(lambda: not r.tx, 5.0, 0.05):
            return True
        raise bench.Refused(f"{r.name} still on the air after its over was stopped")

    def keyed(self, what, f, mode, fn, apart=(), decl=None):
        """S on G set as L (sender(): L set before), on f in mode at 0.1 W (tx_profile, read back), the two compared,
        the gates, the banner; armed under a labelled Watch (both screens pictured on the air), fn(watch) run; then
        the watch closed, S off the air and locked again (rxo 1). (fn's result, the watch)"""
        if decl:                            # what apart says is kept apart, declared exactly (bench.apart)
            with bench.apart(decl, f"{self.test}: {what}"):
                return self._keyed(what, f, mode, fn, apart)
        return self._keyed(what, f, mode, fn, apart)

    def _keyed(self, what, f, mode, fn, apart):
        snd = self.snd
        self.sender(f, mode, what, apart)
        bench.radio_tx_gate(snd, self.c, W)
        self.banner(f"{what}: {snd.name.upper()} SENDS {W} W {f / 1e6:.4f} MHZ, {self.lis.name.upper()} LISTENS "
                    f"THROUGH {self.total} DB")
        w = self.watch(what, ignore=apart)
        try:
            bench.arm(snd, True, mode=mode)         # arming puts CW and the data modes into the voice mode: set again
            bench.tx_log({"who": snd.name, "f": f, "w": snd.pwr(), "what": f"suite air_b: {what}"})
            got = fn(w)
        finally:
            w.close()
            try:
                self.off_air(snd)
            finally:
                bench.arm(snd, False)
        return got, w

    def control_back(self, r, where):
        """ctl_back for one of the test's radios"""
        ctl_back(r, where)

    def lock_again(self, r):
        """An app's overs done: r off the air, its app closed, the Sentry's G open, its transmitter locked (rxo 1)"""
        self.control_back(r, "before its app closed")
        self.off_air(r)
        f, m = int(r.state.get("freq") or 0), int(r.state.get("mode", -1))
        with bench.transition():            # G tunes its own channel as it opens: back on the app's dial after it
            bench.close_apps(r)             # (psk, 2026-11-24: 29.6 FM against the other's 3.580)
            bench.open_page(r, "G")
            if f and m >= 0:
                bench.ensure(r, {"freq": f, "mode": m})
        if r.state.get("rxo") != 1 and r.may_tx:
            bench.arm(r, False)
        if r.state.get("rxo") != 1:
            raise bench.Refused(f"{r.name}: its transmitter not locked again after the app (rxo {r.state.get('rxo')})")

    def app_gate(self, r):
        """An app is about to send from r (the Sentry's hold went with the Sentry: the app keys by itself): 0.1 W, read
        back, and the gate (supply, ATU off, an amateur band)"""
        if abs(r.pwr() - W) > 0.05:
            bench.ensure(r, {"pwr": W})
        bench.radio_tx_gate(r, self.c, W)

    def end(self):
        for w in self.watches:
            w.close()
        for r in self.radios:
            # a link that dropped (2026-11-23: each GUI restarted at "wefax save" mid-fax, the session's socket gone)
            # renewed before the cleanups use it: else "WeFax closed" failed on the dead socket, the GUI reopened
            # WeFax as it came back (it restores its screen), the memories file was put back under the open app and
            # finish's close wrote it again
            if r.stale():
                try:
                    r.reconnect()
                except (bench.Refused, OSError) as e:
                    say(f"{r.name}: its link not renewed for the cleanups ({e})")
            # control taken back (2026-11-23: a touch at the radio took it mid-test on both, "controlled by the
            # radio's operator", and the cleanups that close the FT8 apps could not: finish closed them after the
            # memories file was put back, and their closing wrote it again)
            if not r.ctl_mine:
                try:
                    r.take_control()
                except (bench.Refused, OSError) as e:
                    say(f"{r.name}: control not taken back for the cleanups ({e})")
        # the put-back is one transition, the cleanups and finish (which ends the pair alike): an app closed here
        # puts its radio on its own dial (G's channel) while the files are put back (rtty, 2026-11-24)
        with bench.transition():
            for what, fn in reversed(self.cleanups):
                try:
                    fn()
                except Exception as e:      # every cleanup tried, whatever one of them met
                    say(f"{what}: not done ({type(e).__name__} {e})")
                    self.unclean.append(f"{what}: {e}")
            self.cleanups = []
            took = [r for r in self.radios if r.name in self.snaps]
            for r in self.radios:
                if r.name not in self.snaps:
                    r.close()
            if took:
                bench.finish(took, self.snaps, report=self.rep, before=self.before)


def over(run, s_name, l_name, parts, body, listener_tx=False, test=""):
    """One direction: body(o) with o an Over; every part body did not record gets why it was not"""
    o, why = None, None
    try:
        p, total = bench.chain_gate(run.c, "radios")
        o = Over(run, s_name, l_name, total, listener_tx, test)
        o.open()
        body(o)
    except bench.Refused as e:
        errs = []
        for r in (o.radios if o else []):
            errs += [f"{r.name}: {x}" for x in r.errs[-2:]]
        why = f"refused: {e}" + (f" (the radios said: {errs})" if errs else "")
    except Exception as e:                  # one test's fault never ends the batch: said, the radios put back
        import traceback
        why = f"error: {type(e).__name__} {e} ({traceback.format_exc().strip().splitlines()[-3:]})"
    finally:
        if o:
            o.end()
    out = dict(o.out) if o else {}
    lost = [f"{r.name} {w}" for r in (o.radios if o else []) for w in r.__dict__.get("ctl_lost", [])]
    if lost:
        for name in out:
            ok, says = out[name]
            out[name] = (ok, says + f"; control found gone from the harness (the test stopped there): {lost}")
    for name, _ in parts:
        if name not in out:
            out[name] = (False, why or "not reached")
        elif why:
            ok, says = out[name]
            out[name] = (ok, says + f"; then {why}")
    bad = []
    if o and o.rep.get("not_restored"):
        bad.append(f"NOT put back: {o.rep['not_restored']}")
    if o and o.unclean:
        bad.append(f"not cleaned up: {o.unclean}")
    if bad:                                 # a radio not as found fails the test, whatever it measured
        for name in out:
            ok, says = out[name]
            out[name] = (False, says + " (" + "; ".join(bad) + ")")
    return out


def two_way(run, test, parts, body, ways=None, listener_tx=False):
    """The test both ways (or the ways given): both radios in sync before it (sync_both, the owner's rule) and back as
    found after it (unsync_both); a result per part and direction, then each part 1:1"""
    run.close()
    run.radios = {}
    a, b = run.names[0], run.names[1]
    ways = ways or ((a, b), (b, a))
    # AIR_B_WAYS="x6100>x6200": only those directions (a test's round again where the other direction is settled,
    # e.g. CW to the X6100, whose decoder garbles the X6200's clean CW: a known product bug)
    only_ways = [tuple(x.split(">", 1)) for x in os.environ.get("AIR_B_WAYS", "").split(",") if ">" in x]
    if only_ways:
        ways = tuple(w for w in ways if w in only_ways)
    snaps, seen, why = sync_both(run)
    res = {}
    try:
        for s, l in ways:
            if why:
                out = {n: (False, f"refused: the radios are not in sync before the test ({why})") for n, _ in parts}
            else:
                out = over(run, s, l, parts, body, listener_tx, test)
            for n, covers in parts:
                ok, says = out[n]
                run.result(B, f"{n} {s}->{l}", ok, says, covers=covers)
                res.setdefault(n, []).append((f"{s}->{l}", ok))
    finally:
        unsync_both(run, snaps)
    if len(ways) == 2:
        for n, covers in parts:
            got = res.get(n, [])
            run.result(B, f"{n} 1:1", pair_ok(ok for _, ok in got),
                       ", ".join(f"{d} {'pass' if ok else 'look' if ok is None else 'FAIL'}" for d, ok in got),
                       covers=covers)


# ---- the sender's own TCI: CW text, audio files ----

def cw_key(o, text, max_s, watch):
    """text keyed by S's own keyer through its TCI (tci_cw_tx.py on S, the text passed as base64: no shell or locale
    in between touches Å), stopped at once if the watch stops: (its output, launch, end)"""
    snd = o.snd
    b64 = base64.b64encode(text.encode("utf-8")).decode()
    cmd = (f"python3 -c \"import base64,sys;sys.argv=['tci_cw_tx.py',base64.b64decode('{b64}').decode('utf-8'),"
           f"'{max_s:.1f}'];exec(open('/tmp/tci_cw_tx.py').read())\"")
    t0 = time.time()
    proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{snd.host}", cmd], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    while proc.poll() is None:
        if watch.why or time.time() - t0 > max_s + 20:
            bench.stop_bench_programs(snd)
            break
        time.sleep(0.05)
    out = proc.communicate(timeout=30)[0]
    return out, t0, time.time()


def tci_play(snd, path, max_s, watch, at=None, radio_at=None):
    """The 16-bit 24 kHz mono file `path` (on S) into S's transmitter through its own TCI (tci_tone_tx.py, copied
    there before), launched at `at` (epoch) when given, or keyed at radio_at by S's own clock (the program waits there
    for it: no SSH start-up in between), killed at once if the watch stops: (its lines, launch, end)"""
    while at and time.time() < at:
        time.sleep(0.01)
    t0 = time.time()
    args = f"'@{path}' 0.3 {max_s:.1f} 50001 0.05"
    cmd = f"python3 /tmp/tci_tone_tx.py {args}"
    if radio_at:
        cmd = (f"python3 -c \"import sys,time;time.sleep(max(0.0,{radio_at:.3f}-time.time()));"
               f"sys.argv=['tci_tone_tx.py','@{path}','0.3','{max_s:.1f}','50001','0.05'];"
               f"exec(open('/tmp/tci_tone_tx.py').read())\"")
    proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{snd.host}", cmd],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    while proc.poll() is None:
        if watch.why:
            sh(snd.host, "kill $(cat /tmp/tci_tone_tx.pid) 2>/dev/null", 15)
            bench.stop_bench_programs(snd)      # one still waiting for radio_at too (its command names the program)
            break
        time.sleep(0.05)
    out = proc.communicate(timeout=30)[0]
    return out.splitlines(), t0, time.time()


def radio_offset(host):
    """The radio's clock less this computer's (s), read over SSH (within half its round trip): None unread"""
    t0 = time.time()
    out = sh(host, "python3 -c 'import time;print(time.time())'", 20).strip()
    t1 = time.time()
    try:
        return float(out) - (t0 + t1) / 2
    except ValueError:
        return None


def raw_to(o, x, name):
    """Audio x (float, 24 kHz) written here as 16-bit and copied to S's /tmp/NAME.raw (taken out after), with
    tci_tone_tx.py: (its path there, its seconds)"""
    import digimodes
    local = os.path.join(o.run.dir, f"{name}_{o.snd.name}.raw")
    secs = digimodes.write_raw(x, local)
    scp_to(local, o.snd.host, f"/tmp/{name}.raw")
    scp_to(os.path.join(HERE, "tci_tone_tx.py"), o.snd.host, "/tmp/tci_tone_tx.py")
    o.later(f"{o.snd.name}: /tmp/{name}.raw", lambda: sh(o.snd.host, f"rm -f /tmp/{name}.raw", 15))
    return f"/tmp/{name}.raw", secs


def heard_elements(x, hz, wpm, fs=24000):
    """The CW in audio x (the listener's TCI audio, its key tone hz) read here element by element, as keyed at wpm:
    (its text, words on the elements): the keying as it reached the listener, to tell a decoder's fault from the
    sender's. None without enough audio"""
    import numpy as np
    if len(x) < fs:
        return None, "no TCI audio"
    t = np.arange(len(x)) / fs
    k = max(1, int(0.004 * fs))
    env = np.abs(np.convolve(x * np.exp(-2j * np.pi * hz * t), np.ones(k) / k, mode="same"))
    top, floor = np.percentile(env, 97), np.percentile(env, 10)
    on = env > (top + floor) / 2
    cuts = np.flatnonzero(np.diff(on.astype(np.int8))) + 1
    runs = [(bool(seg[0]), len(seg) / fs) for seg in np.split(on, cuts) if len(seg)]
    unit = 1.2 / max(wpm, 1)
    marks = [d for m, d in runs if m and d > unit * 0.3]
    rev = {v: c for c, v in MORSE.items()}
    out, code = [], ""
    for i, (m, d) in enumerate(runs):
        if m:
            if d > unit * 0.3:
                code += "." if d < 2 * unit else "-"
        elif i and code and (d >= 2 * unit or i == len(runs) - 1):
            out.append(rev.get(code, "<" + code + ">"))
            code = ""
            if d >= 5 * unit:
                out.append(" ")
    if code:
        out.append(rev.get(code, "<" + code + ">"))
    dots = [d for d in marks if d < 2 * unit]
    dashes = [d for d in marks if d >= 2 * unit]
    words = (f"dots {1000 * np.median(dots):.0f} ms, dashes {1000 * np.median(dashes):.0f} ms "
             f"({len(dots)}/{len(dashes)}; at {wpm} WPM {1000 * unit:.0f} and {3000 * unit:.0f}), the audio "
             f"{20 * np.log10(top / max(floor, 1e-9)):.0f} dB over its gaps") if dots and dashes else "no elements"
    return "".join(out).strip(), words


def both_at(o, f, mode):
    """Both radios on f in mode at once (sync_pair), before each one's receiver is said (rx_profile, a radio at a time:
    2026-11-24, JS8's first radio took 9 s over its PRE, ATT, AGC and filter while the other sat on the old dial)"""
    bench.sync_pair(list(o.radios), {"freq": f, "mode": mode})


def listen_at(o, f, mode, agc=3, filt=None):
    """L on f in mode, its receiver said whole (PRE and ATT off, RF gain 100, the AGC, squelch open), read back; S on
    the same dial and mode with it (sync_pair), so the two are not apart while the test readies the over (cw-copy's
    listener sat in CW 9 s before its sender followed, 2026-11-24). S holding an app (its dial the app's): L alone"""
    if not (o.snd.state.get("app") or (o.snd.state.get("win") or "Sentry") != "Sentry"):
        bench.sync_pair([o.snd, o.lis], {"freq": f, "mode": mode})
    return bench.ensure(o.lis, *bench.rx_profile(f, mode, pre=0, att=0, rfg=100, agc=agc, filt=filt))


PSD_BIN_HZ = 750            # the X6200 base's spectrum: 512 bins over 384 kHz at any zoom (measured 2026-11-24)
CW_FILTER = (300, 1100)     # 800 Hz on the 700 Hz key tone: a CW operator's; the probes leave CW 400-3000 (tests leave
                            # the receiver as they end), and through it the X6200's decoder printed the cable's noise


def listen_cw(o, f=F_CW):
    """L in CW on f, its receiver said whole (PRE and ATT off, RF gain 100, AGC slow, the CW filter, squelch open),
    read back; S with it (listen_at)"""
    listen_at(o, f, 4, agc=2, filt=CW_FILTER)


def rx_keys(r, agc=3):
    """r's receiver inside an app (the app holds the dial and mode): PRE and ATT off, RF gain 100, the AGC, squelch
    open, no NB, NR or RIT; its base out of play mode and no TCI over muting it"""
    bench.ensure(r, {"att": 0, "pre": 0, "agc": agc, "rfg": 100, "sql": 0, "nb": 0, "nr": 0, "rit": 0},
                 {"play": (lambda v: v == 0, "its base is in play mode: no receiver audio for the app"),
                  "tcim": (lambda v: v == 0, "a TCI client's over has it muted")})


# ---- CW: the sender's own keyer, the listener's decoder ----

def t_cw_copy(run):
    import numpy as np
    from tones import TciRx, peak
    parts = [("cw-letters", ["cw-decoder-extended-chars"]), ("cw-vfo-line", ["cw-station-on-vfo-line"])]

    def body(o):
        snd, lis = o.snd, o.lis
        listen_cw(o)
        o.prove(F_CW)
        listen_cw(o)
        scp_to(os.path.join(HERE, "tci_cw_tx.py"), snd.host, "/tmp/tci_cw_tx.py")
        # two V groups first: the decoder finds the speed on them (with one, Å came while both were still locking,
        # 2026-11-24: 'TQÄÖÜÉ', 'TTTTTÄÖÜÉÑ'), as cw-kbd's and cw-carrier's
        # at 20 WPM, not the 25 a 12 s over needs: at 25 a dot of the X6100's went missing in the X6200's audio
        # (Ä read as W, 2026-11-24); 14.2 s, under the over's 16
        text, wpm, secs = cw_speedup("VVV VVV ÅÄÖÜÉÑ Q1TST", key_wpm(snd.host), most=14.5)
        kt = int(lis.state.get("kt", 0))
        tci = TciRx(lis.host)
        o.later(f"{lis.name}: its TCI audio closed", tci.close)
        time.sleep(1.5)
        lis.cw = ""
        (said, t_on, t_off), w = o.keyed("CW 'VVV ÅÄÖÜÉÑ'", F_CW, 4, lambda w: cw_key(o, text, secs + 4, w))
        time.sleep(2.5)                     # the last letter shows after a pause
        got = " ".join(lis.cw.split())
        stop = f"; STOPPED: {w.why}" if w.why else ""
        letters = [c for c in "ÅÄÖÜÉÑ" if c in got]
        ok = not w.why and len(letters) == 6 and "Q1TST" in got.upper()     # a <?> in place of one: missing
        # the keying as it reached L (its TCI audio, read here element by element): the sender's or the decoder's
        xa, _ = tci.audio(t_on - 0.3, t_off + 1.0)
        els, el_words = heard_elements(xa, kt or 700, wpm)
        try:
            import digimodes
            digimodes.write_raw(xa, os.path.join(o.run.dir, f"cw_copy_{snd.name}_to_{lis.name}.raw"))
        except (ImportError, OSError, ValueError):
            pass
        # whose a misread is (cw_timing.py): the keyer late on its deadline is the GUI's (a fail); the keyer on time and
        # the carrier not, on an X6100 with Xiegu's factory base, is that base's keying (said, not judged: 2026-11-25,
        # its carrier ran on 30-80 ms after the key went up now and then); clean keying misread is the decoder's
        whose = ""
        if not ok and not w.why and len(xa) > 24000:
            import cw_timing as ct
            import suite_air_a as aa
            fw, _ = peak(xa, kt or 700, 120)
            tj = ct.judge(ct.cw_elements(xa, 24000, fw or kt or 700)[0], ct.cw_ideal(text), 1200.0 / wpm)
            keyer = ct.keyer_said(lambda h, c: sh(h, c, 15), snd.host)
            base = aa.base_of(snd.host) if snd.name == "x6100" else ""
            if keyer is not None and keyer[1]:
                whose = f"; the keyer itself late ({keyer[1]} of {keyer[0]} edges over 15 ms): the GUI's"
            elif ct.clean(tj):
                whose = f"; the air as keyed ({ct.words(tj)}): the decoder misread clean keying"
            elif snd.name == "x6100" and aa.gcfix_of(base) == 0 and keyer is not None:
                ok = None
                whose = (f"; the keyer on time ({keyer[0]} edges, the latest {keyer[2]} ms), the air not "
                         f"({ct.words(tj)}): {snd.name}'s base, Xiegu's {base or 'factory'} (said, not judged)")
            else:
                whose = f"; the air: {ct.words(tj)}"
        o.res("cw-letters", ok, f"{snd.name} keyed {text!r} at {wpm} WPM ({secs:.1f} s, its meter "
                                f"{w.tx_peak.get(snd.name)} W); {lis.name}'s decoder: {got[-90:]!r}: of the six "
                                f"letters {''.join(letters) or 'none'}, <?> {got.count('<?>')} times, Q1TST "
                                f"{'copied' if 'Q1TST' in got.upper() else 'NOT copied'}; heard "
                                f"{w.heard_db.get(snd.name)} dB over its quiet (CH is a sign the keyer cannot send: "
                                f"its typed CH goes as C and H); its TCI audio read here element by element: "
                                f"{els!r} ({el_words}){whose}; {shots_words(w)}{stop}")
        # the station on L's dial: L's own spectrum frames during the over, and its TCI audio at its key tone
        a0, a1 = t_on + 1.5, t_off - 0.5
        x, _ = tci.audio(a0, a1)
        pitch, lvl = peak(x, kt or 700, 300) if len(x) > 4096 else (None, None)
        fr = [(cen, wid, np.array(v, float)) for tt, cen, wid, v in list(lis.r.spectra) if a0 <= tt <= a1]
        at = per = width = None
        if fr:
            cen, wid = fr[-1][0], fr[-1][1]
            avg = np.mean([v for c_, w_, v in fr if c_ == cen and w_ == wid], axis=0)
            per = wid / len(avg)
            lo = max(0, int((F_CW - 1500 - (cen - wid / 2)) / per))
            hi = min(len(avg), int((F_CW + 1500 - (cen - wid / 2)) / per) + 1)
            if hi > lo:
                k = lo + int(np.argmax(avg[lo:hi]))
                at = cen - wid / 2 + (k + 0.5) * per
                width = per * int(np.sum(avg[lo:hi] >= avg[k] - 6))    # Hz within 6 dB of its top
        tol = min(max(1.5 * (per or 0), 80.0), (kt or 700) / 2)
        if at is None or pitch is None:
            ok2, note = False, "no spectrum frames or no TCI audio during the over"
        elif per > (kt or 700) / 2 or (kt and width > kt):
            # the X6200's base sends 512 bins over 384 kHz (750 Hz each) whatever the zoom, stretched to the shown
            # span: its picture places a station on the line to half a bin; the audio at the key tone places it
            # exactly (on the dial in CW: heard at the key tone)
            ok2 = abs(at - F_CW) <= PSD_BIN_HZ / 2 and abs(pitch - kt) <= 25 and not w.why
            note = (f"its picture is its base's {PSD_BIN_HZ} Hz bins stretched ({width:.0f} Hz within 6 dB of its "
                    f"top): on the line within half a bin, {PSD_BIN_HZ // 2} Hz, and the audio at the key tone")
        else:
            ok2, note = abs(at - F_CW) <= tol and abs(pitch - kt) <= 25 and not w.why, ""
        o.res("cw-vfo-line", ok2,
              f"{lis.name} in CW on {F_CW} Hz, key tone {kt} Hz: its spectrum has {snd.name}'s CW at "
              f"{fmt(at, '.0f')} Hz ({fmt(None if at is None else at - F_CW, '+.0f')} Hz from the dial, within "
              f"{tol:.0f} Hz asked; bins of {fmt(per, '.0f')} Hz), its TCI audio at {fmt(pitch, '.1f')} Hz "
              f"({fmt(lvl, '.1f')} dBFS; within 25 Hz of the key tone asked)" + (f"; {note}" if note else "")
              + f"; {shots_words(w)}" + stop)
    two_way(run, "cw-copy", parts, body)


CARRIER_APART = ("freq", "mode", "flo", "fhi")    # S in USB-D 1000 Hz under L's CW dial: a carrier on it, at its tone
CARRIER_DECL = {"freq": 1000, "mode": [(3, 4)], "filter": bench.STEPPED}    # the same, exactly


def t_cw_carrier(run):
    parts = [("cw-carrier", ["cw-decoder-no-noise"])]

    def body(o):
        import numpy as np
        from tones import TciRx
        snd, lis = o.snd, o.lis
        listen_cw(o)
        o.prove(F_CW)
        listen_cw(o)
        kt = int(lis.state.get("kt", 0))
        tci = TciRx(lis.host)               # the carrier's envelope as the listener hears it
        o.later(f"{lis.name}: its TCI audio closed", tci.close)
        time.sleep(1.5)                     # the proof's tone (1000 Hz off its dial, outside its CW filter) gone
        lis.cw = ""
        time.sleep(12.0)                    # the cable's quiet
        quiet = " ".join(lis.cw.split())
        lis.cw = ""
        mid = {}

        def carrier_over(w):
            """the 10 s carrier, the decoder read over its steady middle (from 2 s after the key to 2 s before the
            end): as an over starts the X6200's power comes up from 0 in steps, which the decoder takes for dots
            ('EE E SH' one run in two, 2026-11-24); a steady carrier is what must print nothing"""
            th = threading.Thread(target=lambda: bench.tci_send(snd, [[0, 0.3], [1000, 10.0]], max_s=13, watch=w),
                                  daemon=True)
            th.start()
            if wait_for(lambda: snd.tx or w.why, 6):
                time.sleep(2.0)
                lis.cw = ""
                t0 = time.time()
                time.sleep(6.0)
                mid["text"] = " ".join(lis.cw.split())
                x, _ = tci.audio(t0, time.time())
                if len(x) > 24000:          # 20 ms blocks: its deepest dip under the middle's median, dB
                    n = len(x) // 480
                    lv = 20 * np.log10(np.sqrt(np.mean(x[:n * 480].reshape(n, 480) ** 2, axis=1)) + 1e-9)
                    mid["dip"] = float(np.median(lv) - np.min(lv))
            th.join(20)
        # S in USB-D 1000 Hz under L's dial sends 1000 Hz: a carrier on L's dial, at L's key tone in its decoder
        _, w = o.keyed("A STEADY CARRIER ON THE LISTENER'S CW TONE", F_CW - 1000, 3, carrier_over,
                       apart=CARRIER_APART, decl=CARRIER_DECL)
        bench.sync_pair([snd, lis], {"freq": F_CW, "mode": 4})     # S back on L's dial in CW, the two alike again
        time.sleep(3.0)
        edges = " ".join(lis.cw.split())    # the carrier's last 2 s and its end
        carrier = mid.get("text")
        heard = w.heard_db.get(snd.name) or []
        # its decoder was listening: S keys CW on L's dial and L copies it
        scp_to(os.path.join(HERE, "tci_cw_tx.py"), snd.host, "/tmp/tci_cw_tx.py")
        text, wpm, secs = cw_speedup("VVV VVV Q1TST", key_wpm(snd.host))
        lis.cw = ""
        _, w2 = o.keyed("CW AFTER THE CARRIER", F_CW, 4, lambda w2: cw_key(o, text, secs + 4, w2))
        time.sleep(2.5)
        copy = " ".join(lis.cw.split())
        why = w.why or w2.why
        # a character in the middle with the carrier dipping 6 dB or more under its median is the carrier's own gap
        # (the sender's loop), not the decoder printing noise: said, the decoder judged on a carrier that held
        dip = mid.get("dip")
        held = dip is not None and dip < 6.0
        try:                                # the listener's audio of it all kept: tools/cw_replay replays it through
            import digimodes                # the decoder (2026-11-25: the X6200 printed 'ETE T TT' in the quiet)
            digimodes.write_raw(tci.audio()[0], os.path.join(o.run.dir, f"cw_carrier_{snd.name}_to_{lis.name}.raw"))
        except (ImportError, OSError, ValueError, AttributeError):
            pass
        ok = (not why and not quiet and bool(heard) and max(heard) >= bench.DEAF_DB and "Q1TST" in copy.upper()
              and (carrier == "" or (carrier is not None and not held)))
        if ok and carrier:
            ok = None                       # the carrier itself dipped: the decoder not judged on it, to look at
        o.res("cw-carrier", ok, f"{lis.name}'s decoder (key tone {kt} Hz): 12 s of quiet gave {quiet[-40:]!r}; "
                                f"{snd.name}'s 10 s carrier on its dial (its spectrum {heard} dB over its quiet, "
                                f"{bench.DEAF_DB} at least; {snd.name}'s meter {w.tx_peak.get(snd.name)} W; "
                                f"{snd.name} in USB-D {F_CW - 1000} Hz, the dial, mode and filter apart on purpose, "
                                f"the rest alike) gave {(carrier or '')[-40:]!r} in its steady middle 6 s (the "
                                f"carrier's deepest dip there {fmt(dip, '.1f')} dB under its median in "
                                f"{lis.name}'s audio, 20 ms blocks)"
                                f"{'' if carrier is not None else ' (NOT read: the carrier did not come)'}, "
                                f"{edges[-30:]!r} as it ended; then {text!r} at {wpm} WPM gave "
                                f"{copy[-50:]!r}; the carrier {shots_words(w)}; the CW {shots_words(w2)}"
                                + (f"; STOPPED: {why}" if why else ""))
    two_way(run, "cw-carrier", parts, body)


def t_cw_kbd(run):
    parts = [("cwkbd-message", ["cwkbd-message-pages"]), ("cwkbd-stop", ["cwkbd-send-stop-clear"])]

    def body(o):
        snd, lis = o.snd, o.lis
        listen_cw(o)
        o.prove(F_CW)
        listen_cw(o)
        wpm = key_wpm(snd.host)
        call = test_call(snd.host)
        # two V groups first: the listener's decoder finds the speed on them (with one, round 1 2026-11-23, both
        # decoders were still on the last test's speed when the call came: 'TCHTST', 'E E TS T EI')
        short = "VVV VVV {CALL}" if call else "VVV VVV Q1TST"
        want = call or "Q1TST"
        if morse_secs(f"VVV VVV {want}", wpm) > CW_MAX_S:
            raise bench.Refused(f"{snd.name}'s KEY Speed {wpm} WPM: 'VVV VVV {want}' takes "
                                f"{morse_secs('VVV VVV ' + want, wpm):.1f} s, over {CW_MAX_S:.0f} s (an over is 16 s "
                                f"at most); this test leaves the speed alone")
        long_ = " ".join(["VVV TEST"] * 7)
        ip = web_ip(snd.host)
        st, body0 = http(ip, "/api/cw_messages")
        if st != 200:
            raise bench.Refused(f"{snd.name}'s web page /api/cw_messages answered {st}")
        before = json.loads(body0)["messages"]
        if len(before) > 6:
            raise bench.Refused(f"{snd.name} has {len(before)} CW messages: the CW Keyboard shows eight, two more do "
                                f"not fit")
        mine = []

        def drop():
            for i in mine:
                http(ip, f"/api/cw_messages/{i}", method="DELETE")
            st_, b_ = http(ip, "/api/cw_messages")
            if st_ != 200 or json.loads(b_)["messages"] != before:
                raise bench.Refused(f"{snd.name}: its CW messages not as before")
        o.later(f"{snd.name}: the test's CW messages taken out", drop)
        for t in (short, long_):
            st, b_ = http(ip, "/api/cw_messages", method="POST", body={"text": t})
            if st != 200:
                raise bench.Refused(f"{snd.name}: a CW message not added ({st} {b_[:80]})")
            mine.append(max(m["id"] for m in json.loads(b_)["messages"] if m["text"] == t and m["id"] not in mine))
        order = [m["id"] for m in json.loads(http(ip, "/api/cw_messages")[1])["messages"]]
        i_short, i_long, n = order.index(mine[0]), order.index(mine[1]), len(order)
        npg = (n + 3) // 4                  # the message pages (dialog_cw_kbd.c msgs_load): page 1, these, Live
        # S in CW on the dial at 0.1 W set as L, then its CW Keyboard (it replaces the Sentry and keys by itself),
        # the two compared again with it open
        o.sender(F_CW, 4, "the CW Keyboard", page="A")
        o.later(f"{snd.name}: the CW Keyboard closed", lambda: bench.close_apps(snd))
        open_listed(snd, "cw keyboard")
        if int(snd.state.get("mode", -1)) != 4 or int(snd.state.get("freq", 0)) != F_CW:
            raise bench.Refused(f"{snd.name}: the CW Keyboard opened on {snd.state.get('freq')} mode "
                                f"{snd.state.get('mode')}, not CW on {F_CW}")
        o.apps_alike("the CW Keyboard open")
        o.app_gate(snd)
        o.banner(f"CW KEYBOARD: {snd.name.upper()} KEYS ITS MESSAGES {W} W, {lis.name.upper()} DECODES (RX ONLY)")
        page = 0                            # 0 page 1, k + 1 message page k, npg + 1 Live

        def to_page(target):
            nonlocal page
            for _ in range((target - page) % (npg + 2)):
                tap(snd, "F1")
            page = target
        w = o.watch("CW Keyboard")
        try:
            lis.cw = ""
            to_page(1 + i_short // 4)
            bench.tx_log({"who": snd.name, "f": F_CW, "w": snd.pwr(), "what": "suite air_b: CW Keyboard message"})
            tap(snd, f"F{2 + i_short % 4}")
            on = wait_for(lambda: snd.tx or w.why, 5)
            settle(snd, w)
            o.off_air(snd)
            time.sleep(2.5)
            got = " ".join(lis.cw.split())
            ok = on and not w.why and want in got.upper()
            known = ""
            if on and not w.why and not ok and "VVV" in got.upper():
                # whose the misread is, as tci-cw tells it (cw_timing.py): the keyer late on its deadlines is the GUI's
                # (a fail); on time, an X6100 on Xiegu's factory base runs elements together now and then (cw-timing
                # x6100: said; 2026-11-25 the X6200 read its Q1TST as QCHTST, the V groups right); from clean keying a
                # misread is the decoder's (a fail)
                import cw_timing as ct
                import suite_air_a as aa
                keyer = ct.keyer_said(lambda h, c: bench.ssh(h, c, timeout=15), snd.host, last=1)
                base = aa.base_of(snd.host) if aa.model(o.run, snd.name) == "x6100" else ""
                if keyer is not None and keyer[1]:
                    known = f"; the keyer itself late ({keyer[1]} of {keyer[0]} edges over 15 ms): the GUI's"
                elif aa.model(o.run, snd.name) == "x6100" and aa.gcfix_of(base) == 0 and keyer is not None:
                    ok = None
                    known = (f"; LOOK: the keyer on time ({keyer[0]} edges, the latest {keyer[2]} ms), {snd.name}'s "
                             f"base (Xiegu's {base or 'factory'}) moves its key edges (cw-timing x6100): said")
                else:
                    known = "; the decoder misread clean keying"
            o.res("cwkbd-message", ok,
                  f"{snd.name}'s CW Keyboard, message {i_short + 1} of {n} ({short!r}: page {2 + i_short // 4}, "
                  f"F{2 + i_short % 4}): on the air {bool(on)}, its meter {w.tx_peak.get(snd.name)} W; {lis.name}'s "
                  f"decoder {got[-60:]!r} ({want} {'copied' if want in got.upper() else 'NOT copied'}"
                  + ("" if call else "; {CALL} not tried: its callsign is not a test call") + ")" + known
                  + (f"; STOPPED: {w.why}" if w.why else ""))
            # the long one, stopped with Stop (page 1, F3) a few seconds in
            secs_long = morse_secs(long_, wpm)
            to_page(1 + i_long // 4)
            bench.tx_log({"who": snd.name, "f": F_CW, "w": snd.pwr(), "what": "suite air_b: CW Keyboard, stopped"})
            tap(snd, f"F{2 + i_long % 4}")
            on = wait_for(lambda: snd.tx or w.why, 5)
            time.sleep(2.0)
            to_page(0)
            still = snd.tx
            t_stop = time.time()
            tap(snd, "F3", 0.1)
            off = wait_for(lambda: not snd.tx, 3.0, 0.05)
            off_s = time.time() - t_stop
            o.off_air(snd)
            if not on or not still:
                o.res("cwkbd-stop", None, f"the long message ({secs_long:.0f} s at {wpm} WPM) was "
                                          f"{'never on the air' if not on else 'over before Stop'}: Stop not tried")
            else:
                o.res("cwkbd-stop", off and not w.why,
                      f"the long message ({secs_long:.0f} s at {wpm} WPM) on the air, Stop (page 1, F3): off the air "
                      f"after {off_s:.1f} s (3 s at most: the QSK delay); Clear is the screen's log (not read)"
                      + (f"; STOPPED: {w.why}" if w.why else ""))
        finally:
            w.close()                       # the overs' pictures fetched
            o.lock_again(snd)
        for name in ("cwkbd-message", "cwkbd-stop"):
            if name in o.out:
                o.out[name] = (o.out[name][0], f"{o.out[name][1]}; its two overs: {shots_words(w)}")
    two_way(run, "cw-kbd", parts, body)


# ---- the keyboard modes: RTTY made here, PSK31 and Olivia from the apps ----

def close_on_dial(r, close=None):
    """r's app closed (its line ends onto the card) and r back on the dial and mode it was on: closing puts it on the
    Sentry's own dial (G's channel, 29.6 FM), apart from the other radio while the test reads the text log for up to
    half a minute (rtty, psk, jtty, olivia, 2026-11-24). One transition"""
    f, m = int(r.state.get("freq", 0)), int(r.state.get("mode", -1))
    with bench.transition():
        (close or (lambda: bench.close_apps(r)))()
        bench.settled(r)
        bench.ensure(r, {"freq": f, "mode": m})


def open_rtty(o, r):
    """r's RTTY panel (no window, so not in the remote's apps list) from the Modes menu: RTTY is its first row, and
    the MFK turned left goes there whatever row it was on (dialog_modes.c: no wrap); its press opens it"""
    o.later(f"{r.name}: the RTTY panel closed (the Sentry back)", lambda: bench.open_page(r, "G"))
    r.send(f"open {ACTION_APP_MODES}")
    time.sleep(1.5)
    r.send("knob mfk -30")
    time.sleep(0.8)
    tap(r, "MFK")
    if not wait_for(lambda: app_name(r).lower().startswith("rtty"), 8):
        bench.close_apps(r)
        raise bench.Refused(f"{r.name}: the RTTY panel did not open from the Modes menu (app {app_name(r)!r})")
    time.sleep(1.5)


def t_rtty(run):
    import digimodes
    parts = [("rtty", ["rtty-rx-decode"])]
    TEXT = "RYRY CQ Q1TST Q1TST TEST K"

    def body(o):
        snd, lis = o.snd, o.lis
        listen_at(o, F_RTTY, 2)
        o.prove(F_RTTY)
        keep_file(o, lis, APP_FREQS)
        logs = tl_keep(o, lis)
        open_rtty(o, lis)
        f, m = int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))
        if abs(f - F_RTTY) <= 20000:
            # the panel set L's mode from its memory (USB on 7077000, S still in LSB: the two apart 6 s while the
            # over was readied, round 2 2026-11-26): S with it at once
            both_at(o, f, m)
        else:
            # the panel went where its memory said: the path proved there too, the panel closed for it (the proof
            # opens the Sentry's G on both, which closed the panel under the over, round 1 2026-11-23: L's G took the
            # over, its panel's text log got noise) and opened again, back on the same dial
            bench.open_page(lis, "G")
            listen_at(o, f, m)
            o.prove(f)
            open_rtty(o, lis)
            if (int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))) != (f, m):
                raise bench.Refused(f"{lis.name}: its RTTY panel opened again on {lis.state.get('freq')} mode "
                                    f"{lis.state.get('mode')}, not {f} mode {m}")
            both_at(o, f, m)
        rx_keys(lis)
        if not app_name(lis).lower().startswith("rtty"):
            raise bench.Refused(f"{lis.name}: its RTTY panel not open for the over ({app_name(lis)!r})")
        rows = sh(lis.host, "sqlite3 /mnt/params.db \"select name,val from params where name like 'rtty%'\"", 20)
        cfg = {k: int(v) for k, v in (ln.split("|", 1) for ln in rows.split() if "|" in ln) if v.lstrip("-").isdigit()}
        mark_low = (m in (2, 3)) == bool(cfg.get("rtty_reverse", 1))   # rtty.c tx_start: mark_hi = USB != reverse
        x = digimodes.rtty_audio(TEXT, center=cfg.get("rtty_center", 800), shift=cfg.get("rtty_shift", 450),
                                 baud=cfg.get("rtty_rate", 5000) / 100, mark_low=mark_low)
        path, secs = raw_to(o, x, "bench_rtty")
        if secs > 13.5:
            raise bench.Refused(f"the RTTY text takes {secs:.1f} s at {cfg.get('rtty_rate', 5000) / 100} Bd")
        mark = tl_sizes(lis.host)           # the over's lines from here (the first panel's noise left out)
        _, w = o.keyed("RTTY", f, m, lambda w: tci_play(snd, path, secs + 3, w))
        panel = app_name(lis)               # still open after the over (nothing closed it under it)
        time.sleep(2.0)
        close_on_dial(lis, lambda: bench.open_page(lis, "G"))   # the panel closed: its line ends onto the card
        new = tl_wait(lis.host, mark, lambda ls: any(" RTTY RX " in ln and "Q1TST" in ln for ln in ls))
        rx = [ln for ln in new if " RTTY RX " in ln]
        ok = not w.why and any("Q1TST" in ln for ln in rx)
        shown = [scrub(ln.split(" ", 5)[-1])[-90:] for ln in rx[-2:]] or "no RTTY RX line"
        o.res("rtty", ok, f"{snd.name} sent RTTY {TEXT!r} ({secs:.1f} s on {f} Hz, mode {m}, made to {lis.name}'s "
                          f"settings {cfg}; its meter {w.tx_peak.get(snd.name)} W, heard {w.heard_db.get(snd.name)} "
                          f"dB); {lis.name}'s text log: {shown} (its {panel or 'panel NOT'} open after the over); "
                          f"{shots_words(w)}"
                          + (f"; STOPPED: {w.why}" if w.why else ""))
    two_way(run, "rtty", parts, body)


def add_macros(o, mode, items):
    """The macros (name, text) added at the end of S's list for `mode` on its web page, the file kept aside and put
    back after (the GUI reads it as the app opens: add before opening): their places in the list"""
    snd = o.snd
    keep_file(o, snd, {"psk": "/mnt/psk_macros.txt", "olivia": "/mnt/olivia_macros.txt",
                       "jtty": "/mnt/jtty_macros.txt"}[mode])
    ip = web_ip(snd.host)
    st, b_ = http(ip, f"/api/macros/{mode}")
    if st != 200:
        raise bench.Refused(f"{snd.name}: /api/macros/{mode} answered {st}")
    state = json.loads(b_)
    room = state["max"] - len(state["macros"])
    if room < len(items):
        # the list full (2026-11-23: both radios' JTTY had 8 of 8): its last ones taken out for the test, the last
        # first (the file is kept aside above and put back byte for byte after: they come back)
        for m in list(reversed(state["macros"]))[:len(items) - room]:
            st, b_ = http(ip, f"/api/macros/{mode}/{m['id']}", method="DELETE")
            if st != 200:
                raise bench.Refused(f"{snd.name}: a {mode} macro not taken out for the test's ({st} {b_[:80]})")
        say(f"{snd.name}: {len(state['macros'])} {mode} macros of {state['max']}: its last {len(items) - room} out "
            f"for the test (its file put back after)")
    idx = []
    for name, text in items:
        st, b_ = http(ip, f"/api/macros/{mode}", method="POST", body={"name": name, "text": text})
        if st != 200:
            raise bench.Refused(f"{snd.name}: macro {name} not added ({st} {b_[:80]})")
        i = next((m["id"] for m in json.loads(b_)["macros"] if m["name"] == name and m["text"] == text), None)
        if i is None:
            raise bench.Refused(f"{snd.name}: macro {name} not in the list its web page gave back")
        idx.append(i)
    return idx


class Pages:
    """An app's six button pages, F1 to the next (page 1 as it opens): the macros four a page on F2-F5 from page
    first + 1 (dialog_psk.c and dialog_olivia.c: from page 3, page 1 with Stop TX on F3 and Mode on F4;
    dialog_jtty.c: from page 2)"""

    def __init__(self, r, n=6, first=2):
        self.r, self.n, self.first, self.at = r, n, first, 0

    def to(self, k):
        for _ in range((k - self.at) % self.n):
            tap(self.r, "F1")
        self.at = k

    def macro(self, i):
        self.to(self.first + i // 4)
        tap(self.r, f"F{2 + i % 4}")


def both_in_app(o, names, f0, same=None):
    """The same app open on both radios on one dial, the path proved there, the two alike (same(o), given, after each
    opening: what else of the app the two must share, Olivia's variant). Each app opens where its
    memory says and holds its dial (the remote's "set freq" is refused under it: 2026-11-23 PSK31 opened on 3.580 MHz
    on both, not f0), so: L's and S's app opened (L first) from the apps list; on one dial, the apps closed again (the
    path proof opens the Sentry's G on both: it would close them under the over), the path proved there on G in the
    app's mode, L set first, S set as L (its TX set-up at 0.1 W), then the apps opened again, back on that dial; the
    receivers said, the two compared (apps_alike). Their memories file is put back after. (the app's name, the dial)"""
    snd, lis = o.snd, o.lis
    for r in (lis, snd):
        keep_file(o, r, APP_FREQS)

    def open_both():
        for r in (lis, snd):
            o.later(f"{r.name}: its app closed", lambda r=r: bench.close_apps(r))
            open_listed(r, *names)
        if same:
            same(o)
        if app_name(snd) != app_name(lis):
            raise bench.Refused(f"the apps differ: {snd.name} {app_name(snd)}, {lis.name} {app_name(lis)}")
        return int(snd.state.get("freq", 0)), int(lis.state.get("freq", 0))
    fs, fl = open_both()
    if fs != fl:
        raise bench.Refused(f"{app_name(lis)} not on one dial ({snd.name} {fs}, {lis.name} {fl}): each app holds its "
                            f"own (its memory in {APP_FREQS})")
    f, m, app = fl, int(lis.state.get("mode", -1)), app_name(lis)
    if f != f0:
        say(f"{app} opened on {f} Hz on both (its memory), not {f0}: the test goes there")
    for r in (snd, lis):
        bench.close_apps(r)
    listen_at(o, f, m)
    o.prove(f)                          # on G: inside the app the listener sends no spectrum
    listen_at(o, f, m)
    o.sender(f, m, app)                 # both alike before the apps open again (each takes the radio as it is)
    fs, fl = open_both()
    if (fs, fl) != (f, f):
        raise bench.Refused(f"{app} opened again on {fs}/{fl} Hz, not {f}")
    rx_keys(lis)
    rx_keys(snd)
    o.apps_alike(f"{app} open on both")
    return app, f


def t_psk(run):
    parts = [("psk-macro", ["psk-macros-tx"]), ("psk-stop", ["psk-stop-tx"])]
    SHORT = "cq cq Q1TST Q1TST test k"      # no "DE <call>": the only thing the app spots to PSK Reporter
    LONG = ("ryryryry the quick brown fox jumps over the lazy dog 0123456789 " * 3)[:190].strip()

    def body(o):
        snd, lis = o.snd, o.lis
        i_short, i_long = add_macros(o, "psk", [("BENCH1", SHORT), ("BENCH2", LONG)])
        logs = {r.name: tl_keep(o, r) for r in (snd, lis)}
        app, f = both_in_app(o, ("psk",), F_PSK)     # the path proved on the apps' dial, both alike
        o.app_gate(snd)
        o.banner(f"{app}: {snd.name.upper()} SENDS ITS MACROS {W} W {f / 1e6:.4f} MHZ, {lis.name.upper()} DECODES "
                 f"(RX ONLY) THROUGH {o.total} DB")
        pg = Pages(snd)
        w = o.watch(app)
        try:
            bench.tx_log({"who": snd.name, "f": f, "w": snd.pwr(), "what": f"suite air_b: {app} macro"})
            pg.macro(i_short)
            on = wait_for(lambda: snd.tx or w.why, 5)
            settle(snd, w)
            o.off_air(snd)
            m1 = (w.tx_peak.get(snd.name), w.heard_db.get(snd.name))
            time.sleep(2.0)
            # the long one, Stop TX (page 1, F3) a few seconds in
            bench.tx_log({"who": snd.name, "f": f, "w": snd.pwr(), "what": f"suite air_b: {app} macro, stopped"})
            pg.macro(i_long)
            on2 = wait_for(lambda: snd.tx or w.why, 5)
            time.sleep(3.0)
            pg.to(0)
            still = snd.tx
            t_stop = time.time()
            tap(snd, "F3", 0.1)
            off = wait_for(lambda: not snd.tx, 2.0, 0.05)
            off_s = time.time() - t_stop
            o.off_air(snd)
        finally:
            w.close()
            o.lock_again(snd)
        close_on_dial(lis)                  # its line ends onto the card (PSK31's never pauses on noise)
        got = tl_wait(lis.host, logs[lis.name], lambda ls: any("Q1TST" in ln.upper() for ln in ls))
        rx = [ln for ln in got if f" {app} RX " in ln]
        sent = [ln for ln in tl_new(snd.host, logs[snd.name]) if f" {app} TX " in ln]
        ok = bool(on) and not w.why and any("Q1TST" in ln.upper() for ln in rx)
        o.res("psk-macro", ok, f"{snd.name}'s {app} macro BENCH1 {SHORT!r} (macro {i_short + 1}: page "
                               f"{3 + i_short // 4}, F{2 + i_short % 4}) on {f} Hz: on the air {bool(on)}, its meter "
                               f"{m1[0]} W, heard "
                               f"{m1[1]} dB; its own log {[x.split(' ', 5)[-1][:60] for x in sent[:1]]}; "
                               f"{lis.name}'s text log {[scrub(x.split(' ', 5)[-1])[-80:] for x in rx[-2:]]}; "
                               f"its two overs: {shots_words(w)}" + (f"; STOPPED: {w.why}" if w.why else ""))
        if not on2 or not still:
            o.res("psk-stop", None, f"the long macro was {'never on the air' if not on2 else 'over before Stop TX'}: "
                                    f"Stop TX not tried")
        else:
            o.res("psk-stop", off and not w.why,
                  f"the long macro on the air, Stop TX (page 1, F3): off the air after {off_s:.1f} s (2 s at most); "
                  f"its two overs: {shots_words(w)}" + (f"; STOPPED: {w.why}" if w.why else ""))
    two_way(run, "psk", parts, body)


def t_jtty(run):
    parts = [("jtty", ["jtty-txrx-macros"])]
    # a macro of the test's own on S's F-keys: JTTY's CQ key sends the radio's locator, and a CQ heard is spotted to
    # PSK Reporter (dialog_jtty.c): neither goes out here
    TEXT = "Q1TST TEST"

    def body(o):
        snd, lis = o.snd, o.lis
        (i_m,) = add_macros(o, "jtty", [("BENCH1", TEXT)])
        logs = {r.name: tl_keep(o, r) for r in (snd, lis)}
        app, f = both_in_app(o, ("jtty",), F_JTTY)     # the path proved on the apps' dial, both alike
        o.app_gate(snd)
        o.banner(f"JTTY: {snd.name.upper()} SENDS ITS MACRO {W} W {f / 1e6:.4f} MHZ, {lis.name.upper()} DECODES "
                 f"(RX ONLY) THROUGH {o.total} DB")
        pg = Pages(snd, first=1)
        w = o.watch(app)
        try:
            bench.tx_log({"who": snd.name, "f": f, "w": snd.pwr(), "what": "suite air_b: JTTY macro"})
            pg.macro(i_m)
            on = wait_for(lambda: snd.tx or w.why, 8)
            settle(snd, w)
            o.off_air(snd)
        finally:
            w.close()
            o.lock_again(snd)
        close_on_dial(lis)                  # its lines end onto the card
        got = tl_wait(lis.host, logs[lis.name], lambda ls: any(" JTTY RX " in ln and TEXT in ln.upper() for ln in ls))
        rx = [ln for ln in got if " JTTY RX " in ln]
        ok = bool(on) and not w.why and any(TEXT in ln.upper() and re.search(r" JTTY RX [+-]\d+dB ", ln) for ln in rx)
        o.res("jtty", ok, f"{snd.name}'s JTTY macro BENCH1 {TEXT!r} (macro {i_m + 1}: page {2 + i_m // 4}, "
                          f"F{2 + i_m % 4}) on {f} Hz: on the air {bool(on)}, its meter {w.tx_peak.get(snd.name)} W; "
                          f"{lis.name}'s text log {[scrub(x.split(' ', 5)[-1])[-80:] for x in rx[-2:]]} (a JTTY RX "
                          f"line with its dB asked); {shots_words(w)}" + (f"; STOPPED: {w.why}" if w.why else ""))
    two_way(run, "jtty", parts, body)


OLIVIA_VARIANTS = 11                # olivia_tx.h: Olivia 8/250, 16/500, 32/1000, Contestia 4/250 ... 32/1000
OLIVIA_N = {"OLIVIA": 3, "CONTESTIA": 8}


def olivia_variant(r):
    """r's Olivia variant (0-10) as its remote says it ("av"), None from a GUI that does not"""
    v = r.state.get("av")
    return int(v) if isinstance(v, int) and v >= 0 else None


def olivia_to(o, r, k, pages=None):
    """r's Olivia app on variant k, its Mode key (page 1's F4) tapped a step at a time, each step read back ("av"):
    a tap that did not take tapped again, three tries a step (2026-11-24: one lost tap left the two radios a variant
    apart for the rest of the test, and the next run found one in Olivia, the other in Contestia). False from a GUI
    that does not say its variant"""
    if olivia_variant(r) is None:
        return False
    for _ in range(OLIVIA_VARIANTS):
        v = olivia_variant(r)
        if v == k:
            return True
        o.off_air(r)                        # refused while sending
        if pages:
            pages.to(0)
        want = (v + 1) % OLIVIA_VARIANTS
        for _ in range(3):
            tap(r, "F4")
            if wait_for(lambda: olivia_variant(r) == want, 3):
                break
        else:
            raise bench.Refused(f"{r.name}: Olivia's Mode did not step from variant {v} to {want} in three taps")
    if olivia_variant(r) != k:
        raise bench.Refused(f"{r.name}: Olivia not on variant {k} (on {olivia_variant(r)})")
    return True


def olivia_alike(o):
    """L's Olivia on S's variant, as both open (each keeps its own while its GUI runs)"""
    if olivia_variant(o.snd) is not None:
        olivia_to(o, o.lis, olivia_variant(o.snd))


def t_olivia(run):
    parts = [("olivia-modes", ["olivia-modes-txrx"]), ("contestia-modes", ["contestia-modes-txrx"])]
    # no "DE <call>" (the app's PSK Reporter spot); four letters: the app sends "\nTEXT\n" (dialog_olivia.c send_text),
    # Olivia 8/250 3 characters a 2.048 s block after 4 run-in blocks and before 1 tail block (olivia_tx.c): 6
    # characters are 7 blocks, 14.3 s on the air; 7 would be 16.4 s, past the watch's 16
    TEXT = "TEST"

    def body(o):
        snd, lis = o.snd, o.lis
        (i_m,) = add_macros(o, "olivia", [("BENCH1", TEXT)])
        logs = {r.name: tl_keep(o, r) for r in (snd, lis)}
        app0, f = both_in_app(o, ("olivia", "contestia"), F_OLIVIA, same=olivia_alike)   # the path proved on the
        o.app_gate(snd)                                                                  # apps' dial, both alike
        pages = {r.name: Pages(r) for r in (snd, lis)}
        # both from the first variant (Olivia 8/250), each step read back on both; a GUI that does not say its variant
        # (older than "av") steps blind as before, the two compared by the app's name alone
        said = all(olivia_to(o, r, 0, pages[r.name]) for r in (snd, lis))
        pressed = 0                         # the Mode key stepped on both this many times: 11 is round to the start
        rows = []

        def mode_next():
            nonlocal pressed
            pressed += 1
            for r in (snd, lis):
                if said:
                    olivia_to(o, r, pressed % OLIVIA_VARIANTS, pages[r.name])
                else:
                    o.off_air(r)
                    pages[r.name].to(0)
                    tap(r, "F4")            # Mode, page 1 (refused while sending: off the air first)
            time.sleep(1.5)
        try:
            for k in range(OLIVIA_VARIANTS):
                if said and olivia_variant(snd) != olivia_variant(lis):
                    raise bench.Refused(f"the variants differ: {snd.name} {olivia_variant(snd)}, {lis.name} "
                                        f"{olivia_variant(lis)}")
                app = app_name(snd)
                o.banner(f"{app.upper()} VARIANT {k + 1} OF {OLIVIA_VARIANTS}: {snd.name.upper()} SENDS {W} W, "
                         f"{lis.name.upper()} DECODES (RX ONLY)")
                mark = tl_sizes(snd.host)
                o.apps_alike(f"{app} (variant {k + 1}) on both")      # a variant may set its own filter
                w = o.watch(f"{app} variant {k + 1}")
                try:
                    bench.tx_log({"who": snd.name, "f": f, "w": snd.pwr(), "what": f"suite air_b: {app} {k + 1}"})
                    pages[snd.name].macro(i_m)
                    on = wait_for(lambda: snd.tx or w.why, 6)
                    settle(snd, w)
                    o.off_air(snd)
                finally:
                    w.close()
                tx = [ln for ln in tl_wait(snd.host, mark, lambda ls: any(" TX " in x for x in ls), 6) if " TX " in ln]
                variant = tx[-1].split()[3] if tx and len(tx[-1].split()) > 3 else None
                rows.append({"variant": variant, "on": bool(on), "why": w.why, "w": w.tx_peak.get(snd.name),
                             "shots": [x for x in w.shots if x["on_air"]], "overs": len(w.shots)})
                if w.why:
                    break
                time.sleep(4.0)             # L's decoder's last block, then the variant moves on
                mode_next()
        finally:
            while pressed % OLIVIA_VARIANTS:    # both back on the variant they were on
                mode_next()
            o.lock_again(snd)
        close_on_dial(lis)                  # its lines end onto the card
        rx = [ln for ln in tl_wait(lis.host, logs[lis.name], lambda ls: False, 6) if " RX " in ln]
        for row in rows:
            row["rx"] = bool(row["variant"]) and any(f" {row['variant']} RX " in ln and TEXT in ln.upper()
                                                     for ln in rx)
        for name, fam in (("olivia-modes", "OLIVIA"), ("contestia-modes", "CONTESTIA")):
            mine = [x for x in rows if (x["variant"] or "").upper().startswith(fam + "-")]
            ok = len(mine) == OLIVIA_N[fam] and all(x["on"] and not x["why"] and x["rx"] for x in mine)
            o.res(name, ok, f"{snd.name}'s macro {TEXT!r} on {f} Hz in each {fam.title()} variant, {lis.name}'s text "
                            f"log: " + ("; ".join(f"{x['variant']} " + ("copied" if x["rx"] else "NOT copied" if x["on"]
                                                                       else "never on the air")
                                                  + (f" (STOPPED: {x['why']})" if x["why"] else "") for x in mine)
                                        or "none sent")
                            + f" ({len(mine)} of {OLIVIA_N[fam]}; variants unread from {snd.name}'s log: "
                              f"{sum(1 for x in rows if not x['variant'])}; the Mode key pressed {pressed} times on "
                              f"both, round to where they were); pictured on the air: "
                            + ", ".join(f"{x['variant']} {len(x['shots'])}/{x['overs']}" for x in mine)
                            + (" (" + ", ".join(os.path.basename(p) for p in mine[0]["shots"][0]["paths"].values()
                                                if p) + ")" if mine and mine[0]["shots"] else ""))
    two_way(run, "olivia", parts, body)


# ---- WeFax: a chart made here (120 LPM, IOC 576), the listener's WeFax app ----
FAX_FS = 24000
FAX_LINE_S = 0.5                    # 120 LPM


def fax_audio(segments, amp=0.5):
    """WeFax audio as wefax/decoder.h takes it: FM, black 1075 Hz, white 1925 Hz (1500 +- 425). segments: [(kind, s)]:
    start (the APT start: black and white alternating at 300 Hz), stop (at 450 Hz), black, chart (each 0.5 s line
    white for its first half, black for the second), chart_q (white for its first three quarters)"""
    import numpy as np
    parts, t0 = [], 0.0
    for kind, secs in segments:
        n = int(round(secs * FAX_FS))
        t = np.arange(n) / FAX_FS + t0
        if kind in ("start", "stop"):
            grey = (np.sin(2 * np.pi * (300.0 if kind == "start" else 450.0) * t) >= 0).astype(float)
        elif kind == "black":
            grey = np.zeros(n)
        elif kind == "chart_q":                 # each line white for its first three quarters, black for the last:
            grey = ((t % FAX_LINE_S) < FAX_LINE_S * 0.75).astype(float)     # paper and ink told apart (wefax-look)
        else:
            grey = ((t % FAX_LINE_S) < FAX_LINE_S / 2).astype(float)
        parts.append(1075.0 + 850.0 * grey)
        t0 += n / FAX_FS
    x = amp * np.sin(2 * np.pi * np.cumsum(np.concatenate(parts)) / FAX_FS)
    r = int(0.01 * FAX_FS)
    x[:r] *= np.linspace(0, 1, r)
    x[-r:] *= np.linspace(1, 0, r)
    return x


def fax_contrast(local):
    """A saved fax's columns: the mean of each over its rows, sorted; the brightest 40 % against the darkest 40 % (the
    chart's two halves: about 200 grey levels; noise: under 30). None without PIL or a picture"""
    try:
        import numpy as np
        from PIL import Image
        a = np.asarray(Image.open(local).convert("L"), float)
    except (ImportError, OSError, ValueError):
        return None
    if a.ndim != 2 or a.shape[0] < 4:
        return None
    cols = np.sort(a.mean(axis=0))
    k = max(1, int(len(cols) * 0.4))
    return round(float(cols[-k:].mean() - cols[:k].mean()), 1)


def gui_pid(host):
    """The radio's GUI process (pidof x6100_gui), "" when none: a new one after an over is a GUI that restarted"""
    return sh(host, "pidof x6100_gui", 15).strip()


def fax_listen(o):
    """The path proved on G at F_FAX, then L's WeFax app open there in USB (it opens where its memory says: put back on
    F_FAX), Auto Plan off; the faxes the test saves and its memory file put back after"""
    lis = o.lis
    listen_at(o, F_FAX, 2)
    o.prove(F_FAX)
    keep_file(o, lis, APP_FREQS)
    files0 = set(sh(lis.host, "find /mnt/wefax -type f 2>/dev/null", 20).split())

    def drop():
        new = set(sh(lis.host, "find /mnt/wefax -type f 2>/dev/null", 20).split()) - files0
        if new:
            sh(lis.host, "rm -f " + " ".join(sorted(new)), 20)
        if set(sh(lis.host, "find /mnt/wefax -type f 2>/dev/null", 20).split()) - files0:
            raise bench.Refused("the test's faxes not all taken out of /mnt/wefax")
    o.later(f"{lis.name}: the test's faxes out of /mnt/wefax", drop)

    def close():
        lis.send("wefax close")
        wait_for(lambda: lis.state.get("app") != "WeFax" and last_json(lis, "wefax").get("open") != 1, 8)
    o.later(f"{lis.name}: WeFax closed", close)
    # one change: the proof left both in USB-D, the app opens on its memory in USB; back on F_FAX, S with it in USB
    # (2026-11-24: S sat 20 s in USB-D on G while L's WeFax was in USB, the files for the overs being made)
    with bench.transition():
        lis.send("wefax open")
        if not wait_for(lambda: lis.state.get("app") == "WeFax" or last_json(lis, "wefax").get("open") == 1, 10):
            raise bench.Refused(f"{lis.name}: its WeFax app did not open")
        time.sleep(1.5)
        if last_json(lis, "wefax").get("plan"):
            raise bench.Refused(f"{lis.name}: WeFax's Auto Plan is on (it follows the day's charts off the amateur "
                                f"bands)")
        bench.ensure(lis, {"freq": F_FAX, "mode": 2})
        bench.ensure(o.snd, {"split": 0, "vfo": 0, "freq": F_FAX, "mode": 2})
    rx_keys(lis)


class FaxLog:
    """L's WeFax state sampled every 0.2 s: [(when, rows, saves, fax, gen, auto, saved)]"""

    def __init__(self, r):
        self.r, self.rows, self.on = r, [], True
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while self.on:
            s = last_json(self.r, "wefax")
            if s:
                self.rows.append((time.time(), s.get("rows", 0), s.get("saves", 0), s.get("fax", 0), s.get("gen", 0),
                                  s.get("auto", 0), s.get("saved", "")))
            time.sleep(0.2)

    def between(self, t0, t1):
        return [x for x in self.rows if t0 <= x[0] <= t1]

    def stop(self):
        self.on = False


def fetch_fax(o, name):
    """The saved fax `name` (/mnt/wefax) copied into the run's folder: its path here, or None"""
    if not name:
        return None
    local = os.path.join(o.run.dir, f"wefax_{o.lis.name}_{os.path.basename(name)}")
    r = subprocess.run(["scp", "-q", "-o", "BatchMode=yes", f"root@{o.lis.host}:/mnt/wefax/{os.path.basename(name)}",
                        local], capture_output=True, timeout=60)
    return local if r.returncode == 0 and os.path.exists(local) else None


def t_wefax_cont(run):
    parts = [("wefax-cont", ["wefax-rx-auto-cont"]), ("wefax-save-clear", ["wefax-save-clear"])]

    def body(o):
        snd, lis = o.snd, o.lis
        fax_listen(o)
        lis.send("wefax cont")
        if not wait_for(lambda: last_json(lis, "wefax").get("auto") == 0, 5):
            raise bench.Refused(f"{lis.name}: WeFax did not go to Cont")
        path, secs = raw_to(o, fax_audio([("chart", 12.0)]), "bench_fax")
        fl = FaxLog(lis)
        o.later("the WeFax sampler", fl.stop)
        marks = {}

        def steps():
            lis.send("wefax clear")         # Cont takes rows from the first sample: the noise's before the chart out
            time.sleep(6.5)
            marks["save"] = time.time()
            lis.send("wefax save")
            time.sleep(3.5)
            marks["clear"] = time.time()
            lis.send("wefax clear")

        def send(w):
            th = threading.Thread(target=steps)
            th.start()
            out = tci_play(snd, path, secs + 3, w)
            th.join()
            return out
        saves0 = last_json(lis, "wefax").get("saves", 0)
        pid0 = gui_pid(lis.host)
        (lines, t0, t1), w = o.keyed("WEFAX CHART (CONT)", F_FAX, 2, send)     # USB, as L's WeFax
        time.sleep(3.0)
        pid1 = gui_pid(lis.host)
        gui = "" if pid0 == pid1 else (f"; {lis.name}'s GUI RESTARTED during the over (pid {pid0} -> {pid1 or 'none'}"
                                       f"): a product bug (2026-11-23: wefax/quality.c measure() fills 41 floats into "
                                       f"a buffer of 0.81 x rows: a fax saved under 51 rows overflows the heap)")
        fl.stop()
        ts, tc = marks.get("save", t1), marks.get("clear", t1)
        r_save = max([x[1] for x in fl.between(t0, ts)] or [0])
        after_save = [x[1] for x in fl.between(ts + 0.3, ts + 2.0)]
        saved = next((x[6] for x in fl.between(ts, t1 + 3) if x[2] > saves0), None)
        r_clear = max([x[1] for x in fl.between(ts + 2.0, tc)] or [0])
        after_clear = [x[1] for x in fl.between(tc + 0.3, tc + 1.5)]
        local = fetch_fax(o, saved)
        contrast = fax_contrast(local) if local else None
        stop = f"; STOPPED: {w.why}" if w.why else ""
        o.res("wefax-cont", not w.why and r_save >= 6 and contrast is not None and contrast >= 80,
              f"{snd.name} sent 12 s of a chart, half white, half black (its meter {w.tx_peak.get(snd.name)} W, heard "
              f"{w.heard_db.get(snd.name)} dB); {lis.name}'s WeFax in Cont had {r_save} rows by the save; the PNG "
              f"{saved or 'none'}: its two halves {contrast} grey levels apart (the chart about 200, noise under "
              f"30; 80 at least asked); {shots_words(w)}" + gui + stop)
        o.res("wefax-save-clear", not w.why and saved is not None and bool(after_save) and min(after_save) <= 2 and
              r_clear >= 3 and bool(after_clear) and min(after_clear) <= 1,
              f"'wefax save' mid-fax: saves {saves0} -> {saves0 + 1 if saved else saves0} ({saved or 'nothing saved'}),"
              f" the rows {r_save} -> {min(after_save) if after_save else '-'}; 'wefax clear' with {r_clear} rows: "
              f"{min(after_clear) if after_clear else '-'} after" + gui + stop)
    two_way(run, "wefax-cont", parts, body)


def t_wefax_auto(run):
    parts = [("wefax-auto", ["wefax-rx-auto-cont"])]
    OVER_S = 13.0                           # each over's audio (the watch's limit is 16 s)
    ROWS = 205                              # Auto saves a fax of 200 rows and more (wefax_window.c AUTO_MIN_ROWS)

    def body(o):
        snd, lis = o.snd, o.lis
        fax_listen(o)
        if last_json(lis, "wefax").get("auto") != 1:
            lis.send("wefax auto")
            if not wait_for(lambda: last_json(lis, "wefax").get("auto") == 1, 5):
                raise bench.Refused(f"{lis.name}: WeFax did not go to Auto")
        st0 = last_json(lis, "wefax")
        gen0, saves0 = st0.get("gen", 0), st0.get("saves", 0)
        first, _ = raw_to(o, fax_audio([("start", 2.5), ("black", 3.0), ("chart", OVER_S - 5.5)]), "bench_fax1")
        mid, _ = raw_to(o, fax_audio([("chart", OVER_S)]), "bench_fax2")
        last, _ = raw_to(o, fax_audio([("chart", OVER_S - 3.5), ("stop", 3.5)]), "bench_fax3")
        fl = FaxLog(lis)
        o.later("the WeFax sampler", fl.stop)
        overs, whys, shots = [], [], []
        # Each over's chart on the first one's line grid (its lines 0.5 s apart from its first frame on): the decoder
        # keeps its line clock through the gaps, so an over begun at any moment drew its chart shifted (2026-11-23:
        # seven overs, seven offsets, the PNG's halves 60 grey levels apart where one grid gives about 200). Each over
        # after the first keyed by S's own clock on that grid (radio_at), the delay from there to its first frame
        # learned over by over
        off = radio_offset(snd.host)
        grid = None                         # the first over's first frame, by S's clock
        lag = 0.4                           # from radio_at to the first frame (s), as the last over had it
        phases = []
        k = 0

        def play(w, src, k):
            nonlocal grid, lag
            at = None
            if off is not None:
                now_r = time.time() + off
                at = now_r + 2.0            # the first too: its delay to its first frame learned for the second
                if grid is not None:
                    at = grid + math.ceil((now_r + 2.0 + lag - grid) / FAX_LINE_S) * FAX_LINE_S - lag
            lines, t0, t1 = tci_play(snd, src, OVER_S + 3, w, radio_at=at)
            start = next((float(x.split()[1]) for x in lines if x.startswith("start ")), None)
            if start is not None:
                if grid is None:
                    grid = start
                if at is not None:
                    lag = start - at
                ph = ((start - grid) % FAX_LINE_S) * 1000.0
                phases.append(round(ph if ph <= FAX_LINE_S * 500 else ph - FAX_LINE_S * 1000))
            return lines, t0, t1
        while True:
            k += 1
            rows_now = last_json(lis, "wefax").get("rows", 0)
            # the last over (the chart, then the stop tone) once its 9.5 s of chart and the gap before it take the
            # rows past ROWS
            src = first if k == 1 else last if rows_now + 20 >= ROWS or k >= 16 else mid
            _, w = o.keyed(f"WEFAX CHART (AUTO), OVER {k}", F_FAX, 2,          # USB, as L's WeFax
                           lambda w, src=src, k=k: play(w, src, k))
            shots.append(w)
            st = last_json(lis, "wefax")
            overs.append((k, os.path.basename(src)[6:-4], st.get("gen", 0) - gen0, st.get("rows", 0), st.get("fax"),
                          w.tx_peak.get(snd.name), w.heard_db.get(snd.name)))
            if w.why:
                whys.append(w.why)
                break
            if src == last or (k == 1 and st.get("gen", 0) == gen0):
                break                       # done, or no start taken: no point going on
            time.sleep(1.0)
        time.sleep(4.0)                     # the save at the stop tone: a worker writes the PNG
        fl.stop()
        st = last_json(lis, "wefax")
        saved = st.get("saved") if st.get("saves", 0) > saves0 else None
        local = fetch_fax(o, saved)
        contrast = fax_contrast(local) if local else None
        top = max([x[1] for x in fl.rows] or [0])
        ok = (not whys and overs[0][2] >= 1 and top >= 200 and saved is not None and st.get("rows", 1) == 0 and
              contrast is not None and contrast >= 60)
        o.res("wefax-auto", ok,
              f"{snd.name}'s overs (number, audio, starts taken, rows, fax on, its meter W, heard dB): "
              + "; ".join(" ".join(str(v) for v in x) for x in overs)
              + f"; {lis.name}'s WeFax in Auto: {top} rows at most, after the stop tone saves {saves0} -> "
                f"{st.get('saves')} ({saved or 'nothing saved'}), rows {st.get('rows')}; the PNG's two halves "
                f"{contrast} grey levels apart (60 at least asked: noise rows between the overs); each over's line "
                f"phase against the first's (ms, by {snd.name}'s clock): {phases}; pictured on the "
                f"air: {sum(1 for w in shots if any(x['on_air'] for x in w.shots))} of {len(shots)} overs ("
              + ", ".join(os.path.basename(p) for w in shots[:1] for x in w.shots[:1] for p in x["paths"].values() if p)
              + ")" + (f"; STOPPED: {whys}" if whys else ""))
    two_way(run, "wefax-auto", parts, body)


# ---- FT8: two messages at once from the sender's TCI, the listener's FT8 app ----

def gen_ft8_24k(c, text, audio_hz, run_dir):
    """gen_ft8's message (a 12 kHz WAV) at audio_hz, as float at 24 kHz"""
    import numpy as np
    wav = os.path.join(run_dir, f"ft8_{audio_hz}.wav")
    bench.gen_ft8_wav(c, text, audio_hz, wav)
    with wave.open(wav) as wv:
        rate = wv.getframerate()
        a = np.frombuffer(wv.readframes(wv.getnframes()), dtype=np.int16).astype(float) / 32768.0
    n = int(len(a) * FAX_FS / rate)
    return np.interp(np.arange(n) * rate / FAX_FS, np.arange(len(a)), a)


def ft8_open(r, proto="FT8"):
    """r's FT8 app open in `proto` (MODE stepped to it) on its 20 m dial: the steps taken, to step back"""
    r.send("ft8 open")
    r.wait("the FT8 app", lambda: r.ft8.get("open") == 1, 10)
    time.sleep(1.0)
    steps = 0
    while r.state.get("app") != proto and steps < 3:
        p = r.ft8.get("proto")
        r.send("ft8 mode")
        r.wait("FT8's mode stepped", lambda p=p: r.ft8.get("proto") != p, 5)
        time.sleep(0.8)
        steps += 1
    if r.state.get("app") != proto:
        raise bench.Refused(f"{r.name}: its FT8 app not in {proto} ({r.state.get('app')})")
    bench.ft8_to_band(r, FT_DIAL[proto])
    ctl_back(r, "after the FT8 app's band keys")
    return steps


def ft8_close(r, steps):
    """MODE stepped round to where it was (FT8, FT4, FT2: three steps round), stopped, the app closed"""
    r.send("ft8 stop")
    for _ in range((3 - steps % 3) % 3):
        p = r.ft8.get("proto")
        r.send("ft8 mode")
        try:
            r.wait("FT8's mode back", lambda p=p: r.ft8.get("proto") != p, 5)
        except bench.Refused:
            pass
        time.sleep(0.5)
    r.send("ft8 close")
    if not wait_for(lambda: r.ft8.get("open") != 1, 8):
        raise bench.Refused(f"{r.name}: its FT8 app did not close")


def next_slot_at(period, margin):
    return math.ceil((time.time() + margin) / period) * period


FT8_LEAD = 0.2                      # quiet before the signal in a file sent into an FT8 slot (s)
FT8_KEY_LAG = 0.4                   # from tci_play's radio_at to the over's first frame (s; WeFax's overs, 2026-11-23)


def t_ft8_multistream(run):
    import numpy as np
    parts = [("ft8-multistream", ["ft8-multistream-decode"])]
    MSGS = (("CQ Q8TST FN00", 1000), ("CQ Q9TST FN00", 1600))

    def body(o):
        snd, lis = o.snd, o.lis
        if not no_spots(lis.host):
            raise bench.Refused(f"{lis.name}: PSK Reporter is on (its default): its FT8 decodes would be spotted on "
                                f"the internet; switch it off at the radio for this test")
        gen = o.c.get("gen_ft8") or "gen_ft8"
        if not (os.path.exists(gen) or shutil.which(gen)):
            raise bench.Refused(f"no gen_ft8 on this computer ({gen}): bench.json's gen_ft8")
        listen_at(o, FT_DIAL["FT8"], 3)
        o.prove(FT_DIAL["FT8"])
        x = sum(gen_ft8_24k(o.c, t, hz, o.run.dir) for t, hz in MSGS)
        # gen_ft8's WAV is the whole 15 s slot, the signal 1.18 s to 13.82 s in it (2026-11-23: with 1 s more in
        # front, 16 s on the air, past an over's limit): the two signals alone, FT8_LEAD s of quiet before them
        on = np.flatnonzero(np.abs(x) > 1e-3 * max(np.abs(x).max(), 1e-9))
        x = x[max(0, on[0] - int(FT8_LEAD * FAX_FS)):on[-1] + int(0.1 * FAX_FS)]
        x = 0.45 * x / max(np.abs(x).max(), 1e-9)
        path, secs = raw_to(o, x, "bench_ft8ms")
        if secs > 14.0:
            raise bench.Refused(f"the two messages take {secs:.1f} s on the air")
        logs_prepare(o, lis, qsos=False)    # its app memories kept, its decodes not sent to loggers over UDP
        steps = ft8_open(lis, "FT8")
        o.later(f"{lis.name}: its FT8 app as found, closed", lambda: ft8_close(lis, steps))
        rx_keys(lis)
        n0 = len(lis.rows)

        def send(w):
            # keyed by S's own clock (radio_at: no SSH start-up in between) so the signal begins 0.5 s into a slot
            # (FT8's own start), FT8_KEY_LAG s from there to its first frame (2026-11-23: about 0.4 s on both)
            off = radio_offset(snd.host) or 0.0
            slot = math.ceil((time.time() + off + 3.0) / 15.0) * 15.0
            lines, t0, t1 = tci_play(snd, path, secs + 2, w, radio_at=slot + 0.5 - FT8_LEAD - FT8_KEY_LAG)
            return lines, t0, t1, slot
        # S on G set as L's FT8 app has L (its dial and mode, filter...)
        fl, ml = int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))
        if fl != FT_DIAL["FT8"]:
            raise bench.Refused(f"{lis.name}'s FT8 app on {fl} Hz, not {FT_DIAL['FT8']}")
        (lines, t0, t1, slot), w = o.keyed("FT8 TWO MESSAGES AT ONCE", fl, ml, send)
        start = next((float(x.split()[1]) for x in lines if x.startswith("start ")), None)
        calls = [t.split()[1] for t, _ in MSGS]
        wait_for(lambda: all(any(c in bench.msg(r) for r in lis.rows[n0:]) for c in calls), 12, 0.2)
        rows = lis.rows[n0:]
        got = {c: next((r for r in rows if c in bench.msg(r)), None) for c in calls}
        ok = not w.why and all(got.values())
        o.res("ft8-multistream", ok,
              f"{snd.name} sent {[t for t, _ in MSGS]} at once at {[hz for _, hz in MSGS]} Hz through its TCI (the "
              f"signals began {fmt(None if start is None else start + FT8_LEAD - slot, '+.2f')} s from the slot's "
              f"start by {snd.name}'s clock, 0.5 asked; its "
              f"meter {w.tx_peak.get(snd.name)} W); {lis.name}'s FT8 app decoded: "
              + "; ".join(f"{c}: " + (f"{scrub(bench.msg(r))!r} {r.get('snr')} dB {r.get('f')} Hz" if r else
                                      "NOT decoded") for c, r in got.items())
              + f" (all its rows: {[scrub(bench.msg(r)) for r in rows][:6]}); {shots_words(w)}"
              + (f"; STOPPED: {w.why}" if w.why else ""))
    two_way(run, "ft8-multistream", parts, body)


# ---- the FT8 and JS8 apps sending: test calls on both, nothing to the internet, the test QSOs out of every log ----

FIELD_CLEAN = r"""
import os,re,time
p={path!r}
new={new!r}
newdir={newdir!r}
d=os.path.dirname(p)
today=os.path.join(d,time.strftime('%Y%m%d',time.gmtime())+'.adi')
n=0
for f in sorted(set([p,today])):
    if not f or not os.path.exists(f): continue
    if f==p and new:
        os.remove(f); n+=1; continue
    s=open(f).read()
    m=re.search(r'(?i)<eoh>',s)
    head,body=(s[:m.end()],s[m.end():]) if m else ('',s)
    parts=re.split(r'(?i)(<eor>)',body)
    keep=[];k=0
    for i in range(0,len(parts)-1,2):
        r=parts[i]+parts[i+1]
        if re.search(r'(?i)<call:\d+(:\w)?>Q\dTST',r): k+=1
        else: keep.append(r)
    keep.append(parts[-1] if len(parts)%2 else '')
    if k:
        open(f,'w').write(head+''.join(keep)); n+=k
if newdir:
    try: os.rmdir(d)
    except OSError: pass
print('field',n)
"""


def logs_prepare(o, r, qsos=True):
    """r's app memories kept (put back after), the FT8 UDP to loggers switched off (its file kept aside and put back
    byte for byte after); with qsos, Wavelog off asked (a QSO would go to the online log) and the test QSOs taken out
    of ft_log.adi, qso_log.db and the day's Field Ops log after (suite_proto's Log test's way)"""
    keep_file(o, r, APP_FREQS)
    if qsos:
        # ft_log.adi kept aside too and put back byte for byte after the records' clean-up below (registered first:
        # it runs last): bench.clean_log takes the file's header out with its first record when that is a test QSO
        # (2026-11-23: both radios' header-only files left as one newline)
        keep_file(o, r, "/mnt/ft_log.adi")
    prep = dict(x.split("=", 1) for x in bench.ssh(r.host, LOG_PREP, 30).split() if "=" in x)

    def back():
        said = ""
        if qsos:
            b64 = base64.b64encode(FIELD_CLEAN.format(path=prep.get("path", ""), new=prep.get("file") == "no",
                                                       newdir=prep.get("dir") == "no").encode()).decode()
            said = sh(r.host, f"python3 -c \"import base64;exec(base64.b64decode('{b64}'))\"", 30).strip()
            bench.clean_log(r.host)
        if prep.get("udp") == "off":
            got = sh(r.host, "cp -p /tmp/bench_ft8_udp.conf /mnt/ft8_udp.conf && cmp /tmp/bench_ft8_udp.conf "
                             "/mnt/ft8_udp.conf && rm /tmp/bench_ft8_udp.conf && echo udp_back", 30)
            if "udp_back" not in got:
                raise bench.Refused(f"{r.name}: /mnt/ft8_udp.conf NOT put back (/tmp/bench_ft8_udp.conf has it)")
        say(f"{r.name}: " + (f"the test QSOs out of the logs ({said!r}), " if qsos else "") + "the FT8 UDP as found")
    o.later(f"{r.name}: " + ("the test QSOs out of the logs, " if qsos else "") + "the FT8 UDP back", back)
    if qsos and prep.get("wavelog") not in ("0", ""):
        raise bench.Refused(f"{r.name}: Wavelog is on: the test QSOs would go to the online log")
    if prep.get("udp") not in ("off", "none"):
        raise bench.Refused(f"{r.name}: the FT8 UDP not switched off ({prep.get('udp')})")


def apps_prepare(o, senders=None):
    """Both radios ready for their FT8 or JS8 apps without reaching anyone: a test call on each radio that sends
    (senders, both by default; never set here: the GUI restarts for that), PSK Reporter off on both (each decodes),
    Wavelog off; the FT8 UDP switched off (the file kept aside, put back byte for byte); after the test its QSOs out of
    ft_log.adi, qso_log.db and the day's Field Ops log. Their app memories put back too. The test calls, {name: call}"""
    calls = {}
    for r in o.radios:
        if not no_spots(r.host):
            raise bench.Refused(f"{r.name}: PSK Reporter is on (its default): the test calls' decodes would be "
                                f"spotted on the internet")
        if senders is not None and r not in senders:
            continue
        call = test_call(r.host)
        if not call:
            raise bench.Refused(f"{r.name}: its callsign is not a test call (bench.py testcall {r.name} Q1TST first: "
                                f"it restarts the GUI, which this batch never does)")
        calls[r.name] = call
    if len(set(calls.values())) < len(calls):
        raise bench.Refused(f"both radios have the same test call {calls[o.snd.name]}")
    for r in o.radios:
        logs_prepare(o, r)
    return calls


def ft8_set(r, key, on):
    if bool(r.ft8.get(key)) != on:
        r.send(f"ft8 {key}")
        r.wait(f"FT8's {key} {'on' if on else 'off'}", lambda: bool(r.ft8.get(key)) == on, 5)


def ft8_both(o, proto, senders=None):
    """Both FT8 apps open on their 20 m dial in proto, their Auto, Hold and TX Hz as found put back after (the
    cleanups "its FT8 app"), the receivers said; each that sends (senders, both by default) at 0.1 W and gated"""
    for r in o.radios:
        steps = ft8_open(r, proto)
        found = {"steps": steps, "auto": bool(r.ft8.get("auto")), "hold": bool(r.ft8.get("hold")),
                 "txf": r.ft8.get("txf")}

        def back(r=r, f=found):
            o.control_back(r, "as its FT8 app was put back")
            r.send("ft8 stop")
            for key in ("auto", "hold"):
                if bool(r.ft8.get(key)) != f[key]:
                    r.send(f"ft8 {key}")
                    wait_for(lambda key=key: bool(r.ft8.get(key)) == f[key], 5)
            if f["txf"] is not None and r.ft8.get("txf") != f["txf"]:
                r.send(f"ft8 txfreq {f['txf']}")
                wait_for(lambda: r.ft8.get("txf") == f["txf"], 5)
            bad = [k for k in ("auto", "hold") if bool(r.ft8.get(k)) != f[k]]
            ft8_close(r, f["steps"])
            if bad:
                raise bench.Refused(f"{r.name}: FT8's {', '.join(bad)} not as found")
        o.later(f"{r.name}: its FT8 app as found, closed", back)
        rx_keys(r)
        if senders is None or r in senders:
            o.app_gate(r)
    o.apps_alike(f"{proto} open on both")


def run_cleanups(o, mark):
    """The cleanups whose words have mark in them, run now (last first) and taken off the list"""
    mine = [c for c in o.cleanups if mark in c[0]]
    o.cleanups = [c for c in o.cleanups if mark not in c[0]]
    for what, fn in reversed(mine):
        try:
            fn()
        except Exception as e:              # every one tried, whatever one of them met
            say(f"{what}: not done ({type(e).__name__} {e})")
            o.unclean.append(f"{what}: {e}")


def is_decode(row):
    """An FT8 list row that is a decode (decode_list.h: RX_MSG 2, RX_CQ 3, RX_TO_ME 4), not the app's own line"""
    return row.get("t") in (2, 3, 4)


def calls_are(row, first, second):
    w = bench.msg(row).split()
    return len(w) >= 2 and w[0].strip("<>") == first and w[1].strip("<>") == second


def ft8_logged(host, call, since):
    """ft_log.adi's records of call logged since `since` (epoch; the records' QSO_DATE and TIME_ON are UTC): (how
    many, the last one's fields)"""
    recs = []
    for rec in adif_records(sh(host, "cat /mnt/ft_log.adi 2>/dev/null", 30)):
        if rec.get("CALL", "").upper() != call.upper():
            continue
        try:
            t = calendar.timegm(time.strptime(rec.get("QSO_DATE", "") + rec.get("TIME_ON", "")[:6], "%Y%m%d%H%M%S"))
        except ValueError:
            t = 0
        if t >= since:
            recs.append(rec)
    return len(recs), (recs[-1] if recs else {})


FT8_ADIF = {"FT8": ("FT8", ""), "FT4": ("MFSK", "FT4"), "FT2": ("MFSK", "FT2")}


def is_report(row, to, by, r_=False):
    """row is by's report to `to` ("<to> <by> -05"; r_: its R-report, "R-05")"""
    w = bench.msg(row).split()
    return calls_are(row, to, by) and len(w) >= 3 and re.match(r"^R[+-]\d\d$" if r_ else r"^[+-]\d\d$", w[2])


def is_rr73(row, to, by):
    w = bench.msg(row).split()
    return calls_are(row, to, by) and len(w) >= 3 and w[2] in ("RR73", "RRR")


def t_ft8_qso(run):
    parts = [("ft8-qso-FT8", ["ft8-auto-sequence", "ft8-auto-log"]), ("ft8-qso-FT4", ["ft4-tx-rx"]),
             ("ft8-force-save", ["ft8-force-qso-save"]), ("ft8-qso-FT2", ["ft2-tx-rx"])]

    def body(o):
        a, b = o.snd, o.lis
        calls = apps_prepare(o)
        ca, cb = calls[a.name], calls[b.name]
        both_at(o, FT_DIAL["FT8"], 3)
        for r in o.radios:
            bench.ensure(r, *bench.rx_profile(FT_DIAL["FT8"], 3, pre=0, att=0, rfg=100, agc=3))
        o.prove(FT_DIAL["FT8"], both=True)
        protos = [x for x in os.environ.get("AIR_B_PROTOS", "FT8,FT4,FT2").split(",") if x in FT_DIAL]
        for proto in protos:
            name = f"ft8-qso-{proto}"
            wa = wb = None
            try:
                ft8_both(o, proto)
                slot = (a.ft8.get("slot") or 15000) / 1000.0
                for r in o.radios:
                    ft8_set(r, "auto", True)
                o.banner(f"{proto} QSO WITH AUTO: {a.name.upper()} CALLS CQ ({ca}), {b.name.upper()} ANSWERS ({cb}), "
                         f"{W} W EACH, ONE AT A TIME, THROUGH {o.total} DB")
                wa = wb = o.watch(f"{proto} QSO", both=True)     # one Watch: the two take turns
                t0 = time.time()
                na, nb = len(a.rows), len(b.rows)
                bench.tx_log({"who": a.name, "f": FT_DIAL[proto], "w": a.pwr(), "what": f"suite air_b: {proto} CQ"})
                a.send("ft8 cq")
                b.wait(f"{a.name}'s CQ decoded", lambda: wa.why or wb.why or
                       any(calls_are(r, "CQ", ca) for r in b.rows[nb:]), 4 * slot + 6)
                cq = next((r for r in b.rows[nb:] if calls_are(r, "CQ", ca)), None)
                if not cq:
                    raise bench.Refused(f"{b.name} did not decode {a.name}'s CQ ({wa.why or wb.why or 'in time'})")
                bench.tx_log({"who": b.name, "f": FT_DIAL[proto], "w": b.pwr(), "what": f"suite air_b: {proto} reply"})
                b.send(f"ft8 reply {cq['id']}")

                def logged(r, call):
                    age = r.ft8.get("logged_s")
                    return r.ft8.get("logged") == call and age is not None and 0 <= age <= time.time() - t0

                forced = None
                if proto == "FT4":
                    # FT4's QSO: B saves it by hand (Force QSO save, page 2, F5) once both reports went, before A's
                    # RR73 comes; the RR73 then adds no second record
                    if wait_for(lambda: wa.why or wb.why or any(is_report(r, cb, ca) for r in b.rows[nb:]),
                                6 * slot + 6, 0.1) and not (wa.why or wb.why):
                        wait_for(lambda: b.tx or wa.why or wb.why, 2 * slot + 2, 0.05)     # its R-report
                        settle(b, wa, quiet=0.3, most=slot + 3)
                        # the save judged by when B logged (its state's "logged" and its age) against when A's RR73
                        # reached B's list (the row's time): round 1, 2026-11-23, read the file after two taps, a
                        # second and an SSH, and A's RR73 had come by then ("too late to tell")
                        t_tap = time.time()
                        rr_before = any(is_rr73(r, cb, ca) for r in b.rows[nb:])
                        tap(b, "F1")
                        tap(b, "F5")
                        wait_for(lambda: logged(b, ca), 4.0, 0.05)
                        t_log = time.time() - float(b.ft8.get("logged_s") or 0) if logged(b, ca) else None
                        rr = next((r for r in b.rows[nb:] if is_rr73(r, cb, ca)), None)
                        t_rr = rr.get("at") if rr else None
                        n_f, _ = ft8_logged(b.host, ca, t0 - 10)
                        late = rr_before or t_log is None or (t_rr is not None and t_rr <= t_log)
                        d_log = None if t_log is None else t_log - t_tap
                        d_rr = None if t_rr is None else t_rr - t_tap
                        forced = (n_f, late, f"the taps {t_tap - (b.tx_last or (t_tap, t_tap))[1]:.1f} s after its "
                                             f"R-report, logged {fmt(d_log, '+.1f')} s from them, {a.name}'s RR73 in "
                                             f"its list {fmt(d_rr, '+.1f')} s")

                done = wait_for(lambda: wa.why or wb.why or (logged(a, cb) and logged(b, ca)), 10 * slot + 8, 0.25)
                done = done and not (wa.why or wb.why)
                # the QSO over, A calls no CQ by itself: two slots quiet once both are off the air
                wait_for(lambda: not a.tx and not b.tx, slot + 2)
                t_q = time.time()
                quiet = not wait_for(lambda: a.tx, 2 * slot, 0.1)
                for r in o.radios:
                    r.send("ft8 stop")
                o.off_air(a)
                o.off_air(b)
                wa.close()
                wb.close()
                time.sleep(2.0)
                la, ra = ft8_logged(a.host, cb, t0 - 10)
                lb, rb = ft8_logged(b.host, ca, t0 - 10)
                mode, sub = FT8_ADIF[proto]

                def rec_ok(n, rec):
                    return (n == 1 and rec.get("MODE") == mode and rec.get("SUBMODE", "") == sub and
                            rec.get("BAND", "").lower() == "20m" and len(rec.get("TIME_ON", "")) == 6 and
                            re.match(r"^\d+\.\d{6}$", rec.get("FREQ", "")) is not None)
                seq_a = [scrub(bench.msg(r)) for r in a.rows[na:] if ca in bench.msg(r) or cb in bench.msg(r)]
                seq_b = [scrub(bench.msg(r)) for r in b.rows[nb:] if ca in bench.msg(r) or cb in bench.msg(r)]
                why = wa.why or wb.why
                ok = bool(done) and not why and rec_ok(la, ra) and rec_ok(lb, rb) and quiet
                o.res(name, ok, f"{proto} ({slot:g} s slots): {b.name} decoded {seq_b[:6]}; {a.name} decoded "
                                f"{seq_a[:6]}; logged by both {bool(done)} ({a.name} {a.ft8.get('logged')!r}, {b.name} "
                                f"{b.ft8.get('logged')!r}); ft_log.adi: {a.name} {la} record(s) {ra.get('MODE')}/"
                                f"{ra.get('SUBMODE', '')} {ra.get('BAND')} {ra.get('FREQ')} TIME_ON "
                                f"{ra.get('TIME_ON')}, {b.name} {lb} {rb.get('MODE')}/{rb.get('SUBMODE', '')} "
                                f"{rb.get('BAND')} {rb.get('FREQ')} (one each, {mode}/{sub} 20m, FREQ to 6 decimals "
                                f"asked); {a.name} quiet {round(time.time() - t_q)} s after: {quiet}; their meters "
                                f"{wa.tx_peak.get(a.name)}/{wa.tx_peak.get(b.name)} W; its overs: "
                                f"{shots_words(wa)}" + (f"; STOPPED: {why}" if why else ""))
                if proto == "FT4":
                    o.res("ft8-force-save", (forced is not None and forced[0] == 1 and not forced[1] and lb == 1
                                             and bool(done) and not why),
                          f"FT4: {b.name}'s Force QSO save once both reports went: "
                          + (f"{forced[0]} record(s) of {ca} in its ft_log.adi then ({forced[2]}): logged "
                             + ("NOT before " if forced[1] else "before ") + f"{a.name}'s RR73 came"
                             + (" (too late to tell)" if forced[1] else "")
                             + f"; after the RR73 {lb} record(s) (one asked: no second)" if forced is not None else
                             "never reached (no report decoded by it)") + (f"; STOPPED: {why}" if why else ""))
                if why:
                    break
            except bench.Refused as e:
                o.res(name, False, f"refused: {e}")
            finally:
                for w in (wa, wb):
                    if w:
                        w.close()
                for r in o.radios:
                    r.send("ft8 stop")
                run_cleanups(o, "its FT8 app")
                for r in o.radios:
                    o.lock_again(r)
    two_way(run, "ft8-qso", parts, body, listener_tx=True)


def t_ft8_stop(run):
    parts = [("ft8-stop", ["ft8-tx-call-stop"])]

    def body(o):
        a, b = o.snd, o.lis                 # B receive only: its session may not send, whatever its FT8 app is set to
        calls = apps_prepare(o, [a])
        both_at(o, FT_DIAL["FT8"], 3)
        for r in o.radios:
            bench.ensure(r, *bench.rx_profile(FT_DIAL["FT8"], 3, pre=0, att=0, rfg=100, agc=3))
        o.prove(FT_DIAL["FT8"])
        ft8_both(o, "FT8", [a])
        ft8_set(a, "auto", False)
        o.banner(f"FT8 CQ STOPPED IN ITS SLOT: {a.name.upper()} SENDS {W} W, {b.name.upper()} LISTENS (RX ONLY)")
        w = o.watch("FT8 CQ stopped", listener=b)
        try:
            nb = len(b.rows)
            bench.tx_log({"who": a.name, "f": FT_DIAL["FT8"], "w": a.pwr(), "what": "suite air_b: FT8 CQ, stopped"})
            a.send("ft8 cq")
            on = wait_for(lambda: a.tx or w.why, 34, 0.05)
            time.sleep(2.0)
            t_stop = time.time()
            a.send("ft8 stop")
            off = wait_for(lambda: not a.tx, 1.0, 0.02)
            off_s = time.time() - t_stop
            o.off_air(a)
            sending, cq_after = a.ft8.get("sending"), a.ft8.get("cq")
            # the slot's decode done (a slot ends on a 15 s mark, its decodes come 1-3 s after), and the next slot:
            # A must not go on the air again
            end = math.ceil(t_stop / 15.0) * 15.0 + 15.0 + 3.0
            again = wait_for(lambda: a.tx, max(0.0, end - time.time()), 0.1)
        finally:
            w.close()
        got = [scrub(bench.msg(r)) for r in b.rows[nb:] if calls[a.name] in bench.msg(r)]
        ok = bool(on) and off and not sending and not cq_after and not got and not again and not w.why
        o.res("ft8-stop", ok, f"{a.name}'s CQ on the air {bool(on)}, 'ft8 stop' 2 s in: off the air after "
                              f"{off_s:.2f} s (1 s at most), sending {sending}, CQ {cq_after}, on the air again "
                              f"{bool(again)}; "
                              f"{b.name} decoded of it {got or 'nothing'}; {shots_words(w)}"
                              + (f"; STOPPED: {w.why}" if w.why else ""))
        run_cleanups(o, "its FT8 app")
        for r in o.radios:
            o.lock_again(r)
    two_way(run, "ft8-stop", parts, body)


def ft8_stop_both(o):
    for r in o.radios:
        r.send("ft8 stop")
    for r in o.radios:
        o.off_air(r)


FT8_COPY_HZ = 156.25        # 25 tone steps: the X6100's FT8 app decoded the X6200's answers this far off, weak (-7 dB
                            # against +9 to +14), the real one then dropped as its duplicate (11-23 to 11-25; the X6200's
                            # log said its VFO right, and a clean TCI tone has nothing there: orch's spurs test)


def ft8_copies(x, txf):
    """An FT8 over in received audio x (TciRx, 24 kHz) sent from TX Hz txf: its 50 Hz (eight tones) found within 20 Hz
    of txf, its power, and the power over the same 50 Hz FT8_COPY_HZ below and above and 400 Hz above (the noise), each
    in dB under it. None without audio"""
    import numpy as np
    from tones import FS
    if x is None or len(x) < FS or not txf:
        return None
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    fr = np.fft.rfftfreq(len(x), 1 / FS)

    def band(a):
        m = (fr >= a - 3) & (fr <= a + 47)
        return 10 * math.log10(float(X[m].sum()) + 1e-20)
    f0 = max((txf + d for d in range(-20, 21)), key=band)
    main = band(f0)
    return {"hz": f0, "below_dbc": round(band(f0 - FT8_COPY_HZ) - main, 1),
            "above_dbc": round(band(f0 + FT8_COPY_HZ) - main, 1), "noise_dbc": round(band(f0 + 400) - main, 1)}


def t_ft8_hold(run):
    parts = [("ft8-hold", ["ft8-hold-freq"])]

    def body(o):
        a, b = o.snd, o.lis                 # A calls CQ at 1500 Hz, B answers it, its TX Hz set to 2000 first
        calls = apps_prepare(o)
        ca, cb = calls[a.name], calls[b.name]
        both_at(o, FT_DIAL["FT8"], 3)
        for r in o.radios:
            bench.ensure(r, *bench.rx_profile(FT_DIAL["FT8"], 3, pre=0, att=0, rfg=100, agc=3))
        o.prove(FT_DIAL["FT8"], both=True)
        ft8_both(o, "FT8")
        for r in o.radios:
            ft8_set(r, "auto", False)       # one CQ and one answer a round, each sent by the test
        a.send("ft8 txfreq 1500")
        a.wait("A's TX Hz 1500", lambda: a.ft8.get("txf") == 1500, 5)
        slot = (a.ft8.get("slot") or 15000) / 1000.0
        o.banner(f"FT8 HOLD: {a.name.upper()} CQ AT 1500 HZ, {b.name.upper()} ANSWERS FROM 2000 HZ (HOLD ON, THEN "
                 f"OFF), {W} W EACH, ONE AT A TIME")
        wa = wb = o.watch("FT8 Hold", both=True)          # one Watch: the two take turns
        rounds, copies = [], []
        from tones import TciRx
        tci = {r.name: TciRx(r.host) for r in (a, b)}     # each over's copies FT8_COPY_HZ off, in the other's audio
        o.later("the TCI recorders", lambda: [t.close() for t in tci.values()])
        try:
            for hold in (True, False):
                ft8_set(b, "hold", hold)
                b.send("ft8 txfreq 2000")
                b.wait("B's TX Hz 2000", lambda: b.ft8.get("txf") == 2000, 5)
                na, nb = len(a.rows), len(b.rows)
                bench.tx_log({"who": a.name, "f": FT_DIAL["FT8"], "w": a.pwr(), "what": "suite air_b: FT8 CQ (Hold)"})
                t_cq = time.time()
                a.send("ft8 cq")
                heard_cq = lambda: next((r for r in b.rows[nb:] if calls_are(r, "CQ", ca) and is_decode(r)  # noqa: E731
                                         and r.get("at", 0) > t_cq), None)
                b.wait(f"{a.name}'s CQ decoded", lambda: wa.why or wb.why or heard_cq(), 4 * slot + 6)
                cq = heard_cq()
                if not cq:
                    raise bench.Refused(f"{b.name} did not decode {a.name}'s CQ ({wa.why or wb.why})")
                copies.append((f"{a.name}'s CQ in {b.name}'s audio",
                               ft8_copies(tci[b.name].audio(t_cq, time.time())[0], a.ft8.get("txf"))))
                bench.tx_log({"who": b.name, "f": FT_DIAL["FT8"], "w": b.pwr(), "what": "suite air_b: FT8 answer"})
                t_reply = time.time()
                b.send(f"ft8 reply {cq['id']}")
                # A's decode of this answer: a decode row (the list also posts its own lines and the QSO pane's: round
                # 1, 2026-11-23, took one at 1347 Hz for the answer at 1493) come after the reply was asked
                heard = lambda: next((r for r in a.rows[na:] if calls_are(r, ca, cb) and is_decode(r)  # noqa: E731
                                      and r.get("at", 0) > t_reply), None)
                a.wait(f"{b.name}'s answer decoded", lambda: wa.why or wb.why or heard(), 4 * slot + 6)
                ans = heard()
                txf_b = b.ft8.get("txf")
                copies.append((f"{b.name}'s answer in {a.name}'s audio",
                               ft8_copies(tci[a.name].audio(t_reply, time.time())[0], txf_b)))
                ft8_stop_both(o)
                if wa.why or wb.why or not ans:
                    raise bench.Refused(f"{a.name} did not decode {b.name}'s answer ({wa.why or wb.why})")
                rounds.append((hold, cq.get("f"), ans.get("f"), txf_b))
                wait_for(lambda: not a.tx and not b.tx, slot)
        finally:
            for w in (wa, wb):
                w.close()
            ft8_stop_both(o)
        (_, cq1, at1, tx1), (_, cq2, at2, tx2) = rounds
        ok = (abs(at1 - 2000) <= 25 and tx1 == 2000 and abs(at2 - cq2) <= 25 and tx2 is not None and
              abs(tx2 - cq2) <= 10 and not (wa.why or wb.why))
        o.res("ft8-hold", ok, f"Hold on: {a.name}'s CQ at {cq1} Hz, {b.name}'s answer heard at {at1} Hz (2000 asked), "
                              f"its TX Hz {tx1}; Hold off: CQ at {cq2} Hz, the answer at {at2} Hz (the CQ's asked), "
                              f"its TX Hz {tx2}; each over's power {FT8_COPY_HZ} Hz below/above it and 400 Hz above "
                              f"(noise), dB under it: "
                              + "; ".join(f"{w}: " + (f"{c['below_dbc']}/{c['above_dbc']}/{c['noise_dbc']} at "
                                                      f"{c['hz']} Hz" if c else "no audio") for w, c in copies)
                              + f"; its overs: {shots_words(wa)}")
        run_cleanups(o, "its FT8 app")
        for r in o.radios:
            o.lock_again(r)
    two_way(run, "ft8-hold", parts, body, listener_tx=True)


def t_ft8_late(run):
    parts = [("ft8-late", ["ft8-late-qso-kept"])]

    def body(o):
        a, b = o.snd, o.lis                 # B calls CQ, A answers; B calls CQ again once its report went
        calls = apps_prepare(o)
        ca, cb = calls[a.name], calls[b.name]
        both_at(o, FT_DIAL["FT8"], 3)
        for r in o.radios:
            bench.ensure(r, *bench.rx_profile(FT_DIAL["FT8"], 3, pre=0, att=0, rfg=100, agc=3))
        o.prove(FT_DIAL["FT8"], both=True)
        ft8_both(o, "FT8")
        for r in o.radios:
            ft8_set(r, "auto", True)
        slot = (a.ft8.get("slot") or 15000) / 1000.0
        o.banner(f"FT8 LATE QSO: {b.name.upper()} CALLS CQ, {a.name.upper()} ANSWERS, {b.name.upper()} CQ AGAIN "
                 f"MID-QSO; {W} W EACH, ONE AT A TIME")
        wa = wb = o.watch("FT8 late QSO", both=True)      # one Watch: the two take turns
        stopped = lambda: wa.why or wb.why                                                  # noqa: E731
        t0 = time.time()
        try:
            na, nb = len(a.rows), len(b.rows)
            bench.tx_log({"who": b.name, "f": FT_DIAL["FT8"], "w": b.pwr(), "what": "suite air_b: FT8 CQ (late QSO)"})
            b.send("ft8 cq")
            a.wait(f"{b.name}'s CQ decoded", lambda: stopped() or any(calls_are(r, "CQ", cb) for r in a.rows[na:]),
                   4 * slot + 6)
            cq = next((r for r in a.rows[na:] if calls_are(r, "CQ", cb) and is_decode(r)), None)
            if not cq:
                raise bench.Refused(f"{a.name} did not decode {b.name}'s CQ ({stopped()})")
            bench.tx_log({"who": a.name, "f": FT_DIAL["FT8"], "w": a.pwr(), "what": "suite air_b: FT8 answer"})
            a.send(f"ft8 reply {cq['id']}")
            a.wait(f"{b.name}'s report decoded", lambda: stopped() or any(is_report(r, ca, cb) for r in a.rows[na:]),
                   6 * slot + 6)
            # CQ again, the QSO not over: A's R-report must still be logged. Not before A's R-report is 2 s on the
            # air: a CQ goes in the slot under way while it is under 1.5 s old (ft8_window.c tx_start_limit), and
            # round 2, 2026-11-23, sent it 1-2 s into A's slot: both radios on the air, the Watch stopped them
            wait_for(lambda: stopped() or a.tx, slot + 2, 0.05)
            wait_for(lambda: stopped() or (a.tx_since is not None and time.time() - a.tx_since >= 2.0), 4.0, 0.05)
            if not a.tx or stopped():
                raise bench.Refused(f"{a.name}'s R-report not on the air to send {b.name}'s CQ after it"
                                    + (f" ({stopped()})" if stopped() else ""))
            b.send("ft8 cq")
            t_cq = time.time()
            def logged_now():               # B's log of A since the test began (its age 0 just after)
                age = b.ft8.get("logged_s")
                return b.ft8.get("logged") == ca and age is not None and 0 <= age <= time.time() - t0

            done = wait_for(lambda: stopped() or (any(is_rr73(r, ca, cb) for r in a.rows[na:]) and logged_now()),
                            6 * slot + 6, 0.25)
            done = done and not stopped()
        finally:
            for w in (wa, wb):
                w.close()
            ft8_stop_both(o)
        time.sleep(2.0)
        nb_log, rec = ft8_logged(b.host, ca, t0 - 10)
        seq = [scrub(bench.msg(r)) for r in a.rows[na:] if ca in bench.msg(r) or cb in bench.msg(r)]
        o.res("ft8-late", bool(done) and nb_log == 1,
              f"{b.name} called CQ again {round(t_cq - t0)} s in, once {a.name} had its report; {a.name} decoded "
              f"{seq[:6]}; {b.name} logged {b.ft8.get('logged')!r}, {nb_log} record(s) of {ca} in its ft_log.adi "
              f"(one asked); its overs: {shots_words(wa)}" + (f"; STOPPED: {stopped()}" if stopped() else ""))
        run_cleanups(o, "its FT8 app")
        for r in o.radios:
            o.lock_again(r)
    two_way(run, "ft8-late", parts, body, listener_tx=True)


# ---- JS8 ----

def js8_open(o, r):
    def close():
        r.send("js8 stop")
        r.send("js8 close")
        if not wait_for(lambda: last_json(r, "js8").get("open") != 1 and r.state.get("app") != "JS8", 8):
            raise bench.Refused(f"{r.name}: its JS8 app did not close")
    o.later(f"{r.name}: its JS8 app closed", close)
    r.send("js8 open")
    r.wait("the JS8 app", lambda: last_json(r, "js8").get("open") == 1 or r.state.get("app") == "JS8", 10)
    time.sleep(1.5)


def js8_to(r, dial):
    """The JS8 app's dial onto dial with the band keys (it opens on its own last band and holds the dial)"""
    for _ in range(12):
        f = int(r.state.get("freq", 0))
        if f == dial:
            return
        tap(r, "BAND_DOWN" if f > dial else "BAND_UP")      # control taken back after it if it went
        r.wait("the next band", lambda f=f: int(r.state.get("freq", 0)) != f, 5)
        time.sleep(0.5)
    raise bench.Refused(f"{r.name}: the JS8 app did not come to {dial} Hz (on {r.state.get('freq')})")


def js8_msgs(r, since, **want):
    """r's js8r messages since `since` with the fields given (strings compared without case)"""
    out = []
    for t, m in lines_json(r, "js8r", since):
        if all((str(m.get(k, "")).upper() == v.upper()) if isinstance(v, str) else m.get(k) == v
               for k, v in want.items()):
            out.append(m)
    return out


def t_js8(run):
    parts = [("js8-cq", ["js8-cq"]), ("js8-heartbeat", ["js8-heartbeat"]), ("js8-directed", ["js8-directed-queries"]),
             ("js8-inbox", ["js8-inbox"]), ("js8-stop", ["js8-stop-sending"])]
    FILES = ("/mnt/js8_inbox.txt", "/mnt/js8_held.txt", "/mnt/js8call_log.adi")

    def body(o):
        a, b = o.snd, o.lis
        calls = apps_prepare(o)
        ca, cb = calls[a.name], calls[b.name]
        both_at(o, F_JS8, 2)
        for r in o.radios:
            for f in FILES:
                keep_file(o, r, f)
            bench.ensure(r, *bench.rx_profile(F_JS8, 2, pre=0, att=0, rfg=100, agc=3))
        o.prove(F_JS8, both=True)
        for r in (b, a):
            js8_open(o, r)
            js8_to(r, F_JS8)
            rx_keys(r)
            o.app_gate(r)
        o.apps_alike("JS8 open on both")
        sa, sb = last_json(a, "js8"), last_json(b, "js8")
        if sa.get("speed") != sb.get("speed") or sa.get("speed") == 3:
            raise bench.Refused(f"JS8 speeds {sa.get('speed')}/{sb.get('speed')} (0 Normal, 1 Fast, 2 Turbo, 3 Slow): "
                                f"the same on both asked, not Slow (its frames are 25 s on the air)")
        for r in o.radios:
            s = last_json(r, "js8")
            if s.get("acq") or s.get("hb"):
                raise bench.Refused(f"{r.name}: JS8's Auto CQ or its heartbeats are on (it would send by itself)")
        auto0 = {r.name: bool(last_json(r, "js8").get("auto")) for r in o.radios}

        def auto_back():
            for r in o.radios:
                if bool(last_json(r, "js8").get("auto")) != auto0[r.name]:
                    r.send("js8 auto")
                    wait_for(lambda r=r: bool(last_json(r, "js8").get("auto")) == auto0[r.name], 5)
            if any(bool(last_json(r, "js8").get("auto")) != auto0[r.name] for r in o.radios):
                raise bench.Refused("JS8's Auto not as found")
        o.later("JS8's Auto as found", auto_back)

        def auto(r, on):
            if bool(last_json(r, "js8").get("auto")) != on:
                r.send("js8 auto")
                if not wait_for(lambda: bool(last_json(r, "js8").get("auto")) == on, 5):
                    raise bench.Refused(f"{r.name}: JS8's Auto not {'on' if on else 'off'}")
        for r in o.radios:
            auto(r, False)                  # nothing answered by itself till the test asks for it
        period = {0: 15, 1: 10, 2: 6}.get(sa.get("speed"), 15)
        o.banner(f"JS8 ({period} S SLOTS): {a.name.upper()} ({ca}) AND {b.name.upper()} ({cb}) TAKE TURNS, {W} W, "
                 f"ONE AT A TIME, THROUGH {o.total} DB")
        wa = wb = o.watch("JS8", both=True)               # one Watch: the two take turns
        stopped = lambda: wa.why or wb.why                                                  # noqa: E731

        def sent(r, cmd, what):
            """cmd on r, its over waited for: on the air within three slots, then off"""
            bench.tx_log({"who": r.name, "f": F_JS8, "w": r.pwr(), "what": f"suite air_b: JS8 {what}"})
            r.send(cmd)
            on = wait_for(lambda: r.tx or stopped(), 3 * period + 8, 0.1)
            wait_for(lambda: not r.tx or stopped(), period + 4, 0.1)
            return on and not stopped()
        try:
            t = time.time()
            on = sent(a, "js8 cq", "CQ")
            wait_for(lambda: js8_msgs(b, t, fr=ca, cq=1), 8)
            got = js8_msgs(b, t, fr=ca)
            o.res("js8-cq", bool(js8_msgs(b, t, fr=ca, cq=1)) and not stopped(),
                  f"{a.name} 'js8 cq': on the air {bool(on)} (its meter {wb.tx_peak.get(a.name)} W); {b.name}'s list: "
                  f"{[scrub(m.get('x')) for m in got][:3] or 'nothing of it'}")
            t = time.time()
            on = sent(a, "js8 hbnow", "heartbeat")
            wait_for(lambda: js8_msgs(b, t, fr=ca, hb=1), 8)
            got = js8_msgs(b, t, fr=ca)
            o.res("js8-heartbeat", None if period == 6 else bool(js8_msgs(b, t, fr=ca, hb=1)) and not stopped(),
                  f"{a.name} 'js8 hbnow': on the air {bool(on)}; {b.name}'s list: "
                  f"{[scrub(m.get('x')) for m in got][:3] or 'nothing of it'}"
                  + ("; Turbo sends no heartbeats (as desktop)" if period == 6 else ""))
            # B heard by A (its CQ), then A asks B "SNR?" and B, Auto on, answers
            t = time.time()
            sent(b, "js8 cq", "CQ (to be heard)")
            wait_for(lambda: js8_msgs(a, t, fr=cb), 8)
            auto(b, True)
            a.send(f"js8 select {cb}")
            time.sleep(1.0)
            t = time.time()
            on = sent(a, f"js8 send {cb} SNR?", "SNR? to the other")
            wait_for(lambda: js8_msgs(a, t, fr=cb, me=1) or stopped(), 3 * period + 10)
            ans = js8_msgs(a, t, fr=cb, me=1)
            o.res("js8-directed", bool(ans) and any("SNR" in (m.get("x") or "").upper() for m in ans) and not stopped(),
                  f"{a.name} sent '{cb} SNR?' (on the air {bool(on)}, its note {last_json(a, 'js8').get('note')!r}); "
                  f"{b.name} with Auto answered: {[scrub(m.get('x')) for m in ans][:3] or 'nothing'} (its meter "
                  f"{wa.tx_peak.get(b.name)} W)")
            auto(b, False)
            wait_for(lambda: not b.tx, period + 4)
            # a MSG for B (its Auto off: kept, not acknowledged): a line in its inbox file (put back after)
            size0 = sh(b.host, "wc -c < /mnt/js8_inbox.txt 2>/dev/null", 15).strip()
            size0 = int(size0) if size0.isdigit() else 0
            on = sent(a, f"js8 send {cb} MSG BENCH TEST", "MSG to the other")
            new, end = "", time.time() + 3 * period + 10
            while time.time() < end and not stopped():
                new = sh(b.host, f"tail -c +{size0 + 1} /mnt/js8_inbox.txt 2>/dev/null", 15)
                if "BENCH TEST" in new.upper():
                    break
                time.sleep(2.0)
            o.res("js8-inbox", "BENCH TEST" in new.upper() and ca in new.upper() and not stopped(),
                  f"{a.name} sent '{cb} MSG BENCH TEST' (on the air {bool(on)}); {b.name}'s inbox file got "
                  f"{scrub(new)[-100:]!r}")
            # a long message, stopped on the air
            long_ = f"{cb} " + " ".join(["THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG"] * 4)
            bench.tx_log({"who": a.name, "f": F_JS8, "w": a.pwr(), "what": "suite air_b: JS8 long, stopped"})
            a.send(f"js8 send {long_}")
            on = wait_for(lambda: a.tx or stopped(), 3 * period + 8, 0.1)
            time.sleep(2.0)
            t_stop = time.time()
            a.send("js8 stop")
            off = wait_for(lambda: not a.tx, 1.5, 0.02)
            off_s = time.time() - t_stop
            o.off_air(a)
            again = wait_for(lambda: a.tx, 2 * period, 0.1)
            still_open = a.state.get("app") == "JS8" or last_json(a, "js8").get("open") == 1
            o.res("js8-stop", bool(on) and off and not again and still_open and not stopped(),
                  f"{a.name}'s long message on the air {bool(on)}, 'js8 stop' 2 s in: off the air after {off_s:.2f} s "
                  f"(1.5 s at most), its next frames {'SENT' if again else 'not sent'}, the app still open "
                  f"{still_open}")
        finally:
            for w in (wa, wb):
                w.close()
            for r in o.radios:
                r.send("js8 stop")
            for r in o.radios:
                o.off_air(r)
        why = stopped()
        for n in list(o.out):
            ok, says = o.out[n]
            o.out[n] = (False if why else ok, says + f"; the test's overs: {shots_words(wa)}"
                        + (f"; STOPPED: {why}" if why else ""))
        run_cleanups(o, "JS8's Auto")
        run_cleanups(o, "its JS8 app")
        for r in o.radios:
            o.lock_again(r)
    two_way(run, "js8", parts, body, listener_tx=True)


TESTS = (("cw-copy", t_cw_copy), ("cw-carrier", t_cw_carrier), ("cw-kbd", t_cw_kbd), ("rtty", t_rtty),
         ("psk", t_psk), ("jtty", t_jtty), ("olivia", t_olivia), ("wefax-cont", t_wefax_cont),
         ("wefax-auto", t_wefax_auto),
         ("ft8-multistream", t_ft8_multistream), ("ft8-qso", t_ft8_qso), ("ft8-stop", t_ft8_stop),
         ("ft8-hold", t_ft8_hold), ("ft8-late", t_ft8_late), ("js8", t_js8))


def batch(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE air_b: {name} (over the cable, 0.1 W through 80 dB: one sends, one listens)")
            fn(run)
