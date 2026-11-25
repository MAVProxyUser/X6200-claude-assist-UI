#!/usr/bin/env python3
"""bench.py's radios-ft8 against two simulated radios: each a WebSocket server on this computer speaking the radio
remote's protocol as far as the harness uses it (auth, panel, control, set, ft8 open/cq/reply/stop/close, the PTT and
TUNE stops; the state, the meters, tx, ft8 and ft8r back), the pair passing each transmission to the other as a
decoded row and an S-meter. Six cases: a CQ and its reply pass; a radio at 0.9 W for 0.1 W asked passes too, the
over logged and not stopped (the owner, 2026-11-21: "don't kill just log. we have attenuators"); a radio that stays
keyed, a bad SWR, more power than its pad takes and a receiver hearing far more than the pads let through each end in
the emergency stop and a refusal, with the stopped radio off the air. And the state layer (2026-11-22): a listener
said whole and read back, put back as found and left on the Sentry's page A; refused for an attenuator that will not
go off, a base held in play mode, a sender that would transmit off its dial, a GUI older than the checks; a dial that
will not go back said; a path proof each way before the overs; a deaf listener refused by it, one deaf after it
refused by the watch; a radio whose link stalls stopped. Nothing on the radio, nothing on the air.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
bench.SET_WAIT_S = 1.0      # the simulated radios answer a "set" at once: a refusal's two tries need not take 12 s

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
TX_S = 2.5          # a simulated FT8 transmission: long enough for the watchdog's 1 s settling
DIRS = []           # the cases' state folders, gone at the end


class SimRadio:
    def __init__(self, name, call, fault=None):
        self.name, self.call, self.fault = name, call, fault
        self.peer, self.conn, self.lock = None, None, threading.Lock()
        # the remote's state as the GUI sends it (src/remote_server.c pstate_read), the bench's checks' keys included
        self.state = {"freq": 14074000, "mode": 3, "pwr": 4.4, "atu": 0, "tx": 0, "app": "", "split": 0, "vfo": 0,
                      "flo": 100, "fhi": 3000, "att": 1, "pre": 1, "agc": 3, "rfg": 39, "sql": 4, "nb": 0, "nr": 0,
                      "rit": 0, "xit": 0, "txf": 14074000, "play": 1, "heard": 1,
                      "spk": 30, "moni": 0, "muted": 0, "tcim": 0, "mic": 2, "rxo": 1, "tune": 0, "pg": "B",
                      "vol": 30, "zoom": 1}
        if fault == "oldgui":
            for k in ("txf", "play", "heard", "tcim"):
                del self.state[k]
        self.tone = None    # (hz above the dial, until): a "sim tone" plan on the air
        self.operator = False  # someone at the radio has control ("ctl local 0", as the GUI says a touch took it)
        self.silent = False  # "silentlink": from the test's first FT8 over its link sends nothing more
        # "deaf": it never hears its peer; "deafafter": it hears the path proof's tone, not the test's overs after it
        self.ft8 = {"open": 0, "auto": 1, "cq": 0, "tx": 1, "sending": 0, "nocall": 0}
        self.tx_until, self.heard_dbm, self.ids, self.got = 0.0, None, 0, []
        self.what = None    # what its FT8 app sends ("cq", "reply"); none for a radio keyed by its PTT alone
        self.first_freq = 14074000
        self.refused_once = False
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(2)
        self.port = self.srv.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()
        threading.Thread(target=self._tick, daemon=True).start()

    # -- WebSocket --
    def _accept(self):
        while True:
            c, _ = self.srv.accept()
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, c):
        req = b""
        while b"\r\n\r\n" not in req:
            d = c.recv(4096)
            if not d:
                return
            req += d
        key = next(l.split(":", 1)[1].strip() for l in req.decode().split("\r\n") if l.lower().startswith("sec-websocket-key"))
        acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        c.sendall(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {acc}\r\n\r\n".encode())
        self.conn = c
        self.silent = False                 # a new connection: a stalled link ("stall") comes back with it
        buf = req.split(b"\r\n\r\n", 1)[1]
        try:
            while True:
                while len(buf) < 2:
                    d = c.recv(4096)
                    if not d:
                        return
                    buf += d
                op, n = buf[0] & 15, buf[1] & 127
                hl = 2 + (2 if n == 126 else 8 if n == 127 else 0) + 4
                while len(buf) < hl:
                    buf += c.recv(4096)
                if n == 126:
                    n = struct.unpack(">H", buf[2:4])[0]
                elif n == 127:
                    n = struct.unpack(">Q", buf[2:10])[0]
                while len(buf) < hl + n:
                    d = c.recv(4096)
                    if not d:
                        return
                    buf += d
                mask, data, buf = buf[hl - 4:hl], buf[hl:hl + n], buf[hl + n:]
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
                if op == 8:
                    return
                if op == 1:
                    self._cmd(data.decode())
        except OSError:
            return

    def send(self, text):
        c = self.conn
        if not c or self.silent:            # "silentlink": its link dropped, nothing reaches the harness
            return
        d = text.encode()
        head = bytes([0x81, len(d)]) if len(d) < 126 else bytes([0x81, 126]) + struct.pack(">H", len(d))
        try:
            with self.lock:
                c.sendall(head + d)
        except OSError:
            pass

    # -- the radio --
    def _state(self):
        self.send("state " + json.dumps(self.state))

    def _cmd(self, t):
        a = t.split()
        self.got.append(t)
        if a[0] == "auth":
            self.send(f"role operate 1 bench-tx")
        elif a[:2] == ["panel", "1"]:
            self._state()
            self.send("ft8 " + json.dumps(self.ft8))
            self.send("ctl local 0" if self.operator else "ctl none 0")     # who has control, as the GUI tells a client
        elif a[:2] == ["control", "take"]:
            self.operator = False
            self.send("ctl browser bench-tx 1")
        elif a[0] == "set" and len(a) >= 3:
            k, v = a[1], float(a[2])
            if k == "vfo":
                self.state["vfo"] ^= 1                  # a toggle, as the GUI's P_VFO
            elif k == "filter":
                self.state["flo"], self.state["fhi"] = int(v), int(float(a[3]))
            elif k == "pwr":
                self.state["pwr"] = v / 10
            elif k == "att" and self.fault == "stuckatt":
                self.send("err att: the radio is busy, try again")
            elif k == "freq" and self.fault == "norestore" and int(v) == self.first_freq and self.state["freq"] != v:
                self.send("err freq: locked (LOCK, or an app holds the frequency)")
            elif k == "freq" and self.fault == "flakyrestore" and int(v) == self.first_freq and not self.refused_once:
                self.refused_once = True        # the first time only: a busy radio, then it takes it
                self.send("err freq: the radio is busy, try again")
            else:
                self.state[k] = int(v)
            if k == "heard" and self.fault != "play":     # "play": a base held in play mode by something else
                self.state["play"] = 1 if self.state["heard"] in (1, 2) else 0
            self.state["txf"] = self.state["freq"] + (5000 if self.fault == "split" else 0)
            self._state()
        elif a[0] == "close":                           # the app open closed (the harness's finish closes it)
            self.ft8["open"], self.state["app"] = 0, ""
            self.send("ft8 " + json.dumps(self.ft8))
            self._state()
        elif a[:2] == ["open", "sentry"] and len(a) >= 3:
            self.state["pg"] = a[2]
            self._state()
        elif a[:2] == ["key", "AB"] and a[2:3] == ["1"]:
            if self.state["pg"] in ("A", "G"):
                self.state["rxo"] ^= 1
                self._state()
        elif a[:2] == ["sim", "tone"]:
            plan = json.loads(t.split(" ", 2)[2])
            secs = sum(x[1] for x in plan)
            self.tone = (max(x[0] for x in plan), time.time() + secs)
            self.what = None
            self.tx_until = time.time() + secs
            self.state["tx"] = 1
            self.send("tx 1")
            self._state()
        elif a[0] == "ft8" and len(a) >= 2:
            w = a[1]
            if w == "open":
                self.ft8["open"], self.state["app"] = 1, "FT8"
            elif w == "close":
                self.ft8["open"], self.state["app"] = 0, ""
            elif w == "auto":
                self.ft8["auto"] ^= 1
            elif w in ("cq", "reply"):
                self.silent = self.fault == "silentlink"      # its link stalls as the test asks for the over
                self.reply_to = a[2] if len(a) > 2 else None
                self.what = w
                threading.Timer(0.5, self._key).start()
            elif w == "stop":
                self._unkey()
            self.send("ft8 " + json.dumps(self.ft8))
        elif a[:3] == ["key", "PTT", "0"] or a[:2] == ["tune", "0"]:
            self._unkey()

    def _key(self):
        self.tx_until = time.time() + (3600 if self.fault == "stuck" else TX_S)
        self.state["tx"] = 1
        self.send("tx 1")
        self._state()
        if self.peer and self.peer.fault == "both" and not self.peer.state["tx"]:
            threading.Timer(0.3, self.peer._key).start()     # the listener keys over the sender

    def _unkey(self):
        if self.state["tx"]:
            self.tx_until = 0
            self._end()

    def _end(self):
        self.tone = None
        self.state["tx"] = 0
        self.send("tx 0")
        self._state()
        p = self.peer
        if p:
            p.heard_dbm = None
        if p and self.what == "cq":
            p._row(f"CQ {self.call} FN00", self.call)
        elif p and self.what == "reply":
            p._row(f"{p.call} {self.call} FN00", self.call)

    def _row(self, text, call):
        self.ids += 1
        self.send("ft8r " + json.dumps({"id": self.ids, "t": 0, "x": time.strftime("%H:%M:%S ") + text, "c": call,
                                        "snr": -5, "f": 1500}))

    def _frame(self):
        """A spectrum frame as the GUI sends one (remote_ws: 0xFFFF, bins, centre, width, then int8 dB): noise at
        -120, and the peer's transmission where it is, through the pads, unless this radio is deaf to it"""
        nb, wid, cen = 256, 100000, self.state["freq"]
        v = [-120 + (i * 7) % 3 for i in range(nb)]
        p = self.peer
        deaf = self.fault == "deaf" or (self.fault == "deafafter" and p is not None and p.what is not None)
        if p and p.state["tx"] and not deaf:
            at = p.state["txf"] + (p.tone[0] if p.tone and time.time() < p.tone[1] else 1500)
            i = int((at - (cen - wid / 2)) / (wid / nb))
            if 0 <= i < nb:
                v[i] = -60
        body = b"\xff\xff" + struct.pack("<HIII", nb, cen, wid, 0) + struct.pack(f"<{nb}b", *v)
        c = self.conn
        if c and not self.silent:
            try:
                with self.lock:
                    c.sendall(bytes([0x82, 126]) + struct.pack(">H", len(body)) + body)
            except OSError:
                pass

    def _tick(self):
        while True:
            time.sleep(0.1)
            self._frame()
            if self.state["tx"]:
                if time.time() > self.tx_until:
                    self._end()
                    continue
                w = 12.0 if self.fault == "power" else 0.9 if self.fault == "hot" else self.state["pwr"]
                shown = w
                if self.fault == "spike" and not getattr(self, "spiked", False) and self.tx_until - time.time() < 0.5:
                    self.spiked, shown = True, 18.5  # one bad meter frame as an over ends, the power itself as set
                swr = 3.0 if self.fault == "swr" else 1.1
                self.send(f"t {shown:.1f} {swr:.2f} 0.0")
                if self.peer:   # what the other one hears through the pads (70 dB), or with none (loud)
                    dbm = 10 * __import__("math").log10(max(w, 1e-3) * 1000) - (10 if self.fault == "loud" else 70)
                    self.peer.heard_dbm = dbm
            self.send(f"m {self.heard_dbm if self.heard_dbm is not None else -121.0:.1f}")


