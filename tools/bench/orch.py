#!/usr/bin/env python3
"""The two-radio orchestrator (the owner, 2026-11-23): one process holds both radios and runs every test on them as a
pair.

  - Both radios are ALWAYS on the same frequency and mode. The only thing that changes from test to test is the role:
    both listen, the X6100 sends and the X6200 listens, or the other way round (the HackRF feeding both: later).
  - Every step is driven by confirmed status: a setting is sent to both and the test goes on only once both radios'
    own state messages say it (bench.ensure, read back), never after a fixed wait.
  - A sync guard samples both radios' reported frequency and mode all run long (every GUARD_POLL_S); outside a change
    the orchestrator is making to both, any difference that lasts GUARD_S is a violation: the test it happened in
    fails ("out of sync"), and the console shows it.
  - Audio over the air is proved by a stepped sequence of tones (STEP_HZ) the listener must be shown to hear, step by
    step, in order, in its own receiver's audio (TCI); data by what was sent against what was decoded.
  - A console over HTTP (python3 orch.py run ..., then http://127.0.0.1:8765/) shows the run: the test and step, both
    radios live, the sync state, every result with its numbers. Read only.

    python3 orch.py list
    python3 orch.py run [--only NAME,...] [--port 8765] [--lan] [--linger 3600]

Transmissions go through bench.py's gates (the owner's word on the cable, the pads, the power, the listener's supply),
its Watch (stops everything on a radio keyed too long, too much power, a listener hearing too much) and tx_log; the
first over of a session at 0.1 W. Ends with both radios on the Sentry's page A, TX locked, on their last frequency.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import collections
import html
import json
import math
import os
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from bench import Refused, say  # noqa: E402

try:
    import numpy as np
except ImportError:
    np = None

A, B = "x6100", "x6200"
GUARD_POLL_S = 0.2          # the sync guard's look at both radios
GUARD_S = 0.6               # a difference lasting this long outside a change: a violation
W = bench.FIRST_PROBE_W     # every over at 0.1 W
FS = 24000                  # TCI's audio
STEP_HZ = [600, 1000, 1500, 2100, 2400]     # the stepped sequence, each STEP_S, in this order. Its gaps all differ
# (400, 500, 600, 300 Hz): even ones let a run of steps fit the plan shifted by one step as well (AM, 2026-11-23: four
# steps 400 Hz apart read as the plan +400 Hz, the X6200 hearing the X6100 ~380 Hz high anyway)
STEP_S = 0.5
STEP_LEAD_S = 0.3           # silence before the first step (the transmitter's own start)
STEP_TAIL_S = 0.8           # keyed silence after the last: the sender unkeys when its TCI plan ends, and the audio
                            # still in its buffer was lost (the last step missing both ways, 2026-11-23)
STEP_ANCHOR_DB = 15         # a clear step, to find the timing and the offset by
STEP_SNR_DB = 10            # a step's tone over the audio's own noise, at least (the X6100's AM 600 Hz came 15.6 dB over in
                            # 0.5 s windows but under 15 in the 80 ms ones, 2026-11-23: heard, its TX audio tilted)
STEP_SPREAD_HZ = 40         # the steps' offsets from the plan alike within this (the two radios' references differ)
STEP_MAX_OFFSET_HZ = 600    # and none further than this (SSB: the X6100 reads the X6200 150-170 Hz high at 14 MHz,
                            # the X6200 the X6100 ~380 Hz high, 2026-11-23)


# ---- The run's state: what the console shows ----

class Status:
    def __init__(self, tests):
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.phase = "starting"
        self.tests = [{"name": t.name, "role": t.role, "what": t.what, "est_s": t.est_s, "result": None,
                       "says": "", "secs": None, "steps": []} for t in tests]
        self.cur = None             # index into tests
        self.events = collections.deque(maxlen=3000)
        self.log = None             # the run's events.jsonl: every event, kept for the audit
        self.radios = {}
        self.sync = {"ok": True, "since": time.time(), "violations": 0, "last": None, "changing": False}
        self.run_dir = None

    def ev(self, text, kind="info"):
        e = {"t": time.time(), "kind": kind, "text": text,
             "test": self.tests[self.cur]["name"] if self.cur is not None else None}
        with self.lock:
            self.events.append(e)
            if self.log:
                self.log.write(json.dumps(e, default=str) + "\n")
                self.log.flush()
        if kind != "cmd":
            say(text)

    def to_json(self):
        with self.lock:
            done = [t for t in self.tests if t["result"] is not None]
            left = sum(t["est_s"] for t in self.tests if t["result"] is None)
            counts = collections.Counter(t["result"] for t in done)
            return json.dumps({
                "now": time.time(), "t0": self.t0, "phase": self.phase, "cur": self.cur, "tests": self.tests,
                "events": list(self.events)[-400:], "radios": self.radios, "sync": self.sync,
                "counts": {"pass": counts["PASS"], "fail": counts["FAIL"], "look": counts["LOOK"],
                           "notrun": counts["NOT RUN"], "done": len(done), "total": len(self.tests)},
                "eta_s": left, "run_dir": self.run_dir}, default=str)


# ---- The console ----

PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bench console</title><style>
:root{--bg:#0b0b0b;--panel:#161616;--line:#2a2a2a;--ink:#e8e2d0;--dim:#8b8676;--amb:#f2b440;--ok:#5ec27a;--bad:#ff5d5d;--look:#e8c14a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.4 ui-monospace,Menlo,Consolas,monospace}
header{display:flex;flex-wrap:wrap;gap:16px;align-items:baseline;padding:12px 16px;border-bottom:1px solid var(--line)}
h1{font-size:16px;margin:0;color:var(--amb)}.muted{color:var(--dim)}
main{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:12px;padding:12px 16px}
@media(max-width:900px){main{grid-template-columns:minmax(0,1fr)}}
section{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:10px 12px;min-width:0}
h2{font-size:13px;margin:0 0 8px;color:var(--dim);text-transform:uppercase;letter-spacing:.06em}
.radios{display:grid;grid-template-columns:1fr 1fr;gap:10px}.radio{border:1px solid var(--line);border-radius:6px;padding:8px}
.freq{font-size:22px;color:var(--amb)}.tx{color:var(--bad);font-weight:bold}.rx{color:var(--ok)}
.sync{font-size:18px;font-weight:bold;padding:6px 10px;border-radius:4px;display:inline-block}
.sync.ok{background:#173a22;color:var(--ok)}.sync.bad{background:#4a1515;color:var(--bad)}.sync.chg{background:#3a3115;color:var(--look)}
table{width:100%;border-collapse:collapse}td,th{padding:4px 6px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--dim);font-weight:normal}.PASS{color:var(--ok)}.FAIL{color:var(--bad)}.LOOK{color:var(--look)}.NOTRUN{color:var(--dim)}
.cur{background:#211d10}.bar{height:8px;background:var(--line);border-radius:4px;overflow:hidden;margin-top:6px}.bar>div{height:100%;background:var(--amb)}
.ev{max-height:340px;overflow:auto;font-size:12px}.ev div{padding:1px 0;border-bottom:1px solid #1d1d1d}.ev .bad{color:var(--bad)}.ev .ok{color:var(--ok)}
.wide{grid-column:1/-1}.says{color:var(--dim);font-size:12px;word-break:break-word}
.ev .cmd{color:#7d8aa0}.ev .conf{color:#9fd3b0}.ev .radio{color:#d9a35a}label{color:var(--dim);font-size:12px;margin-right:10px}
</style></head><body>
<header><h1>Two-radio bench</h1><span id="phase"></span><span id="counts" class="muted"></span><span id="clock" class="muted"></span></header>
<main>
<section><h2>Radios</h2><div id="sync"></div><div class="radios" id="radios" style="margin-top:8px"></div></section>
<section><h2>Now</h2><div id="now"></div><div class="bar"><div id="prog" style="width:0"></div></div></section>
<section class="wide"><h2>Tests</h2><table><thead><tr><th>#</th><th>test</th><th>role</th><th>result</th><th>secs</th><th>what it found</th></tr></thead><tbody id="tests"></tbody></table></section>
<section class="wide"><h2>Events (everything the orchestrator does; <a href="log" style="color:var(--amb)">the whole log</a>)</h2>
<div><label><input type="checkbox" id="fcmd" checked> commands sent</label><label><input type="checkbox" id="fconf" checked> read-backs</label><label><input type="checkbox" id="fradio" checked> the radios' errors</label></div><div class="ev" id="ev"></div></section>
</main>
<script>
const $=id=>document.getElementById(id);const esc=s=>String(s==null?"":s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const mhz=f=>f?(f/1e6).toFixed(4)+" MHz":"?";const t=s=>new Date(s*1000).toLocaleTimeString();
const MODES={0:"LSB",1:"LSB-D",2:"USB",3:"USB-D",4:"CW",5:"CW-R",6:"AM",7:"NFM",8:"WFM"};
async function tick(){let d;try{d=await (await fetch("status",{cache:"no-store"})).json()}catch(e){$("phase").textContent="console: no answer";return}
$("phase").innerHTML="<b>"+esc(d.phase)+"</b>";const c=d.counts;
$("counts").textContent=`${c.done}/${c.total} done: ${c.pass} pass, ${c.fail} fail, ${c.look} look, ${c.notrun} not run`;
const el=Math.round(d.now-d.t0);$("clock").textContent=`running ${Math.floor(el/60)}m${el%60}s, about ${Math.round(d.eta_s/60)} min left`;
const s=d.sync;let cls=s.changing?"chg":(s.ok?"ok":"bad");
$("sync").innerHTML=`<span class="sync ${cls}">${s.changing?"CHANGING BOTH":(s.ok?"IN SYNC":"OUT OF SYNC")}</span> <span class="muted">violations this run: ${s.violations}${s.last?" (last: "+esc(s.last)+")":""}</span>`;
$("radios").innerHTML=Object.entries(d.radios).map(([n,r])=>`<div class="radio"><b>${n.toUpperCase()}</b> <span class="${r.tx?"tx":"rx"}">${r.tx?"ON THE AIR":"RX"}</span>
<div class="freq">${mhz(r.freq)}</div><div>${MODES[r.mode]||r.mode} &middot; ${r.pwr} W &middot; page ${esc(r.pg)} ${r.rxo===0?"&middot; <span class=tx>TX ARMED</span>":""}</div>
<div class="muted">S ${r.meter==null?"?":r.meter.toFixed(1)+" dBm"} &middot; role ${esc(r.role||"-")} &middot; state ${r.age<2?"live":r.age.toFixed(0)+" s old"}</div></div>`).join("");
const cur=d.cur!=null?d.tests[d.cur]:null;
$("now").innerHTML=cur?`<div><b>${esc(cur.name)}</b> <span class="muted">(${esc(cur.role)})</span></div><div class="says">${esc(cur.what)}</div>`+
 cur.steps.map(st=>`<div class="${st.state==="confirmed"?"PASS":st.state==="failed"?"FAIL":"LOOK"}">${st.state==="running"?"&#9654;":st.state==="confirmed"?"&#10003;":"&#10007;"} ${esc(st.text)} <span class="muted">${st.secs!=null?st.secs.toFixed(1)+" s":""} ${esc(st.detail||"")}</span></div>`).join(""):"<span class=muted>no test running</span>";
$("prog").style.width=(100*c.done/Math.max(1,c.total))+"%";
$("tests").innerHTML=d.tests.map((x,i)=>`<tr class="${i===d.cur?"cur":""}"><td>${i+1}</td><td>${esc(x.name)}</td><td>${esc(x.role)}</td><td class="${(x.result||"").replace(" ","")}">${esc(x.result||(i===d.cur?"running":""))}</td><td>${x.secs!=null?x.secs.toFixed(0):""}</td><td class="says">${esc(x.says)}</td></tr>`).join("");
const show={cmd:$("fcmd").checked,conf:$("fconf").checked,radio:$("fradio").checked};
$("ev").innerHTML=d.events.slice().reverse().filter(e=>show[e.kind]!==false).map(e=>`<div class="${e.kind}">${t(e.t)} ${e.test?"["+esc(e.test)+"] ":""}${esc(e.text)}</div>`).join("");}
tick();setInterval(tick,700);
</script></body></html>"""


