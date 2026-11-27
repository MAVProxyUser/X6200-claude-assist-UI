"""The 1:1 suite's "air_c" batch: TCI, the remote, the Sentry, the walkie and the other apps over the air, over the cable between the radios (bench.py chain-ok radios:
80 dB of pads): one radio sends at 0.1 W (1 W at most), the other listens; the path proved first, every over watched,
never both keyed; each radio put back and left on the Sentry's page A. Both directions where both radios can do it.

    suite.py run air_c [--only TEST,...]

The owner, 2026-11-23: "One has to RX the other has to TX!" Each test runs each way, the radios in sync first
(suite_more.sync_both) and as found after: the SENDER with the bench's session that may transmit, the LISTENER with the
receive-only one (while it holds control the radio holds itself receive only: it cannot key at all), the listener set
whole (bench.rx_profile, PRE and ATT said), bench.path_proof before anything is measured, every over under bench.Watch
and the sender locked again and off the air after; a result for each direction naming the features it covers, with the
numbers seen (the listener's spectrum and S-meter over its quiet, the decoder's text, the sender's meter). Before every
over both radios are set alike, the listener first, the sender given its set-up (bench.PAIR_SET), and compared: the
owner's rule of 2026-11-23 ("they should have the same settings"), which path_proof and the Watch enforce too. Only
sentry-lock keeps the dial, mode and filter apart (A's scan moves the listener's: the test's subject), and tells the
Watch so (pair_ignore). Each mode's own filter a test changed on either radio is put back first (finish's before).

The TCI clients that key are programs on the sender itself (a client from 127.0.0.1 is never asked on the screen):
tci_tone_tx.py for a tone (bench.tci_send), and LAB_PY below, written to /tmp/tci_tone_tx_lab.py, for the rest (several
connections, TX frames of each shape, the texts and the RX audio each connection got, timed on the radio's own clock).
Only tci-ask uses a client on this computer (the ones the radio asks about).

The tests (covers; partial: part of a feature):
  tci-mic         a TCI client's trx:0,true; keys the radio with its own microphone (AM on G: its carrier is what the
                  listener hears), trx:0,false; unkeys; tcim 1 (the speaker and TX monitor quiet) only through the over;
                  a program on the radio keys at once, nobody asked [tci-trx-radio-mic, tci-speaker-quiet-during-
                  client-tx; partial tci-ask-not-local]
  tci-audio       a TCI audio over in USB-D: no RX audio to the keying client during it, back after; tx_sensors only
                  on the air, power > 0 and SWR >= 1.0; WSJT-X's double-size frames keep it going, one frame of the
                  wrong size ends it at once; control to TCI for the over and back to the bench's session; a client that
                  leaves unkeys at once, a key still on its way when its client leaves keys nothing [tci-no-rx-audio-
                  during-tx, tci-tx-sensors, tci-tx-frame-size-check, control-returns-after-key, tci-leave-unkeys]
  remote-busy     the bench's session keys PTT (AM on G): 'tx 1' then 'tx 0' to every client (another session, an
                  unpaired viewer); 'control take' from another session refused while on the air; a TCI client's start
                  refused, its trx:0,false; unkeying nothing; then a TCI audio over: a second client's starts refused,
                  control and the over unchanged [panel-tx-broadcast, control-stays-during-tx, tci-one-tx-at-a-time;
                  partial tci-leave-unkeys]
  tci-cw          CW text through TCI into the listener's CW decoder (the remote's 'cw' lines): ^ ~ |BT| > and an
                  unknown character, a second text queued behind the first; cw_macros_stop ends a long text at once;
                  a client holding PTT and another client cannot send text meanwhile, the same client's text after its
                  unkey goes [tci-cw-text-syntax, tci-cw-stop, tci-cw-ptt-holder-no-text, panel-cw-decoder-text]
  tci-level       the TCI transmit level against the sender's own meter and the listener's level: the X6200 at 0.5 W
                  from a full-scale tone over three overs, then 0.3 W; the X6100 at 0.1 W and 1 W, then 1 W from -20
                  and -6 dBFS [tci-tx-level-x6200 (the X6200 sends), tci-tx-level-x6100, tx-tci-transmit-level (the
                  X6100 sends)]
  tci-ask         a client on this computer: asked on the screen, Allow once (MFK, push) keys it while connected,
                  asked again on a new connection (No), Allow once after it hung up lets its next connection key;
                  Ask before TX off (the web's /api/tci) keys at once and closes an open question; a tci.conf without
                  tx_confirm asks; the same through wss://<radio>/ws/tci asked by this computer's address; tci.conf and
                  tci_allowed.conf compared after [tci-ask-allow-once, tci-ask-off, tci-ask-not-local]
  ext-tune        TUNE for an external tuner from the remote ('tune 1 HOLD' renewed): on the air, tune and tune_s in
                  the state, the listener hears the carrier; no renewal: stopped about 3 s later with the error; 'tune
                  0' at once; another page stops it only with control (TUNE at its own power, 1 W at most, else not
                  run) [panel-ext-tune, panel-ext-tune-other]
  ft8-cq          the FT8 app's TX Call from the remote, CQ held off by it through two slots and sent in the next one
                  when it is on again; the timeline in 'ft8 {...}' once a second; the CQ going on after the page that
                  started it closed, seen by a new connection; 'ft8 stop' in an over: off the air within a slot, CQ and
                  TX off, Auto unchanged (test callsigns only, the WSJT-X UDP off for it, the listener decodes only with
                  its PSK Reporter off) [panel-ft8-txcall, panel-ft8-stop, ft8-runs-on-radio; partial
                  ft8-timeline-state]
  sentry-lock     the listener's Sentry A on the sender's carrier: the MFK pans by A's KNOB STEP, its push locks on the
                  carrier; SCANNER (SCAN CFG's style for the test, put back) stops A's scan of 20 m on a carrier while
                  it talks and goes on about 3 s after; SCAN CFG's F2 HOLD keeps it past that, F2 GO ON moves on; C LOG
                  gains each, newest first, the same signal again counted, every entry taken out again
                  [sentry-mfk-pan-push, sentry-a-scan-stops-on-signal, c-log-entries, scancfg-f2-hold-goon; partial
                  scancfg-style-behaviour, scancfg-stay-goon]
  g-moni          an armed PTT over on G (AM): the X6100 raises its TX monitor to 30 or more for it (the voiceprint's
                  TX audio), the X6200 does not; the listener hears the over [partial g-voiceprint-tx-x6100]

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import collections
import json
import math
import os
import re
import socket
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import suite_more  # noqa: E402
from remote_ws import Remote  # noqa: E402
from suite_proto import fresh, http, texts  # noqa: E402
from suite_more import shots_words  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BATCH = "air_c"
F = 14200000                    # 20 m: amateur on both radios, the suite's own dial
F_CW = 14040000
F_FT8 = 14074000
W0 = bench.FIRST_PROBE_W        # 0.1 W: every over but where the feature is a power level
W_TOP = 1.0                     # those: 1 W at most, whatever the configuration allows
HEARD_DB = 15                   # the listener's spectrum (or S-meter) over its quiet: heard
LAB_ON_RADIO = "/tmp/tci_tone_tx_lab.py"     # its name matches bench.stop_bench_programs ('tci_(tone|cw)_tx')
LAB_PID = "/tmp/tci_tone_tx_lab.pid"
TEST_CALL = re.compile(r"^Q[0-9]TST$")


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def sh(host, cmd, timeout=40):
    """A command on a radio over SSH (IPv4: a .local name over IPv6 first took 5 s): its output; Refused on a failure"""
    r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "AddressFamily=inet",
                        f"root@{host}", cmd], capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise bench.Refused(f"{host}: {cmd.split()[0]}: {(r.stderr or r.stdout).strip()[-200:]}")
    return r.stdout


def param(host, name, default=None):
    """One setting of the radio's params.db, read only (no row: default)"""
    out = sh(host, f"sqlite3 /mnt/params.db \"select val from params where name='{name}'\"", 20).strip()
    return out if out else default


def fresh_run(run):
    """The suite's own connections (its banner's) closed: each test works with sessions of its own"""
    run.close()
    run.radios = {}


def model(run, name):
    return run.c["radios"][name].get("model") or name


# ---- The TCI clients on the sender (written to /tmp/tci_tone_tx_lab.py and run there) ----

LAB_PY = r'''#!/usr/bin/env python3
"""TCI clients on the radio itself for the bench's air_c tests (tools/bench/suite_air_c.py writes this file to
/tmp/tci_tone_tx_lab.py on the sender and runs it; its name matches bench.stop_bench_programs). Clients from 127.0.0.1
are not asked about on the screen. A plan of steps over one or more connections, every text each one got and every
RX audio frame's time kept, printed at the end as one line "LAB {json}". Standard library only.

    tci_tone_tx_lab.py PLAN_BASE64 MAX_S [PORT]

Steps: {"open": k} a connection, its greeting read to "ready;"; {"send": k, "text": "trx:0,true;"}; {"audio": k, "hz":
1000, "amp": 0.3, "secs": 3, "shape": "ok"|"double"|"bad"} the next TX_CHRONOs of k answered (ok: TCI's frame;
double: WSJT-X's, twice the size; bad: one frame of a length its header does not say, then TCI's own again);
{"wait": s};
{"until": k, "text": "trx:0,false", "secs": 3} till k gets a text starting so; {"close": k} the socket shut, no close
frame; {"mark": "name"}; {"speed": k, "min": 20, "max": 26} KEY Speed into that range if out of it (put back at the
end). Every keyed connection unkeys at the end; SIGTERM ends it the same way.
"""
import base64, hashlib, json, math, os, select, signal, socket, struct, sys, time
from array import array

PID = "/tmp/tci_tone_tx_lab.pid"
FS, N = 24000, 512
T0 = time.time()
EV = []             # [t, k, kind, data]
CONNS = {}
ATEXIT = []         # (k, text) sent before leaving
DEADLINE = [0.0]


def now():
    return round(time.time() - T0, 3)


def ev(k, kind, data=None):
    EV.append([now(), k, kind, data])


class Gone(Exception):
    pass


def tone(hz, amp, stereo):
    """One second of the tone (a whole number of cycles: it loops without a step), int16, mono or L=R pairs"""
    a, w = amp * 32767, 2 * math.pi * float(hz) / FS
    m = array("h", (int(a * math.sin(w * i)) for i in range(FS)))
    if not stereo:
        return m
    s = array("h", bytes(4 * FS))
    for i in range(FS):
        s[2 * i] = s[2 * i + 1] = m[i]
    return s


class Conn:
    def __init__(self, k, port):
        self.k, self.buf, self.open, self.audio, self.keyed = k, b"", False, None, False
        self.texts, self.rx, self.chrono, self.sent, self.greet = [], [], 0, 0, []
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                   f"User-Agent: GoldCorpHF bench air_c {k}\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            d = s.recv(4096)
            if not d:
                raise Gone("closed during the handshake")
            resp += d
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        if b" 101" not in head.split(b"\r\n")[0] or want not in head:
            raise Gone("not a WebSocket answer")
        s.setblocking(False)
        self.s, self.open = s, True

    def raw(self, op, payload):
        n = len(payload)
        head = bytes([0x80 | op])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        data = head + bytes(4) + payload        # a zero mask (RFC 6455 allows it): no work a byte on a slow Python
        self.s.setblocking(True)
        try:
            self.s.sendall(data)
        finally:
            self.s.setblocking(False)

    def text(self, t):
        self.raw(1, t.encode())
        ev(self.k, "sent", t)
        low = t.lower()
        if low.startswith("trx:0,true") or low.startswith("cw_macros:"):
            self.keyed = True

    def close(self):
        if self.open:
            self.open = False
            try:
                self.s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.s.close()
            ev(self.k, "closed")

    def frame_for_chrono(self):
        a = self.audio
        if not a or time.time() > a["until"]:
            if a:
                ev(self.k, "audio_end", a["shape"])
            self.audio = None
            return
        if a["shape"] == "bad":
            hdr = struct.pack("<8I", 0, FS, 0, 0, 0, N, 2, 1) + bytes(32)
            self.raw(2, hdr + bytes(1000))      # 1000 bytes where 512 int16 samples (1024) or WSJT-X's 2048 belong
            ev(self.k, "bad_frame")
            a["shape"] = "ok"                   # then good frames: a session the bad one did not end goes on
            return
        stereo = a["shape"] == "double"
        tab, pos = a["tab"], a["pos"]
        w = 2 * N if stereo else N
        if pos + w <= len(tab):
            chunk = tab[pos:pos + w]
        else:
            chunk = tab[pos:] + tab[:w - (len(tab) - pos)]
        a["pos"] = (pos + w) % len(tab)
        if a["faded"] < a["fade_n"]:            # the first 50 ms faded in: no click for the transmitter's ALC
            chunk = array("h", chunk)
            for i in range(0, w, 2 if stereo else 1):
                j = a["faded"] + (i // 2 if stereo else i)
                if j >= a["fade_n"]:
                    break
                g = j / a["fade_n"]
                chunk[i] = int(chunk[i] * g)
                if stereo:
                    chunk[i + 1] = int(chunk[i + 1] * g)
            a["faded"] += N
        if stereo:
            # WSJT-X's frame: sized for length pairs, the first half the audio (length / 2 pairs, L = R)
            hdr = struct.pack("<8I", 0, FS, 0, 0, 0, 2 * N, 2, 2) + bytes(32)
            payload = chunk.tobytes() + bytes(4 * N)
        else:
            hdr = struct.pack("<8I", 0, FS, 0, 0, 0, N, 2, 1) + bytes(32)
            payload = chunk.tobytes()
        self.raw(2, hdr + payload)
        self.sent += 1
        if not a["first"]:
            a["first"] = True
            ev(self.k, "audio_first", a["shape"])

    def pump(self):
        try:
            d = self.s.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError as e:
            ev(self.k, "error", str(e))
            self.open = False
            return
        if not d:
            ev(self.k, "eof")
            self.open = False
            return
        self.buf += d
        self.parse()

    def parse(self):
        while len(self.buf) >= 2:
            b0, b1 = self.buf[0], self.buf[1]
            op, n, h = b0 & 0x0F, b1 & 0x7F, 2
            if n == 126:
                if len(self.buf) < 4:
                    return
                n, h = struct.unpack(">H", self.buf[2:4])[0], 4
            elif n == 127:
                if len(self.buf) < 10:
                    return
                n, h = struct.unpack(">Q", self.buf[2:10])[0], 10
            if b1 & 0x80:
                h += 4
            if len(self.buf) < h + n:
                return
            p = self.buf[h:h + n]
            if b1 & 0x80:
                m = self.buf[h - 4:h]
                p = bytes(c ^ m[i % 4] for i, c in enumerate(p))
            self.buf = self.buf[h + n:]
            if op == 9:
                self.raw(10, p)
            elif op == 8:
                ev(self.k, "close_frame")
                self.open = False
                return
            elif op == 1:
                for t in p.decode(errors="replace").split(";"):
                    t = t.strip()
                    if t:
                        self.texts.append((now(), t + ";"))
                        if not t.startswith(("rx_smeter", "rx_channel_sensors")):
                            ev(self.k, "text", t + ";")
            elif op == 2 and len(p) >= 32:
                typ = struct.unpack("<8I", p[:32])[6]
                if typ == 1:
                    self.rx.append(now())
                elif typ == 3:
                    self.chrono += 1
                    self.frame_for_chrono()


def pump(secs):
    end = time.time() + secs
    while True:
        if time.time() > DEADLINE[0]:
            raise Gone("its own deadline")
        live = [c for c in CONNS.values() if c.open]
        left = end - time.time()
        if left <= 0:
            return
        if not live:
            time.sleep(min(left, 0.02))
            continue
        r, _, _ = select.select([c.s for c in live], [], [], min(left, 0.02))
        for c in live:
            if c.s in r:
                c.pump()


def got(k, prefix, since):
    return next((t for at, t in CONNS[k].texts if at >= since and t.lower().startswith(prefix)), None)


def step(st, port):
    if "open" in st:
        k = st["open"]
        t = now()
        CONNS[k] = c = Conn(k, port)
        ev(k, "open")
        c.parse()                               # the greeting may have come with the handshake's answer
        end = time.time() + 6
        while time.time() < end and not got(k, "ready", t):
            pump(0.05)
        c.greet = [x for _, x in c.texts]
        ev(k, "ready" if got(k, "ready", t) else "no_ready")
    elif "send" in st:
        CONNS[st["send"]].text(st["text"])
    elif "audio" in st:
        c = CONNS[st["audio"]]
        shape = st.get("shape", "ok")
        c.audio = {"shape": shape, "until": time.time() + float(st.get("secs", 1)), "pos": 0, "first": False,
                   "faded": 0, "fade_n": int(0.05 * FS),
                   "tab": tone(st.get("hz", 1000), st.get("amp", 0.3), shape == "double")}
        ev(c.k, "audio_set", shape)
    elif "wait" in st:
        pump(float(st["wait"]))
    elif "until" in st:
        k, t, end = st["until"], now(), time.time() + float(st.get("secs", 3))
        hit = None
        while time.time() < end and not hit:
            pump(0.02)
            hit = got(k, st["text"].lower(), t)
        ev(k, "until_hit" if hit else "until_miss", st["text"])
    elif "close" in st:
        CONNS[st["close"]].close()
    elif "mark" in st:
        ev(None, "mark", st["mark"])
    elif "speed" in st:
        k = st["speed"]
        g = next((x for x in CONNS[k].greet if x.startswith("cw_macros_speed:")), None)
        was = int(g.split(":")[1].rstrip(";")) if g and g.split(":")[1].rstrip(";").isdigit() else None
        want = None if was is None else min(max(was, int(st["min"])), int(st["max"]))
        ev(k, "speed", [was, want])
        if was is not None and want != was:
            CONNS[k].text(f"cw_macros_speed:{want};")
            ATEXIT.append((k, f"cw_macros_speed:{was};"))
            t = now()
            end = time.time() + 3
            while time.time() < end and not got(k, f"cw_macros_speed:{want}", t):
                pump(0.05)


def on_term(*_):
    raise Gone("SIGTERM")


def main():
    plan = json.loads(base64.b64decode(sys.argv[1]))
    DEADLINE[0] = time.time() + float(sys.argv[2])
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 50001
    with open(PID, "w") as f:
        f.write(f"{os.getpid()}\n")
    signal.signal(signal.SIGTERM, on_term)
    why = None
    try:
        for st in plan:
            step(st, port)
    except Gone as e:
        why = str(e)
    except (OSError, ValueError, KeyError) as e:
        why = f"{type(e).__name__}: {e}"
    finally:
        DEADLINE[0] = time.time() + 5
        for c in CONNS.values():
            if c.open and c.keyed:
                try:
                    c.text("trx:0,false;")
                except OSError:
                    pass
        for k, t in ATEXIT:
            c = CONNS.get(k)
            try:
                if c is None or not c.open:
                    CONNS[k] = c = Conn(k, port)
                    pump(1.0)
                c.text(t)
                end = time.time() + 2
                while time.time() < end and not got(k, t.split(";")[0].lower(), now() - 0.1):
                    pump(0.05)
                ev(k, "restored", t)
            except (OSError, Gone) as e:
                ev(k, "not_restored", f"{t} {e}")
        try:
            pump(0.4)
        except Gone:
            pass
        for c in CONNS.values():
            if c.open:
                try:
                    c.raw(8, struct.pack(">H", 1000))
                except OSError:
                    pass
                c.close()
        try:
            os.unlink(PID)
        except OSError:
            pass
        out = {"t0": T0, "why": why, "ev": EV,
               "rx": {str(k): [round(x, 2) for x in c.rx] for k, c in CONNS.items()},
               "chrono": {str(k): c.chrono for k, c in CONNS.items()},
               "sent": {str(k): c.sent for k, c in CONNS.items()},
               "greet": {str(k): c.greet[:40] for k, c in CONNS.items()}}
        print("LAB " + json.dumps(out, separators=(",", ":")), flush=True)


if __name__ == "__main__":
    main()
'''

