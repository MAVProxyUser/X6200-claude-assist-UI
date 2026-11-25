"""The 1:1 suite's "cover_b" batch: the Sentry pages and the receiver core, features of features.json not reached by the other batches,
each tried on both radios and compared, receive only (see suite_cover.py for the rules and the helpers).

    suite.py run cover_b [--only TEST,...]

Read what it reads: the remote's state, /mnt/gui_state.conf (the Sentry's own choices and scans, written within 2 s of
a change: F's item s.sys, D's band s.bsel, E's type and row s.dsel/s.drow, A's routine s.rout, the scans' style and
step time, the sweep marks, G's 3D), params.db (A's LISTEN, START IN, the knob binds, the B rows of SCAN CFG), the web's
/api/keys and /api/df, SSH. Nothing transmits: no PTT, no TUNE, no A/B (it arms on A and G), no ATU hold, never F2 on
F's BASE FIRMWARE (every F2 on F only after F's choice reads back as the item meant). Each test puts back what it
changes that per_radio's finish does not (the step, a lock, a mute, a key's action, A's LISTEN, SCAN CFG's rows, the
lists' choices, page I's settings, a recording), and reads it back. A style of SCAN CFG that stops on signals (a push on
STYLE or STAY while a row is found, the styles cycled, EMERGENCY's scan) lets A lock on a signal and write it into C LOG:
the dial is put where little is heard first, and a new entry is taken out again with C's own F2 (log_put_back).

The tests (the features each covers in brackets):
  a-scan          A's F1 scan along a band: its hops from the picture's middle at the band's bottom, 0.9 of the picture
                  a step (X6200 324 kHz of 360, X6100 45 kHz of 50), the top, round again, the step time apart; F1 off
                  [sentry-a-f1-scan, sentry-x6100-a-50khz]
  manual          the VFO on A stops A's scan (MANUAL) and tunes by the step, F1 hands it back; G never tunes by itself
                  (A's scan waits there, a 'set freq' stays) and its VFO sets MANUAL [sentry-vfo-manual, g-follows-dial]
  a-span          the X6200's 'wide 1': the base's 384 kHz around the dial; the X6100 refuses it and its base is held
                  at its widest (100 kHz) while the Sentry is open, its zoom back on the classic screen
                  [sentry-a-spectrum, sentry-x6100-a-50khz]
  a-band          A's F2 from 40 M: each next band's LISTEN frequency and mode, 30 M heard on WWV 10 MHz and still counted
                  as 30 M (the next F2 goes on to 25 M BC); BAND up and down [sentry-a-f2-band]
  keys            on A: BAND up/down, CW, AM, SSB, DFN (press NR, hold NB), DFL (the filter), FST (the step), APP (F)
                  [sentry-band-mode-keys]
  gen-help        GEN's help: the next key only closes it, the one after acts [sentry-gen-help]
  x6100-keys      the X6100's PRE (held ATT), AGC (held: no split here), V/M (nothing) on A; the X6200 has none of
                  them [sentry-x6100-own-keys]
  vol-knob        VOL's push: squelch, push: RF gain, the volume again 5 s after [sentry-vol-sql-rfg-knob]
  listen          A's F4 through X6200, SDR, BOTH, OFF and round (params.db), OFF silent with the volume kept
                  [sentry-f4-listen]
  pool            APP held on A opens the SDR pool (its APP then closes it, not F); F's SDR POOL takes the MFK, F5
                  closes it [pool-panel]
  home            an app closed brings the Sentry back; F's START IN to the classic screen and back [sentry-home-start,
                  f-system-start-in]
  f-open          F's SETTINGS (closing: F again), DIGITAL MODES, APPS [f-system-settings, f-system-apps-modes]
  f-go            F's AIRSPACE (the Airspace app) and VHF/UHF (B) [f-system-airspace-vhf]
  f-classic       F's CLASSIC RADIO: the main screen, an app closed leaves it there, the Sentry tile brings it back
                  and with it the return [f-system-classic-radio]
  f-shell         F's UNIX SHELL: a /bin/sh child of the GUI, gone when closed [f-system-unix-shell]
  f-tribal        F's TRIBAL KNOWLEDGE: the MFK turns its pages (not F's list), F2 closes it [f-system-tribal-knowledge]
  c-listen        C's push plays the chosen entry on the X6200, which stays on it on A, A not scanning [c-log-listen,
                  c-log-play-stays]
  c-empty         an empty C LOG: F2 and F3 F3 change nothing (dimmed); a log with entries is not touched
                  [c-log-remove-clear]
  c-dwell         C's F5 DWELL: every scan's step time 1/2/4/8 s and round [c-log-f5-dwell]
  d-scan-listen   D's F2 LISTEN on 40 M (A, 7.150 LSB, A's LISTEN OFF set to the X6200), F1 SCAN on 17 M (A scanning
                  in it) [d-f1-scan, d-f2-listen]
  d-sweep-mark    D's F3 puts 20 M in and out of A's sweep; F4 and F5 do nothing [d-f3-sweep-mark]
  d-e-rule        what D's LISTEN and E's OPEN set: 80 M PRE off, 20 M PRE on, ATT off, AGC AUTO, the X6100's RF gain
                  100; E's CW: AGC FAST [d-e-listen-sets-x6200]
  e-emergency     EMERGENCY's 65 rows (the MFK round), CB 9 opened: 27.065 AM on A, PRE on, ATT off, AGC SLOW
                  [e-emergency-list]
  e-scan-sel      EMERGENCY's F2 SCAN SEL on LOW BAND: A through its four channels in NFM, F2 off, C LOG as found
                  [e-emergency-scan-sel]
  g-3d            G's F4: its spectrum 3D/2D (a look from v0.3.12.48: F4 is G's channel list there) [g-spectrum-f4]
  g-own           G opens on its own channel, A's dial back as it is left; CB 9 tuned on G is G's, 14.200 is not;
                  the remote's wch and set wch (memory only, 5 Hz refused) [g-own-channel, remote-g-channel]
  g-list          G's F4 CH LIST on CB: the MFK two down and its push tune CB 21 from CB 19 [g-ch-list]
  close-rtty      the remote's close closes the RTTY panel [remote-close-rtty]
  tune-power-remote  the remote's tpwr: TUNE's own power read, set, saved, clamped at 0.1 W [remote-tune-power]
  scancfg-open    F5 on A and on B opens SCAN CFG for that screen, a push changes the chosen row (put back), F5 back
                  [scancfg-open]
  scancfg-style   F1 STYLE through the five styles (step time, stay, go on after each), a row by hand: CUSTOM
                  [scancfg-style-select]
  scancfg-step    STEP TIME 4 s: A's scan hops 4 s apart [scancfg-step-time]
  scancfg-chans   F1 SCANS THE CHANNELS: A through the channel list's channels with their mode, AGC, PRE, ATT
                  [scancfg-f1-scans-channels]
  scancfg-sweep   SWEEP D'S BANDS: the X6200 hops between the bands marked in D [scancfg-sweep-d-bands]
  scancfg-astep   A: KNOB STEP: one MFK click pans A by it, for two of its steps [scancfg-knob-steps]
  scancfg-rec     F4 REC from A: a new recording in /mnt/rec (removed after) [scancfg-f4-rec]
  df-source       page I with the test source: x6x00-df -T, its state, bearings turning, fit, method, the array
                  calculator [df-test-source, df-display]
  df-keys         page I's F3 METHOD (a bearing still made), its VFO 1 kHz a click, GEN's help holding it [df-f3-method,
                  df-keys]
  df-tune         page I's FOLLOW A takes the dial, the VFO turns it off, F1 held AUTO [df-f1-tune]
  df-targets      TARGET 2 in the passband with a bearing of its own, out of it past the passband [df-targets-2-4]
  fst             FST: a press up the steps and round, held one down [radio-core-fst-step]
  key-actions     FST's hold given actions (mute, lock, VFO A/B, step up, mode next/back, AGC, FT8) on the classic
                  screen, then its own job again (Reset to own) [radio-core-key-action-list,
                  radio-core-top-side-key-actions]
  user-page       USER F1 given Mode next, the USER page from FST's hold: its F1 steps the mode [radio-core-user-page]
  page-key-hold   KEY's long press given Mute: nothing in the Sentry, the mute on the classic screen [radio-core-page-
                  key-long-press]
  agc-split       the X6100's AGC held: split on the classic screen, not with FT8 open; the X6200 has no AGC key
                  [radio-core-x6100-agc-hold-split]
  value-buttons   the classic screen's NB, NR (DFN pages), PRE, ATT, ATU (VOL 2:2) and AGC (MFK 2:4) buttons
                  [radio-core-value-buttons]
  knob-bind       a value button held three times: none -> VOL -> MFK -> none (params.db encoder_bind)
                  [input-knob-assign-hold]
  walk            the MFK held on the classic screen walks the values; a pick and a turn change the one framed
                  [radio-core-walk-values]
  freq-lock       FREQUENCY LOCK per band and mode: 20 m USB locked, CW free, USB locked again
                  [radio-core-freq-lock-per-band-mode]
  band-across     the VFO from 40 m SSB into 40 m CW takes that band's mode, PRE, ATT and AGC; VFO B stays
                  [radio-core-band-tune-across]

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import functools
import json
import re
import socket
import struct
import subprocess
import time

import bench
from suite_cover import KEEP, by_radio, wait_state  # noqa: F401
from suite_more import F_TEST, per_radio  # noqa: F401
from suite_proto import FT8_APPS, expect, fresh, http, line_after, texts  # noqa: F401

# ---- The Sentry's tables as the GUI has them (app_sentry.c, v0.3.12.47) ----

# D's bands (bands[]): name, kHz low, kHz high, LISTEN's Hz, LISTEN's mode (radio_mode_t)
BANDS = [("SHIP MF", 415, 526, 518000, 2), ("AM BC", 530, 1700, 720000, 6), ("160 M", 1800, 2000, 1900000, 0),
         ("80 M", 3500, 4000, 3850000, 0), ("SHIP 4", 4063, 4438, 4235000, 2), ("60 M BC", 4750, 5060, 4950000, 6),
         ("60 M", 5330, 5410, 5357000, 2), ("49 M BC", 5900, 6200, 6050000, 6), ("SHIP 6", 6200, 6525, 6340500, 2),
         ("40 M", 7000, 7300, 7150000, 0), ("41 M BC", 7300, 7600, 7450000, 6), ("SHIP 8", 8195, 8815, 8503900, 2),
         ("UTIL 9", 9040, 9399, 9110000, 2), ("31 M BC", 9400, 9900, 9650000, 6), ("30 M", 10100, 10150, 10000000, 6),
         ("25 M BC", 11600, 12100, 11850000, 6), ("SHIP 12", 12230, 13200, 12750000, 2),
         ("20 M", 14000, 14350, 14200000, 2), ("19 M BC", 15100, 15800, 15000000, 6),
         ("SHIP 16", 16360, 17410, 17151200, 2), ("17 M", 18068, 18168, 18120000, 2), ("15 M", 21000, 21450, 21300000, 2),
         ("12 M", 24890, 24990, 24940000, 2), ("11 M CB", 26965, 27405, 27185000, 6),
         ("10 M", 28000, 29700, 28400000, 2), ("6 M", 50000, 54000, 50125000, 2), ("FM BC", 88000, 108000, 92300000, 9),
         ("AIR", 118000, 137000, 121500000, 6), ("WX SAT", 137000, 138000, 137100000, 7),
         ("2 M", 144000, 148000, 146520000, 7), ("VHF LMR", 150000, 174000, 152000000, 7),
         ("MARINE", 156000, 162025, 156800000, 7), ("NOAA WX", 162400, 162550, 162550000, 7),
         ("1.25 M", 222000, 225000, 223500000, 7), ("SONDES", 400000, 406000, 403000000, 7),
         ("70 CM", 420000, 450000, 446000000, 7), ("ISM 433", 433050, 434790, 433920000, 7),
         ("UHF LMR", 450000, 470000, 460000000, 7), ("GMRS", 462550, 467725, 462562500, 7),
         ("ISM 915", 902000, 928000, 915000000, 7), ("UAT 978", 977000, 979000, 978000000, 7),
         ("ADS-B", 1089000, 1091000, 1090000000, 7), ("23 CM", 1240000, 1300000, 1294500000, 7)]
BANDS_N = len(BANDS)                    # 43
B = {b[0]: i for i, b in enumerate(BANDS)}
VIEW = {"x6200": 360000, "x6100": 50000}       # A's picture (VIEW_HZ): the X6200's base, the X6100's own 50 of 100 kHz
SCAN_ON = {"x6200": "10 M", "x6100": "20 M"}   # a band A's F1 takes several steps along on each radio

# F SYSTEM's items (sys_items[]); BASE FIRMWARE (9) is never opened: on the X6200 it writes the base
SYS = {"SETTINGS": 0, "APPS": 1, "MODES": 2, "AIRSPACE": 3, "VHF": 4, "POOL": 5, "GPS": 6, "CLASSIC": 7, "SHELL": 8,
       "TRIBAL": 10, "NIFOG": 11, "HOME": 12}
SYS_N, SYS_BASEFW = 13, 9

# E's types (decoders[]) and EMERGENCY's rows (emer[])
DEC_N, DEC_EMERGENCY, DEC_CW = 23, 0, 10
EMER_N, EMER_CB9, EMER_LOW_BAND = 65, 7, 52
LOW_BAND = (39460000, 39480000, 45860000, 45880000)      # LLAW1, LFIRE2, LLAW3, LFIRE4 (NFM): seldom heard

# SCAN CFG's rows, its styles (step time ms, stay, go on after), A's knob steps
CR = {"STYLE": 0, "STEP": 1, "STAY": 2, "QUIET": 3, "WHAT": 4, "PRIO": 5, "SWEEP": 6, "ASTEP": 7, "BSTEP": 8,
      "BGAIN": 9, "BSQL": 10}
CR_N = 11
STYLES = [(2000, 0, 1), (2000, 4, 1), (2000, 2, 1), (2000, 5, 1), (1000, 1, 0)]   # BROWSE SCANNER SAMPLER STAY SURVEY
STYLE_NAMES = ("BROWSE", "SCANNER", "SAMPLER", "STOP & STAY", "SURVEY", "CUSTOM")
A_STEPS = (1000, 5000, 10000, 25000, 100000)
# the rows' cycle lengths: the pushes after the first that bring a row back (B'S GAIN: read till it is back)
CR_MORE = {0: 4, 1: 3, 2: 5, 3: 3, 4: 1, 5: 1, 6: 1, 7: 4, 8: 4, 10: 4}

MODE_N = {"LSB": 0, "LSB-D": 1, "USB": 2, "USB-D": 3, "CW": 4, "CW-R": 5, "AM": 6, "NFM": 7, "SAM": 8, "WFM": 9}
AGC_N = {"OFF": 0, "SLOW": 1, "FAST": 2, "AUTO": 3}
STEPS = (10, 100, 500, 1000, 5000, 10000, 25000, 100000)    # FST's
LETTERS = tuple("ABCDEFGS")
APP_GPS = 103                           # ACTION_APP_GPS: a window that only shows (the GPS / Clock app)


def tunes(radio, hz):
    """The radio's own receiver takes hz (board.h: both 100 kHz-55 MHz; the X6200 also 87.5-137 MHz)"""
    return 100000 <= hz <= 55000000 or (radio == "x6200" and 87500000 <= hz <= 137000000)


def guarded(fn):
    """A test's own error (a timeout over SSH, a reply it did not expect) as a refusal: per_radio says it as a fail,
    finishes the radio and goes on, instead of the batch stopping"""
    @functools.wraps(fn)
    def run(*a):
        try:
            return fn(*a)
        except bench.Refused:
            raise
        except Exception as e:      # noqa: BLE001 - every error is the test's failure, said with its kind
            raise bench.Refused(f"{type(e).__name__}: {e}") from e
    return run


# ---- Reading the radio ----

def ssh(host, cmd, timeout=40):
    """suite_cover's ssh, over IPv4 only: a radio's .local name tried over IPv6 first took 5 s a connection
    (2026-11-23), and these tests read the radio over SSH many times each"""
    r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "AddressFamily=inet",
                        f"root@{host}", cmd], capture_output=True, text=True, timeout=timeout)
    return r.stdout


def keep(h, path, what):
    """suite_cover's keep() over the ssh above: 'save' a radio's file aside, or 'back' (compared, put back)"""
    return ssh(h, "set -- " + what + "\n" + KEEP.format(f=path)).strip()


def S(r, k, d=-999):
    v = r.state.get(k, d)
    try:
        return int(v)
    except (TypeError, ValueError):
        return v


def ip_of(r):
    return socket.gethostbyname(r.host)


def gstate(h):
    """/mnt/gui_state.conf as {key: int}: the Sentry's choices and scans as it keeps them (written within 2 s)"""
    d = {}
    for ln in ssh(h, "cat /mnt/gui_state.conf 2>/dev/null").splitlines():
        k, _, v = ln.partition("=")
        if v.strip().lstrip("-").isdigit():
            d[k.strip()] = int(v)
    return d


