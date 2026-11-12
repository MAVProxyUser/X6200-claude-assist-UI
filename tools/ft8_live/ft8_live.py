#!/usr/bin/env python3
"""
FT8 live: the radio's receiver decoded on this computer, in a web page that shows every message heard so far
and adds each new 15 s slot's messages as they come.

  radio: PulseAudio, 12 kHz mono  --ssh, parec-->  this script: one WAV per slot  -->  ft8_lib's decode_ft8
      -->  ft8_live.jsonl (every message, kept)  -->  http://127.0.0.1:8073 (the history, then live over Server-Sent Events)

Receive only: nothing is sent to the radio. Its dial and mode are read from its TCI server (port 50001, the
websocket-client package) for the page; for decodes the radio must sit on an FT8 frequency in USB or DIGU.
Each slot's messages carry the dial the radio sat on for most of that slot ("retuned": true if it moved during it).

  python3 tools/ft8_live/ft8_live.py [--decoder /path/to/ft8_lib/decode_ft8] (default: bin/decode_ft8 beside this)
      [--radio root@192.168.0.224] [--port 8073] [--log radio_captures/ft8_live.jsonl]
      [--import-wav FILE@2026-11-07T23:58:45Z ...]   (slots recorded before, decoded into the log once)

build_decoder.sh beside this builds the decoder into bin/. The viewer runs while this script runs, nothing more:
it is not installed as a service and does not start by itself.
"""

import argparse
import collections
import json
import math
import os
import queue
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import wave
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SR = 12000          # samples per second asked of PulseAudio: what decode_ft8 reads best
SLOT = 15.0         # FT8's slot
CUT = 14.6          # seconds of each slot decoded: a transmission is 12.64 s, plus its time offset
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

BANDS = [(1800000, 2000000, "160 m"), (3500000, 4000000, "80 m"), (5330000, 5410000, "60 m"),
         (7000000, 7300000, "40 m"), (10100000, 10150000, "30 m"), (14000000, 14350000, "20 m"),
         (18068000, 18168000, "17 m"), (21000000, 21450000, "15 m"), (24890000, 24990000, "12 m"),
         (28000000, 29700000, "10 m"), (50000000, 54000000, "6 m")]


def iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def band_of(hz):
    for lo, hi, name in BANDS:
        if hz and lo <= hz <= hi:
            return name
    return ""


class Cty:
    """Country of a callsign by its prefix, from cty.dat (third-party/cty in this repository)"""

    def __init__(self, path):
        self.exact, self.prefixes = {}, {}
        if not os.path.exists(path):
            return
        entity = None
        for line in open(path, errors="replace"):
            if not line.startswith((" ", "\t")):
                fields = [f.strip() for f in line.split(":")]
                if len(fields) >= 8:
                    entity = fields[0]
                continue
            for tok in line.replace(";", ",").split(","):
                tok = re.sub(r"[\(\[<\{~].*", "", tok.strip())
                if not tok:
                    continue
                if tok.startswith("="):
                    self.exact[tok[1:]] = entity
                else:
                    self.prefixes[tok] = entity

    def country(self, call):
        if call in self.exact:
            return self.exact[call]
        # K1ABC/P, K1ABC/4: the call; 6W/IK2HKT, VP2M/K1ABC: the prefix, where the station is operating
        parts = [p for p in call.split("/") if p and p not in ("P", "M", "MM", "AM", "QRP", "A", "R") and not p.isdigit()]
        short = min(parts, key=len) if len(parts) > 1 else ""
        base = short if short and len(short) <= 4 else (max(parts, key=len) if parts else call)
        for n in range(len(base), 0, -1):
            if base[:n] in self.prefixes:
                return self.prefixes[base[:n]]
        return ""


CALL = re.compile(r"^(?=.*\d)[A-Z0-9/]{3,}$")
GRID = re.compile(r"^[A-R]{2}\d\d$")