# ---- What a radio said, from its session's lines (each with when it came) ----

def lines(r, t0=0.0, t1=None, prefix=None):
    with r.lock:
        out = list(r.lines)
    return [(t, x) for t, x in out if t >= t0 and (t1 is None or t <= t1) and (prefix is None or x.startswith(prefix))]


def edges(r, t0, t1=None):
    """r's TX state changes since t0, as its remote told every client ('tx 0|1'): [(t, 0|1)]"""
    return [(t, int(x.split()[1])) for t, x in lines(r, t0, t1, "tx ") if x.split()[1:] in (["0"], ["1"])]


def overs(r, t0, t1=None):
    """r's overs since t0: [(on, off)], off None for one still going"""
    out, on = [], None
    for t, v in edges(r, t0, t1):
        if v and on is None:
            on = t
        elif not v and on is not None:
            out.append((on, t))
            on = None
    if on is not None:
        out.append((on, None))
    return out


def edges_settled(r, t0, secs=3.0):
    """r's 'tx 0' line for its last over waited for (the state's tx can say 0 before the remote's 'tx 0' line comes:
    the X6200's came after the over was read, 2026-11-23, and its over had no end): True when the last edge is an off"""
    end = time.time() + secs
    while time.time() < end:
        e = edges(r, t0)
        if not e or e[-1][1] == 0:
            return True
        time.sleep(0.05)
    return False


def meters(r, t0, t1):
    """r's TX meters ('t W SWR ALC') between t0 and t1: [(t, W, SWR, ALC)]"""
    out = []
    for t, x in lines(r, t0, t1, "t "):
        try:
            v = [float(y) for y in x.split()[1:]]
        except ValueError:
            continue
        if len(v) >= 2:
            out.append((t, v[0], v[1], v[2] if len(v) > 2 else None))
    return out


def steady(ms, skip=1.5):
    """The over's power once it settled: the median of the readings skip seconds after its first, and the ALC's"""
    if not ms:
        return None, None
    late = [m for m in ms if m[0] >= ms[0][0] + skip] or ms[-3:]
    alc = [m[3] for m in late if m[3] is not None]
    return round(statistics.median(m[1] for m in late), 3), (round(statistics.median(alc), 2) if alc else None)


def s_meter(r, t0, t1):
    out = []
    for t, x in lines(r, t0, t1, "m "):
        try:
            out.append(float(x.split()[1]))
        except (ValueError, IndexError):
            pass
    return out


def ctl_lines(r, t0, t1=None):
    return [(t, x[4:]) for t, x in lines(r, t0, t1, "ctl ")]


def fmt(v, f="{:.1f}"):
    return "-" if v is None else f.format(v)


def tail(stop, doc=None):
    """A result's end: the watch's stop, the client's own word"""
    return ((f"; STOPPED: {stop}" if stop else "") +
            (f"; the client: {doc.get('why')}" if doc is not None and doc.get("why") else ""))


def ctl_who(r):
    """Who has r's control as its remote last said ("ctl WHO MINE": none, local, tci, browser NAME): its WHO"""
    for _, x in reversed(list(r.lines)):
        if x.startswith("ctl "):
            return x.split()[1] if len(x.split()) > 1 else ""
    return None


def take_if_free(r, where):
    """Control taken by r only when nobody holds it (the test's own other session let it go); held by anyone else
    (the radio's operator above all): Refused, never taken back while a test runs"""
    if r.ctl_mine:
        return
    time.sleep(0.5)
    who = ctl_who(r)
    if who not in ("none", ""):
        raise bench.Refused(f"{r.name}: control with {who} ({where}): the test stops, control not taken back while it "
                            "runs")
    r.take_control()


def until(r, cond, secs):
    """cond() of r's state within secs: True, or False (Radio.wait's, without its refusal)"""
    try:
        return r.wait("it", cond, secs)
    except bench.Refused:
        return False


# ---- One direction: the sender, the listener, the path, the watch, both put back ----

class Pair:
    """SENDER keys (the bench's session that may transmit), LISTENER listens (the receive-only session: while it holds
    control the radio holds itself receive only). Snapshot of both first; end() puts both back (bench.finish: the
    transmitter locked, every setting read back, page A)"""

    def __init__(self, run, s_name, l_name, test=""):
        self.run, self.c, self.test = run, run.c, test
        self.p, self.total = bench.chain_gate(self.c, "radios")
        for n in (s_name, l_name):
            if n not in self.p.get("between", []):
                raise bench.Refused(f"{n} is not on the radios profile's cable")
        self.S = self.L = None
        self.snaps, self.rep, self.extra, self.watch, self.copied, self.stops = {}, {}, [], None, False, []
        self.apart = set()
        self._decl = None
        self.done = []                  # every watch closed: its overs' pictures, named in the result
        self.proof_shots = None         # the path proof's pair of pictures
        self.cleanups = []              # (what, fn) run before finish (the last registered first)
        self.before = {s_name: [], l_name: []}      # each mode's own filter and zoom a test changed, put back first
        self.kept = set()
        self.S = bench.Radio(s_name, self.c["radios"][s_name], True)
        try:
            self.L = bench.Radio(l_name, self.c["radios"][l_name], False)
            for r in (self.S, self.L):
                self.deep(r)
                r.take_control()
                self.snaps[r.name] = bench.snapshot(r)
        except BaseException:
            for r in (self.S, self.L):          # nothing changed yet: the sessions closed, control released
                if r is not None:
                    try:
                        r.close()
                    except OSError:
                        pass
            raise

    @staticmethod
    def deep(r):
        """r's texts and spectrum frames kept for the whole test (the defaults, 600 texts and 300 frames, held under
        a minute of meters: a long test's first over's edges were gone by its end)"""
        r.lines = collections.deque(r.lines, maxlen=60000)
        r.r.spectra = collections.deque(r.r.spectra, maxlen=4000)

    def label(self, what):
        """The name of an over's pictures (bench.SHOTS_DIR): the batch, the test, the direction, the over"""
        return f"air_c {self.test} {self.S.name} to {self.L.name} {what}"

    def later(self, what, fn):
        self.cleanups.append((what, fn))

    def pics(self):
        """The pictures of this direction's overs (both screens while the sender was on the air), named"""
        shots = [x for w in self.done for x in w.shots]
        proof = ""
        if self.proof_shots is not None:
            p, on = self.proof_shots
            proof = (f"the path proof pictured {'on the air' if on else 'NOT on the air'} ("
                     + ", ".join(os.path.basename(x) for x in p.values() if x) + "); ")
        if not shots:
            return proof + "no over pictured"
        on = [x for x in shots if x["on_air"]]
        pair = (on or shots)[0]["paths"]
        return proof + (f"{len(on)} of {len(shots)} overs pictured on the air ("
                        + ", ".join(os.path.basename(p) for p in pair.values() if p) + ")")

    def ctl_check(self, where):
        """Control still the harness's on both, else the test stops here: never taken back while it runs (the owner
        may be at the radio: its keys and knobs give control to the radio's operator)"""
        for r in (self.S, self.L):
            if r is not None and not r.ctl_mine:
                raise bench.Refused(f"{r.name}: control went to {ctl_who(r) or 'someone'} ({where}): the test stops, "
                                    "control not taken back while it runs")

    def expect_dbm(self, w):
        return 10 * math.log10(w * 1000) - self.total

    def banner(self, what):
        line = (f"BENCH AIR_C {what}: {self.S.name.upper()} SENDS, {self.L.name.upper()} LISTENS (RX ONLY) THROUGH "
                f"{self.total} DB")
        for r in (self.S, self.L):
            try:
                r.send(f"banner {line}")
            except OSError:
                pass

    def keep_filter(self, r, f, mode):
        """r's own filter of mode, as found, kept for finish() to put back first (a mode's filter is the mode's own:
        a snapshot of another mode does not hold it); r tuned to f in mode for the reading"""
        if (r.name, mode) in self.kept:
            return
        if r is self.S and r.state.get("rxo") == 0:
            bench.arm(r, False)
        bench.ensure(r, {"freq": f, "mode": mode})
        self.before[r.name].append({"freq": f, "mode": mode, "filter": bench._value(r, "filter"),
                                    "zoom": bench._value(r, "zoom")})
        self.kept.add((r.name, mode))

    def prove(self, f, l_mode=2, l_page="G", agc=0, filt=None):
        """The listener on f (PRE and ATT off, RF gain 100), the path proved at f (bench.path_proof: both alike in
        USB-D on G, the sender's 0.1 W tone 20 dB over the listener's quiet, both screens pictured), then the listener
        on l_page in l_mode: the proof's words"""
        self.supply = bench.listener_gate(self.L)
        bench.open_page_both([self.S, self.L], "G")      # G tunes each to its own channel: set alike again
        # both on the proof's dial at once, then each one's receiver (a radio at a time: 2026-11-24, JS8's listener
        # took 9 s over its PRE, ATT, AGC and filter while the other sat on the old dial)
        bench.sync_pair([self.S, self.L], {"freq": f, "mode": 3})
        self.keep_filter(self.L, f, 3)          # the listener's own USB-D filter and zoom as found, for finish
        bench.ensure(self.L, *bench.rx_profile(f, 3, pre=0, att=0, rfg=100, agc=agc, filt=filt))
        self.keep_filter(self.S, f, 3)          # the proof gives the sender the listener's USB-D filter and zoom
        res = bench.path_proof(self.c, self.total, self.S, self.L, f, self.rep,
                               label=self.label(f"path proof {self.S.name} to {self.L.name}"))
        self.proof_shots = (res.get("shots") or {}, bool(res.get("shots_on_air")))
        say(f"path proof {self.S.name} to {self.L.name}: {res['rise_db']} dB, both screens "
            f"{'on the air' if res.get('shots_on_air') else 'NOT caught on the air'}: "
            + ", ".join(os.path.basename(x) for x in (res.get("shots") or {}).values() if x))
        bench.open_page(self.L, l_page)
        if l_mode != 3:
            bench.sync_pair([self.S, self.L], {"freq": f, "mode": l_mode})     # the sender with it, not after the
            self.keep_filter(self.L, f, l_mode)                                 # test's own set-up (sender())
        bench.ensure(self.L, *bench.rx_profile(f, l_mode, pre=0, att=0, rfg=100, agc=agc, filt=filt))
        return f"path {res['rise_db']} dB ({self.S.name}'s meter {res['sender_peak_w']} W)"

    def ignored(self):
        """The receiver settings the watch does not compare: those the test keeps apart on purpose"""
        out = set(self.apart) - {"filter"}
        return out | ({"flo", "fhi"} if "filter" in self.apart else set())

    def set_apart(self, keys, why=None):
        """What this test keeps apart on purpose, declared in the one registry (bench.apart, the owner, 2026-11-24:
        "NO deviation"): each key as the test's own (STEPPED), with why; () ends the declaration"""
        old = getattr(self, "_decl", None)
        if old is not None:
            old.__exit__(None, None, None)
            self._decl = None
        self.apart = set(keys)
        if keys:
            spec = {("filter" if k in ("flo", "fhi") else k): bench.STEPPED for k in keys}
            self._decl = bench.apart(spec, why or f"air_c {self.test}: {', '.join(sorted(keys))} kept apart on "
                                                  f"purpose")
            self._decl.__enter__()

    def sender(self, f, mode, w, apart=(), why=None):
        """Both alike (the owner, 2026-11-23: "they should have the same settings"): the listener first on f in mode,
        then the sender given the listener's set-up (bench.PAIR_SET) on f in mode at w, armed on G (locked first: an
        armed radio holds its dial; arming puts CW and the data modes into the voice mode: set again), the two
        compared, a difference refused. apart: what the test keeps apart on purpose (A's scan moves the listener's
        dial: freq, mode, filter), not set on the listener nor compared, by the watch neither"""
        S, L = self.S, self.L
        self.set_apart(apart, why)
        self.ctl_check("before the sender's set-up")
        if L.tx or S.tx:
            raise bench.Refused(f"{(L if L.tx else S).name} on the air before the over was set up")
        bench.open_page(S, "G")
        if S.state.get("rxo") == 0:
            bench.arm(S, False)
        if "freq" not in self.apart:
            if "mode" not in self.apart and int(L.state.get("mode", -1)) != mode:
                self.keep_filter(L, f, mode)        # the listener's own memory of the mode, as found, for finish
            bench.ensure(L, {"freq": f, "mode": mode})      # the listener there first, read back
        self.keep_filter(S, f, mode)
        want = {k: bench._value(L, k) for k in bench.PAIR_SET if k not in self.apart and k not in ("freq", "mode")}
        bench.ensure(S, dict({k: v for k, v in want.items() if v is not None}, freq=f, mode=mode))
        bench.ensure(S, *bench.tx_profile(f, mode, w))
        bench.radio_tx_gate(S, self.c, w)
        bench.arm(S, True, mode=mode)
        time.sleep(0.5)
        diff = {k: v for k, v in bench.pair_diff(S, L).items() if k not in self.ignored()}
        if diff:
            raise bench.Refused(f"{S.name} and {L.name} not set alike before the over: " +
                                ", ".join(f"{k} {x}/{y}" for k, (x, y) in diff.items()))

    retune = sender

    def apps_alike(self, what):
        """S's app open (L on its dial in its own app, or on G): S given L's set-up where they differ (bench.pair_align:
        the apps hold the dial and the mode, so those must already match), its own memory of the mode kept for
        finish; compared (suite_air_b's apps_alike). An app that will not take it: Refused, naming what differs"""
        S, L = self.S, self.L
        self.set_apart(())
        self.keep_filter(S, int(S.state.get("freq", 0)), int(S.state.get("mode", -1)))
        d = bench.pair_diff(S, L)
        if d:
            say(f"{what}: {S.name} takes {L.name}'s set-up: " + ", ".join(f"{k} {x}/{y}" for k, (x, y) in d.items()))
            bench.pair_align(S, L)
        d = bench.pair_diff(S, L)
        if d:
            raise bench.Refused(f"{what}: {S.name} and {L.name} not set alike: " +
                                ", ".join(f"{k} {x}/{y}" for k, (x, y) in d.items()))

    def listener_as_sender(self):
        """The listener given the sender's set-up (an app holds the sender's: the FT8 app's dial, mode and filter),
        compared: a difference refused"""
        S, L = self.S, self.L
        self.set_apart(())
        f, mode = int(S.state.get("freq", 0)), int(S.state.get("mode", -1))
        self.keep_filter(L, f, mode)
        bench.ensure(L, {k: bench._value(S, k) for k in bench.PAIR_SET if bench._value(S, k) is not None})
        time.sleep(0.5)
        diff = bench.pair_diff(S, L)
        if diff:
            raise bench.Refused(f"{L.name} not set as {S.name}: " + ", ".join(f"{k} {x}/{y}" for k, (x, y) in
                                                                               diff.items()))

    def watch_on(self, w, what="over"):
        """A bench.Watch over both for the over `what` at w (its pictures labelled 'air_c TEST S to L WHAT'); the
        listener must be where the sender sends and not on the air (the owner: "One has to RX the other has to TX!")"""
        self.watch_off()
        if self.L.tx:
            raise bench.Refused(f"{self.L.name} (the listener) on the air before {self.S.name}'s over")
        if "freq" not in self.apart:
            d = {k: v for k, v in bench.pair_diff(self.S, self.L).items() if k not in self.ignored()}
            if d:
                raise bench.Refused(f"{what}: {self.S.name} and {self.L.name} not set alike before the over: " +
                                    ", ".join(f"{k} {x}/{y}" for k, (x, y) in d.items()))
        self.watch = bench.Watch([self.S, self.L], self.c, expect_dbm=self.expect_dbm(w), listener=self.L, w=w,
                                 label=self.label(what), pair_ignore=self.ignored())
        return self.watch

    def watch_off(self):
        """The watch ended (its pictures fetched, kept for the result); why it stopped anything, or None"""
        w = self.watch
        if w is None:
            return None
        w.close()
        self.watch = None
        self.done.append(w)
        if w.why:
            self.stops.append(w.why)
        if "two radios on the air" in (w.why or ""):
            say(f"INCIDENT: {w.why}")
        return w.why

    def off_air(self, secs=5.0):
        try:
            self.S.wait("off the air", lambda: not self.S.tx, secs)
        except bench.Refused:
            pass
        edges_settled(self.S, time.time() - 60, 2.0)

    # the TCI clients on the sender itself (LAB_PY)
    def lab_start(self, plan, max_s):
        """LAB_PY run on the sender with plan, beside the caller: a Lab (join() for its record)"""
        if not self.copied:
            path = os.path.join(self.run.dir, "tci_tone_tx_lab.py")
            with open(path, "w") as f:
                f.write(LAB_PY)
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", "-o", "AddressFamily=inet", path,
                            f"root@{self.S.host}:{LAB_ON_RADIO}"], check=True, timeout=30)
            self.copied = True
        return Lab(self, plan, max_s)

    def lab(self, plan, max_s):
        return self.lab_start(plan, max_s).join()

    def end(self):
        """Every connection of the test's closed, the bench's programs on the sender ended, both radios put back as
        found and read back, on page A: what did not come back, or None"""
        try:
            self.watch_off()
        except Exception:      # noqa: BLE001 - the end puts the radios back whatever went wrong before it
            pass
        self.set_apart(())     # its declaration ended (bench.apart): the put-back makes the two alike again
        for x in self.extra:
            try:
                x.close()
            except OSError:
                pass
        if self.S is not None:
            try:
                sh(self.S.host, f"kill $(cat {LAB_PID} 2>/dev/null) 2>/dev/null; true", 20)
            except (bench.Refused, subprocess.SubprocessError):
                pass
            # the sender stopped, checked off the air and locked again before anything is put back (ifshift.py's end)
            try:
                self.S.stop()
                self.off_air(5)
                if self.S.state.get("rxo") == 0 and self.S.ctl_mine:
                    bench.arm(self.S, False)
            except (bench.Refused, OSError):
                pass                            # finish() tries again, three times, and records what stays
        radios = [r for r in (self.S, self.L) if r is not None]
        if self.cleanups:
            for r in radios:
                # the test is over: a stalled link renewed and control taken for the cleanups (finish does the same)
                try:
                    if r.stale():
                        r.reconnect()
                    if not r.ctl_mine:
                        say(f"{r.name}: control with {ctl_who(r) or 'someone'} at the test's end: taken for the "
                            "cleanups")
                        r.take_control()
                except (bench.Refused, OSError) as e:
                    say(f"{r.name}: not ready for the cleanups ({e})")
        with bench.transition():        # the put-back, the cleanups and finish (which ends the pair alike), one
            for what, fn in reversed(self.cleanups):
                try:
                    fn()
                except Exception as e:  # noqa: BLE001 - every cleanup tried, whatever one of them met
                    say(f"{what}: not done ({type(e).__name__} {e})")
                    self.rep.setdefault("unclean", []).append(f"{what}: {e}")
            self.cleanups = []
            if radios:
                bench.finish(radios, {r.name: self.snaps[r.name] for r in radios if r.name in self.snaps},
                             report=self.rep, before=self.before)
        bad = dict(self.rep.get("not_restored") or {})
        if self.rep.get("unclean"):
            bad["cleanups"] = self.rep["unclean"]
        return bad or None