def settled(cond, secs=3.0):
    """cond() true within secs: what the simulated radios do on a command comes on their own threads"""
    end = time.time() + secs
    while not cond():
        if time.time() > end:
            return False
        time.sleep(0.02)
    return True


def run_case(fault, sender_fault=True):
    a = SimRadio("simA", "Q1TST", fault if sender_fault else None)
    b = SimRadio("simB", "Q2TST", None if sender_fault else fault)
    a.peer, b.peer = b, a
    state = tempfile.mkdtemp(prefix="bench-sim-")
    DIRS.append(state)
    bench.STATE = state
    bench.CONF = os.path.join(state, "bench.json")
    conf = {"radios": {"simA": {"host": "127.0.0.1", "port": a.port}, "simB": {"host": "127.0.0.1", "port": b.port}},
            "profiles": {"radios": {"between": ["simA", "simB"],
                                    "pads": [{"db": 40, "watts": 10, "at": "simA"}, {"db": 30, "watts": 10, "at": "simB"}]}},
            "limits": {"radio_w": 1.0, "max_key_s": 4}}
    bench.save_json(bench.CONF, conf)
    bench.save_json(os.path.join(state, "tokens.json"), {"simA": {"tx": "a" * 32}, "simB": {"tx": "b" * 32}})
    bench.save_json(os.path.join(state, "chain.json"), {"radios": {"at": time.time(), "words": "sim", "pads": []}})
    calls = {"127.0.0.1": None}
    bench.ssh = lambda host, cmd, timeout=60: ("0 0" if "python3" in cmd else
                                               (calls.setdefault("n", 0), calls.__setitem__("n", calls["n"] + 1),
                                                "Q1TST" if calls["n"] == 1 else "Q2TST")[2])

    class A:
        tx, rx, band = "simA", "simB", "20m"
    try:
        bench.run_radios_ft8(bench.conf(), A)
        why = None
    except bench.Refused as e:
        why = str(e)
    return why, a, b, state


