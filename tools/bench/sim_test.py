#!/usr/bin/env python3
"""bench.py's radios-ft8 against two simulated radios: each a WebSocket server on this computer speaking the radio
remote's protocol as far as the harness uses it (auth, panel, control, set, ft8 open/cq/reply/stop/close, the PTT and
TUNE stops; the state, the meters, tx, ft8 and ft8r back), the pair passing each transmission to the other as a
decoded row and an S-meter. Five cases: a CQ and its reply pass; a radio that stays keyed, a bad SWR, too much power
and a receiver hearing far more than the pads let through each end in the emergency stop and a refusal, with the
stopped radio off the air. Nothing on the radio, nothing on the air.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
TX_S = 2.5          # a simulated FT8 transmission: long enough for the watchdog's 1 s settling
DIRS = []           # the cases' state folders, gone at the end


class SimRadio:
    def __init__(self, name, call, fault=None):
        self.name, self.call, self.fault = name, call, fault
        self.peer, self.conn, self.lock = None, None, threading.Lock()
        self.state = {"freq": 14074000, "mode": 3, "pwr": 4.4, "atu": 0, "tx": 0, "app": ""}
        self.ft8 = {"open": 0, "auto": 1, "cq": 0, "tx": 1, "sending": 0, "nocall": 0}
        self.tx_until, self.heard_dbm, self.ids, self.got = 0.0, None, 0, []
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
        if not c:
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
        elif a[:2] == ["control", "take"]:
            self.send("ctl browser bench-tx 1")
        elif a[0] == "set" and len(a) >= 3:
            v = float(a[2])
            self.state[a[1]] = v / 10 if a[1] == "pwr" else int(v)
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

    def _unkey(self):
        if self.state["tx"]:
            self.tx_until = 0
            self._end()

    def _end(self):
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

    def _tick(self):
        while True:
            time.sleep(0.1)
            if self.state["tx"]:
                if time.time() > self.tx_until:
                    self._end()
                    continue
                w = 5.0 if self.fault == "power" else self.state["pwr"]
                swr = 3.0 if self.fault == "swr" else 1.1
                self.send(f"t {w:.1f} {swr:.2f} 0.0")
                if self.peer:   # what the other one hears through the pads (70 dB), or with none (loud)
                    dbm = 10 * __import__("math").log10(max(w, 1e-3) * 1000) - (10 if self.fault == "loud" else 70)
                    self.peer.heard_dbm = dbm
            self.send(f"m {self.heard_dbm if self.heard_dbm is not None else -121.0:.1f}")


def run_case(fault, sender_fault=True):
    a = SimRadio("simA", "Q1TST", fault if sender_fault else None)
    b = SimRadio("simB", "Q2TST")
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
    return why, a, b


def main():
    results = []

    def check(ok, what):
        results.append(ok)
        print(("ok    " if ok else "WRONG ") + what, flush=True)

    why, a, b = run_case(None)
    check(why is None, f"a CQ and its reply between two radios pass ({why or 'passed'})")
    check(any(g.startswith("set pwr 1") for g in a.got) and any(g.startswith("set pwr 1") for g in b.got),
          "both radios set to 0.1 W (the first probe's power) before keying")
    check(any(g == "ft8 auto" for g in a.got), "a radio's FT8 Auto turned off before the test")
    check(not a.state["tx"] and not b.state["tx"] and a.ft8["open"] == 0 and b.ft8["open"] == 0,
          "both off the air and their FT8 apps closed after")
    for fault, word in (("stuck", "on the air for over"), ("swr", "SWR"), ("power", "W on the air"),
                        ("loud", "pads missing")):
        why, a, b = run_case(fault)
        check(why is not None and word in why, f"{fault}: refused ({why})")
        check(not a.state["tx"] and any(g == "ft8 stop" for g in a.got), f"{fault}: the sender stopped, off the air")
    for d in DIRS:
        __import__("shutil").rmtree(d, ignore_errors=True)
    bad = results.count(False)
    print(f"bench sim: {len(results) - bad} of {len(results)} cases as they should be")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
