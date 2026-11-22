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


def ssh(host, cmd, timeout=60):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                       capture_output=True, text=True, timeout=timeout)
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
        self.r = Remote(self.host, int(rc.get("port", 50002)))     # another port: the simulated radios' (sim_test.py)
        self.state, self.meter, self.txm, self.ft8, self.apps = {}, None, None, {}, None
        self.rows, self.errs, self.lock = [], [], threading.Lock()
        self.tx, self.tx_since, self.ctl_mine, self.role = False, None, False, None
        self.tx_last = None         # (on, off) of the last over: the pad's rest above HARD_MAX_W
        self.tx_off_at = 0.0       # when it last left TX (the radios' gate: one on the air at a time)
        self.last_msg = time.time()
        self.r.send(f"auth {tok}")
        self.r.send("panel 1")
        self.r.send("screen 0")
        threading.Thread(target=self._read, daemon=True).start()
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

    def _read(self):
        while True:
            t = self.r.recv()
            if t is None:
                return
            k, _, rest = t.partition(" ")
            self.last_msg = time.time()        # anything at all from it: a stall is measured, not guessed
            with self.lock:
                if k == "state":
                    self.state = json.loads(rest)
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
                elif k == "role":
                    self.role = rest.split(" ", 2)
                elif k == "err":
                    self.errs.append(rest)

    def _tx(self, on):
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


def radio_tx_gate(radio, c, w):
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
    freq = int(radio.state.get("freq", 0))
    if not any(lo <= freq <= hi for lo, hi in HAM_HF):
        raise Refused(f"{radio.name}: {freq} Hz is outside the amateur bands")
    if abs(radio.pwr() - w) > 0.05:
        raise Refused(f"{radio.name}: its power reads {radio.pwr()} W, not the {w} W asked")


class Watch:
    """While a test runs: a radio on the air past max_key_s, on a bad SWR, or over the rating of the pad on its jack,
    or a receiver hearing far more than the chain lets through, stops everything at once. More power than asked, under
    the pad's rating, is logged (said, tx_log, over_power) and the over goes on"""

    def __init__(self, radios, c, hackrf=None, expect_dbm=None, listener=None, w=None):
        self.radios, self.c, self.hackrf, self.why = radios, c, hackrf, None
        self.expect_dbm, self.listener, self.w = expect_dbm, listener, w
        self.tx_peak = {}       # each radio's own forward-power meter, its highest while on the air (W)
        self.over_power = {}      # the radios that went over the power asked, and their highest reading (W)
        # the rating of the pad on each radio's jack (the radios profile), the most a reading may be: the watch's stop
        pads = c.get("profiles", {}).get("radios", {}).get("pads", [])
        self.pad_w = {x["at"]: float(x.get("watts", HARD_MAX_W)) for x in pads if x.get("at")}
        self.rx_peak = None     # the listener's S-meter, its highest while another radio is on the air (dBm)
        self.on = True
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while self.on and not self.why:
            if self.hackrf is None and sum(1 for r in self.radios if r.tx) > 1:
                # the owner, 2026-11-19: "won't that damage the radios? don't do that again" (at 0.1 W through 70 dB
                # each hears the other at -50 dBm, no harm; never all the same): one on the air at a time
                self.trip("two radios on the air at once: " + ", ".join(r.name for r in self.radios if r.tx))
            for r in self.radios:
                with r.lock:
                    since, txm, meter = r.tx_since, r.txm, (self.listener.meter if self.listener else None)
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
                    if swr > MAX_SWR:
                        self.trip(f"{r.name}: SWR {swr:.2f} on the air: the chain is not a load")
                    elif w > self.pad_w.get(r.name, HARD_MAX_W):
                        self.trip(f"{r.name}: {w:.1f} W on the air, over the {self.pad_w.get(r.name, HARD_MAX_W):g} W "
                                  f"its pad takes")
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

    def close(self):
        self.on = False