class Lab:
    """LAB_PY on the sender, its ssh beside the caller; the watch's stop kills it (its leaving unkeys)"""

    def __init__(self, pair, plan, max_s):
        self.pair, self.max_s, self.doc = pair, max_s, None
        b64 = base64.b64encode(json.dumps(plan).encode()).decode()
        self.t0 = time.time()
        self.proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", "-o", "AddressFamily=inet",
                                      f"root@{pair.S.host}", f"python3 {LAB_ON_RADIO} {b64} {max_s:.1f}"],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.th = threading.Thread(target=self._run, daemon=True)
        self.th.start()

    def _run(self):
        killed = False
        while self.proc.poll() is None:
            w = self.pair.watch
            if not killed and ((w is not None and w.why) or time.time() - self.t0 > self.max_s + 25):
                subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "AddressFamily=inet",
                                f"root@{self.pair.S.host}", f"kill $(cat {LAB_PID}) 2>/dev/null"],
                               capture_output=True, timeout=20)
                killed = True
            time.sleep(0.05)
        try:
            out = self.proc.communicate(timeout=30)[0]
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out = ""
        line = next((x for x in out.splitlines() if x.startswith("LAB ")), None)
        try:
            self.doc = json.loads(line[4:]) if line else {"why": f"no record: {out.strip()[-300:]}", "ev": []}
        except ValueError:
            self.doc = {"why": f"its record unreadable: {line[:200]}", "ev": []}
        self.doc["mac_t0"], self.doc["mac_t1"] = self.t0, time.time()

    def alive(self):
        return self.th.is_alive()

    def join(self):
        self.th.join(self.max_s + 90)
        return self.doc or {"why": "it did not end", "ev": []}


# ---- A lab's record (times on the radio's own clock, from the lab's start) ----

def evs(doc, k=None, kind=None):
    return [e for e in doc.get("ev", []) if (k is None or e[1] == k) and (kind is None or e[2] == kind)]


def ev_at(doc, k, kind, data=None, after=0.0):
    """The time of the first event of k of that kind (and data, a prefix for a text) at or after after, or None"""
    for t, kk, kd, d in doc.get("ev", []):
        if t >= after and kk == k and kd == kind and (data is None or (isinstance(d, str) and
                                                                        d.lower().startswith(data.lower()))):
            return t
    return None


def texts_of(doc, k, t0=0.0, t1=1e9, prefix=None):
    return [(t, d) for t, kk, kd, d in doc.get("ev", []) if kk == k and kd == "text" and t0 <= t <= t1 and
            (prefix is None or d.lower().startswith(prefix))]


def rx_in(doc, k, t0, t1):
    return sum(1 for t in doc.get("rx", {}).get(str(k), []) if t0 <= t <= t1)


# ---- The listener's ear ----

def heard(P, hz, quiet, loud, half=300):
    """The listener on a window of an over: its spectrum's strongest bin within half of hz and its S-meter, the loud
    window against the quiet one. (rise dB, the better of the two, or None; words). The X6200's S-meter does not follow
    every signal its spectrum shows (an AM carrier 40 dB up, 2026-11-19), the X6100's spectrum in AM has its own spike
    at the dial: either one rising is the receiver's"""
    q, nq = bench.spectrum_level(P.L, hz, quiet[0], quiet[1], half=half)
    lo, nl = bench.spectrum_level(P.L, hz, loud[0], loud[1], half=half)
    sr = None if q is None or lo is None else round(lo - q, 1)
    mq, ml = s_meter(P.L, *quiet), s_meter(P.L, *loud)
    mr = round(max(ml) - statistics.median(mq), 1) if mq and ml else None
    best = max([x for x in (sr, mr) if x is not None], default=None)
    m0, m1 = (statistics.median(mq) if mq else None), (max(ml) if ml else None)
    return best, (f"spectrum {fmt(q, '{:.0f}')}->{fmt(lo, '{:.0f}')} ({fmt(sr, '{:+.1f}')} dB, {nq}/{nl} frames), "
                  f"S-meter {fmt(m0, '{:.0f}')}->{fmt(m1, '{:.0f}')} dBm ({fmt(mr, '{:+.1f}')})")


def heard_over(P, hz, on, off, half=300, pre=2.5):
    """An over [on, off] (the sender's own TX edges) heard: its middle against the listener's quiet before it"""
    return heard(P, hz, (on - pre, on - 0.3), (on + 1.0, (off or time.time()) - 0.3), half)


def ptt_over(P, secs, hold, during=None, wait_on=8.0):
    """The sender keyed with its remote's PTT lease, renewed every quarter second, secs on the air (WALKIE's way):
    during(seconds on the air) called at each renewal. (on, off) by the remote's own TX edges (Mac clock)"""
    S, t0, on = P.S, time.time(), None
    try:
        while not (P.watch and P.watch.why) and (on is None and time.time() - t0 < wait_on or
                                                 on is not None and time.time() - on < secs):
            if P.L.tx:                          # the other radio on the air: never over it
                break
            S.send(f"key PTT 1 {hold}")
            if on is None and S.tx:
                on = time.time()
            if during and on is not None:
                during(time.time() - on)
            time.sleep(0.25)
    finally:
        S.send("key PTT 0")
    P.off_air(5)
    edges_settled(S, t0)
    ov = overs(S, t0)
    return (ov[-1][0], ov[-1][1]) if ov else (None, None)


class Bg:
    """fn(*a, **k) beside the caller: join() gives what it returned (an exception it raised is raised there)"""

    def __init__(self, fn, *a, **k):
        self.out, self.err = None, None

        def go():
            try:
                self.out = fn(*a, **k)
            except Exception as e:      # noqa: BLE001 - raised again in join()
                self.err = e
        self.th = threading.Thread(target=go, daemon=True)
        self.th.start()

    def alive(self):
        return self.th.is_alive()

    def join(self, timeout=None):
        self.th.join(timeout)
        if self.err:
            raise self.err
        return self.out


def tone_over(P, hz, secs, w, amp=0.3):
    """The sender's TCI tone (tci_tone_tx.py on it) secs long, under a watch at w, beside the caller: a Bg whose
    join() gives (on, off) by the remote's TX edges and the tone's own output"""
    def go():
        t0 = time.time()
        out, a, b = bench.tci_send(P.S, [[0, 0.3], [hz, secs]], amp=amp, max_s=secs + 4, watch=P.watch)
        P.off_air(5)
        edges_settled(P.S, t0)
        ov = overs(P.S, t0)
        return (ov[-1] if ov else (None, None)), out
    bench.tx_log({"who": P.S.name, "f": int(P.S.state.get("txf") or 0), "w": P.S.pwr(), "what": f"air_c tone {hz} Hz"})
    return Bg(go)


# ---- Each direction, the radios in sync first and as found after ----

def mfk_busy():
    """AIR_C_MFK_BUSY="x6200:why": the radios whose MFK a test must not use now, and why (a box over the screen
    that takes the MFK, 2026-11-23: the X6200's update prompt, "Apply GoldCorpHF 0.3.12.47?", its press answers it:
    the owner's to answer, never the harness's)"""
    out = {}
    for x in os.environ.get("AIR_C_MFK_BUSY", "").split(";"):
        if ":" in x:
            n, why = x.split(":", 1)
            out[n.strip()] = why.strip()
    return out


def directions(run, test, body, covers, partial=(), senders=None, mfk=None):
    """body(Pair) -> (ok, says) or (ok, says, covers, partial) for each direction (senders: only these send); a result
    each, and the pair 1:1. Both radios in sync before (the owner's rule: suite_more.sync_both) and as found after.
    mfk(sender, listener): the radio whose MFK the test turns and presses; a direction whose MFK radio is busy
    (mfk_busy) is not run, said"""
    fresh_run(run)
    a, b = run.names[0], run.names[1]
    dirs = [(s, l_) for s, l_ in ((a, b), (b, a)) if senders is None or s in senders]
    # AIR_B_WAYS="x6100>x6200": only those directions, as suite_air_b's two_way (orch: the half recorded as such)
    only = [tuple(x.split(">", 1)) for x in os.environ.get("AIR_B_WAYS", "").split(",") if ">" in x]
    if only:
        dirs = [d for d in dirs if d in only]
    busy = mfk_busy()
    if mfk is not None:
        for s, l_ in list(dirs):
            if mfk(s, l_) in busy:
                dirs.remove((s, l_))
                run.result(BATCH, f"{test} {s}->{l_}", None, f"NOT RUN: the test turns and presses {mfk(s, l_)}'s "
                                                             f"MFK, and {busy[mfk(s, l_)]}")
    # AIR_C_WAYS="x6100>x6200": only those directions (a round again on the direction not yet right)
    only_ways = [tuple(x.split(">", 1)) for x in os.environ.get("AIR_C_WAYS", "").split(",") if ">" in x]
    if only_ways:
        dirs = [d for d in dirs if d in only_ways]
    snaps, seen, why = suite_more.sync_both(run)
    res = {}
    try:
        for s, l_ in dirs:
            cov, par = list(covers(s) if callable(covers) else covers), list(partial)
            if why:
                ok, says = False, f"refused: the radios are not in sync before the test ({why})"
            else:
                out = one(run, test, s, l_, body)
                ok, says = out[0], out[1]
                if len(out) > 2:
                    cov, par = list(out[2]), list(out[3])
            ok = ok if ok is None else bool(ok)
            res[(s, l_)] = ok
            run.result(BATCH, f"{test} {s}->{l_}", ok, says, covers=cov, partial=par)
    finally:
        suite_more.unsync_both(run, snaps)
    if len(res) == 2:
        vals = list(res.values())
        pair = False if False in vals else None if None in vals else True
        run.result(BATCH, f"{test} 1:1", pair, ", ".join(f"{s}->{l_} {'pass' if v else 'look' if v is None else 'FAIL'}"
                                                         for (s, l_), v in res.items()))


def one(run, test, s, l_, body):
    P, out = None, None
    try:
        P = Pair(run, s, l_, test)
        out = list(body(P))
    except bench.Refused as e:
        errs = [f"{r.name}: {x}" for r in ((P.S, P.L) if P else ()) if r is not None for x in r.errs[-2:]]
        out = [False, f"refused: {e}" + (f" (the radios said: {errs})" if errs else "")]
    except Exception as e:      # noqa: BLE001 - the test's own error is its failure, said; the radios still put back
        import traceback
        out = [False, f"{type(e).__name__}: {e} ({traceback.format_exc().strip().splitlines()[-3:]})"]
    finally:
        if P is not None:
            try:
                bad = P.end()
            except (bench.Refused, OSError) as e:
                bad = {"error": str(e)}
            if bad:
                out = out or [False, ""]
                out[0] = False
                out[1] = f"{out[1]} (NOT put back: {bad})"
    if P is not None and P.stops and out:
        out[0] = False                      # a watch that stopped anything fails the test, said or not
        if "STOPPED" not in out[1]:
            out[1] += f"; STOPPED: {P.stops[0]}"
    if P is not None and out:
        deaf = [d for w in P.done for d in w.deaf]
        if deaf:                            # an over the listener did not hear: nothing measured of it counts
            out[0] = False if out[0] else out[0]
            out[1] += "; the Watch: " + ", ".join(f"{d['listener']} did NOT hear {d['sender']}'s over at {d['hz']} "
                                                  f"Hz ({d['best_rise_db']} dB)" for d in deaf)
        hd = [x for w in P.done for x in w.heard_db.get(P.S.name, [])]
        peak = [w.tx_peak.get(P.S.name) for w in P.done if w.tx_peak.get(P.S.name) is not None]
        over_p = [w.over_power.get(P.S.name) for w in P.done if w.over_power.get(P.S.name)]
        out[1] += (f"; the Watch: {P.S.name}'s overs heard {hd} dB over the quiet, "
                   f"its meter's highest {max(peak) if peak else None} W"
                   + (f" (OVER the power asked: {max(over_p)} W, logged)" if over_p else "")
                   + f"; pictures: {P.pics()}")
    return out


# ======== tci-mic: a TCI client's trx:0,true; keys the radio with its own microphone ========