def console(status, port, lan):
    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/status"):
                body, ctype = status.to_json().encode(), "application/json"
            elif self.path.startswith("/log"):          # the run's every event, as kept in events.jsonl
                path = os.path.join(status.run_dir or "", "events.jsonl")
                body = open(path, "rb").read() if os.path.exists(path) else b""
                ctype = "text/plain; charset=utf-8"
            elif self.path in ("/", "/index.html"):
                body, ctype = PAGE.encode(), "text/html; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    ThreadingHTTPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer(("0.0.0.0" if lan else "127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ---- The orchestrator ----

class Orch:
    def __init__(self, status):
        self.st, self.c = status, bench.conf()
        self.r = {}
        for n in (A, B):
            self.r[n] = bench.Radio(n, self.c["radios"][n], True)
            self.r[n].take_control()        # only when free or the bench's own (bench.Radio.take_control)
        for n, r in self.r.items():
            self._log_sends(n, r)
        self._errs = {n: len(r.errs) for n, r in self.r.items()}
        self._tx = {n: bool(r.tx) for n, r in self.r.items()}
        self.roles = {A: "RX", B: "RX"}
        self.restore_calls = set()          # the radios given a test call this run (test_calls)
        self.changing = 0                   # changes to both under way: the guard looks away meanwhile
        self.violation = None               # this test's first sync violation
        self._diff_since = None
        self.on = True
        threading.Thread(target=self._guard, daemon=True).start()

    def _log_sends(self, n, r):
        """Every command sent to radio n logged (the console's "commands sent"), its session's own send kept"""
        send = r.send
        st = self.st

        def logged(cmd, send=send):
            if not cmd.startswith(("ping", "banner")):
                st.ev(f"-> {n}: {cmd}", "cmd")
            return send(cmd)
        r.send = logged

    # -- the sync guard --
    def _guard(self):
        while self.on:
            now = time.time()
            rs = {}
            for n, r in self.r.items():
                s = r.state
                rs[n] = {"freq": s.get("freq"), "mode": s.get("mode"), "tx": bool(r.tx), "pwr": s.get("pwr"),
                         "pg": s.get("pg"), "rxo": s.get("rxo"), "meter": r.meter, "role": self.roles.get(n),
                         "age": round(now - r.last_msg, 1)}
            for n, r in self.r.items():             # each radio on and off the air, as its own state says
                if bool(r.tx) != self._tx.get(n):
                    self._tx[n] = bool(r.tx)
                    self.st.ev(f"<- {n} {'ON THE AIR' if r.tx else 'off the air'} ({r.state.get('freq')} Hz mode "
                               f"{r.state.get('mode')}, {r.state.get('pwr')} W set)", "radio")
            for n, r in self.r.items():             # what each radio answered as an error
                errs = list(r.errs)
                for e in errs[self._errs.get(n, 0):]:
                    self.st.ev(f"<- {n} said: err {e}", "radio")
                self._errs[n] = len(errs)
            a, b = rs[A], rs[B]
            differ = (a["freq"], a["mode"]) != (b["freq"], b["mode"])
            with self.st.lock:
                self.st.radios = rs
                self.st.sync["changing"] = self.changing > 0
                if self.changing:
                    self._diff_since = None
                elif differ:
                    if self._diff_since is None:
                        self._diff_since = now
                    elif now - self._diff_since >= GUARD_S and self.st.sync["ok"]:
                        what = f"{A} {a['freq']} mode {a['mode']}, {B} {b['freq']} mode {b['mode']}"
                        self.st.sync.update(ok=False, since=now, last=what)
                        self.st.sync["violations"] += 1
                        self.violation = self.violation or what
                        self.st.events.append({"t": now, "kind": "bad", "text": f"OUT OF SYNC: {what}"})
                else:
                    self._diff_since = None
                    if not self.st.sync["ok"]:
                        self.st.sync.update(ok=True, since=now)
            time.sleep(GUARD_POLL_S)

    # -- steps --
    def step(self, text):
        orch = self

        class Step:
            def __enter__(self):
                self.t0 = time.time()
                self.rec = {"text": text, "state": "running", "secs": None, "detail": ""}
                with orch.st.lock:
                    if orch.st.cur is not None:
                        orch.st.tests[orch.st.cur]["steps"].append(self.rec)
                orch.st.ev(f"  step: {text}")
                return self

            def confirm(self, detail=""):
                self.rec["detail"] = detail

            def __exit__(self, et, e, tb):
                self.rec["secs"] = time.time() - self.t0
                if et is None:
                    self.rec["state"] = "confirmed"
                    orch.st.ev(f"  confirmed: {text}" + (f" ({self.rec['detail']})" if self.rec["detail"] else ""), "ok")
                else:
                    self.rec["state"] = "failed"
                    self.rec["detail"] = str(e)[:300]
                    orch.st.ev(f"  failed: {text}: {e}", "bad")
                return False
        return Step()

    def both(self, want, checks=None, what=None):
        """want set on both radios at once, each confirmed by its own state (bench.ensure), then the two compared:
        any key of want that reads differently on the two is refused"""
        self.changing += 1
        errs = {}
        self.st.ev(f"set on both: {want}", "conf")
        try:
            def one(n):
                try:
                    bench.ensure(self.r[n], want, checks)
                except Refused as e:
                    errs[n] = str(e)
            th = [threading.Thread(target=one, args=(n,)) for n in self.r]
            [t.start() for t in th]
            [t.join(30) for t in th]
            if errs:
                raise Refused("; ".join(errs.values()))
            diff = {k: (bench._value(self.r[A], k), bench._value(self.r[B], k)) for k in want
                    if bench._value(self.r[A], k) != bench._value(self.r[B], k)}
            self.st.ev("read back: " + "; ".join(f"{n} " + ", ".join(f"{k} {bench._value(r, k)}" for k in want)
                                                 for n, r in self.r.items()), "conf")
            if diff:
                raise Refused("the two read differently after the set: " + ", ".join(f"{k} {a}/{b}"
                                                                                     for k, (a, b) in diff.items()))
        finally:
            self.changing -= 1
        return {k: bench._value(self.r[A], k) for k in want}

    def ready(self):
        """Before every test: neither keyed, both transmitters locked, the two on one frequency and mode. What is not
        is put right (both locked, both on page G on the first radio's frequency and mode); what cannot be stops the
        run: no test goes on from radios set wrong"""
        rs = self.r.values()
        bad = []
        if any(r.tx for r in rs):
            bad.append("a radio on the air")
        if any(r.state.get("rxo") == 0 for r in rs):
            bad.append("a transmitter armed")
        if (self.r[A].state.get("freq"), self.r[A].state.get("mode")) != \
                (self.r[B].state.get("freq"), self.r[B].state.get("mode")):
            bad.append("the two apart")
        if not bad:
            self.st.ev("ready: both locked, off the air, on "
                       f"{int(self.r[A].state.get('freq', 0)) / 1e6:.4f} MHz mode {self.r[A].state.get('mode')}", "conf")
            return
        self.st.ev(f"not ready ({', '.join(bad)}): put right first", "bad")
        self.changing += 1
        try:
            for r in rs:
                r.stop()
                bench.close_apps(r)
        finally:
            self.changing -= 1
        self.both_page("G")                 # the Sentry's page first: its A/B is what locks the transmitter
        self.changing += 1
        try:
            for r in rs:
                if r.state.get("rxo") == 0:
                    bench.arm(r, False)
        finally:
            self.changing -= 1
        self.both({"freq": int(self.r[A].state.get("freq") or 14200000), "mode": int(self.r[A].state.get("mode") or 2)})
        if any(r.tx or r.state.get("rxo") == 0 for r in rs):
            raise Refused("the radios could not be made ready (keyed or armed): the run stops")
        with self.st.lock:
            self.st.sync.update(ok=True, since=time.time())
        self.st.ev("ready after putting right: both locked, alike", "ok")

    def both_page(self, letter):
        self.changing += 1
        try:
            th = [threading.Thread(target=bench.open_page, args=(r, letter)) for r in self.r.values()]
            [t.start() for t in th]
            [t.join(20) for t in th]
            bad = [n for n, r in self.r.items() if r.state.get("pg") != letter]
            if bad:
                raise Refused(f"{', '.join(bad)} not on the Sentry's page {letter}")
        finally:
            self.changing -= 1

    def close(self):
        import subprocess
        for n in sorted(self.restore_calls):     # the own calls back (each GUI restarts)
            self.st.ev(f"{n}: its own call back, PSK Reporter as found: its GUI restarts")
            subprocess.run([sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"),
                            "testcall", n, "restore"], capture_output=True, timeout=120)
            t0 = time.time()
            while time.time() - t0 < 90:
                try:
                    self.r[n].reconnect()
                    break
                except (Refused, OSError):
                    time.sleep(3)
        for r in self.r.values():
            try:
                r.stop()
            except OSError:
                pass
        # the last test's frequency and mode, the same on both (they were, on G): page A brings back each radio's own
        # A dial (each different: the audit, 2026-11-23, why they were never alike after a test), so A's dial is set
        # to it on both and confirmed
        last = {k: self.r[A].state.get(k) for k in ("freq", "mode")}
        for n, r in self.r.items():
            try:
                if r.state.get("rxo") == 0:
                    bench.arm(r, False)
                bench.close_apps(r)
            except (Refused, OSError) as e:
                self.st.ev(f"{n}: {e}", "bad")
        try:
            self.both_page("A")
            if None not in last.values():
                self.both({"freq": int(last["freq"]), "mode": int(last["mode"])})
            self.st.ev(f"both on the Sentry's page A on {int(last['freq']) / 1e6:.4f} MHz mode {last['mode']}, "
                       f"confirmed alike", "ok")
        except Refused as e:
            self.st.ev(f"the end: {e}", "bad")
        time.sleep(1.0)                     # the guard's last look at the two
        self.on = False
        for r in self.r.values():
            r.close()