def emergency_stop(c):
    subprocess.run(["pkill", "-f", "hackrf_transfer"], capture_output=True)
    toks = load_json(os.path.join(STATE, "tokens.json"), {})
    for name, rc in c.get("radios", {}).items():
        for kind in ("tx", "rx"):
            if toks.get(name, {}).get(kind):
                try:
                    r = Radio(name, rc, kind == "tx")
                    r.take_control()
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
    radio.send(f"banner BENCH HackRF into {name.upper()} through the pads, {name.upper()} RECEIVES ONLY: FT8 "
               f"'{text}' on {args.band}")
    found, watch, hk = None, None, None
    try:
        radio.send(f"set freq {dial}")      # first: the app holds the dial once open
        radio.send("ft8 open")
        radio.wait("the FT8 app", lambda: radio.ft8.get("open") == 1, 10)
        rx_only_gate(radio)
        ft8_to_band(radio, dial)
        radio.wait(f"the dial on {dial}", lambda: int(radio.state.get("freq", 0)) == dial, 5)
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
        radio.close()
    if watch.why:
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
    was = {k: radio.state.get(k) for k in ("freq", "mode", "sql")}
    hk, watch, during, was_filter = None, None, None, None
    try:
        radio.send(f"set freq {args.at}")          # the frequency first: a new band brings its own mode back
        radio.wait(f"{args.at} Hz", lambda: int(radio.state.get("freq", 0)) == args.at, 5)
        radio.send(f"set mode {args.mode}")
        radio.send("set sql 0")
        radio.wait(f"mode {args.mode}", lambda: int(radio.state.get("mode", -1)) == args.mode, 5)
        if args.filter:                         # the mode's own filter, put back before the mode is
            was_filter = (radio.state.get("flo"), radio.state.get("fhi"))
            radio.send(f"set filter {args.filter[0]} {args.filter[1]}")
            radio.wait(f"the filter {args.filter}", lambda: [radio.state.get("flo"), radio.state.get("fhi")] ==
                       list(args.filter), 5)
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
        if was_filter and None not in was_filter:
            radio.send(f"set filter {int(was_filter[0])} {int(was_filter[1])}")
            time.sleep(0.4)
        for k in ("freq", "mode", "sql"):        # the frequency first, as above
            if was.get(k) is not None:
                radio.send(f"set {k} {int(was[k])}")
                time.sleep(0.4)
        time.sleep(0.5)
        radio.close()
    if during:
        out, _ = during.communicate(timeout=120)
        say("beside it:\n" + out.rstrip())
    if watch.why:
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
    was_pwr, was_rx = {}, {}
    for r in radios:
        r.take_control()
        was_pwr[r.name] = r.pwr()          # put back after (the first run left both at 0.1 W, 2026-11-18)
        was_rx[r.name] = {"freq": r.state.get("freq"), "mode": r.state.get("mode")}    # and where they were tuned
        if getattr(args, "rx_alike", False):
            # the receivers alike for a 1:1 of the two: preamp and attenuator off (the first run had the X6200's preamp
            # on and the X6100's off, 13-18 dB apart); put back after
            was_rx[r.name].update({k: r.state.get(k) for k in ("pre", "att", "rfg")})
            r.send("set pre 0")
            r.send("set att 0")
            # and RF gain at its top: below 100 the X6100's base switches in its stepped attenuator (100 - rfg steps,
            # strijar/x6100-base docs/protocol.md), and the X6100 had run at 63, its default, through every 1:1 so far
            r.send("set rfg 100")
        r.send(f"set freq {dial}")          # first: the app holds the dial once open
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
    watch = Watch(radios, c, expect_dbm=expect, listener=b, w=FIRST_PROBE_W)
    result = {}
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
        watch.listener, watch.expect_dbm = a, expect
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
            if r.name in was_pwr and was_pwr[r.name] < 99:
                r.send(f"set pwr {round(was_pwr[r.name] * 10)}")
            for k in ("freq", "mode", "pre", "att", "rfg"):  # the frequency first: a band brings its own mode back
                v = was_rx.get(r.name, {}).get(k)
                if v is not None:
                    r.send(f"set {k} {int(v)}")
                    time.sleep(0.3)
            time.sleep(0.3)
            r.close()
        for r in radios:
            clean_log(r.host)
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
    was = {}
    for r in radios:
        r.take_control()
        was[r.name] = {k: r.state.get(k) for k in ("freq", "mode", "pg", "atu")}
        was[r.name]["pwr"] = r.pwr()
        if r.state.get("atu"):                       # the tuner out for the test (it tunes by sending), back after
            r.send("set atu 0")
            r.wait("the tuner out", lambda r=r: not r.state.get("atu"), 10)
        r.send("open sentry G")
        r.wait("G, the Walkie", lambda r=r: r.state.get("pg") == "G", 10)
        r.send(f"set pwr {round(FIRST_PROBE_W * 10)}")
        r.wait("the power", lambda r=r: abs(r.pwr() - FIRST_PROBE_W) < 0.05, 5)
    expect = 10 * math.log10(FIRST_PROBE_W * 1000) - total
    watch = Watch(radios, c, expect_dbm=expect, listener=b, w=FIRST_PROBE_W)
    results, hold = [], [int(time.time()) % 100000]

    def tap(r, key):
        r.send(f"key {key} 1")
        time.sleep(0.15)
        r.send(f"key {key} 0")

    def arm(r, on):
        for tries in (1, 2, 3):                 # a tap the radio missed (over WiFi): again, then refused
            if (r.state.get("rxo") == 0) == on:
                return
            tap(r, "AB")
            try:
                r.wait("TX " + ("armed" if on else "locked again"), lambda r=r: (r.state.get("rxo") == 0) == on, 5)
                return
            except Refused:
                if tries == 3:
                    raise

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
            tune(r, band, hz, mode)
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
        arm(sender, True)
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
        peak, w = watch.rx_peak, watch.tx_peak.get(sender.name, 0.0)
        # heard: the listener's own spectrum at the dial up WALKIE_RISE_DB (its S-meter beside it: the X6200's did not
        # follow an AM signal, 2026-11-19)
        # no quiet reading (the listener's link silent before the over): the signal against what the chain lets through
        spec_rise = keyed and sp is not None and (sp - sq >= WALKIE_RISE_DB if sq is not None
                                                  else sp >= expect - 30)
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
               "over_power_w": watch.over_power.get(sender.name), "listener_silent_s": round(gap, 1)}
        results.append(res)
        where = f"page {listener.state.get('pg')}, {listener.state.get('freq')} Hz"
        say(f"{'PASS' if heard else 'FAIL'} {band} {mode} {sender.name} -> {listener.name}: keyed {keyed}"
            f"{f' after {on_at - t0:.1f} s' if on_at else ''}, "
            f"{w:.2f} W by its meter; {listener.name}'s spectrum at the dial {sq} -> {sp} dB, its S-meter {quiet} -> "
            f"{peak} dBm{'' if meter_rise else ' (the S-meter did not follow)'}"
            f"{'' if stayed else ' (the listener left G or the channel: ' + where + ')'}"
            f" (the chain: about {expect:.0f}); the listener's tx/hold {sorted(set(ltx))}")

    try:
        def tune(r, band, hz, mode):
            """The channel and its mode on r, each asked again once; what r has when it does not come"""
            m = WALKIE_MODES[mode]
            for k, v, get in (("freq", hz, lambda: int(r.state.get("freq", 0))),
                              ("mode", m, lambda: int(r.state.get("mode", -1)))):
                for tries in (1, 2):
                    if get() == v:                    # there already: asked again, the X6200's receiver paused
                        break                         # its spectrum and meter for the over (2026-11-19)
                    r.send(f"set {k} {v}")
                    try:
                        r.wait(f"{band} {mode}: {k} {v}", lambda get=get, v=v: get() == v, 6)
                        break
                    except Refused:
                        if tries == 2:
                            raise Refused(f"{r.name}: {k} {v} not taken (it has freq {r.state.get('freq')} mode "
                                          f"{r.state.get('mode')} rxo {r.state.get('rxo')} pg {r.state.get('pg')}; "
                                          f"it said {r.errs[-1:] or 'nothing'})")

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
    finally:
        watch.close()
        for r in radios:
            r.stop()
        time.sleep(1)
        for r in radios:
            try:
                arm(r, False)
            except Refused:
                pass
            r.send("banner")
            if was[r.name]["pwr"] < 99:
                r.send(f"set pwr {round(was[r.name]['pwr'] * 10)}")
            for k in ("freq", "mode"):          # both on the first radio's channel after (the owner: the radios
                if was[a.name].get(k) is not None:  # side by side, on the same frequency)
                    r.send(f"set {k} {int(was[a.name][k])}")
                    time.sleep(0.3)
            if was[r.name].get("atu"):
                r.send("set atu 1")
            r.send("open sentry G")               # both left on G alike (their frequency, mode and power back)
            time.sleep(0.3)
            r.close()
        out = os.path.join(STATE, "runs", time.strftime("%Y%m%d-%H%M%S-walkie.json"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w") as f:
            json.dump(results, f, indent=1)
        say(f"the overs: {out}")
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
          "parts=re.split(r'(?i)(<eor>)',s);keep=[];m=0\n"
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


def cmd_status(c, a):
    for name in a.names or list(c["radios"]):
        try:
            toks = load_json(os.path.join(STATE, "tokens.json"), {}).get(name, {})
            r = Radio(name, c["radios"][name], "rx" not in toks)
            time.sleep(1.5)
            s = r.state
            say(f"{name}: {s.get('freq')} Hz mode {s.get('mode')} pwr {s.get('pwr')} W atu {s.get('atu')} "
                f"tx {int(r.tx)} app {s.get('app') or '-'} S {r.meter} dBm; session may transmit: {r.may_tx}")
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
        say(f"{r.name}: receive-only session, not on the air, FT8 not set to send")
        r.close()
    else:
        for n in p.get("between", []):
            r = Radio(n, c["radios"][n], True)
            call = ssh(r.host, "sqlite3 /mnt/params.db \"select val from params where name='callsign'\"").strip()
            say(f"{n}: may transmit {r.may_tx}, ATU {r.state.get('atu')}, {r.pwr()} W, callsign {call or 'none'}"
                f"{'' if TEST_CALL.match(call) else ' (NOT a test call)'}")
            r.close()
    say("preflight passed: nothing was sent")


def cmd_testcall(c, a):
    rc = c["radios"][a.name]
    saved = load_json(os.path.join(STATE, "calls.json"), {})
    cur = ssh(rc["host"], "sqlite3 /mnt/params.db \"select val from params where name='callsign'\"").strip()
    if a.call in ("restore", "--restore"):
        old = saved.pop(a.name, None)
        if old is None:
            raise Refused(f"{a.name}: no callsign kept to go back to")
        q = (f"insert or replace into params(name,val) values('callsign','{old}')" if old else
             "delete from params where name='callsign'")
    else:
        if not TEST_CALL.match(a.call):
            raise Refused("a test callsign is Q<digit>TST")
        if not TEST_CALL.match(cur):
            saved[a.name] = cur
        q = f"insert or replace into params(name,val) values('callsign','{a.call}')"
    save_json(os.path.join(STATE, "calls.json"), saved)
    ssh(rc["host"], f"/etc/init.d/S95gui stop >/dev/null 2>&1; sqlite3 /mnt/params.db \"{q}\"; "
                    f"/etc/init.d/S95gui start >/dev/null 2>&1")
    say(f"{a.name}: callsign {cur or 'none'} -> {a.call if a.call not in ('restore', '--restore') else (old or 'none')}")


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
    s = sub.add_parser("hackrf-ft8")
    s.add_argument("--band", default="20m", choices=FT8_DIAL); s.add_argument("--x", type=int, default=None)
    s.add_argument("--text", default="CQ Q9TST FN00"); s.add_argument("--audio", type=int, default=1500)
    s = sub.add_parser("hackrf-iq"); s.add_argument("file"); s.add_argument("--at", type=int, required=True)
    s.add_argument("--offset", type=int, required=True); s.add_argument("--fs", type=int, default=2000000)
    s.add_argument("--mode", type=int, default=7); s.add_argument("--x", type=int, default=None)
    s.add_argument("--during", default=None); s.add_argument("--lead", type=float, default=3.0)
    s.add_argument("--filter", type=int, nargs=2, default=None, metavar=("LO", "HI"))
    s = sub.add_parser("radios-walkie"); s.add_argument("a"); s.add_argument("b")
    s.add_argument("--bands", default=""); s.add_argument("--modes", default="am,fm")
    s.add_argument("--senders", default="", help="only these send (the others listen): A,B")
    s.add_argument("--keep-armed", action="store_true", help="each radio armed through a channel's overs, as an "
                   "operator keeps it (locked again only to retune), not locked after each over")
    s = sub.add_parser("radios-ft8"); s.add_argument("tx"); s.add_argument("rx")
    s.add_argument("--rx-alike", action="store_true", help="both receivers' preamp and attenuator off, put back after")
    s.add_argument("--band", default="20m", choices=FT8_DIAL)
    s = sub.add_parser("testcall"); s.add_argument("name")
    s.add_argument("call", help="Q<digit>TST, or restore (argparse took --restore for an option: 2026-11-18)")
    s = sub.add_parser("cleanlog"); s.add_argument("name")
    a = ap.parse_args()
    c = None

    def on_signal(*_):
        say("interrupted: the emergency stop")
        emergency_stop(c or {})
        os._exit(130)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    try:
        c = conf()
        if a.cmd in ("hackrf-ft8", "hackrf-iq") and a.x is None:
            a.x = c["limits"]["hackrf_x"]
        {"provision": cmd_provision, "status": cmd_status, "chain-ok": cmd_chain_ok, "power-ok": cmd_power_ok,
         "preflight": cmd_preflight,
         "stop": lambda c, a: emergency_stop(c), "hackrf-ft8": run_hackrf_ft8, "hackrf-iq": run_hackrf_iq, "radios-ft8": run_radios_ft8,
         "radios-walkie": run_radios_walkie,
         "testcall": cmd_testcall, "cleanlog": lambda c, a: clean_log(c["radios"][a.name]["host"])}[a.cmd](c, a)
    except Refused as e:
        say(f"REFUSED: {e}")
        sys.exit(2)


if __name__ == "__main__":
    main()