def b_tci_mic(P):
    S = P.S
    proof = P.prove(F, l_mode=6)
    P.sender(F, 6, W0)                      # AM: the carrier is what the listener hears of a microphone over
    keys = ("spk", "moni", "muted", "tcim")
    before = {k: S.state.get(k) for k in keys}
    P.banner("TCI trx:0,true; (THE RADIO'S OWN MICROPHONE, AM)")
    P.watch_on(W0, "TCI trx with the mic AM")
    bench.tx_log({"who": S.name, "f": F, "w": S.pwr(), "what": "air_c tci-mic"})
    t0 = time.time()
    lab = P.lab_start([{"open": 0}, {"wait": 1.5}, {"send": 0, "text": "trx:0,true;"},
                       {"until": 0, "text": "trx:0,true", "secs": 3}, {"wait": 4.0},
                       {"send": 0, "text": "trx:0,false;"}, {"until": 0, "text": "trx:0,false", "secs": 3},
                       {"wait": 1.5}], 25)
    seen, asks = [], set()
    while lab.alive():
        asks.add(S.state.get("ask"))
        seen.append((time.time(), S.state.get("tcim")))
        time.sleep(0.2)
    doc = lab.join()
    stop = P.watch_off()
    time.sleep(1.5)
    after = {k: S.state.get(k) for k in keys}
    ov = overs(S, t0)
    # tcim inside the over by its own TX edges (its ends are the state's: the state's tcim and tx do not change in
    # the same message as it unkeys, round 1 2026-11-23: one 0 at its very end)
    during = [v for t, v in seen if ov and ov[0][1] and ov[0][0] + 0.6 <= t <= ov[0][1] - 0.5]
    ends = ([v for t, v in seen if ov[0][0] <= t < ov[0][0] + 0.6], [v for t, v in seen if ov[0][1] - 0.5 < t <=
                                                                    ov[0][1] + 0.3]) if ov and ov[0][1] else None
    keyed = ev_at(doc, 0, "until_hit", "trx:0,true") is not None
    unkeyed = ev_at(doc, 0, "until_hit", "trx:0,false") is not None
    one_over = len(ov) == 1 and ov[0][1] is not None
    dur = (ov[0][1] - ov[0][0]) if one_over else None
    rise, words = heard_over(P, F, *ov[0]) if ov else (None, "no over")
    ok = (not stop and keyed and unkeyed and one_over and 3.0 <= dur <= 7.5 and bool(during) and
          set(during) == {1} and asks <= {0} and after == before and rise is not None and rise >= HEARD_DB)
    return ok, (f"{proof}; a program on {S.name} (127.0.0.1) sent trx:0,true;: answered trx:0,true {keyed}, "
                f"{len(ov)} over(s) by its remote ({fmt(dur)} s), trx:0,false; ended it {unkeyed}; asked on the "
                f"screen: {'never' if asks <= {0} else 'YES'}; tcim during the over {sorted(set(during), key=str)} (1: "
                f"the speaker and TX monitor quiet; its first 0.6 s and last 0.5 s read {ends}), after {after} (before "
                f"{before}); {P.L.name} on the carrier: "
                f"{words}" + tail(stop, doc))


# ======== tci-audio: an audio over: RX audio, sensors, frame sizes, control, leaving ========

FMT24 = ("audio_samplerate:24000;audio_stream_sample_type:int16;audio_stream_channels:1;audio_stream_samples:512;")


def sensors_of(doc, k, t0, t1):
    out = []
    for t, x in texts_of(doc, k, t0, t1, "tx_sensors:"):
        try:
            v = [float(y) for y in x.split(":", 1)[1].rstrip(";").split(",")]
            out.append((t, v[2], v[4]))         # 0, mic, power, power, SWR, ALC
        except (ValueError, IndexError):
            pass
    return out


def b_tci_audio(P):
    S = P.S
    proof = P.prove(F, l_mode=3)
    P.sender(F, 3, W0)                      # both USB-D on F, the tone 1000 Hz up
    P.banner("TCI AUDIO OVER: RX AUDIO, SENSORS, FRAME SIZES, CONTROL, LEAVING")
    P.watch_on(W0, "TCI audio overs")
    bench.tx_log({"who": S.name, "f": F + 1000, "w": S.pwr(), "what": "air_c tci-audio"})
    t0 = time.time()
    tone = {"hz": 1000, "amp": 0.3, "secs": 12}
    doc = P.lab([{"open": 0}, {"send": 0, "text": FMT24}, {"send": 0, "text": "audio_start:0;"},
                 {"send": 0, "text": "tx_sensors_enable:true;"}, {"open": 1}, {"wait": 1.5}, {"mark": "over1"},
                 {"send": 0, "text": "trx:0,true,tci;"}, dict(tone, audio=0, shape="ok"),
                 {"until": 0, "text": "trx:0,true", "secs": 3}, {"wait": 2.5}, {"mark": "double"},
                 dict(tone, audio=0, shape="double"), {"wait": 2.5}, {"mark": "bad"},
                 dict(tone, audio=0, shape="bad", secs=6), {"until": 0, "text": "trx:0,false", "secs": 2},
                 {"wait": 3.5}, {"mark": "over2"},
                 {"send": 0, "text": "trx:0,true,tci;"}, dict(tone, audio=0, shape="ok"),
                 {"until": 0, "text": "trx:0,true", "secs": 3}, {"wait": 2.0}, {"close": 0},
                 {"until": 1, "text": "trx:0,false", "secs": 3}, {"wait": 2.5}, {"mark": "c2"},
                 {"open": 2}, {"send": 2, "text": "trx:0,true,tci;"}, {"close": 2}, {"wait": 3.0}], 45)
    stop = P.watch_off()
    P.off_air(5)
    time.sleep(1.0)
    m = {x[3]: x[0] for x in evs(doc, None, "mark")}
    if not all(k in m for k in ("over1", "double", "bad", "over2", "c2")):
        return False, f"{proof}; the client did not run through ({doc.get('why')})"
    on1 = next((t for t, x in texts_of(doc, 0, m["over1"], m["double"], "trx:0,true")), None)
    bad = ev_at(doc, 0, "bad_frame", after=m["bad"])
    off0 = next((t for t, x in texts_of(doc, 0, bad or 1e9, prefix="trx:0,false")), None)
    off1 = next((t for t, x in texts_of(doc, 1, bad or 1e9, prefix="trx:0,false")), None)
    early = [t for k in (0, 1) for t, x in texts_of(doc, k, (on1 or 0) + 0.05, bad or 0, "trx:0,false")]
    dbl = ev_at(doc, 0, "audio_first", "double", after=m["double"]) is not None
    # ended by the frame itself: good frames followed it, and the 500 ms silence watchdog had nothing to end
    frames_ok = (on1 is not None and bad is not None and dbl and not early and off0 is not None and
                 off0 - bad <= 1.0 and off1 is not None and off1 - bad <= 1.0)
    # RX audio to the keying client: on before, none on the air, back after
    rx_b = rx_in(doc, 0, m["over1"] - 1.2, m["over1"])
    rx_d = rx_in(doc, 0, (on1 or 0) + 0.3, bad or 0)
    rx_a = rx_in(doc, 0, (off1 or 1e9) + 0.5, m["over2"])
    no_rx = on1 is not None and rx_b >= 20 and rx_d <= 2 and rx_a >= 20
    # tx_sensors: on the air only, the power read, the SWR 1.0 or more
    en = bool(texts_of(doc, 0, 0, 1e9, "tx_sensors_enable:true"))
    sen = sensors_of(doc, 0, on1 or 0, (off0 or 0) + 0.3)
    out_ = sensors_of(doc, 0, m["over1"] - 1.2, (on1 or 0) - 0.05) + sensors_of(doc, 0, (off1 or 1e9) + 0.5,
                                                                                     m["over2"])
    pos = [p for _, p, _ in sen if p >= 0.03]
    sens_ok = en and len(sen) >= 5 and len(pos) >= 3 and all(s_ >= 1.0 for _, _, s_ in sen) and not out_
    # leaving: the key's client gone, the radio unkeyed at once (seen by the other connection); a key still on its
    # way when its client left keyed nothing
    closed = ev_at(doc, 0, "closed", after=m["over2"])
    on2 = next((t for t, x in texts_of(doc, 0, m["over2"], closed or 1e9, "trx:0,true")), None)
    left = next((t for t, x in texts_of(doc, 1, closed or 1e9, prefix="trx:0,false")), None)
    leave_ok = on2 is not None and closed is not None and left is not None and left - closed <= 1.0
    ghost = texts_of(doc, 1, m["c2"], 1e9, "trx:0,true")
    ov = overs(S, t0)
    # the listener: the tone through TCI's frames, then through WSJT-X's double ones
    h1 = heard(P, F + 1000, (ov[0][0] - 2.5, ov[0][0] - 0.3), (ov[0][0] + 0.8, ov[0][0] + 2.3), half=150) \
        if ov and ov[0][1] else (None, "no first over")
    h2 = heard(P, F + 1000, (ov[0][0] - 2.5, ov[0][0] - 0.3), (ov[0][1] - 2.2, ov[0][1] - 0.4), half=150) \
        if ov and ov[0][1] else (None, "no first over")
    heard_ok = all(h[0] is not None and h[0] >= HEARD_DB for h in (h1, h2))
    # control: TCI's for the over, the bench's session's again after it
    ctl = ctl_lines(S, (ov[0][0] - 1.0) if ov else t0, ((ov[0][1] or time.time()) + 4.0) if ov else None)
    to_tci = any(x.startswith("tci") for _, x in ctl)
    back = bool(ctl and ov and ov[0][1] and any(t >= ov[0][1] - 0.5 and x.startswith("browser") and x.endswith(" 1")
                                                for t, x in ctl))
    ctl_ok = to_tci and bool(back) and S.ctl_mine
    ok = (not stop and frames_ok and no_rx and sens_ok and leave_ok and not ghost and len(ov) == 2 and heard_ok
          and ctl_ok)
    d0 = off0 - bad if off0 is not None and bad is not None else None
    d1 = off1 - bad if off1 is not None and bad is not None else None
    dl = left - closed if left is not None and closed is not None else None
    return ok, (f"{proof}; over 1: TCI frames, then WSJT-X's double-size ones (sent {dbl}): no unkey before the "
                f"wrong-size frame {not early}, after it (good frames following) trx:0,false in {fmt(d0, '{:.2f}')} s "
                f"(the other connection {fmt(d1, '{:.2f}')} s); {P.L.name}: TCI's {h1[1]}; double {h2[1]}. RX audio "
                f"frames to the keying client: {rx_b} in 1.2 s before, {rx_d} on the air, {rx_a} from 0.5 s after the "
                f"unkey to the next over. tx_sensors {len(sen)} on the air ({len(pos)} with power, the last "
                f"{sen[-1][1:] if sen else None} W/SWR), {len(out_)} off it. Control: to TCI {to_tci}, back to the "
                f"bench's session {bool(back)} ({[x for _, x in ctl][:6]}). Over 2: its client left after "
                f"{fmt(closed - on2 if closed and on2 else None)} s, the radio's unkey seen {fmt(dl, '{:.2f}')} s "
                f"later; a key sent by a client that left at once keyed {'nothing' if not ghost else 'THE RADIO'}; "
                f"{len(ov)} overs by the remote" + tail(stop, doc))


# ======== remote-busy: the radio on the air for one, the others refused ========

def b_remote_busy(P):
    S, c = P.S, P.c
    proof = P.prove(F, l_mode=6)
    P.sender(F, 6, W0)
    P.banner("PTT ON THE AIR: THE OTHER CLIENTS TOLD, THEIR STARTS AND CONTROL REFUSED")
    viewer, _ = fresh(S.host)
    P.extra.append(viewer)
    other = bench.Radio(S.name, c["radios"][S.name], False)     # the receive-only session's own connection
    P.extra.append(other)
    time.sleep(1.0)
    texts(viewer, 0.3)
    P.watch_on(W0, "PTT AM")
    bench.tx_log({"who": S.name, "f": F, "w": S.pwr(), "what": "air_c remote-busy PTT"})
    tA = time.time()
    lab = P.lab_start([{"open": 0}, {"until": 0, "text": "trx:0,true", "secs": 16}, {"wait": 1.0}, {"mark": "try"},
                       {"send": 0, "text": "trx:0,true;"}, {"until": 0, "text": "trx:0,false", "secs": 2},
                       {"wait": 0.8}, {"mark": "release"}, {"send": 0, "text": "trx:0,false;"}, {"wait": 3.0}], 35)
    time.sleep(2.5)                         # the client connected and waiting
    took, mine = {}, []

    def during(s_on):
        mine.append(S.ctl_mine)
        if s_on > 1.0 and "t" not in took:
            took["t"] = time.time()
            other.send("control take")
    hold = int(time.time() * 10) % 10 ** 7
    on, off = ptt_over(P, 7.0, hold, during)
    docA = lab.join()
    stopA = P.watch_off()
    time.sleep(1.0)
    got_v = [x for x in texts(viewer, 0.5) if x.startswith("tx ")]
    got_o = [x for _, x in lines(other, tA, prefix="tx ")]
    busy = any("busy: the radio is transmitting, control stays where it is" in e for e in other.errs)
    other_had = other.ctl_mine
    other.send("control release")
    mA = {x[3]: x[0] for x in evs(docA, None, "mark")}
    tci_no = bool(texts_of(docA, 0, mA.get("try", 1e9), mA.get("release", 0), "trx:0,false"))
    ctl_tci = [x for _, x in ctl_lines(S, on or tA, off) if x.startswith("tci")]
    dur = (off - on) if on and off else None
    ovA = overs(S, tA)
    edA = [(round(t - tA, 2), v) for t, v in edges(S, tA)]
    held = dur is not None and dur >= 6.0 and len(ovA) == 1    # held 7 s; the client's trx:0,false; 2-3 s into it
    hA = heard_over(P, F, on, off) if on else (None, "no over")
    a_ok = (not stopA and got_v[-2:] == ["tx 1", "tx 0"] and got_o[-2:] == ["tx 1", "tx 0"] and busy and
            not other_had and all(mine) and tci_no and not ctl_tci and held and hA[0] is not None and
            hA[0] >= HEARD_DB)
    saysA = (f"PTT {fmt(dur)} s on the air: an unpaired viewer got {got_v}, the receive-only session {got_o}; its "
             f"'control take' {'refused (busy)' if busy else 'NOT refused'}, control stayed with the bench "
             f"{all(mine) and not other_had}; a TCI client's trx:0,true; answered trx:0,false {tci_no}, control to "
             f"TCI {bool(ctl_tci)}, its trx:0,false; unkeyed {'nothing' if held else 'THE RADIO'} (the remote's TX "
             f"edges from the start, s: {edA}); {P.L.name}: "
             f"{hA[1]}" + (f"; STOPPED: {stopA}" if stopA else ""))
    other.close()
    viewer.close()
    # B: one TCI client on the air, another's starts refused
    # a 'control take' that was not refused took it for the test's own other session, which let it go (closed):
    # taken again only when nobody holds it; the radio's operator's is never taken back while the test runs
    take_if_free(S, "after the PTT over")
    P.retune(F, 3, W0)                      # both USB-D on F now, the tone 1000 Hz up
    P.watch_on(W0, "TCI over B")
    bench.tx_log({"who": S.name, "f": F, "w": S.pwr(), "what": "air_c remote-busy TCI"})
    tB = time.time()
    docB = P.lab([{"open": 1}, {"open": 2}, {"wait": 1.0}, {"send": 1, "text": "trx:0,true,tci;"},
                  {"audio": 1, "hz": 1000, "amp": 0.3, "secs": 12}, {"until": 1, "text": "trx:0,true", "secs": 3},
                  {"wait": 1.5}, {"mark": "c2"}, {"send": 2, "text": "trx:0,true;"},
                  {"until": 2, "text": "trx:0,false", "secs": 2}, {"send": 2, "text": "trx:0,true,tci;"},
                  {"until": 2, "text": "trx:0,false", "secs": 2}, {"wait": 0.3}, {"send": 2, "text": "trx:0,false;"},
                  {"wait": 2.0}, {"mark": "end"}, {"send": 1, "text": "trx:0,false;"},
                  {"until": 1, "text": "trx:0,false", "secs": 3}, {"wait": 2.0}], 35)
    stopB = P.watch_off()
    P.off_air(5)
    mB = {x[3]: x[0] for x in evs(docB, None, "mark")}
    refused2 = len([e for e in evs(docB, 2, "until_hit") if e[3] == "trx:0,false"]) == 2
    on1 = next((t for t, x in texts_of(docB, 1, 0, 1e9, "trx:0,true")), None)
    c1_cut = texts_of(docB, 1, (on1 or 0) + 0.05, mB.get("end", 0), "trx:0,false")
    ovB = overs(S, tB)
    durB = (ovB[0][1] - ovB[0][0]) if len(ovB) == 1 and ovB[0][1] else None
    ctlB = [x for _, x in ctl_lines(S, ovB[0][0] - 0.5, ovB[0][1]) if x.startswith("tci")] if ovB else []
    hB = heard(P, F + 1000, (ovB[0][0] - 2.5, ovB[0][0] - 0.3), (ovB[0][1] - 2.5, ovB[0][1] - 0.4), half=150) \
        if durB else (None, "no over")
    # the plan holds the first client's over about 4 s (its key, 1.5 s, the second client's two starts answered at
    # once, 0.3 s, its stop, 2 s; round 2 2026-11-23: 4.1 s both ways): 3.5 s and its own unkey after the 'end' mark
    b_ok = (not stopB and refused2 and on1 is not None and not c1_cut and durB is not None and durB >= 3.5 and
            len(ctlB) == 1 and hB[0] is not None and hB[0] >= HEARD_DB)
    saysB = (f"a TCI over of {fmt(durB)} s (the first client keyed {on1 is not None}): the second client's "
             f"trx:0,true; and trx:0,true,tci; both answered "
             f"trx:0,false {refused2}, its trx:0,false; ending nothing ({'none' if not c1_cut else 'CUT'} to the first "
             f"before its own unkey), control told {len(ctlB)}x to TCI (one: the first client's); {P.L.name} in the "
             f"over's last 2 s: {hB[1]}" + (f"; STOPPED: {stopB}" if stopB else ""))
    return a_ok and b_ok, f"{proof}; A: {saysA}. B: {saysB}"