# ---- The proofs ----

def step_plan():
    return [[0, STEP_LEAD_S]] + [[hz, STEP_S] for hz in STEP_HZ] + [[0, STEP_TAIL_S]]


def find_steps(x, fs=FS):
    """The stepped sequence in audio x, in two passes. First the clear steps: per 20 ms hop (80 ms window) the strongest
    frequency in 300-3000 Hz, runs of one frequency (within 40 Hz) at least STEP_S * 0.6 long and STEP_ANCHOR_DB over
    the band's median; two or more consecutive runs that are consecutive steps of the plan at one offset (within
    STEP_SPREAD_HZ, at most STEP_MAX_OFFSET_HZ) give the offset and the time each step began. Then every step in its own
    window (STEP_S, its edges trimmed): its tone (plan + offset, within 30 Hz) must be the strongest thing in the band
    there and STEP_SNR_DB over the band's median. Returns (found, offset Hz, [per step: (heard Hz, SNR dB, seconds)],
    the runs the first pass saw)"""
    hop, win = int(0.02 * fs), int(0.08 * fs)
    if np is None or len(x) < win * 4:
        return False, None, [], []
    w = np.hanning(win)
    fr = np.fft.rfftfreq(8192, 1 / fs)
    band = (fr >= 300) & (fr <= 3000)
    frames = []
    for i in range(0, len(x) - win, hop):
        X = np.abs(np.fft.rfft(x[i:i + win] * w, 8192))[band]
        k = int(np.argmax(X))
        snr = 20 * math.log10((X[k] + 1e-12) / (np.median(X) + 1e-12))
        frames.append(fr[band][k] if snr >= STEP_ANCHOR_DB else None)
    runs, cur = [], None
    for j, f in enumerate(frames + [None]):
        if f is not None and cur is not None and abs(f - cur["f"][-1]) < 40:
            cur["f"].append(f)
            continue
        if cur and len(cur["f"]) * hop / fs >= STEP_S * 0.6:
            runs.append({"f": float(np.median(cur["f"])), "t": cur["j"] * hop / fs, "s": len(cur["f"]) * hop / fs})
        cur = {"f": [f], "j": j} if f is not None else None
    seen = [(round(r["f"]), round(r["s"], 2)) for r in runs]
    n, best = len(STEP_HZ), None
    for i, r in enumerate(runs):
        for k0 in range(n):
            off = r["f"] - STEP_HZ[k0]
            if abs(off) > STEP_MAX_OFFSET_HZ:
                continue
            m = 1
            while i + m < len(runs) and k0 + m < n and abs(runs[i + m]["f"] - STEP_HZ[k0 + m] - off) <= STEP_SPREAD_HZ:
                m += 1
            if m >= 2 and (best is None or m > best[0] or (m == best[0] and abs(off) < abs(best[3]))):
                best = (m, k0, r["t"], off)     # the longest; of two as long, the nearer the plan
    if best is None:
        return False, None, [], seen
    _, k0, t_k0, off = best
    nfft = 16384
    fr2 = np.fft.rfftfreq(nfft, 1 / fs)
    band2 = (fr2 >= 300) & (fr2 <= 3000)
    steps, ok = [], True
    for k, hz in enumerate(STEP_HZ):
        a = int((t_k0 + (k - k0) * STEP_S + 0.08) * fs)
        b = int((t_k0 + (k - k0 + 1) * STEP_S - 0.08) * fs)
        if a < 0 or b > len(x) or b - a < fs // 10:
            steps.append((None, None, 0.0))
            ok = False
            continue
        seg = x[a:b] * np.hanning(b - a)
        X = np.abs(np.fft.rfft(seg, nfft))
        near = (fr2 >= hz + off - 30) & (fr2 <= hz + off + 30)
        tone = float(X[near].max())
        snr = 20 * math.log10((tone + 1e-12) / (float(np.median(X[band2])) + 1e-12))
        top = float(X[band2].max())
        heard_hz = round(float(fr2[near][int(np.argmax(X[near]))]))
        steps.append((heard_hz, round(snr, 1), round((b - a) / fs, 2)))
        if snr < STEP_SNR_DB or tone < top * 0.999:     # not loud enough, or something else louder there
            ok = False
    return ok, round(off), steps, seen


def save_wav(o, label, x):
    """The listener's audio of a test, kept in the run's folder as the evidence of what it heard"""
    import wave
    path = os.path.join(o.st.run_dir, "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in label) + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
    return path


def over(o, sender, listener, f, mode, label, body, w=W, agc=0, pages=True):
    """One transmission from sender to listener, both on f in mode (set alike and confirmed first): the gates, the
    Watch, the transmitter armed for it and locked after; body(watch) sends. Returns the Watch (closed)"""
    S, L = o.r[sender], o.r[listener]
    p, total = bench.chain_gate(o.c, "radios")
    with o.step(f"both on {f / 1e6:.4f} MHz mode {mode}, alike"):
        if not pages:                       # the listener in an app that holds its dial: the sender put on it
            o.both({"freq": f, "mode": mode, "pwr": w, "atu": 0, "xit": 0, "split": 0, "rit": 0})
    with o.step(f"both on page G, {f / 1e6:.4f} MHz mode {mode}, alike") if pages else o.step("(the app's page kept)"):
        if pages:
            o.both_page("G")
        # the same filter on both in SSB (each radio's own differed: the X6100 300-2700, the X6200 100-3000; a step
        # heard 380 Hz high went past the X6100's top); AM and FM keep their own, a width of their own kind
        if pages:
            want, checks = bench.rx_profile(f, mode, pre=0, att=0, rfg=100, agc=agc, sql=0,
                                            filt=(100, 3000) if mode in (0, 1, 2, 3) else None)
            o.both(dict(want, pwr=w, atu=0, xit=0), checks)
    with o.step(f"{sender} may send, {listener} may listen") as s:
        bench.ensure(S, *bench.tx_profile(f, mode, w))
        bench.radio_tx_gate(S, o.c, w)
        sup = bench.listener_gate(L)
        s.confirm(f"{listener}'s supply {sup}" if sup else "")
    o.roles = {sender: "TX", listener: "RX"}
    watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(w * 1000) - total, listener=L, w=w, label=label)
    try:
        with o.step(f"{sender}'s transmitter armed (the Sentry's A/B), confirmed"):
            o.changing += 1                 # arming puts a data mode into voice and back (bench.arm): not a slip
            try:
                bench.arm(S, True, mode=mode)
            finally:
                o.changing -= 1
        bench.tx_log({"who": sender, "f": f, "w": S.pwr(), "what": f"orch: {label}"})
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
            o.st.ev(f"{sender}: TX not locked again: {e}", "bad")
        o.roles = {A: "RX", B: "RX"}
    o.st.ev(f"over {label}: {sender}'s meter at most {watch.tx_peak.get(sender)} W, {listener}'s S-meter at most "
            f"{watch.rx_peak} dBm, heard {watch.heard_db.get(sender) if hasattr(watch, 'heard_db') else None} dB, "
            f"deaf overs {len(watch.deaf)}" + (f", stopped: {watch.why}" if watch.why else ""), "conf")
    if watch.why:
        raise Refused(f"the watch stopped the over: {watch.why}")
    return watch