def parse_msg(msg):
    """(to, from, grid, cq) of a decoded message: the second callsign is the station transmitting"""
    toks = msg.split()
    to = frm = extra = ""
    cq = bool(toks) and toks[0] == "CQ"
    if cq:
        rest = toks[1:]
        if len(rest) == 3 and not any(c.isdigit() for c in rest[0]):   # CQ DX K1ABC FN42, CQ POTA ...
            rest = rest[1:]
        frm = rest[0] if rest else ""
        extra = rest[1] if len(rest) > 1 else ""
    elif len(toks) >= 2:
        to, frm = toks[0], toks[1]
        extra = toks[2] if len(toks) > 2 else ""
    if frm.startswith("<") or not CALL.match(frm):   # a hashed callsign or free text
        frm = ""
    grid = extra if extra != "RR73" and GRID.match(extra) else ""
    return to, frm, grid, cq


class Store:
    """Every message (the log file, read back at the start), the radio's state, and the pages listening"""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.records = []
        self.subs = []
        self.status = {"audio": "starting", "tci": "starting", "dial": 0, "band": "", "mode": "",
                       "last_slot": "", "last_slot_at": 0, "last_n": 0}
        self.tuning = collections.deque(maxlen=500)   # (time, dial, mode) at each change, to tag slots with
        if os.path.exists(path):
            with open(path) as f:
                for line in f:
                    try:
                        self.records.append(json.loads(line))
                    except ValueError:
                        pass

    def slots(self):
        with self.lock:
            return {r["slot"] for r in self.records}

    def add(self, slot, recs):
        with self.lock:
            self.records.extend(recs)
            with open(self.path, "a") as f:
                for r in recs:
                    f.write(json.dumps(r) + "\n")
        self.publish("slot", {"slot": slot, "records": recs})

    def set_status(self, **kw):
        with self.lock:
            changed = any(self.status.get(k) != v for k, v in kw.items())
            self.status.update(kw)
            snap = dict(self.status)
        if changed:
            self.publish("status", snap)

    def snapshot(self):
        with self.lock:
            return {"records": list(self.records), "status": dict(self.status)}

    def note_tuning(self):
        """Remember when the dial or mode changed, so each slot is tagged with what it was tuned to at the time"""
        with self.lock:
            now = (self.status["dial"], self.status["mode"])
            if not self.tuning or self.tuning[-1][1:] != now:
                self.tuning.append((time.time(),) + now)

    def tuning_during(self, t0, t1):
        """(dial, mode, retuned) of a slot heard from t0 to t1: what the radio sat on for most of it, and whether
        it was retuned during it. Read at decode time instead, a retune just after the slot would tag its messages
        with the new frequency"""
        with self.lock:
            log = list(self.tuning)
            now = (self.status["dial"], self.status["mode"])
        if not log:
            return now + (False,)
        state = next(((d, m) for t, d, m in reversed(log) if t <= t0), log[0][1:])
        held, at = collections.Counter(), t0
        for t, d, m in log:
            if t0 < t < t1:
                held[state] += t - at
                state, at = (d, m), t
        held[state] += t1 - at
        dial, mode = held.most_common(1)[0][0]
        return dial, mode, len(held) > 1

    def publish(self, kind, data):
        with self.lock:
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait((kind, data))
            except queue.Full:
                pass


def decode(decoder, pcm, slot_t, dial, mode, cty):
    """The messages of one slot: pcm is 16-bit mono at SR"""
    fd, path = tempfile.mkstemp(suffix=".wav")
    os.close(fd)
    try:
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm)
        out = subprocess.run([decoder, path], capture_output=True, text=True, timeout=30).stdout
    finally:
        os.unlink(path)
    recs = []
    for line in out.splitlines():
        if "~" not in line:
            continue
        head, _, msg = line.partition("~")
        p = head.split()
        msg = " ".join(msg.split())
        try:
            score, dt, df = float(p[1]), float(p[2]), int(float(p[3]))
        except (IndexError, ValueError):
            continue
        to, frm, grid, cq = parse_msg(msg)
        recs.append({"slot": iso(slot_t), "dial": dial, "band": band_of(dial), "mode": mode, "df": df, "dt": dt,
                     "score": score, "msg": msg, "from": frm, "to": to, "grid": grid, "cq": cq,
                     "country": cty.country(frm) if frm else ""})
    recs.sort(key=lambda r: r["df"])
    return recs