# ======== tci-cw: CW text through TCI into the listener's decoder ========

MORSE = {"A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....", "I": "..",
         "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
         "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..",
         "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....", "6": "-....",
         "7": "--...", "8": "---..", "9": "----.", ":": "---...", ",": "--..--", "=": "-...-"}
CW_TEXT1 = "VV TEST^1~2 |BT| >FAST#"      # ^ : and ~ , (TCI's stand-ins), |BT| its =, > 5 WPM up, # no sign: dropped
CW_TEXT2 = " Q1TST"                       # sent while the first goes: queued behind it
CW_LONG = "VVV VVV VVV VVV VVV VVV"


def morse_units(text):
    """The dit lengths of a plain text keyed (TCI's stand-ins and signs as the keyer takes them, the rest dropped)"""
    t = text.replace("^", ":").replace("~", ",").replace("|BT|", "=").replace(">", "").replace("#", "")
    n, prev = 0, False
    for ch in t.upper():
        if ch == " ":
            n += 4 if prev else 0
            prev = False
            continue
        code = MORSE.get(ch)
        if not code:
            continue
        n += sum(1 if x == "." else 3 for x in code) + len(code) - 1 + 3
        prev = True
    return n


def squeeze(s):
    return "".join(s.upper().split())


def decoded(L, t0, t1):
    out = ""
    for _, x in lines(L, t0, t1, "cw "):
        try:
            out += json.loads(x[3:])
        except ValueError:
            pass
    return out


def b_tci_cw(P):
    S, L = P.S, P.L
    proof = P.prove(F_CW, l_mode=4, agc=2)
    P.sender(F_CW, 4, W0)
    units = morse_units(CW_TEXT1 + CW_TEXT2)
    # both texts within 14.3 s (the watch's limit is 16 s; FAST goes 5 WPM up, the rest at 21 WPM): at 28, 24 and 23
    # WPM (rounds 1-4 2026-11-23, 2026-11-25) the X6200's decoder misread the X6100's fast elements (',' as 'Q M',
    # <?> or 'Ñ', ':' as '8' or '9', S as 'EEE' or 'I', FAST as 'ENAST'). The X6200 sending, 13.5 s (23 WPM): its texts
    # keyed 13.8 s at 21 WPM and it stays on the air a second or two after its last element (its QSK delay and its
    # base's tail), and one over in two went over 16 s and the Watch stopped it (2026-11-25 05:41)
    lo = max(18, min(24, math.ceil(1.2 * units / (13.5 if model(P.run, S.name) == "x6200" else 14.3))))
    P.banner(f"TCI CW TEXT: '{CW_TEXT1}{CW_TEXT2}', CW_MACROS_STOP, A PTT HOLDER'S TEXT REFUSED")
    P.watch_on(W0, "TCI CW text")
    bench.tx_log({"who": S.name, "f": F_CW, "w": S.pwr(), "what": "air_c tci-cw"})
    # the listener's audio of the whole of it kept (cw_replay replays it through the decoder: 2026-11-25 the X6100
    # printed nothing of the X6200's TEST 2 s after the X6200's PTT over, twice)
    from tones import TciRx
    rec = TciRx(L.host)
    t0 = time.time()
    doc = P.lab([{"open": 0}, {"open": 1}, {"speed": 0, "min": lo, "max": 28}, {"wait": 1.0}, {"mark": "syntax"},
                 {"send": 0, "text": f"cw_macros:0,{CW_TEXT1};"}, {"wait": 0.4},
                 {"send": 0, "text": f"cw_macros:0,{CW_TEXT2};"}, {"until": 0, "text": "cw_macros_empty", "secs": 16},
                 {"wait": 3.0}, {"mark": "long"}, {"send": 0, "text": f"cw_macros:0,{CW_LONG};"}, {"wait": 3.0},
                 {"mark": "stop"}, {"send": 0, "text": "cw_macros_stop;"},
                 {"until": 0, "text": "cw_macros_empty", "secs": 3}, {"wait": 3.0},
                 {"mark": "ptt"}, {"open": 2}, {"send": 1, "text": "trx:0,true;"},
                 {"until": 1, "text": "trx:0,true", "secs": 3}, {"wait": 0.8}, {"mark": "own"},
                 {"send": 1, "text": "cw_macros:0,EEEEE;"}, {"wait": 1.5}, {"mark": "other"},
                 {"send": 2, "text": "cw_macros:0,EEEEE;"}, {"until": 2, "text": "trx:0,false", "secs": 2},
                 {"wait": 1.0}, {"mark": "unkey"}, {"send": 1, "text": "trx:0,false;"},
                 {"until": 1, "text": "trx:0,false", "secs": 3}, {"wait": 2.5}, {"mark": "after"},
                 {"send": 1, "text": "cw_macros:0,TEST;"}, {"until": 1, "text": "cw_macros_empty", "secs": 8},
                 {"wait": 2.5}], 70)
    try:
        import digimodes
        digimodes.write_raw(rec.audio(t0, time.time())[0], os.path.join(P.run.dir, f"tci_cw_{S.name}_to_{L.name}.raw"))
    except (ImportError, OSError, ValueError):
        pass
    finally:
        rec.close()
    stop = P.watch_off()
    P.off_air(5)
    time.sleep(1.5)
    m = {x[3]: x[0] for x in evs(doc, None, "mark")}
    need = ("syntax", "long", "stop", "ptt", "own", "other", "unkey", "after")
    if not all(k in m for k in need):
        return False, f"{proof}; the client did not run through ({doc.get('why')}; marks {sorted(m)})"
    # the radio's clock onto this computer's: the first key-down, as the client heard it and as the remote told it
    r_key = next((t for t, x in texts_of(doc, 1, m["syntax"], m["long"], "trx:0,true")), None)
    m_key = next((t for t, v in edges(S, t0) if v), None)
    if r_key is None or m_key is None:
        return False, f"{proof}; the text keyed nothing (the client: {doc.get('why')}; speed {evs(doc, 0, 'speed')})"

    def mac(r):
        return m_key + (r - r_key)
    # The PTT holder's over (trx:0,true and no text of its own that keys: in CW nothing goes out) is no over to hear:
    # the Watch's 'did NOT hear' of it dropped (2026-11-25: its rise 12.1 and 15.0 dB, the deaf check's 15 at the
    # edge, failed a test whose every check was right)
    ptt_lo, ptt_hi = mac(m["ptt"]) - 0.5, mac(m["unkey"]) + 1.5
    for w_ in P.done:
        w_.deaf[:] = [d for d in w_.deaf if not ptt_lo <= d.get("since", 0) <= ptt_hi]
    txt1 = decoded(L, mac(m["syntax"]), mac(m["long"]))
    s1 = squeeze(txt1)
    i_t = s1.find("TEST:1,2")
    i_eq = s1.find("=", i_t) if i_t >= 0 else -1
    i_f = s1.find("FAST", max(i_eq, 0)) if i_eq >= 0 else -1
    i_c = s1.find("Q1TST", max(i_f, 0)) if i_f >= 0 else -1
    empty1 = ev_at(doc, 0, "until_hit", "cw_macros_empty", after=m["syntax"])
    syntax_ok = i_t >= 0 and i_eq > i_t and i_f > i_eq and i_c > i_f and "#" not in s1 and empty1 is not None
    # cw_macros_stop: trx:0,false then cw_macros_empty:0 at once, the radio off the air
    a_f = next((t for t, x in texts_of(doc, 0, m["stop"], m["ptt"], "trx:0,false")), None)
    a_e = next((t for t, x in texts_of(doc, 0, m["stop"], m["ptt"], "cw_macros_empty")), None)
    later = [t for t, v in edges(S, mac(m["stop"]) + 0.8, mac(m["ptt"])) if v]
    off_s = next((t - mac(m["stop"]) for t, v in edges(S, mac(m["stop"]) - 0.3, mac(m["ptt"])) if not v), None)
    # cw_macros_stop has no answer of its own (TCI has none: src/tci_server.c, "the radio's trx follows"): its
    # trx:0,false and the radio off the air at once; cw_macros_empty is the end of a text that all went
    stop_ok = (a_f is not None and a_f - m["stop"] <= 0.8 and off_s is not None and off_s <= 1.2 and not later)
    txt2 = decoded(L, mac(m["long"]), mac(m["ptt"]))
    # a client holding PTT sends no text, nor does another meanwhile; its own text after its unkey goes
    ptt_keyed = ev_at(doc, 1, "until_hit", "trx:0,true", after=m["ptt"]) is not None     # its PTT did key
    own = texts_of(doc, 1, m["own"], m["other"], "trx:0,false")
    other_no = ev_at(doc, 2, "until_hit", "trx:0,false", after=m["other"]) is not None
    txt3 = decoded(L, mac(m["own"]), mac(m["unkey"]) + 1.5)
    on_air = [v for t, v in edges(S, mac(m["own"]), mac(m["unkey"]))]
    empty4 = ev_at(doc, 1, "until_hit", "cw_macros_empty", after=m["after"])
    txt4 = decoded(L, mac(m["after"]), time.time())
    ptt_ok = ptt_keyed and "EEE" not in squeeze(txt3) and other_no and 0 not in on_air and empty4 is not None and \
        "TEST" in squeeze(txt4)
    speed = evs(doc, 0, "speed")
    changed = bool(speed) and speed[0][3][0] != speed[0][3][1]
    speed_back = not changed or bool(evs(doc, 0, "restored"))
    ok = not stop and syntax_ok and stop_ok and ptt_ok and speed_back
    in_order = i_t >= 0 and i_eq > i_t and i_f > i_eq and i_c > i_f
    # the decoders' known faults (the owner's list, 2026-11-23): the X6200's misreads a word's first letter (Q as Å),
    # the X6100's garbles the X6200's CW into E's and T's. Everything but the decoded text right: a look, said
    rest = (not stop and empty1 is not None and "#" not in s1 and stop_ok and ptt_keyed and other_no and
            0 not in on_air and empty4 is not None and speed_back)
    # the X6200's decoder at 24-28 WPM read ':' as '8', ',' as <?> or 'Q M', S as 'EEE', F as <SN> (rounds 1-3,
    # 2026-11-23): a stand-in turned into its sign (':' or ','), |BT| as '=', then FAST's end and the queued text, in
    # that order, with no '#'
    k = [s1.find("=")]
    for x in ("AST", "1TST"):
        k.append(s1.find(x, k[-1] + 1) if k[-1] >= 0 else -1)
    loose = (all(x >= 0 for x in k) and ("," in s1[:k[0]] or ":" in s1[:k[0]]) and "EST" in squeeze(txt4) and
             "EEE" not in squeeze(txt3))
    known = ""
    if not ok and rest and not in_order and "EEE" not in squeeze(txt3) and txt1.strip():
        # whose the misread is (cw_timing.py, 2026-11-25): the sender's keyer late on its deadlines is the GUI's (a
        # fail); on time, an X6100 on Xiegu's factory base moves its key edges 30-50 ms now and then (cw-timing x6100:
        # said); from clean keying (the X6200's, within 7 ms) a misread is the decoder's (a fail)
        import cw_timing as ct
        import suite_air_a as aa
        keyer = ct.keyer_said(lambda h, c: bench.ssh(h, c, timeout=15), S.host, last=6)
        base = aa.base_of(S.host) if model(P.run, S.name) == "x6100" else ""
        if keyer is not None and keyer[1]:
            known = f"; the keyer itself late ({keyer[1]} of {keyer[0]} edges over 15 ms): the GUI's"
        elif model(P.run, S.name) == "x6100" and aa.gcfix_of(base) == 0 and keyer is not None:
            ok = None
            known = (f"; LOOK: the keyer on time ({keyer[0]} edges, the latest {keyer[2]} ms), {S.name}'s base "
                     f"(Xiegu's {base or 'factory'}) moves its key edges (cw-timing x6100): every TCI check right but "
                     f"the decoded text (said)")
        else:
            known = "; the decoder misread clean keying"
    df = a_f - m["stop"] if a_f is not None else None
    de = a_e - m["stop"] if a_e is not None else None
    return ok, (f"{proof}; KEY Speed {speed[0][3] if speed else '?'} (was, set) for {units} dits; {L.name}'s "
                f"decoder (the remote's 'cw' lines) on the texts: {txt1.strip()[-70:]!r} (TEST:1,2 = FAST then Q1TST "
                f"in order: {in_order}; cw_macros_empty at the end {empty1 is not None}); cw_macros_stop 3 s into a 10 "
                f"s text: trx:0,false {fmt(df, '{:.2f}')} s after it, off the air {fmt(off_s, '{:.2f}')} s "
                f"(this computer's clock), cw_macros_empty {fmt(de, '{:.2f}')} s (none: TCI's stop has no answer), keyed "
                f"again after it {len(later)}x (decoded {txt2.strip()[-30:]!r}); a client holding PTT (keyed "
                f"{ptt_keyed}): its text answered {[x for _, x in own] or 'nothing'}, another client's refused "
                f"{other_no}, decoded meanwhile "
                f"{txt3.strip()[-20:]!r} (no E's: {'EEE' not in squeeze(txt3)}), the radio stayed keyed "
                f"{0 not in on_air}; its text after its unkey: {txt4.strip()[-20:]!r}; KEY Speed put back "
                f"{speed_back if changed else '(not changed)'}" + known + tail(stop, doc))


# ======== tci-level: the TCI transmit level, by the sender's meter and the listener's ear ========

def b_tci_level(P):
    S = P.S
    mdl = model(P.run, S.name)
    if mdl == "x6200":
        steps = [(0.5, 1.0), (0.5, 1.0), (0.5, 1.0), (0.3, 1.0)]
        cov = ["tci-tx-level-x6200"]
    else:
        steps = [(0.1, 0.3), (1.0, 0.3), (1.0, 0.1), (1.0, 0.5)]
        cov = ["tci-tx-level-x6100", "tx-tci-transmit-level"]
    proof = P.prove(F, l_mode=3)
    P.sender(F, 3, W0)                      # both USB-D on F, the tone 1000 Hz up
    rows, stop = [], None
    for w, amp in steps:
        if w > W_TOP:
            raise bench.Refused(f"{w} W asked: over the bench's 1 W for these tests")
        bench.ensure(S, {"pwr": w})
        bench.radio_tx_gate(S, P.c, w)
        P.banner(f"TCI TRANSMIT LEVEL: {w} W, THE TONE AT {20 * math.log10(amp):.0f} DBFS")
        P.watch_on(w, f"TCI level {w} W {20 * math.log10(amp):.0f} dBFS")
        (on, off), _ = tone_over(P, 1000, 4.0, w, amp=amp).join()
        stop = P.watch_off()
        if stop or not on or not off:
            rows.append({"w": w, "amp": amp, "meter": None})
            break
        ws, alc = steady(meters(S, on, off))
        q = bench.spectrum_level(P.L, F + 1000, on - 2.5, on - 0.3, half=150)[0]
        lvl = bench.spectrum_level(P.L, F + 1000, on + 1.5, off - 0.3, half=150)[0]
        rows.append({"w": w, "amp": amp, "meter": ws, "alc": alc, "lvl": lvl,
                     "rise": None if q is None or lvl is None else round(lvl - q, 1)})
        time.sleep(1.5)
    said = "; ".join(f"{r['w']} W at {20 * math.log10(r['amp']):.0f} dBFS: meter {fmt(r['meter'], '{:.2f}')} W "
                     f"(ALC {fmt(r.get('alc'), '{:.2f}')}), {P.L.name} {fmt(r.get('rise'), '{:+.1f}')} dB over its "
                     f"quiet" for r in rows)
    if stop or len(rows) < len(steps) or any(r["meter"] is None or r["lvl"] is None for r in rows):
        return False, f"{proof}; {said}" + (f"; STOPPED: {stop}" if stop else ""), cov, []
    heard_all = all(r["rise"] is not None and r["rise"] >= HEARD_DB for r in rows)
    if mdl == "x6200":
        r1, r2, r3, r4 = rows
        kept = 0.25 <= r2["meter"] <= 0.65 and 0.25 <= r3["meter"] <= 0.65
        steady_ = abs(r3["lvl"] - r2["lvl"]) <= 2.0
        drop = r3["lvl"] - r4["lvl"]
        scaled = 0.15 <= r4["meter"] <= 0.42 and r4["meter"] < r3["meter"] and 0.5 <= drop <= 5.0
        ok = heard_all and kept and steady_ and scaled
        verdict = (f"a full-scale tone at 0.5 W read {r2['meter']:.2f} and {r3['meter']:.2f} W in the 2nd and 3rd "
                   f"overs (at most about 0.6: {kept}), the listener steady within {abs(r3['lvl'] - r2['lvl']):.1f} dB "
                   f"({steady_}); 0.3 W: {r4['meter']:.2f} W, {drop:.1f} dB lower at the listener (2.2 expected: "
                   f"{scaled})")
    else:
        r1, r2, r3, r4 = rows
        at1 = 0.7 <= r2["meter"] <= 1.3
        span = r2["lvl"] - r1["lvl"]
        ten = 6.0 <= span <= 14.0
        # the meter at 0.1 W: 0.05-0.16, or under 0.05 with the power proved by the listener (heard, 10 dB under the
        # 1 W over): the X6100's factory base's reading is whole tenths cut off, a true 0.09 W reads 0.0 (2026-11-25,
        # the listener 10.0 dB apart)
        at01 = 0.05 <= r1["meter"] <= 0.16 or (r1["meter"] < 0.05 and ten and at1 and r1["rise"] is not None and
                                                r1["rise"] >= HEARD_DB)
        same = r3["meter"] >= 0.6 and r4["meter"] >= 0.6 and abs(10 * math.log10(r4["meter"] / r3["meter"])) <= 1.5
        ok = heard_all and at01 and at1 and ten and same
        if not ok and heard_all and at1 and ten and same and 0.16 < r1["meter"] <= 0.45:
            # known (the owner's list, 2026-11-23): the X6100's meter reads 0.2-0.4 W at 0.1 W asked, its listener's
            # level 10 dB under the 1 W over all the same: a look at the meter, not a fail
            ok = None
        verdict = (f"levelled to the setting after the first second: 0.1 W read {r1['meter']:.2f} ({at01}), 1 W "
                   f"{r2['meter']:.2f} ({at1}), {span:.1f} dB apart at the listener (10 expected: {ten}); -20 and -6 "
                   f"dBFS at 1 W: {r3['meter']:.2f} and {r4['meter']:.2f} W ({same})")
    return ok, f"{proof}; {said}; {verdict}" + ("; LOOK: the X6100's meter at 0.1 W reads high (known)" if ok is None
                                                 else ""), cov, []