def audio_steps(o, sender, listener, f, mode, label):
    """The stepped sequence sent by sender's TCI on f in mode, recorded from listener's own receiver (its TCI audio),
    found there step by step in order: (ok, says, data)"""
    from tones import TciRx
    L = o.r[listener]
    rx = TciRx(L.host)
    got = {}
    try:
        def body(watch):
            with o.step(f"{sender} sends {len(STEP_HZ)} tones ({', '.join(map(str, STEP_HZ))} Hz, {STEP_S} s each)"):
                out, t_on, t_off = bench.tci_send(o.r[sender], step_plan(), watch=watch)
                got["t"] = (t_on, t_off)
                if watch.why:
                    raise Refused(watch.why)
                if not watch.tx_peak.get(sender):
                    raise Refused(f"{sender}'s meter showed no power")
        over(o, sender, listener, f, mode, label, body)
        time.sleep(1.0)                     # the audio's last frames
        with o.step(f"{listener} heard every step, in order") as s:
            t_on, t_off = got["t"]
            x, _ = rx.audio(t_on - 0.5, t_off + 1.5)
            if len(x) < FS:
                raise Refused(f"{listener}'s TCI sent {len(x) / FS:.1f} s of audio")
            found, off, steps, seen = find_steps(x)
            got.update(found=found, offset=off, steps=steps, seen=seen, wav=save_wav(o, label, x))
            if not found:
                raise Refused(f"the sequence not heard whole in {listener}'s audio: per step (Hz, dB over the noise, "
                              f"s) {steps or 'no anchor'}; clear tones (Hz, s) {seen[:8]}")
            spread = max(d for _, d, _ in steps) - min(d for _, d, _ in steps)
            got["spread_db"] = round(spread, 1)
            s.confirm(f"offset {off:+d} Hz; " + ", ".join(f"{h} Hz {d} dB" for h, d, _ in steps)
                      + f"; spread {spread:.1f} dB")
    except Refused as e:
        return False, str(e), got
    finally:
        rx.close()
    return True, (f"all {len(STEP_HZ)} steps heard in order, offset {got['offset']:+d} Hz, weakest "
                  f"{min(d for _, d, _ in got['steps'])} dB over the audio's noise, spread {got['spread_db']} dB "
                  f"across the steps"), got


CW_TEXT = "VVV TEST DE Q1TST"
CW_MUST = "TEST DE Q1TST"


def lev(a, b):
    """Levenshtein distance"""
    d = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, d[0] = d[0], i
        for j, cb in enumerate(b, 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (ca != cb))
    return d[len(b)]


def cw_text(o, sender, listener, f, label):
    """CW_TEXT keyed by sender's own keyer (its TCI) on f, both in CW; the listener's decoder (the remote's cw lines):
    PASS when CW_MUST is in what it printed (spaces as they come), its character error rate against CW_TEXT said"""
    import subprocess
    L, S = o.r[listener], o.r[sender]
    got = {}

    def body(watch):
        with o.step(f"{sender} keys '{CW_TEXT}' at its KEY Speed"):
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "tci_cw_tx.py"), f"root@{S.host}:/tmp/tci_cw_tx.py"], check=True)
            L.cw = ""
            proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                     f"python3 /tmp/tci_cw_tx.py '{CW_TEXT}' 14"], stdout=subprocess.PIPE, text=True)
            while proc.poll() is None:
                if watch.why:
                    S.stop()
                    raise Refused(watch.why)
                time.sleep(0.05)
            if not watch.tx_peak.get(sender):
                raise Refused(f"{sender}'s meter showed no power")
    try:
        # AGC fast on both: with it off a signal 60 dB up overloaded the X6100's audio and its decoder printed garbage
        # (2026-11-23; cw_diag, which copied, had it fast)
        over(o, sender, listener, f, 4, label, body, agc=2)
        with o.step(f"{listener}'s decoder printed the text") as s:
            t0, last = time.time(), None
            while time.time() - t0 < 6:             # the decoder's last characters: settled when 1.5 s unchanged
                if L.cw != last:
                    last, t_last = L.cw, time.time()
                elif time.time() - t_last > 1.5:
                    break
                time.sleep(0.1)
            text = " ".join(L.cw.split()).upper()
            cer = lev(text.replace(" ", ""), CW_TEXT.replace(" ", "")) / len(CW_TEXT.replace(" ", ""))
            got.update(decoded=text, cer=round(cer, 3))
            if CW_MUST not in text:
                raise Refused(f"{listener} decoded {text!r} ({cer:.0%} of the characters wrong): {CW_MUST!r} not in it")
            s.confirm(f"{text!r}, {cer:.0%} of the characters wrong")
    except Refused as e:
        return False, str(e), got
    return True, f"decoded {got['decoded']!r} ({got['cer']:.0%} of the characters wrong)", got


POWER_STEPS = [0.1, 0.3, 1.0]       # W: the listener's level must rise 10*log10 of each ratio
POWER_TOL_DB = 2.0


def power_steps(o, sender, listener, f, label):
    """A 1000 Hz tone 2 s at each of POWER_STEPS (USB, one over each): the listener's spectrum at the tone, between the
    moments the sender's own state said it went on and off the air, must rise by the power's ratio in dB, within
    POWER_TOL_DB (the spectrum is the receiver's before its AGC; RF gain fixed)"""
    levels, got = [], {}
    S, L = o.r[sender], o.r[listener]
    try:
        for w in POWER_STEPS:
            peak = {}

            def body(watch, w=w):
                with o.step(f"{sender} sends 1000 Hz 2 s at {w} W"):
                    bench.tci_send(S, [[0, 0.3], [1000, 2.0], [0, STEP_TAIL_S]], watch=watch)
                    if watch.why:
                        raise Refused(watch.why)
                    peak["w"] = watch.tx_peak.get(sender)
            over(o, sender, listener, f, 2, f"{label} {w} W", body, w=w)
            with o.step(f"{listener}'s level while {sender} said it was on the air") as s:
                if not S.tx_last:
                    raise Refused(f"{sender} never said it was on the air")
                on, off = S.tx_last
                lvl, n = bench.spectrum_level(L, f + 1000, on + 0.6, off - 0.4, half=300)
                if lvl is None or n < 3:
                    raise Refused(f"{listener} sent {n} spectrum frames covering {f + 1000} Hz while {sender} was on "
                                  f"the air ({off - on:.1f} s)")
                levels.append((w, round(lvl, 1), peak.get("w")))
                s.confirm(f"{lvl:.1f} dB over {n} frames, {off - on:.1f} s on the air")
        with o.step(f"{listener}'s level rose with the power") as s:
            got["levels"] = levels
            bad = []
            for (w0, l0, _), (w1, l1, _) in zip(levels, levels[1:]):
                want = 10 * math.log10(w1 / w0)
                if abs((l1 - l0) - want) > POWER_TOL_DB:
                    bad.append(f"{w0}->{w1} W: {l1 - l0:+.1f} dB (want {want:+.1f})")
            say_lv = ", ".join(f"{w} W {l} dB (meter {m and round(m, 2)} W)" for w, l, m in levels)
            if bad:
                raise Refused(f"levels {say_lv}; off by more than {POWER_TOL_DB} dB: {'; '.join(bad)}")
            s.confirm(say_lv)
    except Refused as e:
        return False, str(e), got
    finally:
        o.both({"pwr": W})                  # the power back at 0.1 W on both
    return True, f"the listener's level followed the power: " + ", ".join(f"{w} W {l} dB" for w, l, _ in levels), got


def ptt_refused(o, radio, other, f, arm, label):
    """PTT on radio with its transmitter locked (arm False) or armed outside the amateur bands (f): it must stay off
    the air, and the other radio, on the same frequency, must hear nothing"""
    R, X = o.r[radio], o.r[other]
    with o.step(f"both on {f / 1e6:.3f} MHz USB, alike"):
        o.both_page("G")
        o.both({"freq": f, "mode": 2, "pwr": W})
    with o.step(f"{radio}'s transmitter {'armed' if arm else 'locked'}"):
        o.changing += 1
        try:
            bench.arm(R, arm)
        finally:
            o.changing -= 1
    went, t0 = False, time.time()
    quiet = X.meter
    try:
        with o.step(f"PTT pressed on {radio}: it must stay off the air 2.5 s") as s:
            R.send("key PTT 1 9")
            while time.time() - t0 < 2.5:
                if R.tx:
                    went = True
                    break
                time.sleep(0.05)
            R.send("key PTT 0")
            if went:
                raise Refused(f"{radio} WENT ON THE AIR")
            s.confirm(f"{other}'s S-meter {quiet} -> {X.meter} dBm")
    finally:
        R.send("key PTT 0")
        R.stop()
        if arm:
            bench.arm(R, False)
    return (not went), f"PTT on {radio} ({'armed out of the amateur bands' if arm else 'transmitter locked'}): " + \
        ("stayed off the air" if not went else "WENT ON THE AIR"), {}