def gwait(h, cond, secs=7.0):
    """gui_state read till cond holds or secs went by: the last read"""
    end = time.time() + secs
    while True:
        g = gstate(h)
        if cond(g) or time.time() > end:
            return g
        time.sleep(0.5)


def params(h, names):
    """Settings of params.db by name ({name: text}; a setting never changed has no row), read only"""
    q = ",".join(f"'{n}'" for n in names)
    out = ssh(h, f'sqlite3 -separator "|" /mnt/params.db "select name,val from params where name in ({q})"')
    d = {}
    for ln in out.splitlines():
        k, _, v = ln.partition("|")
        if k:
            d[k] = v.strip()
    return d


def pwait(h, name, cond, secs=4.0, default=None):
    end = time.time() + secs
    while True:
        v = params(h, [name]).get(name, default)
        if cond(v) or time.time() > end:
            return v
        time.sleep(0.3)


def hear_a(h):
    """A's LISTEN (params.db sentry_hear_a): 0 X6200, 1 the SDR, 2 BOTH, 3 OFF"""
    return int(params(h, ["sentry_hear_a"]).get("sentry_hear_a", "0"))


# ---- Working the radio ----

def hold(r, key, secs=1.6):
    """A key held past its long press (1 s)"""
    r.send(f"key {key} 1")
    time.sleep(secs)
    r.send(f"key {key} 0")
    time.sleep(0.4)


def clicks(r, knob, n, gap=0.12):
    """n clicks of a knob one at a time (no spin: A's pan and the dial take each as one step)"""
    for _ in range(abs(n)):
        r.send(f"knob {knob} {1 if n > 0 else -1}")
        time.sleep(gap)


def taps(r, key, n, gap=0.3):
    for _ in range(n):
        bench.tap(r, key)
        time.sleep(gap)


def changes(r, key, secs, start=None):
    """Every new value of the state's key over secs: [(seconds from now, value)]"""
    out, t0, last = [], time.time(), start if start is not None else S(r, key)
    while time.time() - t0 < secs:
        v = S(r, key)
        if v != last:
            out.append((round(time.time() - t0, 2), v))
            last = v
        time.sleep(0.05)
    return out


def pick(r, h, key, want, n, secs=7.0):
    """A Sentry list's choice (gui_state key: F's s.sys, D's s.bsel, E's s.drow) turned by the MFK to want and read
    back. Refused if it does not get there"""
    cur = gstate(h).get(key)
    if cur is None:
        raise bench.Refused(f"{r.name}: {key} not in /mnt/gui_state.conf")
    sign = 1
    for _ in range(3):
        if cur == want:
            return cur
        d = (want - cur) % n
        if d > n // 2:
            d -= n
        start = cur
        clicks(r, "mfk", sign * d)
        cur = gwait(h, lambda g: g.get(key) == want, secs).get(key)
        if d and cur == (start - d) % n:
            sign = -sign                # the knob went the other way round the list
    if cur != want:
        raise bench.Refused(f"{r.name}: {key} reads {cur}, not {want}, after the MFK")
    return cur


def sys_open(r, h, item):
    """F SYSTEM's item opened with F2, only once F's choice reads back as that item (never BASE FIRMWARE)"""
    i = SYS[item]
    if i == SYS_BASEFW:
        raise bench.Refused("F's BASE FIRMWARE is never opened by a test")
    bench.open_page(r, "F")
    time.sleep(0.6)
    pick(r, h, "s.sys", i, SYS_N)
    if r.state.get("pg") != "F":
        raise bench.Refused(f"{r.name}: not on F (page {r.state.get('pg')!r}): F2 not pressed")
    bench.tap(r, "F2")


def sys_back(r, h, i0):
    """F's choice back where it was found"""
    if i0 is None:
        return
    bench.open_page(r, "F")
    time.sleep(0.6)
    pick(r, h, "s.sys", i0, SYS_N)


def on_classic(r):
    return r.state.get("pg") == "" and not r.state.get("win") and not r.state.get("app")


def to_classic(r, h):
    sys_open(r, h, "CLASSIC")
    r.wait("the classic radio screen", lambda: on_classic(r), 6)
    time.sleep(0.8)


def ctap(r, key, gap=0.45):
    """A key pressed on the classic screen only (its F keys are the main screen's buttons there)"""
    if not on_classic(r):
        raise bench.Refused(f"{r.name}: not on the classic screen (page {r.state.get('pg')!r}, window "
                            f"{r.state.get('win')!r}): {key} not pressed")
    bench.tap(r, key)
    time.sleep(gap)


def chold(r, key, secs=1.6):
    if not on_classic(r):
        raise bench.Refused(f"{r.name}: not on the classic screen: {key} not held")
    hold(r, key, secs)


def etype(r, h, want):
    """E's TYPE list to want by F4 (down) and F3 (up), read back (each puts the row at 0)"""
    cur = gstate(h).get("s.dsel")
    if cur is None:
        raise bench.Refused(f"{r.name}: s.dsel not in /mnt/gui_state.conf")
    d = (want - cur) % DEC_N
    key, n = ("F4", d) if d <= DEC_N // 2 else ("F3", DEC_N - d)
    taps(r, key, n, 0.25)
    g = gwait(h, lambda g: g.get("s.dsel") == want, 6)
    if g.get("s.dsel") != want:
        raise bench.Refused(f"{r.name}: E's type reads {g.get('s.dsel')}, not {want}")


def e_back(r, h, dsel0, drow0):
    """E's type and row as found: the type (away and back when it is the same one, which puts its row at 0), then
    the row by the MFK from 0"""
    if dsel0 is None:
        return
    bench.open_page(r, "E")
    time.sleep(0.6)
    g = gstate(h)
    if g.get("s.dsel") == dsel0 and g.get("s.drow") == drow0:
        return
    if g.get("s.dsel") == dsel0:
        bench.tap(r, "F4")
        gwait(h, lambda g: g.get("s.dsel") == (dsel0 + 1) % DEC_N, 5)
    etype(r, h, dsel0)
    if drow0:
        clicks(r, "mfk", drow0, 0.1)
        gwait(h, lambda g: g.get("s.drow") == drow0, 6)


def d_back(r, h, bsel0):
    if bsel0 is None:
        return
    bench.open_page(r, "D")
    time.sleep(0.6)
    pick(r, h, "s.bsel", bsel0, BANDS_N)


def hear_next(r, h, prev):
    """A's F4 (on A): LISTEN's next, read back from params.db once it moved (else what it reads)"""
    bench.tap(r, "F4")
    v = pwait(h, "sentry_hear_a", lambda v: v is not None and int(v) != prev, 5.0, str(prev))   # params.db over the X6200's WiFi: 2.5 s missed one (2026-11-24)
    return int(v)


def hear_to(r, h, want):
    """A's LISTEN (params.db) to want with A's F4, a step at a time, each read back"""
    cur = hear_a(h)
    if cur == want:
        return True
    bench.open_page(r, "A")
    for _ in range(4):
        cur = hear_next(r, h, cur)
        if cur == want:
            return True
    return hear_a(h) == want


hear_back = hear_to


def set_step(r, step):
    if S(r, "step") != step:
        r.send(f"set step {step}")
        wait_state(r, lambda s: int(s.get("step", 0)) == step, 3.0)
    return S(r, "step") == step


# ---- The web's Keys page (/api/keys): what the keys do; every change put back and read back ----

def keys_get(ip):
    st, body = http(ip, "/api/keys")
    if st != 200:
        raise bench.Refused(f"/api/keys: {st} {body[:80]}")
    return json.loads(body)


def keys_set(ip, key, val=None):
    st, body = http(ip, "/api/keys", method="POST", body={"key": key} if val is None else {"key": key, "val": val})
    if st != 200:
        raise bench.Refused(f"/api/keys {key}={val}: {st} {body[:80]}")


def top_of(k, kid):
    return next(t for t in k["top"] if t["id"] == kid)


def page_of(k, kid):
    return next(p["val"] for p in k["page"] if p["id"] == kid)


def keys_put_back(ip, k0):
    """Every key's action as k0 had it; True when they all read back so"""
    k = keys_get(ip)
    for t0 in k0["top"]:
        t = top_of(k, t0["id"])
        for w in ("press", "hold"):
            if t[w] != t0[w]:
                keys_set(ip, f"{w}_{t0['id']}", t0[w])
    for p0 in k0["page"]:
        if page_of(k, p0["id"]) != p0["val"]:
            keys_set(ip, p0["id"], p0["val"])
    for i, (u, u0) in enumerate(zip(k["user"], k0["user"])):
        if u != u0:
            keys_set(ip, f"user_f{i + 1}", u0)
    if k["lock_per_band"] != k0["lock_per_band"]:
        keys_set(ip, "lock_per_band", k0["lock_per_band"])
    k = keys_get(ip)
    return all(k[x] == k0[x] for x in ("top", "page", "user", "lock_per_band"))


def unmute(r, ip, muted0):
    """The radio's mute as found: the Mute action (radio_change_mute) on FST's hold for a moment, then FST's own"""
    if S(r, "muted") == muted0:
        return True
    k = keys_get(ip)
    h0 = top_of(k, "fst")["hold"]
    try:
        keys_set(ip, "hold_fst", 3)
        hold(r, "FST")
        wait_state(r, lambda s: int(s.get("muted", -1)) == muted0, 2.0)
    finally:
        keys_set(ip, "hold_fst", h0)
    return S(r, "muted") == muted0


# ======== A: the X6200's (and the X6100's) spectrum screen ========