class AudioSlicer(threading.Thread):
    """The radio's receive audio, streamed without a stop over ssh, cut into FT8 slots by this computer's clock.
    Each slot starts where the stream says that time was: the arrival time of the samples less the smallest delay
    seen over the last minute, which follows the radio's sound card clock as it drifts against this one"""

    def __init__(self, radio, slots_q, store):
        super().__init__(daemon=True)
        self.radio, self.slots_q, self.store = radio, slots_q, store
        self.proc = None    # the ssh streaming the audio, ended with this program (stop())

    def stop(self):
        p = self.proc
        if p and p.poll() is None:
            p.kill()

    def run(self):
        while True:
            try:
                self.stream()
            except Exception as e:   # noqa: BLE001 - any failure: report and start over
                self.store.set_status(audio="error: %s" % e)
            self.store.set_status(audio="reconnecting")
            time.sleep(3)

    def stream(self):
        cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=5",
               "-o", "ServerAliveCountMax=3", self.radio,
               "PULSE_RUNTIME_PATH=/var/run/pulse exec parec --device=alsa_input.platform-sound.stereo-fallback"
               " --rate=%d --channels=1 --format=s16le --raw --latency-msec=100" % SR]
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
        self.proc = p
        buf = bytearray()
        base = 0                # sample number of buf's first sample
        nbytes = 0
        offs = collections.deque(maxlen=600)   # arrival time less the stream's length, ~60 s of reads
        next_slot = None
        try:
            while True:
                chunk = p.stdout.read(2400)
                if not chunk:
                    return
                now = time.time()
                buf += chunk
                nbytes += len(chunk)
                total = nbytes // 2
                offs.append(now - total / SR)
                start = min(offs)          # the time of sample 0, plus the smallest delay seen lately
                if next_slot is None:
                    next_slot = math.ceil(start / SLOT + 0.05) * SLOT
                    self.store.set_status(audio="streaming")
                while total >= (next_slot + CUT - start) * SR:
                    i0 = int(round((next_slot - start) * SR))
                    i1 = i0 + int(CUT * SR)
                    lo, hi = max(i0, base), min(i1, total)
                    pcm = bytes(2 * (lo - i0)) + bytes(buf[2 * (lo - base):2 * (hi - base)])
                    self.slots_q.put((next_slot, pcm))
                    next_slot += SLOT
                    keep = max(base, int((next_slot - 1.0 - start) * SR))
                    del buf[:2 * (keep - base)]
                    base = keep
        finally:
            p.kill()
            p.wait()


class Decoder(threading.Thread):
    def __init__(self, decoder, slots_q, store, cty):
        super().__init__(daemon=True)
        self.decoder, self.slots_q, self.store, self.cty = decoder, slots_q, store, cty

    def run(self):
        while True:
            slot_t, pcm = self.slots_q.get()
            dial, mode, retuned = self.store.tuning_during(slot_t, slot_t + CUT)
            try:
                recs = decode(self.decoder, pcm, slot_t, dial, mode, self.cty)
            except Exception as e:   # noqa: BLE001
                self.store.set_status(audio="decoder error: %s" % e)
                continue
            if retuned:     # the dial moved during the slot: tagged with the one it held longest
                for r in recs:
                    r["retuned"] = True
            self.store.add(iso(slot_t), recs)
            self.store.set_status(last_slot=iso(slot_t), last_slot_at=time.time(), last_n=len(recs))


class TciWatch(threading.Thread):
    """The radio's dial and mode from its TCI server. Sends nothing: a client that only listens takes no control"""

    def __init__(self, url, store):
        super().__init__(daemon=True)
        self.url, self.store = url, store

    def run(self):
        try:
            import websocket
        except ImportError:
            self.store.set_status(tci="websocket-client not installed: no dial shown")
            return
        while True:
            try:
                ws = websocket.create_connection(self.url, timeout=10)
                self.store.set_status(tci="connected")
                while True:
                    m = ws.recv()
                    if isinstance(m, bytes):
                        continue
                    for part in m.split(";"):
                        k, _, v = part.partition(":")
                        if k == "vfo":
                            hz = int(v.split(",")[2])
                            self.store.set_status(dial=hz, band=band_of(hz))
                            self.store.note_tuning()
                        elif k == "modulation":
                            self.store.set_status(mode=v.split(",")[1])
                            self.store.note_tuning()
            except Exception as e:   # noqa: BLE001
                self.store.set_status(tci="lost (%s), retrying" % e.__class__.__name__)
                time.sleep(5)