TEST_CALLS = {A: "Q1TST", B: "Q2TST"}


def test_calls(o):
    """Both radios on their test calls (bench.py testcall: PSK Reporter off with it, the own call kept to put back;
    each GUI restarted to read it), the sessions connected again. Once a run; close() puts the own calls back"""
    import subprocess
    if getattr(o, "calls_set", False):
        return
    o.changing += 1
    try:
        for n, call in TEST_CALLS.items():
            cur = bench.ssh(o.r[n].host, "sqlite3 -readonly /mnt/params.db \"select val from params where "
                                         "name='callsign'\"").strip()
            if cur == call:
                continue
            o.st.ev(f"{n}: test call {call} set, PSK Reporter off: its GUI restarts")
            subprocess.run([sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"),
                            "testcall", n, call], check=True, capture_output=True, timeout=120)
            o.restore_calls.add(n)
        for n in o.restore_calls:
            t0 = time.time()
            while True:
                try:
                    o.r[n].reconnect()
                    break
                except (Refused, OSError):
                    if time.time() - t0 > 90:
                        raise Refused(f"{n}: its GUI did not come back after the test call")
                    time.sleep(3)
    finally:
        o.changing -= 1
    o.calls_set = True


def mask(m):
    """An FT8 message as the console may show it: the third word (a locator) hidden"""
    w = m.split()
    return " ".join(w[:2] + ["****"] * (len(w) > 2) + w[3:])


def ft8_cq(o, sender, listener, label, dial=14074000):
    """The FT8 app on both on dial (USB-D), the sender's CQ (its test call) in the next slot at 0.1 W: the listener
    must decode exactly the message the sender said it sent ("next" in its FT8 state)"""
    S, L = o.r[sender], o.r[listener]
    got = {}
    try:
        with o.step("both on their test calls (PSK Reporter off)"):
            test_calls(o)
        with o.step(f"the FT8 app open on both, on {dial / 1e6:.3f} MHz"):
            o.changing += 1
            try:
                for r in (S, L):
                    r.send("ft8 open")
                for r in (S, L):
                    r.wait("the FT8 app", lambda r=r: r.ft8.get("open") == 1, 10)
                    time.sleep(1.0)
                    for _ in range(3):              # it opens in the protocol it was left in (the X6100's in FT4):
                        if r.state.get("app") == "FT8":     # its MODE steps FT8, FT4, FT2 round
                            break
                        p = r.ft8.get("proto")
                        r.send("ft8 mode")
                        r.wait("FT8's protocol stepped", lambda r=r, p=p: r.ft8.get("proto") != p, 5)
                        time.sleep(0.8)
                    if r.state.get("app") != "FT8":
                        raise Refused(f"{r.name}: its FT8 app not in FT8 ({r.state.get('app')})")
                th = [threading.Thread(target=bench.ft8_to_band, args=(r, dial)) for r in (S, L)]
                [t.start() for t in th]
                [t.join(40) for t in th]
                for r in (S, L):
                    if r.ft8.get("auto"):
                        r.send("ft8 auto")
                        r.wait("Auto off", lambda r=r: not r.ft8.get("auto"), 5)
            finally:
                o.changing -= 1
            o.both({"freq": dial, "pwr": W})
            if int(S.state.get("mode", -1)) != int(L.state.get("mode", -2)):
                raise Refused(f"the two in different modes: {S.state.get('mode')}/{L.state.get('mode')}")
        p, total = bench.chain_gate(o.c, "radios")
        with o.step(f"{sender} may send, {listener} may listen"):
            bench.radio_tx_gate(S, o.c, W)
            bench.listener_gate(L)
        o.roles = {sender: "TX", listener: "RX"}
        watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W, label=label)
        try:
            with o.step(f"{sender} calls CQ in the next slot") as s:
                n0 = len(L.rows)
                S.send("ft8 cq")
                S.wait("its transmission", lambda: S.tx or watch.why, 32)
                sent = bench.norm(S.ft8.get("next") or "")
                bench.tx_log({"who": sender, "f": dial, "w": S.pwr(), "what": f"orch: {label}"})
                S.wait("the end of its transmission", lambda: not S.tx or watch.why, bench.MAX_KEY_S + 2)
                S.send("ft8 stop")
                if watch.why:
                    raise Refused(watch.why)
                got["sent"] = mask(sent)
                s.confirm(f"sent {mask(sent)!r}")
        finally:
            watch.close()
            S.send("ft8 stop")
            o.roles = {A: "RX", B: "RX"}
        with o.step(f"{listener} decoded exactly that") as s:
            try:
                L.wait("the decode", lambda: any(bench.msg(r) == sent for r in L.rows[n0:]), 8)
            except Refused:
                heard = [mask(bench.msg(r)) for r in L.rows[n0:]]
                raise Refused(f"{listener} decoded {heard or 'nothing'}, not {mask(sent)!r}")
            row = next(r for r in L.rows[n0:] if bench.msg(r) == sent)
            got.update(decoded=mask(bench.msg(row)), snr=row.get("snr"))
            s.confirm(f"{mask(bench.msg(row))!r} at {row.get('snr')} dB")
    except Refused as e:
        return False, str(e), got
    finally:
        for r in (S, L):
            r.send("ft8 close")
    return True, f"sent {got['sent']!r}, decoded the same at {got['snr']} dB", got


def hello(r):
    """The GUI's version from the remote's first line ("hello <version> <boot>")"""
    for _, x in list(r.lines):
        if x.startswith("hello "):
            return " ".join(x.split()[1:-1]) or x.split()[1]     # "hello <name> <version> <boot>": all but the boot
    return None


def t_identity(o):
    """Both radios run the same GUI and offer the same apps, and neither's Sentry A runs a routine of its own (it
    would retune A away from the other)"""
    with o.step("the GUI's version, the apps and A's routine on both") as s:
        for r in o.r.values():
            if r.apps is None:
                r.send("apps")
                r.wait("its apps", lambda r=r: r.apps is not None, 5)
        v = {n: hello(r) for n, r in o.r.items()}
        apps = {n: sorted(a[1] for a in (r.apps or [])) for n, r in o.r.items()}
        rout = {n: bench.ssh(r.host, "grep -h '^s.rout=' /mnt/gui_state.conf 2>/dev/null | tail -1").strip()
                for n, r in o.r.items()}
        bad = []
        if len(set(v.values())) != 1 or None in v.values():
            bad.append(f"GUI versions {v}")
        if apps[A] != apps[B]:
            bad.append(f"apps only on one: {sorted(set(apps[A]) ^ set(apps[B]))}")
        busy = [n for n, x in rout.items() if x and x != "s.rout=0"]
        if busy:
            bad.append(f"A's routine on: {busy} ({rout})")
        if bad:
            raise Refused("; ".join(bad))
        s.confirm(f"GUI {v[A]}, {len(apps[A])} apps alike, A's routine off")
    return True, f"same GUI ({v[A]}) and {len(apps[A])} apps on both; A's routine off on both", {"gui": v}


def t_services(o):
    """The ports and pages each radio must answer, and the two it must not (50003/50004, closed since the HTTPS proxy)"""
    import socket
    from suite_proto import http
    found = {}
    with o.step("ports and pages on both") as s:
        bad = []
        for n, r in o.r.items():
            ports = {}
            for port in (22, 80, 443, 4532, 50001, 50002, 50003, 50004):
                try:
                    socket.create_connection((r.host, port), timeout=3).close()
                    ports[port] = True
                except OSError:
                    ports[port] = False
            page = http(r.host, "/")[0]
            disp = http(r.host, "/api/display")[0]
            found[n] = {"ports": ports, "/": page, "/api/display": disp}
            miss = [p for p in (22, 80, 443, 4532, 50001, 50002) if not ports[p]]
            open_ = [p for p in (50003, 50004) if ports[p]]
            if miss or open_ or page != 200 or disp != 200:
                bad.append(f"{n}: not answering {miss}, open {open_}, / {page}, /api/display {disp}")
        if bad:
            raise Refused("; ".join(bad))
        s.confirm("22, 80, 443, 4532, 50001, 50002 answer and 50003/50004 closed on both; / and /api/display 200")
    return True, "every service answers on both radios, the closed ports closed", found


def t_web_secrets(o):
    """The Files page never hands out the HTTPS private key, the backups or the remote's sessions (their tokens):
    each asked for by name (HEAD, nothing fetched) must be 403"""
    from suite_proto import http
    want = ("https/key.pem", "backups/backup.tgz", "remote_sessions.conf")
    got, bad = {}, []
    with o.step("the secrets asked for by name on both (HEAD only)") as s:
        for n, r in o.r.items():
            got[n] = {f: http(r.host, "/files/" + f, method="HEAD")[0] for f in want}
            served = [f for f, st in got[n].items() if st == 200]
            if served:
                bad.append(f"{n} SERVES {served}")
        if bad:
            raise Refused("; ".join(bad) + " (fixed in the repo: 3906e452 key.pem, 7b01807b sessions; the radios run "
                                           "an image older than both)")
        s.confirm(str(got))
    return True, "the key, the backups and the sessions refused (403) on both", got