def a_hops(lo, hi, view):
    """A's F1 scan along a band (a_scan_hop): the picture's middle from the band's bottom, 0.9 of the picture a step,
    the last at the top, then the bottom again; a band no wider than the picture: its middle alone"""
    half = view // 2
    if hi - lo <= view:
        return [(lo + hi) // 2]
    out = [lo + half]
    while out[-1] + half < hi:
        n = out[-1] + half * 9 // 5
        if n + half > hi:
            n = hi - half
        out.append(n)
    return out


def a_idle(r, h):
    """A's scan as found, and SCAN CFG's style: None when A is not scanning and the style is one that never stops
    (BROWSE: A only marks what it sees), else the reason the test leaves A alone. In a style that stops on signals A
    locks on one in its picture by itself and writes it into C LOG, which only C's own F2 takes out again"""
    g = gstate(h)
    if g.get("s.rout", 0) != 0:
        return f"A's routine {g.get('s.rout')} as found (scanning or sweeping): left as it was"
    if g.get("s.stay", 0) != 0:
        return (f"SCAN CFG's style stops on signals as found (stay {g.get('s.stay')}): A would lock on one and log it, "
                "left alone")
    return None


QUIET_HZ = 41000000         # A's dial while a push in SCAN CFG may switch a stopping style on for a moment: no band
                            # memory there, far from any C LOG entry, the attenuator in


def log_text(h):
    return ssh(h, "cat /mnt/sentry_log.txt 2>/dev/null")


def log_put_back(r, h, before):
    """C LOG as the test found it. A moment of a style that stops on signals (a push on STYLE or STAY, the styles
    cycled, EMERGENCY's scan) may have had A lock on one and log it: a new entry is taken out with C's own F2 once
    C's choice reads back on it. True when the file is as before (an entry heard again, its count up: False)"""
    time.sleep(2.5)
    now = log_text(h)
    if now == before:
        return True
    b = [ln for ln in before.splitlines() if ln.strip()]
    n = [ln for ln in now.splitlines() if ln.strip()]
    old = {ln.split("\t")[0] for ln in b}
    new = [i for i, ln in enumerate(n) if ln.split("\t")[0] not in old]
    if len(new) != 1 or len(n) != len(b) + 1:
        return False
    bench.open_page(r, "C")
    time.sleep(0.8)
    if pick(r, h, "s.gsel", new[0], len(n)) != new[0]:
        return False
    bench.tap(r, "F2")
    time.sleep(3.0)
    return [ln for ln in log_text(h).splitlines() if ln.strip()] == b


def t_a_scan(run):
    @guarded
    def fn(r):
        h = r.host
        name = SCAN_ON[r.name]
        _, lo, hi, listen, mode = BANDS[B[name]]
        why = a_idle(r, h)
        if why:
            return None, why
        bench.open_page(r, "A")                 # A first: from v0.3.12.48 leaving G puts A's own dial back
        bench.ensure(r, {"freq": listen, "mode": mode})
        g = gstate(h)
        dwell = g.get("s.dwell", 2000) / 1000
        cycle = a_hops(lo * 1000, hi * 1000, VIEW[r.name])
        want = cycle + [cycle[0]]
        secs = len(want) * dwell + 2.5 if dwell <= 2 else 3 * dwell + 2.5
        f0 = S(r, "freq")
        bench.tap(r, "F1")
        seen = changes(r, "freq", secs, start=f0)
        mid = gstate(h).get("s.rout")
        bench.tap(r, "F1")
        g2 = gwait(h, lambda g: g.get("s.rout") == 0, 6)
        f_off = S(r, "freq")
        time.sleep(dwell + 1.0)
        still = S(r, "freq") == f_off
        fs = [v for _, v in seen]
        k = min(len(fs), len(want))
        right = fs[:k] == want[:k] and (k == len(want) or dwell > 2)
        gaps = [round(b[0] - a[0], 1) for a, b in zip(seen, seen[1:])]
        timed = all(abs(x - dwell) <= 0.9 for x in gaps[:len(want) - 1]) and bool(gaps)
        step = VIEW[r.name] * 9 // 10
        ok = right and timed and mid == 2 and g2.get("s.rout") == 0 and still
        return ok, (f"{name} on A ({VIEW[r.name] // 1000} kHz picture): F1 went "
                    + " ".join(f"{v / 1e6:.3f}" for v in fs[:len(want) + 1])
                    + f" (the GUI's own walk: {' '.join(f'{v / 1e6:.3f}' for v in want)}; {step // 1000} kHz a step, "
                    f"round again {fs[:k] == want[:k] and k == len(want)}), {gaps[:len(want)]} s apart (step time "
                    f"{dwell:g} s); routine {mid} while it ran; F1 again: routine {g2.get('s.rout')}, the dial "
                    f"{'stayed' if still else 'MOVED'} at {f_off / 1e6:.3f}")
    per_radio(run, "cover_b", "a-scan", fn, ["sentry-a-f1-scan", "sentry-x6100-a-50khz"])


def t_manual(run):
    @guarded
    def fn(r):
        h = r.host
        name = SCAN_ON[r.name]
        _, lo, hi, listen, mode = BANDS[B[name]]
        why = a_idle(r, h)
        if why:
            return None, why
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": listen, "mode": mode})
        dwell = gstate(h).get("s.dwell", 2000) / 1000
        step = S(r, "step")
        said, ok = [], True
        # A: F1 scans, the VFO stops it where it turns (MANUAL), a step a click
        f0 = S(r, "freq")
        bench.tap(r, "F1")
        moving = wait_state(r, lambda s: int(s.get("freq", 0)) != f0, 2.5)
        time.sleep(0.4)
        fs = []
        for _ in range(3):
            fa = S(r, "freq")
            r.send("knob main 1")
            wait_state(r, lambda s, fa=fa: int(s.get("freq", 0)) != fa, 1.5)
            time.sleep(0.3)
            fs.append(S(r, "freq"))
        g = gwait(h, lambda g: g.get("s.rout") == 0, 6)
        f_m = S(r, "freq")
        time.sleep(dwell * 2 + 0.5)
        held = S(r, "freq") == f_m
        # the step where the clicks were: the scan can take the dial into another mode's part of the band (mode from
        # the band plan), whose step is its own (the zoom and step per mode): CW's 100 Hz on 20 m (orch, 2026-11-24)
        step = S(r, "step")
        by_step = abs(fs[2] - fs[1]) == step and abs(fs[1] - fs[0]) == step
        ok &= moving and g.get("s.rout") == 0 and held and by_step
        said.append(f"A: F1 scanning {moving}; the VFO 3 clicks: {' '.join(f'{v / 1e6:.4f}' for v in fs)} "
                    f"({step} Hz a click {by_step}), the scan off (routine {g.get('s.rout')}), the dial held "
                    f"{dwell * 2 + 0.5:.1f} s {held}")
        # F1 hands it back
        fr = S(r, "freq")
        bench.tap(r, "F1")
        resumed = wait_state(r, lambda s: int(s.get("freq", 0)) != fr, dwell + 2.0)
        g = gwait(h, lambda g: g.get("s.rout") == 2, 5)
        ok &= resumed and g.get("s.rout") == 2
        said.append(f"F1 again: scanning (routine {g.get('s.rout')}, the dial moving {resumed})")
        # G: A's scan waits, the dial is where it is put; G's VFO is MANUAL
        fa = S(r, "freq")
        bench.open_page(r, "G")
        time.sleep(0.5)
        fg = S(r, "freq")
        said.append(f"entering G: the dial {'kept' if fg == fa else f'to G own channel {fg / 1e6:.3f} (v0.3.12.48 on)'}")
        time.sleep(dwell * 2 + 0.5)
        frozen = S(r, "freq") == fg
        fx = (lo * 1000 + 37000) // 1000 * 1000
        r.send(f"set freq {fx}")
        wait_state(r, lambda s: int(s.get("freq", 0)) == fx, 3.0)
        time.sleep(dwell * 2 + 0.5)
        stays = S(r, "freq") == fx
        still_scan = gstate(h).get("s.rout") == 2
        r.send("knob main 1")
        turned = wait_state(r, lambda s: int(s.get("freq", 0)) != fx, 2.0)
        g = gwait(h, lambda g: g.get("s.rout") == 0, 6)
        ok &= frozen and stays and still_scan and turned and g.get("s.rout") == 0
        said.append(f"G: the dial still {frozen} ({dwell * 2 + 0.5:.1f} s, A's scan kept waiting {still_scan}); set "
                    f"{fx / 1e6:.3f}: stayed {stays}; G's VFO: tuned {turned}, A's scan off (routine "
                    f"{g.get('s.rout')}: MANUAL)")
        return ok, "; ".join(said)
    per_radio(run, "cover_b", "manual", fn, ["sentry-vfo-manual", "g-follows-dial"])


def wide_heads(r, t0):
    """The 'wide' frames (0xFFFC: n, centre, width, sequence) the bench's session got since t0"""
    out = []
    for at, _n, data in list(r.r.frames):
        if at >= t0 and data[:2] == b"\xfc\xff" and len(data) >= 16:
            out.append(struct.unpack_from("<HIII", data, 2))
    return out


def widths(r, t0):
    return sorted({w for at, _c, w, _b in list(r.r.spectra) if at >= t0})


def t_a_span(run):
    @guarded
    def fn(r):
        h = r.host
        bench.open_page(r, "A")
        time.sleep(0.5)
        if r.name == "x6200":
            f = S(r, "freq")
            t0 = time.time()
            try:
                r.send("wide 1")
                time.sleep(3.0)
            finally:
                r.send("wide 0")                # the main screen's waterfall its own again
            hs = wide_heads(r, t0)
            time.sleep(1.0)
            t1 = time.time()
            time.sleep(1.5)
            after = wide_heads(r, t1)
            ws = sorted({w for _, _, w, _ in hs})
            cs = sorted({c for _, c, _, _ in hs})
            ok = len(hs) >= 3 and all(360000 <= w <= 400000 for w in ws) and all(abs(c - f) <= 30000 for c in cs) \
                and not after
            return ok, (f"'wide 1' on A: {len(hs)} frames of the base's own spectrum, {sorted({n for n, *_ in hs})} "
                        f"bins, {ws} Hz wide (A shows the middle 360 kHz), centred {[c - f for c in cs][:3]} Hz off "
                        f"the dial; 'wide 0': {len(after)} more")
        t0 = time.time()
        r.send("wide 1")
        refused = line_after(r, t0, "err wide", 3.0)
        r.send("wide 0")
        sys0 = gstate(h).get("s.sys")
        try:
            bench.ensure(r, {"zoom": 2})
            time.sleep(0.5)
            t1 = time.time()
            time.sleep(2.0)
            w_sentry = widths(r, t1)
            to_classic(r, h)
            t2 = time.time()
            time.sleep(2.5)
            w_classic = widths(r, t2)
            bench.open_page(r, "A")
            t3 = time.time()
            time.sleep(2.5)
            w_back = widths(r, t3)
            z = S(r, "zoom")
        finally:
            sys_back(r, h, sys0)
        ok = bool(refused) and w_sentry == [100000] and bool(w_classic) and max(w_classic) < 100000 and \
            w_back == [100000] and z == 2
        return ok, (f"'wide 1': {refused!r}; zoom x2: the spectrum {w_sentry} Hz wide in the Sentry (the base held "
                    f"at its widest), {w_classic} on the classic screen (its zoom back), {w_back} in the Sentry again; "
                    f"the zoom setting {z} throughout")
    per_radio(run, "cover_b", "a-span", fn, ["sentry-a-spectrum", "sentry-x6100-a-50khz"])


def t_a_band(run):
    @guarded
    def fn(r):
        why = a_idle(r, r.host)
        if why:
            return None, why
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": 7150000, "mode": 0})
        time.sleep(0.5)
        seen, ok = [], True
        plan = [("F2", i) for i in range(B["41 M BC"], B["25 M BC"] + 1)] + \
            [("BAND_UP", B["SHIP 12"]), ("BAND_DOWN", B["25 M BC"])]
        for key, i in plan:
            name, _, _, hz, mode = BANDS[i]
            bench.tap(r, key)
            got = wait_state(r, lambda s, hz=hz: int(s.get("freq", 0)) == hz, 3.0)
            time.sleep(0.4)
            m = S(r, "mode")
            seen.append(f"{key} {name} {S(r, 'freq') / 1e6:.4f} mode {m}")
            ok &= got and m == mode and r.state.get("pg") == "A"
        return ok, ("from 40 M: " + "; ".join(seen) + " (30 M's F2 heard on WWV 10.000 MHz, its next F2 25 M BC: "
                    "WWV counted as 30 M)")
    per_radio(run, "cover_b", "a-band", fn, ["sentry-a-f2-band"])


def t_keys(run):
    @guarded
    def fn(r):
        why = a_idle(r, r.host)
        if why:
            return None, why
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": F_TEST, "mode": 2, "nr": 0, "nb": 0})
        time.sleep(0.5)
        step0 = S(r, "step")
        res = {}
        s0 = s1 = step0
        try:
            bench.tap(r, "BAND_UP")
            res["BAND_UP 19 M BC"] = wait_state(r, lambda s: int(s.get("freq", 0)) == 15000000, 3)
            bench.tap(r, "BAND_DOWN")
            res["BAND_DOWN 20 M"] = wait_state(r, lambda s: int(s.get("freq", 0)) == 14200000, 3)
            wait_state(r, lambda s: int(s.get("mode", -1)) == 2, 2)
            for key, m in (("CW", 4), ("AM", 6), ("SSB", 0), ("SSB", 2)):
                bench.tap(r, key)
                res[f"{key} mode {m}"] = wait_state(r, lambda s, m=m: int(s.get("mode", -1)) == m, 2.5)
            bench.tap(r, "DFN")
            res["DFN NR on"] = wait_state(r, lambda s: int(s.get("nr", 0)) == 1, 2)
            bench.tap(r, "DFN")
            res["DFN NR off"] = wait_state(r, lambda s: int(s.get("nr", 1)) == 0, 2)
            hold(r, "DFN")
            res["DFN held NB on"] = wait_state(r, lambda s: int(s.get("nb", 0)) == 1, 2)
            hold(r, "DFN")
            res["DFN held NB off"] = wait_state(r, lambda s: int(s.get("nb", 1)) == 0, 2)
            fhi = S(r, "fhi")
            nxt = {3000: 2400, 2400: 1800, 1800: 3000}.get(min((3000, 2400, 1800), key=lambda w: abs(w - fhi)))
            bench.tap(r, "DFL")
            res[f"DFL filter {fhi}->{nxt}"] = wait_state(r, lambda s: int(s.get("fhi", 0)) == nxt, 2)
            s0 = S(r, "step")
            bench.tap(r, "FST")
            res["FST: the next step"] = wait_state(r, lambda s: int(s.get("step", s0)) != s0, 2)
            s1 = S(r, "step")
            bench.tap(r, "APP")
            res["APP: F"] = wait_state(r, lambda s: s.get("pg") == "F", 3)
        finally:
            if S(r, "mode") != 2:
                bench.ensure(r, {"mode": 2})
            back = set_step(r, step0)
        ok = all(res.values()) and back
        return ok, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            f" (step {s0} -> {s1}, put back {back})"
    per_radio(run, "cover_b", "keys", fn, ["sentry-band-mode-keys"])


def t_gen_help(run):
    @guarded
    def fn(r):
        bench.open_page(r, "A")
        bench.ensure(r, {"nr": 0})
        time.sleep(0.5)
        bench.tap(r, "GEN")
        time.sleep(0.8)
        bench.tap(r, "DFN")             # the help shown: this only closes it
        time.sleep(1.5)
        swallowed = S(r, "nr") == 0
        bench.tap(r, "DFN")
        acts = wait_state(r, lambda s: int(s.get("nr", 0)) == 1, 2.0)
        bench.tap(r, "DFN")
        back = wait_state(r, lambda s: int(s.get("nr", 1)) == 0, 2.0)
        return swallowed and acts and back, (f"GEN, then DFN: NR unchanged {swallowed} (the key closed the help); DFN "
                                              f"again: NR on {acts}, off again {back}")
    per_radio(run, "cover_b", "gen-help", fn, ["sentry-gen-help"])


def t_x6100_keys(run):
    @guarded
    def fn(r):
        bench.open_page(r, "A")
        time.sleep(0.5)
        if r.name != "x6100":
            f0 = {k: r.state.get(k) for k in ("pre", "att", "agc", "split", "pg")}
            got = {}
            for key in ("PRE", "AGC", "VM", "LOCK"):
                t0 = time.time()
                r.send(f"key {key} 1")
                got[key] = line_after(r, t0, f"err unknown key {key}", 2.0)
            same = all(r.state.get(k) == v for k, v in f0.items())
            said = ", ".join(f"{k} {v!r}" for k, v in got.items())
            return all(got.values()) and same, f"the X6200 has no PRE, AGC, V/M or LOCK key: {said}; nothing changed {same}"
        bench.ensure(r, {"split": 0})
        pre0, att0, agc0 = S(r, "pre"), S(r, "att"), S(r, "agc")
        res = {}
        bench.tap(r, "PRE")
        res["PRE: preamp"] = wait_state(r, lambda s: int(s.get("pre", -1)) != pre0, 2)
        bench.tap(r, "PRE")
        res["PRE again"] = wait_state(r, lambda s: int(s.get("pre", -1)) == pre0, 2)
        hold(r, "PRE")
        res["PRE held: attenuator"] = wait_state(r, lambda s: int(s.get("att", -1)) != att0, 2)
        hold(r, "PRE")
        res["PRE held again"] = wait_state(r, lambda s: int(s.get("att", -1)) == att0, 2)
        bench.tap(r, "AGC")
        res["AGC: the next"] = wait_state(r, lambda s: int(s.get("agc", -1)) == (agc0 + 1) % 4, 2)
        hold(r, "AGC")
        time.sleep(0.8)
        res["AGC held: no split here"] = S(r, "split") == 0
        f0, w0 = S(r, "freq"), r.state.get("win")
        bench.tap(r, "VM")
        time.sleep(1.5)
        res["V/M: nothing"] = r.state.get("pg") == "A" and r.state.get("win") == w0 and S(r, "freq") == f0
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items())
    per_radio(run, "cover_b", "x6100-keys", fn, ["sentry-x6100-own-keys"])


def t_vol_knob(run):
    @guarded
    def fn(r):
        bench.open_page(r, "A")
        time.sleep(0.5)
        sql0, rfg0, vol0 = S(r, "sql"), S(r, "rfg"), S(r, "vol")
        bench.tap(r, "VOL")                         # VOL's push: the squelch
        time.sleep(0.5)
        r.send("knob vol 2")
        wait_state(r, lambda s: int(s.get("sql", sql0)) != sql0, 2.5)
        sql1, vol_a = S(r, "sql"), S(r, "vol")
        bench.tap(r, "VOL")                         # again: the RF gain
        time.sleep(0.5)
        r.send("knob vol -2")
        wait_state(r, lambda s: int(s.get("rfg", rfg0)) != rfg0, 2.5)
        rfg1, sql_b = S(r, "rfg"), S(r, "sql")
        time.sleep(6.0)                             # 5 s after its last turn it is the volume again
        r.send("knob vol 1")
        wait_state(r, lambda s: int(s.get("vol", vol0)) != vol0, 2.5)
        vol1, sql_c, rfg_c = S(r, "vol"), S(r, "sql"), S(r, "rfg")
        up = (sql1 - sql0, rfg1 - rfg0, vol1 - vol0)
        ok = up[0] > 0 and up[1] < 0 and up[2] > 0 and vol_a == vol0 and sql_b == sql1 and sql_c == sql1 and \
            rfg_c == rfg1
        return ok, (f"push, +2: squelch {sql0}->{sql1} (volume kept {vol_a == vol0}); push, -2: RF gain {rfg0}->{rfg1} "
                    f"(squelch kept {sql_b == sql1}); 6 s later +1: volume {vol0}->{vol1} (squelch and RF gain kept "
                    f"{sql_c == sql1 and rfg_c == rfg1})")
    per_radio(run, "cover_b", "vol-knob", fn, ["sentry-vol-sql-rfg-knob"])


def t_listen(run):
    @guarded
    def fn(r):
        h = r.host
        bench.open_page(r, "A")
        time.sleep(0.5)
        p0, vol0 = hear_a(h), S(r, "vol")
        seen = []
        try:
            for i in range(4):
                want = (p0 + i + 1) % 4
                bench.tap(r, "F4")
                v = int(pwait(h, "sentry_hear_a", lambda v, w=want: v is not None and int(v) == w, 3.0) or 0)
                time.sleep(0.5)
                seen.append((v, S(r, "heard"), S(r, "muted"), S(r, "spk"), S(r, "vol")))
        finally:
            back = hear_back(r, h, p0)
        names = ("X6200", "SDR", "BOTH", "OFF")
        order = [s[0] for s in seen] == [(p0 + i + 1) % 4 for i in range(4)]
        off = next((s for s in seen if s[0] == 3), None)
        silent = bool(off) and off[1] == 3 and (off[2] == 1 or off[3] == 0) and off[4] == vol0
        ok = order and silent and back and all(s[4] == vol0 for s in seen)
        return ok, ("F4 x4: " + ", ".join(f"{names[s[0]]} (heard {s[1]}, muted {s[2]}, speaker {s[3]})" for s in seen)
                    + f"; OFF silent with the volume kept ({vol0}) {silent}; as found {back} (no SDR plugged in: the "
                    "X6200 stands in for the SDR and BOTH)")
    per_radio(run, "cover_b", "listen", fn, ["sentry-f4-listen"])