# ======== tci-ask: a client on this computer, asked on the screen ========

def b_tci_ask(P):
    from suite_cover_b import keep
    S = P.S
    h = S.host
    ip = socket.gethostbyname(h)
    st, body = http(ip, "/api/tci")
    orig = json.loads(body) if st == 200 else None
    if not orig or not orig.get("enabled"):
        return None, f"TCI not on as found ({orig}): not run"
    port = int(orig.get("port", 50001))
    me = sh(h, "echo ${SSH_CLIENT%% *}", 20).strip()
    allowed0 = sh(h, "cat /mnt/tci_allowed.conf 2>/dev/null; true", 20)
    if me and any(x.split("\t")[0] == me for x in allowed0.splitlines()):
        return None, f"this computer ({me}) is allowed always on {S.name} (Settings > TCI): nothing to ask; not run"
    k_conf, k_allow = keep(h, "/mnt/tci.conf", "save"), keep(h, "/mnt/tci_allowed.conf", "save")
    res, said, conns = {}, [], []

    def post(confirm):
        b = {"enabled": True, "port": port, "trust": orig.get("trust", "operate"), "tx_confirm": bool(confirm)}
        code = http(ip, "/api/tci", method="POST", body=b)[0]
        time.sleep(2.6)                      # the radio reads tci.conf when its time changes (a second's grain)
        return code

    def client(wss=False):
        t = (Remote(ip, 443, path="/ws/tci", tls=True, headers={"Host": ip}) if wss else Remote(h, port, timeout=5))
        conns.append(t)
        P.extra.append(t)
        end = time.time() + 6
        while time.time() < end:
            x = t.recv(timeout=0.5)
            if x is not None and x.startswith("ready"):
                break
        return t

    def start(t):
        """trx:0,true; from t: the radio's answer (trx:0,true; or trx:0,false;), the question open, on the air"""
        texts(t, 0.2)
        t.send("trx:0,true;")
        ans, end = None, time.time() + 3
        while time.time() < end and ans is None:
            x = t.recv(timeout=0.2)
            if x and x.lower().startswith("trx:0,"):
                ans = x
        time.sleep(1.0)
        return ans, S.state.get("ask"), S.tx

    def unkey(t):
        t.send("trx:0,false;")
        P.off_air(5)

    def answer(once):
        """The question answered at the radio: Allow once (the MFK five clicks left: the first of the three, it
        stops there) and a push, or a push alone (No: the one chosen as it opens)"""
        if once:
            for _ in range(5):
                S.send("knob mfk -1")
                time.sleep(0.3)
        time.sleep(0.3)
        bench.tap(S, "MFK")
        try:
            S.wait("the question closed", lambda: S.state.get("ask") == 0, 4)
            return True
        except bench.Refused:
            return False

    def over_heard(t, secs=2.5):
        t0 = time.time()
        try:
            S.wait("on the air", lambda: S.tx, 3)
        except bench.Refused:
            return False, "not on the air"
        time.sleep(secs)
        unkey(t)
        ov = overs(S, t0 - 1.5)
        if not ov:
            return False, "no over"
        r, w_ = heard_over(P, F, ov[-1][0], ov[-1][1])
        return r is not None and r >= HEARD_DB, w_

    try:
        proof = P.prove(F, l_mode=6)
        P.sender(F, 6, W0)
        P.banner("TCI ASK BEFORE TX: CLIENTS ON THE NETWORK ASKED, ANSWERED FROM THE BENCH")
        if not orig.get("tx_confirm"):
            post(True)
        P.watch_on(W0, "TCI ask")
        bench.tx_log({"who": S.name, "f": F, "w": S.pwr(), "what": "air_c tci-ask"})
        f0 = int(S.state.get("freq", 0))
        # 0: a program on the radio itself keys at once, nobody asked
        t0 = time.time()
        doc = P.lab([{"open": 0}, {"send": 0, "text": "trx:0,true;"}, {"until": 0, "text": "trx:0,true", "secs": 3},
                     {"wait": 2.0}, {"send": 0, "text": "trx:0,false;"}, {"until": 0, "text": "trx:0,false",
                                                                        "secs": 3}, {"wait": 1.0}], 20)
        ov = overs(S, t0)
        res["local keys at once, not asked"] = (ev_at(doc, 0, "until_hit", "trx:0,true") is not None and len(ov) == 1
                                                and S.state.get("ask") == 0)
        said.append(f"a program on the radio: keyed {len(ov)}x, asked {S.state.get('ask')}")
        # 1: asked; Allow once keys it while connected
        t1 = client()
        a, ask, tx = start(t1)
        once = answer(True)
        a2, _, _ = start(t1)
        k1, w1 = over_heard(t1)
        res["asked, then Allow once keys it"] = a == "trx:0,false;" and ask == 1 and not tx and once and \
            a2 == "trx:0,true;" and k1
        said.append(f"a client here: {a} and asked {ask}; Allow once taken {once}; again: {a2}, heard {k1} ({w1})")
        t1.close()
        # 2: a new connection is asked again (No)
        t2 = client()
        a, ask, tx = start(t2)
        no = answer(False)
        res["asked again on a new connection"] = a == "trx:0,false;" and ask == 1 and not tx and no
        said.append(f"its new connection: {a}, asked {ask}, No taken {no}")
        t2.close()
        # 3: answered after it hung up: its next connection may transmit
        t3 = client()
        a, ask, tx = start(t3)
        t3.close()
        time.sleep(1.0)
        late = answer(True)
        t4 = client()
        a4, ask4, _ = start(t4)
        k4, w4 = over_heard(t4)
        res["Allow once after it hung up: the next connection keys"] = ask == 1 and late and a4 == "trx:0,true;" and \
            ask4 == 0 and k4
        said.append(f"asked {ask}, it hung up, Allow once {late}: its next connection {a4} (asked {ask4}), heard {k4}")
        t4.close()
        # 4: Ask before TX off: keys at once
        c4 = post(False)
        t5 = client()
        a5, ask5, _ = start(t5)
        k5, w5 = over_heard(t5)
        res["Ask before TX off keys at once"] = c4 == 200 and a5 == "trx:0,true;" and ask5 == 0 and k5
        said.append(f"tx_confirm off ({c4}): {a5}, asked {ask5}, heard {k5}")
        t5.close()
        # 5: turned off while it asks: the question closes
        post(True)
        t6 = client()
        a6, ask6, _ = start(t6)
        c6 = post(False)
        try:
            S.wait("the question closed", lambda: S.state.get("ask") == 0, 4)
            gone = True
        except bench.Refused:
            gone = False
        res["turning it off closes the question"] = ask6 == 1 and c6 == 200 and gone
        said.append(f"asked {ask6}, tx_confirm off meanwhile: the question closed {gone}")
        t6.close()
        # 6: a tci.conf of before tx_confirm asks
        sh(h, f"printf 'enabled=1\\nport={port}\\ntrust={orig.get('trust', 'operate')}\\n' > /mnt/tci.conf.bench && "
              "mv /mnt/tci.conf.bench /mnt/tci.conf", 20)
        time.sleep(2.6)
        t7 = client()
        a7, ask7, _ = start(t7)
        no7 = answer(False)
        res["a tci.conf without tx_confirm asks"] = a7 == "trx:0,false;" and ask7 == 1 and no7
        said.append(f"a tci.conf with no tx_confirm: {a7}, asked {ask7}")
        t7.close()
        # 7: through the https port's proxy (wss://<radio>/ws/tci): asked about by this computer's address
        t8 = client(wss=True)
        a8, ask8, tx8 = start(t8)
        no8 = answer(False)
        res["wss: known by its own address, asked"] = a8 == "trx:0,false;" and ask8 == 1 and not tx8 and no8
        said.append(f"wss://…/ws/tci: {a8}, asked {ask8}")
        t8.close()
        res["the dial held through the MFK clicks"] = int(S.state.get("freq", 0)) == f0
    finally:
        stop = P.watch_off()
        if S.state.get("ask") == 1:
            bench.tap(S, "MFK")             # an open question: No
        for t in conns:
            try:
                t.close()
            except OSError:
                pass
        back = http(ip, "/api/tci", method="POST", body={"enabled": True, "port": port,
                                                          "trust": orig.get("trust", "operate"),
                                                          "tx_confirm": bool(orig.get("tx_confirm"))})[0]
        allowed1 = sh(h, "cat /mnt/tci_allowed.conf 2>/dev/null; true", 20)
        k1 = keep(h, "/mnt/tci.conf", "back")
        k2 = keep(h, "/mnt/tci_allowed.conf", "back")
        time.sleep(2.6)
    same_allowed = addrs(allowed1) == addrs(allowed0)       # none kept as Always by the test (the GUI holds them too)
    ok = (not stop and all(res.values()) and back == 200 and k1 in ("same", "removed", "restored") and
          same_allowed)
    bad = [k for k, v in res.items() if not v]
    return ok, (f"{proof}; " + "; ".join(said) + f". Not: {bad}" * bool(bad) +
                f"; put back: /api/tci {back}, tci.conf {k0_word(k_conf, k1)}, tci_allowed.conf {k0_word(k_allow, k2)}"
                + ("" if same_allowed else f" (CHANGED: {sorted(addrs(allowed1) - addrs(allowed0))} kept as Always)")
                + (f"; STOPPED: {stop}" if stop else ""))


def k0_word(k0, k1):
    return f"{k0} then {k1}"


def addrs(text):
    """The addresses of tci_allowed.conf (ADDR\tNAME\tCREATED\tLAST a line)"""
    return {ln.split("\t")[0] for ln in text.splitlines() if ln.strip()}


# ======== ext-tune: TUNE for an external tuner from the remote ========

TUNE_W = 0.1                    # TUNE's own power for ext-tune (the remote's "set tpwr 1"; finish puts it back)
TUNE_GUARD_W = 0.4              # a tune's TX meter over this: "tune 0" at once (suite_air_a's guard: twice T, T + 0.3)
TUNE_MAX_S = 12.0               # a tune still on after this: "tune 0" here


class Renew:
    """'tune 1 HOLD' sent every half second by r until stop(); guarded: its meter over TUNE_GUARD_W (after its first
    0.6 s) or on past TUNE_MAX_S, "tune 0" at once and the renewals stopped (self.over, self.long say so)"""

    def __init__(self, r, hold):
        self.r, self.hold, self.on, self.over, self.long = r, hold, True, None, None
        r.send(f"tune 1 {hold}")
        self.th = threading.Thread(target=self._run, daemon=True)
        self.th.start()

    def _run(self):
        last = 0.0
        while self.on:
            now = time.time()
            if now - last >= 0.5:
                try:
                    self.r.send(f"tune 1 {self.hold}")
                except OSError:
                    return
                last = now
            since, txm = self.r.tx_since, self.r.txm
            if self.r.tx and since and now - since > 0.6 and txm and txm[0] > TUNE_GUARD_W:
                self.over = txm[0]
                self.on = False
                self.r.send("tune 0")
                say(f"{self.r.name}: TUNE read {txm[0]:.2f} W for {TUNE_W} W: stopped")
                return
            if self.r.tx and since and now - since > TUNE_MAX_S:
                self.long = now - since
                self.on = False
                self.r.send("tune 0")
                return
            time.sleep(0.1)

    def stop(self):
        self.on = False
        self.th.join(2)


def b_ext_tune(P):
    S, c = P.S, P.c
    # TUNE at 0.1 W, its own power set with the remote's "set tpwr" (tenths of a watt; the snapshot keeps it as found
    # and finish puts it back, its settings row too: suite_air_a.tune_gate's way), read back from the radio's state
    if "tpwr" not in S.state:
        return None, f"{S.name}'s remote has no TUNE power setter (tpwr): its TUNE power cannot be set to 0.1 W; not run"
    want = round(TUNE_W * 10)
    if int(S.state["tpwr"]) != want:
        S.send(f"set tpwr {want}")
        S.wait(f"TUNE power {TUNE_W} W", lambda: int(S.state.get("tpwr", -1)) == want, bench.SET_WAIT_S)
    tw = int(S.state["tpwr"]) / 10.0
    if tw > TUNE_W + 1e-6:
        return None, f"TUNE's own power on {S.name} reads {tw} W, not {TUNE_W}: not run"
    proof = P.prove(F, l_mode=3)
    P.sender(F, 3, W0)                      # USB-D on both: TUNE sends in USB-D, the mode stays as it keys
    if int(S.state.get("tpwr", -1)) != want:
        raise bench.Refused(f"{S.name}: TUNE power reads {S.state.get('tpwr')} tenths after the set-up, {want} set")
    P.banner(f"TUNE FOR AN EXTERNAL TUNER FROM THE REMOTE, {tw} W")
    S2 = bench.Radio(S.name, c["radios"][S.name], True)          # another page of the same session
    P.extra.append(S2)
    P.watch_on(max(tw, W0), "TUNE")
    bench.tx_log({"who": S.name, "f": F, "w": tw, "what": "air_c ext-tune"})
    hold = int(time.time() * 10) % 10 ** 7
    said, res, rn, rn_all = [], {}, None, []
    try:
        # 1: on while renewed
        S.errs.clear()
        t0 = time.time()
        rn = Renew(S, hold + 1)
        rn_all.append(rn)
        S.wait("on the air", lambda: S.tx or (P.watch and P.watch.why), 4)
        on = time.time()
        seen = []
        while time.time() - on < 3.0:
            seen.append((S.state.get("tune"), S.state.get("tune_s")))
            time.sleep(0.25)
        rn.stop()
        t_last = time.time()
        ms = meters(S, on, t_last)
        r1, w1 = heard(P, F, (on - 2.5, on - 0.4), (on + 0.8, t_last), half=1500)
        res["on: tune 1, tune_s, the meters, heard"] = (all(a == 1 and (b or 0) > 0 for a, b in seen[2:]) and
                                                        len(ms) >= 5 and r1 is not None and r1 >= HEARD_DB)
        said.append(f"'tune 1 N' renewed: on the air, tune/tune_s {seen[-1] if seen else None}, {len(ms)} meter "
                    f"readings (the last {ms[-1][1:3] if ms else None} W/SWR); {P.L.name}: {w1}")
        # 2: no renewal: stopped about 3 s after the last, and said
        S.wait("TUNE stopped", lambda: not S.state.get("tune") and not S.tx, 8)
        d2 = time.time() - t_last
        err2 = any("tune stopped: no renewal from the page" in e for e in S.errs)
        res["no renewal: stopped ~3 s later, said"] = 2.0 <= d2 <= 5.5 and err2
        said.append(f"renewals stopped: off {d2:.1f} s after the last, 'err tune stopped: no renewal' {err2}")
        time.sleep(2.0)
        # 3: 'tune 0' at once
        rn = Renew(S, hold + 2)
        rn_all.append(rn)
        S.wait("on the air", lambda: S.tx, 4)
        time.sleep(1.5)
        rn.stop()
        t3 = time.time()
        S.send("tune 0")
        S.wait("TUNE stopped", lambda: not S.state.get("tune") and not S.tx, 4)
        d3 = time.time() - t3
        res["'tune 0' at once"] = d3 <= 1.2
        said.append(f"'tune 0': off in {d3:.2f} s")
        time.sleep(2.0)
        # 4: another page: without control it stops nothing, with control it stops this one's
        rn = Renew(S, hold + 3)
        rn_all.append(rn)
        S.wait("on the air", lambda: S.tx, 4)
        time.sleep(1.0)
        S2.send("tune 0")
        time.sleep(1.0)
        kept = bool(S.state.get("tune")) and S.tx
        S.send("control release")
        S2.send("control take")
        S2.wait("control for the other page", lambda: S2.ctl_mine, 4)
        t4 = time.time()
        S2.send("tune 0")
        S2.wait("TUNE stopped by the other page", lambda: not S2.state.get("tune") and not S2.tx, 4)
        d4 = time.time() - t4
        time.sleep(1.5)                     # the first page still renewing: a renewal starts nothing again
        again = bool(S.state.get("tune")) or S.tx
        rn.stop()
        res["another page: refused without control, stops it with control"] = kept and d4 <= 1.2 and not again
        said.append(f"another page's 'tune 0' without control: the tune went on {kept}; with control: off in "
                    f"{d4:.2f} s, the first page's renewals starting nothing again {not again}")
    except bench.Refused as e:
        said.append(f"refused: {e}" + (f" (the radio said: {S.errs[-2:]})" if S.errs else ""))
        res["ran through"] = False
    finally:
        guard = [x for x in (rn_all or []) if x.over or x.long]
        if rn:
            rn.stop()
        S.send("tune 0")
        stop = P.watch_off()
        P.off_air(5)
        try:
            if S2.ctl_mine:                 # the test's own other page has it: given back to the bench's first page
                S2.send("control release")
                time.sleep(0.8)
            take_if_free(S, "after ext-tune's other page")
        except (bench.Refused, OSError) as e:
            said.append(f"control: {e}")
            res["control back with the bench's page"] = False
    if guard:
        res["TUNE within its power and time"] = False
        said.append("the guard stopped a tune: " + ", ".join(f"{fmt(x.over, '{:.2f}')} W" if x.over else
                                                             f"on {x.long:.1f} s" for x in guard))
    ok = not stop and bool(res) and all(res.values())
    return ok, f"{proof}; TUNE power {tw} W; " + "; ".join(said) + (f"; STOPPED: {stop}" if stop else "")