RX_TRIES = [("att", 1), ("att", 0), ("pre", 1), ("pre", 0), ("agc", 1), ("agc", 2), ("agc", 3), ("agc", 0),
            ("nb", 1), ("nb", 0), ("nr", 1), ("nr", 0), ("sql", 20), ("sql", 0), ("rfg", 50), ("rfg", 100)]


def t_rx_settings(o):
    """The receiver's settings, each set on both radios at once, read back on each and alike"""
    with o.step("both on 14.200 MHz USB"):
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
    for k, v in RX_TRIES:
        with o.step(f"{k} {v} on both"):
            o.both({k: v})
    return True, f"{len(RX_TRIES)} settings ({', '.join(sorted({k for k, _ in RX_TRIES}))}), each confirmed on both " \
                 f"and alike", {}


def ptt_lease(o, radio, other, label):
    """One PTT on radio, armed on 14.200 USB at 0.1 W, never renewed: off the air within 4 s (the lease, 3 s); the
    other radio on the same frequency"""
    R = o.r[radio]
    res = {}

    def body(watch):
        with o.step(f"one PTT on {radio}, never renewed: off by itself within 4 s") as s:
            t0 = time.time()
            R.send("key PTT 1 7")
            on = off = None
            while time.time() - t0 < 6:
                if R.tx and on is None:
                    on = time.time() - t0
                if on is not None and not R.tx:
                    off = time.time() - t0
                    break
                time.sleep(0.05)
            R.send("key PTT 0")
            res.update(on=on, off=off)
            if on is None:
                raise Refused(f"{radio} never went on the air")
            if off is None or off > 4.0:
                raise Refused(f"{radio} still on the air {off or '6+'} s after the one PTT")
            s.confirm(f"on at {on:.1f} s, off at {off:.1f} s")
    try:
        over(o, radio, other, 14200000, 2, label, body)
    except Refused as e:
        return False, str(e), res
    return True, f"on the air at {res['on']:.1f} s, off by itself at {res['off']:.1f} s (the lease: 3 s)", res


def tune_carrier(o, sender, listener, label, f=14200000):
    """TUNE at 0.1 W (tpwr) on f USB: its lease renewed 3 s, then let lapse. The sender on the air with tune 1; the
    listener's spectrum within 3 kHz of f up >= 15 dB over its quiet; off by itself 2.5-5 s after the last renewal"""
    S, L = o.r[sender], o.r[listener]
    res = {}
    with o.step("TUNE's power 0.1 W on both"):
        o.both({"tpwr": 1})

    def body(watch):
        with o.step(f"{sender}'s TUNE held 3 s by its lease, then let lapse") as s:
            t0 = time.time()
            quiet, _ = bench.spectrum_level(L, f, t0 - 2.0, t0, half=3000)
            last = None
            while time.time() - t0 < 3.0:
                S.send("tune 1 7")
                last = time.time()
                time.sleep(0.8)
            on = S.state.get("tune") and S.tx
            off_at = None
            while time.time() - last < 7:
                if not S.tx and not S.state.get("tune"):
                    off_at = time.time() - last
                    break
                time.sleep(0.05)
            S.send("tune 0")
            loud, n = bench.spectrum_level(L, f, t0 + 0.8, last, half=3000)
            res.update(on=bool(on), off_after_s=off_at and round(off_at, 1), quiet=quiet, loud=loud, frames=n)
            if not on:
                raise Refused(f"{sender} not on the air with TUNE (tune {S.state.get('tune')}, tx {S.tx})")
            if quiet is None or loud is None or loud - quiet < 15:
                raise Refused(f"{listener}'s spectrum near {f} Hz: {quiet} -> {loud} dB, not 15 dB up")
            if off_at is None or not 2.5 <= off_at <= 5.0:
                raise Refused(f"TUNE off {off_at} s after the last renewal (2.5-5 s wanted)")
            s.confirm(f"{listener} {quiet:.0f} -> {loud:.0f} dB; off {off_at:.1f} s after the last renewal")
    try:
        over(o, sender, listener, f, 2, label, body)
    except Refused as e:
        return False, str(e), res
    return True, (f"on the air with TUNE, {listener} {res['loud'] - res['quiet']:.0f} dB up, off by itself "
                  f"{res['off_after_s']} s after the last renewal"), res


def ft8_stop(o, sender, listener, label, dial=14074000):
    """The FT8 app on both on dial, the sender's CQ stopped 2 s into its transmission: off the air within 1 s"""
    S, L = o.r[sender], o.r[listener]
    res = {}
    try:
        with o.step("both on their test calls, the FT8 app open on both"):
            test_calls(o)
            o.changing += 1
            try:
                for r in (S, L):
                    r.send("ft8 open")
                for r in (S, L):
                    r.wait("the FT8 app", lambda r=r: r.ft8.get("open") == 1, 10)
                    time.sleep(1.0)
                    for _ in range(3):
                        if r.state.get("app") == "FT8":
                            break
                        p = r.ft8.get("proto")
                        r.send("ft8 mode")
                        r.wait("FT8's protocol stepped", lambda r=r, p=p: r.ft8.get("proto") != p, 5)
                        time.sleep(0.8)
                th = [threading.Thread(target=bench.ft8_to_band, args=(r, dial)) for r in (S, L)]
                [t.start() for t in th]
                [t.join(40) for t in th]
            finally:
                o.changing -= 1
            o.both({"freq": dial, "pwr": W})
        p, total = bench.chain_gate(o.c, "radios")
        bench.radio_tx_gate(S, o.c, W)
        bench.listener_gate(L)
        o.roles = {sender: "TX", listener: "RX"}
        watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W, label=label)
        try:
            with o.step(f"{sender}'s CQ stopped 2 s into it: off the air within 1 s") as s:
                S.send("ft8 cq")
                S.wait("its transmission", lambda: S.tx or watch.why, 32)
                bench.tx_log({"who": sender, "f": dial, "w": S.pwr(), "what": f"orch: {label}"})
                time.sleep(2.0)
                t0 = time.time()
                S.send("ft8 stop")
                S.wait("off the air", lambda: not S.tx, 3)
                res["off_s"] = round(time.time() - t0, 2)
                if res["off_s"] > 1.0:
                    raise Refused(f"off the air {res['off_s']} s after ft8 stop")
                time.sleep(3.0)
                if S.tx:
                    raise Refused(f"{sender} on the air again after the stop")
                s.confirm(f"off {res['off_s']} s after the stop, not again")
        finally:
            watch.close()
            S.send("ft8 stop")
            o.roles = {A: "RX", B: "RX"}
    except Refused as e:
        return False, str(e), res
    finally:
        for r in (S, L):
            r.send("ft8 close")
    return True, f"off the air {res['off_s']} s after ft8 stop, not again", res


def best_cer(got, want):
    """The character error rate of the stretch of got nearest to want (spaces squeezed): (rate, that stretch)"""
    g, w = " ".join(got.upper().split()), " ".join(want.upper().split())
    best = (1.0, "")
    for n in range(max(1, len(w) - 6), len(w) + 7):
        for i in range(0, max(1, len(g) - n + 1)):
            seg = g[i:i + n]
            r = lev(seg, w) / len(w)
            if r < best[0]:
                best = (r, seg)
    return best