def t_pool(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        res = {}
        try:
            bench.open_page(r, "A")
            time.sleep(0.5)
            hold(r, "APP")                          # the SDR pool's panel
            time.sleep(0.8)
            res["APP held: still A"] = r.state.get("pg") == "A"
            bench.tap(r, "APP")                     # with the panel open APP closes it (F otherwise)
            time.sleep(1.5)
            res["APP then closed the panel, not F"] = r.state.get("pg") == "A"
            bench.tap(r, "APP")
            res["APP again: F"] = wait_state(r, lambda s: s.get("pg") == "F", 3)
            sys_open(r, h, "POOL")
            time.sleep(1.0)
            clicks(r, "mfk", 1)
            g = gwait(h, lambda g: g.get("s.sys") != SYS["POOL"], 3.5)
            res["F's SDR POOL: the MFK in the panel"] = g.get("s.sys") == SYS["POOL"]
            if g.get("s.sys") == SYS["POOL"]:
                bench.tap(r, "F5")                  # closes it
                time.sleep(0.8)
                clicks(r, "mfk", 1)
                g = gwait(h, lambda g: g.get("s.sys") == SYS["GPS"], 5)
                res["F5 closed it (the MFK on F's list again)"] = g.get("s.sys") == SYS["GPS"]
        finally:
            sys_back(r, h, sys0)
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (no SDR plugged in: the panel's rows not read; G held up not tried)"
    per_radio(run, "cover_b", "pool", fn, ["pool-panel"])


def app_round(r, letter_back=True, secs=6.0):
    """The GPS / Clock app opened and closed again: (opened, page after the close)"""
    r.send(f"open {APP_GPS}")
    opened = wait_state(r, lambda s: s.get("win") == "GPS" and s.get("pg") == "", secs)
    r.send("close")
    if letter_back:
        wait_state(r, lambda s: s.get("pg") in LETTERS, secs)
    else:
        time.sleep(3.0)
    return opened, r.state.get("pg")


def t_home(run):
    @guarded
    def fn(r):
        h = r.host
        home0 = params(h, ["sentry_home"]).get("sentry_home", "1")
        if home0 != "1":
            return None, "START IN is the classic screen as found: left alone"
        sys0 = gstate(h).get("s.sys")
        res = {}
        try:
            opened, pg = app_round(r)
            res["an app closed: the Sentry again"] = opened and pg in LETTERS
            sys_open(r, h, "HOME")
            hv = pwait(h, "sentry_home", lambda v: v == "0", 4.0, "1")
            res["START IN: the classic screen"] = hv == "0"
            opened, pg = app_round(r, letter_back=False)
            res["then an app closed: the classic screen"] = opened and pg == ""
            bench.open_page(r, "F")
            time.sleep(0.6)
            if gwait(h, lambda g: g.get("s.sys") == SYS["HOME"], 4).get("s.sys") == SYS["HOME"]:
                bench.tap(r, "F2")
            hv = pwait(h, "sentry_home", lambda v: v == "1", 4.0, "0")
            res["START IN: the Sentry again"] = hv == "1"
            opened, pg = app_round(r)
            res["an app closed: the Sentry"] = opened and pg in LETTERS
        finally:
            if params(h, ["sentry_home"]).get("sentry_home", "1") != home0:
                sys_open(r, h, "HOME")
                pwait(h, "sentry_home", lambda v: v == home0, 4.0)
            sys_back(r, h, sys0)
        back = params(h, ["sentry_home"]).get("sentry_home", "1") == home0
        return all(res.values()) and back, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            f"; START IN as found {back}"
    per_radio(run, "cover_b", "home", fn, ["sentry-home-start", "f-system-start-in"])


def t_f_open(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        res, said = {}, []
        try:
            sys_open(r, h, "SETTINGS")
            res["SETTINGS: Settings"] = wait_state(r, lambda s: s.get("win") == "Settings", 6)
            r.send("close")
            wait_state(r, lambda s: s.get("pg") in LETTERS, 6)
            # the Sentry again, on A: after any app it opens on A (app_sentry.c open_at), the manual's "comes back to
            # it when an app closes"; F was this check's own guess (orch, 2026-11-24)
            res["closed: back in the Sentry"] = r.state.get("pg") in LETTERS
            said.append(f"Settings closed: page {r.state.get('pg')!r}")
            sys_open(r, h, "MODES")
            res["DIGITAL MODES: the Modes menu"] = wait_state(
                r, lambda s: s.get("pg") == "" and s.get("win") not in ("", "Sentry", None), 6)
            said.append(f"Modes: window {r.state.get('win')!r}")
            r.send("close")
            res["closed: the Sentry"] = wait_state(r, lambda s: s.get("pg") in LETTERS, 6)
            sys_open(r, h, "APPS")
            res["APPS: the APP grid (no window, the Sentry closed)"] = wait_state(
                r, lambda s: s.get("pg") == "" and not s.get("win"), 6)
            r.send("close")
            if not wait_state(r, lambda s: s.get("pg") in LETTERS, 4):
                bench.open_page(r, "F")
        finally:
            sys_back(r, h, sys0)
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (" + "; ".join(said) + ")"
    per_radio(run, "cover_b", "f-open", fn, ["f-system-settings", "f-system-apps-modes"])


def t_f_go(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        res = {}
        try:
            sys_open(r, h, "AIRSPACE")
            res["AIRSPACE: the Airspace app (H)"] = wait_state(
                r, lambda s: s.get("win") == "Airspace" and s.get("pg") == "", 6)
            bench.open_page(r, "F")
            sys_open(r, h, "VHF")
            res["VHF/UHF: B"] = wait_state(r, lambda s: s.get("pg") == "B", 5)
        finally:
            sys_back(r, h, sys0)
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items())
    per_radio(run, "cover_b", "f-go", fn, ["f-system-airspace-vhf"])


def t_f_classic(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        res = {}
        try:
            to_classic(r, h)
            res["CLASSIC RADIO: the main screen"] = on_classic(r)
            opened, pg = app_round(r, letter_back=False)
            res["an app closed: still the main screen"] = opened and pg == ""
            bench.open_page(r, "A")
            res["the Sentry opened again"] = r.state.get("pg") == "A"
            opened, pg = app_round(r)
            res["then an app closed: the Sentry"] = opened and pg in LETTERS
        finally:
            sys_back(r, h, sys0)
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items())
    per_radio(run, "cover_b", "f-classic", fn, ["f-system-classic-radio"])


KIDS = ("awk -v p=\"$(pidof x6100_gui)\" '/^Name:/{n=$2} /^PPid:/{if ($2==p) {split(FILENAME, a, \"/\"); "
        "print a[3], n}}' /proc/[0-9]*/status 2>/dev/null; true")       # the GUI's children: pid, name


def gui_kids(h):
    return {ln.split()[0]: ln.split()[1] for ln in ssh(h, KIDS).splitlines() if len(ln.split()) == 2}


def t_f_shell(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        k0 = gui_kids(h)
        new = {}
        gone = False
        try:
            sys_open(r, h, "SHELL")
            end = time.time() + 6
            while time.time() < end and not new:
                time.sleep(0.8)
                new = {p: c for p, c in gui_kids(h).items() if p not in k0 and c == "sh"}
            r.send("close")
            end = time.time() + 6
            while time.time() < end:
                time.sleep(0.8)
                if not any(p in gui_kids(h) for p in new):
                    gone = True
                    break
            if r.state.get("pg") not in LETTERS:
                bench.open_page(r, "F")
        finally:
            sys_back(r, h, sys0)
        ok = bool(new) and gone
        return ok, (f"UNIX SHELL: {len(new)} new 'sh' child(ren) of x6100_gui {sorted(new)}; closed: gone {gone} "
                    f"(nothing typed into it)")
    per_radio(run, "cover_b", "f-shell", fn, ["f-system-unix-shell"])


def t_f_tribal(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        files = ssh(h, "wc -c < /usr/share/x6100/tribal.txt 2>/dev/null; [ -s /mnt/tribal.txt ] && echo own || "
                       "echo none").split()
        paged = closed = False
        try:
            sys_open(r, h, "TRIBAL")
            time.sleep(1.0)
            clicks(r, "mfk", 1)
            g = gwait(h, lambda g: g.get("s.sys") != SYS["TRIBAL"], 3.5)
            paged = g.get("s.sys") == SYS["TRIBAL"]
            if paged:                               # only while it is open: F2 then closes it (else F2 opens)
                clicks(r, "mfk", -1)
                bench.tap(r, "F2")
                time.sleep(0.8)
                clicks(r, "mfk", 1)
                g = gwait(h, lambda g: g.get("s.sys") == SYS["HOME"], 5)
                closed = g.get("s.sys") == SYS["HOME"]
        finally:
            sys_back(r, h, sys0)
        ok = paged and closed and bool(files) and files[0].isdigit() and int(files[0]) > 0
        return ok, (f"the radio's text {files[0] if files else '?'} bytes, the owner's own notes "
                    f"{files[1] if len(files) > 1 else '?'} (not read here); TRIBAL KNOWLEDGE open: the MFK turned its "
                    f"pages, F's choice stayed {paged}; F2 closed it, the MFK on F's list again {closed} (its words "
                    "on screen not read: no text recognition)")
    per_radio(run, "cover_b", "f-tribal", fn, ["f-system-tribal-knowledge"])


# ======== C LOG ========

def size_time(h, path):
    """[bytes, mtime] of a radio's file ([] when it is not there): its BusyBox has no stat"""
    return ssh(h, f"[ -e {path} ] && echo $(wc -c < {path}) $(date -r {path} +%s)").split()


def log_rows(h):
    return [ln.split("\t") for ln in ssh(h, "cat /mnt/sentry_log.txt 2>/dev/null").splitlines() if ln.strip()]


def t_c_listen(run):
    @guarded
    def fn(r):
        h = r.host
        rows = log_rows(h)
        bench.open_page(r, "C")
        time.sleep(0.8)
        f0 = S(r, "freq")
        if not rows:
            bench.tap(r, "MFK")
            time.sleep(2.0)
            return None, (f"C LOG empty on this radio: nothing to play (the push did nothing: the dial "
                          f"{'stayed' if S(r, 'freq') == f0 else 'MOVED'})")
        sel = gstate(h).get("s.gsel", 0)
        row = rows[sel] if sel < len(rows) else rows[0]
        hz, mode, rtl = int(row[0]), int(row[1]), row[2] != "0"
        if rtl or not tunes(r.name, hz):
            return None, f"C's chosen entry ({hz / 1e6:.4f} MHz) is the SDR's: no SDR plugged in"
        dwell = gstate(h).get("s.dwell", 2000) / 1000
        bench.tap(r, "MFK")
        played = wait_state(r, lambda s: abs(int(s.get("freq", 0)) - hz) <= 1500, 4.0)
        time.sleep(0.5)
        fp, mp = S(r, "freq"), S(r, "mode")
        bench.open_page(r, "A")
        time.sleep(dwell + 1.5)
        stays = abs(S(r, "freq") - hz) <= 1500
        rout = gstate(h).get("s.rout")
        ok = played and mp == mode and stays and rout != 2
        return ok, (f"{len(rows)} entries, row {sel} chosen ({hz / 1e6:.4f} MHz, mode {mode}): the push played it on "
                    f"the radio's own receiver: {fp / 1e6:.4f} MHz mode {mp}; on A it stayed there {stays} "
                    f"({S(r, 'freq') / 1e6:.4f}), A's routine {rout} (not scanning)")
    per_radio(run, "cover_b", "c-listen", fn, ["c-log-listen", "c-log-play-stays"])


def t_c_empty(run):
    @guarded
    def fn(r):
        h = r.host
        st0 = size_time(h, "/mnt/sentry_log.txt")
        if not st0 or st0[0] != "0":
            return None, (f"C LOG holds the owner's entries ({st0[0] if st0 else '?'} bytes): F2 and F3 F3 would remove "
                          "them for good (only a GUI restart reads the file again): not pressed")
        bench.open_page(r, "C")
        time.sleep(0.8)
        f0 = S(r, "freq")
        bench.tap(r, "F2")
        time.sleep(0.6)
        bench.tap(r, "F3")
        time.sleep(0.6)
        bench.tap(r, "F3")
        time.sleep(3.0)
        st1 = size_time(h, "/mnt/sentry_log.txt")
        ok = st1 == st0 and S(r, "freq") == f0 and r.state.get("pg") == "C"
        return ok, (f"C LOG empty: F2, F3, F3 (dimmed): the file {st0} -> {st1} (size, time), the dial "
                    f"{'kept' if S(r, 'freq') == f0 else 'MOVED'}, still on C {r.state.get('pg') == 'C'} (removing "
                    "an entry not tried: there is none to remove)")
    per_radio(run, "cover_b", "c-empty", fn, ["c-log-remove-clear"])


def dwell_next(d):
    return 1000 if d >= 8000 else d * 2


def t_c_dwell(run):
    @guarded
    def fn(r):
        h = r.host
        bench.open_page(r, "C")
        time.sleep(0.6)
        g0 = gstate(h)
        d0 = g0.get("s.dwell")
        seen, want, d = [], [], d0
        for _ in range(4):
            d = dwell_next(d)
            want.append(d)
            bench.tap(r, "F5")
            seen.append(gwait(h, lambda g, d=d: g.get("s.dwell") == d, 5).get("s.dwell"))
        g1 = gstate(h)
        if g1.get("s.dwell") != d0:             # round again to where it was
            for _ in range(4):
                bench.tap(r, "F5")
                if gwait(h, lambda g: g.get("s.dwell") is not None, 3).get("s.dwell") == d0:
                    break
        ok = seen == want and seen[-1] == d0 and g1.get("s.stay") == g0.get("s.stay") and \
            g1.get("s.quiet") == g0.get("s.quiet")
        return ok, (f"F5 DWELL x4: the scans' step time {d0} -> {' -> '.join(str(x) for x in seen)} ms "
                    f"(SCAN CFG's STEP TIME is the same setting, s.dwell); stay and go-on-after kept "
                    f"{g1.get('s.stay') == g0.get('s.stay') and g1.get('s.quiet') == g0.get('s.quiet')}")
    per_radio(run, "cover_b", "c-dwell", fn, ["c-log-f5-dwell"])


# ======== D BANDS, E DECODE ========

def t_d_scan_listen(run):
    @guarded
    def fn(r):
        h = r.host
        why = a_idle(r, h)
        if why:
            return None, why
        bsel0, p0 = gstate(h).get("s.bsel"), hear_a(h)
        res, said = {}, []
        try:
            if not hear_to(r, h, 3):            # A's LISTEN OFF first
                raise bench.Refused(f"{r.name}: A's LISTEN not OFF ({hear_a(h)})")
            bench.open_page(r, "D")
            time.sleep(0.6)
            pick(r, h, "s.bsel", B["40 M"], BANDS_N)
            bench.tap(r, "F2")
            res["F2 on 40 M: A"] = wait_state(r, lambda s: s.get("pg") == "A", 5)
            res["7.150 LSB"] = wait_state(r, lambda s: int(s.get("freq", 0)) == 7150000 and int(s.get("mode", -1)) == 0, 3)
            hv = int(pwait(h, "sentry_hear_a", lambda v: v == "0", 3.0, "3") or 3)
            res["A's LISTEN OFF -> X6200"] = hv == 0 and S(r, "heard") == 0
            said.append(f"40 M: {S(r, 'freq') / 1e6:.3f} mode {S(r, 'mode')}, A's LISTEN {hv}, heard {S(r, 'heard')}")
            bench.open_page(r, "D")
            time.sleep(0.6)
            pick(r, h, "s.bsel", B["17 M"], BANDS_N)
            bench.tap(r, "F1")
            res["F1 on 17 M: A"] = wait_state(r, lambda s: s.get("pg") == "A", 5)
            time.sleep(1.0)
            fA = S(r, "freq")
            res["the dial in 17 M"] = 18068000 <= fA <= 18168000
            g = gwait(h, lambda g: g.get("s.rout") == 2 and g.get("s.aband") == B["17 M"], 6)
            res["A scanning 17 M"] = g.get("s.rout") == 2 and g.get("s.aband") == B["17 M"]
            said.append(f"17 M: {fA / 1e6:.4f}, A's routine {g.get('s.rout')} band {g.get('s.aband')}")
            bench.tap(r, "F1")                  # A's F1: the scan off again
            res["A's F1: off"] = gwait(h, lambda g: g.get("s.rout") == 0, 6).get("s.rout") == 0
        finally:
            back = hear_back(r, h, p0)
            d_back(r, h, bsel0)
        return all(res.values()) and back, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (" + "; ".join(said) + f"); A's LISTEN as found {back}"
    per_radio(run, "cover_b", "d-scan-listen", fn, ["d-f1-scan", "d-f2-listen"])


def t_d_sweep_mark(run):
    @guarded
    def fn(r):
        h = r.host
        bsel0 = gstate(h).get("s.bsel")
        i = B["20 M"]
        res = {}
        try:
            bench.open_page(r, "D")
            time.sleep(0.6)
            pick(r, h, "s.bsel", i, BANDS_N)
            g0 = gstate(h)
            m0 = (g0.get("s.sweep0", 0) >> i) & 1
            bench.tap(r, "F3")
            g1 = gwait(h, lambda g: (g.get("s.sweep0", 0) >> i) & 1 != m0, 5)
            res["F3: 20 M's sweep mark switched"] = (g1.get("s.sweep0", 0) >> i) & 1 != m0
            bench.tap(r, "F3")
            g2 = gwait(h, lambda g: (g.get("s.sweep0", 0) >> i) & 1 == m0, 5)
            res["F3 again: as it was"] = g2.get("s.sweep0") == g0.get("s.sweep0")
            f0 = S(r, "freq")
            bench.tap(r, "F4")
            time.sleep(0.5)
            bench.tap(r, "F5")
            time.sleep(2.6)
            g3 = gstate(h)
            res["F4, F5: nothing"] = all(g3.get(k) == g0.get(k) for k in ("s.sweep0", "s.sweep1", "s.bsel", "s.rout")) \
                and S(r, "freq") == f0 and r.state.get("pg") == "D"
        finally:
            d_back(r, h, bsel0)
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            f" (marks {g0.get('s.sweep0')} -> {g1.get('s.sweep0')} -> {g2.get('s.sweep0')})"
    per_radio(run, "cover_b", "d-sweep-mark", fn, ["d-f3-sweep-mark"])


def rx_set(r):
    return {k: S(r, k) for k in ("pre", "att", "agc", "rfg")}


def t_d_e_rule(run):
    @guarded
    def fn(r):
        h = r.host
        why = a_idle(r, h)
        if why:
            return None, why
        g0 = gstate(h)
        bsel0, dsel0, drow0 = g0.get("s.bsel"), g0.get("s.dsel"), g0.get("s.drow")
        x61 = r.name == "x6100"
        got, bad = [], []
        try:
            for band, pre in (("80 M", 0), ("20 M", 1)):
                bench.open_page(r, "D")
                if x61:
                    bench.ensure(r, {"rfg": 80})    # the rule takes its RF gain to 100
                time.sleep(0.6)
                pick(r, h, "s.bsel", B[band], BANDS_N)
                bench.tap(r, "F2")
                hz = BANDS[B[band]][3]
                wait_state(r, lambda s, hz=hz: s.get("pg") == "A" and int(s.get("freq", 0)) == hz, 5)
                time.sleep(1.5)
                have, want = rx_set(r), {"pre": pre, "att": 0, "agc": 3}
                if x61:
                    want["rfg"] = 100
                got.append(f"D's LISTEN {band}: {have}")
                if any(have[k] != v for k, v in want.items()) or S(r, "freq") != hz:
                    bad.append(f"{band} {have} (rule {want})")
            bench.open_page(r, "E")
            time.sleep(0.6)
            etype(r, h, DEC_CW)
            bench.tap(r, "F2")                      # CW: the main screen in CW
            opened = wait_state(r, lambda s: s.get("pg") == "" and int(s.get("mode", -1)) in (4, 5), 6)
            time.sleep(1.5)
            have = rx_set(r)
            got.append(f"E's OPEN CW: mode {S(r, 'mode')} {have}")
            if not opened or have["agc"] != 2 or have["att"] != 0:
                bad.append(f"CW {have} (AGC FAST, ATT off)")
            bench.open_page(r, "E")
        finally:
            e_back(r, h, dsel0, drow0)
            d_back(r, h, bsel0)
        return not bad, "; ".join(got) + ("" if not bad else "; NOT: " + "; ".join(bad))
    per_radio(run, "cover_b", "d-e-rule", fn, ["d-e-listen-sets-x6200"])


def t_e_emergency(run):
    @guarded
    def fn(r):
        h = r.host
        g0 = gstate(h)
        dsel0, drow0, p0 = g0.get("s.dsel"), g0.get("s.drow"), hear_a(h)
        res, said = {}, []
        try:
            bench.open_page(r, "E")
            time.sleep(0.6)
            etype(r, h, DEC_EMERGENCY)
            c = gwait(h, lambda g: g.get("s.drow") is not None, 3).get("s.drow", 0)
            clicks(r, "mfk", EMER_N - 1, 0.1)       # 64 clicks: one short of round, then the 65th: where it began
            last = gwait(h, lambda g: g.get("s.drow") == (c - 1) % EMER_N, 6).get("s.drow")
            clicks(r, "mfk", 1)
            first = gwait(h, lambda g: g.get("s.drow") == c, 6).get("s.drow")
            res["65 rows (the MFK round)"] = last == (c - 1) % EMER_N and first == c
            pick(r, h, "s.drow", EMER_CB9, EMER_N)
            bench.tap(r, "MFK")                     # OPEN
            res["CB 9: A on 27.065 AM"] = wait_state(
                r, lambda s: s.get("pg") == "A" and int(s.get("freq", 0)) == 27065000 and int(s.get("mode", -1)) == 6, 6)
            time.sleep(1.5)
            have = rx_set(r)
            want = {"pre": 1, "att": 0, "agc": 1}
            if r.name == "x6100":
                want["rfg"] = 100
            res["PRE on, ATT off, AGC SLOW"] = all(have[k] == v for k, v in want.items())
            res["heard on the X6200"] = S(r, "heard") == 0
            said.append(f"opened: {S(r, 'freq') / 1e6:.3f} mode {S(r, 'mode')} {have}, heard {S(r, 'heard')}, "
                        f"rxo {S(r, 'rxo')} (receive only)")
        finally:
            e_back(r, h, dsel0, drow0)
            back = hear_back(r, h, p0)
        return all(res.values()) and back, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (" + "; ".join(said) + f"; E and A's LISTEN as found {back})"
    per_radio(run, "cover_b", "e-emergency", fn, ["e-emergency-list"])


def t_e_scan_sel(run):
    @guarded
    def fn(r):
        h = r.host
        g0 = gstate(h)
        dsel0, drow0, p0 = g0.get("s.dsel"), g0.get("s.drow"), hear_a(h)
        if g0.get("s.rout", 0) != 0:
            return None, f"A's routine {g0.get('s.rout')} as found: left as it was"
        dwell = g0.get("s.dwell", 2000) / 1000
        lb = log_text(h)
        n0 = len(log_rows(h))
        seen, still, modes = [], False, None
        try:
            bench.open_page(r, "E")
            time.sleep(0.6)
            etype(r, h, DEC_EMERGENCY)
            pick(r, h, "s.drow", EMER_LOW_BAND, EMER_N)
            f0 = S(r, "freq")
            bench.tap(r, "F2")                      # SCAN SEL: LOW BAND
            seen = changes(r, "freq", 4 * dwell + 2.0, start=f0)
            modes = S(r, "mode")
            bench.tap(r, "F2")                      # off
            time.sleep(0.8)
            fs = S(r, "freq")
            time.sleep(dwell + 1.2)
            still = S(r, "freq") == fs
        finally:
            n1 = len(log_rows(h))
            logged = log_put_back(r, h, lb)         # the scan stops on one that talks, and logs it
            e_back(r, h, dsel0, drow0)
            back = hear_back(r, h, p0)
        fq = [v for _, v in seen]
        ok = len(set(fq)) >= 2 and set(fq) <= set(LOW_BAND) and modes == 7 and still and logged and back
        return ok, (f"SCAN SEL on LLAW1 (LOW BAND): A went {' '.join(f'{v / 1e6:.3f}' for v in fq[:6])} (the group: "
                    f"{' '.join(f'{v / 1e6:.3f}' for v in LOW_BAND)}), mode {modes} (NFM 7); F2 again: the dial still "
                    f"{still}; C LOG {n0} -> {n1} entries, as found {logged}; A's LISTEN as found {back}")
    per_radio(run, "cover_b", "e-scan-sel", fn, ["e-emergency-scan-sel"])


# ======== G WALKIE ========

def gui_version(r):
    """The radio's GUI version from the remote's greeting ('hello GoldCorpHF - v0.3.12.47 ...'), or None"""
    with r.lock:
        hello = next((x for _, x in list(r.lines) if x.startswith("hello ")), "")
    m = re.search(r"v(\d+(?:\.\d+)+)", hello)
    return tuple(int(x) for x in m.group(1).split(".")) if m else None


def t_g_3d(run):
    @guarded
    def fn(r):
        h = r.host
        v = gui_version(r)
        if (v and v > (0, 3, 12, 47)) or "wch" in r.state:     # a test build of .48's G says .47: G's channel tells
            return None, ("G's F4 is its channel list from v0.3.12.48 (the manual, the owner's word of 2026-11-23: 3D "
                          "only); the 3D/2D toggle this feature names is gone (g-spectrum-f4 is 3D only now)")
        bench.open_page(r, "G")
        time.sleep(0.6)
        v0 = gstate(h).get("s.3d")
        bench.tap(r, "F4")
        v1 = gwait(h, lambda g: g.get("s.3d") != v0, 5).get("s.3d")
        bench.tap(r, "F4")
        v2 = gwait(h, lambda g: g.get("s.3d") == v0, 5).get("s.3d")
        ok = v0 is not None and v1 != v0 and v2 == v0
        return ok, (f"G's F4: its spectrum 3D {v0} -> {v1} -> {v2} (1 3D, 0 the flat waterfall); the spans "
                    "(X6200 100 kHz, X6100 50 kHz) are on screen only, not read")
    per_radio(run, "cover_b", "g-3d", fn, ["g-spectrum-f4"])


# ======== SCAN CFG ========

class Unsure(Exception):
    """SCAN CFG as found keeps a row from being found and put back (CUSTOM, a priority set, a scan running)"""


def style_of(dw, stay, quiet):
    for k, (d, s, q) in enumerate(STYLES):
        if d == dw and s == stay and (stay in (0, 5) or q == quiet):
            return k
    return 5


def cfg_look(h):
    g = gstate(h)
    p = params(h, ["rtl_sql", "rtl_gain"])
    return {"dwell": g.get("s.dwell"), "stay": g.get("s.stay"), "quiet": g.get("s.quiet"), "chans": g.get("s.chans"),
            "prio": g.get("s.prio"), "rout": g.get("s.rout"), "astep": g.get("s.astep"), "bsql": p.get("rtl_sql"),
            "bgain": p.get("rtl_gain")}


def cfg_wait(h, cond, secs=6.0):
    end = time.time() + secs
    while True:
        a = cfg_look(h)
        if cond(a) or time.time() > end:
            return a
        time.sleep(0.4)


def cfg_push(r, n=1):
    for _ in range(n):
        bench.tap(r, "MFK")
        time.sleep(0.35)


def cfg_row_of(b, a):
    """The row a push changed, from the settings before (b) and after (a): None for none seen, -1 unexplained"""
    ch = {k for k in a if a[k] != b[k]}
    if not ch:
        return None
    single = {"chans": "WHAT", "prio": "PRIO", "rout": "SWEEP", "astep": "ASTEP", "bsql": "BSQL", "bgain": "BGAIN",
              "quiet": "QUIET", "dwell": "STEP"}
    if len(ch) == 1 and next(iter(ch)) in single and not (ch == {"dwell"} and a["dwell"] != dwell_next(b["dwell"])):
        return CR[single[next(iter(ch))]]
    if ch <= {"dwell", "stay", "quiet"}:
        k = style_of(b["dwell"], b["stay"], b["quiet"])
        nk = 0 if k >= 4 else k + 1
        if style_of(a["dwell"], a["stay"], a["quiet"]) == nk:
            return CR["STYLE"]
        if ch == {"stay"} and a["stay"] == (b["stay"] + 1) % 6:
            return CR["STAY"]
    return -1


def cfg_undo(r, h, row, b):
    """More pushes on the row a push changed, till its setting is as before (b); True when all read back so"""
    if row == CR["BGAIN"]:
        for _ in range(40):
            cfg_push(r)
            if params(h, ["rtl_gain"]).get("rtl_gain") == b["bgain"]:
                break
    else:
        cfg_push(r, CR_MORE[row])
    a = cfg_wait(h, lambda a: a == b, 6.0)
    return a == b


def cfg_find(r, h):
    """SCAN CFG's chosen row (the GUI keeps it, no file says it): a push, the setting it changed found and put back
    with more pushes on that row. A push that shows nothing (GO ON AFTER with a stay that does not use it, or B'S STEP,
    kept nowhere) is told apart by the row before it (STAY or A'S STEP). A push on STYLE or STAY switches on a style
    that stops on signals for a moment: A's dial is put where little is heard first (QUIET_HZ, the attenuator in),
    and the test puts C LOG back after (log_put_back). (the row chosen now, the row it was on as found); every
    setting as before"""
    b = cfg_look(h)
    if style_of(b["dwell"], b["stay"], b["quiet"]) == 5 or b["prio"] or b["rout"]:
        raise Unsure(f"SCAN CFG as found (style {STYLE_NAMES[style_of(b['dwell'], b['stay'], b['quiet'])]}, priority "
                     f"{b['prio']}, A's routine {b['rout']}): a push there could not be put back")
    bench.ensure(r, {"freq": QUIET_HZ, "att": 1})
    cfg_push(r)
    time.sleep(2.4)
    a = cfg_wait(h, lambda a: a != b, 2.5)
    row = cfg_row_of(b, a)
    if row is None:
        clicks(r, "mfk", -1)
        cfg_push(r)
        time.sleep(2.4)
        a = cfg_wait(h, lambda a: a != b, 2.5)
        prev = cfg_row_of(b, a)
        if prev == CR["STAY"]:
            if not cfg_undo(r, h, prev, b):
                raise bench.Refused(f"{r.name}: SCAN CFG's STAY not put back ({cfg_look(h)})")
            return CR["STAY"], CR["QUIET"]
        if prev == CR["ASTEP"]:
            if not cfg_undo(r, h, prev, b):
                raise bench.Refused(f"{r.name}: SCAN CFG's A'S STEP not put back ({cfg_look(h)})")
            clicks(r, "mfk", 1)
            cfg_push(r, CR_MORE[CR["BSTEP"]])     # B'S STEP's five steps round to where it was
            return CR["BSTEP"], CR["BSTEP"]
        raise bench.Refused(f"{r.name}: SCAN CFG's row not found (a push showed nothing, the row before it {prev})")
    if row < 0:
        raise bench.Refused(f"{r.name}: a push in SCAN CFG changed {b} -> {a}: no row explains it")
    if not cfg_undo(r, h, row, b):
        raise bench.Refused(f"{r.name}: SCAN CFG's row {row} not put back ({cfg_look(h)}, was {b})")
    return row, row


def cfg_goto(r, cur, want):
    d = (want - cur) % CR_N
    if d > CR_N // 2:
        d -= CR_N
    clicks(r, "mfk", d, 0.2)
    time.sleep(0.4)
    return want


def cfg_open(r, letter="A"):
    bench.open_page(r, letter)
    time.sleep(0.5)
    bench.tap(r, "F5")
    r.wait("SCAN CFG", lambda: r.state.get("pg") == "S", 5)
    time.sleep(0.5)


def cfg_close(r):
    """SCAN CFG's F5 (BACK) to the screen it came from"""
    if r.state.get("pg") == "S":
        bench.tap(r, "F5")
        wait_state(r, lambda s: s.get("pg") in ("A", "B"), 4)


def says(res):
    return "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items())


def t_scancfg_open(run):
    @guarded
    def fn(r):
        h = r.host
        why = a_idle(r, h)
        if why:
            return None, why
        lb = log_text(h)
        res = {}
        try:
            cfg_open(r, "A")
            res["F5 on A: SCAN CFG"] = r.state.get("pg") == "S"
            res["its radio A's"] = gwait(h, lambda g: g.get("s.cfgfrom") == 0, 4).get("s.cfgfrom") == 0
            try:
                row, row0 = cfg_find(r, h)
            except Unsure as e:
                return None, str(e)
            res[f"a push changed its chosen row ({row0}), put back"] = True
            cfg_goto(r, row, row0)
            cfg_close(r)
            res["F5 (BACK): A"] = r.state.get("pg") == "A"
            cfg_open(r, "B")
            res["F5 on B: SCAN CFG"] = r.state.get("pg") == "S"
            res["its radio B's"] = gwait(h, lambda g: g.get("s.cfgfrom") == 5, 4).get("s.cfgfrom") == 5
            cfg_close(r)
            res["F5 (BACK): B"] = r.state.get("pg") == "B"
        finally:
            cfg_close(r)
            res["C LOG as found"] = log_put_back(r, h, lb)
        return all(res.values()), says(res) + " (the badge and the box's words are on screen only)"
    per_radio(run, "cover_b", "scancfg-open", fn, ["scancfg-open"])


def t_scancfg_style(run):
    @guarded
    def fn(r):
        h = r.host
        why = a_idle(r, h)
        if why:
            return None, why
        lb = log_text(h)
        seen, ok, custom, browse, back, logged = [], True, False, False, False, False
        a = {}
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": QUIET_HZ, "att": 1})   # the styles that stop on signals: where little is heard
        try:
            cfg_open(r, "A")
            b = cfg_look(h)
            k0 = style_of(b["dwell"], b["stay"], b["quiet"])
            if k0 == 5:
                return None, "SCAN CFG's style is CUSTOM as found: F1 would lose it"
            for i in range(5):
                want = STYLES[(k0 + i + 1) % 5]
                bench.tap(r, "F1")
                x = cfg_wait(h, lambda a, w=want: (a["dwell"], a["stay"], a["quiet"]) == w
                             or (w[1] in (0, 5) and (a["dwell"], a["stay"]) == w[:2]), 5)
                k = style_of(x["dwell"], x["stay"], x["quiet"])
                seen.append(f"{STYLE_NAMES[k]} {x['dwell'] / 1000:g} s/{x['stay']}/{x['quiet']}")
                ok &= k == (k0 + i + 1) % 5
            try:
                row, row0 = cfg_find(r, h)
            except Unsure as e:
                return None, f"styles {'; '.join(seen)}; then {e}"
            cfg_goto(r, row, CR["STEP"])
            cfg_push(r)                             # STEP TIME by hand
            a = cfg_wait(h, lambda a: a["dwell"] == dwell_next(b["dwell"]), 5)
            custom = style_of(a["dwell"], a["stay"], a["quiet"]) == 5
            bench.tap(r, "F1")                      # F1 from CUSTOM: BROWSE
            a2 = cfg_wait(h, lambda a: style_of(a["dwell"], a["stay"], a["quiet"]) == 0, 5)
            browse = style_of(a2["dwell"], a2["stay"], a2["quiet"]) == 0
            for _ in range(k0):
                bench.tap(r, "F1")
                time.sleep(0.3)
            end = cfg_wait(h, lambda a: (a["dwell"], a["stay"], a["quiet"]) == (b["dwell"], b["stay"], b["quiet"]), 6)
            back = (end["dwell"], end["stay"], end["quiet"]) == (b["dwell"], b["stay"], b["quiet"])
            cfg_goto(r, CR["STEP"], row0)           # the row chosen as found
        finally:
            cfg_close(r)
            logged = log_put_back(r, h, lb)
        ok = ok and custom and browse and back and logged
        return ok, (f"from {STYLE_NAMES[k0]}: F1 x5: " + "; ".join(seen) + f"; STEP TIME by hand ({a.get('dwell')} ms): "
                    f"CUSTOM {custom}; F1: BROWSE {browse}; as found {back} (kept in /mnt/gui_state.conf, what a "
                    f"restart reads); C LOG as found {logged}")
    per_radio(run, "cover_b", "scancfg-style", fn, ["scancfg-style-select"])


def t_scancfg_step(run):
    @guarded
    def fn(r):
        h = r.host
        name = SCAN_ON[r.name]
        _, lo, hi, listen, mode = BANDS[B[name]]
        why = a_idle(r, h)
        if why:
            return None, why
        lb = log_text(h)
        gaps, d1, back, logged = [], None, False, False
        b = {}
        try:
            cfg_open(r, "A")
            b = cfg_look(h)
            try:
                row, row0 = cfg_find(r, h)
            except Unsure as e:
                return None, str(e)
            cfg_goto(r, row, CR["STEP"])
            cfg_push(r)
            d1 = cfg_wait(h, lambda a: a["dwell"] == dwell_next(b["dwell"]), 5)["dwell"]
            cfg_close(r)
            bench.ensure(r, {"freq": listen, "mode": mode})
            f0 = S(r, "freq")
            bench.tap(r, "F1")
            seen = changes(r, "freq", 3 * d1 / 1000 + 1.5, start=f0)
            bench.tap(r, "F1")
            gwait(h, lambda g: g.get("s.rout") == 0, 6)
            gaps = [round(y[0] - x[0], 1) for x, y in zip(seen, seen[1:])]
            cfg_open(r, "A")                    # the row chosen is still STEP TIME
            cfg_push(r, 3)
            back = cfg_wait(h, lambda a: a["dwell"] == b["dwell"], 6)["dwell"] == b["dwell"]
            cfg_goto(r, CR["STEP"], row0)
        finally:
            cfg_close(r)
            logged = log_put_back(r, h, lb)
        ok = d1 is not None and len(gaps) >= 2 and all(abs(g - d1 / 1000) <= 0.9 for g in gaps) and back and logged
        return ok, (f"STEP TIME {b.get('dwell', 0) / 1000:g} -> {(d1 or 0) / 1000:g} s: A's scan on {name} hopped "
                    f"{gaps} s apart; STEP TIME as found {back}; C LOG as found {logged}")
    per_radio(run, "cover_b", "scancfg-step", fn, ["scancfg-step-time"])


def channels_of(ip):
    st, body = http(ip, "/api/channels")
    if st != 200:
        raise bench.Refused(f"/api/channels {st}")
    return json.loads(body).get("channels", [])


def t_scancfg_chans(run):
    @guarded
    def fn(r):
        h = r.host
        chans = [c for c in channels_of(ip_of(r)) if tunes(r.name, int(c.get("frequency", 0)))]
        if not chans:
            return None, "the channel list has none this radio's own receiver takes"
        by_hz = {int(c["frequency"]): c for c in chans}
        why = a_idle(r, h)
        if why:
            return None, why
        lb = log_text(h)
        visits, bad, chans_on, back, logged = [], [], None, False, False
        b = {"dwell": 2000}
        try:
            cfg_open(r, "A")
            b = cfg_look(h)
            try:
                row, row0 = cfg_find(r, h)
            except Unsure as e:
                return None, str(e)
            cfg_goto(r, row, CR["WHAT"])
            cfg_push(r)
            chans_on = cfg_wait(h, lambda a: a["chans"] == (0 if b["chans"] else 1), 5)["chans"]
            dwell = b["dwell"] / 1000
            cfg_close(r)
            bench.tap(r, "F1")
            end, last = time.time() + 3 * dwell + 1.5, None
            while time.time() < end:
                f = S(r, "freq")
                if f != last:
                    last = f
                    time.sleep(0.5)
                    visits.append({k: S(r, k) for k in ("freq", "mode", "agc", "pre", "att")})
                time.sleep(0.05)
            bench.tap(r, "F1")
            gwait(h, lambda g: g.get("s.rout") == 0, 6)
            cfg_open(r, "A")
            cfg_push(r)
            back = cfg_wait(h, lambda a: a["chans"] == b["chans"], 5)["chans"] == b["chans"]
            cfg_goto(r, CR["WHAT"], row0)
        finally:
            cfg_close(r)
            logged = log_put_back(r, h, lb)
        visits = [v for v in visits if v["freq"] != QUIET_HZ]      # where cfg_find left the dial, before the first hop
        for v in visits:
            c = by_hz.get(v["freq"])
            if not c:
                bad.append(f"{v['freq']} not a channel")
                continue
            want = {"mode": MODE_N.get(str(c.get("mode")).upper()), "agc": AGC_N.get(str(c.get("agc")).upper()),
                    "pre": 1 if c.get("pre") else 0, "att": 1 if c.get("att") else 0}
            off = {k: (v[k], w) for k, w in want.items() if w is not None and v[k] != w}
            if off:
                bad.append(f"{v['freq'] / 1e6:.4f} {off}")
        distinct = len({v["freq"] for v in visits})
        ok = chans_on == 1 and distinct >= min(2, len(chans)) and not bad and back and logged
        return ok, (f"{len(chans)} channels this radio takes; F1 SCANS: THE CHANNELS {chans_on}; A's F1 visited "
                    f"{distinct} channels in {3 * b['dwell'] / 1000 + 1.5:g} s, each on its own frequency, mode, AGC, "
                    f"PRE and ATT" + ("" if not bad else f": NOT {bad}") + f"; THE BAND again {back}; C LOG as found "
                    f"{logged}")
    per_radio(run, "cover_b", "scancfg-chans", fn, ["scancfg-f1-scans-channels"])


def t_scancfg_sweep(run):
    @guarded
    def fn(r):
        h = r.host
        why = a_idle(r, h)
        if why:
            return None, why
        g = gstate(h)
        marks = [i for i in range(BANDS_N) if (g.get(f"s.sweep{i // 30}", 0) >> (i % 30)) & 1]
        hops = {BANDS[i][3]: BANDS[i][0] for i in marks if tunes(r.name, BANDS[i][3])}
        if len(hops) < 2:
            return None, f"fewer than two bands marked in D that this radio tunes ({[BANDS[i][0] for i in marks]})"
        lb = log_text(h)
        seen, on, back, logged = [], None, False, False
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": F_TEST, "mode": 2})    # in a band: A's F1 has one to scan
        try:
            f0 = S(r, "freq")                   # out of MANUAL first: F1 on and off (a hand on A's VFO stops a sweep)
            bench.tap(r, "F1")
            wait_state(r, lambda s: int(s.get("freq", 0)) != f0, 3)
            bench.tap(r, "F1")
            gwait(h, lambda g: g.get("s.rout") == 0, 5)
            cfg_open(r, "A")
            b = cfg_look(h)
            try:
                row, row0 = cfg_find(r, h)
            except Unsure as e:
                return None, str(e)
            cfg_goto(r, row, CR["SWEEP"])
            cfg_push(r)
            on = cfg_wait(h, lambda a: a["rout"] == 1, 5)["rout"]
            cfg_close(r)
            seen = changes(r, "freq", 3 * b["dwell"] / 1000 + 1.0, start=-1)
            cfg_open(r, "A")
            cfg_push(r)
            back = cfg_wait(h, lambda a: a["rout"] == 0, 5)["rout"] == 0
            cfg_goto(r, CR["SWEEP"], row0)
        finally:
            cfg_close(r)
            logged = log_put_back(r, h, lb)
        fq = [v for _, v in seen if v != QUIET_HZ]  # where cfg_find left the dial, if the sweep had not hopped yet
        ok = on == 1 and len(set(fq)) >= 2 and set(fq) <= set(hops) and back and logged
        return ok, (f"bands marked in D: {', '.join(hops.values())}; SWEEP ON (routine {on}): the X6200 went "
                    + " ".join(hops.get(v, f'{v / 1e6:.3f} (NOT A MARKED BAND)') for v in fq[:6])
                    + f"; SWEEP OFF {back}; C LOG as found {logged}")
    per_radio(run, "cover_b", "scancfg-sweep", fn, ["scancfg-sweep-d-bands"])


def t_scancfg_astep(run):
    @guarded
    def fn(r):
        h = r.host
        why = a_idle(r, h)
        if why:
            return None, why
        lb = log_text(h)
        said, ok, back, logged = [], True, False, False
        try:
            cfg_open(r, "A")
            b = cfg_look(h)
            try:
                row, row0 = cfg_find(r, h)
            except Unsure as e:
                return None, str(e)
            cfg_goto(r, row, CR["ASTEP"])
            bench.ensure(r, {"freq": 14203000, "mode": 2})
            for i in range(2):
                st = A_STEPS[(b["astep"] + i) % len(A_STEPS)]
                cfg_close(r)
                time.sleep(1.0)
                f0 = S(r, "freq")
                r.send("knob mfk 1")
                wait_state(r, lambda s, f0=f0: int(s.get("freq", 0)) != f0, 2.5)
                time.sleep(0.4)
                f1 = S(r, "freq")
                base = f0 // st * st
                right = f1 in (base + st, base - st)
                ok &= right
                said.append(f"{st / 1000:g} kHz: {f0 / 1e6:.4f} -> {f1 / 1e6:.4f} "
                            f"({'one step' if right else 'NOT one step'})")
                cfg_open(r, "A")
                if i == 0:
                    cfg_push(r)
                    a = cfg_wait(h, lambda a: a["astep"] == (b["astep"] + 1) % len(A_STEPS), 5)
                    ok &= a["astep"] == (b["astep"] + 1) % len(A_STEPS)
            cfg_push(r, CR_MORE[CR["ASTEP"]])
            back = cfg_wait(h, lambda a: a["astep"] == b["astep"], 5)["astep"] == b["astep"]
            cfg_goto(r, CR["ASTEP"], row0)
        finally:
            cfg_close(r)
            logged = log_put_back(r, h, lb)
        return ok and back and logged, ("A: KNOB STEP, one MFK click on A: " + "; ".join(said)
                                        + f"; as found {back}; C LOG as found {logged}")
    per_radio(run, "cover_b", "scancfg-astep", fn, ["scancfg-knob-steps"])


def t_scancfg_rec(run):
    @guarded
    def fn(r):
        h = r.host
        before = set(ssh(h, "ls /mnt/rec 2>/dev/null").split())
        new, sizes, grows = [], {}, {}
        try:
            cfg_open(r, "A")
            bench.tap(r, "F4")
            time.sleep(3.5)
            bench.tap(r, "F4")
            time.sleep(1.5)
            new = [f for f in ssh(h, "ls /mnt/rec 2>/dev/null").split() if f not in before]
            for f in new:
                sizes[f] = (size_time(h, f"/mnt/rec/{f}") or ["?"])[0]
            time.sleep(1.0)
            grows = {f: (size_time(h, f"/mnt/rec/{f}") or ["?"])[0] for f in new}
        finally:
            cfg_close(r)
            for f in new:                           # the bench's own recording, gone again
                if f.startswith("REC_") and "/" not in f:
                    ssh(h, f"rm -f /mnt/rec/{f}")
        left = [f for f in new if f in ssh(h, "ls /mnt/rec 2>/dev/null").split()]
        ok = len(new) == 1 and new[0].startswith("REC_") and sizes[new[0]].isdigit() and int(sizes[new[0]]) > 0 and \
            grows == sizes and not left
        return ok, (f"SCAN CFG from A, F4, 3.5 s, F4: {new} {sizes} bytes, stopped (no more written {grows == sizes}); "
                    f"removed again {not left}")
    per_radio(run, "cover_b", "scancfg-rec", fn, ["scancfg-f4-rec"])


# ======== I: the direction finder with its test source ========

DF_KEEP = ("fc", "off", "target", "bw", "band", "follow", "auto", "method", "kind", "size_mm", "rate", "gain", "sql",
           "mask", "nsrc")


def df_get(ip):
    """/api/df without its GPS part (never said: the owner's position)"""
    for _ in range(4):
        st, body = http(ip, "/api/df")
        if st == 200:
            d = json.loads(body)
            if d.get("loaded"):
                d.pop("gp", None)
                return d
        time.sleep(0.5)
    raise bench.Refused(f"/api/df: {st}")


def df_set(ip, key, val):
    st, body = http(ip, "/api/df/set", method="POST", body={"key": key, "val": val})
    if st != 200:
        raise bench.Refused(f"/api/df/set {key}={val}: {st} {body[:80]}")


def df_cfg(d):
    c = d.get("cfg", {})
    return {k: c.get(k) for k in DF_KEEP}, [t.get("hz") for t in c.get("tg", [])]


def df_open(r, h, ip):
    """Page I with the test source (/mnt/df_synth: x6x00-df -T, no sticks): (its state, as found)"""
    keep(h, "/mnt/df_synth", "save")
    ssh(h, "touch /mnt/df_synth")
    r.send("open sentry I")
    r.wait("page I", lambda: r.state.get("win") == "Direction finder", 8)
    end = time.time() + 12
    d = df_get(ip)
    while time.time() < end and not (d.get("connected") and "do" in d):
        time.sleep(0.8)
        d = df_get(ip)
    return d


def df_close(r, h):
    """The Sentry again (page I closed, its daemon told to end), /mnt/df_synth as found; x6x00-df gone"""
    try:
        bench.open_page(r, "A")
    finally:
        keep(h, "/mnt/df_synth", "back")
    # the GUI keeps x6x00-df WEB_IDLE_MS (20 s) after the last web request (app_df.c: the browser's DF page goes on
    # working without page I), and these checks read page I through the web: 26 s, no web request meanwhile (6 s was
    # too short: orch, 2026-11-24)
    end = time.time() + 26
    while time.time() < end:
        if not ssh(h, "pidof x6x00-df").strip():
            return True
        time.sleep(0.8)
    return False


def df_restore(ip, c0, t0):
    """Page I's settings as found (the target first: it sets FOLLOW and AUTO off), read back"""
    d = df_get(ip)
    c, t = df_cfg(d)
    if c["target"] != c0["target"] or c["fc"] != c0["fc"] or c["off"] != c0["off"]:
        df_set(ip, "target", c0["target"])
    for k in ("follow", "auto", "method"):
        if df_cfg(df_get(ip))[0][k] != c0[k]:
            df_set(ip, k, int(c0[k]))
    for i, hz in enumerate(t0):
        if df_cfg(df_get(ip))[1][i] != hz:
            df_set(ip, f"tg{i + 2}", hz or 0)
    c, t = df_cfg(df_get(ip))
    return c == c0 and t == t0, c


def t_df_source(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        c0, t0 = df_cfg(df_get(ip))
        brg, states, fits = [], set(), []
        d = {}
        try:
            d = df_open(r, h, ip)
            pid = ssh(h, "pidof x6x00-df").strip()
            st = d.get("st", {})
            for _ in range(10):
                x = df_get(ip)
                states.add(x.get("st", {}).get("state"))
                if x.get("do", {}).get("bearing") is not None:
                    brg.append(x["do"]["bearing"])
                    fits.append(x["do"].get("fit"))
                time.sleep(1.0)
            arr = d.get("arr", {})
            methods = d.get("lists", {}).get("methods", [])
        finally:
            gone = df_close(r, h)
        back, c1 = df_restore(ip, c0, t0)
        spread = (max(brg) - min(brg)) if brg else 0
        ok = bool(pid) and d.get("present") == 1 and d.get("connected") == 1 and st.get("synth") == 1 and \
            len(set(brg)) >= 3 and spread >= 8 and states & {"TRACKING", "HOLD", "LISTENING", "CALIBRATING", "SLIP"} \
            and bool(arr.get("layouts")) and gone and back
        return ok, (f"/mnt/df_synth, page I: x6x00-df {pid or 'NOT running'}, present {d.get('present')}, connected "
                    f"{d.get('connected')}, test source {st.get('synth')}; over 10 s bearings "
                    f"{brg[:10]} (spread {spread:.0f} deg), fit {fits[:5]} %, states {sorted(states, key=str)}, "
                    f"method {methods[c0['method']] if methods and c0['method'] is not None else '?'}, the array "
                    f"calculator's layouts {len(arr.get('layouts', []))}; closed: x6x00-df gone {gone}; page I's "
                    f"settings as found {back}")
    per_radio(run, "cover_b", "df-source", fn, ["df-test-source", "df-display"])


def t_df_keys(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        c0, t0 = df_cfg(df_get(ip))
        res, said = {}, []
        try:
            d = df_open(r, h, ip)
            if not d.get("connected"):
                raise bench.Refused(f"{r.name}: page I's daemon not connected")
            n = len(d.get("lists", {}).get("methods", [])) or 6
            m0, seq0 = d["cfg"]["method"], d.get("do", {}).get("seq", 0)
            bench.tap(r, "F3")
            end, x = time.time() + 9, {}
            while time.time() < end:                # the test source sends 6 s on, 3 s off: up to 9 s for estimates
                x = df_get(ip)
                if x["cfg"]["method"] != m0 and x.get("do", {}).get("seq", 0) > seq0 + 1:
                    break
                time.sleep(0.5)
            res["F3: the next method"] = x["cfg"]["method"] == (m0 + 1) % n
            est = x.get("do", {})
            res["a bearing with it"] = est.get("seq", 0) > seq0 and est.get("bearing") is not None
            m1 = x["cfg"]["method"]
            said.append(f"method {m0} -> {m1} ({x.get('lists', {}).get('methods', ['?'] * n)[m1]}), bearing "
                        f"{est.get('bearing')}")
            df_set(ip, "method", m0)
            tg = df_get(ip)["cfg"]["target"]
            bench.tap(r, "GEN")                     # the help: the VFO does nothing under it
            time.sleep(0.8)
            r.send("knob main 3")
            time.sleep(1.5)
            res["GEN's help holds the VFO"] = df_get(ip)["cfg"]["target"] == tg
            bench.tap(r, "GEN")
            time.sleep(0.8)
            r.send("knob main 3")
            end = time.time() + 3
            while time.time() < end and df_get(ip)["cfg"]["target"] == tg:
                time.sleep(0.3)
            t1 = df_get(ip)["cfg"]["target"]
            res["the VFO: the target 1 kHz a click"] = t1 - tg == 3000
            r.send("knob main -3")
            time.sleep(1.0)
            said.append(f"target {tg} -> {t1} -> {df_get(ip)['cfg']['target']}")
        finally:
            gone = df_close(r, h)
        back, c1 = df_restore(ip, c0, t0)
        return all(res.values()) and gone and back, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (" + "; ".join(said) + f"); closed {gone}; page I's settings as found {back}"
    per_radio(run, "cover_b", "df-keys", fn, ["df-f3-method", "df-keys"])


def t_df_tune(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        c0, t0 = df_cfg(df_get(ip))
        fx = 125000000 if r.name == "x6200" else 28400000     # inside the Kraken's 24-1766 MHz, on each radio's dial
        res, said = {}, []
        try:
            d = df_open(r, h, ip)
            if not d.get("connected"):
                raise bench.Refused(f"{r.name}: page I's daemon not connected")
            t_before = df_get(ip)["cfg"]["target"]
            df_set(ip, "follow", 1)                 # FOLLOW A
            r.send(f"set freq {fx}")
            res["the dial set"] = wait_state(r, lambda s: int(s.get("freq", 0)) == fx, 3.0)
            end = time.time() + 6
            while time.time() < end and df_get(ip)["cfg"]["target"] != fx:
                time.sleep(0.4)
            x = df_get(ip)["cfg"]
            # the target follows A's dial when the antenna set takes it (the Kraken's own 24-1766 MHz), and waits
            # outside (its whips' 100-1000 MHz: the X6100's dial, 54 MHz at most, never in it; app_df.c follow_tick
            # says "OUTSIDE ...: WAITING"): either is right, the web does not say which antenna is set (2026-11-24)
            took = x["target"] == fx and x["follow"] == 1
            waits = x["target"] == t_before and x["follow"] == 1
            res["FOLLOW A: the target is A's dial, or waits outside the antenna's range"] = took or waits
            said.append(f"target {x['target']} follow {x['follow']} ({'followed' if took else 'waiting' if waits else '?'})")
            base = fx if took else t_before
            r.send("knob main 1")
            end = time.time() + 3
            while time.time() < end and df_get(ip)["cfg"]["follow"] != 0:
                time.sleep(0.3)
            x = df_get(ip)["cfg"]
            res["the VFO: FOLLOW OFF"] = x["follow"] == 0 and x["target"] == base + 1000
            a0 = x["auto"]
            hold(r, "F1")
            time.sleep(0.6)
            a1 = df_get(ip)["cfg"]["auto"]
            hold(r, "F1")
            time.sleep(0.6)
            a2 = df_get(ip)["cfg"]["auto"]
            res["F1 held: AUTO, and back"] = a1 != a0 and a2 == a0
            said.append(f"AUTO {a0} -> {a1} -> {a2}")
        finally:
            gone = df_close(r, h)
        back, c1 = df_restore(ip, c0, t0)
        return all(res.values()) and gone and back, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (" + "; ".join(said) + f"); closed {gone}; page I's settings as found {back} (the F1 panel's rows on " \
            "screen not read)"
    per_radio(run, "cover_b", "df-tune", fn, ["df-f1-tune"])


def t_df_targets(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        c0, t0 = df_cfg(df_get(ip))
        res, said = {}, []
        try:
            d = df_open(r, h, ip)
            if not d.get("connected"):
                raise bench.Refused(f"{r.name}: page I's daemon not connected")
            tg = d["cfg"]["target"]
            df_set(ip, "tg2", tg)                   # TARGET 2 where the main one is, then the main one moved
            r.send("knob main 5")
            end, t2 = time.time() + 8, {}
            while time.time() < end:
                t2 = df_get(ip)["cfg"]["tg"][0]
                if t2.get("in") and t2.get("bearing") is not None:
                    break
                time.sleep(0.6)
            res["TARGET 2 in the passband, its own bearing"] = t2.get("hz") == tg and bool(t2.get("in")) and \
                t2.get("bearing") is not None
            said.append(f"TARGET 2 {t2.get('hz')}: in {t2.get('in')}, bearing {t2.get('bearing')}, fit {t2.get('fit')}")
            df_set(ip, "tg2", tg + 1500000)         # past the passband
            time.sleep(1.5)
            t2 = df_get(ip)["cfg"]["tg"][0]
            res["1.5 MHz off: out of the passband"] = t2.get("hz") == tg + 1500000 and not t2.get("in")
            said.append(f"TARGET 2 {t2.get('hz')}: in {t2.get('in')}")
            r.send("knob main -5")
            time.sleep(1.0)
            df_set(ip, "tg2", t0[0] or 0)
        finally:
            gone = df_close(r, h)
        back, c1 = df_restore(ip, c0, t0)
        return all(res.values()) and gone and back, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            " (" + "; ".join(said) + f"); closed {gone}; page I's settings as found {back}"
    per_radio(run, "cover_b", "df-targets", fn, ["df-targets-2-4"])


# ======== The receiver core: the main screen's keys, buttons and knobs ========

def t_fst(run):
    @guarded
    def fn(r):
        ip = ip_of(r)
        own = top_of(keys_get(ip), "fst")["hold"] == -1
        bench.open_page(r, "A")                 # FST in the Sentry is the main screen's (its front key passes it on)
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        s0 = S(r, "step")
        seq, before, after = [s0], s0, s0
        try:
            for _ in range(len(STEPS)):             # once round the eight (fewer when Settings narrows them)
                prev = S(r, "step")
                bench.tap(r, "FST")
                wait_state(r, lambda s, p=prev: int(s.get("step", p)) != p, 2.0)
                seq.append(S(r, "step"))
            before = S(r, "step")
            if own:
                hold(r, "FST")
                wait_state(r, lambda s: int(s.get("step", before)) != before, 2.0)
            after = S(r, "step")
        finally:
            back = set_step(r, s0)
        vals = seq[1:]
        ups = list(zip(seq, seq[1:]))
        wraps = [i for i, (a, b) in enumerate(ups) if b < a]
        up_ok = all(v in STEPS for v in vals) and bool(wraps) and \
            all(a == max(vals) and b == min(vals) for i, (a, b) in enumerate(ups) if i in wraps and i) and \
            all(STEPS.index(b) == STEPS.index(a) + 1 for i, (a, b) in enumerate(ups) if i not in wraps and a in STEPS)
        down_ok = after in STEPS and before in STEPS and \
            (STEPS.index(after) == STEPS.index(before) - 1 or (before == min(vals) and after > before))
        ok = (up_ok and down_ok and back) if own else (None if up_ok and back else False)
        return ok, (f"FST x{len(STEPS)}: {' -> '.join(str(v) for v in seq)} Hz (a press up the list, round at the top "
                    f"{bool(wraps)}); held: " + (f"{before} -> {after} (one down)" if own else
                                                  "not tried (FST's hold given another action)") + f"; put back {back}")
    per_radio(run, "cover_b", "fst", fn, ["radio-core-fst-step"])


def t_key_actions(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        k0 = keys_get(ip)
        all_own = all(t["press"] == -1 and t["hold"] == -1 for t in k0["top"])
        muted0, step0, mode0 = S(r, "muted"), None, None
        if S(r, "ulock") or muted0:
            return None, f"the dial locked ({S(r, 'ulock')}) or muted ({muted0}) as found: left alone"
        res, said = {}, []
        sys0 = gstate(h).get("s.sys")
        try:
            to_classic(r, h)
            step0, mode0 = S(r, "step"), S(r, "mode")     # the step is the mode's own
            for a, name, k in ((3, "Mute", "muted"), (17, "Frequency lock", "ulock"), (25, "VFO A/B", "vfo")):
                keys_set(ip, "hold_fst", a)
                v0 = S(r, k)
                chold(r, "FST")
                on = wait_state(r, lambda s, k=k, v0=v0: int(s.get(k, v0)) != v0, 2.0)
                chold(r, "FST")
                off = wait_state(r, lambda s, k=k, v0=v0: int(s.get(k, -1)) == v0, 2.0)
                res[name] = on and off
            keys_set(ip, "hold_fst", 4)             # Step up
            s0 = S(r, "step")
            chold(r, "FST")
            res["Step up"] = wait_state(r, lambda s: int(s.get("step", s0)) != s0, 2.0)
            set_step(r, step0)
            keys_set(ip, "hold_fst", 23)            # Mode: next, then back
            m0 = S(r, "mode")
            chold(r, "FST")
            nxt = wait_state(r, lambda s: int(s.get("mode", m0)) != m0, 2.0)
            m1 = S(r, "mode")
            keys_set(ip, "hold_fst", 24)
            chold(r, "FST")
            res["Mode: next, Mode: back"] = nxt and wait_state(r, lambda s: int(s.get("mode", -1)) == m0, 2.0)
            said.append(f"mode {m0} -> {m1} -> {S(r, 'mode')}")
            keys_set(ip, "hold_fst", 28)            # AGC
            g0 = S(r, "agc")
            chold(r, "FST")
            res["AGC"] = wait_state(r, lambda s: int(s.get("agc", g0)) == (g0 + 1) % 4, 2.0)
            keys_set(ip, "hold_fst", 101)           # APP FT8: opened (receive only), closed again
            chold(r, "FST")
            res["APP FT8"] = wait_state(r, lambda s: s.get("app") in FT8_APPS, 8.0)
            r.send("ft8 close")
            wait_state(r, lambda s: s.get("app") not in FT8_APPS, 8.0)
            wait_state(r, lambda s: on_classic(r), 4.0)
            # FST's own job again: Reset to own (all were own as found), else FST's hold as found
            if all_own:
                keys_set(ip, "reset_top")
            else:
                keys_set(ip, "hold_fst", top_of(k0, "fst")["hold"])
            if top_of(keys_get(ip), "fst")["hold"] == -1:
                s1 = S(r, "step")                   # the mode now's (FT8 may have left another): back to it after
                chold(r, "FST")
                res["own again: step down"] = wait_state(r, lambda s: int(s.get("step", s1)) < s1, 2.0) or s1 == STEPS[0]
                set_step(r, s1)
        finally:
            if S(r, "ulock"):
                r.send("set lock 0")
                wait_state(r, lambda s: int(s.get("ulock", 1)) == 0, 3.0)
            back = keys_put_back(ip, k0)
            unmuted = unmute(r, ip, muted0)
            if step0 is not None and S(r, "mode") == mode0:
                set_step(r, step0)
            sys_back(r, h, sys0)
        return all(res.values()) and back and unmuted, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) \
            + " (" + "; ".join(said) + f"); the keys as found {back} ({'Reset to own' if all_own else 'one by one'}); " \
            f"mute as found {unmuted}"
    per_radio(run, "cover_b", "key-actions", fn, ["radio-core-key-action-list", "radio-core-top-side-key-actions"])


def t_user_page(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        k0 = keys_get(ip)
        sys0 = gstate(h).get("s.sys")
        moved = False
        try:
            to_classic(r, h)
            keys_set(ip, "user_f1", 23)             # Mode: next
            keys_set(ip, "hold_fst", 30)            # USER page
            ctap(r, "GEN")                          # a GEN page: its F1 the page button, should the USER page not come
            m0 = S(r, "mode")
            chold(r, "FST")
            time.sleep(0.6)
            ctap(r, "F1")
            moved = wait_state(r, lambda s: int(s.get("mode", m0)) != m0, 2.0)
            m1 = S(r, "mode")
            ctap(r, "GEN")
        finally:
            back = keys_put_back(ip, k0)
            sys_back(r, h, sys0)
        return moved and back, (f"USER F1 = Mode next, FST held = USER page: F1 then took the mode {m0} -> {m1} "
                                f"({moved}); the keys as found {back} (the button's name on screen not read)")
    per_radio(run, "cover_b", "user-page", fn, ["radio-core-user-page"])


def t_page_key_hold(run):
    @guarded
    def fn(r):
        h, ip = r.host, ip_of(r)
        k0 = keys_get(ip)
        m0 = S(r, "muted")
        if m0:
            return None, "muted as found: left alone"
        sys0 = gstate(h).get("s.sys")
        res = {}
        try:
            keys_set(ip, "long_key", 3)             # KEY's long press: Mute
            bench.open_page(r, "A")
            hold(r, "KEY")
            time.sleep(1.0)
            res["in the Sentry: nothing (KEY transmits: locked out)"] = S(r, "muted") == m0
            to_classic(r, h)
            chold(r, "KEY")
            res["classic screen: muted"] = wait_state(r, lambda s: int(s.get("muted", 0)) == 1, 2.5)
            chold(r, "KEY")
            res["again: not muted"] = wait_state(r, lambda s: int(s.get("muted", 1)) == 0, 2.5)
        finally:
            if S(r, "muted") != m0 and on_classic(r):
                chold(r, "KEY")
            back = keys_put_back(ip, k0)
            unmuted = unmute(r, ip, m0)
            sys_back(r, h, sys0)
        return all(res.values()) and back and unmuted, "; ".join(
            f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + f"; KEY's long press as found {back}"
    per_radio(run, "cover_b", "page-key-hold", fn, ["radio-core-page-key-long-press"])


def t_agc_split(run):
    @guarded
    def fn(r):
        h = r.host
        if r.name != "x6100":
            t0 = time.time()
            r.send("key AGC 1")
            got = line_after(r, t0, "err unknown key AGC", 2.0)
            return bool(got), f"the X6200 has no AGC key: {got!r}"
        bench.open_page(r, "A")
        bench.ensure(r, {"split": 0})
        sys0 = gstate(h).get("s.sys")
        res = {}
        try:
            to_classic(r, h)
            chold(r, "AGC")
            res["AGC held: split on"] = wait_state(r, lambda s: int(s.get("split", 0)) == 1, 2.5)
            chold(r, "AGC")
            res["again: off"] = wait_state(r, lambda s: int(s.get("split", 1)) == 0, 2.5)
            r.send("ft8 open")
            res["FT8 open"] = wait_state(r, lambda s: s.get("app") in FT8_APPS, 10)
            hold(r, "AGC")
            time.sleep(1.0)
            res["with FT8 open: no split"] = S(r, "split") == 0
        finally:
            if r.state.get("app") in FT8_APPS:
                r.send("ft8 close")
                wait_state(r, lambda s: s.get("app") not in FT8_APPS, 10)
            sys_back(r, h, sys0)
        return all(res.values()), "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items())
    per_radio(run, "cover_b", "agc-split", fn, ["radio-core-x6100-agc-hold-split"])


class Stop(Exception):
    """A press that did not do what the page it was believed on says: the test goes no further on those pages"""


def press_toggle(r, key, k, v0, res, name):
    """A button pressed on a page the test believes it is on: its setting k must switch; if it does not, pressed
    again (whatever it did, undone) and the test stops there"""
    ctap(r, key)
    on = wait_state(r, lambda s: int(s.get(k, v0)) != v0, 2.0)
    if not on:
        ctap(r, key)
        res[name] = False
        raise Stop(name)
    ctap(r, key)
    res[name] = wait_state(r, lambda s: int(s.get(k, -1)) == v0, 2.0)


def t_value_buttons(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        dnf0 = params(h, ["dnf"]).get("dnf")
        res = {}
        try:
            to_classic(r, h)
            ctap(r, "GEN")
            ctap(r, "DFN")                          # DFN 1:3 (another key's page before it)
            ctap(r, "DFN")                          # DFN 2:3: F2 NB
            try:
                press_toggle(r, "F2", "nb", S(r, "nb"), res, "DFN 2:3 NB")
                ctap(r, "DFN")                      # DFN 3:3: F2 NR
                press_toggle(r, "F2", "nr", S(r, "nr"), res, "DFN 3:3 NR")
                ctap(r, "GEN")                      # VOL 1:2
                ctap(r, "GEN")                      # VOL 2:2: F3 PRE, F4 ATT, F5 ATU (in or out; tuning not here)
                press_toggle(r, "F3", "pre", S(r, "pre"), res, "VOL 2:2 PRE")
                press_toggle(r, "F4", "att", S(r, "att"), res, "VOL 2:2 ATT")
                press_toggle(r, "F5", "atu", S(r, "atu"), res, "VOL 2:2 ATU")
                ctap(r, "GEN")                      # MFK 1:4
                ctap(r, "GEN")                      # MFK 2:4: F2 AGC
                a0 = S(r, "agc")
                ctap(r, "F2")
                res["MFK 2:4 AGC: the next"] = wait_state(r, lambda s: int(s.get("agc", a0)) == (a0 + 1) % 4, 2.0)
                if not res["MFK 2:4 AGC: the next"]:
                    ctap(r, "F2")
            except Stop:
                pass
        finally:
            dnf1 = params(h, ["dnf"]).get("dnf")
            sys_back(r, h, sys0)
        return all(res.values()) and dnf1 == dnf0, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) + \
            f" (each switched and back; DNF untouched {dnf1 == dnf0}; a value button's press onto a knob not tried: " \
            "the knob's control would stay changed)"
    per_radio(run, "cover_b", "value-buttons", fn, ["radio-core-value-buttons"])


def bind_step(a, b):
    """The one position two encoder_bind strings differ in, and whether b's letter follows a's (_ -> V -> M -> _)"""
    if a is None or b is None or len(a) != len(b):
        return None, False
    d = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    if len(d) != 1:
        return None, False
    i = d[0]
    return i, {"_": "V", "V": "M", "M": "_"}.get(a[i]) == b[i]


def t_knob_bind(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        e0 = params(h, ["encoder_bind"]).get("encoder_bind")
        binds = [e0]
        try:
            to_classic(r, h)
            ctap(r, "GEN")
            ctap(r, "DFN")
            ctap(r, "GEN")                          # VOL 1:2: F3 Squelch
            for _ in range(3):
                chold(r, "F3")
                last = binds[-1]
                binds.append(pwait(h, "encoder_bind", lambda v, last=last: v is not None and v != last, 3.0))
                if binds[-1] == last:
                    break
        finally:
            e1 = params(h, ["encoder_bind"]).get("encoder_bind")
            for _ in range(2):                      # a cycle cut short: round to where it was (_ V M, three holds)
                if e0 is None or e1 == e0 or e1 == binds[0] or not on_classic(r) or len(binds) < 2 or \
                        binds[1] == binds[0]:
                    break
                chold(r, "F3")
                e1 = pwait(h, "encoder_bind", lambda v, last=e1: v != last, 3.0)
            sys_back(r, h, sys0)
        steps = [bind_step(a, b) for a, b in zip(binds[1:], binds[2:])]
        first = binds[1] != binds[0]
        same_pos = len({i for i, _ in steps}) == 1 and all(ok for _, ok in steps)
        back = e1 == e0 if e0 is not None else (len(binds) == 4 and binds[3] not in (binds[1], binds[2]))
        i = steps[0][0] if steps else None
        letters = [b[i] if b and i is not None and i < len(b) else "?" for b in binds]
        ok = len(binds) == 4 and first and same_pos and back
        return ok, (f"VOL 1:2's F3 held three times: control {i}'s knob {' -> '.join(letters)} (_ none, V VOL, M MFK; "
                    f"params.db encoder_bind), as found {back} (the message line not read)")
    per_radio(run, "cover_b", "knob-bind", fn, ["input-knob-assign-hold"])


WALK = ("vfo", "mode", "agc", "pre", "att", "atu", "nr", "nb", "rit", "xit", "step")


def t_walk(run):
    @guarded
    def fn(r):
        h = r.host
        sys0 = gstate(h).get("s.sys")
        dnf0 = params(h, ["dnf"]).get("dnf")
        try:
            to_classic(r, h)
            before = {k: S(r, k) for k in WALK}
            chold(r, "MFK")                         # the frame on the first value
            r.send("knob mfk 1")                    # to the next
            time.sleep(0.6)
            ctap(r, "MFK")                          # picked
            mid = {k: S(r, k) for k in WALK}
            r.send("knob mfk 1")                    # changed where it is
            time.sleep(1.0)
            after = {k: S(r, k) for k in WALK}
            r.send("knob mfk -1")                   # and back
            time.sleep(1.0)
            undone = {k: S(r, k) for k in WALK}
            ctap(r, "MFK")                          # the MFK its own again
        finally:
            dnf1 = params(h, ["dnf"]).get("dnf")
            sys_back(r, h, sys0)
        changed = [k for k in WALK if after[k] != mid[k]]
        if "mode" in changed:           # the step and zoom kept per mode come with it (zoom-step): not a second value
            changed = [k for k in changed if k not in ("step", "zoom")]
        ok = mid == before and len(changed) == 1 and undone == before and dnf1 == dnf0
        what = ", ".join(f"{k} {mid[k]} -> {after[k]}" for k in WALK if after[k] != mid[k]) or "NOTHING changed"
        return ok, (f"MFK held, one click, a press, one click: {what} (the manual's walk: VFO, then mode); turned "
                    f"back: as before {undone == before}; nothing else "
                    f"moved on the way {mid == before}; DNF untouched {dnf1 == dnf0} (the frame on screen not read)")
    per_radio(run, "cover_b", "walk", fn, ["radio-core-walk-values"])


def t_freq_lock(run):
    @guarded
    def fn(r):
        ip = ip_of(r)
        k0 = keys_get(ip)
        if S(r, "ulock") or S(r, "lock"):
            return None, "the dial locked as found: left alone"
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        res = {}
        try:
            keys_set(ip, "lock_per_band", 1)
            r.send("set lock 1")
            res["20 m USB locked"] = wait_state(r, lambda s: int(s.get("ulock", 0)) == 1, 3)
            r.send("set mode 4")
            wait_state(r, lambda s: int(s.get("mode", -1)) == 4, 3)
            time.sleep(0.8)
            res["20 m CW: its own, free"] = S(r, "ulock") == 0
            r.send("set mode 2")
            wait_state(r, lambda s: int(s.get("mode", -1)) == 2, 3)
            time.sleep(0.8)
            res["20 m USB again: locked"] = S(r, "ulock") == 1
            r.send("set lock 0")
            res["unlocked"] = wait_state(r, lambda s: int(s.get("ulock", 1)) == 0, 3)
        finally:
            if S(r, "ulock"):
                if S(r, "mode") != 2:
                    r.send("set mode 2")
                    wait_state(r, lambda s: int(s.get("mode", -1)) == 2, 3)
                r.send("set lock 0")
                wait_state(r, lambda s: int(s.get("ulock", 1)) == 0, 3)
            back = keys_put_back(ip, k0)
        free = S(r, "ulock") == 0
        return all(res.values()) and back and free, "; ".join(f"{k} {'yes' if v else 'NO'}" for k, v in res.items()) \
            + f"; per band and mode as found {back}, the dial free {free} (across a restart: not tried)"
    per_radio(run, "cover_b", "freq-lock", fn, ["radio-core-freq-lock-per-band-mode"])


def t_band_across(run):
    @guarded
    def fn(r):
        bm = bench.band_memories(r)
        if not bm:
            return None, "the band memories not read (params.db)"
        cw = bm["mem"].get(3, {})                   # 40m CW (params.db bands)
        if "vfoa_mode" not in cw:
            return None, "40 m CW has no mode saved: nothing to tell it from"
        want = {"mode": cw["vfoa_mode"]}
        setup = {"freq": 7040000, "mode": 0 if cw["vfoa_mode"] != 0 else 2}
        for k in ("pre", "att"):
            if f"vfoa_{k}" in cw:
                want[k] = cw[f"vfoa_{k}"]
                setup[k] = 1 - cw[f"vfoa_{k}"]
        if "vfoa_agc" in cw:
            want["agc"] = cw["vfoa_agc"]
            setup["agc"] = (cw["vfoa_agc"] + 1) % 4
        bench.open_page(r, "A")                 # A's VFO tunes the X6200 a step a click (MANUAL)
        bench.ensure(r, setup)
        step0 = S(r, "step")
        back, f1, got, vb, vb2 = False, None, {}, None, None
        try:
            set_step(r, 1000)
            r.send("set vfo 1")
            wait_state(r, lambda s: int(s.get("vfo", 0)) == 1, 3)
            vb = (S(r, "freq"), S(r, "mode"))
            r.send("set vfo 1")
            wait_state(r, lambda s: int(s.get("vfo", 1)) == 0, 3)
            wait_state(r, lambda s: int(s.get("freq", 0)) == 7040000, 3)
            r.send("knob main -1")
            wait_state(r, lambda s: int(s.get("freq", 0)) != 7040000, 3)
            time.sleep(1.2)
            f1 = S(r, "freq")
            got = {k: S(r, k) for k in want}
            r.send("set vfo 1")
            wait_state(r, lambda s: int(s.get("vfo", 0)) == 1, 3)
            vb2 = (S(r, "freq"), S(r, "mode"))
            r.send("set vfo 1")
            wait_state(r, lambda s: int(s.get("vfo", 1)) == 0, 3)
        finally:
            bench.ensure(r, {"freq": 7040000, "mode": setup["mode"]})
            back = set_step(r, step0)
        ok = f1 == 7039000 and got == want and vb2 == vb and back
        return ok, (f"40 m SSB 7.040 ({setup}), the VFO one 1 kHz click down: {f1 / 1e6:.3f} (40 m CW) took {got} "
                    f"(40 m CW's saved {want}); VFO B {vb} -> {vb2}; the step as found {back}")
    per_radio(run, "cover_b", "band-across", fn, ["radio-core-band-tune-across"])


# ======== G's own channel and its channel list (v0.3.12.48: the owner, 2026-11-23) ========
CB = {9: 27065000, 19: 27185000, 20: 27205000, 21: 27215000}       # CB channels' Hz (AM: mode 6), RX only


def t_g_own(run):
    """G opens on its own channel and gives A's dial back as it is left; a channel tuned on G is G's own, a band's dial
    is not; the remote's wch says it and "set wch" sets the memory alone (a frequency the radio does not tune refused).
    Receive only: CB is never keyed. finish puts wch back as found (SET_ORDER)"""
    @guarded
    def fn(r):
        if "wch" not in r.state:
            return None, "this GUI has no G channel of its own (before v0.3.12.48)"
        bench.open_page(r, "A")
        a0 = {k: S(r, k) for k in ("freq", "mode", "agc", "pre", "att")}
        w0 = (S(r, "wch"), S(r, "wchm"))
        res = {}
        # the remote: G's memory set, the dial not tuned; a frequency this radio does not tune refused
        r.send(f"set wch {CB[21]} 6")
        r.wait("wch set", lambda: (S(r, "wch"), S(r, "wchm")) == (CB[21], 6), 5)
        res["set wch: memory only"] = S(r, "freq") == a0["freq"]
        n = len(r.errs)                             # the lines deque is bounded: the errors' list is not
        r.send("set wch 5 6")
        time.sleep(1.0)
        res["set wch 5 refused"] = any("wch" in x for x in r.errs[n:]) and S(r, "wch") == CB[21]
        # G entered: its own channel tuned (CB 21 AM)
        bench.open_page(r, "G")
        try:
            r.wait("G on its channel", lambda: S(r, "freq") == CB[21] and S(r, "mode") == 6, 6)
            res["G opens on its channel"] = True
        except bench.Refused:
            res["G opens on its channel"] = False
        # a channel tuned on G is G's own; a band's dial (14.200, 20 M's) is not
        bench.ensure(r, {"freq": CB[9], "mode": 6})
        try:
            r.wait("CB 9 G's own", lambda: S(r, "wch") == CB[9], 5)
            res["CB 9 tuned on G: G's own"] = True
        except bench.Refused:
            res["CB 9 tuned on G: G's own"] = False
        bench.ensure(r, {"freq": 14200000, "mode": 2})
        time.sleep(1.5)
        res["14.200 on G: not G's"] = S(r, "wch") == CB[9]
        # G left: A's dial back exactly (frequency, mode, AGC, PRE, ATT)
        bench.open_page(r, "A")
        try:
            r.wait("A's dial back", lambda: all(S(r, k) == v for k, v in a0.items()), 6)
            res["A's dial back"] = True
        except bench.Refused:
            res["A's dial back"] = False
        got = {k: S(r, k) for k in a0}
        ok = all(res.values())
        return ok, (", ".join(f"{k}: {'yes' if v else 'NO'}" for k, v in res.items())
                    + f"; A {a0} -> {got}; wch as found {w0} (put back by finish)")
    per_radio(run, "cover_b", "g-own", fn, ["g-own-channel", "remote-g-channel", "g-follows-dial"])


def t_g_list(run):
    """G's F4 CH LIST on CB: the MFK two down the list and its push tune CB 21 from CB 19 and close the list (the dial
    moves no more with the MFK: off the list it goes group to group, so a third click would leave CB). Receive only"""
    @guarded
    def fn(r):
        if "wch" not in r.state:
            return None, "this GUI's F4 on G is 3D/2D (before v0.3.12.48)"
        bench.open_page(r, "G")
        time.sleep(0.8)
        bench.ensure(r, {"freq": CB[19], "mode": 6})
        time.sleep(0.8)
        bench.tap(r, "F4")                          # CH LIST
        time.sleep(1.2)
        clicks(r, "mfk", 2, gap=0.5)                # the X6100's G (portrait, held up) missed clicks 0.3 s apart
        time.sleep(0.8)
        moved_early = S(r, "freq") != CB[19]        # the list picks; the dial waits for the push
        bench.tap(r, "MFK")
        try:
            r.wait("CB 21", lambda: S(r, "freq") == CB[21], 5)
            tuned = True
        except bench.Refused:
            tuned = False
        f1 = S(r, "freq")
        ok = tuned and not moved_early
        return ok, (f"CB 19 ({CB[19] / 1e6:.3f}), F4, the MFK two clicks"
                    f"{' (the dial moved before the push: NOT the list)' if moved_early else ''}, its push: "
                    f"{f1 / 1e6:.3f} MHz ({'CB 21' if tuned else 'NOT CB 21'}); a keyboard's digits are not reachable "
                    "from the remote (its keys are the radio's)")
    per_radio(run, "cover_b", "g-list", fn, ["g-ch-list"])


def t_close_rtty(run):
    """The remote's close closes the RTTY panel (the one app with no window): opened (E's types, as open_app does),
    the state's app RTTY, then "close": none within 3 s"""
    @guarded
    def fn(r):
        import suite_more
        if not suite_more.open_app(r, "rtty"):
            return False, "the RTTY panel did not open"
        app0 = r.state.get("app") or r.state.get("win")
        r.send("close")
        try:
            r.wait("RTTY closed", lambda: not r.state.get("app") and (r.state.get("win") or "Sentry") in ("Sentry", ""), 4)
            closed = True
        except bench.Refused:
            closed = False
        return closed, (f"opened: {app0!r}; close: " + ("closed" if closed else
                        f"STILL OPEN (app {r.state.get('app')!r}, window {r.state.get('win')!r})"))
    per_radio(run, "cover_b", "close-rtty", fn, ["remote-close-rtty"])


def t_tune_power(run):
    """The remote's tpwr: TUNE's own power read and set, saved in the settings database as Settings does (tenths:
    0.2 W is "2"), "set tpwr 0" clamped to 0.1 W; nothing keyed. finish puts the power back as found, its settings row
    too (SET_ORDER, then the database put-back)"""
    @guarded
    def fn(r):
        if "tpwr" not in r.state:
            return None, "this GUI has no TUNE power setter (before v0.3.12.48)"
        t0 = S(r, "tpwr")
        want = 2 if t0 != 2 else 3
        res = {}
        r.send(f"set tpwr {want}")
        try:
            r.wait(f"tpwr {want}", lambda: S(r, "tpwr") == want, 5)
            res[f"set tpwr {want}"] = True
        except bench.Refused:
            res[f"set tpwr {want}"] = False
        time.sleep(bench.DB_SETTLE_S)               # the GUI writes a changed setting within 3 s
        row = bench.ssh(r.host, "sqlite3 -readonly -cmd '.timeout 3000' /mnt/params.db "
                                "\"select val from params where name='tune_pwr'\"").strip()
        res[f"saved as {want}"] = row == str(want)
        r.send("set tpwr 0")
        try:
            r.wait("tpwr clamped", lambda: S(r, "tpwr") == 1, 5)
            res["set tpwr 0: 0.1 W"] = True
        except bench.Refused:
            res["set tpwr 0: 0.1 W"] = False
        res["nothing keyed"] = not S(r, "tx", 0) and not S(r, "tune", 0)
        return all(res.values()), (", ".join(f"{k}: {'yes' if v else 'NO'}" for k, v in res.items())
                                   + f"; the row read {row!r}; as found {t0} (put back by finish)")
    per_radio(run, "cover_b", "tune-power-remote", fn, ["remote-tune-power"])


TESTS = (("a-scan", t_a_scan), ("manual", t_manual), ("a-span", t_a_span), ("a-band", t_a_band), ("keys", t_keys),
         ("gen-help", t_gen_help), ("x6100-keys", t_x6100_keys), ("vol-knob", t_vol_knob), ("listen", t_listen),
         ("pool", t_pool), ("home", t_home), ("f-open", t_f_open), ("f-go", t_f_go), ("f-classic", t_f_classic),
         ("f-shell", t_f_shell), ("f-tribal", t_f_tribal), ("c-listen", t_c_listen), ("c-empty", t_c_empty),
         ("c-dwell", t_c_dwell), ("d-scan-listen", t_d_scan_listen), ("d-sweep-mark", t_d_sweep_mark),
         ("d-e-rule", t_d_e_rule), ("e-emergency", t_e_emergency), ("e-scan-sel", t_e_scan_sel), ("g-3d", t_g_3d),
         ("g-own", t_g_own), ("g-list", t_g_list), ("close-rtty", t_close_rtty),
         ("tune-power-remote", t_tune_power),
         ("scancfg-open", t_scancfg_open), ("scancfg-style", t_scancfg_style), ("scancfg-step", t_scancfg_step),
         ("scancfg-chans", t_scancfg_chans), ("scancfg-sweep", t_scancfg_sweep), ("scancfg-astep", t_scancfg_astep),
         ("scancfg-rec", t_scancfg_rec), ("df-source", t_df_source), ("df-keys", t_df_keys), ("df-tune", t_df_tune),
         ("df-targets", t_df_targets), ("fst", t_fst), ("key-actions", t_key_actions), ("user-page", t_user_page),
         ("page-key-hold", t_page_key_hold), ("agc-split", t_agc_split), ("value-buttons", t_value_buttons),
         ("knob-bind", t_knob_bind), ("walk", t_walk), ("freq-lock", t_freq_lock), ("band-across", t_band_across))


def batch(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE cover_b: {name} (receive only, nothing sent)")
            fn(run)