def one_radio(fault):
    """A single simulated radio and the harness's session to it, for the state layer's own cases"""
    r = SimRadio("simC", "Q3TST", fault)
    state = tempfile.mkdtemp(prefix="bench-sim-")
    DIRS.append(state)
    bench.STATE = state
    bench.CONF = os.path.join(state, "bench.json")
    bench.save_json(bench.CONF, {"radios": {"simC": {"host": "127.0.0.1", "port": r.port}},
                                 "limits": {"radio_w": 1.0, "max_key_s": 4}})
    bench.save_json(os.path.join(state, "tokens.json"), {"simC": {"tx": "c" * 32}})
    h = bench.Radio("simC", {"host": "127.0.0.1", "port": r.port}, True)
    h.take_control()
    return r, h


def refused(fn):
    try:
        fn()
        return None
    except bench.Refused as e:
        return str(e)


def refused_why(fn):
    try:
        fn()
    except bench.Refused as e:
        return str(e)
    return None


def main():
    results = []

    def check(ok, what):
        results.append(ok)
        print(("ok    " if ok else "WRONG ") + what, flush=True)

    # ---- the state layer (bench.ensure, snapshot, restore, finish), 2026-11-22: the whole put back (bench.py
    # restore's and LEAVE_TUNING False's); finish's own since 2026-11-23 last ----
    bench.LEAVE_TUNING = False
    sim, h = one_radio(None)
    snap = bench.snapshot(h)
    got = bench.ensure(h, *bench.rx_profile(14200000, 2, pre=0, att=0, rfg=100, agc=0))
    check(got["att"] == 0 and got["pre"] == 0 and got["rfg"] == 100 and got["agc"] == 0 and got["sql"] == 0 and
          got["heard"] == 0 and sim.state["play"] == 0, f"a listener said whole and read back: {got}")
    check(sim.state["pg"] == "B" and any(g == "set heard 0" for g in sim.got),
          "its speaker taken off the SDR (LISTEN 0), the base out of play mode")
    bad = bench.restore(h, snap)
    check(not bad and sim.state["att"] == 1 and sim.state["pre"] == 1 and sim.state["rfg"] == 39 and
          sim.state["sql"] == 4 and sim.state["freq"] == 14074000, f"put back as found ({bad or 'all of it'})")
    bench.finish([h], {"simC": snap})
    check(settled(lambda: sim.state["pg"] == "A"), "finish leaves it on the Sentry's page A")
    for fault, do, word, what in (
            ("stuckatt", lambda h: bench.ensure(h, *bench.rx_profile(14200000, 2, pre=0, att=0)), "att 0 not taken",
             "an attenuator that will not go off"),
            ("play", lambda h: bench.ensure(h, *bench.rx_profile(14200000, 2, pre=0, att=0)), "play mode",
             "a listener whose base stays in play mode"),
            ("split", lambda h: bench.ensure(h, *bench.tx_profile(14200000, 2, 0.1)), "off its dial",
             "a sender that would transmit off its dial"),
            ("oldgui", lambda h: bench.snapshot(h), "does not report", "a GUI older than the checks")):
        sim, h = one_radio(fault)
        why = refused(lambda: do(h))
        check(why is not None and word in why, f"{what}: refused ({why})")
        h.close()
    sim, h = one_radio("norestore")
    snap = bench.snapshot(h)
    bench.ensure(h, {"freq": 14200000})
    bad = bench.restore(h, snap)
    check("freq" in bad, f"a dial that will not go back is said, not passed over ({bad})")
    h.close()
    # ---- a radio NEVER left not as found (the owner, 2026-11-22: "this should NEVER happen") ----
    sim, h = one_radio("flakyrestore")
    snap = bench.snapshot(h)
    bench.ensure(h, {"freq": 14200000})
    rep = {}
    bench.finish([h], {"simC": snap}, report=rep)
    left = bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {})
    check(sim.state["freq"] == 14074000 and not rep.get("not_restored") and not left,
          f"a radio busy at the first try: finish tries again, put back, nothing recorded ({rep or 'clean'})")
    sim, h = one_radio("stall")
    snap = bench.snapshot(h)
    bench.ensure(h, {"freq": 14200000, "att": 0})
    sim.silent = True                       # its link stalls as the test ends
    rep = {}
    bench.finish([h], {"simC": snap}, report=rep)
    check(sim.state["freq"] == 14074000 and sim.state["att"] == 1 and not rep.get("not_restored"),
          f"its link stalled at the end: finish connects again and puts it back ({rep or 'clean'})")
    sim, h = one_radio("norestore")
    snap = bench.snapshot(h)
    bench.ensure(h, {"freq": 14200000})
    rep = {}
    bench.finish([h], {"simC": snap}, report=rep)
    left = bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {})
    check("simC" in left and rep.get("not_restored"), f"a radio that will not go back after {bench.FINISH_TRIES} "
          f"tries: recorded as left not as found ({list(left)})")
    h2 = bench.Radio("simC", {"host": "127.0.0.1", "port": sim.port}, True)
    h2.take_control()
    why = refused(lambda: bench.snapshot(h2))
    check(why is not None and "left not as found" in why, f"and the next test refuses it ({why})")
    h2.close()
    sim.fault = None                        # its trouble gone: bench.py restore puts it back from the snapshot
    bench.cmd_restore(bench.conf(), None)
    left = bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {})
    check(not left and sim.state["freq"] == 14074000 and sim.state["pg"] == "A",
          f"bench.py restore: put back as the test found it, on page A, the record gone ({left or 'clean'})")
    # ---- the operator at the radio keeps control: never taken back, not even to put back (the owner's rule;
    # cover_a found put_back taking it at every test's end, 2026-11-23) ----
    sim, h = one_radio(None)
    snap = bench.snapshot(h)
    bench.ensure(h, {"freq": 14200000})
    sim.operator = True
    sim.send("ctl local 0")                 # a touch at the radio: control is the operator's
    settled(lambda: h.ctl_who == "local" and not h.ctl_mine)
    takes = sum(g == "control take" for g in sim.got)
    rep = {}
    bench.finish([h], {"simC": snap}, report=rep)
    left = bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {})
    check(sum(g == "control take" for g in sim.got) == takes and sim.operator,
          "the operator at the radio has control: finish never takes it back")
    check("simC" in left and rep.get("not_restored") and sim.state["freq"] == 14200000,
          f"and the radio is recorded as not put back, nothing changed under the operator ({list(left)})")
    h2 = bench.Radio("simC", {"host": "127.0.0.1", "port": sim.port}, True)
    why = refused(h2.take_control)
    check(why is not None and "local" in why, f"a test's own take refused too ({why})")
    h2.close()
    sim.operator = False                    # the operator let it go: bench.py restore puts it back
    sim.send("ctl none 0")
    bench.cmd_restore(bench.conf(), None)
    left = bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {})
    check(not left and sim.state["freq"] == 14074000,
          f"control free again: bench.py restore puts it back ({left or 'clean'})")

    why, a, b, _ = run_case(None)
    check(why is None, f"a CQ and its reply between two radios pass ({why or 'passed'})")
    check(any(g.startswith("set pwr 1") for g in a.got) and any(g.startswith("set pwr 1") for g in b.got),
          "both radios set to 0.1 W (the first probe's power) before keying")
    check(any(g == "ft8 auto" for g in a.got), "a radio's FT8 Auto turned off before the test")
    # The harness sends "ft8 close" and hangs up: the simulated radios' threads handle it a moment later, longer on a
    # busy CI runner (2026-11-18: checked before it, on a run of 26 minutes against the usual 17)
    check(settled(lambda: not a.state["tx"] and not b.state["tx"] and a.ft8["open"] == 0 and b.ft8["open"] == 0),
          "both off the air and their FT8 apps closed after")
    # more than asked, under the pad's 10 W: logged in tx_log, the over and the test go on
    why, a, b, state = run_case("hot")
    check(why is None, f"hot: 0.9 W for 0.1 W asked passes ({why or 'passed'})")
    try:
        logged = [json.loads(l) for l in open(os.path.join(state, "tx_log.jsonl"))]
    except OSError:
        logged = []
    check(any(x.get("who") == "simA" and (x.get("over_power_w") or 0) >= 0.85 for x in logged),
          "hot: the over-power logged in tx_log.jsonl")
    why, a, b, _ = run_case("spike")
    try:
        logged = [json.loads(l) for l in open(os.path.join(bench.STATE, "tx_log.jsonl"))]
    except OSError:
        logged = []
    check(why is None and any(x.get("one_frame_over_pad_w") == 18.5 for x in logged),
          f"spike: one TX meter frame over the pad's rating is logged, not stopped ({why or 'passed'})")
    # power: the pad's rating takes two TX meter frames in a row, so the listener hearing far too much (the first
    # frame) may stop it first: either way at once
    for fault, word in (("stuck", "on the air for over"), ("swr", "SWR"), ("power", ("its pad takes", "pads missing")),
                        ("loud", "pads missing")):
        why, a, b, _ = run_case(fault)
        words = word if isinstance(word, tuple) else (word,)
        check(why is not None and any(w in why for w in words), f"{fault}: refused ({why})")
        check(settled(lambda: not a.state["tx"] and any(g == "ft8 stop" for g in a.got)),
              f"{fault}: the sender stopped, off the air")
    # the owner, 2026-11-19 ("won't that damage the radios? don't do that again"): a listener that keys while the
    # sender is on the air stops everything, both off the air
    why, a, b, _ = run_case("both", sender_fault=False)
    check(why is not None and "two radios on the air at once" in why, f"both: refused ({why})")
    check(settled(lambda: not a.state["tx"] and not b.state["tx"]), "both: both off the air")
    # ---- what the listener hears, 2026-11-22: hours of tests on a listener that heard nothing, nothing said ----
    why, a, b, _ = run_case(None)
    check(why is None and a.state["pg"] == "A" and b.state["pg"] == "A", "after a pass both radios on page A")
    check(a.state["att"] == 1 and a.state["rfg"] == 39 and b.state["sql"] == 4,
          "their receivers put back as found (ATT, RF gain, squelch)")
    check(any(g.startswith("sim tone") for g in a.got) and any(g.startswith("sim tone") for g in b.got),
          "a path proof each way before the FT8 overs")
    why, a, b, _ = run_case("deaf", sender_fault=False)
    check(why is not None and "path proof" in why and "does not hear" in why, f"a deaf listener: refused by the "
          f"path proof ({why})")
    check(not any(g.startswith("ft8 cq") for g in a.got), "deaf: nothing of the test sent after the proof failed")
    check(settled(lambda: a.state["pg"] == "A" and b.state["pg"] == "A"), "deaf: both put back on page A all the same")
    why, a, b, _ = run_case("deafafter", sender_fault=False)
    check(why is not None and "did not hear" in why, f"a listener deaf after the proof: the over unheard, refused "
          f"({why})")
    why, a, b, _ = run_case("silentlink")
    check(why is not None and "no word from" in why, f"a radio whose link stalls on the air: stopped ({why})")
    check(settled(lambda: not a.state["tx"]), "silentlink: off the air")
    # a stall of a radio that cannot key unseen goes on to STALE_HELD_S (2026-11-24: the X6200's WiFi held its link 4 s
    # in WeFax's Auto; the over was stopped though it could not have keyed)
    class R:
        def __init__(self, tx, **st):
            self.tx, self.state = tx, st
    lis = R(False, rxo=0, app="WeFax")
    check(bench.stale_limit(lis, lis) == bench.STALE_HELD_S and bench.stale_limit(R(False, rxo=1)) ==
          bench.STALE_HELD_S, "a stalled listener in WeFax, or a radio with its TX locked: said, the over goes on")
    check(bench.stale_limit(R(True, rxo=0, app="WeFax"), None) == bench.STALE_S and
          bench.stale_limit(lis, None) == bench.STALE_S and
          (lambda j: bench.stale_limit(j, j))(R(False, rxo=0, app="JS8")) == bench.STALE_S,
          "a stall stops the over at STALE_S for a radio on the air, a sender in WeFax, or one in an app that keys "
          "by itself")
    # ---- finish leaves the dial where the test left it (the owner, 2026-11-23: "simply leave the radio on the last
    # frequency it was on ... short of simply being on the same screen") ----
    bench.LEAVE_TUNING = True
    sim, h = one_radio(None)
    snap = bench.snapshot(h)
    bench.ensure(h, {"freq": 14200000, "mode": 2, "att": 0, "rfg": 100})
    h.send("set pwr 1")                     # the power still put back (0.1 W left on both radios, 2026-11-18)
    settled(lambda: abs(sim.state["pwr"] - 0.1) < 0.01)
    rep = {}
    bench.finish([h], {"simC": snap}, report=rep)
    left = bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {})
    check(sim.state["freq"] == 14200000 and sim.state["mode"] == 2 and sim.state["att"] == 0 and
          sim.state["rfg"] == 100 and not rep.get("not_restored") and not left,
          f"finish leaves the dial and the receiver as the test left them, nothing recorded ({rep or 'clean'})")
    check(settled(lambda: sim.state["pg"] == "A") and abs(sim.state["pwr"] - 4.4) < 0.01,
          f"and still opens the Sentry's page A and puts the power back ({sim.state['pg']}, {sim.state['pwr']} W)")
    for d in DIRS:
        __import__("shutil").rmtree(d, ignore_errors=True)
    bad = results.count(False)
    # ---- the one sync framework (the owner, 2026-11-24: "EVERY test should use the same unified framework to sync
    # the radios or make exceptions for tests so that there is NO deviation"): bench.apart's declarations, read by the
    # Watch and orch's guard alike; transitions; sync_pair ----
    check(not bench.declared("freq", 14200000, 14198500), "nothing declared: a difference is not passed")
    with bench.apart({"freq": 1500}, "a test's listener 1500 Hz below"):
        ok1 = bench.declared("freq", 14200000, 14198500) and bench.declared("freq", 14198500, 14200000)
        ok2 = not bench.declared("freq", 14200000, 14199000) and not bench.declared("mode", 2, 3)
        seen = bench.declarations()
        w = bench.Watch.__new__(bench.Watch)
        ok3 = w._declared("freq", (14200000, 14198500)) and not w._declared("freq", (14200000, 14199000))
    check(ok1 and ok2 and ok3 and bool(seen) and not bench.declared("freq", 14200000, 14198500),
          "freq 1500 declared: exactly that passes, either way, the Watch reading the same; gone with its block")
    with bench.apart({"mode": [(6, 2), (6, 8)], "split": (1, 0), "filter": bench.STEPPED}, "a test's phases"):
        ok = (bench.declared("mode", 2, 6) and bench.declared("mode", 6, 8) and not bench.declared("mode", 2, 8) and
              bench.declared("split", 0, 1) and not bench.declared("split", 1, 2) and
              bench.declared("flo", 100, 300) and bench.declared("fhi", 2700, 3000) and not bench.declared("agc", 1, 3))
    check(ok, "pairs for each phase (either way), STEPPED for the filter's edges; nothing undeclared passes")
    try:
        bench.apart({}, "nothing")
        empty_refused = False
    except ValueError:
        empty_refused = True
    check(empty_refused, "a declaration of nothing (or without a reason) refused")
    with bench.apart({"freq": 1000}, "image x: a band"):
        bench.undeclare("image x:")
        gone = not bench.declared("freq", 0, 1000)
    check(gone, "undeclare ends a test's own declarations a refusal left")
    bench.dial_settled(force=True)
    c0 = bench.changing()
    with bench.transition():
        c1 = bench.changing()
    c2 = bench.changing()
    bench._dial_moved()
    c3 = bench.changing()
    bench.dial_settled()
    c4 = bench.changing()
    bench.dial_apart()
    bench.dial_settled()
    c5 = bench.changing()
    check(not c0 and c1 and not c2 and c3 and c4 and not c5, "a transition counts while it runs; a dial set on one "
          "radio leaves the other DIAL_FOLLOW_S to follow, closed once the two are alike again after the guard saw "
          "them apart (not while the app asked to open has not tuned yet)")
    # a bench program run as a child: its transition and declaration count in this process while it runs
    code = ("import sys, time; sys.path.insert(0, %r); import bench\n"
            "with bench.apart({'freq': 777}, 'a child test'):\n"
            "    with bench.transition():\n"
            "        print('in', flush=True)\n"
            "        time.sleep(2.5)\n") % os.path.dirname(os.path.abspath(__file__))
    bench.dial_settled(force=True)
    child = subprocess.Popen([sys.executable, "-B", "-c", code], stdout=subprocess.PIPE, text=True)
    child.stdout.readline()
    ch1, dc1 = bench.changing(), bench.declared("freq", 14200000, 14200777)
    child.wait(20)
    ch2, dc2 = bench.changing(), bench.declared("freq", 14200000, 14200777)
    check(ch1 and dc1 and not ch2 and not dc2, "a child bench program's transition and declaration count in its "
          f"parent while it runs, not after ({ch1}, {dc1}; {ch2}, {dc2})")
    sa, ha = one_radio(None)
    sb, hb = one_radio(None)
    bench.dial_settled(force=True)
    ha.send("open 12")
    d1 = bench.changing()
    bench.dial_settled(force=True)
    ha.send("set pwr 1")
    d2 = bench.changing()
    check(d1 and not d2, "an app opened through Radio.send leaves the other radio DIAL_FOLLOW_S to follow; a power "
          "set does not")
    sb.state["freq"], sb.state["mode"] = 7074000, 0
    got = bench.sync_pair([ha, hb], {"freq": 14200000, "mode": 2})
    check(got == {"freq": 14200000, "mode": 2} and sb.state["freq"] == 14200000 and sb.state["mode"] == 2 and
          sa.state["freq"] == 14200000, f"sync_pair: both set at once, read back alike ({got})")
    sb.fault = "stuckatt"
    why = refused_why(lambda: bench.sync_pair([ha, hb], {"att": 0}))
    check(why is not None and "att" in why, f"sync_pair: a radio that will not take it refused ({why})")
    ha.close()
    hb.close()
    print(f"bench sim: {len(results) - bad} of {len(results)} cases as they should be")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