# ======== ft8-cq: the FT8 app from the remote, on the air ========

def udp_off(h):
    """The FT8 app's WSJT-X UDP off for the test (nothing to the loggers), put back by udp_back: 'saved' or 'none'"""
    from suite_cover_b import keep
    k = keep(h, "/mnt/ft8_udp.conf", "save")
    if k == "saved":
        sh(h, "sed -i 's/^enabled=1/enabled=0/' /mnt/ft8_udp.conf", 20)
    else:                                   # none: the app would make it, enabled; one off until the test ends
        sh(h, "printf 'enabled=0\\n' > /mnt/ft8_udp.conf", 20)
    if "enabled=0" not in sh(h, "cat /mnt/ft8_udp.conf", 20):
        raise bench.Refused(f"{h}: the WSJT-X UDP not switched off")
    return k


def udp_back(h):
    from suite_cover_b import keep
    return keep(h, "/mnt/ft8_udp.conf", "back")


APP_FREQS = "/mnt/app_freqs"        # app_window.c: each app's last frequency, written as it closes


def ft8_open(P, role, dial):
    """P.S's or P.L's (role "S" or "L") FT8 app open on dial in FT8 (suite_air_a.ft8_open's way): its WSJT-X UDP off
    first and /mnt/app_freqs kept aside (the app writes its dial there as it closes), both put back byte for byte once
    the app closed; CQ off. The app opens in the protocol it was left in and its MODE steps FT8, FT4, FT2 round: the
    steps taken are stepped on round to where it was as it closes, live (the settings put-back fixes the database row
    only), and its TX Call and Auto as found then too. The role, not the session: the sender's page is replaced
    mid-test (the CQ goes on after the page that started it closed)"""
    from suite_cover import keep
    r = getattr(P, role)
    h = r.host
    k_udp = udp_off(h)

    def udp_put():
        b = udp_back(h)
        if b not in ("same", "removed", "restored"):
            raise bench.Refused(f"{r.name}: /mnt/ft8_udp.conf not put back ({k_udp} then {b!r})")
    P.later(f"{r.name}: the WSJT-X UDP as found", udp_put)
    got = keep(h, APP_FREQS, "save")
    if got not in ("saved", "none"):
        raise bench.Refused(f"{r.name}: {APP_FREQS} not kept aside ({got!r})")

    def freqs_put():
        x = getattr(P, role)
        bench.close_apps(x)                 # an app still open writes its frequency there as it closes: first
        b = keep(h, APP_FREQS, "back")
        if b not in ("same", "restored", "removed"):
            raise bench.Refused(f"{x.name}: {APP_FREQS} not put back ({b!r})")
    P.later(f"{r.name}: {APP_FREQS} as found", freqs_put)
    r.send("ft8 open")
    r.wait("the FT8 app", lambda: r.ft8.get("open") == 1, 10)
    time.sleep(1.0)
    steps, found = [0], {"auto": r.ft8.get("auto"), "tx": r.ft8.get("tx")}

    def close():
        x = getattr(P, role)
        x.send("ft8 stop")
        until(x, lambda: not x.tx, 5)
        if x.ft8.get("open") == 1:
            for _ in range((3 - steps[0] % 3) % 3):
                pr = x.ft8.get("proto")
                x.send("ft8 mode")
                if not until(x, lambda pr=pr: x.ft8.get("proto") != pr, 5):
                    break
                time.sleep(0.5)
            for k, cmd in (("auto", "ft8 auto"), ("tx", "ft8 txcall")):
                if found[k] is not None and x.ft8.get(k) is not None and bool(x.ft8.get(k)) != bool(found[k]):
                    x.send(cmd)
                    until(x, lambda k=k: bool(x.ft8.get(k)) == bool(found[k]), 4)
        x.send("ft8 close")
        until(x, lambda: x.ft8.get("open") != 1, 5)
    P.later(f"{r.name}: its FT8 app as found and closed", close)
    while r.state.get("app") != "FT8" and steps[0] < 3:
        pr = r.ft8.get("proto")
        r.send("ft8 mode")
        r.wait("FT8's mode stepped", lambda pr=pr: r.ft8.get("proto") != pr, 5)
        time.sleep(0.8)
        steps[0] += 1
    if r.state.get("app") != "FT8":
        raise bench.Refused(f"{r.name}: its FT8 app not in FT8 ({r.state.get('app')})")
    bench.ft8_to_band(r, dial)
    if r.ft8.get("cq"):
        r.send("ft8 stop")
        r.wait("CQ off", lambda: not r.ft8.get("cq"), 5)
    return found


def ft8_lines(r, t0, t1=None):
    out = []
    for t, x in lines(r, t0, t1, "ft8 {"):
        try:
            out.append((t, json.loads(x[4:])))
        except ValueError:
            pass
    return out


def is_cq(row, call):
    w = bench.msg(row).split()
    return len(w) >= 2 and w[0] == "CQ" and w[1].strip("<>") == call


def b_ft8(P):
    S, L = P.S, P.L
    call = (param(S.host, "callsign", "") or "").strip()
    if not TEST_CALL.match(call):
        return None, (f"{S.name}'s callsign is {call or 'none'}, not a test call: its CQ would send it (bench.py "
                      f"testcall {S.name} Q1TST first); not run")
    reps = int(param(S.host, "ft8_max_repeats", "6"))
    if reps < 3:
        return None, f"{S.name}'s FT8 Retries is {reps}: three CQs are needed; not run"
    decode = param(L.host, "pskreporter", "1") == "0"    # the listener's FT8 decodes go to PSK Reporter when it is on
    proof = P.prove(F_FT8, l_mode=3, agc=3)
    said, res, stop = [], {}, None
    auto0 = None
    try:
        if decode:                          # the listener first: on the dial, in its own FT8 app, before S's opens
            ft8_open(P, "L", F_FT8)
        found = ft8_open(P, "S", F_FT8)
        bench.ensure(S, {"pwr": W0})
        bench.radio_tx_gate(S, P.c, W0)
        P.apps_alike("FT8 open on both" if decode else "FT8 open on the sender")
        auto0, tx0 = found["auto"], found["tx"]
        P.banner(f"FT8 FROM THE REMOTE: TX CALL, CQ {call}, THE PAGE CLOSED, STOP")
        # TX Call both ways
        S.send("ft8 txcall")
        flip = until(S, lambda: S.ft8.get("tx") != tx0, 4)
        S.send("ft8 txcall")
        back = until(S, lambda: S.ft8.get("tx") == tx0, 4)
        if tx0:
            S.send("ft8 txcall")
            S.wait("TX Call off", lambda: not S.ft8.get("tx"), 4)
        P.watch_on(W0, "FT8 CQ 1")
        bench.tx_log({"who": S.name, "f": F_FT8, "w": S.pwr(), "what": "air_c ft8 CQ"})
        S.send("ft8 cq")
        S.wait("CQ", lambda: S.ft8.get("cq") == 1 and S.ft8.get("tx") == 1, 4)
        hz = F_FT8 + int(S.ft8.get("txf") or 1500)
        S.send("ft8 txcall")
        S.wait("TX Call off", lambda: not S.ft8.get("tx") and S.ft8.get("cq") == 1, 4)
        t_h = time.time()
        keyed = False
        while time.time() - t_h < 31:
            keyed = keyed or S.tx or bool(S.ft8.get("sending"))
            time.sleep(0.2)
        quiet_h, _ = heard(P, hz, (t_h - 3.0, t_h), (t_h + 1, t_h + 30), half=120)
        nxt = S.ft8.get("next") or ""
        res["TX Call toggles; off: the CQ queued, nothing sent"] = flip and back and not keyed and "CQ" in nxt.upper()
        said.append(f"'ft8 txcall' turned TX {'on and off' if flip and back else 'NOT both ways'}; with CQ queued "
                    f"({nxt!r}) and TX off, 31 s: keyed {keyed}, the listener {fmt(quiet_h, '{:+.1f}')} dB")
        S.send("ft8 txcall")
        S.wait("TX Call on", lambda: S.ft8.get("tx") == 1, 4)
        t_r = time.time()
        S.wait("the CQ's slot", lambda: S.tx or P.watch.why, 33)
        on1 = time.time()
        S.wait("the CQ's end", lambda: not S.tx or P.watch.why, 17)
        off1 = time.time()
        h1, w1 = heard(P, hz, (on1 - 3.0, on1 - 0.3), (on1 + 1.0, off1 - 0.5), half=120)
        in_slot = (on1 % 15.0) < 2.5
        res["TX Call on: the queued CQ in the next slot, heard"] = on1 - t_r <= 31.5 and in_slot and \
            h1 is not None and h1 >= HEARD_DB
        said.append(f"TX on: the CQ went {on1 - t_r:.1f} s later, {on1 % 15.0:.1f} s into its slot, "
                    f"{off1 - on1:.1f} s; {L.name}: {w1}")
        before = [d for t, d in ft8_lines(S, t_h - 5.0, on1) if d.get("rep") is not None]
        rep_a = before[-1].get("rep") if before else None
        # the page that started it closes; the CQ goes on, a new page sees where the radio is
        stop = P.watch_off()
        time.sleep(1.0)
        old = P.S
        old.close()
        t_new = time.time()
        S = P.S = bench.Radio(old.name, P.c["radios"][old.name], True)
        P.deep(S)
        P.watch_on(W0, "FT8 CQ 2 and 3")                      # watched again before the next CQ's slot (17 s after the first ended)
        time.sleep(4.0)
        seen = ft8_lines(S, t_new)
        S.wait("the second CQ", lambda: S.tx or P.watch.why, 33)
        on2 = time.time()
        tl = ft8_lines(S, t_new, on2)
        S.wait("its end", lambda: not S.tx or P.watch.why, 17)
        off2 = time.time()
        h2, w2 = heard(P, hz, (on2 - 3.0, on2 - 0.3), (on2 + 1.0, off2 - 0.5), half=120)
        res["the page closed: the CQ goes on, a new page sees it"] = bool(seen) and seen[-1][1].get("cq") == 1 and \
            h2 is not None and h2 >= HEARD_DB
        said.append(f"the page closed: a new one saw cq {seen[-1][1].get('cq') if seen else None}, the next CQ went "
                    f"{on2 - off1:.1f} s after the first ended; {L.name}: {w2}")
        # the timeline: once a second, 'in' counting down to the slot, slot 15000, rep one less after each CQ
        ins = []
        for t, d in tl:
            if d.get("in") is None or d.get("in") < 0:
                continue
            if ins and t - ins[-1][0] < 0.3:
                continue        # the new page's greeting and that second's own line (round 2: 0.01 s, 3 ms apart): one
            ins.append((t, d.get("in"), d.get("slot"), d.get("rep")))
        gaps = [round(b[0] - a[0], 2) for a, b in zip(ins, ins[1:])]
        steps = [a[1] - b[1] for a, b in zip(ins, ins[1:])]
        rep_b = ins[-1][3] if ins else None
        # once a second: the steps of 'in' about 1000 ms (a line the link held back or merged makes one of about
        # 2000: one tolerated), never up
        big = [s_ for s_ in steps if s_ > 1500]
        timeline = (len(ins) >= 8 and gaps and 0.8 <= statistics.median(gaps) <= 1.2 and
                    all(400 <= s_ <= 2100 for s_ in steps if s_ > 0) and len(big) <= 1 and
                    sum(1 for s_ in steps if s_ <= 0) <= 1 and
                    all(x[2] == 15000 for x in ins) and rep_a is not None and rep_b == rep_a - 1)
        res["the timeline"] = timeline
        said.append(f"timeline: {len(ins)} lines, a median {statistics.median(gaps) if gaps else None} s apart "
                    f"(gaps {gaps}), 'in' {ins[0][1] if ins else None} -> {ins[-1][1] if ins else None} ms (steps "
                    f"{steps}), slot {sorted({x[2] for x in ins})}, rep {rep_a} before the first CQ, {rep_b} after it")
        # STOP in an over: off the air within a slot, CQ and TX off, Auto as it was ('ft8 stop' needs no control:
        # the new page has none, and none is taken while the test runs)
        S.wait("the third CQ", lambda: S.tx or P.watch.why, 33)
        on3 = time.time()
        time.sleep(3.0)
        t_s = time.time()
        auto_b = S.ft8.get("auto")
        S.send("ft8 stop")
        S.wait("off the air", lambda: not S.tx, 15)
        d_s = time.time() - t_s
        time.sleep(1.0)
        st_ = {k: S.ft8.get(k) for k in ("cq", "tx", "sending", "auto")}
        time.sleep(2.0)
        h3, w3 = heard(P, hz, (t_s + 1.5, t_s + 3.0), (on3 + 1.0, t_s - 0.2), half=120)
        res["STOP in an over"] = d_s <= 2.0 and st_["cq"] == 0 and st_["tx"] == 0 and st_["sending"] == 0 and \
            st_["auto"] == auto_b and h3 is not None and h3 >= HEARD_DB
        said.append(f"'ft8 stop' 3 s into the third CQ: off the air in {d_s:.1f} s, then {st_} (Auto before "
                    f"{auto_b}); {L.name} fell: {w3}")
        if decode:
            rows = [r for r in L.rows if r.get("at", 0) >= on1 and is_cq(r, call)]
            res["the listener's FT8 decoded the CQs"] = len(rows) >= 2
            said.append(f"{L.name}'s FT8 decoded {len(rows)} of the CQs ({rows[-1].get('x') if rows else None!r})")
        else:
            said.append(f"{L.name}'s FT8 not opened: its PSK Reporter is on (a decode would be uploaded)")
        stop = P.watch_off() or stop
    finally:
        S = P.S
        try:
            S.send("ft8 stop")              # no control needed
        except OSError:
            pass
        P.watch_off()
        try:
            # the new page holds no control: taken only when nobody has it (the old page let it go as it closed)
            take_if_free(S, "the FT8 test's end")
        except bench.Refused as e:
            said.append(f"{e}")
            res["control free at the end"] = False
        for host in (S.host, L.host):       # the app's closing cleanups (P.later) put its files back after
            try:
                bench.clean_log(host)
            except (bench.Refused, subprocess.SubprocessError):
                pass
    ok = not stop and bool(res) and all(res.values())
    bad = [k for k, v in res.items() if not v]
    return ok, (f"{proof}; " + "; ".join(said) + (f". Not: {bad}" if bad else "") +
                (f"; STOPPED: {stop}" if stop else ""))


# ======== sentry-lock: the listener's Sentry A on the sender's carrier ========

def log_rows(h):
    """C LOG (/mnt/sentry_log.txt) as [(hz, times, line)], newest first"""
    from suite_cover_b import log_text
    out = []
    for ln in log_text(h).splitlines():
        p = ln.split("\t")
        if len(p) >= 6 and p[0].lstrip("-").isdigit() and p[5].lstrip("-").isdigit():
            out.append((int(p[0]), int(p[5]), ln))
    return out


def radio_now(h):
    return int(sh(h, "date +%s", 20).strip())


def log_saved(h, since, secs=75.0):
    """C LOG's file written at or after since (the radio's clock): the GUI keeps the list in memory and writes it a
    minute at most behind (app_sentry.c: hits_save every 120 ticks), so the file is read only once it caught up
    (round 1 2026-11-23: read at once, it lagged the list, and the put-back's choice went wrong). True when it did"""
    end = time.time() + secs
    while time.time() < end:
        try:
            if int(sh(h, "date -r /mnt/sentry_log.txt +%s 2>/dev/null || echo 0", 20).strip() or 0) >= since:
                return True
        except (bench.Refused, ValueError):
            pass
        time.sleep(3.0)
    return False