def digi_text(o, sender, listener, mode, label):
    """The listener's app for mode (PSK31) open, the sender on its dial and mode (confirmed alike), the text played as
    the mode's audio through the sender's TCI at 0.1 W; the listener's decoded line (its text log, once the app
    closed): PASS at most 5 % of the characters wrong against the text sent"""
    import subprocess
    import digimodes
    import suite_more
    S, L = o.r[sender], o.r[listener]
    text = suite_more.DIGI_TEXT[mode]
    res = {"sent": text}
    try:
        with o.step(f"{listener}'s {mode} app open, {sender} put on its dial and mode in the same change") as s:
            o.changing += 1             # the app moves the listener: the sender follows before the guard looks again
            try:
                if not suite_more.open_app(L, suite_more.DIGI_APP[mode]):
                    raise Refused(f"{listener}'s {mode} app did not open")
                L.wait("the app's dial", lambda: L.state.get("app") or L.state.get("win"), 5)
                time.sleep(1.0)
                f, m = int(L.state.get("freq", 0)), int(L.state.get("mode", -1))
                o.both({"freq": f, "mode": m})
            finally:
                o.changing -= 1
            s.confirm(f"both on {f / 1e6:.4f} MHz mode {m}")
        x = digimodes.psk31_audio(text)
        local = os.path.join(o.st.run_dir, f"{mode}_{sender}.raw")
        secs = digimodes.write_raw(x, local)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:/tmp/bench_digi.raw"], check=True)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "tci_tone_tx.py"), f"root@{S.host}:/tmp/tci_tone_tx.py"], check=True)
        sizes = "cd /mnt/digi_text 2>/dev/null && for f in *.txt; do echo \"$f $(wc -c < $f)\"; done"
        # every day's file and its size: the line may go into a new day's file (UTC midnight, 2026-11-24: the
        # text was decoded right and the reader looked in yesterday's)
        log0 = dict(ln.split() for ln in bench.ssh(L.host, sizes).split("\n") if len(ln.split()) == 2)

        def body(watch):
            with o.step(f"{sender} sends {text!r} ({secs:.1f} s of {mode})"):
                bench.tci_send(S, None, max_s=secs + 4, watch=watch, file="/tmp/bench_digi.raw", amp=0.5)
                if watch.why:
                    raise Refused(watch.why)
        over(o, sender, listener, f, m, label, body, pages=False)
        with o.step(f"{listener}'s app closed (both on the dial again), its decoded line read") as s:
            o.changing += 1
            try:
                bench.close_apps(L)
                o.both({"freq": f, "mode": m})
            finally:
                o.changing -= 1
            got, t0 = "", time.time()
            while time.time() - t0 < 30:
                now = dict(ln.split() for ln in bench.ssh(L.host, sizes).split("\n") if len(ln.split()) == 2)
                got = "".join(bench.ssh(L.host, f"tail -c +{int(log0.get(n, 0)) + 1} /mnt/digi_text/{n}")
                              for n, size in sorted(now.items()) if int(size) > int(log0.get(n, 0)))
                cer, seg = best_cer(got, text)
                if cer <= 0.05:
                    break
                time.sleep(3.0)
            res.update(decoded=seg, cer=round(cer, 3))
            if cer > 0.05:
                raise Refused(f"{listener} decoded {seg!r} ({cer:.0%} wrong against {text!r}; the log's new text: "
                              f"{' '.join(got.split())[:160]!r})")
            s.confirm(f"{seg!r}, {cer:.0%} wrong")
    except Refused as e:
        return False, str(e), res
    return True, f"sent {text!r}, decoded {res['decoded']!r} ({res['cer']:.0%} of the characters wrong)", res


def in_parallel(o, fn):
    """fn(name, radio) on both radios at once, inside one guarded change: {name: its return}; a Refused in either is
    raised after both are done"""
    out, errs = {}, {}

    def one(n):
        try:
            out[n] = fn(n, o.r[n])
        except (Refused, OSError, ValueError, IndexError) as e:
            errs[n] = f"{n}: {e}"
    o.changing += 1
    try:
        th = [threading.Thread(target=one, args=(n,)) for n in o.r]
        [t.start() for t in th]
        [t.join(30) for t in th]
    finally:
        o.changing -= 1
    if errs:
        raise Refused("; ".join(errs.values()))
    return out


def alike(o, keys):
    """The two radios read the same for keys (their state as they last said it): refused naming what differs"""
    d = {k: (bench._value(o.r[A], k), bench._value(o.r[B], k)) for k in keys
         if bench._value(o.r[A], k) != bench._value(o.r[B], k)}
    if d:
        raise Refused("the two differ: " + ", ".join(f"{k} {a}/{b}" for k, (a, b) in d.items()))


def t_tci_control(o):
    """Each radio's own TCI (port 50001) given the same VFO and modulation at once: each radio's remote state must
    say them (confirmed), the two alike; then back"""
    from remote_ws import Remote
    plan = [(7074000, "USB", 2), (14040000, "CW", 4), (21074000, "LSB", 0), (14200000, "USB", 2)]
    seen = []
    with o.step("both on page G"):
        o.both_page("G")
    clients = {n: Remote(r.host, 50001, timeout=5) for n, r in o.r.items()}
    time.sleep(1.0)                             # the server's greeting first (it ends with ready)
    try:
        for f, mod, m in plan:
            with o.step(f"TCI vfo:0,0,{f}; modulation:0,{mod}; on both, each confirmed by its own state") as s:
                def one(n, r, f=f, mod=mod, m=m):
                    clients[n].send(f"vfo:0,0,{f};")
                    clients[n].send(f"modulation:0,{mod};")
                    r.wait(f"{f} Hz {mod}", lambda: int(r.state.get("freq", 0)) == f and int(r.state.get("mode", -1)) == m,
                           6)
                in_parallel(o, one)
                alike(o, ("freq", "mode"))
                seen.append(f"{f / 1e6:.3f} {mod}")
                s.confirm(f"both {f} Hz mode {m}")
    finally:
        for c in clients.values():
            c.close()
    return True, f"TCI set {len(plan)} VFO/modulation pairs, each confirmed on both: {', '.join(seen)}", {"seen": seen}


def t_rigctl(o):
    """Hamlib's NET rigctl (4532) on both at once: F/f, M/m read back and confirmed by each radio's own state, the two
    alike; T 1 (PTT) refused and nothing keyed"""
    from suite_more import rig
    with o.step("both on page G"):
        o.both_page("G")
    with o.step("F 14074000, f, M USB 2400, m on both; each radio's state confirms") as s:
        def one(n, r):
            out = rig(r.host, ["F 14074000", "f", "M USB 2400", "m"])
            if not out[0].startswith("RPRT 0") or out[1].split()[:1] != ["14074000"] or out[3].split()[:1] != ["USB"]:
                raise Refused(f"rigctl answered {out}")
            r.wait("14.074 USB", lambda: int(r.state.get("freq", 0)) == 14074000 and int(r.state.get("mode", -1)) == 2, 5)
            return out
        got = in_parallel(o, one)
        alike(o, ("freq", "mode"))
        s.confirm(str(got))
    with o.step("T 1 (rigctl's PTT) on both: refused, nothing keyed") as s:
        def ptt(n, r):
            out = rig(r.host, ["T 1", "t"])
            time.sleep(0.5)
            if not out[0].startswith("RPRT -") or out[1].split()[:1] != ["0"] or r.tx:
                raise Refused(f"rigctl's T 1 answered {out}, on the air {r.tx}")
            return out
        got2 = in_parallel(o, ptt)
        s.confirm(str(got2))
    return True, "rigctl F/f and M/m confirmed on both and alike; its PTT refused on both, nothing keyed", \
        {"set": got, "ptt": got2}


def t_dial_lock(o):
    """The dial locked on both: a frequency set refused (err, the dial where it was once the err came); unlocked, the
    same set taken on both"""
    f0, f1 = 14200000, 14201000
    with o.step("both on page G, 14.200 MHz USB"):
        o.both_page("G")
        o.both({"freq": f0, "mode": 2})
    try:
        with o.step("the dial lock on, on both, confirmed"):
            def lock(n, r):
                r.send("set lock 1")
                r.wait("locked", lambda: int(r.state.get("ulock", 0)) == 1, 5)
            in_parallel(o, lock)
        with o.step(f"set freq {f1} on both: refused, the dial kept") as s:
            def try_set(n, r):
                e0 = len(r.errs)
                r.send(f"set freq {f1}")
                r.wait("its refusal", lambda: len(r.errs) > e0, 5)
                time.sleep(0.3)
                if int(r.state.get("freq", 0)) != f0:
                    raise Refused(f"the dial moved to {r.state.get('freq')} while locked")
                return r.errs[-1]
            said = in_parallel(o, try_set)
            alike(o, ("freq", "mode"))
            s.confirm(str(said))
    finally:
        with o.step("the dial lock off on both, confirmed"):
            def unlock(n, r):
                r.send("set lock 0")
                r.wait("unlocked", lambda: int(r.state.get("ulock", 1)) == 0, 5)
            in_parallel(o, unlock)
    with o.step(f"set freq {f1} on both: taken"):
        o.both({"freq": f1})
        o.both({"freq": f0})
    return True, f"locked: the set refused on both ({said[A]!r}), the dial kept; unlocked: taken on both", {"said": said}


# ---- The tests ----

class Test:
    def __init__(self, name, role, what, fn, est_s):
        self.name, self.role, self.what, self.fn, self.est_s = name, role, what, fn, est_s


SYNC_PLAN = [(3573000, 2), (7074000, 2), (7200000, 0), (14040000, 4), (14074000, 3), (14200000, 2), (21074000, 3),
             (28074000, 2), (29000000, 6), (29600000, 7)]


def t_sync_bands(o):
    """Both radios through the HF bands and modes together: each setting confirmed on both, the two the same"""
    seen = []
    for f, m in SYNC_PLAN:
        with o.step(f"both to {f / 1e6:.3f} MHz mode {m}"):
            o.both({"freq": f, "mode": m})
            seen.append(f"{f / 1e6:.3f}/{m}")
    return True, f"{len(seen)} settings, each confirmed on both and alike", {"settings": seen}


def voice(sender, listener, f, mode, name):
    return lambda o: audio_steps(o, sender, listener, f, mode, name)


def pair(name, role, what, mk, est):
    """A test both ways: the X6100 sending to the X6200, then the other way round"""
    return [Test(f"{name} x6100>x6200", "X6100 TX, X6200 RX", what, mk(A, B, f"{name} x6100>x6200"), est),
            Test(f"{name} x6200>x6100", "X6200 TX, X6100 RX", what, mk(B, A, f"{name} x6200>x6100"), est)]