def make_handler(store, page):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                with open(page, "rb") as f:
                    self.send(200, "text/html; charset=utf-8", f.read())
            elif path == "/api/history":
                self.send(200, "application/json", json.dumps(store.snapshot()).encode())
            elif path == "/api/events":
                self.events()
            else:
                self.send(404, "text/plain", b"not found")

        def events(self):
            q = queue.Queue(maxsize=1000)
            with store.lock:
                store.subs.append(q)
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(("event: status\ndata: %s\n\n" % json.dumps(store.snapshot()["status"])).encode())
                self.wfile.flush()
                while True:
                    try:
                        kind, data = q.get(timeout=15)
                        self.wfile.write(("event: %s\ndata: %s\n\n" % (kind, json.dumps(data))).encode())
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with store.lock:
                    store.subs.remove(q)

    return Handler


def main():
    ap = argparse.ArgumentParser(description="FT8 live decodes from the radio, in a web page")
    ap.add_argument("--radio", default="root@192.168.0.224", help="ssh target of the radio")
    ap.add_argument("--tci", default=None, help="TCI WebSocket (default ws://<radio host>:50001)")
    ap.add_argument("--decoder", default=os.path.join(HERE, "bin", "decode_ft8"),
                    help="ft8_lib's decode_ft8 (default: bin/decode_ft8 beside this script, as build_decoder.sh builds it)")
    ap.add_argument("--host", default="127.0.0.1", help="address to serve on (0.0.0.0: the whole network)")
    ap.add_argument("--port", type=int, default=8073)
    ap.add_argument("--log", default=os.path.join(REPO, "radio_captures", "ft8_live.jsonl"))
    ap.add_argument("--cty", default=os.path.join(REPO, "third-party", "cty", "cty.dat"))
    ap.add_argument("--import-wav", action="append", default=[], metavar="FILE@UTC",
                    help="a slot recorded before, decoded into the log once (UTC like 2026-11-07T23:58:45Z)")
    ap.add_argument("--import-dial", type=int, default=14074000, help="the dial of the imported slots")
    a = ap.parse_args()
    if not os.access(a.decoder, os.X_OK):
        sys.exit("ft8_live: no decoder at %s (build_decoder.sh builds one, or give --decoder)" % a.decoder)

    os.makedirs(os.path.dirname(os.path.abspath(a.log)), exist_ok=True)
    store = Store(a.log)
    cty = Cty(a.cty)
    for r in store.records:   # the countries again, from today's lookup: older lines may have had an older one
        if r.get("from"):
            r["country"] = cty.country(r["from"])
    have = store.slots()
    for spec in a.import_wav:
        path, _, when = spec.rpartition("@")
        t = datetime.strptime(when, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()
        if iso(t) in have:
            continue
        with wave.open(path) as w:
            pcm = w.readframes(w.getnframes())
        store.add(iso(t), decode(a.decoder, pcm, t, a.import_dial, "usb", cty))

    slots_q = queue.Queue()
    host = a.radio.split("@")[-1]
    TciWatch(a.tci or "ws://%s:50001" % host, store).start()
    Decoder(a.decoder, slots_q, store, cty).start()
    audio = AudioSlicer(a.radio, slots_q, store)
    audio.start()

    # Stopped (kill, Ctrl-C): the ssh on the radio's audio goes with it, not left streaming
    def quit_now(signum, frame):
        audio.stop()
        os._exit(0)
    signal.signal(signal.SIGTERM, quit_now)
    signal.signal(signal.SIGINT, quit_now)
    srv = ThreadingHTTPServer((a.host, a.port), make_handler(store, os.path.join(HERE, "index.html")))
    srv.daemon_threads = True
    print("FT8 live on http://%s:%d/ (%d messages in %s)" % (a.host, a.port, len(store.records), a.log), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