def c_log_back(r, h, before, said):
    """C LOG as the test found it: the list caught up in its file first, then each entry the test added taken out with
    C's own F2 once C's choice (s.gsel) reads back on it, the last first (an entry out moves only those under it: the
    ones still to go keep their rows); then the file waited for again and compared. True when it is as before"""
    from suite_cover_b import log_text, pick
    log_saved(h, radio_now(h), 65.0)        # a list changed since its last write is written within a minute
    keep = [ln for ln in before.splitlines() if ln.strip()]
    old = {ln.split("\t")[0] for ln in keep}
    now = [ln for ln in log_text(h).splitlines() if ln.strip()]
    new = [i for i, ln in enumerate(now) if ln.split("\t")[0] not in old]
    if not new:
        return now == keep
    said.append(f"C LOG put back: {len(new)} entries out of {len(now)} ({[now[i].split(chr(9))[0] for i in new]})")
    bench.open_page(r, "C")
    time.sleep(1.0)
    if r.state.get("pg") != "C":
        return False
    n = len(now)
    for i in reversed(new):
        if pick(r, h, "s.gsel", i, n) != i:
            return False
        time.sleep(0.4)
        bench.tap(r, "F2")
        n -= 1
        time.sleep(1.0)
    t = radio_now(h)
    log_saved(h, t)
    return [ln for ln in log_text(h).splitlines() if ln.strip()] == keep


def near(v, hz, within=1500):
    return v is not None and abs(int(v) - hz) <= within


def b_sentry(P):
    import suite_cover_b as cb
    S, L = P.S, P.L
    h = L.host
    why = cb.a_idle(L, h)
    if why:
        return None, f"{L.name}'s A: {why}"
    before = cb.log_text(h)
    old = [hz for hz, _, _ in log_rows(h)]
    if len(old) >= 60:
        return None, f"{L.name}'s C LOG has {len(old)} entries: a new one could push the oldest out; not run"

    def free(x):
        return all(abs(x - o) > 3000 for o in old)
    f1 = next((x for x in (14215000, 14219000, 14211000, 14222000) if free(x)), None)
    scan = [x for x in (14012000, 14018000, 14024000, 14030000, 14036000, 14042000) if free(x)]
    f2 = scan[0] if scan else None
    f3 = next((x for x in scan if f2 and x - f2 >= 6000), None)
    if not (f1 and f2 and f3):
        return None, f"{L.name}'s C LOG has entries near every frequency the test would use; not run"
    st0 = cb.cfg_look(h)
    k0 = cb.style_of(st0["dwell"], st0["stay"], st0["quiet"])
    if k0 != 0:
        return None, (f"{L.name}: SCAN CFG's style as found is {cb.STYLE_NAMES[k0]} (BROWSE needed to put it back); "
                      f"not run")
    proof = P.prove(F, l_mode=2, l_page="A")
    res, said, style_set = {}, [], False

    def carrier(f, secs):
        """the sender's 1000 Hz tone with its dial 1000 Hz under f: a carrier at f, beside the caller. Apart on
        purpose: the dial, mode and filter (the sender in USB-D under the carrier, the listener's A where it looks)
        and the AGC and zoom (A's scan and its stops load each segment's own: round 1 2026-11-23, the listener went
        to AGC 3, zoom 4 under the sender, and the Watch stopped the over); the rest alike"""
        P.retune(f - 1000, 3, W0, apart=("freq", "mode", "filter", "agc", "zoom"),
                 why="sentry-lock: A's scan moves the listener's dial and loads each segment's mode, filter, AGC and "
                     "zoom (the test's subject); the sender's carrier 1000 Hz under each frequency")
        P.watch_on(W0, f"carrier {f / 1e6:.3f}")
        return tone_over(P, 1000, secs, W0)

    def a_scan(on):
        if (cb.gstate(h).get("s.rout", 0) != 0) != on:
            bench.open_page(L, "A")
            bench.tap(L, "F1")
            cb.gwait(h, lambda g: (g.get("s.rout", 0) != 0) == on, 6)

    def lock_at(f, secs):
        t0 = time.time()
        while time.time() - t0 < secs:
            if near(L.state.get("freq"), f):
                return time.time()
            time.sleep(0.1)
        return None

    def push_on(f, label):
        bench.ensure(L, {"freq": F, "mode": 2})
        bg = carrier(f, 9.0)
        until(S, lambda: S.tx, 6)
        time.sleep(2.5)                     # the carrier in A's picture
        t = time.time()
        bench.tap(L, "MFK")
        got = lock_at(f, 3.0)
        (on, off), _ = bg.join()
        stop = P.watch_off()
        r_, w_ = heard_over(P, f, on, off) if on else (None, "no over")
        how = f"locked in {got - t:.1f} s" if got else "NOT locked"
        said.append(f"{label}: the MFK pushed with the carrier at {f / 1e6:.3f} in view: A to "
                    f"{L.state.get('freq')} ({how}); {w_}")
        return bool(got) and not stop and r_ is not None and r_ >= HEARD_DB

    try:
        P.banner("SENTRY A: SCANNER ON A CARRIER, HOLD AND GO ON, THE MFK, C LOG")
        # the scans first, from A's dial as the proof left it (a push locks A on the carrier and holds its dial:
        # round 2 2026-11-23, A's dial would not leave 14.215 for the scan), then BROWSE again, then the MFK
        # SCANNER for the scans (SCAN CFG's F1 STYLE: BROWSE -> SCANNER), put back after
        cb.cfg_open(L, "A")
        bench.tap(L, "F1")
        style_set = True
        x = cb.cfg_wait(h, lambda a: (a["dwell"], a["stay"], a["quiet"]) == cb.STYLES[1], 5)
        cb.cfg_close(L)
        if (x["dwell"], x["stay"], x["quiet"]) != cb.STYLES[1]:
            raise bench.Refused(f"SCAN CFG's style did not go to SCANNER ({x})")
        # 2: SCANNER: A's scan of 20 m stops on the carrier while it talks, goes on about 3 s after it
        # A's dial by the carrier first (its scan begins where it is: from f1 it took 9 s to come round, round 1),
        # then a carrier long enough to be heard talking after the lock (14 s; the watch's limit is 16)
        bench.ensure(L, {"freq": f2 - 2000, "mode": 2})
        bg = carrier(f2, 14.0)
        until(S, lambda: S.tx, 6)
        time.sleep(1.0)
        t_s = time.time()
        a_scan(True)
        t_l = lock_at(f2, 8.0)
        track = []
        while bg.alive():
            track.append(L.state.get("freq"))
            time.sleep(0.1)
        (on, off), _ = bg.join()
        stop = P.watch_off()
        stayed = bool(t_l) and len(track[5:]) >= 20 and all(near(v, f2) for v in track[5:])
        t_go = None
        t_e = time.time()
        while time.time() - t_e < 10:
            if not near(L.state.get("freq"), f2):
                t_go = time.time()
                break
            time.sleep(0.1)
        gone_s = (t_go - off) if t_go and off else None
        a_scan(False)
        r2, w2 = heard_over(P, f2, on, off) if on else (None, "no over")
        res["SCANNER: stops on it while it talks, goes on ~3 s after"] = (not stop and bool(t_l) and stayed and
                                                                          gone_s is not None and 1.5 <= gone_s <= 6.5
                                                                          and r2 is not None and r2 >= HEARD_DB)
        said.append(f"SCANNER, A's scan of 20 m with a carrier at {f2 / 1e6:.3f}: locked "
                    f"{fmt((t_l - t_s) if t_l else None)} s after F1, stayed while it talked {stayed}, went on "
                    f"{fmt(gone_s)} s after it stopped (GO ON AFTER 3 s); {w2}")
        # 3: SCAN CFG's F2 HOLD keeps it past GO ON AFTER; F2 again (GO ON) moves on at once
        try:
            bench.ensure(L, {"freq": f3 - 2000, "mode": 2})
        except bench.Refused as e:          # A's dial held where its scan stopped: the scan goes on from there
            said.append(f"A's dial not set by the carrier for HOLD ({e})")
        bg = carrier(f3, 13.0)
        until(S, lambda: S.tx, 6)
        time.sleep(1.0)
        a_scan(True)
        t_l3 = lock_at(f3, 10.0)
        cb.cfg_open(L, "A")
        bench.tap(L, "F2")                  # HOLD
        (on3, off3), _ = bg.join()
        stop3 = P.watch_off()
        time.sleep(7.0)
        held = near(L.state.get("freq"), f3)
        t_g = time.time()
        bench.tap(L, "F2")                  # GO ON
        moved = None
        while time.time() - t_g < 4.0:
            if not near(L.state.get("freq"), f3):
                moved = time.time() - t_g
                break
            time.sleep(0.1)
        cb.cfg_close(L)
        a_scan(False)
        res["F2 HOLD keeps it, GO ON moves on"] = not stop3 and bool(t_l3) and held and moved is not None
        said.append(f"locked on {f3 / 1e6:.3f} {bool(t_l3)}, F2 HOLD in SCAN CFG: still there 7 s after the carrier "
                    f"ended {held}; F2 GO ON: moved on in {fmt(moved)} s")
        # BROWSE again (F1 round the styles, the dial where little is heard: SAMPLER, STAY and SURVEY stop on signals)
        bench.ensure(L, {"freq": cb.QUIET_HZ, "att": 1})
        cb.cfg_open(L, "A")
        for k in (2, 3, 4, 0):
            bench.tap(L, "F1")
            cb.cfg_wait(h, lambda a, k=k: cb.style_of(a["dwell"], a["stay"], a["quiet"]) == k, 5)
        x = cb.cfg_look(h)
        cb.cfg_close(L)
        style_set = cb.style_of(x["dwell"], x["stay"], x["quiet"]) != 0
        bench.ensure(L, *bench.rx_profile(F, 2, pre=0, att=0, rfg=100, agc=0))
        # 1: the MFK pans A by its KNOB STEP; its push locks on the strongest signal
        st = cb.A_STEPS[cb.gstate(h).get("s.astep", 0) % len(cb.A_STEPS)]
        bench.open_page(L, "A")
        bench.ensure(L, {"freq": F, "mode": 2})
        f0 = int(L.state.get("freq", 0))
        L.send("knob mfk 1")
        until(L, lambda: int(L.state.get("freq", 0)) != f0, 2.5)
        time.sleep(0.4)
        fp = int(L.state.get("freq", 0))
        L.send("knob mfk -1")
        until(L, lambda: int(L.state.get("freq", 0)) != fp, 2.5)
        time.sleep(0.4)
        base = f0 // st * st
        pan = fp in (base + st, base - st)
        said.append(f"A's KNOB STEP {st / 1000:g} kHz: one MFK click {f0 / 1e6:.4f} -> {fp / 1e6:.4f}, back "
                    f"{int(L.state.get('freq', 0)) / 1e6:.4f}")
        res["the MFK pans by A's KNOB STEP, its push locks on the carrier"] = pan and push_on(f1, "push")
        # 4: the same signal again: its C LOG entry counted up and brought to the top
        t4 = radio_now(h)
        again = push_on(f1, "the same carrier pushed again")
        saved = log_saved(h, t4)            # the list in its file (a minute at most behind)
        rows = log_rows(h)
        new = [(hz, n) for hz, n, _ in rows if all(abs(hz - o) > 2500 for o in old)]
        top = [hz for hz, _, _ in rows[:len(new)]]
        # the test's three, in the order they came, newest first; others A's scan stopped on as it went (the
        # X6100's own birdies, round 1: 14.074, 14.0959, 14.0513, 14.1923) said, not judged, and taken out too
        mine = [(hz, n) for hz, n in new if any(near(hz, f, 2000) for f in (f1, f2, f3))]
        order = (saved and len(mine) == 3 and near(mine[0][0], f1, 2000) and mine[0][1] == 2 and
                 near(mine[1][0], f3, 2000) and mine[1][1] == 1 and near(mine[2][0], f2, 2000) and mine[2][1] == 1 and
                 [hz for hz, _ in new] == top)
        res["C LOG: each lock, newest first, the same signal counted"] = again and order
        said.append(f"C LOG's new entries (hz, times), newest first: {new} (file caught up {saved}; the test's: the "
                    f"push's twice, then the HOLD's, then SCANNER's: {order}"
                    + (f"; others A stopped on: {[x for x in new if x not in mine]}" if len(mine) != len(new) else "")
                    + ")")
    except bench.Refused as e:              # said, and the Sentry still put back and checked below
        said.append(f"refused: {e}")
        res["ran through"] = False
    finally:
        try:
            P.watch_off()
            if cb.gstate(h).get("s.rout", 0) != 0:
                a_scan(False)
            if style_set:
                bench.ensure(L, {"freq": cb.QUIET_HZ, "att": 1})
                cb.cfg_open(L, "A")
                for _ in range(6):
                    x = cb.cfg_look(h)
                    k = cb.style_of(x["dwell"], x["stay"], x["quiet"])
                    if k == 0:
                        break
                    bench.tap(L, "F1")      # each step read back before the next (round 2: 0.8 s apart, the file
                    cb.cfg_wait(h, lambda a, k=k: cb.style_of(a["dwell"], a["stay"], a["quiet"]) != k, 6)
                    # behind, it went round past BROWSE and stayed on SAMPLER)
                cb.cfg_close(L)
            logged = c_log_back(L, h, before, said)
        except (bench.Refused, OSError) as e:
            logged = False
            said.append(f"putting the Sentry back: {e}")
    x = cb.cfg_look(h)
    style_back = cb.style_of(x["dwell"], x["stay"], x["quiet"]) == 0
    ok = bool(res) and all(res.values()) and logged and style_back
    return ok, (f"{proof}; " + "; ".join(said) + f"; SCAN CFG's style BROWSE again {style_back}, C LOG as found "
                f"{logged}")


# ======== g-moni: an armed PTT over on G and the TX monitor ========

def b_g_moni(P):
    S = P.S
    mdl = model(P.run, S.name)
    proof = P.prove(F, l_mode=6)
    P.sender(F, 6, W0)
    moni0 = S.state.get("moni")
    P.banner("G: AN ARMED PTT OVER, THE TX MONITOR")
    P.watch_on(W0, "G PTT AM")
    bench.tx_log({"who": S.name, "f": F, "w": S.pwr(), "what": "air_c g-moni"})
    seen = []
    on, off = ptt_over(P, 4.0, int(time.time() * 10) % 10 ** 7,
                       lambda s: seen.append(S.state.get("moni")) if s > 0.8 else None)
    stop = P.watch_off()
    time.sleep(1.5)
    moni1 = S.state.get("moni")
    r_, w_ = heard_over(P, F, on, off) if on else (None, "no over")
    top = max([v for v in seen if v is not None], default=None)
    if mdl == "x6100":
        right = top is not None and top >= 30
        what = f"raised to {top} (30 or more)"
    else:
        right = bool(seen) and all(v == moni0 for v in seen)
        what = f"{'left at' if right else 'CHANGED from'} {moni0} ({sorted(set(seen), key=str)})"
    ok = not stop and on is not None and right and moni1 == moni0 and r_ is not None and r_ >= HEARD_DB
    return ok, (f"{proof}; {mdl}: an armed PTT over on G ({fmt((off - on) if on and off else None)} s): the TX "
                f"monitor {what}, {moni1} after (was {moni0}); {P.L.name}: {w_}" + tail(stop))


# ---- the batch ----

def t_tci_mic(run):
    directions(run, "tci-mic", b_tci_mic, ["tci-trx-radio-mic", "tci-speaker-quiet-during-client-tx"],
               partial=["tci-ask-not-local"])


def t_tci_audio(run):
    directions(run, "tci-audio", b_tci_audio, ["tci-no-rx-audio-during-tx", "tci-tx-sensors", "tci-tx-frame-size-check",
                                               "control-returns-after-key", "tci-leave-unkeys"])


def t_remote_busy(run):
    directions(run, "remote-busy", b_remote_busy, ["panel-tx-broadcast", "control-stays-during-tx",
                                                   "tci-one-tx-at-a-time"], partial=["tci-leave-unkeys"])


def t_tci_cw(run):
    directions(run, "tci-cw", b_tci_cw, ["tci-cw-text-syntax", "tci-cw-stop", "tci-cw-ptt-holder-no-text",
                                         "panel-cw-decoder-text"])


def t_tci_level(run):
    directions(run, "tci-level", b_tci_level,
               lambda s: ["tci-tx-level-x6200"] if model(run, s) == "x6200" else ["tci-tx-level-x6100",
                                                                                  "tx-tci-transmit-level"])


def t_tci_ask(run):
    directions(run, "tci-ask", b_tci_ask, ["tci-ask-allow-once", "tci-ask-off", "tci-ask-not-local"],
               mfk=lambda s, l_: s)


def t_ext_tune(run):
    directions(run, "ext-tune", b_ext_tune, ["panel-ext-tune", "panel-ext-tune-other"])


def t_ft8(run):
    directions(run, "ft8-cq", b_ft8, ["panel-ft8-txcall", "panel-ft8-stop", "ft8-runs-on-radio"],
               partial=["ft8-timeline-state"])


def t_sentry(run):
    directions(run, "sentry-lock", b_sentry, ["sentry-mfk-pan-push", "sentry-a-scan-stops-on-signal", "c-log-entries",
                                              "scancfg-f2-hold-goon"],
               partial=["scancfg-style-behaviour", "scancfg-stay-goon"], mfk=lambda s, l_: l_)


def t_g_moni(run):
    directions(run, "g-moni", b_g_moni, [], partial=["g-voiceprint-tx-x6100"])


TESTS = (("tci-mic", t_tci_mic), ("tci-audio", t_tci_audio), ("remote-busy", t_remote_busy), ("tci-cw", t_tci_cw),
         ("tci-level", t_tci_level), ("tci-ask", t_tci_ask), ("ext-tune", t_ext_tune), ("ft8-cq", t_ft8),
         ("sentry-lock", t_sentry), ("g-moni", t_g_moni))


def batch(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE air_c: {name} (over the cable, 0.1 W through 80 dB: one sends, one listens)")
            fn(run)