TESTS = [
    Test("identity", "both", "the same GUI and apps on both, neither's A running a routine of its own", t_identity, 10),
    Test("services", "both", "the ports and pages that must answer, and the two that must not", t_services, 15),
    Test("web-secrets", "both", "the HTTPS key, the backups and the remote's sessions refused (403) by the Files page",
         t_web_secrets, 5),
    Test("sync-bands", "both RX", "both radios through 10 bands/modes together, each confirmed on both", t_sync_bands, 40),
    Test("usb-steps x6100>x6200", "X6100 TX, X6200 RX", "USB 14.200: a stepped tone sequence from the X6100 heard in "
         "order in the X6200's audio", voice(A, B, 14200000, 2, "usb-steps x6100>x6200"), 30),
    Test("usb-steps x6200>x6100", "X6200 TX, X6100 RX", "USB 14.200: a stepped tone sequence from the X6200 heard in "
         "order in the X6100's audio", voice(B, A, 14200000, 2, "usb-steps x6200>x6100"), 30),
    Test("lsb-steps x6100>x6200", "X6100 TX, X6200 RX", "LSB 7.200: the stepped sequence heard",
         voice(A, B, 7200000, 0, "lsb-steps x6100>x6200"), 30),
    Test("lsb-steps x6200>x6100", "X6200 TX, X6100 RX", "LSB 7.200: the stepped sequence heard",
         voice(B, A, 7200000, 0, "lsb-steps x6200>x6100"), 30),
    Test("am-steps x6100>x6200", "X6100 TX, X6200 RX", "AM 29.000: the stepped sequence heard",
         voice(A, B, 29000000, 6, "am-steps x6100>x6200"), 30),
    Test("am-steps x6200>x6100", "X6200 TX, X6100 RX", "AM 29.000: the stepped sequence heard",
         voice(B, A, 29000000, 6, "am-steps x6200>x6100"), 30),
    Test("fm-steps x6100>x6200", "X6100 TX, X6200 RX", "FM 29.600: the stepped sequence heard",
         voice(A, B, 29600000, 7, "fm-steps x6100>x6200"), 30),
    Test("fm-steps x6200>x6100", "X6200 TX, X6100 RX", "FM 29.600: the stepped sequence heard",
         voice(B, A, 29600000, 7, "fm-steps x6200>x6100"), 30),
    *pair("cw-text", "", "CW 14.040: 'VVV TEST DE Q1TST' keyed, the other's decoder must print TEST DE Q1TST",
          lambda s, l, n: (lambda o: cw_text(o, s, l, 14040000, n)), 30),
    *pair("power-steps", "", "USB 14.200: a tone at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB)",
          lambda s, l, n: (lambda o: power_steps(o, s, l, 14200000, n)), 45),
    *pair("ft8-cq", "", "FT8 14.074: a CQ in the next slot, the other must decode exactly the message sent",
          lambda s, l, n: (lambda o: ft8_cq(o, s, l, n)), 40),
    *pair("psk31-text", "", "PSK31 on the listener's app dial: 'CQ CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % "
          "of the characters wrong", lambda s, l, n: (lambda o: digi_text(o, s, l, "psk31", n)), 45),
    *pair("tune-carrier", "", "TUNE at 0.1 W: on the air, heard 15 dB up, off by itself 2.5-5 s after the lease's last "
          "renewal", lambda s, l, n: (lambda o: tune_carrier(o, s, l, n)), 25),
    *pair("ft8-stop", "", "FT8: ft8 stop 2 s into a CQ takes the sender off the air within 1 s, not again",
          lambda s, l, n: (lambda o: ft8_stop(o, s, l, n)), 35),
    Test("tci-control", "both RX", "TCI vfo and modulation on both radios' own TCI: each confirmed by its own state, "
         "the two alike", t_tci_control, 20),
    Test("rigctl", "both RX", "rigctl F/f M/m on both: confirmed by each radio's state, alike; its PTT refused",
         t_rigctl, 15),
    Test("dial-lock", "both RX", "the dial lock on both: a set refused with the dial kept; off, the set taken",
         t_dial_lock, 15),
    Test("rx-settings", "both RX", "PRE, ATT, AGC, NB, NR, squelch, RF gain: each set on both, read back alike",
         t_rx_settings, 40),
    Test("ptt-lease x6100", "X6100 TX, X6200 RX", "one PTT never renewed: off the air by itself within 4 s",
         lambda o: ptt_lease(o, A, B, "ptt-lease x6100"), 20),
    Test("ptt-lease x6200", "X6200 TX, X6100 RX", "one PTT never renewed: off the air by itself within 4 s",
         lambda o: ptt_lease(o, B, A, "ptt-lease x6200"), 20),
    Test("ptt-locked x6100", "both RX", "the X6100's PTT with its transmitter locked: stays off the air",
         lambda o: ptt_refused(o, A, B, 14200000, False, "ptt-locked x6100"), 12),
    Test("ptt-locked x6200", "both RX", "the X6200's PTT with its transmitter locked: stays off the air",
         lambda o: ptt_refused(o, B, A, 14200000, False, "ptt-locked x6200"), 12),
    Test("ptt-out-of-band x6100", "both RX", "the X6100 armed on 15.000 MHz (WWV, no amateur band): PTT refused",
         lambda o: ptt_refused(o, A, B, 15000000, True, "ptt-out-of-band x6100"), 12),
    Test("ptt-out-of-band x6200", "both RX", "the X6200 armed on 15.000 MHz (WWV, no amateur band): PTT refused",
         lambda o: ptt_refused(o, B, A, 15000000, True, "ptt-out-of-band x6200"), 12),
]


def run(names, port, lan, linger):
    tests = [t for t in TESTS if not names or t.name in names or t.name.split(" ")[0] in names]
    if not tests:
        raise SystemExit(f"no test named {names}")
    st = Status(tests)
    srv = console(st, port, lan)
    st.run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-orch"))
    os.makedirs(st.run_dir, exist_ok=True)
    st.log = open(os.path.join(st.run_dir, "events.jsonl"), "a")
    st.ev(f"console: http://{'0.0.0.0' if lan else '127.0.0.1'}:{port}/ ; run {st.run_dir}")
    o = None

    def stop(*_):
        if st.phase in ("done",) or st.phase.startswith("stopped"):
            os._exit(0)                 # the run is over: only the console to close
        st.ev("stopped by a signal: every transmitter stopped", "bad")
        bench.emergency_stop(bench.conf())
        os._exit(2)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        st.phase = "connecting"
        o = Orch(st)
        st.phase = "syncing both"
        # the start: both transmitters locked, both on the Sentry's page G on one frequency and mode, confirmed, before
        # any test (the first full run began with the X6100 armed on 14.074 USB-D and the X6200 on 29.600 NFM)
        o.changing += 1
        try:
            for n, r in o.r.items():
                bench.close_apps(r)
                if r.state.get("rxo") == 0:
                    bench.arm(r, False)
                r.send("apps")
        finally:
            o.changing -= 1
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
        st.ev("both locked, on page G, on 14.2000 MHz USB, confirmed alike", "ok")
        st.phase = "running"
        for i, t in enumerate(tests):
            with st.lock:
                st.cur = i
            st.ev(f"TEST {i + 1}/{len(tests)} {t.name} ({t.role}): {t.what}")
            o.ready()                       # never on from radios set wrong: refused here stops the run
            o.violation = None
            for r in o.r.values():
                r.send(f"banner BENCH {t.name.upper()} ({t.role.upper()})"[:100])
            t0 = time.time()
            try:
                ok, says, data = t.fn(o)
            except Refused as e:
                ok, says, data = False, f"refused: {e}", {}
            except Exception as e:      # a bug in the test itself: said, the run goes on
                ok, says, data = False, f"error in the test: {type(e).__name__}: {e}", {}
            if o.violation:
                ok, says = False, f"OUT OF SYNC during the test ({o.violation}); " + says
            res = "PASS" if ok is True else "LOOK" if ok is None else "FAIL"
            with st.lock:
                st.tests[i].update(result=res, says=says, secs=time.time() - t0, data=data)
            st.ev(f"{res} {t.name}: {says}", "ok" if res == "PASS" else "bad")
            st.ev(f"data of {t.name}: {json.dumps(data, default=str)[:4000]}", "conf")
            for r in o.r.values():
                r.send("banner")
            with open(os.path.join(st.run_dir, "results.json"), "w") as fh:
                json.dump(st.tests, fh, indent=1, default=str)
        st.phase = "done"
    except Refused as e:
        st.phase = f"stopped: {e}"
        st.ev(f"run stopped: {e}", "bad")
    finally:
        with st.lock:
            st.cur = None
        if o:
            o.close()
        c = collections.Counter(t["result"] for t in st.tests)
        st.ev(f"END: {c['PASS']} pass, {c['FAIL']} fail, {c['LOOK']} look, "
              f"{sum(1 for t in st.tests if t['result'] is None)} not run; both radios on the Sentry's page A")
        with open(os.path.join(st.run_dir, "results.json"), "w") as fh:
            json.dump(st.tests, fh, indent=1, default=str)
    if linger > 0:
        st.ev(f"the console stays up {linger} s")
        time.sleep(linger)
    srv.shutdown()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    rp = sub.add_parser("run")
    rp.add_argument("--only", default="")
    rp.add_argument("--port", type=int, default=8765)
    rp.add_argument("--lan", action="store_true", help="the console on the network, not only this computer")
    rp.add_argument("--linger", type=int, default=3600, help="seconds the console stays up after the run")
    a = ap.parse_args()
    if a.cmd == "list":
        for t in TESTS:
            print(f"{t.name:26s} {t.role:20s} ~{t.est_s:3d} s  {t.what}")
        return
    run([x for x in a.only.split(",") if x], a.port, a.lan, a.linger)


if __name__ == "__main__":
    main()
