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
import re
import signal
import sys
import threading
import types
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
TRAIL_S = 20.0             # with a violation, both radios' dial, mode, page and app over this long before it
GUARD_S = 0.6               # a difference lasting this long outside a change: a violation
STALE_S = 3.0               # no word from a radio this long: its GUI down (planned or not), said on its card
SQL_HOLD = 0                # the squelch every test holds: open. The base mutes the received audio while it is closed
                            # (speaker, decoders, TCI; not the spectrum), and the owner, 2026-11-24: "that is all I
                            # care about is signal. I don't want wrong squelch to prevent reception" (10 kept AM audio
                            # closed 1.3-2 s into a carrier). The volume is 0, so nothing is heard; --sql N for another
RF_ALARM_DBM = -100         # a receiver over this with neither radio saying it transmits: a signal nobody explains
RF_ALARM_S = 1.0            # (the cable at 0.1 W: about -70 dBm; quiet -121; the X6100's own spurs about -110)
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


# ---- What has passed: kept across runs (the owner, 2026-11-23: "keep track of passed tests persistently so they
# don't re run"; "each new test should be tacked on, and simply run ... any failures addressed, and if they can't be,
# simply documented, and move on") ----

STORE = os.path.join(bench.STATE, "orch_results.json")
# The run's state as the console shows it, written every second: the console (orch.py console) is a process of its own
# that serves it, up between runs (the owner, 2026-11-24: "ensure http://127.0.0.1:8765 stays up!")
SNAPSHOT = os.path.join(bench.STATE, "orch_status.json")
SNAPSHOT_STALE_S = 10       # older than this: no run is writing it

# Failures that cannot be put right from here (or not found yet): said with each, not run again until --all (or the
# reason is gone)
KNOWN = {
}


CAPS = os.path.join(bench.STATE, "orch_caps.json")     # what each radio takes, from the caps test


def caps_load():
    return bench.load_json(CAPS, {})


def common(key, value):
    """True unless the caps map says one of the radios does not take value for key (an unprobed one: True)"""
    c = caps_load().get(f"{key} {value}")
    return c is None or c.get("same", True) or all(x.get("ok") for x in c.get("radios", {}).values())


def test_code(t):
    """A test's own code as a fingerprint: the source of its function (the one a lambda wraps, by its defaults or its
    closure). The tally ("passes/runs") counts from the last change to it: a test failed six times while it was being
    fixed and passed the seventh read 1/7 (the owner, 2026-11-25: "several 1/7 passed, but never 7/7")"""
    import hashlib
    import inspect
    fns, seen = [t.fn], []
    while fns:
        f = fns.pop()
        if not callable(f) or not hasattr(f, "__code__") or f in seen:
            continue
        seen.append(f)
        fns += list(f.__defaults__ or ()) + [c.cell_contents for c in (f.__closure__ or ())
                                             if callable(getattr(c, "cell_contents", None))]
    h = hashlib.sha1()
    for f in seen:
        if f.__name__ == "<lambda>":
            continue
        try:
            h.update(inspect.getsource(f).encode())
        except (OSError, TypeError):
            h.update(f.__code__.co_code)
    return h.hexdigest()[:12]


def store_load():
    return bench.load_json(STORE, {})


def store_put(name, rec):
    doc = store_load()
    doc[name] = rec
    bench.save_json(STORE, doc)


# ---- The run's state: what the console shows ----

class Status:
    def __init__(self, tests, kept=()):
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.phase = "starting"
        self.tests = [{"name": t.name, "role": t.role, "what": t.what, "est_s": t.est_s, "result": None,
                       "says": "", "secs": None, "steps": [], "shots": []} for t in tests]
        # the tests not run this time: their last result as kept (STORE), shown under this run's
        self.kept = list(kept)
        self.cur = None             # index into tests
        self.events = collections.deque(maxlen=3000)
        self.log = None             # the run's events.jsonl: every event, kept for the audit
        self.radios = {}
        self.sync = {"ok": True, "since": time.time(), "violations": 0, "last": None, "changing": False}
        self.run_dir = None
        self.shots = []             # both screens as photographed: {"t", "label", "paths": {name: path}}
        self.hackrf = {"in_test": False, "running": False, "what": "not part of any test yet"}

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

    def snapshot(self):
        """to_json() into SNAPSHOT, whole (a new file renamed over the old)"""
        tmp = SNAPSHOT + ".tmp"
        with open(tmp, "w") as fh:
            fh.write(self.to_json())
        os.replace(tmp, SNAPSHOT)

    def to_json(self):
        with self.lock:
            done = [t for t in self.tests if t["result"] is not None]
            left = sum(t["est_s"] for t in self.tests if t["result"] is None)
            counts = collections.Counter(t["result"] for t in done)
            return json.dumps({
                "now": time.time(), "t0": self.t0, "phase": self.phase, "cur": self.cur,
                "tests": [dict(x, shots=[{"label": sh["label"], "paths": {n: os.path.basename(p) for n, p in
                                                                         (sh.get("paths") or {}).items() if p}}
                                         for sh in x.get("shots", [])]) for x in self.tests],
                "events": list(self.events)[-400:], "radios": self.radios, "sync": self.sync, "kept": self.kept,
                "caps_diff": [dict(v, key=k) for k, v in caps_load().items() if not v.get("same", True)],
                "hackrf": self.hackrf,
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
.radios{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}@media(max-width:560px){.radios{grid-template-columns:minmax(0,1fr)}}.radio{border:1px solid var(--line);border-radius:6px;padding:8px}
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
<section class="wide"><h2>Radios</h2><div id="sync"></div><div class="radios" id="radios" style="margin-top:8px"></div></section>
<section class="wide"><h2>Now</h2><div id="now"></div><div class="bar"><div id="prog" style="width:0"></div></div></section>
<section class="wide"><h2>This run: only the tests not passed yet (and any new ones)</h2><table><thead><tr><th>#</th><th>test</th><th>role</th><th>result</th><th>secs</th><th>what it found</th></tr></thead><tbody id="tests"></tbody></table></section>
<section class="wide"><h2 id="kepth">Not rerun: passed in an earlier run (or a known fault), kept in orch_results.json, the latest first</h2><table><thead><tr><th>test</th><th>result</th><th>when</th><th>GUI</th><th>what it found</th></tr></thead><tbody id="kept"></tbody></table></section>
<section class="wide"><h2>Where the two radios differ (the caps test: the same value sent to both, what each did)</h2><table><thead><tr><th>asked</th><th>X6100</th><th>X6200</th></tr></thead><tbody id="caps"></tbody></table></section>

<section class="wide"><h2>Events (everything the orchestrator does; <a href="log" style="color:var(--amb)">the whole log</a>)</h2>
<div><label><input type="checkbox" id="fcmd" checked> commands sent</label><label><input type="checkbox" id="fconf" checked> read-backs</label><label><input type="checkbox" id="fradio" checked> the radios' errors</label></div><div class="ev" id="ev"></div></section>
</main>
<script>
const $=id=>document.getElementById(id);const esc=s=>String(s==null?"":s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const mhz=f=>f?(f/1e6).toFixed(4)+" MHz":"?";const t=s=>new Date(s*1000).toLocaleTimeString();
const MODES={0:"LSB",1:"LSB-D",2:"USB",3:"USB-D",4:"CW",5:"CW-R",6:"AM",7:"NFM",8:"WFM"};
async function tick(){let d;try{d=await (await fetch("status",{cache:"no-store"})).json()}catch(e){$("phase").textContent="console: no answer";return}
$("phase").innerHTML="<b>"+esc(d.phase)+"</b>";const c=d.counts;
const kp=(d.kept||[]).filter(k=>k.result==="PASS"&&!k.known).length,kk=(d.kept||[]).filter(k=>k.known).length;$("counts").textContent=`this run: ${c.done}/${c.total} done: ${c.pass} pass, ${c.fail} fail, ${c.look} look, ${c.notrun} not run · not rerun: ${kp} passed before, ${kk} known faults (of ${c.total+(d.kept||[]).length} tests)`;$("kepth").textContent=`Not rerun: ${kp} passed in an earlier run, ${kk} known faults, the latest first (kept in orch_results.json; orch.py run --all would run them again)`;
const el=Math.round(d.now-d.t0);$("clock").textContent=`running ${Math.floor(el/60)}m${el%60}s, about ${Math.round(d.eta_s/60)} min left`;
const s=d.sync;let cls=s.changing?"chg":(s.ok?"ok":"bad");
$("sync").innerHTML=`<span class="sync ${cls}">${s.changing?"CHANGING BOTH":(s.ok?"IN SYNC":"OUT OF SYNC")}</span> <span class="muted">violations this run: ${s.violations}${s.last?" (last: "+esc(s.last)+")":""}</span>`+((s.declared||[]).length?`<div class="says">declared apart by the test (bench.apart): ${(s.declared||[]).map(esc).join("; ")}</div>`:"");
const hk=d.hackrf||{};
$("radios").innerHTML=["x6100","x6200"].map(n=>[n,d.radios[n]]).map(([n,r])=>!r?`<div class="radio"><b>${n.toUpperCase()}</b> <span class="LOOK">connecting</span><div class="freq" style="color:var(--dim)">?</div><div class="muted">no state from it yet</div></div>`:`<div class="radio"><b>${n.toUpperCase()}</b> <span class="${r.tx?"tx":"rx"}">${r.tx?"ON THE AIR":"RX"}</span>
<div class="freq">${mhz(r.freq)}</div><div>${MODES[r.mode]||r.mode} &middot; ${r.pwr} W &middot; page ${esc(r.pg)} ${r.rxo===0?"&middot; <span class=tx>TX ARMED</span>":""}</div>
<div class="muted">S ${r.meter==null?"?":r.meter.toFixed(1)+" dBm"} &middot; squelch ${esc(r.sql)} &middot; volume ${esc(r.vol)} &middot; app ${esc(r.app||"-")}</div>
<div><b>should be: ${esc(r.role||"-")}</b> ${r.role==="TX"&&!r.tx?"<span class=muted>(between its sends)</span>":""}${r.role==="RX"&&r.tx?" <span class=tx>BUT TRANSMITS</span>":""}</div>
<div class="${r.link==="live"?"rx":(String(r.link).startsWith("GUI RESTARTING")?"LOOK":"tx")}"><b>${esc(r.link==="live"?"GUI live":r.link)}</b>${r.link==="live"?"":" &middot; "+r.age.toFixed(0)+" s"}</div></div>`).join("")+`<div class="radio"><b>HACKRF</b> <span class="${hk.running?"tx":"rx"}">${hk.running?"TRANSMITTING":"idle"}</span>
<div class="freq" style="${hk.running?"":"color:var(--dim)"}">${hk.running?"ON THE AIR":"idle"}</div><div>${hk.in_test?"<b>part of this test</b>":"not part of this test"}</div>
<div class="muted">${esc(hk.what||"")}</div></div>`;
const cur=d.cur!=null?d.tests[d.cur]:null;
$("now").innerHTML=cur?`<div><b>${esc(cur.name)}</b> <span class="muted">(${esc(cur.role)})</span></div><div class="says">${esc(cur.what)}</div>`+
 cur.steps.map(st=>`<div class="${st.state==="confirmed"?"PASS":st.state==="failed"?"FAIL":"LOOK"}">${st.state==="running"?"&#9654;":st.state==="confirmed"?"&#10003;":"&#10007;"} ${esc(st.text)} <span class="muted">${st.secs!=null?st.secs.toFixed(1)+" s":""} ${esc(st.detail||"")}</span></div>`).join(""):"<span class=muted>no test running</span>";
$("prog").style.width=(100*c.done/Math.max(1,c.total))+"%";
const pics=x=>x.shots.map(sh=>`<div class="says">${esc(sh.label)}</div>`+Object.entries(sh.paths).map(([n,p])=>`<a href="shot/${encodeURIComponent(p)}" target="_blank"><img src="shot/${encodeURIComponent(p)}" title="${esc(n)}" style="width:48%;margin:2px 1%;border:1px solid #333"></a>`).join("")).join("");
$("tests").innerHTML=d.tests.map((x,i)=>{const failed=x.result&&x.result!=="PASS";const n=failed?x.shots.length:0;
return `<tr class="${i===d.cur?"cur":""}"><td>${i+1}</td><td>${esc(x.name)}</td><td>${esc(x.role)}</td><td class="${(x.result||"").replace(" ","")}">${esc(x.result||(i===d.cur?"running":""))}${n?` <button onclick="tog(${i})" style="font:inherit;font-size:11px">pictures (${n})</button>`:""}</td><td>${x.secs!=null?x.secs.toFixed(0):""}</td><td class="says">${esc(x.says)}</td></tr>`+(n&&openPics.has(i)?`<tr><td></td><td colspan="5">${pics(x)}</td></tr>`:"");}).join("");
const cs=r=>r?(r.ok?"takes it":"refuses")+(r.reads!=null?" (reads "+esc(JSON.stringify(r.reads))+")":"")+(r.err?" "+esc(r.err):""):"";
$("caps").innerHTML=(d.caps_diff||[]).map(c=>`<tr><td>${esc(c.key)}</td><td class="says">${cs(c.radios.x6100)}</td><td class="says">${cs(c.radios.x6200)}</td></tr>`).join("")||"<tr><td class=muted colspan=3>not probed yet, or no difference</td></tr>";
$("kept").innerHTML=(d.kept||[]).slice().sort((a,b)=>(b.at||0)-(a.at||0)).map(k=>`<tr><td>${esc(k.name)}</td><td class="${(k.result||"").replace(" ","")}">${esc(k.result)}${k.known?" (known)":""}${k.runs?" &middot; "+k.passes+"/"+k.runs+" passed since its code last changed"+(k.all_runs&&k.all_runs!==k.runs?" ("+k.all_passes+"/"+k.all_runs+" in all)":""):""}</td><td>${k.at?new Date(k.at*1000).toLocaleString():""}</td><td class="says">${esc(k.gui||"")}</td><td class="says">${esc(k.known||k.says)}</td></tr>`).join("");
const show={cmd:$("fcmd").checked,conf:$("fconf").checked,radio:$("fradio").checked};
$("ev").innerHTML=d.events.slice().reverse().filter(e=>show[e.kind]!==false).map(e=>`<div class="${e.kind}">${t(e.t)} ${e.test?"["+esc(e.test)+"] ":""}${esc(e.text)}</div>`).join("");}
const openPics=new Set();function tog(i){openPics.has(i)?openPics.delete(i):openPics.add(i);tick();}
tick();setInterval(tick,700);
</script></body></html>"""


# Between runs the standing console watches the two radios itself (the owner, 2026-11-25: one found on USB-D, the other
# on CW after a dial bumped at the radio, "make sure that is part of the scripted radio sync checks"): their frequency
# and mode read over receive-only sessions (nothing set, control never taken), a difference standing over
# IDLE_APART_S shown as OUT OF SYNC on the console and kept in idle_sync.jsonl. A run's own guard takes over while it
# runs (the sessions closed meanwhile); its ready() puts the two alike again
IDLE_POLL_S = 2.0
IDLE_APART_S = 3.0
IDLE_LOG = os.path.join(bench.STATE, "idle_sync.jsonl")
IDLE = {"ok": None, "radios": {}, "since": None, "said": False, "events": [], "violations": 0, "last": ""}
IDLE_LOCK = threading.Lock()


def idle_watch():
    """The standing console's watch between runs (IDLE)"""
    rs = {}
    c = bench.conf()
    while True:
        time.sleep(IDLE_POLL_S)
        try:
            age = time.time() - float(json.load(open(SNAPSHOT)).get("now") or 0)
        except (OSError, ValueError):
            age = 1e9
        if age < SNAPSHOT_STALE_S:              # a run is going: its guard watches, the sessions stand aside
            for r in rs.values():
                try:
                    r.r.close()
                except OSError:
                    pass
            rs = {}
            with IDLE_LOCK:
                IDLE.update(ok=None, since=None, said=False)
            continue
        for n in (A, B):
            if n not in rs or rs[n].stale(10):
                try:
                    if n in rs:
                        rs[n].r.close()
                    rs[n] = bench.Radio(n, c["radios"][n], False)
                except (Refused, OSError):
                    rs.pop(n, None)
        if len(rs) < 2:
            continue
        now = {n: {"freq": int(r.state.get("freq", 0)), "mode": int(r.state.get("mode", -1)),
                   "pg": r.state.get("pg"), "app": r.state.get("app") or r.state.get("win"), "tx": bool(r.tx)}
               for n, r in rs.items()}
        apart = [k for k in ("freq", "mode") if now[A][k] != now[B][k]]
        t = time.time()
        with IDLE_LOCK:
            IDLE["radios"] = now
            if not apart:
                if IDLE["said"]:
                    e = {"t": t, "kind": "ok", "text": f"idle: both alike again ({now[A]['freq']} Hz mode "
                                                        f"{now[A]['mode']})", "test": None}
                    IDLE["events"].append(e)
                    _idle_log(e)
                IDLE.update(ok=True, since=None, said=False)
                continue
            IDLE["since"] = IDLE["since"] or t
            if t - IDLE["since"] >= IDLE_APART_S and not IDLE["said"]:
                text = (f"idle: OUT OF SYNC, {' and '.join(apart)} apart: {A} {now[A]['freq']} Hz mode {now[A]['mode']}"
                        f", {B} {now[B]['freq']} Hz mode {now[B]['mode']} (the next run puts them alike)")
                e = {"t": t, "kind": "bad", "text": text, "test": None}
                IDLE["events"].append(e)
                del IDLE["events"][:-50]
                IDLE.update(ok=False, said=True, violations=IDLE["violations"] + 1, last=text)
                _idle_log(e)


def _idle_log(e):
    try:
        with open(IDLE_LOG, "a") as f:
            f.write(json.dumps(e) + "\n")
    except OSError:
        pass


def snapshot_json():
    """SNAPSHOT as the console serves it: a run not writing it said in its phase (the run's last state kept)"""
    try:
        d = json.load(open(SNAPSHOT))
    except (OSError, ValueError):
        return json.dumps({"now": time.time(), "t0": time.time(), "phase": "no run yet", "cur": None, "tests": [],
                           "events": [], "radios": {}, "sync": {}, "kept": [], "caps_diff": [], "hackrf": {},
                           "counts": {"pass": 0, "fail": 0, "look": 0, "notrun": 0, "done": 0, "total": 0},
                           "eta_s": 0, "run_dir": None})
    age = time.time() - float(d.get("now") or 0)
    if age > SNAPSHOT_STALE_S:
        d["phase"] = f"no run going: the last ({os.path.basename(d.get('run_dir') or '')}) {d.get('phase')}, " \
                     f"its last word {int(age // 60)} min ago"
        d["cur"] = None
        with IDLE_LOCK:                         # the console's own watch between runs (idle_watch)
            if IDLE["ok"] is not None:
                d["sync"] = {"ok": IDLE["ok"], "changing": False, "violations": IDLE["violations"],
                             "last": IDLE["last"], "declared": []}
                for n, v in IDLE["radios"].items():
                    d.setdefault("radios", {}).setdefault(n, {}).update(v, age=0, link="live (idle watch)")
            d["events"] = list(d.get("events") or []) + list(IDLE["events"])
    d["now"] = time.time()
    return json.dumps(d, default=str)


def console(status, port, lan):
    """The console on port: status's own state, or (status None: orch.py console) the runs' SNAPSHOT"""
    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/status"):
                body = status.to_json() if status is not None else snapshot_json()
                body, ctype = body.encode(), "application/json"
            elif self.path.startswith("/shot/"):        # a picture of a radio's screen (bench.SHOTS_DIR only)
                name = os.path.basename(self.path[6:].split("?")[0])
                path = os.path.join(bench.SHOTS_DIR, name)
                if not name.endswith(".png") or not os.path.isfile(path):
                    self.send_error(404)
                    return
                body, ctype = open(path, "rb").read(), "image/png"
            elif self.path.startswith("/log"):          # the run's every event, as kept in events.jsonl
                rd = status.run_dir if status is not None else json.loads(snapshot_json()).get("run_dir")
                path = os.path.join(rd or "", "events.jsonl")
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
            t0 = time.time()
            while True:                     # only when free or the bench's own (bench.Radio.take_control): someone
                try:                        # at the radio keeps it; the run waits for them, said on the console
                    self.r[n].take_control()
                    break
                except Refused as e:
                    if time.time() - t0 > 900:
                        raise
                    if int(time.time() - t0) % 60 < 5:
                        status.ev(f"waiting for {n}'s operator to let go of control ({e})", "radio")
                    status.phase = f"waiting: {n}'s control is at the radio"
                    time.sleep(5)
        for n, r in self.r.items():
            self._log_sends(n, r)
            hello(r)                        # the version kept now, while its first line is still there
        self._errs = {n: len(r.errs) for n, r in self.r.items()}
        self._tx = {n: bool(r.tx) for n, r in self.r.items()}
        self.planned = {}                   # name: why its GUI is restarting by the orchestrator's own doing
        self._down = {}                     # name: when its state stopped coming
        self._rf_since = None
        self.trouble = None                 # this test's GUI or RF trouble: it fails the test
        self._pids = {}                     # (name, before/after): the GUI's process around a silence
        self._unplanned = {}                # name: its silence began with no planned restart
        self._role_said = {}                # name: its "transmits but its role is RX" said for this over
        self.sql = None                     # the squelch both are held at (squelch_cal: closed on the bench's quiet)
        self.roles = {A: "RX", B: "RX"}
        self.restore_calls = set()          # the radios given a test call this run (test_calls)
        self.changing = 0                   # changes to both under way: the guard looks away meanwhile
        self.violation = None               # this test's first sync violation

        self._diff_since = None
        self._trail = collections.deque(maxlen=int(TRAIL_S / GUARD_POLL_S))   # both radios' last TRAIL_S, said with a
                                                                             # violation: what moved them apart
        self.on = True
        threading.Thread(target=self._guard, daemon=True).start()

    def snap(self, label):
        """Both screens photographed now (in a thread: the guard does not wait), kept for the console and the run"""
        def go():
            try:
                paths = bench.screenshots_both(list(self.r.values()), label.replace(" ", "_")[:60])
            except Exception as e:          # a picture never stops a test
                paths = {}
                self.st.ev(f"pictures not taken: {e}", "radio")
            with self.st.lock:                  # the test running now keeps them (shown if it fails)
                if self.st.cur is not None:
                    self.st.tests[self.st.cur]["shots"].append({"t": time.time(), "label": label, "paths": paths})
            self.st.ev(f"pictures: {label}: {', '.join(os.path.basename(p) for p in paths.values() if p)}", "conf")
        threading.Thread(target=go, daemon=True).start()

    def _pid_now(self, n, when):
        try:
            self._pids[(n, when)] = bench.ssh(self.r[n].host, "pidof x6100_gui", timeout=15).strip()
        except (Refused, OSError, Exception):
            self._pids[(n, when)] = None

    def _stall_or_crash(self, n, gone):
        """After a radio's silence: its GUI's process the same as when the silence began (a link stall: said, the test
        not failed for it) or another (its GUI died and was started again: the test fails)"""
        t0 = time.time()
        while (n, "before") not in self._pids and time.time() - t0 < 20:
            time.sleep(0.2)
        self._pid_now(n, "after")
        a_, b_ = self._pids.pop((n, "before"), None), self._pids.pop((n, "after"), None)
        if a_ is None:                      # not read as it began: nothing to judge by, said
            self.st.ev(f"{n}: back after {gone:.0f} s; its GUI's process not read as the silence began", "radio")
            return
        if a_ and b_ and a_ == b_:
            self.st.ev(f"{n}: its link stalled {gone:.0f} s, its GUI the same process ({b_}): not a crash", "radio")
            if self.trouble and self.trouble.startswith(f"{n}: NO WORD"):
                self.trouble = None
        else:
            what = f"{n}: its GUI RESTARTED by itself (process {a_} -> {b_}): a crash"
            self.st.ev(what, "bad")
            self.trouble = what

    def hold(self):
        """A PTT press's own number (the remote's lease: each press a new, higher one, its renewals the same; a number
        not above the last is taken for a late renewal of an ended press and does nothing, remote_server.c ptt_down)"""
        self._hold = max(getattr(self, "_hold", 0) + 1, int(time.time() * 10) % 2 ** 31)
        return self._hold

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
                age = now - r.last_msg
                link = "live" if age < STALE_S else (f"GUI RESTARTING ({self.planned[n]})" if n in self.planned
                                                     else "NO STATE: GUI DOWN?")
                rs[n] = {"freq": s.get("freq"), "mode": s.get("mode"), "tx": bool(r.tx), "pwr": s.get("pwr"),
                         "pg": s.get("pg"), "rxo": s.get("rxo"), "meter": r.meter, "role": self.roles.get(n),
                         "age": round(age, 1), "link": link, "sql": s.get("sql"), "vol": s.get("vol"),
                         "app": s.get("app") or s.get("win")}
                if age >= STALE_S and n not in self._down:
                    self._down[n] = now
                    self._unplanned[n] = n not in self.planned     # only a silence nobody planned is judged
                    if n in self.planned:
                        self.st.ev(f"{n}: its GUI restarting ({self.planned[n]}): no state meanwhile", "radio")
                    else:
                        what = f"{n}: NO WORD for {age:.0f} s: its GUI down or its link lost, not by the orchestrator"
                        self.st.ev(what, "bad")
                        self.trouble = self.trouble or what
                        threading.Thread(target=self._pid_now, args=(n, "before"), daemon=True).start()
                elif age < STALE_S and n in self._down:
                    gone = now - self._down.pop(n)
                    self.st.ev(f"{n}: state again after {gone:.0f} s (GUI {hello(r)})", "ok")
                    if self._unplanned.pop(n, False):   # the GUI the same process: its link stalled (the X6200's WiFi
                        threading.Thread(target=self._stall_or_crash, args=(n, gone), daemon=True).start()
            for n, r in self.r.items():             # a radio on the air that should be listening: stopped, photographed
                if r.tx and self.roles.get(n) == "RX" and not self.changing and not self._role_said.get(n):
                    self._role_said[n] = True
                    what = f"{n} TRANSMITS but its role is RX: both stopped"
                    self.st.ev(what, "bad")
                    self.trouble = self.trouble or what
                    for x in self.r.values():
                        try:
                            x.stop()
                        except OSError:
                            pass
                    self.snap(f"ANOMALY: {what}")
                elif not r.tx:
                    self._role_said[n] = False
            # a signal on a receiver that no radio says it sends (a transmitter keyed with its GUI down or its state
            # wrong): an alarm and both stopped, whatever the GUIs say
            keyed = any(r.tx for r in self.r.values()) or any(v != "RX" for v in self.roles.values())
            loud = {n: r.meter for n, r in self.r.items() if r.meter is not None and r.meter > RF_ALARM_DBM}
            if loud and not keyed and not self.changing:
                if self._rf_since is None:
                    self._rf_since = now
                elif now - self._rf_since >= RF_ALARM_S:
                    what = f"SIGNAL NOBODY SENDS: {loud} dBm with neither radio saying it transmits: both stopped"
                    self.st.ev(what, "bad")
                    self.trouble = self.trouble or what
                    for r in self.r.values():
                        try:
                            r.stop()
                        except OSError:
                            pass
                    self.snap(f"ANOMALY: {what}")
                    self._rf_since = now + 30            # said once a half minute
            elif not loud:
                self._rf_since = None
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
            # the one registry (bench.apart): a difference passes only when a running test declared it, exactly; a
            # change under way (bench.transition, a dial still within DIAL_FOLLOW_S for the other to follow, or a
            # change orch makes to both) is not a difference
            freq_ok = a["freq"] == b["freq"] or bench.declared("freq", a["freq"], b["freq"])
            mode_ok = a["mode"] == b["mode"] or bench.declared("mode", a["mode"], b["mode"])
            differ = not (freq_ok and mode_ok)
            moving = self.changing > 0 or bench.changing()
            cur = tuple((x["freq"], x["mode"], x["pg"], x["app"]) for x in (a, b)) + (moving,)
            if not self._trail or self._trail[-1][1:] != cur:
                self._trail.append((now,) + cur)
            if differ:
                bench.dial_apart()          # seen apart within the follow window: alike again closes it
            elif not self.changing:
                bench.dial_settled()        # alike again (after apart): the follow window closed
            with self.st.lock:
                self.st.radios = rs
                self.st.sync["changing"] = moving
                self.st.sync["declared"] = [f"{', '.join(f'{k} {v}' for k, v in sp.items())}: {why}"
                                            for sp, why in bench.declarations()]
                if moving:
                    self._diff_since = None
                elif differ:
                    if self._diff_since is None:
                        self._diff_since = now
                    elif now - self._diff_since >= GUARD_S and self.st.sync["ok"]:
                        what = f"{A} {a['freq']} mode {a['mode']}, {B} {b['freq']} mode {b['mode']}"
                        self.st.sync.update(ok=False, since=now, last=what)
                        self.st.sync["violations"] += 1
                        self.violation = self.violation or what
                        trail = (f"the {TRAIL_S:.0f} s before it ({A}; {B}; a change under way): " + " | ".join(
                            f"{time.strftime('%H:%M:%S', time.localtime(t))}.{int(t % 1 * 10)} "
                            f"{ra[0]} m{ra[1]} {ra[2]} {ra[3] or ''}; {rb[0]} m{rb[1]} {rb[2]} {rb[3] or ''}"
                            f"{' (changing)' if mv else ''}" for t, ra, rb, mv in
                            [x for x in self._trail if x[0] >= now - TRAIL_S]))
                        # said once out of the lock (ev takes it): in the run's events.jsonl and the log
                        threading.Thread(target=lambda w=what, tr=trail: (self.st.ev(f"OUT OF SYNC: {w}", "bad"),
                                                                          self.st.ev(tr, "conf")),
                                         daemon=True).start()
                        self.snap(f"OUT OF SYNC: {what}")
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
        only_one = [f"{k} {v}" for k, v in want.items() if k in ("freq", "mode", "ifs") and not common(k, v)]
        if only_one:                        # the caps map: a value only one radio takes would part them
            raise Refused(f"not on both radios (the caps map): {', '.join(only_one)}")
        self.changing += 1
        self.st.ev(f"set on both: {want}", "conf")
        try:
            try:
                bench.sync_pair([self.r[A], self.r[B]], want, checks)     # the one sync: both at once, compared
            finally:
                self.st.ev("read back: " + "; ".join(f"{n} " + ", ".join(f"{k} {bench._value(r, k)}" for k in want)
                                                     for n, r in self.r.items()), "conf")
        finally:
            self.changing -= 1
        return {k: bench._value(self.r[A], k) for k in want}

    SQL_TRY = (2, 4, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60)

    def squelch_cal(self):
        """The lowest squelch that silences both radios' quiet (their TCI audio with nothing sent: 20 dB under its
        level with the squelch open, or -60 dBFS), the volume 0 on both: the level every test then holds (the owner,
        2026-11-23: "make sure squelch is at a reasonable level before each test", "turn the volume down")"""
        from tones import TciRx
        rx = {n: TciRx(r.host) for n, r in self.r.items()}

        def level(n):
            t0 = time.time()
            time.sleep(1.5)
            x, _ = rx[n].audio(t0, time.time())
            return 20 * math.log10(float(np.sqrt(np.mean(x ** 2))) + 1e-9) if len(x) else 0.0
        try:
            time.sleep(1.0)
            self.both({"vol": 0, "sql": 0})
            time.sleep(1.0)
            open_ = {n: level(n) for n in self.r}
            curve = []
            for q in self.SQL_TRY:
                self.both({"sql": q})
                time.sleep(1.0)
                now = {n: level(n) for n in self.r}
                curve.append(f"{q}: " + "/".join(f"{now[n]:.0f}" for n in sorted(self.r)))
                if all(now[n] <= max(open_[n] - 20, -60) for n in self.r):     # 20 dB down, or silent
                    self.sql = q
                    break
            self.st.ev("squelch tried (dBFS of the quiet, " + "/".join(sorted(self.r)) + "): open "
                       + "/".join(f"{open_[n]:.0f}" for n in sorted(self.r)) + "; " + ", ".join(curve), "conf")
        finally:
            for c in rx.values():
                c.close()
        if self.sql is None:
            self.sql = 10
            self.st.ev(f"no squelch up to {self.SQL_TRY[-1]} silenced the quiet ({open_} dBFS open): held at 10 (the TCI audio may not follow the squelch)", "bad")
        else:
            self.st.ev(f"squelch {self.sql} silences the quiet on both (open: {({n: round(v) for n, v in open_.items()})} "
                       f"dBFS, at {self.sql}: {({n: round(v) for n, v in now.items()})} dBFS); volume 0 on both", "ok")
        self.both({"sql": self.sql})

    def ready(self):
        """Before every test: neither keyed, both transmitters locked, the two on one frequency and mode. What is not
        is put right (both locked, both on page G on the first radio's frequency and mode); what cannot be stops the
        run: no test goes on from radios set wrong"""
        rs = self.r.values()
        for n, r in self.r.items():         # a GUI that died (or a link that dropped): connected again first, the run
            if r.stale(STALE_S):            # goes on (the owner: "keep the tests running")
                self.st.ev(f"{n}: no word from it: connecting again (its GUI may have died)", "bad")
                t0 = time.time()
                while True:
                    try:
                        r.reconnect()
                        break
                    except (Refused, OSError):
                        if time.time() - t0 > 120:
                            raise Refused(f"{n}: not back within 2 minutes: the run stops")
                        time.sleep(3)
                self.st.ev(f"{n}: connected again (GUI {hello(r)})", "ok")
        bad = []
        if any(r.tx for r in rs):
            bad.append("a radio on the air")
        if any(r.state.get("rxo") == 0 for r in rs):
            bad.append("a transmitter armed")
        if (self.r[A].state.get("freq"), self.r[A].state.get("mode")) != \
                (self.r[B].state.get("freq"), self.r[B].state.get("mode")):
            bad.append("the two apart")
        if not bad and self.sql is not None and any(int(r.state.get("sql", -1)) != self.sql or
                                                    int(r.state.get("vol", -1)) != 0 for r in rs):
            self.both({"sql": self.sql, "vol": 0})      # the squelch and the volume as calibrated, before every test
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

    PAIR = ("filter", "pre", "att", "agc", "rfg", "sql", "nb", "nr", "rit", "split", "zoom")

    def pair_alike(self, ref=A):
        """Every receiver setting the Watch compares (bench.PAIR_KEYS) made the same on both: ref's values set on both,
        each confirmed (an app's filter per mode had parted them: the X6100's USB-D from 100 Hz, the X6200's from 200,
        2026-11-23, and the Watch stopped the FT8 QSO before it began)"""
        want = {k: bench._value(self.r[ref], k) for k in self.PAIR}
        want = {k: v for k, v in want.items() if v is not None and v != (-1, -1)}
        self.both(want)

    def apps_closed(self, f, m):
        """The apps closed on both and both on the Sentry's G on f in m, in one guarded change"""
        self.changing += 1
        try:
            for r in self.r.values():
                r.stop()
                bench.close_apps(r)
            self.both_page("G")
            self.both({"freq": f, "mode": m})
        finally:
            self.changing -= 1

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
            self.st.ev(f"{n}: its own call back, PSK Reporter as found: its GUI restarts (planned)", "radio")
            self.planned[n] = "planned: own call back"
            subprocess.run([sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"),
                            "testcall", n, "restore"], capture_output=True, timeout=120)
            t0 = time.time()
            while time.time() - t0 < 90:
                try:
                    self.r[n].reconnect()
                    break
                except (Refused, OSError):
                    time.sleep(3)
            self.planned.pop(n, None)
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


SPEC_LO, SPEC_HI = 100, 3300   # Hz from the dial the spectrum judge looks in (a step 380 Hz low lands at 220 Hz)
SPEC_TOL_HZ = 150           # a step in a fine spectrum within this of its tone + the radios' offset: the X6100's frames
                            # are 1024 bins over 100 kHz (it keeps zoom 1 there), drawn in pairs: ~195 Hz a real bin;
                            # its steps came within 120 Hz (2026-11-23, 6 overs saved)
SPEC_MIN_DB = 6             # a step's own energy over the keyed silence around the sequence, at least
SPEC_MIN_DB_COARSE = 3      # in a coarse spectrum (the X6100's 600 Hz, its TX ~15 dB down there, came 6.0 dB; quiet
                            # frames give at most 1.5)
SPEC_CARRIER_DB = 15        # AM: the carrier over the frames before the sender keyed
# The radios whose spectrum cannot place a tone: the X6200 sends its base's own spectrum (dsp_psd: ~750 Hz bins, the
# zoom only stretches them), one tone a hump ~2-3 kHz wide. There the steps' centres must rise in order; their exact
# frequencies come from its audio
SPEC_COARSE = {B: "its base's own spectrum (bins ~750 Hz wide, stretched over the zoom)"}


def spec_steps(L, f, mode, on, off, coarse=False):
    """The stepped sequence in the listener's own spectrum (the frames its screen draws, ~8 a second) while the
    sender was on the air (on..off, the sender's own state). The sequence's start is fitted (the tones begin after the
    lead and the transmitter's latency, 0.7-0.9 s here): each step's frames (trimmed 0.06 s) averaged, the keyed
    frames outside the sequence (the lead and tail: a carrier leak, the transmitter's noise) taken off, the centre of
    what is left (the bins within 3 dB of its top, SPEC_LO..SPEC_HI from the dial on the mode's side). The start that
    best fits the plan is kept.
      fine (the X6100): every step within SPEC_TOL_HZ of its tone + one offset (at most STEP_MAX_OFFSET_HZ) and
        SPEC_MIN_DB up; AM: the carrier SPEC_CARRIER_DB over the quiet (its sidebands at 0.1 W stay under the carrier's
        skirt in these bins; their tones are judged in the audio)
      coarse (SPEC_COARSE): every step SPEC_MIN_DB up and the centres rising in order; AM: the carrier
    Returns a dict: ok, kind ('steps', 'order' or 'carrier'), says, frames, start (s after on), offset, steps
    [(Hz heard, its error, dB)]"""
    fr = [(t, cen, wid, v) for (t, cen, wid, v) in list(L.r.spectra) if on - 1.5 <= t <= off + 1.0 and v]
    keyed = [x for x in fr if on + 0.05 <= x[0] <= off - 0.05]
    out = {"ok": False, "kind": None, "frames": len(keyed), "start": None, "offset": None, "steps": []}
    if len(keyed) < 12:
        out["says"] = f"{len(keyed)} spectrum frames while on the air"
        return out
    nb = len(keyed[0][3])
    fr = [x for x in fr if len(x[3]) == nb and x[1] == keyed[0][1] and x[2] == keyed[0][2]]
    _, cen, wid, _ = keyed[0]
    per = wid / nb
    sign = -1 if mode in (0, 1) else 1
    offs = ((cen - wid / 2 + np.arange(nb) * per) - f) * sign      # bin i at centre - width/2 + i * width/n (dsp.h)
    T = np.array([x[0] for x in fr])
    P = 10 ** (np.array([x[3] for x in fr], float) / 10)
    k_m = (T >= on + 0.05) & (T <= off - 0.05)
    q_m = T < on - 0.1
    if mode == 6:                                                   # AM: the carrier, the reference
        ka = P[k_m].mean(axis=0)
        near = np.where(np.abs(offs) <= STEP_MAX_OFFSET_HZ)[0]
        ic = near[int(np.argmax(ka[near]))]
        quiet = P[q_m].mean(axis=0)[ic] if q_m.any() else np.median(ka)
        up = 10 * math.log10(ka[ic] / max(quiet, 1e-30))
        out.update(kind="carrier", offset=round(float(offs[ic])), carrier_db=round(up, 1),
                   ok=up >= SPEC_CARRIER_DB,
                   says=f"the carrier at {offs[ic]:+.0f} Hz from the dial, {up:.0f} dB over the quiet before the "
                        f"over (want {SPEC_CARRIER_DB}); its sidebands are judged in the audio")
        return out
    band = (offs >= SPEC_LO) & (offs <= SPEC_HI)
    n = len(STEP_HZ)
    best = None
    for ts in np.arange(on - 0.2, on + 1.5, 0.02):
        st = []
        for k in range(n):
            m = (T >= ts + k * STEP_S + 0.06) & (T <= ts + (k + 1) * STEP_S - 0.06)
            if not m.any():
                break
            st.append(P[m].mean(axis=0))
        if len(st) < n:
            continue
        bm = k_m & ~((T >= ts - 0.1) & (T <= ts + n * STEP_S + 0.1))
        base = P[bm].mean(axis=0) if bm.sum() >= 2 else np.min(st, axis=0)
        got = []
        for sp_ in st:
            lin = np.where(band, np.clip(sp_ - base, 0, None), 0)
            if lin.max() <= 0:
                break
            w = np.where(lin >= lin.max() * 0.5, lin, 0)
            i = int(np.argmax(lin))
            got.append(((w * offs).sum() / w.sum(), 10 * math.log10(sp_[i] / max(base[i], 1e-30))))
        if len(got) < n:
            continue
        c = np.array([g[0] for g in got])
        o_ = float(np.median(c - np.array(STEP_HZ)))
        err = c - np.array(STEP_HZ) - o_
        sse = float(np.sum(err ** 2))
        if best is None or sse < best[0]:
            best = (sse, ts - on, o_, got, err)
    if best is None:
        out["says"] = "no start fits: a step without frames or without energy over the keyed silence"
        return out
    _, ts, o_, got, err = best
    out.update(start=round(float(ts), 2), offset=round(o_),
               steps=[(round(g[0]), round(float(e)), round(g[1], 1)) for g, e in zip(got, err)])
    weak = [h for h, g in zip(STEP_HZ, got) if g[1] < (SPEC_MIN_DB_COARSE if coarse else SPEC_MIN_DB)]
    if coarse:
        rising = all(a[0] < b[0] for a, b in zip(got, got[1:]))
        out.update(kind="order", ok=rising and not weak,
                   says=("every step up and their centres rising in order" if rising and not weak else
                         (f"steps {weak} under {SPEC_MIN_DB_COARSE} dB" if weak else "the centres not rising in order"))
                   + f" ({SPEC_COARSE.get(L.name, 'a coarse spectrum')}: the exact tones from its audio)")
        return out
    far = [h for h, e in zip(STEP_HZ, err) if abs(e) > SPEC_TOL_HZ]
    out.update(kind="steps", ok=not far and not weak and abs(o_) <= STEP_MAX_OFFSET_HZ,
               says=("every step at its tone" if not far and not weak else
                     ", ".join(x for x in (f"steps {far} more than {SPEC_TOL_HZ} Hz off" if far else "",
                                           f"steps {weak} under {SPEC_MIN_DB} dB" if weak else "") if x))
               + (f"; the offset {o_:+.0f} Hz more than {STEP_MAX_OFFSET_HZ}" if abs(o_) > STEP_MAX_OFFSET_HZ else ""))
    return out


def file_name(label):
    """A test's label as a file name in its run's folder"""
    return "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in label)


def save_wav(o, x, name):
    """Received audio (TciRx, 24 kHz float) kept in the run's folder as a 16-bit WAV, the evidence of what the listener
    heard: its path, None without a folder. (One definition: a second one with the arguments the other way round,
    11cb8f8b, took the place of this one and the step tests' call would have failed on its next run)"""
    import wave
    d = getattr(o.st, "run_dir", None)
    if not d:
        return None
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
    return path


def save_spectrum(o, label, L, f, mode, on, off):
    """The listener's spectrum frames around an over (1 s either side), kept in the run's folder: the judge can be
    replayed on them (orch.py spec FILE)"""
    frames = [(t, cen, wid, list(v)) for (t, cen, wid, v) in list(L.r.spectra) if on - 1.0 <= t <= off + 1.0 and v]
    path = os.path.join(o.st.run_dir, file_name(label) + "_spectrum.json")
    with open(path, "w") as fh:
        json.dump({"label": label, "listener": L.name, "f": f, "mode": mode, "on": on, "off": off,
                   "frames": frames}, fh)
    return os.path.basename(path)


def spec_track(L, f, mode, t0, t1):
    """Per frame in t0..t1 the strongest bin SPEC_LO..SPEC_HI Hz from the dial on the mode's side: (t, Hz, dB over the
    frame's median), the judge's own first look, logged when it finds nothing"""
    sign = -1 if mode in (0, 1) else 1
    out = []
    for t, cen, wid, v in list(L.r.spectra):
        if not (t0 <= t <= t1) or not v:
            continue
        per = wid / len(v)
        med = sorted(v)[len(v) // 2]
        cand = [(v[i], ((cen - wid / 2 + (i + 0.5) * per) - f) * sign) for i in range(len(v))]
        cand = [c for c in cand if SPEC_LO <= c[1] <= SPEC_HI]
        if cand:
            db, a = max(cand)
            out.append((t, a, db - med))
    return out


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
            # AM: the squelch open for the over: at 10 both radios' AM audio stayed closed 1.3-2 s into a carrier 57 dB
            # up (am-steps, 2026-11-23; at 0 both passed); ready() puts SQL_HOLD back before the next test
            if mode == 6 and o.sql:
                o.st.ev(f"AM: the squelch 0 on both for this over (held at {o.sql} it keeps AM audio closed seconds into "
                        f"a carrier), {o.sql} again before the next test", "conf")
            want, checks = bench.rx_profile(f, mode, pre=0, att=0, rfg=100, agc=agc, sql=0 if mode == 6 else (o.sql or 0),
                                            filt=(100, 3000) if mode in (0, 1, 2, 3) else None)
            # zoom 8: the spectrum 12.5 kHz wide, 12 Hz a bin, each tone where it should be told apart
            o.both(dict(want, pwr=w, atu=0, xit=0, zoom=8), checks)
    with o.step("every receiver setting the Watch compares alike on both"):
        o.pair_alike()
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
    for sh in watch.shots:
        with o.st.lock:
            if o.st.cur is not None:
                o.st.tests[o.st.cur]["shots"].append({"t": time.time(), "label": f"{label}: {sender} on the air"
                                                      + ("" if sh.get("on_air") else " (NOT on the air in the picture)"),
                                                      "paths": sh.get("paths") or {}})
    o.st.ev(f"over {label}: {sender}'s meter at most {watch.tx_peak.get(sender)} W, {listener}'s S-meter at most "
            f"{watch.rx_peak} dBm, heard {watch.heard_db.get(sender) if hasattr(watch, 'heard_db') else None} dB, "
            f"deaf overs {len(watch.deaf)}" + (f", stopped: {watch.why}" if watch.why else ""), "conf")
    if watch.why:
        raise Refused(f"the watch stopped the over: {watch.why}")
    return watch


def audio_steps(o, sender, listener, f, mode, label):
    """The stepped sequence sent by sender's TCI on f in mode, judged twice at the listener: in its own spectrum (the
    frames its screen draws, spec_steps: what that spectrum can show) and in its own audio (its TCI, find_steps: every
    step's exact tone, in order). PASS when both find it; one without the other is a LOOK (both said); neither a FAIL.
    FM is judged by its audio (its sidebands spread with the deviation). The spectrum frames and the audio are kept in
    the run's folder (orch.py spec FILE replays the spectrum judge). Returns (ok, says, data)"""
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
                # at 0.1 W the X6100's meter (0.1 W steps) can read 0 through a whole over: the listener's S-meter
                # over RF_ALARM_DBM proves the power then
                if not watch.tx_peak.get(sender) and not (watch.rx_peak is not None and watch.rx_peak > RF_ALARM_DBM):
                    raise Refused(f"{sender}'s meter showed no power, and {listener}'s S-meter at most "
                                  f"{watch.rx_peak} dBm")
        over(o, sender, listener, f, mode, label, body)
        time.sleep(1.0)                     # the audio's and the spectrum's last frames
        S = o.r[sender]
        on, off = S.tx_last if S.tx_last else got["t"]
        sp = None
        if mode != 7:
            coarse = listener in SPEC_COARSE
            try:
                with o.step(f"{listener}'s own spectrum: " + ("the carrier" if mode == 6 else
                            "every step up, their centres rising in order" if coarse else
                            f"every step at the dial {'-' if mode in (0, 1) else '+'} its tone (within "
                            f"{SPEC_TOL_HZ} Hz)") + f", while {sender} was on the air") as s:
                    sp = spec_steps(L, f, mode, on, off, coarse=coarse)
                    sp["saved"] = save_spectrum(o, label, L, f, mode, on, off)
                    got["spectrum"] = sp
                    detail = (f"{sp['says']}; start {sp['start']:+.2f} s after on, offset {sp['offset']:+d} Hz; per step "
                              f"(Hz heard, error, dB): {sp['steps']}" if sp.get("steps") else sp["says"])
                    if not sp["ok"]:
                        o.st.ev(f"{listener}'s spectrum, per frame the strongest bin {SPEC_LO}-{SPEC_HI} Hz from the "
                                f"dial (s from {sender}'s on, Hz, dB over the frame's median): "
                                + " ".join(f"{t - on:.1f}/{a:.0f}/{h:.0f}"
                                           for t, a, h in spec_track(L, f, mode, on - 0.2, off + 0.5)), "bad")
                        raise Refused(f"{listener}'s spectrum ({sp['frames']} frames on the air): {detail}")
                    s.confirm(detail)
            except Refused:
                pass
        audio_ok, audio_says = False, ""
        try:
            with o.step(f"{listener} heard every step, in order, at its tone (its audio)") as s:
                t_on, t_off = got["t"]
                x, _ = rx.audio(t_on - 0.5, t_off + 1.5)
                if len(x) < FS:
                    raise Refused(f"{listener}'s TCI sent {len(x) / FS:.1f} s of audio")
                found, off_a, steps, seen = find_steps(x)
                got.update(found=found, offset=off_a, steps=steps, seen=seen, wav=save_wav(o, x, file_name(label) + ".wav"))
                if not found:
                    raise Refused(f"the sequence not heard whole in {listener}'s audio: per step (Hz, dB over the "
                                  f"noise, s) {steps or 'no anchor'}; clear tones (Hz, s) {seen[:8]}")
                spread = max(d for _, d, _ in steps) - min(d for _, d, _ in steps)
                got["spread_db"] = round(spread, 1)
                audio_says = (f"audio: all {len(STEP_HZ)} at their tones in order, offset {off_a:+d} Hz, weakest "
                              f"{min(d for _, d, _ in steps)} dB, spread {spread:.1f} dB")
                s.confirm(audio_says)
                audio_ok = True
        except Refused as e:
            audio_says = f"audio: {e}"
    except Refused as e:
        return False, str(e), got
    finally:
        rx.close()
    if sp is None:                          # FM
        return (True, "FM (judged by its audio): " + audio_says, got) if audio_ok else (False, audio_says, got)
    spec_says = f"spectrum ({sp['kind'] or 'nothing'}): {sp['says']}"
    if sp["ok"] and audio_ok:
        return True, f"{spec_says}; {audio_says}", got
    if sp["ok"] or audio_ok:
        return None, f"{'only the spectrum' if sp['ok'] else 'only the audio'} found it: {spec_says}; {audio_says}", got
    return False, f"{spec_says}; {audio_says}", got


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
            got["keyed_at_s"] = round(time.time() - rec_t0, 2)
            proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                     f"python3 /tmp/tci_cw_tx.py '{CW_TEXT}' 14"], stdout=subprocess.PIPE, text=True)
            while proc.poll() is None:
                if watch.why:
                    S.stop()
                    raise Refused(watch.why)
                time.sleep(0.05)
            time.sleep(1.0)
            if not watch.tx_peak.get(sender):
                raise Refused(f"{sender}'s meter showed no power")
    # The listener's audio from before the set-up to the end, kept (tools/cw_replay replays it through the decoder):
    # 2026-11-25 the X6200 printed the X6100's VVV as 'TTTT TTTT TTTT', its decoder some 50 WPM fast at the over's
    # start, which the over's audio alone, replayed, did not make. Its decoder gets audio only in CW: the switch to CW
    # and the receiver's settings just before the over are in it (got's keyed_at_s: where the sender began)
    from tones import TciRx
    rec, rec_t0 = TciRx(L.host), time.time()
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
    finally:
        try:
            got["wav"] = save_wav(o, rec.audio(rec_t0, time.time())[0], f"cw_text_{sender}_to_{listener}.wav")
        finally:
            rec.close()
    return True, f"decoded {got['decoded']!r} ({got['cer']:.0%} of the characters wrong)", got


CW_QUIET_ROUNDS = 3         # cw-quiet: switches from USB-D to CW
CW_QUIET_USB_S = 15.0       # each round's USB-D (the decoder fed nothing: it gets audio only in CW)
CW_QUIET_S = 12.0           # then CW's quiet, as cw-carrier's after its path proof
CW_QUIET_FILTER = (300, 1100)   # air_b's CW filter (CW_FILTER there): through it the X6200 printed its noise


def t_cw_quiet(o, filt=CW_QUIET_FILTER):
    """Both radios' CW decoders in the cable's quiet, nothing sent by anyone: each round both in USB-D on F_CW for
    CW_QUIET_USB_S, then both in CW (air_b's listener: the 300-1100 Hz filter, AGC slow) for CW_QUIET_S, and once more
    in CW without the switch (filt: CW's filter, air_b's by default; "cw-quiet wide" 100-1300 Hz, the X6200's resonance
    model 5 dB under its noise at 150 Hz and the X6100's noise 9-15 dB up under 300 Hz, cw-noise-shape). PASS when
    neither decoder printed a character. Both radios' TCI audio is kept from before
    the first switch, the switches' times in the result (tools/cw_replay replays it through the decoder): 2026-11-25
    the X6200 printed noise in cw-carrier's 12 s quiet after the path proof's USB-D, which the quiet's audio alone,
    replayed from cold, did not"""
    from tones import TciRx
    # the squelch open (the base's: held closed it would give the decoders silence); ready() holds it again after
    want_u, checks = bench.rx_profile(F_CW, 3, pre=0, att=0, rfg=100, agc=2, sql=0)
    want_c, _ = bench.rx_profile(F_CW, 4, pre=0, att=0, rfg=100, agc=2, sql=0, filt=filt)
    with o.step(f"both on page G, {F_CW / 1e6:.3f} MHz USB-D, the receiver alike"):
        o.both_page("G")
        o.both(dict(want_u, zoom=8), checks)
    rec = {n: TciRx(r.host) for n, r in o.r.items()}
    t_rec, got = time.time(), {"switch_s": [], "printed": []}
    try:
        for k in range(CW_QUIET_ROUNDS + 1):
            stay = k == CW_QUIET_ROUNDS
            if not stay:
                with o.step(f"round {k + 1}: both in USB-D {CW_QUIET_USB_S:.0f} s (their decoders fed nothing)"):
                    if k:
                        o.both(want_u, checks)
                    time.sleep(CW_QUIET_USB_S)
            what = ("both stay in CW" if stay else f"round {k + 1}: both to CW (filter {filt[0]}-{filt[1]} Hz, AGC slow)")
            with o.step(f"{what}, {CW_QUIET_S:.0f} s of quiet: neither decoder prints a character") as s:
                for r in o.r.values():
                    r.cw = ""
                if not stay:
                    o.both(want_c, checks)
                    got["switch_s"].append(round(time.time() - t_rec, 2))
                time.sleep(CW_QUIET_S)
                said = {n: " ".join(r.cw.split()) for n, r in o.r.items()}
                got["printed"].append(said)
                s.confirm("; ".join(f"{n} {v!r}" for n, v in said.items()))
    except Refused as e:
        return False, str(e), got
    finally:
        for n, c in rec.items():
            try:
                got[f"wav_{n}"] = save_wav(o, c.audio(t_rec, time.time())[0], f"cw_quiet_{filt[0]}_{filt[1]}_{n}.wav")
            finally:
                c.close()
    noisy = [(i, n, v) for i, said in enumerate(got["printed"]) for n, v in said.items() if v]
    words = "; ".join(f"{'stay' if i == CW_QUIET_ROUNDS else f'round {i + 1}'} {n} {v!r}" for i, n, v in noisy)
    if noisy:
        return False, f"decoders printed in the quiet: {words} (switches at {got['switch_s']} s of the recordings)", got
    return True, (f"{CW_QUIET_ROUNDS} switches from USB-D to CW and a stay in CW through {filt[0]}-{filt[1]} Hz, "
                  f"{CW_QUIET_S:.0f} s of quiet each: neither decoder printed a character"), got


CW_SHAPE_FILTERS = [(300, 1100), (500, 900), (600, 800), (200, 1200), (100, 1300)]   # centred on the 700 Hz key tone (CW's filters are)
CW_SHAPE_S = 6.0            # cw-noise-shape: each filter's quiet, both radios at once


def noise_shape(x, fs, lo, hi):
    """The noise's shape in a quiet (welch, 5 Hz bins) inside the CW filter's lo-hi: dB over the band's 25th
    percentile (the CW decoder's floor, src/cw_decoder/tone_finder.cpp) at every 50 Hz, its peak, and its width
    within 6 dB of the peak"""
    import numpy as np
    from scipy.signal import welch
    f, p = welch(x, fs, nperseg=int(fs / 5))
    band = (f >= max(lo, 150)) & (f <= hi)
    ref = np.percentile(p[band], 25)
    db = 10 * np.log10(p / ref + 1e-12)
    top = int(np.argmax(np.where(band, db, -1e9)))
    near = band & (db >= db[top] - 6)
    return {"peak_hz": round(float(f[top])), "peak_db": round(float(db[top]), 1),
            "width6_hz": round(float(f[near].max() - f[near].min())) if near.any() else 0,
            "at": {int(q): round(float(np.mean(db[(f >= q - 25) & (f < q + 25)])), 1)
                   for q in range(int(lo // 50 * 50 + 50), int(hi), 50) if q >= 150}}


def t_cw_noise_shape(o):
    """The receive noise's shape in CW on both radios, nothing sent: through each of CW_SHAPE_FILTERS (both alike,
    the key tone as each radio has it; and CW-R through the first), CW_SHAPE_S of the cable's quiet in each one's TCI audio: the shape over the
    band's 25th percentile, the CW decoder's floor. Measured, said: 2026-11-25 the X6200's rose 17 dB at its 700 Hz key
    tone through 300-1100 Hz (its base's CW filter), the X6100's flat within 5 dB, and the X6200's decoder printed its
    noise in quiets (cw-quiet). PASS when every filter was measured on both"""
    from tones import TciRx
    with o.step(f"both on page G, {F_CW / 1e6:.3f} MHz CW, the receiver alike"):
        o.both_page("G")
        want, checks = bench.rx_profile(F_CW, 4, pre=0, att=0, rfg=100, agc=2, sql=0)
        o.both(dict(want, zoom=8), checks)
    kt = {n: int(r.state.get("kt") or 0) for n, r in o.r.items()}
    rec = {n: TciRx(r.host) for n, r in o.r.items()}
    shapes = {}
    try:
        time.sleep(1.5)                     # the streams started
        for mode, (lo, hi) in [(4, f) for f in CW_SHAPE_FILTERS] + [(5, CW_SHAPE_FILTERS[0])]:
            name = "CW" if mode == 4 else "CW-R"
            with o.step(f"both in {name} through {lo}-{hi} Hz, {CW_SHAPE_S:.0f} s of quiet each, measured") as s:
                o.both({"mode": mode, "filter": (lo, hi)} if mode == 5 else {"filter": (lo, hi)})
                time.sleep(1.5)             # the base's filter and AGC settled
                t0 = time.time()
                time.sleep(CW_SHAPE_S)
                row = {}
                for n, c in rec.items():
                    x, _ = c.audio(t0, time.time())
                    if len(x) < FS * CW_SHAPE_S * 0.8:
                        raise Refused(f"{n}: {len(x) / FS:.1f} s of audio in {CW_SHAPE_S:.0f} s")
                    row[n] = noise_shape(x, FS, lo, hi)
                shapes[f"{lo}-{hi}" + (" CW-R" if mode == 5 else "")] = row
                s.confirm("; ".join(f"{n} peak {v['peak_db']} dB at {v['peak_hz']} Hz, {v['width6_hz']} Hz wide"
                                    for n, v in row.items()))
    except Refused as e:
        return False, str(e), {"kt": kt, "shapes": shapes}
    finally:
        for c in rec.values():
            c.close()
    words = "; ".join(f"{k}: " + ", ".join(f"{n} {v['peak_db']} dB at {v['peak_hz']}" for n, v in row.items())
                      for k, row in shapes.items())
    return True, f"key tones {kt} Hz; the noise's peak over the band's 25th percentile, per filter: {words}", \
        {"kt": kt, "shapes": shapes}


KT_TRY = 600                # cw-noise-tone: the key tone tried (the default 700; 10 Hz a click of the MFK)


def t_cw_noise_tone(o):
    """Whether the X6200's CW resonance follows the key tone (the decoder's colour takes it to, src/cw.cpp BASE_PEAK_Q),
    nothing sent: both on page G in CW on F_CW, the key tone KT_TRY on both (the remote's set kt, read back as kt), the
    noise's shape (cw-noise-shape's) through an 800 Hz filter around it; the key tone put back as found. PASS when the
    X6200's noise peaks within 30 Hz of KT_TRY (2026-11-25: 19.8 dB at 600 Hz, where 700 gave it at 700); the X6100's
    said. (Its first form set the tone through the classic screen's KEY page and the MFK; its put-back pressed KEY
    once more, which steps to KEY 2:2, and the MFK moved the keyer's ratio: the remote's kt since)"""
    from tones import TciRx
    got, kt0 = {}, {n: int(r.state.get("kt") or 700) for n, r in o.r.items()}
    lo, hi = KT_TRY - 400, KT_TRY + 400
    try:
        with o.step(f"both on page G, {F_CW / 1e6:.3f} MHz CW, the key tone {KT_TRY} Hz (kt)") as s:
            o.both_page("G")
            want, checks = bench.rx_profile(F_CW, 4, pre=0, att=0, rfg=100, agc=2, sql=0)
            o.both(dict(want, zoom=8), checks)
            o.both({"kt": KT_TRY})
            o.both({"filter": (lo, hi)})
            s.confirm(f"key tones as found {kt0}, now {KT_TRY}; filter {lo}-{hi}")
        with o.step(f"both through {lo}-{hi} Hz, {CW_SHAPE_S:.0f} s of quiet each: the noise's peak") as s:
            rec = {n: TciRx(r.host) for n, r in o.r.items()}
            try:
                time.sleep(2.5)
                t0 = time.time()
                time.sleep(CW_SHAPE_S)
                for n, c in rec.items():
                    x, _ = c.audio(t0, time.time())
                    got[n] = noise_shape(x, FS, lo, hi)
            finally:
                for c in rec.values():
                    c.close()
            s.confirm("; ".join(f"{n} peak {v['peak_db']} dB at {v['peak_hz']} Hz, {v['width6_hz']} Hz wide"
                                for n, v in got.items()))
    except Refused as e:
        return False, str(e), {"kt0": kt0, "shapes": got}
    finally:
        with o.step("the key tone as found"):
            in_parallel(o, lambda n, r: bench.ensure(r, {"kt": kt0[n]}), wait=30)
    x62 = got.get(B, {})
    follows = x62 and abs(x62["peak_hz"] - KT_TRY) <= 30
    words = "; ".join(f"{n}: the noise's peak {v['peak_db']} dB over the band's 25th percentile at {v['peak_hz']} Hz "
                      f"({v['width6_hz']} Hz wide)" for n, v in got.items())
    if not follows:
        return False, (f"the X6200's resonance did NOT follow the key tone {KT_TRY} Hz: {words} (the decoder's colour, "
                       f"src/cw.cpp, assumes it does)"), {"kt0": kt0, "shapes": got}
    return True, f"the X6200's resonance followed the key tone {KT_TRY} Hz: {words}", {"kt0": kt0, "shapes": got}


WF3D_ACTION = 18            # ACTION_WF_3D (src/params/params.h), the action Settings > Keys names "WF 3D on/off"
WF3D_KEY = "fst"            # whose hold is lent to it (its own: a step down, nothing that parts the radios)
WF_MOVING_MIN = 0.05        # the waterfall's half changing this much a second (0-255 mean): not frozen
WF_3D_APART_MIN = 1.5       # the 3D screen's lower half this far from 2D's: the perspective drawn


def gui_cpu(host, secs=3.0):
    """The GUI's share of one core over secs (/proc's utime + stime, 100 ticks a second), and its pid"""
    import subprocess
    out = subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{host}",
                          f"p=$(pidof x6100_gui); a=$(cut -d' ' -f14,15 /proc/$p/stat); sleep {secs}; "
                          f"b=$(cut -d' ' -f14,15 /proc/$p/stat); echo $p $a $b"],
                         capture_output=True, text=True, timeout=secs + 30).stdout.split()
    if len(out) != 5:
        raise Refused(f"{host}: the GUI's CPU not read ({' '.join(out)!r})")
    p, a1, a2, b1, b2 = (int(x) for x in out)
    return round((b1 + b2 - a1 - a2) / secs, 1), p


def shot_stats(paths):
    """Two screenshots of one radio (landscape PNGs) a second apart: the lower half's (the waterfall's) mean level
    0-255 in the first and the mean change between them"""
    from PIL import Image
    import numpy as np
    a, b = (np.asarray(Image.open(p).convert("L"), dtype=float) for p in paths)
    h = a.shape[0]
    lo_a, lo_b = a[h // 2:], b[h // 2:]
    return round(float(lo_a.mean()), 1), round(float(np.abs(lo_a - lo_b).mean()), 2)


def t_classic_wf3d(o):
    """The classic screen's waterfall in 3D and in 2D on both radios (upstream's 3D drawing thread, merged 11-23: a look
    left from the merge), nothing sent: both on 14.200 USB, on the classic screen (F SYSTEM > CLASSIC RADIO), where the 3D waterfall
    is on by default; two screenshots a second apart in each, the waterfall's half lit and moving, and the GUI's CPU;
    then 2D (FST's hold lent to "WF 3D on/off" through Settings > Keys' web page, wf_3d read back), the same, and 3D
    again, the key, the screen and the Sentry put back. PASS when both draw a moving waterfall both ways and their
    GUIs ran on (the same pid); the CPU said"""
    import suite_cover_b as cb
    from bench import screenshots_both
    rec = {n: {} for n in o.r}
    kept, sys0 = {}, {}

    def to_classic(n, r):
        kept[n] = cb.keys_get(cb.ip_of(r))
        sys0[n] = cb.gstate(r.host).get("s.sys")
        cb.to_classic(r, r.host)
        return cb.on_classic(r)

    def look(label):
        for n, r in o.r.items():
            rec[n][f"{label}_cpu"], rec[n][f"{label}_pid"] = gui_cpu(r.host)
        a = screenshots_both(list(o.r.values()), f"classic {label} a")
        time.sleep(1.0)
        b = screenshots_both(list(o.r.values()), f"classic {label} b")
        for n in o.r:
            if not a.get(n) or not b.get(n):
                raise Refused(f"{n}: no screenshot of the classic screen in {label}")
            rec[n][f"{label}_level"], rec[n][f"{label}_moving"] = shot_stats([a[n], b[n]])
            rec[n][f"{label}_shots"] = [a[n], b[n]]
        return "; ".join(f"{n}: the waterfall's half at {rec[n][label + '_level']} of 255, changing "
                         f"{rec[n][label + '_moving']} a second, the GUI {rec[n][label + '_cpu']} % of a core"
                         for n in o.r)

    def toggle(n, r, want):
        ip = cb.ip_of(r)
        cb.keys_set(ip, f"hold_{WF3D_KEY}", WF3D_ACTION)
        cb.chold(r, WF3D_KEY.upper())
        got = cb.pwait(r.host, "wf_3d", lambda v: v == str(want), secs=6.0, default="1")
        if got != str(want):
            raise Refused(f"{n}: wf_3d read {got} after FST's hold, {want} wanted")
        return got

    try:
        with o.step("both on the classic screen (F SYSTEM > CLASSIC RADIO), the 3D waterfall on (its default)") as s:
            got = in_parallel(o, to_classic, wait=40)
            if not all(got.values()):
                raise Refused(f"not on the classic screen: {got}")
            # USB, set there: in CW the decoder's panel lies over the waterfall (a run after cw-quiet showed only the
            # panel), and set before, the Sentry's page F brought its own dial back (14.040 CW)
            want, checks = bench.rx_profile(14200000, 2, pre=0, att=0, rfg=100, agc=3, sql=0)
            o.both(want, checks)
            w = {n: cb.params(r.host, ["wf_3d"]).get("wf_3d", "1") for n, r in o.r.items()}
            if any(v != "1" for v in w.values()):
                raise Refused(f"wf_3d not on as found: {w}")
            time.sleep(2.0)
            s.confirm(f"wf_3d {w}")
        with o.step("3D: each waterfall lit and moving, the GUI's CPU") as s:
            s.confirm(look("3d"))
        with o.step("2D: FST's hold lent to WF 3D on/off, held: wf_3d 0, each waterfall lit and moving") as s:
            in_parallel(o, lambda n, r: toggle(n, r, 0), wait=40)
            time.sleep(2.0)
            s.confirm(look("2d"))
        with o.step("3D again (FST held once more: wf_3d 1)") as s:
            in_parallel(o, lambda n, r: toggle(n, r, 1), wait=40)
            s.confirm("wf_3d 1 on both")
    except Refused as e:
        return False, str(e), rec
    finally:
        with o.step("FST's hold, F SYSTEM's choice and the Sentry as found"):
            def back(n, r):
                if n in kept:
                    cb.keys_put_back(cb.ip_of(r), kept[n])
                if n in sys0:
                    cb.sys_back(r, r.host, sys0[n])
                bench.open_page(r, "A")
            in_parallel(o, back, wait=60)
    # Judged: moving = any change in a second (a frozen one, or one under a panel, changes 0.0: the CW decoder's panel
    # over it read 0.0 on both, 2026-11-25; the X6100's waterfall of the quiet cable is near black and changes 0.15-0.5);
    # 3D drawn = its screen apart from 2D's (the perspective and its grid: 3.1 on the X6100, 10.7 on the X6200)
    from PIL import Image
    import numpy as np
    for n in o.r:
        a, b = (np.asarray(Image.open(rec[n][f"{m}_shots"][0]).convert("L"), dtype=float) for m in ("3d", "2d"))
        rec[n]["3d_vs_2d"] = round(float(np.abs(a[a.shape[0] // 2:] - b[b.shape[0] // 2:]).mean()), 2)
    bad = [f"{n} {m} still" for n in o.r for m in ("3d", "2d") if rec[n].get(f"{m}_moving", 0) < WF_MOVING_MIN]
    bad += [f"{n}: 3D as 2D" for n in o.r if rec[n]["3d_vs_2d"] < WF_3D_APART_MIN]
    crashed = [n for n in o.r if len({rec[n].get(k) for k in ("3d_pid", "2d_pid")}) != 1]
    says = "; ".join(f"{n}: 3D {rec[n]['3d_cpu']} % of a core (changing {rec[n]['3d_moving']} a second), 2D "
                     f"{rec[n]['2d_cpu']} % ({rec[n]['2d_moving']}), 3D apart from 2D by {rec[n]['3d_vs_2d']}"
                     for n in o.r)
    if crashed:
        return False, f"the GUI started again during the look on {crashed}: {says}", rec
    if bad:
        return False, f"{bad}: {says}", rec
    return True, f"the classic screen's waterfall drawn and moving in 3D and 2D on both: {says}", rec


POWER_STEPS = [0.2, 0.5, 1.0]       # W: the listener's level must rise 10*log10 of each ratio. Not from 0.1 W: the
# X6200 holds a TCI over's power by its own meter (its ALC reads 0, tx_level.c power_hold), which reads 0.1 W steps,
# so 0.1 W counts as reached from about 0.05 W (2026-11-23: its 0.1 W 4 dB under its 1 W's -10 dB, four runs in five)
POWER_TOL_DB = 2.0
POWER_TONE_S = 4.0          # each power's tone; its level the median of the frames that carry it (power_level)
POWER_TAIL_S = 1.6          # keyed silence after it: the X6200's transmit path holds 1.2-1.4 s of TCI audio at the
                            # unkey (STEP_TAIL_S's 0.8 s cut the end of its 2 s tones, and the level, averaged over a
                            # window that took in that silence, moved with it: 2026-11-24)
POWER_TONE_DB = 15          # a frame carries the tone: within this of the over's strongest frame
POWER_6M = [0.1, 0.3, 1.0]  # 6 m from 0.1 W: the factory X6100 put out 0.8-1.2 W there at any setting under 1 W (its
                            # base's ALC-only power control, 2026-11-19/22); a floor reads as a step under +4.8 dB


def power_level(L, hz, on, off):
    """The listener's spectrum at hz (+-300 Hz) between on and off: per frame its top bin; the frames within
    POWER_TONE_DB of the strongest are the tone's, their first and last 0.5 s left out; (median dB, frames, the
    series as [[s from on, dB]])"""
    fr = []
    for t, cen, wid, v in list(L.r.spectra):
        if on <= t <= off + 0.5 and v:
            per = wid / len(v)
            a, b = int((hz - 300 - (cen - wid / 2)) / per), int((hz + 300 - (cen - wid / 2)) / per)
            seg = v[max(a, 0):min(b + 1, len(v))]
            if seg and a < len(v) and b >= 0:
                fr.append((t, max(seg)))
    if not fr:
        return None, 0, []
    top = max(v for _, v in fr)
    tone = [(t, v) for t, v in fr if v >= top - POWER_TONE_DB]
    t0, t1 = tone[0][0] + 0.5, tone[-1][0] - 0.5
    mid = sorted(v for t, v in tone if t0 <= t <= t1)
    series = [[round(t - on, 2), v] for t, v in fr]
    return (mid[len(mid) // 2] if mid else None), len(mid), series


# A radio's least power where its base sets the carrier itself (AM, FM) and the GUI cannot drive it lower: the X6100's
# factory base (Xiegu's V1.1.9) on 6 m, about 1 W at any setting under 1 W (its ALC's least; measured 2026-11-24).
# The GUI says so at a keying there (radio.c floor_note, its log line FLOOR_LOG); power_steps then checks that floor
# (the levels alike, the note logged) where other radios must follow the setting
FLOOR_6M = {"x6100": 1.0}
FLOOR_LOG = "the base's least there is about"


def floor_of(sender, f, mode):
    return FLOOR_6M.get(sender) if 50000000 <= f <= 54000000 and mode in (6, 7) else None


def power_steps(o, sender, listener, f, label, steps=None, mode=2):
    """A 1000 Hz tone POWER_TONE_S at each of steps (POWER_STEPS; USB, one over each; mode 7 FM or 6 AM: no audio, its
    carrier the level: what the base sets itself, the audio's drive no part of it). Where the sender's base has a floor
    there (floor_of): the levels alike within POWER_TOL_DB, and the GUI's note of it in the sender's log: the listener's spectrum at the tone
    (power_level: the median of the frames that carry it, while the sender said it was on the air) must rise by the
    power's ratio in dB, within POWER_TOL_DB (the spectrum is the receiver's before its AGC; RF gain fixed). Every
    frame and the sender's meter readings kept in the data"""
    levels, got = [], {}
    S, L = o.r[sender], o.r[listener]
    floor = floor_of(sender, f, mode)
    log0 = int(bench.ssh(S.host, "wc -c < /tmp/x6100_log.txt").strip() or 0) if floor else 0
    try:
        for w in steps or POWER_STEPS:
            peak = {}

            def body(watch, w=w):
                with o.step(f"{sender} sends 1000 Hz {POWER_TONE_S:g} s at {w} W"):
                    meters, stop = [], threading.Event()

                    def sample():
                        while not stop.is_set():
                            if S.txm:
                                meters.append([round(time.time(), 2), S.txm[0]])
                            time.sleep(0.2)
                    th = threading.Thread(target=sample, daemon=True)
                    th.start()
                    try:
                        bench.tci_send(S, [[0, 0.3], [0 if mode in (6, 7) else 1000, POWER_TONE_S], [0, POWER_TAIL_S]],
                                       max_s=POWER_TONE_S + POWER_TAIL_S + 4, watch=watch)
                    finally:
                        stop.set()
                    if watch.why:
                        raise Refused(watch.why)
                    peak["w"] = watch.tx_peak.get(sender)
                    peak["meters"] = meters
            over(o, sender, listener, f, mode, f"{label} {w} W", body, w=w)
            with o.step(f"{listener}'s level while {sender} said it was on the air") as s:
                if not S.tx_last:
                    raise Refused(f"{sender} never said it was on the air")
                on, off = S.tx_last
                time.sleep(0.6)                     # the spectrum's last frames
                lvl, n, series = power_level(L, f + (0 if mode in (6, 7) else 1000), on, off)
                got.setdefault("overs", []).append({"w": w, "on_s": round(off - on, 1), "level": lvl, "frames": n,
                                                    "series": series,
                                                    "meters": [[round(t - on, 1), m] for t, m in peak.get("meters", [])]})
                if lvl is None or n < 10:
                    raise Refused(f"{listener}'s spectrum: {n} frames carried {f + (0 if mode in (6, 7) else 1000)} Hz while {sender} was on "
                                  f"the air ({off - on:.1f} s)")
                levels.append((w, float(lvl), peak.get("w")))
                s.confirm(f"{lvl} dB, the median of {n} frames of the tone, {off - on:.1f} s on the air")
        say_lv = ", ".join(f"{w} W {l} dB (meter {m and round(m, 2)} W)" for w, l, m in levels)
        got["levels"] = levels
        if floor:
            with o.step(f"{sender}'s base at its floor (~{floor:g} W) for every setting under it: the levels alike, the "
                        f"GUI's note in its log") as s:
                spread = max(l for _, l, _ in levels) - min(l for _, l, _ in levels)
                if spread > POWER_TOL_DB:
                    raise Refused(f"levels {say_lv}: {spread:.1f} dB apart (the floor would keep them within "
                                  f"{POWER_TOL_DB} dB): the floor changed, take it off FLOOR_6M")
                notes = [ln for ln in bench.ssh(S.host, f"tail -c +{log0 + 1} /tmp/x6100_log.txt").splitlines()
                         if FLOOR_LOG in ln]
                got["notes"] = notes
                if not notes:
                    raise Refused(f"levels {say_lv} at the floor, but the GUI said nothing of it")
                s.confirm(f"{say_lv}; the GUI: {notes[0]}")
        else:
            with o.step(f"{listener}'s level rose with the power") as s:
                bad = []
                for (w0, l0, _), (w1, l1, _) in zip(levels, levels[1:]):
                    want = 10 * math.log10(w1 / w0)
                    if abs((l1 - l0) - want) > POWER_TOL_DB:
                        bad.append(f"{w0}->{w1} W: {l1 - l0:+.1f} dB (want {want:+.1f})")
                if bad:
                    raise Refused(f"levels {say_lv}; off by more than {POWER_TOL_DB} dB: {'; '.join(bad)}")
                s.confirm(say_lv)
    except Refused as e:
        return False, str(e), got
    finally:
        o.both({"pwr": W})                  # the power back at 0.1 W on both
    if floor:
        return True, (f"{sender}'s base at its floor as expected: " + ", ".join(f"{w} W {l} dB" for w, l, _ in levels)
                      + "; the GUI said so"), got
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
            R.send(f"key PTT 1 {o.hold()}")
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
            o.st.ev(f"{n}: test call {call} set, PSK Reporter off: its GUI restarts (planned)", "radio")
            o.planned[n] = "planned: test call set"
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
            o.planned.pop(n, None)
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
        with o.step("both on their test calls, the FT8 app open on both on the dial, everything alike"):
            ft8_both(o, dial)
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
        o.apps_closed(dial, 3)
    return True, f"sent {got['sent']!r}, decoded the same at {got['snr']} dB", got


def hello(r):
    """The GUI's version from the remote's first line ("hello <name> <version> <boot>"), kept on the session once seen
    (the session's recent lines roll over in a long run: identity read None after an hour, 2026-11-23)"""
    for _, x in list(r.lines):
        if x.startswith("hello "):
            r._hello = " ".join(x.split()[1:-1]) or x.split()[1]
            break
    return getattr(r, "_hello", None)


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
            R.send(f"key PTT 1 {o.hold()}")
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
        with o.step("both on their test calls, the FT8 app open on both on the dial, everything alike"):
            ft8_both(o, dial)
        p, total = bench.chain_gate(o.c, "radios")
        bench.radio_tx_gate(S, o.c, W)
        bench.listener_gate(L)
        o.roles = {sender: "TX", listener: "RX"}
        watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W, label=label)
        try:
            with o.step(f"{sender}'s CQ stopped 2 s into it: off the air within 1.5 s (as its state says)") as s:
                S.send("ft8 cq")
                S.wait("its transmission", lambda: S.tx or watch.why, 32)
                bench.tx_log({"who": sender, "f": dial, "w": S.pwr(), "what": f"orch: {label}"})
                time.sleep(2.0)
                if not S.tx:                        # a stop proves nothing on a sender already off the air
                    raise Refused(f"{sender} off the air already 2 s into its CQ (before the stop)")
                t0 = time.time()
                S.send("ft8 stop")
                S.wait("off the air", lambda: not S.tx, 3)
                res["off_s"] = round(time.time() - t0, 2)
                # 1.5 s: the stop's way to the radio and its "tx 0" back over WiFi are in it (0.65-0.93 s, once 1.02)
                if res["off_s"] > 1.5:
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
        o.apps_closed(dial, 3)
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
        if mode == "rtty":                  # made to the listener's own centre, shift, rate and sense
            rows = bench.ssh(L.host, "sqlite3 -readonly /mnt/params.db \"select name,val from params where name like "
                                     "'rtty%'\"")
            cfg = {k: int(v) for k, v in (ln.split("|") for ln in rows.split() if "|" in ln)}
            mark_low = (m in (2, 3)) == bool(cfg.get("rtty_reverse", 1))     # rtty.c tx_start: mark_hi = USB != rev
            x = digimodes.rtty_audio(text, center=cfg.get("rtty_center", 800), shift=cfg.get("rtty_shift", 450),
                                     baud=cfg.get("rtty_rate", 5000) / 100, mark_low=mark_low)
        else:
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
                if not bench.close_apps(L):         # the RTTY panel is no dialog: the Sentry's page closes it
                    bench.open_page(L, "G")
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


def in_parallel(o, fn, wait=30):
    """fn(name, radio) on both radios at once, inside one guarded change: {name: its return}; a Refused in either is
    raised after both are done (each waited for up to wait s)"""
    out, errs = {}, {}

    def one(n):
        try:
            out[n] = fn(n, o.r[n])
        except (Refused, OSError, ValueError, IndexError) as e:
            errs[n] = f"{n}: {e}"
        except Exception as e:              # any other: said with its kind, never taken for a radio that did not answer
            errs[n] = f"{n}: {type(e).__name__}: {e}"
    o.changing += 1
    try:
        th = [threading.Thread(target=one, args=(n,)) for n in o.r]
        [t.start() for t in th]
        [t.join(wait) for t in th]
    finally:
        o.changing -= 1
    late = [n for n in o.r if n not in out and n not in errs]
    if late:
        raise Refused(f"{', '.join(late)}: no answer within {wait} s")
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


def t_remote_security(o):
    """A connection with a token no session has (each radio): it may only watch ("role view 0 -"); its set, PTT,
    knob and control take each refused ("err view only..."), the radio's dial and TX unmoved (its own state)"""
    from remote_ws import Remote
    from suite_proto import texts
    got = {}
    with o.step("both on page G, 14.200 MHz USB"):
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
    with o.step("a stranger's token on each radio: watch only, every command refused") as s:
        def one(n, r):
            c = Remote(r.host, 50002, timeout=5)
            try:
                c.send("auth " + "0" * 64)
                c.send("panel 1")
                hello = [x for x in texts(c, 1.5) if x.startswith("role ")]
                for cmd in ("set freq 7000000", "key PTT 1", "knob main 5", "control take", "tune 1 7", "ft8 cq"):
                    c.send(cmd)
                ans = [x for x in texts(c, 2.5) if x.startswith("err")]
                c.send("key PTT 0")
            finally:
                c.close()
            if not hello or not hello[0].startswith("role view"):
                raise Refused(f"its role {hello}")
            if len(ans) < 6:
                raise Refused(f"only {len(ans)} of 6 refused: {ans}")
            if int(r.state.get("freq", 0)) != 14200000 or r.tx:
                raise Refused(f"moved: {r.state.get('freq')} Hz, on the air {r.tx}")
            return {"role": hello[0], "refused": ans}
        got = in_parallel(o, one)
        alike(o, ("freq", "mode"))
        s.confirm(f"{got[A]['role']!r}; 6 of 6 refused on both")
    return True, f"a stranger: {got[A]['role']!r} on both, set/PTT/knob/control/TUNE/FT8 each refused, nothing moved", got


def t_rx_session_refused(o):
    """The bench's receive-only session (may operate, may not transmit) on each radio: PTT, TUNE and FT8 CQ each
    refused ("err this session may not transmit"), nothing keyed (the transmitting session's own state)"""
    errs = {}
    with o.step("both on page G, 14.200 MHz USB"):
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
    with o.step("PTT, TUNE and FT8 CQ from the receive-only session on both: refused, nothing keyed") as s:
        def one(n, r):
            rx = bench.Radio(n, o.c["radios"][n], False)
            try:
                if rx.may_tx:
                    raise Refused("the receive-only session may transmit")
                r.send("control release")           # control to the receive-only session: its refusals are then
                r.wait("control let go", lambda: not r.ctl_mine, 5)      # the transmit rule's, not control's
                rx.take_control()
                e0 = len(rx.errs)
                for cmd in (f"key PTT 1 {o.hold()}", "tune 1 7", "ft8 cq"):
                    rx.send(cmd)
                t0 = time.time()
                while time.time() - t0 < 3:
                    if r.tx:
                        raise Refused("WENT ON THE AIR")
                    time.sleep(0.05)
                rx.send("key PTT 0")
                said = list(rx.errs[e0:])
            finally:
                rx.send("control release")
                rx.close()
                r.take_control()
            if len([x for x in said if "may not transmit" in x]) < 3:
                raise Refused(f"refusals: {said}")
            return said
        errs = in_parallel(o, one)
        s.confirm(str(errs))
    return True, f"PTT, TUNE and FT8 CQ refused to the receive-only session on both ({errs[A][:2]}), nothing keyed", errs


APPS_SAME = ["JS8", "Olivia", "SSTV", "WeFax", "NavTex", "PSK"]


def t_apps_alike(o):
    """Each of APPS_SAME opened on both radios at once (the remote's apps list): each must say the app is open, and
    the two must sit on the same dial and mode; closed on both after, both set alike again"""
    import suite_more
    res, bad = {}, []
    for name in APPS_SAME:
        with o.step(f"{name} open on both at once") as s:
            o.changing += 1
            try:
                def one(n, r, name=name):
                    if not suite_more.open_app(r, name.lower()):
                        raise Refused(f"{name} did not open")
                    time.sleep(1.5)
                    return (int(r.state.get("freq", 0)), int(r.state.get("mode", -1)))
                try:
                    dial = in_parallel(o, one)
                    res[name] = dial
                    if dial[A] != dial[B]:
                        bad.append(f"{name}: x6100 {dial[A]}, x6200 {dial[B]}")
                        s.confirm(f"APART: {dial}")
                    else:
                        s.confirm(f"both {dial[A][0] / 1e6:.4f} MHz mode {dial[A][1]}")
                finally:
                    for r in o.r.values():
                        bench.close_apps(r)
                    o.both_page("G")
                    o.both({"freq": 14200000, "mode": 2})
            finally:
                o.changing -= 1
    if bad:
        return False, "apps that opened on different dials on the two radios: " + "; ".join(bad), res
    return True, f"{len(APPS_SAME)} apps each opened on the same dial on both: " + ", ".join(
        f"{k} {v[A][0] / 1e6:.4f}" for k, v in res.items()), res


def ft8_both(o, dial):
    """Test calls on both, the FT8 app open on both in FT8 on dial at 0.1 W, Auto off, confirmed alike"""
    test_calls(o)
    o.changing += 1
    try:
        for r in o.r.values():
            r.send("ft8 open")
        for r in o.r.values():
            r.wait("the FT8 app", lambda r=r: r.ft8.get("open") == 1, 10)
            time.sleep(1.0)
            for _ in range(3):
                if r.state.get("app") == "FT8":
                    break
                p = r.ft8.get("proto")
                r.send("ft8 mode")
                r.wait("FT8's protocol stepped", lambda r=r, p=p: r.ft8.get("proto") != p, 5)
                time.sleep(0.8)
            if r.state.get("app") != "FT8":
                raise Refused(f"{r.name}: its FT8 app not in FT8 ({r.state.get('app')})")
        th = [threading.Thread(target=bench.ft8_to_band, args=(r, dial)) for r in o.r.values()]
        [t.start() for t in th]
        [t.join(40) for t in th]
        for r in o.r.values():
            if r.ft8.get("auto"):
                r.send("ft8 auto")
                r.wait("Auto off", lambda r=r: not r.ft8.get("auto"), 5)
    finally:
        o.changing -= 1
    o.both({"freq": dial, "pwr": W})
    alike(o, ("freq", "mode"))
    o.pair_alike()


def ft8_send(o, S, L, watch, cmd, label, dial):
    """S sends cmd (an FT8 transmission) in its next slot; returns the message it said it sent ("next")"""
    S.send(cmd)
    S.wait("its transmission", lambda: S.tx or watch.why, 32)
    sent = bench.norm(S.ft8.get("next") or "")
    bench.tx_log({"who": S.name, "f": dial, "w": S.pwr(), "what": f"orch: {label}"})
    S.wait("the end of its transmission", lambda: not S.tx or watch.why, bench.MAX_KEY_S + 2)
    S.send("ft8 stop")
    if watch.why:
        raise Refused(watch.why)
    return sent


def ft8_reply(o, a, b, label, dial=14074000):
    """a calls CQ, b decodes it and replies to it (the app's own reply), a must decode exactly b's reply"""
    A_, B_ = o.r[a], o.r[b]
    res = {}
    p, total = bench.chain_gate(o.c, "radios")
    try:
        with o.step("both on their test calls, the FT8 app open on both on the dial"):
            ft8_both(o, dial)
        for r in (A_, B_):
            bench.radio_tx_gate(r, o.c, W)
            bench.listener_gate(r)
        watch = bench.Watch([A_, B_], o.c, expect_dbm=10 * math.log10(W * 1000) - total, w=W, label=label)
        try:
            with o.step(f"{a} calls CQ, {b} decodes it") as s:
                o.roles = {a: "TX", b: "RX"}
                n0 = len(B_.rows)
                cq = ft8_send(o, A_, B_, watch, "ft8 cq", label, dial)
                B_.wait("the CQ decoded", lambda: any(bench.msg(r) == cq for r in B_.rows[n0:]), 8)
                row = next(r for r in B_.rows[n0:] if bench.msg(r) == cq)
                s.confirm(f"{mask(cq)!r} at {row.get('snr')} dB")
            with o.step(f"{b} replies to it, {a} decodes exactly the reply") as s:
                o.roles = {b: "TX", a: "RX"}
                n1 = len(A_.rows)
                rep_ = ft8_send(o, B_, A_, watch, f"ft8 reply {row['id']}", label, dial)
                try:
                    A_.wait("the reply decoded", lambda: any(bench.msg(r) == rep_ for r in A_.rows[n1:]), 8)
                except Refused:
                    raise Refused(f"{a} decoded {[mask(bench.msg(r)) for r in A_.rows[n1:]] or 'nothing'}, not "
                                  f"{mask(rep_)!r}")
                row2 = next(r for r in A_.rows[n1:] if bench.msg(r) == rep_)
                res.update(cq=mask(cq), reply=mask(rep_), snr=(row.get("snr"), row2.get("snr")))
                s.confirm(f"{mask(rep_)!r} at {row2.get('snr')} dB")
        finally:
            watch.close()
            for r in (A_, B_):
                r.send("ft8 stop")
            o.roles = {A: "RX", B: "RX"}
    except Refused as e:
        return False, str(e), res
    finally:
        for r in (A_, B_):
            r.send("ft8 close")
        o.apps_closed(dial, 3)
    return True, f"{a}'s {res['cq']!r} decoded by {b}, {b}'s reply {res['reply']!r} decoded by {a}", res


def remote_busy(o, radio, other, label):
    """radio on the air (PTT held by its lease, 14.200 USB, 0.1 W): another session's control take refused while it
    is, and the other radio never keyed"""
    R, X = o.r[radio], o.r[other]
    res = {}

    def body(watch):
        with o.step(f"{radio} on the air 4 s: another session's control take refused, {other} never keyed") as s:
            rx = bench.Radio(radio, o.c["radios"][radio], False)
            try:
                t0 = time.time()
                h = o.hold()
                R.send(f"key PTT 1 {h}")
                R.wait("on the air", lambda: R.tx, 3)
                e0 = len(rx.errs)
                rx.send("control take")
                got = None
                while time.time() - t0 < 4:
                    R.send(f"key PTT 1 {h}")            # its renewal: the same number
                    if X.tx:
                        raise Refused(f"{other} WENT ON THE AIR too")
                    if rx.ctl_mine:
                        raise Refused("the other session took control while on the air")
                    if len(rx.errs) > e0:
                        got = rx.errs[-1]
                    time.sleep(0.5)
                R.send("key PTT 0")
                if not got:
                    raise Refused("the control take neither refused nor taken")
                res["said"] = got
                s.confirm(f"refused: {got!r}")
            finally:
                rx.close()
    try:
        over(o, radio, other, 14200000, 2, label, body)
    except Refused as e:
        return False, str(e), res
    return True, f"while {radio} was on the air another session's control take was refused ({res['said']!r}); " \
                 f"{other} never keyed", res


def t_band_memory(o):
    """Each band keeps its own ATT: 20 m on with ATT on, 15 m with ATT off, then 20 m (ATT on again, from its memory)
    and 15 m (off again), on both, each confirmed and alike"""
    with o.step("both on page G, 20 m (14.200 USB) ATT on, PRE off"):
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
        o.both({"att": 1, "pre": 0})
    with o.step("both to 15 m (21.200), ATT off there"):
        o.both({"freq": 21200000})
        o.both({"att": 0})
    seen = {}
    for f, want in ((14200000, 1), (21200000, 0)):
        with o.step(f"both back to {f / 1e6:.3f} MHz: ATT {want} from that band's memory") as s:
            o.both({"freq": f})
            time.sleep(0.5)
            seen[f] = {n: int(r.state.get("att", -1)) for n, r in o.r.items()}
            alike(o, ("att",))
            s.confirm(str(seen[f]))
    o.both({"freq": 14200000})
    o.both({"att": 0})
    ok = all(v == 1 for v in seen[14200000].values()) and all(v == 0 for v in seen[21200000].values())
    return ok, (f"each band its own ATT on both: 20 m {seen[14200000]}, 15 m {seen[21200000]}" if ok else
                f"a band's ATT not its own: 20 m {seen[14200000]} (1 wanted), 15 m {seen[21200000]} (0 wanted)"), seen


def t_cb_channel(o):
    """On the Sentry's G: CB 19 (27.185 MHz AM) as G's own channel on both, then BAND UP (a channel up on G): CB 20,
    27.205 MHz AM, on both"""
    with o.step("both on page G, G's channel CB 19 (27.185 AM)"):
        o.both_page("G")
        o.both({"wch": (27185000, 6)})
        o.both({"freq": 27185000, "mode": 6})
    with o.step("BAND UP on both: CB 20, 27.205 AM on both") as s:
        def up(n, r):
            bench.tap(r, "BAND_UP")
            r.wait("CB 20", lambda: int(r.state.get("freq", 0)) == 27205000, 5)
            return int(r.state.get("freq")), int(r.state.get("mode"))
        got = in_parallel(o, up)
        alike(o, ("freq", "mode"))
        s.confirm(str(got))
    return True, f"CB 19 then a channel up: both on {got[A][0] / 1e6:.3f} MHz mode {got[A][1]}", got


def t_tci_audio_rate(o):
    """Each radio's receiver audio over its TCI (24 kHz int16): 8 s of it must arrive at 97-103 % of real time, both
    radios listening on one frequency"""
    from tones import TciRx
    with o.step("both on page G, 14.200 MHz USB"):
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
    rates = {}
    with o.step("8 s of each radio's TCI audio, counted") as s:
        rx = {n: TciRx(r.host) for n, r in o.r.items()}
        try:
            time.sleep(2.0)                     # the stream started
            t0 = time.time()
            time.sleep(8.0)
            t1 = time.time()
            for n, c in rx.items():
                x, _ = c.audio(t0, t1)
                rates[n] = round(100 * len(x) / FS / (t1 - t0), 1)
        finally:
            for c in rx.values():
                c.close()
        bad = {n: v for n, v in rates.items() if not 97 <= v <= 103}
        if bad:
            raise Refused(f"audio at {bad} % of real time (97-103 wanted)")
        s.confirm(f"{rates} % of real time")
    return True, f"each radio's TCI audio at {rates} % of real time", rates


def t_tci_takes_control(o):
    """A TCI client's set takes control ("ctl tci" to the orchestrator's sessions) on both; the client gone, control
    is free and taken back (when free: bench.Radio.take_control)"""
    from remote_ws import Remote
    with o.step("both on page G, 14.200 MHz USB"):
        o.both_page("G")
        o.both({"freq": 14200000, "mode": 2})
    seen = {}
    with o.step("a TCI set on both: control goes to TCI, confirmed by the remote's ctl line") as s:
        def one(n, r):
            c = Remote(r.host, 50001, timeout=5)
            try:
                time.sleep(1.0)
                c.send("vfo:0,0,14201000;")
                r.wait("ctl tci", lambda: (r.ctl_who or "").startswith("tci"), 5)
                r.wait("the set", lambda: int(r.state.get("freq", 0)) == 14201000, 5)
                seen[n] = r.ctl_who
            finally:
                c.close()
            r.wait("control free after the client left", lambda: r.ctl_who in ("none", None) or r.ctl_mine, 8)
            r.take_control()
            return seen[n]
        got = in_parallel(o, one)
        alike(o, ("freq", "mode"))
        s.confirm(str(got))
    o.both({"freq": 14200000})
    return True, f"a TCI set took control on both ({got}); the client gone, control taken back", got


JS8_TEXT = "HELLO BENCH 73"


def js8_text(o, a, b, label):
    """The JS8 app open on both on its dial (test calls, PSK Reporter off), a's text to b (directed, not a MSG: nothing
    stored): b's list must show it from a's call with exactly JS8_TEXT in it"""
    import suite_air_b as ab
    A_, B_ = o.r[a], o.r[b]
    res = {}
    p, total = bench.chain_gate(o.c, "radios")
    try:
        with o.step("both on their test calls, the JS8 app open on both on its dial"):
            test_calls(o)
            o.changing += 1
            try:
                for r in (A_, B_):
                    r.send("js8 open")
                for r in (A_, B_):
                    r.wait("the JS8 app", lambda r=r: ab.last_json(r, "js8").get("open") == 1 or
                           r.state.get("app") == "JS8", 10)
                time.sleep(1.5)
                for r in (A_, B_):              # one after the other, each to the end: a thread left stepping a
                    ab.js8_to(r, ab.F_JS8)      # band key after its join moved the X6200 off mid-over (2026-11-23)
            finally:
                o.changing -= 1
            o.both({"freq": ab.F_JS8, "pwr": W})
            alike(o, ("freq", "mode"))
            time.sleep(2.0)
            alike(o, ("freq", "mode"))          # and still, nothing left stepping
            o.pair_alike()
        bench.radio_tx_gate(A_, o.c, W)
        bench.listener_gate(B_)
        o.roles = {a: "TX", b: "RX"}
        watch = bench.Watch([A_, B_], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=B_, w=W, label=label)
        try:
            with o.step(f"{a} sends '{TEST_CALLS[b]} {JS8_TEXT}', every frame of it (JS8 sends a text in slots)") as s:
                t = time.time()
                bench.tx_log({"who": a, "f": ab.F_JS8, "w": A_.pwr(), "what": f"orch: {label}"})
                A_.send(f"js8 send {TEST_CALLS[b]} {JS8_TEXT}")
                A_.wait("its transmission", lambda: A_.tx or watch.why, 60)
                # the frames come a slot each: done once b has the whole text, or nothing more is sent for a slot
                end, last_tx = time.time() + 75, time.time()
                while time.time() < end and not watch.why:
                    if A_.tx:
                        last_tx = time.time()
                    if any(JS8_TEXT in (m.get("x") or "").upper() for m in ab.js8_msgs(B_, t, fr=TEST_CALLS[a])):
                        break
                    if not A_.tx and time.time() - last_tx > 20:
                        break
                    time.sleep(0.2)
                A_.wait("off the air", lambda: not A_.tx or watch.why, bench.MAX_KEY_S + 2)
                if watch.why:
                    raise Refused(watch.why)
                s.confirm(f"on the air {time.time() - t:.0f} s from the send")
        finally:
            watch.close()
            A_.send("js8 stop")
            o.roles = {A: "RX", B: "RX"}
        with o.step(f"{b}'s list shows it from {TEST_CALLS[a]}, the text exact") as s:
            try:
                B_.wait("the message", lambda: any(JS8_TEXT in (m.get("x") or "").upper()
                                                   for m in ab.js8_msgs(B_, t, fr=TEST_CALLS[a])), 20)
            except Refused:
                got = [m.get("x") for m in ab.js8_msgs(B_, t)]
                raise Refused(f"{b}'s list: {got[:4] or 'nothing'}")
            m = next(m for m in ab.js8_msgs(B_, t, fr=TEST_CALLS[a]) if JS8_TEXT in (m.get("x") or "").upper())
            res["decoded"] = m.get("x")
            s.confirm(repr(m.get("x")))
    except Refused as e:
        return False, str(e), res
    finally:
        for r in (A_, B_):
            r.send("js8 stop")
            r.send("js8 close")
        o.apps_closed(ab.F_JS8, 3)
    return True, f"{a}'s '{TEST_CALLS[b]} {JS8_TEXT}' in {b}'s list as {res['decoded']!r}", res


def t_system_health(o):
    """Each radio: its battery log written in the last 150 s, its clock within 1.5 s of this computer's, its data
    partition mounted read-write"""
    out = {}
    with o.step("each radio's battery log, clock and data partition") as s:
        def one(n, r):
            t0 = time.time()
            got = bench.ssh(r.host, "python3 -c 'import time; print(time.time())'; tail -1 /mnt/battery_log.csv; "
                                    "grep ' /mnt ' /proc/mounts")
            t1 = time.time()
            lines = got.strip().split("\n")
            clk = float(lines[0]) - (t0 + t1) / 2
            row_age = time.time() - int(lines[1].split(",")[0])
            rw = " rw," in lines[2] or " rw " in lines[2]
            bad = []
            if abs(clk) > 1.5:
                bad.append(f"clock {clk:+.2f} s")
            if row_age > 150:
                bad.append(f"battery log {row_age:.0f} s old")
            if not rw:
                bad.append(f"/mnt not read-write ({lines[2][:60]})")
            if bad:
                raise Refused(", ".join(bad))
            return {"clock_s": round(clk, 2), "battery_row_s": round(row_age), "mnt_rw": rw}
        out = in_parallel(o, one)
        s.confirm(str(out))
    return True, f"battery logs fresh, clocks within 1.5 s, /mnt read-write on both: {out}", out


def t_js8_close_to_g(o, rounds=5):
    """The X6200's GUI died (SIGSEGV, exit 139, 2026-11-23 20:36) as the JS8 app closed back to the Sentry's G, which
    tuned its own channel (CB 20, 27.205 AM) from 14.078 USB-D. The same on both, rounds times: G's channel CB 20,
    JS8 open on both, closed on both; each GUI must stay up (its state never stale) through every round"""
    import suite_air_b as ab
    deaths = []
    for k in range(rounds):
        with o.step(f"round {k + 1}: G's channel CB 20 AM, JS8 opened and closed on both, both GUIs up") as s:
            o.changing += 1
            try:
                o.both_page("G")
                o.both({"wch": (27205000, 6)})
                for r in o.r.values():
                    r.send("js8 open")
                for r in o.r.values():
                    r.wait("the JS8 app", lambda r=r: ab.last_json(r, "js8").get("open") == 1 or
                           r.state.get("app") == "JS8", 10)
                time.sleep(3.0)
                for r in o.r.values():
                    r.send("js8 close")
                t0 = time.time()
                while time.time() - t0 < 6:
                    for n, r in o.r.items():
                        if r.stale(3.0) and n not in deaths:
                            deaths.append(f"{n} in round {k + 1}")
                    time.sleep(0.2)
            finally:
                o.changing -= 1
            if deaths:
                for n in o.r:
                    if o.r[n].stale(3.0):           # back, for the rounds and tests after
                        t1 = time.time()
                        while time.time() - t1 < 60:
                            try:
                                o.r[n].reconnect()
                                break
                            except (Refused, OSError):
                                time.sleep(3)
                raise Refused(f"a GUI died: {deaths}")
            s.confirm("both GUIs up")
    o.both_page("G")
    o.both({"freq": 14200000, "mode": 2})
    return True, f"{rounds} rounds of JS8 closed back to G on CB 20 AM: both GUIs up throughout", {}


CAP_FREQS = [100000, 500000, 1000000, 1800000, 3500000, 5330500, 7000000, 10100000, 14000000, 18068000, 21000000,
             24890000, 27185000, 28000000, 29700000, 30000000, 49900000, 50000000, 54000000, 60000000, 70000000,
             88000000, 99900000, 108000000, 118000000, 144000000, 146000000, 430000000]


def t_caps(o):
    """Every option probed on both radios at once: the same value sent to both, what each takes and reads back
    (frequencies 0.1-430 MHz, each mode, each mode's widest filter, IF shift, zoom, steps, AGC, NB, NR); both put
    alike again after each probe. The map (orch_caps.json, the console) is what the tests choose their values from"""
    caps = {}

    def probe(label, cmd, read, ok_if, then=None):
        def one(n, r):
            e0 = len(r.errs)
            for attempt in range(2):            # no change and no answer: sent again once (a link stall ate it:
                r.send(cmd)                     # the X6200's 'step 10', 2026-11-23, its link stalling then)
                t0 = time.time()
                while time.time() - t0 < 2.5:
                    if len(r.errs) > e0 or ok_if(r):
                        break
                    time.sleep(0.05)
                if len(r.errs) > e0 or ok_if(r):
                    break
            time.sleep(0.2)
            return {"ok": ok_if(r) and len(r.errs) == e0, "reads": read(r),
                    "err": r.errs[-1] if len(r.errs) > e0 else None}
        got = in_parallel(o, one)
        same = got[A]["ok"] == got[B]["ok"] and got[A]["reads"] == got[B]["reads"]
        caps[label] = {"radios": got, "same": same}
        if not same:
            o.st.ev(f"differ: {label}: x6100 {got[A]}, x6200 {got[B]}", "radio")
        if then:
            then()

    def base():
        o.changing += 1
        try:
            o.both({"freq": 14200000, "mode": 2})
        finally:
            o.changing -= 1
    with o.step("both on page G, 14.200 MHz USB"):
        o.both_page("G")
        base()
    found = {n: {"step": r.state.get("step"), "zoom": r.state.get("zoom")} for n, r in o.r.items()}
    o.changing += 1
    try:
        with o.step("each mode's own tuning step on each radio, as it holds it (a knob turn moves the dial by it)") as s:
            steps = {}
            for m in range(8):
                if not common("mode", m):           # a mode only one radio has (the caps map says)
                    continue
                o.both({"mode": m})
                steps[m] = {n: r.state.get("step") for n, r in o.r.items()}
                same = steps[m][A] == steps[m][B]
                caps[f"step held in mode {m}"] = {"radios": {n: {"ok": True, "reads": v} for n, v in steps[m].items()},
                                                  "same": same}
                if not same:
                    o.st.ev(f"differ: mode {m}'s step: {steps[m]}", "radio")
            base()
            s.confirm(str(steps))
        with o.step("one detent of the main knob on both at 14.200 USB: the same move") as s:
            def detent(n, r):
                f0 = int(r.state.get("freq", 0))
                r.send("knob main 1")
                r.wait("the dial moved", lambda: int(r.state.get("freq", 0)) != f0, 3)
                return int(r.state.get("freq", 0)) - f0
            moved = in_parallel(o, detent)
            caps["knob main 1 at 14.200 USB"] = {"radios": {n: {"ok": True, "reads": v} for n, v in moved.items()},
                                                 "same": moved[A] == moved[B]}
            base()
            s.confirm(f"moved {moved} Hz")
        with o.step("each radio's band table (its web page's /api/bands): names, edges, kinds") as s:
            from suite_proto import http
            tables = {}
            for n, r in o.r.items():
                st_, body = http(r.host, "/api/bands")
                tables[n] = {b["name"]: (b["start_freq"], b["stop_freq"], b.get("type"))
                             for b in (json.loads(body) if st_ == 200 else [])}
            for name in sorted(set(tables[A]) | set(tables[B])):
                a_, b_ = tables[A].get(name), tables[B].get(name)
                caps[f"band {name}"] = {"radios": {A: {"ok": a_ is not None, "reads": a_},
                                                   B: {"ok": b_ is not None, "reads": b_}}, "same": a_ == b_}
            s.confirm(f"{len(tables[A])}/{len(tables[B])} bands, "
                      f"{sum(not v['same'] for k, v in caps.items() if k.startswith('band '))} differ")
        with o.step(f"{len(CAP_FREQS)} frequencies, 0.1 to 430 MHz") as s:
            for f in CAP_FREQS:
                probe(f"freq {f}", f"set freq {f}", lambda r: r.state.get("freq"),
                      lambda r, f=f: int(r.state.get("freq", 0)) == f, base)
            s.confirm(f"{sum(not caps[f'freq {f}']['same'] for f in CAP_FREQS)} differ")
        with o.step("each mode 0-8 at 14.200 MHz (8 at 99.9 MHz too)") as s:
            for m in range(9):
                probe(f"mode {m}", f"set mode {m}", lambda r: r.state.get("mode"),
                      lambda r, m=m: int(r.state.get("mode", -1)) == m, base)
            probe("mode 8 at 99.9 MHz", "set freq 99900000", lambda r: (r.state.get("freq"), r.state.get("mode")),
                  lambda r: int(r.state.get("freq", 0)) == 99900000)
            for r in o.r.values():
                r.send("set mode 8")
            time.sleep(1.0)
            caps["mode 8 at 99.9 MHz"]["radios"] = {n: {"ok": int(r.state.get("freq", 0)) == 99900000 and
                                                        int(r.state.get("mode", -1)) == 8,
                                                        "reads": (r.state.get("freq"), r.state.get("mode"))}
                                                    for n, r in o.r.items()}
            v = caps["mode 8 at 99.9 MHz"]["radios"]
            caps["mode 8 at 99.9 MHz"]["same"] = v[A] == v[B]
            base()
            s.confirm(f"{sum(not v['same'] for k, v in caps.items() if k.startswith('mode'))} differ")
        with o.step("each mode's widest filter (0-6000 Hz asked), each radio's own put back after") as s:
            for m, f in ((0, 7200000), (2, 14200000), (4, 14040000), (6, 29000000), (7, 29600000)):
                o.both({"freq": f, "mode": m})
                # each radio's filter in this mode, put back once probed: kept, 0-6000 stayed in CW, AM and NFM on
                # both radios from 2026-11-23 20:39 (CW 6 kHz wide: the CW decoder's floor taken over 0-2 kHz, the
                # band's noise in the real passband keyed it and set its speed; the first dits read as dahs)
                was = {n: (int(r.state.get("flo", -1)), int(r.state.get("fhi", -1))) for n, r in o.r.items()}
                try:
                    probe(f"filter 0-6000 in mode {m}", "set filter 0 6000",
                          lambda r: (r.state.get("flo"), r.state.get("fhi")), lambda r: True)
                finally:
                    o.changing += 1
                    try:
                        for n, r in o.r.items():
                            if min(was[n]) >= 0:
                                bench.ensure(r, {"filter": was[n]})
                    finally:
                        o.changing -= 1
            base()
            s.confirm(f"{sum(not v['same'] for k, v in caps.items() if k.startswith('filter'))} differ")
        with o.step("IF shift, zoom, steps, AGC, NB, NR at 14.200 USB") as s:
            for v in (500, 0):
                probe(f"ifs {v}", f"set ifs {v}", lambda r: r.state.get("ifs"), lambda r, v=v: int(r.state.get("ifs") or 0) == v)
            for z in (1, 2, 4, 8, 16, 32):
                probe(f"zoom {z}", f"set zoom {z}", lambda r: r.state.get("zoom"), lambda r, z=z: int(r.state.get("zoom", 0)) == z)
            for st_ in (10, 100, 500, 1000, 5000, 9000, 10000):
                probe(f"step {st_}", f"set step {st_}", lambda r: r.state.get("step"),
                      lambda r, st_=st_: int(r.state.get("step", 0)) == st_)
            for k, vals in (("agc", (0, 1, 2, 3)), ("nb", (1, 0)), ("nr", (1, 0))):
                for v in vals:
                    probe(f"{k} {v}", f"set {k} {v}", lambda r, k=k: r.state.get(k), lambda r, k=k, v=v: int(r.state.get(k, -1)) == v)
            s.confirm(f"{sum(not v['same'] for v in caps.values())} differ in all")
    finally:
        o.changing -= 1
        base()
        with o.step("each radio's USB step and zoom put back as found"):     # settings, not the dial: back
            for n, r in o.r.items():
                for k in ("step", "zoom"):
                    if found[n][k] is not None:
                        r.send(f"set {k} {int(found[n][k])}")
            time.sleep(1.0)
    bench.save_json(CAPS, caps)
    diff = [k for k, v in caps.items() if not v["same"]]
    return True, f"{len(caps)} options probed on both, {len(diff)} differ: {', '.join(diff)[:900]}", {"diff": diff}


def t_bands_all(o):
    """Every band both radios have with the same edges (their band tables, the caps map): both into it at once (its
    start + 1 kHz, the band's own mode from its memory), each confirmed, the two alike in frequency and mode"""
    from suite_proto import http
    tables = {}
    for n, r in o.r.items():
        st_, body = http(r.host, "/api/bands")
        tables[n] = {b["name"]: b for b in (json.loads(body) if st_ == 200 else [])}
    names = [k for k in tables[A] if k in tables[B] and (tables[A][k]["start_freq"], tables[A][k]["stop_freq"]) ==
             (tables[B][k]["start_freq"], tables[B][k]["stop_freq"])]
    with o.step("both on page G"):
        o.both_page("G")
    seen, apart, skipped = [], [], []
    for k in sorted(names, key=lambda k: tables[A][k]["start_freq"]):
        f = int(tables[A][k]["start_freq"]) + 1000
        if not common("freq", f):
            continue
        with o.step(f"both into {k} ({f / 1e6:.4f} MHz)") as s:
            try:
                o.both({"freq": f})
            except Refused as e:                # a band in the tables a radio does not tune: mapped, passed over
                n_no = str(e).count("not a frequency this radio tunes")
                if not n_no:
                    raise
                who = "neither" if n_no == 2 else ("x6100" if "x6100: freq" not in str(e) else "x6200") + " only"
                skipped.append(f"{k} ({who})")
                caps = caps_load()
                caps[f"band {k} tuned"] = {"radios": {n: {"ok": f"{n}: freq" not in str(e)} for n in o.r},
                                           "same": n_no == 2}
                bench.save_json(CAPS, caps)
                o.both({"freq": 14200000})
                s.confirm(f"tuned by {who}: passed over, mapped")
                continue
            time.sleep(0.3)
            m = {n: int(r.state.get("mode", -1)) for n, r in o.r.items()}
            if m[A] != m[B]:                    # each band's mode from its own memory: put alike, said
                apart.append(f"{k}: modes {m}")
                o.both({"mode": m[A]})
            seen.append(k)
            s.confirm(f"modes {m}")
    o.both({"freq": 14200000, "mode": 2})
    if apart:
        return None, f"{len(seen)} bands entered alike on both; the bands' memories gave different modes in " \
                     f"{len(apart)}: {'; '.join(apart)}", {"apart": apart}
    return True, (f"{len(seen)} bands, each entered on both at once, the same frequency and mode: {', '.join(seen)}"
                  + (f"; not on both (mapped): {', '.join(skipped)}" if skipped else "")), \
        {"bands": seen, "neither": skipped}


def wefax_chart(o, sender, listener, label):
    """Both on 14.100 MHz USB, the listener's WeFax app open in Cont; 12 s of a chart (each line half white, half
    black) through the sender's TCI at 0.1 W; the listener's fax saved and fetched: at least 6 rows, and its two halves
    at least 80 grey levels apart (the chart about 200, noise under 30). The test's fax taken off the card after"""
    import subprocess
    import suite_air_b as ab
    S, L = o.r[sender], o.r[listener]
    res = {}
    files0 = set(bench.ssh(L.host, "find /mnt/wefax -type f 2>/dev/null").split())
    try:
        with o.step(f"{listener}'s WeFax open in Cont, both on 14.100 MHz USB, alike") as s:
            o.changing += 1
            try:
                o.both_page("G")
                L.send("wefax open")
                L.wait("WeFax open", lambda: L.state.get("app") == "WeFax" or ab.last_json(L, "wefax").get("open") == 1,
                       10)
                time.sleep(1.5)
                if ab.last_json(L, "wefax").get("plan"):
                    raise Refused(f"{listener}'s WeFax Auto Plan is on (it follows the day's charts)")
                L.send("wefax cont")
                L.wait("Cont", lambda: ab.last_json(L, "wefax").get("auto") == 0, 5)
                o.both({"freq": ab.F_FAX, "mode": 2})
            finally:
                o.changing -= 1
            s.confirm("WeFax in Cont, both 14.100 USB")
        x = ab.fax_audio([("chart", 12.0)])
        local = os.path.join(o.st.run_dir, f"fax_{sender}.raw")
        import digimodes
        secs = digimodes.write_raw(x, local)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:/tmp/bench_fax.raw"], check=True)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "tci_tone_tx.py"), f"root@{S.host}:/tmp/tci_tone_tx.py"], check=True)
        L.send("wefax clear")
        saves0 = ab.last_json(L, "wefax").get("saves", 0)

        def body(watch):
            with o.step(f"{sender} sends 12 s of the chart"):
                bench.tci_send(S, None, max_s=secs + 4, watch=watch, file="/tmp/bench_fax.raw", amp=0.5)
                if watch.why:
                    raise Refused(watch.why)
        over(o, sender, listener, ab.F_FAX, 2, label, body, pages=False)
        with o.step(f"{listener}'s fax saved, fetched, its two halves compared") as s:
            rows = ab.last_json(L, "wefax").get("rows", 0)
            L.send("wefax save")
            L.wait("the save", lambda: ab.last_json(L, "wefax").get("saves", 0) > saves0, 8)
            name = ab.last_json(L, "wefax").get("saved", "")
            pic = os.path.join(o.st.run_dir, f"wefax_{listener}_{os.path.basename(name)}")
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", f"root@{L.host}:/mnt/wefax/{os.path.basename(name)}",
                            pic], capture_output=True, timeout=60)
            contrast = ab.fax_contrast(pic) if os.path.exists(pic) else None
            res.update(rows=rows, saved=os.path.basename(name), contrast=contrast, picture=pic)
            if rows < 6 or contrast is None or contrast < 80:
                raise Refused(f"{rows} rows, its halves {contrast} grey levels apart (6 rows and 80 wanted)")
            s.confirm(f"{rows} rows, halves {contrast} grey levels apart, kept as {os.path.basename(pic)}")
    except Refused as e:
        return False, str(e), res
    finally:
        new = set(bench.ssh(L.host, "find /mnt/wefax -type f 2>/dev/null").split()) - files0
        if new:
            bench.ssh(L.host, "rm -f " + " ".join(sorted(new)))
        o.changing += 1
        try:
            L.send("wefax close")
            time.sleep(1.5)
            bench.close_apps(L)
            o.both_page("G")
            o.both({"freq": ab.F_FAX, "mode": 2})
        finally:
            o.changing -= 1
    return True, f"{res['rows']} rows of the chart, its two halves {res['contrast']} grey levels apart (80 wanted)", res


# ---- SSTV, Olivia, the CW Keyboard (2026-11-24, the owner: "Yes, add them") ----

SSTV_MODE = "robot8"            # Robot 8 BW, 160 x 120, 8.9 s (9.7 with the tail): a picture that fits an over (16 s)
SSTV_W, SSTV_H = 160, 120
SSTV_MIN_LEVELS = 80            # the received picture's white quarters over its black ones, grey levels, at least
SSTV_LAST_ROWS = 4              # the received picture's last rows against the sent one's: a sender that unkeys early
SSTV_MAX_ROW_ERR = 40           # leaves them noise (100-130 grey levels off before the 0.8 s tail, 3-6 after, 2026-11-24)


def sstv_encoder():
    """The GUI's own SSTV encoder (src/sstv/sstv_tx.c) built for this computer (sstv_enc.c, libpng), rebuilt when a
    source is newer: its path"""
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    sstv = os.path.join(here, "..", "..", "src", "sstv")
    src = [os.path.join(here, "sstv_enc.c")] + [os.path.join(sstv, n) for n in ("sstv_tx.c", "sstv_png.c",
                                                                                "sstv_mode.c")]
    out = os.path.join(os.path.dirname(STORE), "sstv_enc")
    if os.path.exists(out) and os.path.getmtime(out) >= max(os.path.getmtime(p) for p in src):
        return out
    png = subprocess.run(["pkg-config", "--cflags", "--libs", "libpng"], capture_output=True, text=True).stdout.split()
    r = subprocess.run(["cc", "-O2", "-I", sstv, *src, *png, "-lm", "-o", out], capture_output=True, text=True)
    if r.returncode:
        raise Refused(f"the SSTV encoder did not build here: {r.stderr.strip()[-300:]}")
    return out


def sstv_quarters(path):
    """A picture's four quarters' mean grey (a margin of 15 % of each side left out: the edges of a received picture
    lean with the clocks): (top left, top right, bottom left, bottom right)"""
    from PIL import Image
    a = np.asarray(Image.open(path).convert("L"), float)
    h, w = a.shape
    mh, mw = int(h * 0.15 / 2), int(w * 0.15 / 2)

    def q(r0, r1, c0, c1):
        return round(float(a[r0 + mh:r1 - mh, c0 + mw:c1 - mw].mean()), 1)
    return q(0, h // 2, 0, w // 2), q(0, h // 2, w // 2, w), q(h // 2, h, 0, w // 2), q(h // 2, h, w // 2, w)


def sstv_last_rows(sent, got):
    """The received picture's last SSTV_LAST_ROWS rows, each one's mean grey difference from the sent picture's"""
    from PIL import Image
    s = np.asarray(Image.open(sent).convert("L"), float)
    g = np.asarray(Image.open(got).convert("L").resize((s.shape[1], s.shape[0])), float)
    return [round(float(x), 1) for x in np.abs(s - g).mean(axis=1)[-SSTV_LAST_ROWS:]]


def sstv_picture(o, sender, listener, label):
    """The listener's SSTV app open on its dial (both on it, alike), Auto RX started; a Robot 8 picture (its top left
    and bottom right quarters white, the others black) made by the GUI's own encoder here and played through the
    sender's TCI at 0.1 W; the listener's saved picture fetched: its white quarters at least SSTV_MIN_LEVELS grey
    levels over its black ones, each white one over each black one. The test's pictures taken off the card after"""
    import subprocess
    import suite_air_b as ab
    import suite_more
    from PIL import Image
    S, L = o.r[sender], o.r[listener]
    res = {"mode": SSTV_MODE}
    ls = "ls /mnt/sstv/*.png 2>/dev/null; true"
    files0 = set(bench.ssh(L.host, ls).split())
    f = m = None
    try:
        with o.step(f"{listener}'s SSTV app open, both on its dial and mode, alike") as s:
            o.changing += 1
            try:
                o.both_page("G")
                if not suite_more.open_app(L, "sstv"):
                    raise Refused(f"{listener}'s SSTV app did not open")
                time.sleep(1.5)
                f, m = int(L.state.get("freq", 0)), int(L.state.get("mode", -1))
                o.both({"freq": f, "mode": m})
            finally:
                o.changing -= 1
            s.confirm(f"both on {f / 1e6:.4f} MHz mode {m}")
        with o.step(f"the test picture made in {SSTV_MODE} by the GUI's own encoder") as s:
            a = np.zeros((SSTV_H, SSTV_W), np.uint8)
            a[:SSTV_H // 2, :SSTV_W // 2] = 255
            a[SSTV_H // 2:, SSTV_W // 2:] = 255
            sent = os.path.join(o.st.run_dir, f"sstv_sent_{sender}.png")
            Image.fromarray(a).convert("RGB").save(sent)
            raw = os.path.join(o.st.run_dir, f"sstv_{sender}.raw")
            r = subprocess.run([sstv_encoder(), SSTV_MODE, sent, raw, "24000"], capture_output=True, text=True)
            if r.returncode:
                raise Refused(f"the encoder: {r.stderr.strip()[-200:]}")
            with open(raw, "ab") as fh:     # keyed silence after it, as the steps have: the sender unkeys as its TCI
                fh.write(bytes(2 * int(STEP_TAIL_S * 24000)))   # audio ends (the last rows lost, 2026-11-24)
            secs = os.path.getsize(raw) / 2 / 24000
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", raw, f"root@{S.host}:/tmp/bench_sstv.raw"], check=True)
            res.update(sent=sent, sent_quarters=sstv_quarters(sent))
            s.confirm(f"{secs:.1f} s of audio, the quarters {res['sent_quarters']}")
        with o.step(f"{listener}'s Auto RX started (its page 1, F2)"):
            ab.tap(L, "F2")
            time.sleep(1.0)

        def body(watch):
            with o.step(f"{sender} sends the picture ({secs:.1f} s of {SSTV_MODE})"):
                bench.tci_send(S, None, max_s=secs + 4, watch=watch, file="/tmp/bench_sstv.raw", amp=0.5)
                if watch.why:
                    raise Refused(watch.why)
        over(o, sender, listener, f, m, label, body, pages=False)
        with o.step(f"{listener} saved the picture, fetched, its quarters compared with the sent one's") as s:
            new, t0 = [], time.time()
            while time.time() - t0 < 20:
                new = sorted(p for p in set(bench.ssh(L.host, ls).split()) - files0 if not p.endswith("_dn.png"))
                if new:
                    break
                time.sleep(2.0)
            if not new:
                raise Refused(f"{listener} saved no picture within 20 s of the over")
            got = os.path.join(o.st.run_dir, f"sstv_{listener}_{os.path.basename(new[-1])}")
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", f"root@{L.host}:{new[-1]}", got], capture_output=True,
                           timeout=60)
            if not os.path.exists(got):
                raise Refused(f"{new[-1]} not fetched from {listener}")
            tl, tr, bl, br = sstv_quarters(got)
            size = Image.open(got).size
            apart = round(min(tl, br) - max(tr, bl), 1)
            res.update(received=got, size=size, quarters=(tl, tr, bl, br), apart=apart)
            if apart < SSTV_MIN_LEVELS:
                raise Refused(f"its quarters (top left, top right, bottom left, bottom right) {tl}, {tr}, {bl}, {br}: "
                              f"the white over the black by {apart} grey levels ({SSTV_MIN_LEVELS} wanted); "
                              f"{size[0]}x{size[1]}, kept as {os.path.basename(got)}")
            rows = sstv_last_rows(sent, got)
            res.update(last_rows=rows)
            if max(rows) > SSTV_MAX_ROW_ERR:
                raise Refused(f"its last {SSTV_LAST_ROWS} rows {rows} grey levels off the sent ones ({SSTV_MAX_ROW_ERR} "
                              f"at most): the end of the picture lost; kept as {os.path.basename(got)}")
            s.confirm(f"{size[0]}x{size[1]}, quarters {tl}/{tr}/{bl}/{br}, white over black by {apart}; the last "
                      f"rows {rows} off")
    except Refused as e:
        return False, str(e), res
    finally:
        new = set(bench.ssh(L.host, ls).split()) - files0
        if new:
            bench.ssh(L.host, "rm -f " + " ".join(sorted(new)))
        o.changing += 1
        try:
            bench.close_apps(L)
            o.both_page("G")
            if f:
                o.both({"freq": f, "mode": m})
        finally:
            o.changing -= 1
    return True, (f"a {SSTV_MODE} picture: {listener}'s {res['size'][0]}x{res['size'][1]} shows the quarters "
                  f"{res['quarters']}, white over black by {res['apart']} grey levels ({SSTV_MIN_LEVELS} wanted), "
                  f"its last rows {res['last_rows']} off ({SSTV_MAX_ROW_ERR} at most)"), res


OLIVIA_TEXT = "TEST"        # no "DE <call>" (what the app spots); the app sends "\nTEST\n": 6 characters, 7 blocks of
                            # Olivia 8/250, 14.3 s on the air (suite_air_b)
OLIVIA_MACROS = "/mnt/olivia_macros.txt"


def textlog_lines(host, before):
    """The text log's lines (/mnt/digi_text) since its sizes before (suite_air_b.tl_sizes)"""
    import suite_air_b as ab
    return ab.tl_new(host, before)


def textlog_back(host, before):
    """The text log cut back to its sizes before: the test's lines out (a file the test began removed)"""
    import suite_air_b as ab
    now = ab.tl_sizes(host)
    cmds = [f"rm -f {p}" if p not in before else f"python3 -c \"import os;os.truncate('{p}',{before[p]})\""
            for p, n in now.items() if p not in before or n > before[p]]
    if cmds:
        bench.ssh(host, "; ".join(cmds))


def olivia_text(o, a, b, label):
    """Both radios' Olivia apps open on their dial (each app's memory: alike, else refused), a macro OLIVIA_TEXT added
    to the sender's list (its file kept and put back after), sent by its page key at 0.1 W; the listener's text log:
    an RX line in the variant the sender's TX line names, with OLIVIA_TEXT in it. The test's lines taken out of both
    text logs after"""
    import suite_air_b as ab
    import suite_cover
    S, L = o.r[a], o.r[b]
    res = {"sent": OLIVIA_TEXT}
    p, total = bench.chain_gate(o.c, "radios")
    logs = {r.name: ab.tl_sizes(r.host) for r in (S, L)}
    kept = suite_cover.keep(S.host, OLIVIA_MACROS, "save")
    if kept not in ("saved", "none"):
        return False, f"{a}: {OLIVIA_MACROS} not kept aside ({kept!r})", res
    f = m = None
    try:
        with o.step(f"the macro {OLIVIA_TEXT!r} added to {a}'s Olivia list (its web page)") as s:
            ip = ab.web_ip(S.host)
            st, body0 = ab.http(ip, "/api/macros/olivia")
            if st != 200:
                raise Refused(f"{a}'s /api/macros/olivia answered {st}")
            have = json.loads(body0)
            if len(have["macros"]) >= have["max"]:
                raise Refused(f"{a}'s Olivia list is full ({len(have['macros'])} of {have['max']})")
            st, b_ = ab.http(ip, "/api/macros/olivia", method="POST", body={"name": "BENCH", "text": OLIVIA_TEXT})
            if st != 200:
                raise Refused(f"the macro not added ({st} {b_[:80]})")
            listed = json.loads(b_)["macros"]
            i_m = next((k for k, x in enumerate(listed) if x["name"] == "BENCH" and x["text"] == OLIVIA_TEXT), None)
            if i_m is None:
                raise Refused("the macro not in the list its web page gave back")
            s.confirm(f"macro {i_m + 1} of {len(listed)}")
        with o.step("the Olivia app open on both, on one dial, alike") as s:
            o.changing += 1
            try:
                o.both_page("G")
                for r in (L, S):
                    ab.open_listed(r, "olivia", "contestia")
                if ab.app_name(S) != ab.app_name(L):
                    raise Refused(f"the apps differ: {a} {ab.app_name(S)}, {b} {ab.app_name(L)}")
                f, m = int(L.state.get("freq", 0)), int(L.state.get("mode", -1))
                if (int(S.state.get("freq", 0)), int(S.state.get("mode", -1))) != (f, m):
                    raise Refused(f"the apps on two dials ({a} {S.state.get('freq')}, {b} {f}): each opens where its "
                                  f"memory (/mnt/app_freqs) says")
                o.both({"pwr": W})
            finally:
                o.changing -= 1
            alike(o, ("freq", "mode"))
            o.pair_alike()
            s.confirm(f"{ab.app_name(L)} on {f / 1e6:.4f} MHz mode {m}")
        bench.radio_tx_gate(S, o.c, W)
        bench.listener_gate(L)
        o.roles = {a: "TX", b: "RX"}
        watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W, label=label)
        try:
            with o.step(f"{a} sends the macro (its page key), on the air till it is done") as s:
                t = time.time()
                bench.tx_log({"who": a, "f": f, "w": S.pwr(), "what": f"orch: {label}"})
                ab.Pages(S).macro(i_m)
                S.wait("its transmission", lambda: S.tx or watch.why, 8)
                ab.settle(S, watch)
                S.wait("off the air", lambda: not S.tx or watch.why, bench.MAX_KEY_S + 2)
                if watch.why:
                    raise Refused(watch.why)
                s.confirm(f"on the air {time.time() - t:.0f} s from the key")
        finally:
            watch.close()
            o.roles = {A: "RX", B: "RX"}
        with o.step(f"{b}'s text log: an RX line in {a}'s variant with {OLIVIA_TEXT!r}") as s:
            time.sleep(4.0)                     # the decoder's last block
            # both apps closed in one change (the listener's line ends onto the card as it closes), both on the dial
            # again: a radio leaving its app goes back to the dial it had before it (2026-11-24: the X6100 on 7.200
            # LSB while the X6200's app still held 3.5815, out of sync)
            o.changing += 1
            try:
                for r in (L, S):
                    bench.close_apps(r)
                o.both_page("G")
                o.both({"freq": f, "mode": m})
            finally:
                o.changing -= 1
            tx = [ln for ln in textlog_lines(S.host, logs[a]) if " TX " in ln]
            variant = tx[-1].split()[3] if tx and len(tx[-1].split()) > 3 else None
            rx, t0 = [], time.time()
            while time.time() - t0 < 15:
                rx = [ln for ln in textlog_lines(L.host, logs[b]) if " RX " in ln]
                if any(OLIVIA_TEXT in ln.upper() for ln in rx):
                    break
                time.sleep(3.0)
            hit = [ln for ln in rx if OLIVIA_TEXT in ln.upper()]
            res.update(variant=variant, rx=[ln[-100:] for ln in rx[-3:]])
            if not hit:
                raise Refused(f"{b}'s RX lines: {[ln[-80:] for ln in rx[-3:]] or 'none'} ({a} sent in {variant})")
            if variant and f" {variant} RX " not in hit[-1]:
                raise Refused(f"{b} decoded it in another variant: {hit[-1][-80:]!r} ({a} sent {variant})")
            res["decoded"] = hit[-1].split(" ", 6)[-1].strip()
            s.confirm(f"{variant}: {hit[-1][-80:]!r}")
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for r in (S, L):
                if r.tx:
                    r.stop()
                bench.close_apps(r)
            back = suite_cover.keep(S.host, OLIVIA_MACROS, "back")
            if back not in ("same", "restored", "removed"):
                o.st.ev(f"{a}: {OLIVIA_MACROS} not put back ({back!r})", "bad")
            for r in (S, L):
                textlog_back(r.host, logs[r.name])
            o.both_page("G")
            if f:
                o.both({"freq": f, "mode": m})
        finally:
            o.changing -= 1
    return True, f"{a}'s {res['variant']} macro {OLIVIA_TEXT!r}, {b}'s line {res['decoded']!r}", res


CW_KBD_TEXT = "VVV VVV Q1TST"   # two V groups first: the listener's decoder finds the speed on them (suite_air_b)
CW_KBD_MUST = "Q1TST"


def cw_kbd(o, sender, listener, f, label):
    """A CW message CW_KBD_TEXT added on the sender's web page (taken out after), both in CW on f on G (AGC fast, as
    cw-text), the sender's CW Keyboard opened on that dial and its message keyed by its page key at 0.1 W; the
    listener's decoder (the remote's cw lines) must print CW_KBD_MUST"""
    import suite_air_b as ab
    S, L = o.r[sender], o.r[listener]
    got = {"sent": CW_KBD_TEXT}
    ip = ab.web_ip(S.host)
    mine = []
    try:
        with o.step(f"the message {CW_KBD_TEXT!r} added to {sender}'s CW messages (its web page)") as s:
            wpm = ab.key_wpm(S.host)
            secs = ab.morse_secs(CW_KBD_TEXT, wpm)
            if secs > ab.CW_MAX_S:
                raise Refused(f"{sender}'s KEY Speed {wpm} WPM: {CW_KBD_TEXT!r} takes {secs:.1f} s, over "
                              f"{ab.CW_MAX_S:.0f} s (the speed left alone)")
            st, b0 = ab.http(ip, "/api/cw_messages")
            if st != 200:
                raise Refused(f"{sender}'s /api/cw_messages answered {st}")
            before = json.loads(b0)["messages"]
            if len(before) > 7:
                raise Refused(f"{sender} has {len(before)} CW messages: the CW Keyboard shows eight")
            st, b_ = ab.http(ip, "/api/cw_messages", method="POST", body={"text": CW_KBD_TEXT})
            if st != 200:
                raise Refused(f"the message not added ({st} {b_[:80]})")
            mine.append(max(x["id"] for x in json.loads(b_)["messages"] if x["text"] == CW_KBD_TEXT))
            order = [x["id"] for x in json.loads(ab.http(ip, "/api/cw_messages")[1])["messages"]]
            i, n = order.index(mine[0]), len(order)
            npg = (n + 3) // 4              # dialog_cw_kbd.c: page 1, the message pages, Live
            s.confirm(f"message {i + 1} of {n}, {secs:.1f} s at {wpm} WPM")

        def body(watch):
            with o.step(f"{sender}'s CW Keyboard open on the dial, its message keyed (page {2 + i // 4}, "
                        f"F{2 + i % 4})") as s:
                # the app opens on the radio's own dial, not G's (2026-11-24: on 7.200 LSB): put on f in CW, both
                o.changing += 1
                try:
                    ab.open_listed(S, "cw keyboard")
                    opened = (int(S.state.get("freq", 0)), int(S.state.get("mode", -1)))
                    if opened != (f, 4):
                        o.both({"freq": f, "mode": 4})
                finally:
                    o.changing -= 1
                if (int(S.state.get("freq", 0)), int(S.state.get("mode", -1))) != (f, 4):
                    raise Refused(f"the CW Keyboard opened on {opened[0]} mode {opened[1]} and stayed there, not CW "
                                  f"on {f}")
                alike(o, ("freq", "mode"))
                try:
                    L.cw = ""
                    for _ in range((1 + i // 4) % (npg + 2)):
                        ab.tap(S, "F1")
                    t = time.time()
                    ab.tap(S, f"F{2 + i % 4}")
                    S.wait("its keying", lambda: S.tx or watch.why, 6)
                    ab.settle(S, watch)
                    if watch.why:
                        raise Refused(watch.why)
                    s.confirm(f"keyed {time.time() - t:.0f} s")
                finally:
                    o.changing += 1
                    try:
                        if S.tx:
                            S.stop()
                        bench.close_apps(S)     # the CW Keyboard's keyer goes on past the remote's stop: closed
                        bench.open_page(S, "G")
                        o.both({"freq": f, "mode": 4})
                    finally:
                        o.changing -= 1
        over(o, sender, listener, f, 4, label, body, agc=2)
        with o.step(f"{listener}'s decoder printed {CW_KBD_MUST}") as s:
            t0, last, t_last = time.time(), None, time.time()
            while time.time() - t0 < 6:
                if L.cw != last:
                    last, t_last = L.cw, time.time()
                elif time.time() - t_last > 1.5:
                    break
                time.sleep(0.1)
            text = " ".join(L.cw.split()).upper()
            # the decoder prints the band's noise before and after the over: the error rate of its stretch nearest
            # to the message, the whole print kept
            cer, seg = best_cer(text, CW_KBD_TEXT)
            got.update(printed=text, decoded=seg, cer=round(cer, 3))
            if CW_KBD_MUST not in text:
                raise Refused(f"{listener} printed {text!r}: {CW_KBD_MUST} not in it")
            s.confirm(f"{seg!r} ({cer:.0%} of the characters wrong) in {text!r}")
    except Refused as e:
        return False, str(e), got
    finally:
        for k in mine:
            ab.http(ip, f"/api/cw_messages/{k}", method="DELETE")
        left = [x for x in json.loads(ab.http(ip, "/api/cw_messages")[1])["messages"] if x["id"] in mine]
        if left:
            o.st.ev(f"{sender}: the test's CW message not taken out ({left})", "bad")
    return True, f"the CW Keyboard keyed {CW_KBD_TEXT!r}, {listener} copied {got['decoded']!r} ({got['cer']:.0%} " \
                 f"of the characters wrong; all it printed: {got['printed'][-70:]!r})", got



# ---- TUNE for an external tuner from an app (2026-11-24, the owner: "tune with external tuner, do that") ----
EXT_TUNE_HZ = 1500          # FT8's TX offset for the test: the tune's tone lands there (MANUAL "TUNE for an external tuner")
EXT_TUNE_TOL_HZ = 25        # the tone heard in the listener's audio within this of it
EXT_TUNE_MIN_DB = 15        # over the listener's quiet before it


def tone_at(x, lo=200, hi=3000):
    """The strongest tone in audio x (FS): (Hz, dB over the median of lo-hi)"""
    if len(x) < FS // 2:
        return None, None
    w = np.hanning(len(x))
    sp = np.abs(np.fft.rfft(x * w))
    hz = np.fft.rfftfreq(len(x), 1 / FS)
    band = (hz >= lo) & (hz <= hi)
    k = int(np.argmax(np.where(band, sp, 0)))
    return round(float(hz[k]), 1), round(float(20 * np.log10(sp[k] / (np.median(sp[band]) + 1e-12))), 1)


def ext_tune_app(o, sender, listener, label, dial=14074000):
    """The FT8 app on both on dial (USB-D), the sender's TX offset EXT_TUNE_HZ, TUNE's power 0.1 W on both. The
    sender's Ext tune (FT8 page 4, F5): on the air with tune 1, the listener's audio a tone at the offset (within
    EXT_TUNE_TOL_HZ) EXT_TUNE_MIN_DB over its quiet; pressed again: off within 1.5 s; the dial, the mode and the app
    as they were. Pressed once more and left: off by its time limit (tune_s, 15 s), within 2 s of it"""
    import suite_air_b as ab
    from tones import TciRx
    S, L = o.r[sender], o.r[listener]
    got = {}
    rx = None
    try:
        with o.step("both on their test calls, the FT8 app open on both on the dial, everything alike"):
            ft8_both(o, dial)
        with o.step(f"TUNE's power 0.1 W on both, {sender}'s FT8 TX offset {EXT_TUNE_HZ} Hz") as s:
            o.both({"tpwr": 1})
            S.send(f"ft8 txfreq {EXT_TUNE_HZ}")
            time.sleep(1.0)
            lim = int(S.state.get("tune_s") or 15)
            got["limit_s"] = lim
            if lim > 16:
                raise Refused(f"{sender}'s TUNE time limit {lim} s: over the bench's 16 s for one over")
            s.confirm(f"TUNE power {int(S.state.get('tpwr', -1)) / 10} W, its limit {lim} s")
        p, total = bench.chain_gate(o.c, "radios")
        bench.radio_tx_gate(S, o.c, W)
        bench.listener_gate(L)
        rx = TciRx(L.host)
        time.sleep(2.0)
        o.roles = {sender: "TX", listener: "RX"}
        watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W, label=label)
        pg = ab.Pages(S, n=5)
        try:
            with o.step(f"{sender}'s Ext tune (FT8 page 4, F5): on the air with tune 1") as s:
                q0 = time.time()
                pg.to(3)
                bench.tx_log({"who": sender, "f": dial + EXT_TUNE_HZ, "w": W, "what": f"orch: {label}"})
                ab.tap(S, "F5")
                S.wait("on the air with TUNE", lambda: (S.tx and S.state.get("tune")) or watch.why, 3)
                if watch.why:
                    raise Refused(watch.why)
                t_on = time.time()
                s.confirm(f"tune {S.state.get('tune')}, {S.state.get('tune_s')} s limit")
            time.sleep(4.0)
            with o.step(f"Ext tune pressed again: off within 1.5 s") as s:
                t_stop = time.time()
                ab.tap(S, "F5")
                S.wait("off the air", lambda: not S.tx and not S.state.get("tune"), 3)
                d = time.time() - t_stop
                got["stop_s"] = round(d, 2)
                if d > 1.5:
                    raise Refused(f"off {d:.1f} s after the press")
                s.confirm(f"off {d:.2f} s after the press")
            with o.step(f"{listener} heard a tone at the app's offset ({EXT_TUNE_HZ} Hz), "
                        f"{EXT_TUNE_MIN_DB} dB over its quiet") as s:
                time.sleep(1.5)
                x, _ = rx.audio(t_on + 1.0, t_stop - 0.3)
                q, _ = rx.audio(q0 - 1.8, q0 - 0.1)
                hz, db = tone_at(x)
                qhz, qdb = tone_at(q)
                lvl = 10 * np.log10(np.mean(np.square(x)) + 1e-12) if len(x) else None
                qlvl = 10 * np.log10(np.mean(np.square(q)) + 1e-12) if len(q) else None
                got.update(hz=hz, peak_db=db, level_db=lvl and round(lvl, 1), quiet_db=qlvl and round(qlvl, 1))
                if hz is None or abs(hz - EXT_TUNE_HZ) > EXT_TUNE_TOL_HZ:
                    raise Refused(f"the strongest tone {hz} Hz ({db} dB over the band's median), not "
                                  f"{EXT_TUNE_HZ} Hz")
                if lvl is None or qlvl is None or lvl - qlvl < EXT_TUNE_MIN_DB:
                    raise Refused(f"its audio {qlvl} -> {lvl} dBFS, not {EXT_TUNE_MIN_DB} dB up")
                s.confirm(f"{hz} Hz, {db} dB over the band's median; the audio {qlvl:.0f} -> {lvl:.0f} dBFS")
            with o.step(f"{sender} still in its FT8 app on {dial} Hz USB-D") as s:
                app, f_now, m_now = S.state.get("app"), int(S.state.get("freq", 0)), int(S.state.get("mode", -1))
                got["after"] = (app, f_now, m_now)
                if app != "FT8" or f_now != dial or m_now != 3:
                    raise Refused(f"after the tune: app {app}, {f_now} Hz, mode {m_now}")
                s.confirm(f"{app}, {f_now} Hz, mode {m_now}")
            time.sleep(1.5)
            with o.step(f"Ext tune pressed and left: off by its {lim} s limit") as s:
                ab.tap(S, "F5")
                S.wait("on the air with TUNE", lambda: (S.tx and S.state.get("tune")) or watch.why, 3)
                t1 = time.time()
                S.wait("off by the time limit", lambda: (not S.tx and not S.state.get("tune")) or watch.why, lim + 4)
                if watch.why:
                    raise Refused(watch.why)
                d = time.time() - t1
                got["limit_off_s"] = round(d, 1)
                if abs(d - lim) > 2.0:
                    raise Refused(f"off {d:.1f} s after it keyed, the limit {lim} s")
                s.confirm(f"off {d:.1f} s after it keyed (the limit {lim} s)")
        finally:
            S.send("tune 0")
            watch.close()
            try:
                S.wait("off the air", lambda: not S.tx, 5)
            except Refused:
                S.stop()
            o.roles = {A: "RX", B: "RX"}
        if watch.why:
            raise Refused(f"the watch stopped the tune: {watch.why}")
    except Refused as e:
        return False, str(e), got
    finally:
        if rx:
            rx.close()
        o.changing += 1
        try:
            for r in o.r.values():
                bench.close_apps(r)
            o.both_page("G")
            o.both({"freq": dial, "mode": 3})
        finally:
            o.changing -= 1
    return True, (f"Ext tune from the FT8 app: a tone at {got['hz']} Hz in {listener}'s audio ({EXT_TUNE_HZ} set), "
                  f"off {got['stop_s']} s after the second press, the app and dial as they were, off by its "
                  f"{got['limit_s']} s limit after {got['limit_off_s']} s"), got


# ---- G's voiceprint of the microphone on the air (2026-11-24, the owner: "you can do G's voiceprint with the output
# from the other radio stepping tones. they are right next to each other") ----
VP_TONES = [2000, 1700, 1400, 1100, 800, 500]   # acoustic.py's steps, 1 s each, on the speaker radio's own speaker
VP_VOL = 35                 # the speaker radio's volume while it plays them (0 again after: ready())
# The X6100's G (held up) in its landscape screenshot: the voiceprint's box, 0-4 kHz across (0 Hz at the right edge,
# its 1K/2K/3K marks 41 px apart), time down the box (the newest at the top). Measured on its screenshots, 2026-11-24
VP_BOX = (366, 60, 527, 466)    # x0, y0, x1, y1
VP_X0_HZ, VP_PX_PER_KHZ = 525.5, 41.0
VP_RISE = 80                # a step's line this far over the columns beside it (grey levels): the two
                            # pictures of 2026-11-24 03:30 read every step there, CW overs' pictures none
VP_GAP = 4                  # rows of a step's line missing (the hiss's stripes) bridged
VP_MIN_ROWS = 40            # a step seen: an unbroken run of this many rows (a second is ~85 there)


def vp_steps(png):
    """Each VP_TONES step in a voiceprint picture: the longest unbroken run of rows (a gap of VP_GAP rows bridged) where
    its line (its tone's column +-1 px) stands VP_RISE grey levels over the columns 4-7 px either side (a step is a
    narrow line; the passband's hiss is as bright beside it); seen from VP_MIN_ROWS rows, its middle y. In order when
    the seen steps run from the bottom (older) up (newer), as they were played. (steps, seen, ordered)"""
    from PIL import Image
    a = np.asarray(Image.open(png).convert("L"), float)
    x0, y0, x1, y1 = VP_BOX
    st = []
    for hz in VP_TONES:
        xc = int(round(VP_X0_HZ - hz * VP_PX_PER_KHZ / 1000.0))
        line = a[y0:y1, xc - 1:xc + 2].max(axis=1)
        side = np.concatenate([a[y0:y1, xc - 7:xc - 3], a[y0:y1, xc + 4:xc + 8]], axis=1).mean(axis=1)
        lit = (line - side) >= VP_RISE
        best, cur, start, gap = (0, None), 0, None, 0
        for k, on in enumerate(lit):
            if on:
                if start is None:
                    start = k
                cur, gap = k - start + 1, 0
                if cur > best[0]:
                    best = (cur, start + cur // 2)
            elif start is not None:
                gap += 1
                if gap > VP_GAP:
                    start, cur, gap = None, 0, 0
        st.append({"tone": hz, "rows": best[0], "y": y0 + best[1] if best[1] is not None else None})
    seen = [x for x in st if x["rows"] >= VP_MIN_ROWS]
    ordered = len(seen) >= 2 and all(a["y"] > b["y"] for a, b in zip(seen, seen[1:]))
    return st, seen, ordered


def g_voiceprint(o, mic, speaker, label, f=14200000):
    """speaker plays VP_TONES on its own speaker; mic, on G in USB at 0.1 W, keyed with PTT, its mic hearing them. PASS
    when (1) mic's G voiceprint, pictured twice on the air (mid-over and at the end), shows the steps in order, every
    step in one of the pictures, (2) mic's own capture of its over (its monitor, on the processor's capture line)
    holds them in order, (3) the over reaches the speaker radio's receiver (its passband up through 80 % of the steps)"""
    import subprocess
    import acoustic as ac
    M, Sp = o.r[mic], o.r[speaker]
    got = {"tones": VP_TONES}
    wav = os.path.join(o.st.run_dir, "vp_steps.wav")
    play_s = ac.steps_wav(wav, VP_TONES, 1.0)
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", wav, f"root@{Sp.host}:{ac.WAV_ON_RADIO}"], check=True)
    t = {}
    shots = []

    def body(watch):
        with o.step(f"{speaker}'s volume {VP_VOL} for its speaker"):
            bench.ensure(Sp, {"vol": VP_VOL})
        bench.ssh(M.host, "rm -f /tmp/bench_mic.raw; (parecord --server=/var/run/pulse/native -d "
                          "alsa_input.platform-sound.stereo-fallback --raw --format=s16le --channels=1 "
                          "--rate=44100 /tmp/bench_mic.raw >/dev/null 2>&1 &)")
        t["rec0"] = time.time()
        t["q0"] = time.time()
        time.sleep(1.2)
        t["quiet"] = ac.quiet_bins(Sp, t["q0"], time.time())
        with o.step(f"{mic} keyed with PTT on G; {speaker} plays {'-'.join(map(str, VP_TONES))} Hz on its speaker"):
            hold, on_at, t_play, t0 = o.hold(), None, None, time.time()
            while not watch.why and time.time() - t0 < 14.5:
                M.send(f"key PTT 1 {hold}")
                if on_at is None and M.tx:
                    on_at = time.time()
                if on_at and t_play is None and time.time() - on_at > 0.8:
                    Sp.send(f"play {ac.WAV_ON_RADIO}")
                    Sp.wait("the speaker playing", lambda: Sp.state.get("plays") == 1, 4)
                    t_play = time.time()
                if t_play and not shots and time.time() - t_play > ac.LEAD_S + 3.6:
                    shots.append(bench.screenshot(M, f"{label} mid"))
                if t_play and Sp.state.get("plays") == 0:
                    shots.append(bench.screenshot(M, f"{label} end"))
                    break
                if on_at is None and time.time() - t0 > 6:
                    raise Refused(f"{mic} did not come on the air with PTT")
                time.sleep(0.4)
            M.send("key PTT 0")
            Sp.send("play stop")
            t.update(on=on_at, play=t_play, off=time.time())
            if watch.why:
                raise Refused(watch.why)
            if t_play is None:
                raise Refused(f"{speaker}'s speaker never played the steps")
    try:
        try:
            over(o, mic, speaker, f, 2, label, body)
        finally:
            bench.ssh(M.host, "kill $(pidof parecord) 2>/dev/null; true")
            Sp.send("play stop")
            bench.ensure(Sp, {"vol": 0})
        with o.step(f"{mic}'s G voiceprint on the air: the steps in order") as s:
            per = []
            for png in shots:
                if png:
                    per.append(vp_steps(png))
                    got.setdefault("shots", []).append(png)
            if not per:
                raise Refused(f"no picture of {mic}'s screen on the air")
            got["voiceprint"] = [st for st, _, _ in per]
            union = {x["tone"] for _, seen, _ in per for x in seen}
            ordered = all(od or len(seen) < 2 for _, seen, od in per) and any(od for _, _, od in per)
            if union != set(VP_TONES) or not ordered:
                raise Refused(f"in its voiceprint (per picture, each step's unbroken rows and their middle y): "
                              f"{got['voiceprint']}; steps seen {sorted(union)}, in order {ordered}")
            s.confirm(f"all {len(VP_TONES)} steps in order across {len(per)} pictures: {got['voiceprint']}")
        with o.step(f"{mic}'s own capture of its over: the steps in order") as s:
            raw = os.path.join(o.st.run_dir, f"vp_{mic}_capture.raw")
            subprocess.run(["scp", "-q", f"root@{M.host}:/tmp/bench_mic.raw", raw], capture_output=True)
            x = np.fromfile(raw, dtype=np.int16).astype(float) if os.path.exists(raw) else np.zeros(0)
            msteps, mheard, mord = ac.staircase(ac.audio_points(x, VP_TONES), VP_TONES, 40)
            got["capture"] = msteps
            if not (mheard and mord):
                raise Refused(f"its capture: {msteps}")
            s.confirm(", ".join(f"{m['tone']} Hz {m['frames']}" for m in msteps))
        with o.step(f"the over on {speaker}'s receiver through the steps") as s:
            present = []
            for tt, cen, wid, v in list(Sp.r.spectra):
                if not (t["play"] + 0.4 <= tt <= t["play"] + play_s - ac.TAIL_S - 0.2) or not v:
                    continue
                per_ = wid / len(v)
                a_, b_ = int((f + 300 - (cen - wid / 2)) / per_), int((f + 2700 - (cen - wid / 2)) / per_)
                q = t["quiet"].get((cen, wid, len(v)))
                if q is not None and b_ > a_:
                    present.append(float(np.max(np.array(v[a_:b_ + 1], float) - q[a_:b_ + 1])) >= ac.RISE_DB)
            share = sum(present) / len(present) if present else 0.0
            got["arrived_share"] = round(share, 2)
            if share < ac.PRESENT_SHARE:
                raise Refused(f"{speaker}'s passband up {ac.RISE_DB} dB through {share:.0%} of the steps' frames")
            s.confirm(f"up through {share:.0%} of the steps' frames")
    except Refused as e:
        return False, str(e), got
    return True, (f"{mic}'s G voiceprint drew {speaker}'s {len(VP_TONES)} steps in order on the air "
                  f"({len(got.get('shots', []))} pictures), its capture held them, {speaker} heard the over through "
                  f"{got['arrived_share']:.0%} of them"), got


COMP_MIC_WAV = "/tmp/bench_comp_steps.wav"


def comp_mic_wav(path):
    """1000 Hz loud (0.5) and 20 dB softer taking turns every suite_air_a.COMP_SEG_S, COMP_PAIRS times, 44.1 kHz, the
    tone's phase kept across the steps, acoustic.py's lead and tail: its seconds"""
    import wave
    import acoustic as ac
    import suite_air_a as aa
    fs = ac.FS
    n = int(round(aa.COMP_SEG_S * fs))
    k = np.arange(2 * aa.COMP_PAIRS * n)
    amp = np.where((k // n) % 2 == 0, 0.5, 0.05)
    x = np.concatenate([np.zeros(int(ac.LEAD_S * fs)), amp * np.sin(2 * np.pi * 1000 * k / fs),
                        np.zeros(int(ac.TAIL_S * fs))])
    r = int(0.02 * fs)
    a = int(ac.LEAD_S * fs)
    x[a:a + r] *= np.linspace(0, 1, r)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    return len(x) / fs


def comp_mic_test(o, mic, speaker, f=14200000, vol=VP_VOL):
    """Comp through the mic (settings-audio-compressor, tx-comp-ratio): the speaker radio plays a 1000 Hz tone taking
    turns loud and 20 dB softer every 0.15 s on its own speaker; the mic radio, keyed with PTT on G in USB at 0.1 W,
    its mic hearing them, at Comp 1:1 and then 4:1; its own capture of each over (its monitor on the processor's
    capture line, before the RF and its power loop) read loud against soft (suite_air_a.two_levels, the steps' phase
    from the whole over). Through TCI Comp could not be seen (its audio goes in as the modem's, 2026-11-25). Passes
    with the range at 4:1 at least 3 dB under 1:1's, the path through the room carrying 12 dB of the 20 at least at
    1:1; on Xiegu's factory X6100 base, no compression is said (Comp came with the patched base, MANUAL), not failed"""
    import subprocess
    import acoustic as ac
    import suite_air_a as aa
    from scipy.signal import resample_poly
    M, Sp = o.r[mic], o.r[speaker]
    wav = os.path.join(o.st.run_dir, "comp_steps.wav")
    os.makedirs(o.st.run_dir, exist_ok=True)
    play_s = comp_mic_wav(wav)
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", wav, f"root@{Sp.host}:{COMP_MIC_WAV}"])
    comp0 = M.state.get("comp")
    got = {"comp_as_found": comp0}
    levels = {}

    def one(comp):
        cap = {}

        def body(watch):
            with o.step(f"{mic}'s Comp {comp}:1, {speaker}'s volume {vol}") as s:
                bench.ensure(M, {"comp": comp})
                bench.ensure(Sp, {"vol": vol})
                s.confirm(f"comp {M.state.get('comp')}")
            bench.ssh(M.host, "rm -f /tmp/bench_mic.raw; (parecord --server=/var/run/pulse/native -d "
                              "alsa_input.platform-sound.stereo-fallback --raw --format=s16le --channels=1 "
                              "--rate=44100 /tmp/bench_mic.raw >/dev/null 2>&1 &)")
            with o.step(f"{mic} keyed with PTT on G; {speaker} plays the loud and soft steps on its speaker"):
                hold, on_at, t_play, t0 = o.hold(), None, None, time.time()
                while not watch.why and time.time() - t0 < play_s + 6:
                    M.send(f"key PTT 1 {hold}")
                    if on_at is None and M.tx:
                        on_at = time.time()
                    if on_at and t_play is None and time.time() - on_at > 0.8:
                        Sp.send(f"play {COMP_MIC_WAV}")
                        Sp.wait("the speaker playing", lambda: Sp.state.get("plays") == 1, 4)
                        t_play = time.time()
                    if t_play and Sp.state.get("plays") == 0:
                        break
                    if on_at is None and time.time() - t0 > 6:
                        raise Refused(f"{mic} did not come on the air with PTT")
                    time.sleep(0.4)
                M.send("key PTT 0")
                Sp.send("play stop")
                if watch.why:
                    raise Refused(watch.why)
                if t_play is None:
                    raise Refused(f"{speaker}'s speaker never played the steps")
        try:
            over(o, mic, speaker, f, 2, f"Comp {comp}:1 through {mic}'s mic", body)
        finally:
            bench.ssh(M.host, "kill $(pidof parecord) 2>/dev/null; true")
            Sp.send("play stop")
        raw = os.path.join(o.st.run_dir, f"comp_{mic}_{comp}.raw")
        subprocess.run(["scp", "-4", "-q", f"root@{M.host}:/tmp/bench_mic.raw", raw], capture_output=True)
        x = np.fromfile(raw, dtype=np.int16).astype(float) / 32768 if os.path.exists(raw) else np.zeros(0)
        cap["levels"] = aa.two_levels(resample_poly(x, 80, 147)) if len(x) > ac.FS else (None, None)
        return cap["levels"]
    try:
        for comp in (1, 4):
            with o.step(f"Comp {comp}:1: the over, {mic}'s capture read loud against soft") as s:
                levels[comp] = one(comp)
                lo = levels[comp]
                if None in lo:
                    raise Refused(f"the steps not found in {mic}'s capture at Comp {comp}:1")
                s.confirm(f"loud {lo[0]:.1f}, soft {lo[1]:.1f} dBFS: {lo[0] - lo[1]:.1f} dB apart")
    except Refused as e:
        return False, str(e), got
    finally:
        try:
            bench.ensure(M, {"comp": comp0} if comp0 is not None else {"comp": 1})
            bench.ensure(Sp, {"vol": 0})
        except Refused as e:
            o.st.ev(f"{mic}'s Comp or {speaker}'s volume not put back: {e}", "bad")
    r1, r4 = levels[1][0] - levels[1][1], levels[4][0] - levels[4][1]
    got.update(range_1=round(r1, 1), range_4=round(r4, 1), levels={k: [round(v, 1) for v in lv] for k, lv in levels.items()})
    says = (f"{mic}'s capture of its over, {speaker}'s steps 20 dB apart through the room ({speaker}'s volume {vol}): at 1:1 {r1:.1f} dB apart, at "
            f"4:1 {r4:.1f} (loud {levels[1][0]:.1f} -> {levels[4][0]:.1f} dBFS)")
    if r1 < 12:
        return False, says + ": the room carried too little of the 20 dB at 1:1 to tell", got
    if r4 <= r1 - 3:
        return True, says + ": compressed", got
    base = aa.base_of(M.host) if mic == "x6100" else ""
    if mic == "x6100" and aa.gcfix_of(base) == 0:
        return None, says + f": no compression on Xiegu's {base or 'factory'} base (Comp came with the patched base; said)", got
    return False, says + ": NOT compressed at 4:1", got


# ---- The Sentry's pages on both, as pictures (2026-11-24: the old suite's page walk, t_pages, into the orchestrator;
# the owner: "all tests should ultimately use the same orchestration framework") ----
PAGES_LOOK = "ABCDEFGH"


def pages_look(o):
    """Each Sentry page, A to H (H: AIRSPACE), opened on both radios together and photographed together, receive only.
    Each picture's colours counted against the Sentry's gold on black (suite.palette): more than OFF_PALETTE_FLAG % of
    cyan, blue or purple is an old theme's look left behind, a FAIL. The share of the screen that differs between the
    two radios' pictures (suite.picture_diff) over DIFF_SHARE is shown to look at (a LOOK): the model's name, the
    clock, live spectra and meters differ always. A page tunes the radio its own way (G its channel, D and E their
    lists): the walk is one change for the sync guard, any page where the two dials differ said, both put back on one
    frequency and mode at the end, on page A"""
    import suite
    f0, m0 = int(o.r[A].state.get("freq", 0)), int(o.r[A].state.get("mode", 2))
    res, bad, look = {}, [], []
    o.changing += 1
    try:
        for p in PAGES_LOOK:
            with o.step(f"both on the Sentry's page {p}, photographed together") as s:
                if p == "H":
                    for r in o.r.values():
                        r.send("open sentry H")
                    for r in o.r.values():
                        r.wait("AIRSPACE (page H)", lambda r=r: r.state.get("win") == "Airspace", 10)
                else:
                    th = [threading.Thread(target=bench.open_page, args=(r, p)) for r in o.r.values()]
                    [t.start() for t in th]
                    [t.join(25) for t in th]
                    off_page = [n for n, r in o.r.items() if r.state.get("pg") != p]
                    if off_page:
                        raise Refused(f"{', '.join(off_page)} not on page {p}")
                time.sleep(1.5)                     # drawn once more after it opened
                paths = bench.screenshots_both(list(o.r.values()), f"pages-look {p}")
                if not all(paths.get(n) for n in o.r):
                    raise Refused(f"page {p}: a picture missing ({paths})")
                pal = {n: suite.palette(path) for n, path in paths.items()}
                d = suite.picture_diff([paths[A], paths[B]])
                dials = {n: (int(r.state.get("freq", 0)), int(r.state.get("mode", -1))) for n, r in o.r.items()}
                res[p] = {"off_pct": {n: v["off"] for n, v in pal.items()}, "differs": None if d is None else round(d, 3),
                          "dials": dials, "pictures": {n: os.path.basename(v) for n, v in paths.items()}}
                old = [n for n, v in pal.items() if v["off"] > suite.OFF_PALETTE_FLAG]
                if old:
                    bad.append(f"{p}: {old} {suite.OFF_PALETTE_FLAG}%+ cyan/blue/purple ({res[p]['off_pct']})")
                if d is not None and d > suite.DIFF_SHARE:
                    look.append(f"{p}: {d:.0%} differs")
                if bad or (d is not None and d > suite.DIFF_SHARE):
                    with o.st.lock:
                        if o.st.cur is not None:
                            o.st.tests[o.st.cur]["shots"].append({"t": time.time(), "label": f"page {p}",
                                                                  "paths": paths})
                s.confirm(f"off-palette {res[p]['off_pct']} %, {d:.0%} of the screen differs, dials {dials}"
                          + ("" if dials[A] == dials[B] else " (the page tunes each its own way)"))
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            o.both_page("A")
            if f0:
                o.both({"freq": f0, "mode": m0})
        finally:
            o.changing -= 1
    if bad:
        return False, "an old theme's colours: " + "; ".join(bad), res
    if look:
        return None, f"every page in the Sentry's look on both; to look at: {'; '.join(look)}", res
    return True, (f"pages {PAGES_LOOK} in the Sentry's look on both (cyan/blue/purple at most "
                  f"{max(max(v['off_pct'].values()) for v in res.values()):.1f} %), each pair alike within "
                  f"{suite.DIFF_SHARE:.0%}"), res

def ft8_qso(o, a, b, label, dial=14074000):
    """A whole FT8 QSO by the apps themselves: Auto on both, a calls CQ, b answers it once ("ft8 reply"), the reports,
    RR73 and 73 by the apps; both must log the QSO (their state's "logged" the other's test call) and each ADIF file
    hold exactly one record of it. The test's records taken out of both logs after (bench.clean_log)"""
    import suite_air_b as ab
    A_, B_ = o.r[a], o.r[b]
    ca, cb = TEST_CALLS[a], TEST_CALLS[b]
    res = {}
    p, total = bench.chain_gate(o.c, "radios")
    try:
        with o.step("both on their test calls, the FT8 app open on both on the dial, Auto on both"):
            ft8_both(o, dial)
            for r in (A_, B_):
                bench.radio_tx_gate(r, o.c, W)
                bench.listener_gate(r)
                if not r.ft8.get("auto"):
                    r.send("ft8 auto")
                    r.wait("Auto on", lambda r=r: r.ft8.get("auto"), 5)
        o.roles = {a: "TX", b: "TX"}            # they take turns: the RF watchdog leaves them be
        watch = bench.Watch([A_, B_], o.c, w=W, label=label)
        t0 = time.time()
        try:
            with o.step(f"{a} calls CQ, {b} decodes it and answers once") as s:
                nb = len(B_.rows)
                bench.tx_log({"who": a, "f": dial, "w": A_.pwr(), "what": f"orch: {label} CQ"})
                A_.send("ft8 cq")
                B_.wait(f"{a}'s CQ", lambda: watch.why or any(ab.calls_are(r, "CQ", ca) for r in B_.rows[nb:]), 70)
                if watch.why:
                    raise Refused(watch.why)
                cq = next(r for r in B_.rows[nb:] if ab.calls_are(r, "CQ", ca))
                bench.tx_log({"who": b, "f": dial, "w": B_.pwr(), "what": f"orch: {label} reply"})
                B_.send(f"ft8 reply {cq['id']}")
                s.confirm(f"{mask(bench.msg(cq))!r} at {cq.get('snr')} dB, answered")
            with o.step("the apps finish the QSO by themselves: both log it") as s:
                def logged(r, call):
                    age = r.ft8.get("logged_s")
                    return r.ft8.get("logged") == call and age is not None and 0 <= age <= time.time() - t0
                B_.wait("both logged", lambda: watch.why or (logged(A_, cb) and logged(B_, ca)), 170)
                if watch.why:
                    raise Refused(watch.why)
                s.confirm(f"{a} logged {cb}, {b} logged {ca}, {time.time() - t0:.0f} s from the CQ")
        finally:
            for r in (A_, B_):
                r.send("ft8 stop")
            t1 = time.time()
            while time.time() - t1 < 20 and (A_.tx or B_.tx):
                time.sleep(0.2)
            watch.close()
            for r in (A_, B_):
                if r.ft8.get("auto"):
                    r.send("ft8 auto")
            o.roles = {A: "RX", B: "RX"}
        with o.step("each ADIF holds exactly one record of the QSO") as s:
            n_a, _ = ab.ft8_logged(A_.host, cb, t0 - 10)
            n_b, _ = ab.ft8_logged(B_.host, ca, t0 - 10)
            res.update(records={a: n_a, b: n_b}, secs=round(time.time() - t0))
            if (n_a, n_b) != (1, 1):
                raise Refused(f"records of the QSO: {a} {n_a}, {b} {n_b} (one each wanted)")
            s.confirm(f"{a} 1, {b} 1")
    except Refused as e:
        return False, str(e), res
    finally:
        for r in (A_, B_):
            r.send("ft8 close")
        o.apps_closed(dial, 3)
        for r in (A_, B_):
            try:
                bench.clean_log(r.host)             # the test calls' records out of the logs
            except (Refused, OSError) as e:
                o.st.ev(f"{r.name}: the test QSO not taken out of its log: {e}", "bad")
    return True, f"the apps made the QSO ({res['secs']} s): both logged it, one ADIF record each", res


# ---- The tests ----

# ---- Ported from the 1:1 suite (the owner, 2026-11-24: "all tests should ultimately use the same orchestration
# framework", "whittle down the tests to essential testable things"): suite_more.py's receiver, control and safety
# batches, as tests of the two radios at once ----

F_TEST = 14200000
F_CW = 14040000


def t_rx_vfo_split(o):
    """VFO B and back, split on and off, RIT and XIT at +200, -200 and 0: each set on both at once, read back alike
    (suite_more set-vfo, set-split, set-rit, set-xit); all four off and VFO A after"""
    seen = []
    try:
        with o.step("both on 14.200 USB, VFO A, no split, RIT or XIT"):
            o.both({"vfo": 0, "split": 0, "freq": F_TEST, "mode": 2, "rit": 0, "xit": 0})
        with o.step("VFO B on both, set to 14.200 USB alike, then VFO A again") as s:
            o.both({"vfo": 1, "freq": F_TEST, "mode": 2})
            o.both({"vfo": 0, "freq": F_TEST, "mode": 2})
            s.confirm("VFO B and A each read back alike")
            seen.append("vfo B/A")
        for key, vals in (("split", [1, 0]), ("rit", [200, -200, 0]), ("xit", [200, -200, 0])):
            with o.step(f"{key} " + ", ".join(str(v) for v in vals) + " on both, each read back alike") as s:
                for v in vals:
                    o.both({key: v})
                s.confirm(f"{key} read back alike at each")
                seen.append(f"{key} " + "/".join(str(v) for v in vals))
    except Refused as e:
        return False, str(e), {"seen": seen}
    finally:
        try:
            o.both({"vfo": 0, "split": 0, "rit": 0, "xit": 0, "freq": F_TEST, "mode": 2})
        except Refused as e:
            o.st.ev(f"VFO A, split, RIT, XIT not put back alike: {e}", "bad")
    return True, "set on both, read back alike: " + "; ".join(seen), {"seen": seen}


def tci_open(host):
    """A TCI session from this computer (a client on the network): (session, its greeting up to ready)"""
    from remote_ws import Remote
    t = Remote(host, 50001)
    greet, end = [], time.time() + 6
    while time.time() < end:
        m = t.recv(timeout=0.5)
        if m is None:
            continue
        greet.append(m)
        if m.startswith("ready"):
            break
    return t, greet


def tci_wait(t, prefix, secs=3.0):
    end = time.time() + secs
    while time.time() < end:
        m = t.recv(timeout=0.3)
        if m and m.lower().startswith(prefix):
            return m
    return None


def t_tci_receiver(o):
    """TCI's receiver commands on both radios' own TCI at once, each confirmed by the radio's state (suite_more
    tci-receiver): the filter, AGC fast and slow, RIT on and off, a VFO in capitals (TCI is case-insensitive), an
    unknown command ignored and the session going on; the two alike after; the filter and AGC put back"""
    before = {k: bench._value(o.r[A], k) for k in ("filter", "agc")}
    cmds = (("rx_filter_band:0,300,2400;", lambda r: (r.state.get("flo"), r.state.get("fhi")) == (300, 2400),
             "filter 300-2400"),
            ("agc_mode:0,fast;", lambda r: r.state.get("agc") == 2, "AGC fast"),
            ("agc_mode:0,slow;", lambda r: r.state.get("agc") == 1, "AGC slow"),
            ("rit_offset:0,200;", lambda r: r.state.get("rit") == 200, "RIT +200"),
            ("rit_enable:0,false;", lambda r: r.state.get("rit") == 0, "RIT off"),
            ("VFO:0,0,14100000;", lambda r: int(r.state.get("freq", 0)) == 14100000, "VFO in capitals"))
    got = {}
    try:
        with o.step("both on 14.200 USB, RIT 0"):
            o.both({"freq": F_TEST, "mode": 2, "rit": 0})
        with o.step("each radio's TCI: " + ", ".join(w for _, _, w in cmds) + ", then an unknown command") as s:
            def one(n, r):
                t, greet = tci_open(r.host)
                said = []
                try:
                    if not any(g.startswith("ready") for g in greet):
                        raise Refused(f"no TCI greeting ending in ready ({greet[:3]})")
                    for cmd, cond, what in cmds:
                        t.send(cmd)
                        r.wait(what, lambda: cond(r), 5)
                        said.append(what)
                    t.send("no_such_command:0,1;")
                    t.send("vfo:0,0;")
                    back = tci_wait(t, "vfo:0,0,")
                    if not back:
                        raise Refused(f"{', '.join(said)}; after an unknown command the session answered nothing")
                    return ", ".join(said) + f"; an unknown command ignored ({back})"
                finally:
                    t.close()
            got = in_parallel(o, one, 60)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
        with o.step("the two alike after (14.100, AGC slow, RIT 0, filter 300-2400)") as s:
            time.sleep(1.0)
            alike(o, ["freq", "mode", "agc", "rit", "filter"])
            s.confirm("alike")
    except Refused as e:
        return False, str(e), got
    finally:
        try:
            o.both({"freq": F_TEST, "mode": 2, "rit": 0, **{k: v for k, v in before.items() if v is not None}})
        except Refused as e:
            o.st.ev(f"filter/AGC not put back: {e}", "bad")
    return True, "TCI's receiver commands taken on both: " + got.get(A, ""), got


def rig_ask(host, lines, secs=4.0):
    import socket
    s = socket.create_connection((host, 4532), timeout=secs)
    out = []
    try:
        for ln in lines:
            s.sendall((ln + "\n").encode())
            time.sleep(0.4)
            s.settimeout(1.5)
            try:
                out.append(s.recv(4096).decode(errors="replace").strip())
            except socket.timeout:
                out.append("")
    finally:
        s.close()
    return out


def t_rigctl_more(o):
    """Hamlib's rigctl on both (suite_more rigctl-more): the levels answered (STRENGTH a number), split asked and its
    setting refused (receive only), the state dump; nothing keyed"""
    try:
        with o.step("both on 14.200 USB"):
            o.both({"freq": F_TEST, "mode": 2})
        with o.step("l STRENGTH, l PREAMP, l AF, s, S 1 VFOB, \\dump_state on each") as s:
            def one(n, r):
                out = rig_ask(r.host, ["l STRENGTH", "l PREAMP", "l AF", "s", "S 1 VFOB", "\\dump_state"])
                strength = out[0].lstrip("-").isdigit()
                split_refused = out[3].split()[:1] == ["0"] and out[4].startswith("RPRT -")
                dump = out[5][:1] in ("0", "1")
                said = (f"STRENGTH {out[0]!r}, PREAMP {out[1]!r}, AF {out[2]!r}; s {out[3]!r}, S 1 VFOB {out[4]!r}; "
                        f"dump_state {out[5][:20]!r}")
                if not (strength and split_refused and dump) or r.tx:
                    raise Refused(said)
                return said
            got = in_parallel(o, one, 40)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    except Refused as e:
        return False, str(e), {}
    return True, "rigctl's levels, split (refused) and state dump on both", got


def t_remote_apps(o):
    """The remote's apps list on both (suite_more remote-apps): more than ten, the same on both; opening WSPR (it
    transmits by opening) refused on both"""
    got = {}
    try:
        with o.step("each radio's apps list, the two the same") as s:
            def lst(n, r):
                r.apps = None
                r.send("apps")
                r.wait("the apps list", lambda: r.apps is not None, 5)
                return [a[1] for a in r.apps]
            got = in_parallel(o, lst)
            if len(got[A]) <= 10 or sorted(got[A]) != sorted(got[B]):
                raise Refused(f"{A} {len(got[A])}, {B} {len(got[B])}: " + ", ".join(
                    sorted(set(got[A]) ^ set(got[B]))[:8]))
            s.confirm(f"{len(got[A])} apps on each, the same")
        with o.step("opening WSPR refused on both (it would transmit by opening)") as s:
            def wspr(n, r):
                w = next((a for a in r.apps if "WSPR" in a[1].upper()), None)
                if not w:
                    return "no WSPR in its list"
                n0 = len(r.errs)
                r.send(f"open {w[0]}")
                time.sleep(1.5)
                if r.state.get("app") == "WSPR" or not any("open" in e for e in r.errs[n0:]):
                    raise Refused(f"WSPR opened ({r.errs[n0:]})")
                return r.errs[-1]
            said = in_parallel(o, wspr)
            s.confirm("; ".join(f"{n}: {v}" for n, v in said.items()))
    except Refused as e:
        return False, str(e), {"apps": got}
    return True, f"{len(got[A])} apps alike on both, WSPR's opening refused on both", {"apps": got}


def t_power_cap(o):
    """20 W asked of both (suite_more power-cap): each setting reads its radio's own top (the X6100 10 W, the X6200
    8 W; 5 W on its battery alone, radio.c supply_cb); nothing keyed; 0.1 W again after"""
    got = {}
    try:
        with o.step("20 W asked of both: each reads its own top") as s:
            def one(n, r):
                sup = bench.listener_gate_read(r)
                on_bat = sup is not None and sup["dc_in"] < 5.0
                top = 5.0 if on_bat else {"x6200": 8.0, "x6100": 10.0}[n]
                r.send("set pwr 200")
                time.sleep(1.5)
                pw = r.pwr()
                if abs(pw - top) > 0.05 or r.tx:
                    raise Refused(f"20 W asked: {pw} W (its top {top} W{', on its battery' if on_bat else ''})")
                return {"reads_w": pw, "top_w": top, "on_battery": on_bat}
            got = in_parallel(o, one)
            s.confirm("; ".join(f"{n} {v['reads_w']} W (top {v['top_w']})" for n, v in got.items()))
    except Refused as e:
        return False, str(e), got
    finally:
        o.both({"pwr": W})
    return True, "; ".join(f"{n}: 20 W asked, {v['reads_w']} W, its top" for n, v in got.items()), got


def t_smc_remote(o):
    """TX Cal > S-METER's Measure from the remote (remote_server.c "smc"), nothing on the air: "smc?" on both reads the
    figures in use; Measure on the noise refuses ("feed one ...": nothing changed, the figures as before, PRE, ATT and
    RF gain put back); "smc put" sets PRE a dB off, then the figures found, each read back. The measuring itself: air
    smeter-front-end"""
    import suite_air_a
    got = {}
    try:
        with o.step("smc? on both; Measure on the noise refused; the figures, PRE, ATT and RF gain as before") as s:
            def one(n, r):
                fe = {k: r.state.get(k) for k in ("pre", "att", "rfg")}
                a = suite_air_a.smc_ask(r)
                if a["run"]:
                    raise Refused(f"{n}: a Measure already running ({a['note']!r})")
                r.send("smc start")
                t0, b = time.time(), a
                while time.time() - t0 < 8.0:
                    time.sleep(0.5)
                    b = suite_air_a.smc_ask(r, 2.0)
                    if not b["run"] and time.time() - t0 > 1.0:
                        break
                if b["run"]:
                    r.send("smc stop")
                    raise Refused(f"{n}: Measure on the noise still running after 8 s ({b['note']!r})")
                time.sleep(0.5)
                fe2 = {k: r.state.get(k) for k in ("pre", "att", "rfg")}
                moved = (a["fig"][0] + (1 if a["fig"][0] < 40 else -1),) + tuple(a["fig"][1:])
                put = suite_air_a.smc_put(r, moved)       # put changes one: PRE a dB, read back, then as found
                put = (f"PRE a dB off: {put}; " if put else "") + (suite_air_a.smc_put(r, a["fig"]) or "")
                if not b["note"].endswith("Nothing changed.") or b["fig"] != a["fig"] or fe2 != fe or put:
                    raise Refused(f"{n}: Measure on the noise: {b['note']!r}, the figures {a['fig']} -> {b['fig']}, "
                                  f"PRE/ATT/RF gain {fe} -> {fe2}; put again: {put or 'as found'}")
                return {"fig": a["fig"], "note": b["note"], "secs": round(time.time() - t0, 1)}
            got = in_parallel(o, one, 30)
            s.confirm("; ".join(f"{n} {v['fig']}: {v['note']!r}" for n, v in got.items()))
    except Refused as e:
        return False, str(e), got
    return True, "; ".join(f"{n}: figures (PRE, ATT, RF gain) {v['fig']}, Measure on the noise: {v['note']!r}"
                           for n, v in got.items()), got


WEB_SKIP = {"/api/backup/download", "/api/gallery/zip", "/api/update/check", "/api/flights", "/api/meteo",
            "/api/boot_picture/png", "/files/"}      # big downloads, the internet, binaries (suite_more's)


def web_routes():
    import re
    seen = set()
    for root, _, files in os.walk(os.path.join(TOP, "webserver", "src")):
        for f in files:
            if f.endswith(".py"):
                for m in re.finditer(r"@(?:app|[a-z_]+)\.(?:route|get)\(['\"]([^'\"]+)['\"]",
                                     open(os.path.join(root, f)).read()):
                    if "<" not in m.group(1):
                        seen.add(m.group(1))
    return sorted(seen - WEB_SKIP)


def t_web_pages(o):
    """Every page and GET API of the web configurator on both radios (suite_more's web sweep): each answers the same
    on the two, and none with a server error (5xx) or not at all"""
    import socket
    import urllib.error
    import urllib.request
    routes = web_routes()
    codes = {}
    with o.step(f"{len(routes)} pages and APIs asked of both") as s:
        def one(n, r):
            ip = socket.gethostbyname(r.host)
            got = {}
            for path in routes:
                try:
                    req = urllib.request.Request(f"http://{ip}{path}", headers={"Host": r.host})
                    with urllib.request.urlopen(req, timeout=8) as resp:
                        got[path] = resp.status
                except urllib.error.HTTPError as e:
                    got[path] = e.code
                except (urllib.error.URLError, OSError):
                    got[path] = "none"
            return got
        try:
            codes = in_parallel(o, one, 240)
        except Refused as e:
            return False, str(e), {}
        s.confirm(f"{sum(1 for p in routes if codes[A][p] == 200)}/{len(routes)} answered 200 on {A}, "
                  f"{sum(1 for p in routes if codes[B][p] == 200)} on {B}")
    differ = [f"{p} {codes[A][p]}/{codes[B][p]}" for p in routes if codes[A][p] != codes[B][p]]
    broken = [f"{p} {codes[A][p]}/{codes[B][p]}" for p in routes
              if any(c == "none" or (isinstance(c, int) and c >= 500) for c in (codes[A][p], codes[B][p]))]
    other = sorted({f"{p} {codes[A][p]}" for p in routes if codes[A][p] != 200 and f"{p} {codes[A][p]}/{codes[B][p]}"
                    not in differ + broken})
    with o.step("the two alike, none broken") as s:
        if differ or broken:
            return False, "; ".join((["differ: " + ", ".join(differ)] if differ else []) +
                                    (["broken: " + ", ".join(broken)] if broken else [])), codes
        s.confirm(f"all alike; not 200 on both (by design): {', '.join(other) or 'none'}")
    return True, f"{len(routes)} pages and APIs answered alike on both" + (f"; not 200 by design: {', '.join(other)}"
                                                                           if other else ""), codes


def tci_refused(o, radio, other, kind, label):
    """A TCI transmission that must be refused, the radio armed, watched 4 s off the air (suite_more's safety batch):
    out-of-band (a program on the radio keying audio on 15.000 MHz), ask (a client on the network keying: asked on the
    radio's screen, not keyed; the question gone ~20 s after the client leaves), cw-usb (CW text through TCI in USB),
    key-cw (an audio key in CW). The other radio, on the same frequency, must hear nothing"""
    import subprocess
    R, X = o.r[radio], o.r[other]
    f, mode = {"out-of-band": (15000000, 3), "ask": (F_TEST, 3), "cw-usb": (F_CW, 2), "key-cw": (F_CW, 4)}[kind]
    went, said, t = False, "", None
    with o.step(f"both on {f / 1e6:.3f} MHz mode {mode}, {W} W, alike"):
        o.both_page("G")
        o.both({"freq": f, "mode": mode, "pwr": W})
    if kind != "out-of-band":                       # a failure would key it: the bench's gates as for an over
        bench.chain_gate(o.c, "radios")
        bench.radio_tx_gate(R, o.c, W)
    with o.step(f"{radio}'s transmitter armed (the Sentry's A/B)"):
        o.changing += 1
        try:
            bench.arm(R, True, mode=mode)
        finally:
            o.changing -= 1
    try:
        quiet = X.meter
        with o.step(f"{kind}: {radio} must stay off the air 4 s") as s:
            if kind in ("out-of-band", "key-cw"):
                run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
                        f"root@{R.host}:/tmp/tci_tone_tx.py"])
                subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{R.host}",
                                  f"python3 /tmp/tci_tone_tx.py '[[{700 if kind == 'key-cw' else 1000}, 2.0]]' 0.3 3.0 "
                                  f"50001 0.05"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            elif kind == "cw-usb":
                run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_cw_tx.py"),
                        f"root@{R.host}:/tmp/tci_cw_tx.py"])
                subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{R.host}",
                                  "python3 /tmp/tci_cw_tx.py 'Q9TST' 3"], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            else:
                t, _ = tci_open(R.host)
                t.send("trx:0,true;")
            t0 = time.time()
            while time.time() - t0 < 4.0:
                if R.tx:
                    went = True
                    break
                time.sleep(0.05)
            asked = R.state.get("ask") == 1
            if went:
                raise Refused(f"{radio} WENT ON THE AIR")
            if kind == "ask" and not asked:
                raise Refused("not asked on the radio's screen")
            if kind == "cw-usb" and asked:
                raise Refused("a program on the radio was asked about (only a network client is)")
            said = f"off the air{', asked on its screen' if asked else ''}; {other}'s S-meter {quiet} -> {X.meter} dBm"
            s.confirm(said)
        if kind == "ask":
            with o.step("the client gone: the question gone by itself within 24 s (its 20 s), nothing keyed") as s:
                t.close()
                t = None
                t0 = time.time()
                while time.time() - t0 < 24 and R.state.get("ask") == 1:
                    if R.tx:
                        raise Refused(f"{radio} WENT ON THE AIR")
                    time.sleep(0.5)
                if R.state.get("ask") == 1:
                    raise Refused("the question still up 24 s after the client left")
                s.confirm(f"gone after {time.time() - t0:.1f} s")
    except Refused as e:
        return False, str(e), {}
    finally:
        if t is not None:
            t.close()
        R.send("key PTT 0")
        bench.stop_bench_programs(R)
        time.sleep(1.0)
        R.stop()
        try:
            if not R.ctl_mine:
                R.take_control()            # our own TCI program had it; never the operator's (take_control refuses)
            bench.arm(R, False)
        except Refused as e:
            o.st.ev(f"{radio}: {e}", "bad")
    return True, f"{kind} on {radio}: {said}", {}


# ---- The 1:1 suite's own checks run through orch (the owner: every test through the orchestrator): a batch's t_*
# registers each check with per_radio(run, batch, test, fn(r), covers, want_tx) or by_radio(run, batch, test, covers,
# fn(name, host, ip)); those are collected here without running, and each chosen one runs on both radios at once,
# both put back alike after ----

_SUITE = {}
SUITE_FEATURES = {}     # (module, t_name, check): (its batch in the suite's reports, the features.json ids it covers)

# (its t_ function, the check, what passes): the essential ones not already orch's own (control-tci, freq-lock,
# apps-list and files-secrets are: tci-takes-control, dial-lock, remote-apps, web-secrets)
SUITE_PORTED = [
    ("t_tci_proto", "tci-answers", "dds, start/stop and TCI's fixed answers as the code has them, the unsupported CW "
                                   "commands left unanswered"),
    ("t_tci_proto", "tci-cw-speed", "the KEY speed asked (control kept), set (both names echoed, control to TCI), "
                                    "99 clamped to the radio's top, put back"),
    ("t_tci_proto", "tci-rx-sensors", "rx_smeter ten a second always, rx_channel_sensors only while enabled"),
    ("t_tci_proto", "tci-audio", "the greeting's audio defaults, 24k int16 mono 512 set and streamed, none after stop"),
    ("t_tci_drive", "tci-drive", "drive read and set lower: the power follows, nothing keyed"),
    ("t_tci_guards", "tci-four", "four TCI clients at most, the fifth refused"),
    ("t_tci_guards", "tci-origin", "a page from another site, a rebound name and Pat's mailbox refused; the radio's own "
                                   "page and a program without Origin taken"),
    ("t_panel", "panel-greeting", "the remote's hello, keys list and token"),
    ("t_panel", "panel-bands-plan", "the bands plan as the remote sends it"),
    ("t_panel", "panel-view-only", "a view-only session sends nothing"),
    ("t_panel", "panel-ping-clock", "the link's ping and the clock's answer"),
    ("t_panel", "panel-meter", "the S-meter and the battery in the remote's state"),
    ("t_panel", "panel-kt-cwrise", "the CW rise time read"),
    ("t_panel", "panel-filter-am", "AM/FM filters as the panel sets them"),
    ("t_panel", "panel-refused-set", "a refused setting reverted on the panel"),
    ("t_panel", "panel-play-guard", "a file play refused where it must be"),
    ("t_panel", "panel-wide", "the wide spectrum as the remote sends it"),
    ("t_control_owner", "control-needed", "a setting without control refused"),
    ("t_control_owner", "control-lost-free", "control free again when its holder leaves"),
    ("t_screen", "screen-stream", "the screen's JPEG stream and its quality"),
    ("t_screen", "screen-viewers", "the most screen viewers at once"),
    ("t_screen", "remote-origin", "the remote's WebSocket refuses other sites, takes its own names"),
    ("t_core", "band-cycle", "the band keys through every band, 60 m included, the untunable skipped"),
    ("t_core", "broadcast-am", "the broadcast bands in AM"),
    ("t_core", "tci-refused-answer", "a TCI tune the radio refuses (an app's hold) answered with what it kept"),
    ("t_core", "knobs", "the knobs through the remote tune the radio"),
    ("t_core", "wide-zoom", "the X6200's wide zooms"),
    ("t_web_guards", "web-tci", "the web TCI page"),
    ("t_web_guards", "web-guards", "the web's guarded pages refuse what they must"),
    ("t_web_guards", "funnel-sim", "a Funnel (internet) connection's limits"),
    ("t_web_more", "web-https", "HTTPS served with its own certificate"),
    ("t_web_more", "wss-proxy", "the WebSocket proxy over HTTPS"),
    ("t_web_more", "ssh-key-checks", "the SSH keys page refuses bad keys"),
    ("t_web_more", "backup-parts", "the backup's parts listed"),
    ("t_web_more", "textlog-page", "the text log page"),
]

# suite_cover_a's settings and display checks (receive only; each puts back what it changes, guarded)
SUITE_PORTED_COVER_A = [
    ("t_btcat", "btcat-bridge", "the Bluetooth CAT bridge on its serial port, its RFCOMM listening"),
    ("t_btcat", "btcat-power", "the Bluetooth adapter's power as set"),
    ("t_bt_pairing", "bt-pairing", "the pairing window opened and closed by itself"),
    ("t_backlight", "backlight", "the backlight setting changes the screen's brightness, put back"),
    ("t_screen_off", "screen-off", "the screen off and on again"),
    ("t_screensaver", "screensaver", "the screensaver after its minute, gone at a touch"),
    ("t_themes", "themes", "each theme taken live, 1bit included, put back"),
    ("t_themes_alike", "themes-alike", "the built-in themes drawn alike on the two radios"),
    ("t_zoom_step", "zoom-step", "the zoom kept per mode"),
    ("t_step_zoom", "step-zoom", "the tuning step kept per zoom"),
    ("t_band_plan", "band-plan", "the band plan's segments as set"),
    ("t_own_band", "own-band", "a band of the operator's own on the main screen's plan"),
    ("t_mode_from_plan", "mode-from-plan", "the mode taken from the band plan as the dial moves"),
    ("t_lock_per_band", "lock-per-band", "the dial lock kept per band"),
    ("t_channel_snap", "channel-snap", "a channel band's dial snapped to its channels"),
    ("t_speaker_off", "speaker-off", "the speaker off keeps the audio elsewhere"),
    ("t_line_in", "line-in", "the line in as the audio source"),
    ("t_transverters", "transverters", "a transverter's offsets and its power limit"),
    ("t_tx_shift", "tx-shift", "a TX shift's frequency, and a shift below 0 Hz refused"),
    ("t_tx_outside", "tx-outside", "transmitting outside the ham bands: the switch refuses it while off"),
]

# suite_cover's and suite_cover_b's checks, all but those orch has already (tci-rate is tci-audio-rate, freq-lock is
# dial-lock): (module, its t_ function, the check, what: the t_ function's first line)
SUITE_MORE_SKIP = {"tci-rate", "freq-lock"}


def suite_more_ported():
    import importlib
    out = []
    for module in ("suite_cover", "suite_cover_b"):
        try:
            m = importlib.import_module(module)
        except Exception as e:              # a batch that does not load: said, the rest of orch goes on
            say(f"orch: {module} not loaded ({type(e).__name__} {e})")
            continue
        for t_name in sorted(n for n in dir(m) if n.startswith("t_") and callable(getattr(m, n))):
            try:
                checks = suite_checks(module, t_name)
            except Exception:
                continue
            doc = " ".join((getattr(m, t_name).__doc__ or "").split())
            what = doc.split(". ")[0][:200] if doc else t_name[2:].replace("_", " ")
            out += [(module, t_name, name, what) for name in checks if name not in SUITE_MORE_SKIP]
    return out


# ---- The 1:1 suite's over-the-air groups run whole (air_a, air_b, air_c): each t_* runs as the suite runs it, with a
# suite.Run of its own (its sessions, sync_both and its put-back, a Watch on every over), its result rows collected.
# orch around it: the radios ready before, its sync guard paused (some overs set the radios apart on purpose: split,
# TX shift; each under the suite's Watch, which compares every receiver setting as a radio keys), both radios as
# possible senders (the Watch stops two on the air at once), then control back and the two alike ----

AIR_SKIP = {"power-steps", "ft8-stop", "remote-busy", "ext-tune", "ft8-cq"}     # orch's own already


def air_groups():
    import importlib
    out = []
    for module in ("suite_air_a", "suite_air_b", "suite_air_c"):
        try:
            m = importlib.import_module(module)
        except Exception as e:
            say(f"orch: {module} not loaded ({type(e).__name__} {e})")
            continue
        for name, fn in m.TESTS:
            if name in AIR_SKIP:
                continue
            doc = " ".join((fn.__doc__ or "").split())
            out.append((module, fn.__name__, name, doc.split(". ")[0][:220] if doc else name))
    return out


def suite_group(o, module, t_name, name, call=None):
    """module's t_name run whole on both radios (its own directions, sessions and put-back): passes with no row
    failed; the rows to look at said. call(m, run): another way in (suite_more.paths' parts)"""
    import importlib
    import suite
    m = importlib.import_module(module)
    run = suite.Run([A, B], [f"orch-{name}".replace(" ", "_")])
    f0, m0 = int(o.r[A].state.get("freq") or F_TEST), int(o.r[A].state.get("mode") or 2)
    rows = []
    with o.step(f"{module}: {name}, its directions as the suite runs them (its own Watch on each over)") as s:
        o.roles = {A: "TX", B: "TX"}
        try:
            (call or (lambda m_, r_: getattr(m_, t_name)(r_)))(m, run)
        except Exception as e:              # the group's own fault: its rows so far, and this
            run.results.append({"batch": module, "test": name, "ok": False,
                                "says": f"error: {type(e).__name__}: {e}"})
        finally:
            try:
                run.close()
            finally:
                o.roles = {A: "RX", B: "RX"}
        rows = run.results
        s.confirm(f"{len(rows)} rows: {sum(1 for x in rows if x['ok'] is True)} pass, "
                  f"{sum(1 for x in rows if x['ok'] is None)} to look at, {sum(1 for x in rows if x['ok'] is False)} fail")
    try:
        for r in o.r.values():
            if r.stale():
                r.reconnect()
            if not r.ctl_mine:
                r.take_control()
        o.both({"freq": f0, "mode": m0, "pwr": W})
    except Refused as e:
        o.st.ev(f"not put back alike after {name}: {e}", "bad")
    got = {"rows": [{k: x.get(k) for k in ("batch", "test", "ok", "says", "covers", "partial")} for x in rows],
           "run_dir": run.dir}
    bad = [f"{x['test']}: {x['says']}" for x in rows if x["ok"] is False]
    if bad:
        return False, "; ".join(bad)[:1500], got
    if not rows:
        return False, "the group recorded nothing", got
    look = [x["test"] for x in rows if x["ok"] is None]
    return True, (f"{sum(1 for x in rows if x['ok'])} rows pass: " + ", ".join(x["test"] for x in rows if x["ok"])[:600]
                  + (f"; to look at: {', '.join(look)}" if look else "")), got


def suite_checks(module, t_name):
    """{test: (kind, fn, want_tx)} that module's t_name registers (kind "r": fn(radio), "host": fn(name, host, ip))"""
    import importlib
    key = (module, t_name)
    if key in _SUITE:
        return _SUITE[key]
    m = importlib.import_module(module)
    got = {}

    def per_radio(run, batch, test, fn, covers, want_tx=False):
        got[test] = ("r", fn, want_tx)
        SUITE_FEATURES[(module, t_name, test)] = (batch, list(covers or []))
        return {}

    def by_radio(run, batch, test, covers, fn):
        got[test] = ("host", fn, False)
        SUITE_FEATURES[(module, t_name, test)] = (batch, list(covers or []))

    def both_radios(run, test, covers, fn):        # suite_cover_a's, for a check that waits: fn(name, host, ip)
        got[test] = ("host", fn, False)
        SUITE_FEATURES[(module, t_name, test)] = (module.replace("suite_", ""), list(covers or []))
    saved = {k: getattr(m, k) for k in ("per_radio", "by_radio", "both_radios") if hasattr(m, k)}
    m.per_radio = per_radio
    if "by_radio" in saved:
        m.by_radio = by_radio
    if "both_radios" in saved:
        m.both_radios = both_radios
    c = bench.conf()
    fake = types.SimpleNamespace(c=c, names=[A, B], host=lambda n: c["radios"][n]["host"],
                                 banner=lambda *a, **k: None, result=lambda *a, **k: None,
                                 radios={}, shown={},   # the suite's own sessions: none under orch (its one is the check's)
                                 dir=os.path.join(bench.STATE, "runs", "suite_checks"))
    os.makedirs(os.path.join(fake.dir, "shots"), exist_ok=True)

    def shot(name, label):                  # the suite's own screen picture (suite.Run.shot), into fake.dir/shots
        import suite
        return suite.Run.shot(fake, name, label)
    fake.shot = shot
    try:
        getattr(m, t_name)(fake)
    finally:
        for k, v in saved.items():
            setattr(m, k, v)
    _SUITE[key] = got
    return got


# ---- Coverage: every feature of features.json against the latest result of each test that confirms it, orch's own
# and the 1:1 suite's runs before it (orch.py coverage, suite.py coverage the same). orch's own tests say their
# features here; a suite check run by orch (suite_pair) has those it registered; a suite batch run whole (suite_group)
# has its rows' (from 2026-11-25 kept in orch's results; before, borrowed from the suite's report of the same row).
# A suite result that orch has run again since gives way to orch's ----

# What each of orch's own tests confirms, by features.json id: (covers, partial). A test's radio and direction
# suffixes ("cw-text x6100>x6200", "ft8-copies x6200 tci") are left off its name here (test_base)
ORCH_COVERS = {
    "identity": (["system-identity-same-version", "system-same-apps"], []),
    "services": (["system-services-up"], []),
    "web-secrets": (["web-secrets-never-returned", "web-files-page"], []),
    "caps": (["panel-filter-limits"], ["panel-filter-ssb-dig", "panel-filter-cw", "panel-filter-am-fm",
                                       "panel-filter-wfm", "panel-set-step-zoom", "panel-rx-switches",
                                       "radio-core-x6200-fm-airband"]),
    "bands-all": ([], ["radio-core-band-keys", "main-screen-band-list-region-edges", "panel-set-band"]),
    "sync-bands": ([], ["panel-set-freq", "panel-set-mode"]),
    "usb-steps": (["path-usb-tones"], []),
    "lsb-steps": (["path-lsb-tones"], []),
    "am-steps": (["path-am-voice"], []),
    "fm-steps": (["path-fm-voice"], []),
    "cw-text": (["cw-decoder-copy", "path-cw-both-ways"], []),
    "cw-quiet": (["cw-decoder-no-noise"], []),
    "cw-quiet wide": (["cw-decoder-no-noise", "cw-decoder-tone-floor"], []),
    "cw-noise-tone": ([], ["cw-decoder-no-noise"]),
    "cw-noise-shape": ([], ["cw-decoder-no-noise"]),
    "classic-wf3d": (["settings-display-waterfall-3d"], ["lookfeel-classic-main-screen", "gpu-gui-layer"]),
    "power-steps": (["path-tx-power-readback", "tci-drive"], []),
    "power-6m": (["path-tx-power-readback"], []),
    "power-6m-fm": ([], ["path-tx-power-readback"]),
    "power-6m-am": ([], ["path-tx-power-readback"]),
    "ft8-cq": (["ft8-tx-cq", "panel-ft8-cq", "path-data-ft8-both-ways"], []),
    "psk31-text": (["psk-mode-txrx"], ["psk-macros-tx"]),
    "rtty-text": (["rtty-rx-decode", "rtty-tx-player"], []),
    "wefax-chart": (["wefax-tx-player", "wefax-rx-auto-cont"], ["wefax-save-clear"]),
    "tune-carrier": (["remote-tune-power"], ["panel-ext-tune"]),
    "ft8-qso": (["ft8-auto-sequence", "ft8-auto-log", "ft8-qso-two-logs"], ["panel-ft8-auto-hold"]),
    "ft8-stop": (["panel-ft8-stop", "ft8-tx-call-stop"], []),
    "tci-control": (["tci-vfo", "tci-modulation"], []),
    "rigctl": (["rigctl-get-freq", "rigctl-set-freq", "rigctl-mode", "rigctl-ptt-refused"], []),
    "dial-lock": (["radio-core-freq-lock", "panel-user-lock"], []),
    "remote-security": (["auth-token", "role-view-refusals"], []),
    "rx-session-refused": (["panel-ptt-refused-no-tx", "panel-ft8-tx-refused-no-tx"], ["role-view-refusals"]),
    "apps-alike": ([], ["js8-open", "olivia-open", "sstv-open-freq", "navtex-open", "psk-open",
                        "digi-app-reopen-last-freq"]),
    "ft8-reply": (["panel-ft8-reply", "ft8-reply-row"], []),
    "remote-busy": (["control-stays-during-tx"], []),
    "band-memory": (["radio-core-pre-att-per-band"], ["radio-core-band-last-frequency"]),
    "cb-channel": (["g-own-channel"], ["cb-knob-channels", "radio-core-band-cb-c"]),
    "js8-text": (["js8-reply-send-box"], ["js8-directed-queries"]),
    "sstv-picture": (["sstv-auto-rx"], ["sstv-tx-mode", "sstv-tx-cq"]),
    "olivia-text": (["olivia-modes-txrx"], []),
    "cw-kbd": (["cwkbd-message-pages"], []),
    "pages-look": ([], ["lookfeel-sentry-a", "lookfeel-sentry-b", "lookfeel-sentry-c", "lookfeel-sentry-d",
                        "lookfeel-sentry-e", "lookfeel-sentry-f", "lookfeel-sentry-g", "lookfeel-sentry-h"]),
    "comp-mic": (["settings-audio-compressor"], ["tx-comp-ratio"]),
    "g-voiceprint": (["g-voiceprint", "g-voiceprint-tx-x6100", "path-mic-voice"], []),
    "ext-tune": (["ext-tune-app-button", "ext-tune-time-limit", "ext-tune-carrier-txfreq"], []),
    "js8-close-to-g": ([], ["js8-open"]),
    "system-health": (["system-battery-log"], ["rtc-drift-correction"]),
    "rx-vfo-split": (["panel-set-vfo"], ["settings-general-independent-vfos"]),
    "tci-receiver": (["tci-rx-filter-band", "tci-agc-mode", "tci-rit", "tci-case-insensitive", "tci-unknown-silent"],
                     []),
    "rigctl-more": (["rigctl-levels", "rigctl-split-off", "rigctl-dump-state"], []),
    "remote-apps": (["panel-apps-list", "panel-open-refused"], []),
    "power-cap": (["tx-power-cap-per-radio", "panel-pwr-range"], ["tx-power-battery-5w"]),
    "smc-remote": (["settings-txcal-smeter-measure"], []),
    "web-pages": (["web-configurator-reachable"], "WEB_PAGES"),
    "tci-out-of-band": (["tci-key-refused-band"], []),
    "tci-ask": (["tci-ask-prompt", "tci-ask-timeout"], []),
    "tci-cw-usb": (["tci-cw-refused-not-cw"], []),
    "tci-key-cw": (["tci-key-refused-cw"], []),
    "system-checks": (["system-battery-log", "data-fsck-log", "settings-good-copy", "ssh-keys-only",
                       "tailscale-cli-status", "app-logs-retention", "start-trace-gui", "system-services-up",
                       "cpu-speed", "cpu-temp-x6200", "pmu-temp-x6100", "gpu-mali-draws", "sessions-file-hashed",
                       "adif-logs-header", "system-update-ft8-library", "system-identity-same-version"],
                      ["system-update-check", "rtc-drift-correction"]),
    "image": (["path-tx-image-carrier"], []),
    "spurs": ([], ["path-data-ft8-both-ways"]),
    "skirt": ([], ["tx-tci-transmit-level"]),
    "tci-tail": (["tci-trx-tci-audio", "tci-wsjtx-compat"], []),
    "ft8-copies": ([], ["path-data-ft8-both-ways", "digi-tx-level-x6200-power"]),
    "cw-timing": (["path-cw-both-ways"], ["cw-keyer-settings"]),
    "ambe-vocoder": (["dmr-voice-usb3000"], ["dsdfme-voice"]),
    "dv-dmr": (["dmr-detect", "dmr-voice-usb3000"], []),
    "dv-dmr-noparity": ([], ["dmr-voice-usb3000"]),
    "dv-dstar": (["dsdfme-detect", "dsdfme-voice"], []),
    "dv-ysf": (["dsdfme-detect", "dsdfme-voice"], []),
    "dv-nxdn": (["dsdfme-detect", "dsdfme-voice"], []),
    "dv-p25": (["dsdfme-detect", "dsdfme-voice"], []),
    "tci-audio-rate": (["tci-rx-audio-stream", "tci-audio-format"], []),
    "tci-takes-control": (["control-tci-takes"], []),
    "rx-settings": (["panel-rx-switches", "panel-levels"], []),
    "ptt-lease": (["panel-ptt-lease", "panel-ptt-release"], []),
    "ptt-locked": (["sentry-tx-locked"], []),
    "ptt-out-of-band": (["panel-ptt-refused-band", "tx-out-of-band-ptt-refused"], []),
}


def test_base(name):
    """A test's name without its radios and direction ("cw-text x6100>x6200", "ft8-copies x6200 tci app")"""
    return re.sub(r" (x6100>x6200|x6200>x6100|x6100|x6200)( .*)?$", "", name)


def test_features(t):
    """(batch, covers, partial) of one of TESTS (not a suite batch run whole: its rows say theirs)"""
    if t.suite:
        module, t_name, check = t.suite
        suite_checks(module, t_name)        # registers them, nothing run
        batch, covers = SUITE_FEATURES.get(t.suite, (module.replace("suite_", ""), []))
        return batch, covers, []
    covers, partial = ORCH_COVERS.get(test_base(t.name), ([], []))
    if partial == "WEB_PAGES":
        feats = json.load(open(os.path.join(HERE, "features.json")))
        partial = [f["id"] for f in feats if f["area"] == "web" and f["id"].endswith("-page")]
    return "orch", list(covers), list(partial)


def _ok_of(result):
    return True if result == "PASS" else None if result in ("LOOK", "KNOWN") else False


def coverage_all():
    """features.json against the latest result of every test, orch's and the suite's: a coverage run of its own
    (runs/<time>-coverage: coverage.md, results.json). Its folder"""
    import glob
    import suite_more
    runs = os.path.join(bench.STATE, "runs")
    suite = {}                              # (batch, test): the suite's latest row
    for path in sorted(glob.glob(os.path.join(runs, "*", "report.json"))):
        try:
            rows = json.load(open(path))
        except (OSError, ValueError):
            continue
        if isinstance(rows, list):
            for x in rows:
                if isinstance(x, dict) and "batch" in x and "test" in x:
                    suite[(x["batch"], x["test"])] = dict(x, run=os.path.basename(os.path.dirname(path)))
    by_test = {}
    for (b, t), x in suite.items():
        by_test.setdefault(t, x)
    latest = {}                             # orch's: test name -> (its result, its run)
    for path in sorted(glob.glob(os.path.join(runs, "*-orch*", "results.json"))):
        try:
            res = json.load(open(path))
        except (OSError, ValueError):
            continue
        if isinstance(res, list):
            for x in res:
                if isinstance(x, dict) and x.get("name") and x.get("result"):
                    latest[x["name"]] = (x, os.path.basename(os.path.dirname(path)))
    rows, no_features, orch_n = dict(suite), [], 0
    for t in TESTS:
        if t.name not in latest:
            continue
        res, rdir = latest[t.name]
        orch_n += 1
        if t.group:
            module = t.group[0]
            for x in (res.get("data") or {}).get("rows") or []:
                batch = x.get("batch") or ("paths" if module == "suite_more" else module.replace("suite_", ""))
                old = suite.get((batch, x["test"])) or by_test.get(x["test"]) or {}
                rows[(batch, x["test"])] = {"batch": batch, "test": x["test"], "ok": x.get("ok"),
                                            "covers": x.get("covers") or old.get("covers") or [],
                                            "partial": x.get("partial") or old.get("partial") or [], "run": rdir}
            continue
        batch, covers, partial = test_features(t)
        if t.suite:                         # the suite's own rows of this check give way to orch's result
            check = t.suite[2]
            for k in [k for k in rows if k[0] == batch and k[1] in (check, f"{check} {A}", f"{check} {B}",
                                                                     f"{check} 1:1")]:
                del rows[k]
        if not covers and not partial:
            no_features.append(t.name)
        rows[("orch", t.name)] = {"batch": "orch", "test": t.name, "ok": _ok_of(res["result"]), "covers": covers,
                                  "partial": partial, "run": rdir}
    # Each feature by its latest result: an older row's claim on a feature gives way to a newer row's (an air
    # group's rows renamed since the suite ran it, "ft8-tx x6100->x6200" now "ft8-level x6100->x6200"): a newer
    # full test of it outranks any older; a newer partial one outranks only older partial ones
    newest_full, newest_any = {}, {}
    for x in rows.values():
        x["covers"], x["partial"] = list(x.get("covers") or []), list(x.get("partial") or [])
        when = x["run"][:15]
        for fid in x["covers"]:
            newest_full[fid] = max(newest_full.get(fid, ""), when)
            newest_any[fid] = max(newest_any.get(fid, ""), when)
        for fid in x["partial"]:
            newest_any[fid] = max(newest_any.get(fid, ""), when)
    for x in rows.values():
        when = x["run"][:15]
        x["covers"] = [fid for fid in x["covers"] if when >= newest_full[fid]]
        x["partial"] = [fid for fid in x["partial"] if when >= newest_any[fid]]
    run = types.SimpleNamespace(dir=os.path.join(runs, time.strftime("%Y%m%d-%H%M%S-coverage")),
                                results=list(rows.values()))
    os.makedirs(run.dir, exist_ok=True)
    with open(os.path.join(run.dir, "results.json"), "w") as f:
        json.dump(run.results, f, indent=1)
    suite_more.coverage(run)
    with open(os.path.join(run.dir, "coverage.md"), "a") as f:
        f.write(f"\nFrom {orch_n} of orch's {len(TESTS)} tests' latest results (the suite's own runs before them where "
                f"orch has not run a check again), each feature by its latest result." +(f" orch's tests that name no feature: {', '.join(no_features)}."
                                                     if no_features else "") + "\n")
    return run.dir


def suite_pair(o, module, t_name, test, alone=False):
    """One of the 1:1 suite's checks on both radios at once: each must pass (a LOOK, its own "to look at", is said);
    both put back alike on the frequency and mode they started from, the power at 0.1 W, control the bench's.
    alone: the bench's own remote sessions closed while it runs (every remote client is a viewer, and a viewer keeps
    the screensaver away), their silence planned, then connected again"""
    import socket
    kind, fn, _ = suite_checks(module, t_name)[test]
    f0, m0 = int(o.r[A].state.get("freq") or F_TEST), int(o.r[A].state.get("mode") or 2)
    out = {}
    if alone:
        o.changing += 1                     # no state from either while closed, nor while each connects again
        for n, r in o.r.items():
            o.planned[n] = f"planned: {test}, the bench's remote session closed meanwhile"
            r.close()
        time.sleep(1.5)
    try:
        with o.step(f"{module}: {test} on both at once") as s:
            def one(n, r):
                ok, says = fn(r) if kind == "r" else fn(n, r.host, socket.gethostbyname(r.host))
                return {"ok": ok if ok is None else bool(ok), "says": says}
            out = in_parallel(o, one, 240)
            s.confirm("; ".join(f"{n}: {v['says']}"[:400] for n, v in out.items()))
    except Refused as e:
        return False, str(e), out
    finally:
        try:
            if alone:
                try:
                    for n, r in o.r.items():
                        r.reconnect()       # its session again, control the bench's (when free)
                        o.planned.pop(n, None)
                finally:
                    o.changing -= 1
            for r in o.r.values():
                if not r.ctl_mine:
                    r.take_control()        # the bench's own back (a TCI client of the check's had it); never the
                                            # operator's (take_control refuses that)
            o.both({"freq": f0, "mode": m0, "pwr": W})
        except Refused as e:
            o.st.ev(f"not put back alike after {test}: {e}", "bad")
    bad = [f"{n}: {v['says']}" for n, v in out.items() if v["ok"] is False]
    if bad:
        return False, "; ".join(bad), out
    look = [n for n, v in out.items() if v["ok"] is None]
    return True, ("; ".join(f"{n}: {v['says']}" for n, v in out.items()) +
                  (f" (to look at on {', '.join(look)})" if look else "")), out


def t_system_checks(o):
    """Each radio read over SSH, nothing changed (suite_proto's system batch): the battery log and model, the data
    partition's check, the settings' good copy, password logins refused, Tailscale's status, the run logs kept, the
    start trace, the services and ports, the CPU, temperatures, the GPU, the remote's sessions file, the update state,
    the logs' ADIF header, libft8, the clock; and the two on the same image level and version"""
    import suite_proto as sp
    data, fails, looks = {}, [], []
    with o.step("both radios read (SSH, the ports, a password login tried, the clock)") as s:
        def read(n, r):
            toks = bench.load_json(os.path.join(bench.STATE, "tokens.json"), {}).get(n, {}).values()
            d = sp.sys_read(r.host, list(toks))
            d["ports"] = sp.ports_open(r.host, [22, 80, 443, 4532, 50001, 50002])
            d["pw"] = sp.password_refused(r.host)
            d["clock_off"] = sp.clock_offset(r.host)
            return d
        try:
            data = in_parallel(o, read, 120)
        except Refused as e:
            return False, str(e), {}
        s.confirm(", ".join(f"{n}: {d.get('ident', '')}" for n, d in data.items()))

    def battery_log(n, d):
        age = int(d["now"]) - int(d["bat"].split(",")[0])
        return 0 <= age <= 150, f"its last line {age} s old (a line a minute)"

    def fsck(n, d):
        return "exit 0" in d["fsck"] or "left" in d["fsck"], f"data_fsck.log ends {d['fsck'][-60:]!r}"

    def good_copy(n, d):
        mt, integ = d["good"].split(" ", 1)
        newer = int(mt) >= int(d["gui_start"]) - 5 or int(d["now"]) - int(d["gui_start"]) < 90
        return integ.strip() == "ok" and newer, f"integrity {integ.strip()!r}, written {int(d['now']) - int(mt)} s ago"

    def ssh_keys(n, d):
        ok, last = d["pw"]
        return ok, f"a password login refused, offering {last[-50:]!r}"

    def tailscale(n, d):
        return "state=" in d["ts"] and "version=" in d["ts"], f"{d['ts'][:60]!r}"

    def logs(n, d):
        return int(d["logs"]) <= 50, f"{d['logs']} run logs (50 kept)"

    def trace(n, d):
        have = d.get("trace_has", [])
        return len(have) == 6, f"start_trace.txt has {len(have)} of the 6 steps"

    def services(n, d):
        procs = set(d.get("proc", []))
        need = {"x6100_gui", "x6x00-gui-watch", "gpsd", "NetworkManager", "pulseaudio", "x6100_webserver"}
        return need <= procs and all(d["ports"].values()), (f"ports {sum(d['ports'].values())}/{len(d['ports'])} "
                                                           f"open" + (f", missing {sorted(need - procs)}" if need - procs
                                                                      else ""))

    def cpu(n, d):
        gov, mhz = d["gov"].split()
        return gov == "performance" and (int(mhz) == 1008000 or n == B), f"{gov}, {int(mhz) // 1000} MHz"

    def temps(n, d):
        if n == B:
            c = int(d["cputemp"]) / 1000
            return 15 < c < 90, f"processor {c:.1f} C"
        cs = []
        for x in d.get("iio", []):
            raw, off, scale = x.split()[-3:]
            cs.append(round((int(raw) + int(off)) * float(scale) / 1000, 1))
        return any(5 < c < 90 for c in cs), f"power chip {cs} C"

    def gpu(n, d):
        return "draws" in d.get("gpu", []), f"Mali400 draws {'draws' in d.get('gpu', [])}"

    def sessions(n, d):
        lines, hashed, bak, plain = (d["sess"].split() + ["", "", "", ""])[:4]
        return int(lines) <= 16 and int(hashed) >= 1 and bak == "bak" and plain == "0", \
            f"{lines} lines, {hashed} hashed, .bak {bak == 'bak'}, the bench's tokens in plain {plain}"

    def update(n, d):
        j = json.loads(d["upd"])
        last = j.get("last") or {}
        return j.get("pending") is None and last.get("result") == "confirmed", \
            f"running {j.get('running')}, pending {j.get('pending')}, last {last.get('result')} {last.get('version')}"

    def adif(n, d):
        a = d.get("adif_day") or d["adif"]
        if not a:
            return None, "no ADIF log on it yet"
        if "<ADIF_VER:5>3.1.8" not in a or "<PROGRAMID:" not in a:
            return False, f"starts {a[:60]!r}"
        return (True if "GoldCorpHF" in a else None), f"PROGRAMID {'GoldCorpHF' if 'GoldCorpHF' in a else 'an older name'}"

    def ft8lib(n, d):
        return d.get("libft8") == "yes", f"/usr/lib/libft8.so {d.get('libft8') == 'yes'}"

    def model(n, d):
        age = int(d["model_age"])
        return 0 <= age <= 1500, f"battery_model.json {age} s old"

    def clock(n, d):
        return abs(d["clock_off"]) < 1.5 and d.get("drift") == "yes", (f"{d['clock_off']:+.2f} s, drift learnt "
                                                                        f"{d.get('drift') == 'yes'}")
    for name, fn in (("battery log", battery_log), ("data partition's check", fsck), ("settings' good copy", good_copy),
                     ("password logins refused", ssh_keys), ("Tailscale's status", tailscale),
                     ("run logs kept", logs), ("start trace", trace), ("services and ports", services),
                     ("CPU", cpu), ("temperature", temps), ("GPU", gpu), ("remote's sessions file", sessions),
                     ("update state", update), ("ADIF header", adif), ("libft8", ft8lib), ("battery model", model),
                     ("clock", clock)):
        with o.step(f"{name} on both") as s:
            said = []
            for n, d in data.items():
                try:
                    ok, says = fn(n, d)
                except (KeyError, ValueError, IndexError, TypeError) as e:
                    ok, says = False, f"could not read it: {type(e).__name__} {e}"
                said.append(f"{n}: {says}")
                if ok is False:
                    fails.append(f"{name} {n}: {says}")
                elif ok is None:
                    looks.append(f"{name} {n}: {says}")
            s.confirm("; ".join(said))
    with o.step("the same image level and version on both") as s:
        vals = {n: tuple(d.get("ident", "").split()[1:]) for n, d in data.items()}
        if len(set(vals.values())) != 1 or not all(vals.values()):
            fails.append(f"versions differ: {vals}")
        s.confirm(str(vals))
    got = {"fails": fails, "looks": looks}
    if fails:
        return False, "; ".join(fails), got
    return True, "every check passed on both" + (f" (to look at: {'; '.join(looks)})" if looks else ""), got


def image_test(o, sender, listener, bands=("20", "17", "15", "12", "10"), off=1500, tones=(600, 900), amp=0.3):
    """The sender's opposite-sideband image and carrier leak, band by band (image.py's measurement, the owner's
    2026-11-20 "you MUST prove this via testing"): USB tones 600 and 900 Hz at 0.1 W, the listener OFF Hz below the
    sender's dial (declared to the sync guard and the Watch: the one difference this test makes), so in its USB audio
    the wanted tone is at OFF + fa, the carrier at OFF and the image (the tone mirrored around the sender's dial) at
    OFF - fa. Passes with every image and carrier leak IMAGE_MAX_DBC or lower; each band's figures kept"""
    import subprocess
    import image as im
    from tones import TciRx, peak, FS as TFS
    S, L = o.r[sender], o.r[listener]
    plan = [[0, im.LEAD_S]] + [[t, im.TONE_S] for t in tones] + [[0, im.TAIL_S]]
    key_s = sum(x for _, x in plan) + 1.0
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
            f"root@{S.host}:/tmp/tci_tone_tx.py"])
    rows, worst, tci = [], None, None
    try:
        tci = TciRx(L.host)
        for band in bands:
            f = im.BANDS[band]
            with o.step(f"{band} m: both on {f / 1e6:.3f} USB, the receiver profile alike") as s:
                want, checks = bench.rx_profile(f, 2, pre=0, att=0, rfg=100, agc=3, sql=0, filt=(100, 3000))
                o.both(dict(want, pwr=W, atu=0, xit=0, rit=0, split=0), checks)
            decl = bench.apart({"freq": off}, f"image {sender}: {listener} {off} Hz below {sender} on {band} m, the "
                                              f"image's place in its USB audio")
            decl.__enter__()                    # declared until the two are on one frequency again (below)
            with o.step(f"{band} m: {listener} {off} Hz below (declared: the one difference)") as s:
                o.st.ev(f"declared: {listener} {off} Hz below {sender} for this over", "conf")
                bench.ensure(L, {"freq": f - off})
                s.confirm(f"{sender} {S.state.get('freq')}, {listener} {L.state.get('freq')}")
            with o.step(f"{band} m: {sender} sends {tones} Hz at {W} W") as s:
                _, total = bench.chain_gate(o.c, "radios")
                bench.ensure(S, *bench.tx_profile(f, 2, W))
                bench.radio_tx_gate(S, o.c, W)
                bench.listener_gate(L)
                o.roles = {sender: "TX", listener: "RX"}
                watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W,
                                    label=f"image {band} m")      # the offset declared above (bench.apart)
                try:
                    o.changing += 1
                    try:
                        bench.arm(S, True, mode=2)
                    finally:
                        o.changing -= 1
                    bench.tx_log({"who": sender, "f": f, "w": S.pwr(), "what": f"orch: image {band} m"})
                    t0 = time.time()
                    proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                             f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {amp} {key_s:.1f} "
                                             f"50001 {im.FADE_S}"], stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL)
                    while proc.poll() is None and not watch.why:
                        time.sleep(0.05)
                    if proc.poll() is None:
                        bench.stop_bench_programs(S)
                    t1 = time.time()
                    try:
                        S.wait("off the air", lambda: not S.tx, 5)
                    except Refused:
                        S.stop()
                finally:
                    watch.close()
                    o.changing += 1
                    try:
                        bench.arm(S, False)
                    finally:
                        o.changing -= 1
                    o.roles = {A: "RX", B: "RX"}
                if watch.why:
                    raise Refused(f"the watch stopped the over: {watch.why}")
                time.sleep(1.0)
                x, _ = tci.audio(t0 - 0.5, t1 + 1.0)
                on = im.band_onset(x, off + tones[0]) if len(x) else None
                if on is None:
                    raise Refused(f"no tone in {listener}'s audio")
                row = {"band": band, "f": f, "tones": []}
                for fa, (s0, s1) in zip(tones, im.segs(on, tones)):
                    seg = x[int(s0 * TFS):int(s1 * TFS)]
                    fw, lw = peak(seg, off + fa, 40)
                    fc, lc = peak(seg, off, 40)
                    fi, li = peak(seg, off - fa, 40)
                    fl = im.floor_db(seg)
                    t = {"fa": fa, "wanted_dbfs": round(lw, 1), "floor_dbfs": round(fl, 1),
                         "image_dbc": round(li - lw, 1), "carrier_dbc": round(lc - lw, 1),
                         "image_over_floor_db": round(li - fl, 1)}
                    row["tones"].append(t)
                    w_ = max(t["image_dbc"], t["carrier_dbc"])
                    worst = w_ if worst is None else max(worst, w_)
                rows.append(row)
                s.confirm("; ".join(f"{t['fa']} Hz: image {t['image_dbc']} dBc, carrier {t['carrier_dbc']} dBc"
                                    for t in row["tones"]))
            with o.step(f"{band} m: the two on one frequency again") as s:
                try:
                    bench.ensure(L, {"freq": f})
                finally:
                    decl.__exit__(None, None, None)
                alike(o, ["freq", "mode"])
                s.confirm("alike")
    except Refused as e:
        return False, str(e), {"bands": rows}
    finally:
        bench.undeclare(f"image {sender}:")         # a band's declaration a refusal left: ended with the test
        if tci is not None:
            tci.close()
        try:
            o.both({"freq": int(o.r[A].state.get("freq") or 14200000), "mode": 2})
        except Refused as e:
            o.st.ev(f"not alike after the image test: {e}", "bad")
    said = "; ".join(f"{r['band']} m " + "/".join(str(t["image_dbc"]) for t in r["tones"]) for r in rows)
    carriers = "; ".join(f"{r['band']} m " + "/".join(str(t["carrier_dbc"]) for t in r["tones"]) for r in rows)
    bad = [f"{r['band']} m {t['fa']} Hz {t['image_dbc']}" for r in rows for t in r["tones"]
           if t["image_dbc"] > IMAGE_MAX_DBC]
    if bad:
        return False, (f"image over {IMAGE_MAX_DBC} dBc: {', '.join(bad)} (images dBc: {said}; carrier leak dBc: "
                       f"{carriers})"), {"bands": rows}
    return True, (f"{sender}'s images (dBc, 600/900 Hz): {said}, all {IMAGE_MAX_DBC} dBc or lower; its carrier leak "
                  f"(dBc, said, not judged here): {carriers}"), {"bands": rows}


IMAGE_MAX_DBC = -35         # an image above this is a fault (the X6100's 15 m went to -29 at times on 11-22). The
                            # carrier leak is said with it: the factory X6100's (no DC offsets of its own, its log's
                            # "No results for load dac_offset") reads -17 to -26 dBc at 0.1 W, nulled only by a
                            # calibration that keys it and writes the offsets (the owner's call)


SPUR_HZ = 156.25            # 25 FT8 tone steps: the X6100's FT8 app decoded the X6200's answers 156 Hz off, weak (-7 dB
                            # against +9 to +14), and the real one was dropped as its duplicate (2026-11-23, 11-24, 11-25;
                            # the X6200's log said its VFO right): a copy of the signal that far off
SPUR_MAX_DBC = -30          # a component near the tone above this is a fault (an FT8 decode of it lands 156 Hz off)


def spur_test(o, sender, listener, f=14074000, tone=1500, secs=6.0, runs=((3, 0.3), (0, 0.3), (3, 0.9)),
              judge=True):
    """What else comes with a clean tone near it: the sender's TCI tone at `tone` Hz in USB-D at 0.1 W on the FT8
    dial, the listener's TCI audio recorded with its AGC on (3, as the FT8 tests listen) and off (0); the components
    SPUR_HZ either side of the tone and the strongest one within 400 Hz (not within 20 Hz of the tone), in dB under it,
    for each (AGC, the tone's amplitude) of runs: the tone at 0.3 and near full scale (0.9: the FT8 app's level is
    learned up to the top, where a base that sets the power itself may hunt). Whether a copy 156 Hz off is the sender's
    transmitter or the listener's receiver: present with the AGC off too and in one direction only says the sender;
    gone with the listener's AGC off says its AGC"""
    import subprocess
    import numpy as np
    import image as im
    from tones import TciRx, peak, FS as TFS
    S, L = o.r[sender], o.r[listener]
    plan = [[0, 0.5], [tone, secs], [0, 0.3]]
    key_s = sum(x for _, x in plan) + 1.0
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
            f"root@{S.host}:/tmp/tci_tone_tx.py"])
    rows, tci = [], None
    try:
        tci = TciRx(L.host)
        for agc, amp in runs:
            with o.step(f"AGC {agc}: both on {f / 1e6:.3f} USB-D, the receiver profile alike") as s:
                want, checks = bench.rx_profile(f, 3, pre=0, att=0, rfg=100, agc=agc, sql=0, filt=(100, 3000))
                o.both(dict(want, pwr=W, atu=0, xit=0, rit=0, split=0), checks)
                s.confirm(f"both on {f} mode 3, AGC {agc}")
            with o.step(f"AGC {agc}: {sender} sends {tone} Hz (amplitude {amp}) at {W} W, {listener} records its "
                        f"audio") as s:
                _, total = bench.chain_gate(o.c, "radios")
                bench.ensure(S, *bench.tx_profile(f, 3, W))
                bench.radio_tx_gate(S, o.c, W)
                bench.listener_gate(L)
                o.roles = {sender: "TX", listener: "RX"}
                watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W,
                                    label=f"spurs {sender} AGC {agc} amplitude {amp}")
                try:
                    o.changing += 1
                    try:
                        bench.arm(S, True, mode=3)
                    finally:
                        o.changing -= 1
                    bench.tx_log({"who": sender, "f": f, "w": S.pwr(), "what": f"orch: spurs AGC {agc}"})
                    t0 = time.time()
                    proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                             f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {amp} {key_s:.1f} "
                                             f"50001 0.02"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    while proc.poll() is None and not watch.why:
                        time.sleep(0.05)
                    if proc.poll() is None:
                        bench.stop_bench_programs(S)
                    t1 = time.time()
                    try:
                        S.wait("off the air", lambda: not S.tx, 5)
                    except Refused:
                        S.stop()
                finally:
                    watch.close()
                    o.changing += 1
                    try:
                        bench.arm(S, False)
                    finally:
                        o.changing -= 1
                    o.roles = {A: "RX", B: "RX"}
                if watch.why:
                    raise Refused(f"the watch stopped the over: {watch.why}")
                time.sleep(1.0)
                x, _ = tci.audio(t0 - 0.5, t1 + 1.0)
                on = im.band_onset(x, tone) if len(x) else None
                if on is None:
                    raise Refused(f"no tone in {listener}'s audio")
                seg = x[int((on + 1.0) * TFS):int((on + secs - 0.6) * TFS)]
                fw, lw = peak(seg, tone, 40)
                _, ll = peak(seg, fw - SPUR_HZ, 6)
                _, lh = peak(seg, fw + SPUR_HZ, 6)
                w = np.hanning(len(seg))
                nfft = 1 << (int(math.ceil(math.log2(len(seg)))) + 1)
                X = 20 * np.log10(np.abs(np.fft.rfft(seg * w, nfft)) + 1e-12)
                fr = np.fft.rfftfreq(nfft, 1 / TFS)
                m = (np.abs(fr - fw) <= 400) & (np.abs(fr - fw) >= 20)
                k = int(np.argmax(np.where(m, X, -1e9)))
                top = X[np.searchsorted(fr, fw)]
                P = 10 ** (X / 10)

                def band(a, b):         # the power from a to b Hz off the tone (both sides), dB under the tone's
                    lo_ = (fr >= fw - b) & (fr <= fw - a)
                    hi_ = (fr >= fw + a) & (fr <= fw + b)
                    t_ = np.abs(fr - fw) <= 3
                    return round(10 * math.log10(P[lo_].sum() / P[t_].sum()), 1), \
                        round(10 * math.log10(P[hi_].sum() / P[t_].sum()), 1)
                row = {"agc": agc, "amp": amp, "tone_hz": round(fw, 1), "tone_dbfs": round(lw, 1),
                       "below_dbc": round(ll - lw, 1), "above_dbc": round(lh - lw, 1),
                       "worst_off_hz": round(float(fr[k] - fw), 1), "worst_dbc": round(float(X[k] - top), 1),
                       "floor_dbc": round(im.floor_db(seg) - lw, 1),
                       # the skirt (an oscillator's phase noise, 2026-11-25: the X6200's FT8 overs -25 dBc per 55 Hz
                       # out to 170 Hz), per 55 Hz as ft8_copies measures it
                       "skirt_100_dbc": band(100, 155), "skirt_400_dbc": band(400, 455)}
                row["wav"] = save_wav(o, x, f"spurs_{sender}_to_{listener}_agc{agc}_amp{amp}.wav")
                rows.append(row)
                s.confirm(f"tone {row['tone_hz']} Hz {row['tone_dbfs']} dBFS; -{SPUR_HZ} Hz {row['below_dbc']} dBc, "
                          f"+{SPUR_HZ} Hz {row['above_dbc']} dBc; the strongest within 400 Hz "
                          f"{row['worst_dbc']} dBc at {row['worst_off_hz']:+.1f} Hz; per 55 Hz 100-155 Hz off "
                          f"{row['skirt_100_dbc']}, 400-455 Hz off {row['skirt_400_dbc']} dBc")
    except Refused as e:
        return False, str(e), {"rows": rows}
    finally:
        if tci is not None:
            tci.close()
    said = "; ".join(f"AGC {r['agc']} amplitude {r['amp']}: -{SPUR_HZ} Hz {r['below_dbc']}, +{SPUR_HZ} Hz {r['above_dbc']} dBc (the "
                     f"strongest within 400 Hz {r['worst_dbc']} dBc at {r['worst_off_hz']:+.1f} Hz)" for r in rows)
    bad = [r for r in rows if max(r["below_dbc"], r["above_dbc"]) > SPUR_MAX_DBC]
    if not judge:
        skirt = "; ".join(f"{r['amp']}: {max(r['skirt_100_dbc'])}" for r in rows)
        return None, (f"{sender}'s {tone} Hz tone in {listener}'s audio, the skirt 100-155 Hz off per 55 Hz (dBc, the "
                      f"higher side) at each amplitude in turn: {skirt}: said, not judged"), {"rows": rows}
    return not bad, (f"{sender}'s {tone} Hz tone in {listener}'s audio: {said}"
                     + (f"; a component {SPUR_HZ} Hz off over {SPUR_MAX_DBC} dBc" if bad else
                        f"; none {SPUR_HZ} Hz off over {SPUR_MAX_DBC} dBc")), {"rows": rows}


def ft8_tci_copy_test(o, sender, listener, hz=1500, app=False):
    """The FT8 app's copies 156 Hz either side, the same message sent through TCI instead (gen_ft8 here, 24 kHz, the
    sender's TCI): whether the X6200's skirt is the waveform through its base (here too) or the app's way out (not
    here: its TCI tone has none, 2026-11-25). app: the sender's FT8 app open meanwhile (its waterfall, decoder and
    capture running: whether that is what disturbs its audio). Said, not judged"""
    import subprocess
    import digimodes
    import suite_air_b as ab
    from tones import TciRx
    S, L = o.r[sender], o.r[listener]
    os.makedirs(o.st.run_dir, exist_ok=True)
    x24 = ab.gen_ft8_24k(o.c, "CQ Q2TST FN00", hz, o.st.run_dir)
    local = os.path.join(o.st.run_dir, f"ft8_tci_{sender}.raw")
    secs = digimodes.write_raw(0.8 * x24 / max(1e-9, float(abs(x24).max())), local)
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{S.host}:/tmp/bench_ft8.raw"])
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
            f"root@{S.host}:/tmp/tci_tone_tx.py"])
    tci = TciRx(L.host)
    res = {}

    def body(watch):
        with o.step(f"{sender} sends the FT8 message ({secs:.1f} s) through its TCI, {listener} records") as st_:
            t0 = time.time()
            proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                     f"python3 /tmp/tci_tone_tx.py '@/tmp/bench_ft8.raw' 0.3 {secs + 2:.1f} 50001 "
                                     f"0.05"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            while proc.poll() is None:
                if watch.why:
                    bench.stop_bench_programs(S)
                    raise Refused(watch.why)
                time.sleep(0.05)
            time.sleep(1.0)
            res["x"] = tci.audio(t0, time.time())[0]
            st_.confirm(f"{len(res['x']) / 24000:.1f} s of audio")
    steps = None
    try:
        if app:
            with o.step(f"{sender}'s FT8 app open on its dial, Auto off, {listener} set as it") as st_:
                want, checks = bench.rx_profile(ab.FT_DIAL["FT8"], 3, pre=0, att=0, rfg=100, agc=3, sql=0,
                                                filt=(100, 3000))
                o.both(dict(want, pwr=W, atu=0, xit=0, rit=0, split=0), checks)
                o.changing += 1
                try:
                    steps = ab.ft8_open(S, "FT8")
                finally:
                    o.changing -= 1
                ab.ft8_set(S, "auto", False)
                bench.pair_align(L, S)
                st_.confirm(f"{S.state.get('app')} on {S.state.get('freq')}")
        over(o, sender, listener, ab.FT_DIAL["FT8"], 3, f"FT8 over TCI {sender}", body, agc=3 if not app else 0,
             pages=not app)
    except Refused as e:
        return False, str(e), {}
    finally:
        tci.close()
        subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}", "rm -f /tmp/bench_ft8.raw"],
                       capture_output=True, timeout=20)
        if steps is not None:
            o.changing += 1
            try:
                with bench.transition():
                    ab.ft8_close(S, steps)
                    bench.close_apps(S)
                    bench.sync_pair([L, S], {"freq": int(L.state.get("freq") or ab.FT_DIAL["FT8"]),
                                             "mode": int(L.state.get("mode", 3))})
            except Refused as e:
                o.st.ev(f"{sender}'s FT8 app not closed as found: {e}", "bad")
            finally:
                o.changing -= 1
    c = ab.ft8_copies(res.get("x"), hz)
    if c is None:
        return False, "no audio", {}
    c["wav"] = save_wav(o, res["x"], f"ft8_tci_{sender}_to_{listener}.wav")
    return None, (f"{sender}'s FT8 message through its TCI{' (its FT8 app open)' if app else ''} in {listener}'s audio at {c['hz']} Hz: {SPUR_HZ} Hz below "
                  f"{c['below_dbc']} dBc, above {c['above_dbc']} dBc, 400 Hz above (noise) {c['noise_dbc']} dBc: said, "
                  f"not judged"), c


TAIL_LOST_MAX_S = 0.15      # a TCI over may lose this much of its audio's end at the unkey (the envelope's edges)
TAIL_PAD_S = 1.5            # the control: this much keyed silence after the tone, all of the tone must come then


def tail_test(o, sender, listener, f=14074000, tone=1000, secs=2.0, amp=0.3):
    """A TCI over's last audio on the air (the owner's ask, 2026-11-24: a TCI over unkeys before its audio is out; the
    bench pads STEP_TAIL_S): the sender's TCI plays `secs` of `tone` and unkeys at once, trx:0,false right after its
    last frame as WSJT-X does; the listener's TCI audio measures how much of the tone came. Then the same with
    TAIL_PAD_S of silence after it (the bench's way: all of it comes). PASS when neither lost more than
    TAIL_LOST_MAX_S at its end"""
    import subprocess
    import numpy as np
    from scipy.signal import butter, filtfilt
    from tones import TciRx, FS as TFS
    S, L = o.r[sender], o.r[listener]
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
            f"root@{S.host}:/tmp/tci_tone_tx.py"])
    rows, tci = [], None
    try:
        tci = TciRx(L.host)
        with o.step(f"both on {f / 1e6:.3f} USB-D, the receiver profile alike (AGC off: the tone's edges as sent)") as s:
            want, checks = bench.rx_profile(f, 3, pre=0, att=0, rfg=100, agc=0, sql=0, filt=(100, 3000))
            o.both(dict(want, pwr=W, atu=0, xit=0, rit=0, split=0), checks)
            s.confirm(f"both on {f} mode 3, AGC 0")
        for pad in (0.0, TAIL_PAD_S):
            plan = [[0, 0.5], [tone, secs]] + ([[0, pad]] if pad else [])
            key_s = sum(x for _, x in plan) + 3.0
            with o.step(f"{sender}'s TCI plays {secs} s of {tone} Hz at {W} W, "
                        + (f"then {pad} s of silence, then unkeys" if pad else "then unkeys at once")
                        + f"; {listener} records its audio") as s:
                _, total = bench.chain_gate(o.c, "radios")
                bench.ensure(S, *bench.tx_profile(f, 3, W))
                bench.radio_tx_gate(S, o.c, W)
                bench.listener_gate(L)
                o.roles = {sender: "TX", listener: "RX"}
                watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W,
                                    label=f"tci-tail {sender} pad {pad}")
                try:
                    o.changing += 1
                    try:
                        bench.arm(S, True, mode=3)
                    finally:
                        o.changing -= 1
                    bench.tx_log({"who": sender, "f": f, "w": S.pwr(), "what": f"orch: tci-tail pad {pad}"})
                    t0 = time.time()
                    proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                             f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {amp} {key_s:.1f} "
                                             f"50001 0.01"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    while proc.poll() is None and not watch.why:
                        time.sleep(0.05)
                    if proc.poll() is None:
                        bench.stop_bench_programs(S)
                    t1 = time.time()
                    try:
                        S.wait("off the air", lambda: not S.tx, 5)
                    except Refused:
                        S.stop()
                finally:
                    watch.close()
                    o.changing += 1
                    try:
                        bench.arm(S, False)
                    finally:
                        o.changing -= 1
                    o.roles = {A: "RX", B: "RX"}
                if watch.why:
                    raise Refused(f"the watch stopped the over: {watch.why}")
                time.sleep(1.0)
                x, _ = tci.audio(t0 - 0.5, t1 + 2.0)
                if len(x) < TFS:
                    raise Refused(f"no audio from {listener}")
                n = np.arange(len(x))
                z = x * np.exp(-2j * np.pi * tone * n / TFS)
                b, a = butter(3, 40 / (TFS / 2))
                db = 20 * np.log10(np.abs(filtfilt(b, a, z.real) + 1j * filtfilt(b, a, z.imag)) + 1e-9)
                on = db > np.percentile(db, 97) - 10
                runs, k = [], 0         # the longest stretch over 10 dB under the tone's level: the tone as it came
                while k < len(on):
                    j = k
                    while j < len(on) and on[j] == on[k]:
                        j += 1
                    if on[k]:
                        runs.append(j - k)
                    k = j
                came = max(runs, default=0) / TFS
                row = {"pad_s": pad, "came_s": round(came, 3), "lost_s": round(max(secs - came, 0.0), 3),
                       "wav": save_wav(o, x, f"tci_tail_{sender}_to_{listener}_pad{pad}.wav")}
                rows.append(row)
                s.confirm(f"{came:.2f} s of the {secs} s tone came, {row['lost_s']:.2f} s lost at its end")
    except Refused as e:
        return False, str(e), {"rows": rows}
    finally:
        if tci is not None:
            tci.close()
    lost = {r["pad_s"]: r["lost_s"] for r in rows}
    ok = all(v <= TAIL_LOST_MAX_S for v in lost.values())
    return ok, (f"{sender}'s TCI over of {secs} s of {tone} Hz in {listener}'s audio: unkeyed at once "
                f"{secs - lost[0.0]:.2f} s came ({lost[0.0]:.2f} s of its end lost), with {TAIL_PAD_S} s of silence "
                f"after it {secs - lost[TAIL_PAD_S]:.2f} s ({lost[TAIL_PAD_S]:.2f} s lost); "
                f"{TAIL_LOST_MAX_S} s at most"), {"rows": rows}


def ft8_copy_test(o, sender, listener, txfs=(1325, 1500, 2000), w=W, judge=True):
    """Where the copies 156.25 Hz either side of an FT8 app's transmission come from (spur_test: a TCI tone has none):
    the sender's FT8 app sends one CQ at each TX Hz of txfs, the listener on G in USB-D on the same dial recording its
    audio, the power over the same 50 Hz SPUR_HZ below and above each CQ in dB under it (suite_air_b.ft8_copies). At
    1325 Hz the app moves no VFO (tx_player synthesises there); at 1500 and 2000 it moves the VFO 175 and 675 Hz up for
    the over. Passes with no copy over SPUR_MAX_DBC. w: the power (1 W at most here). judge False: the figures said,
    not judged (the X6200 at 0.1 W, a hundredth of its rating: its base puts a skirt round the signal, -25 dBc in each
    55 Hz out to 170 Hz, -44 at 1 W, 2026-11-25; the decoders take the station, not the copies, since 11-25)"""
    import suite_air_b as ab
    from tones import TciRx
    S, L = o.r[sender], o.r[listener]
    rows, tci, steps = [], None, None
    p, total = bench.chain_gate(o.c, "radios")
    try:
        with o.step(f"both on {ab.FT_DIAL['FT8'] / 1e6:.3f} USB-D, {sender}'s FT8 app open there, Auto off") as s:
            test_calls(o)
            want, checks = bench.rx_profile(ab.FT_DIAL["FT8"], 3, pre=0, att=0, rfg=100, agc=3, sql=0,
                                            filt=(100, 3000))
            o.both(dict(want, pwr=w, atu=0, xit=0, rit=0, split=0), checks)
            o.changing += 1
            try:
                steps = ab.ft8_open(S, "FT8")
            finally:
                o.changing -= 1
            ab.ft8_set(S, "auto", False)
            bench.pair_align(L, S)          # the app sets its own receiver as it opens (AGC off): the listener takes it
            alike(o, ("freq", "mode"))
            s.confirm(f"{sender} {S.state.get('app')} on {S.state.get('freq')}, {listener} on G "
                      f"{L.state.get('freq')}")
        tci = TciRx(L.host)
        bench.radio_tx_gate(S, o.c, w)
        bench.listener_gate(L)
        for txf in txfs:
            with o.step(f"{sender}'s FT8 app: one CQ from TX Hz {txf}, {listener} records") as s:
                S.send(f"ft8 txfreq {txf}")
                S.wait(f"TX Hz {txf}", lambda: S.ft8.get("txf") == txf, 5)
                o.roles = {sender: "TX", listener: "RX"}
                watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(w * 1000) - total, listener=L, w=w,
                                    label=f"ft8 copies {sender} {txf} Hz {w} W")
                try:
                    bench.tx_log({"who": sender, "f": ab.FT_DIAL["FT8"], "w": S.pwr(),
                                  "what": f"orch: FT8 CQ from {txf} Hz"})
                    t0 = time.time()
                    S.send("ft8 cq")
                    S.wait("its CQ on the air", lambda: S.tx or watch.why, 40)
                    S.wait("its CQ over", lambda: not S.tx or watch.why, bench.MAX_KEY_S + 2)
                    t1 = time.time()
                    S.send("ft8 stop")
                finally:
                    watch.close()
                    o.roles = {A: "RX", B: "RX"}
                if watch.why:
                    raise Refused(f"the watch stopped the over: {watch.why}")
                time.sleep(1.0)
                x = tci.audio(t0, t1 + 0.5)[0]
                c = ab.ft8_copies(x, txf)
                if c is None:
                    raise Refused(f"no audio from {listener}")
                c["wav"] = save_wav(o, x, f"ft8_copies_{sender}_to_{listener}_{txf}_{w}W.wav")
                rows.append(dict(c, txf=txf))
                s.confirm(f"at {c['hz']} Hz: {SPUR_HZ} Hz below {c['below_dbc']} dBc, above {c['above_dbc']} dBc, "
                          f"400 Hz above (noise) {c['noise_dbc']} dBc")
                time.sleep(2.0)
    except Refused as e:
        return False, str(e), {"rows": rows}
    finally:
        if tci is not None:
            tci.close()
        if steps is not None:
            o.changing += 1
            try:
                with bench.transition():
                    ab.ft8_close(S, steps)
                    bench.close_apps(S)
                    bench.sync_pair([L, S], {"freq": int(L.state.get("freq") or ab.FT_DIAL["FT8"]),
                                             "mode": int(L.state.get("mode", 3))})
            except Refused as e:
                o.st.ev(f"{sender}'s FT8 app not closed as found: {e}", "bad")
            finally:
                o.changing -= 1
    said = "; ".join(f"TX Hz {r['txf']}: {r['below_dbc']}/{r['above_dbc']} dBc (noise {r['noise_dbc']})" for r in rows)
    # a copy counts where it stands 6 dB over the listener's noise: the X6200's audio is noisier (-32 dBc at 0.1 W
    # through the cable), and a figure at its noise says nothing either way
    bad = [r["txf"] for r in rows if max(r["below_dbc"], r["above_dbc"]) > max(SPUR_MAX_DBC, r["noise_dbc"] + 6)]
    if not judge:
        return None, (f"{sender}'s FT8 CQs at {w} W in {listener}'s audio, the power {SPUR_HZ} Hz below/above each, "
                      f"dB under it: {said}: said, not judged (at {w} W the X6200's base puts this skirt round its "
                      f"signal; judged at 1 W, ft8-copies x6200 1w)"), {"rows": rows}
    return not bad, (f"{sender}'s FT8 CQs at {w} W in {listener}'s audio, the power {SPUR_HZ} Hz below/above each, dB under it: "
                     f"{said}" + (f"; a copy over {SPUR_MAX_DBC} dBc from TX Hz {bad}" if bad else "")), {"rows": rows}


CW_TIMING_TEXT = "PARIS PARIS PARIS PARIS"


def _num(v, spec=".0f"):
    """v as spec, "-" for None"""
    return "-" if v is None else format(v, spec)


def cw_timing_test(o, sender, listener, w=W):
    """The sender's keyer's timing as it reaches the air: CW_TIMING_TEXT keyed through its TCI at 20 WPM (its KEY Speed
    with TCI's speed steps), the listener's audio read element by element and set against the text (cw_timing.py):
    none merged or lost, each mark and gap within 25 ms of its kind's median. The keyer's own account (its GUI's log)
    tells whose an error is: an edge late on its deadline is the GUI's, and fails; with the keyer on time an X6100 on
    Xiegu's factory base is said, not judged (2026-11-25: its base let the carrier run on 30-80 ms after the key went
    up now and then, the keyer's 112 edges within 2 ms; its keyer's sleeps cut short by a signal before, fixed)"""
    import subprocess
    import cw_timing as ct
    import suite_air_a as aa
    import suite_air_b as ab
    from tones import TciRx, peak, FS as TFS
    S, L = o.r[sender], o.r[listener]
    res = {}
    text, wpm, secs = ab.cw_speedup(CW_TIMING_TEXT, ab.key_wpm(S.host), most=14.0)
    tci = TciRx(L.host)

    def body(watch):
        with o.step(f"{sender} keys {text!r} ({wpm} WPM, {secs:.1f} s), {listener} records its audio") as s:
            run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_cw_tx.py"),
                    f"root@{S.host}:/tmp/tci_cw_tx.py"])
            t0 = time.time()
            proc = subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{S.host}",
                                     f"python3 /tmp/tci_cw_tx.py '{text}' {secs + 6:.0f}"], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            while proc.poll() is None:
                if watch.why:
                    S.stop()
                    raise Refused(watch.why)
                time.sleep(0.05)
            time.sleep(1.0)
            res["x"] = tci.audio(t0, time.time())[0]
            s.confirm(f"{len(res['x']) / TFS:.1f} s of audio")
    try:
        over(o, sender, listener, ab.F_CW, 4, f"CW timing {sender}", body, w=w, agc=2)
    except Refused as e:
        return False, str(e), {}
    finally:
        tci.close()
    x = res.get("x")
    if x is None or len(x) < TFS:
        return False, "no audio", {}
    kt = int(L.state.get("kt") or 700)
    pitch, _ = peak(x, kt, 120)
    runs, depth = ct.cw_elements(x, TFS, pitch or kt)
    j = ct.judge(runs, ct.cw_ideal(text), 1200.0 / wpm)
    keyer = ct.keyer_said(bench.ssh, S.host)
    base = aa.base_of(S.host) if sender == "x6100" else ""
    factory = sender == "x6100" and aa.gcfix_of(base) == 0
    data = dict(j, wpm=wpm, pitch=pitch, depth_db=depth, runs=runs, keyer=keyer, base=base,
                wav=save_wav(o, x, f"cw_timing_{sender}_to_{listener}_{w}W.wav"))
    kw = ("its keyer said nothing (a GUI before 11-25)" if keyer is None else
          f"its keyer: {keyer[0]} edges, {keyer[1]} late over 15 ms, the latest {keyer[2]} ms")
    says = (f"{sender}'s {text!r} at {wpm} WPM, {w} W, in {listener}'s audio: {ct.words(j)}; {kw} (key tone "
            f"{_num(pitch)} Hz, {depth} dB deep)")
    if keyer is not None and keyer[1]:
        return False, says + ": the keyer itself late (the GUI's)", data
    if ct.clean(j):
        return True, says, data
    if factory and keyer is not None:
        return None, says + (f": the keyer on time, the carrier not: {sender}'s base, Xiegu's {base or 'factory'} "
                             f"(said, not judged)"), data
    return False, says, data


# ---- The AMBE vocoders (each radio's DVSI chip behind its SDR daemon's port 127.0.0.1:2460) and B's digital voice ----
# Receive only: nothing is keyed. The test speech is this computer's own voice (say), 8 kHz; the calls made of it by
# the repository's generators (tools/dsdfme, tools/rtlspec/dmr_test gen) and replayed into a test SDR daemon on each
# radio (dv_replay.py), as a signal from an SDR would come.

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "../.."))
DV = os.path.join(bench.STATE, "dv")                # what the AMBE tests make: speech, frames, calls, tools
SAY = "Testing the vocoder on the bench. One, two, three, four, five."
P25_FRAMES = os.path.join(TOP, "tools/dsdfme/mbe_dvsi/p25_chip_frames.bin")     # a 1 kHz tone, a USB-3000's P25 frames
VOC_ENV_R, VOC_SPEC_R = 0.90, 0.85      # a chip's speech against what went in (both chips measured 0.96 / 0.94)
# B's chain, the voice heard against the speech (2026-11-24, both radios: DMR 0.97, NXDN48 0.93, YSF 0.81-0.88,
# D-STAR 0.49-0.73: dsd-fme loses D-STAR's sync two or three times in a 5 s call, its voice gone meanwhile; to find)
DV_R = {"dmr": 0.90, "nxdn": 0.85, "ysf": 0.75, "dstar": 0.70}
DV_CALL = {"dmr": "DMR TS1 CC1 TG 3100 FROM 3112345", "dstar": "DSTAR TO CQCQCQ FROM N0CALL",
           "ysf": "YSF TO CQCQCQ FROM N0CALL", "nxdn": "NXDN48 TG 100 FROM 1234 RAN 1",
           "p25": "P25P1 TG 4321 FROM 1234567 NAC 123"}


def run_ok(cmd, env=None, out=None, timeout=300):
    import subprocess
    r = subprocess.run(cmd, capture_output=True, timeout=timeout, env=dict(os.environ, **(env or {})))
    if r.returncode:
        raise Refused(f"{os.path.basename(cmd[0])}: {(r.stderr or r.stdout).decode(errors='replace').strip()[-300:]}")
    if out:
        with open(out, "wb") as f:
            f.write(r.stdout)
    return r.stdout.decode(errors="replace")


def scp_to(host, files, dest="/tmp/"):
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes"] + list(files) + [f"root@{host}:{dest}"], timeout=180)


def scp_from(host, path, dest):
    run_ok(["scp", "-4", "-q", "-o", "BatchMode=yes", f"root@{host}:{path}", dest], timeout=180)


def dv_speech():
    """The test speech, 8 kHz s16, peak 20000 (made once: this computer's say)"""
    os.makedirs(DV, exist_ok=True)
    out = os.path.join(DV, "speech.s16")
    if not os.path.exists(out):
        aiff, wav = os.path.join(DV, "speech.aiff"), os.path.join(DV, "speech8k.wav")
        run_ok(["say", "-v", "Samantha", SAY, "-o", aiff])
        run_ok(["afconvert", "-f", "WAVE", "-d", "LEI16@8000", "-c", "1", aiff, wav])
        import wave
        w = wave.open(wav)
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float)
        (x / np.max(np.abs(x)) * 20000).astype("<i2").tofile(out)
    return out


def dv_tools():
    """The generators' programs, built on this computer when missing or older than their sources; mbe_dvsi_test for
    the radios (ARM, the cross compiler and the sysroot tools/cross_x6200/setup.sh made)"""
    os.makedirs(DV, exist_ok=True)
    sys_ = os.path.join(TOP, "build/x6200/sysroot")
    ma = os.path.join(sys_, "usr/lib/arm-linux-gnueabihf")
    dsd = os.path.join(TOP, "build/x6200/dsdfme/src")
    mb = os.path.join(TOP, "tools/dsdfme/mbe_dvsi")
    want = {
        "dmr_test": (["cc", "-O2", "-o", "{out}", f"{TOP}/tools/rtlspec/dmr_test.c", f"{TOP}/tools/rtlspec/dmr.c", "-lm"],
                     [f"{TOP}/tools/rtlspec/dmr_test.c", f"{TOP}/tools/rtlspec/dmr.c"]),
        "dmr_to_ambed": (["cc", "-O2", "-o", "{out}", f"{TOP}/tools/dsdfme/ysf/dmr_to_ambed.c", f"{mb}/mbe_dvsi.c"],
                         [f"{TOP}/tools/dsdfme/ysf/dmr_to_ambed.c", f"{mb}/mbe_dvsi.c"]),
        "p25_gen": (["c++", "-O2", "-I", f"{dsd}/include", f"{TOP}/tools/dsdfme/p25_gen.cpp", f"{dsd}/src/Hamming.cpp",
                     "-o", "{out}"], [f"{TOP}/tools/dsdfme/p25_gen.cpp"]),
        "mbe_dvsi_test.arm": (["arm-linux-gnueabihf-gcc", f"--sysroot={sys_}", "-isystem",
                               f"{sys_}/usr/include/arm-linux-gnueabihf", f"-L{ma}", f"-Wl,-rpath-link,{ma}", "-O2",
                               "-D_FORTIFY_SOURCE=1", "-static-libgcc", "-o", "{out}", f"{mb}/mbe_dvsi_test.c",
                               f"{mb}/mbe_dvsi.c", "-lm"], [f"{mb}/mbe_dvsi_test.c", f"{mb}/mbe_dvsi.c"]),
    }
    paths = {}
    for name, (cmd, srcs) in want.items():
        out = os.path.join(DV, name)
        if not os.path.exists(out) or any(os.path.getmtime(x) > os.path.getmtime(out) for x in srcs):
            run_ok([c.replace("{out}", out) for c in cmd])
        paths[name] = out
    return paths


def frames_of(path):
    """ambe_port.py's frames file: [(bits, bytes)]"""
    d, i, out = open(path, "rb").read(), 0, []
    while i < len(d):
        n = (d[i] + 7) // 8
        out.append((d[i], d[i + 1:i + 1 + n]))
        i += 1 + n
    return out


def env_r(x, fsx, y, fsy, ms=10):
    """y's loudness against x's (RMS in ms windows), at y's best lag: (r, lag s)"""
    def env(v, fs):
        w = int(fs * ms / 1000)
        n = len(v) // w
        return np.sqrt(np.mean(v[:n * w].reshape(n, w) ** 2, axis=1))
    ex, ey = env(x, fsx), env(y, fsy)
    best = (-1.0, 0)
    for L in range(0, max(1, len(ey) - int(0.8 * len(ex)) + 1)):    # y later than x by L windows, 80 % overlap at least
        seg = ey[L:L + len(ex)]
        if len(seg) < 0.8 * len(ex):
            break
        r = float(np.corrcoef(ex[:len(seg)], seg)[0, 1])
        if r > best[0]:
            best = (r, L)
    return best[0], best[1] * ms / 1000


def spec_r(x, y, fs=8000):
    """The log spectra (20 ms, 300-3400 Hz) of x and y (aligned, the same rate) correlated"""
    w = fs // 50
    n = min(len(x), len(y)) // w
    def sp(v):
        f = np.abs(np.fft.rfft(v[:n * w].reshape(n, w) * np.hanning(w), axis=1))
        b = np.fft.rfftfreq(w, 1 / fs)
        return np.log10(f[:, (b >= 300) & (b <= 3400)] + 1)
    return float(np.corrcoef(sp(x).ravel(), sp(y).ravel())[0, 1])


def tone_frames(y, fs, hz=1000):
    """20 ms frames of y louder than 300 RMS whose 300-3400 Hz energy is mostly within 50 Hz of hz"""
    w = fs // 50
    n = len(y) // w
    fr = y[:n * w].reshape(n, w)
    F = np.abs(np.fft.rfft(fr * np.hanning(w), axis=1)) ** 2
    b = np.fft.rfftfreq(w, 1 / fs)
    share = F[:, (b >= hz - 50) & (b <= hz + 50)].sum(1) / (F[:, (b >= 300) & (b <= 3400)].sum(1) + 1e-9)
    loud = np.sqrt(np.mean(fr ** 2, axis=1)) > 300
    return int(np.sum(loud & (share > 0.5))), int(np.sum(loud))


def chip_ids(o):
    return in_parallel(o, lambda n, r: bench.ssh(r.host, "python3 /tmp/ambe_port.py id").split("\n")[0].strip(), 30)


def parity(o, want=None):
    """Both chips' parity field (PKT_PARITYMODE) set to want ("on"/"off") or read: {name: "parity on"/"parity off"}"""
    return in_parallel(o, lambda n, r: bench.ssh(r.host, "python3 /tmp/ambe_port.py parity" +
                                                 (f" {want}" if want else "")).strip(), 30)


def t_ambe_vocoder(o):
    """Each radio's AMBE-3000 through its SDR daemon's port, the two at once: the daemon has it send its packets'
    parity field (the DVMega stick's AMBE3000F sends none until asked: parsed as if it did, a frame ending in 0x2F lost
    two bytes); the test speech through each chip at DMR's rate (33: YSF's and NXDN's too) and D-STAR's and back, and
    each chip's frames through the other; libmbe-dvsi (dsd-fme's and the GUI's) alike with the chip's parity field on
    and off; P25 phase 1's full rate where the chip has DVSI's P25 option, silence where it has not"""
    got = {}
    tools = dv_tools()
    sp = dv_speech()
    x = np.fromfile(sp, "<i2").astype(float)
    work = os.path.join(o.st.run_dir or DV, "ambe")
    os.makedirs(work, exist_ok=True)
    try:
        with o.step("each radio's AMBE chip answers through its SDR daemon's port 127.0.0.1:2460") as s:
            def push(n, r):
                scp_to(r.host, [os.path.join(HERE, "ambe_port.py"), sp, P25_FRAMES, tools["mbe_dvsi_test.arm"]])
                bench.ssh(r.host, "chmod 755 /tmp/mbe_dvsi_test.arm")
            in_parallel(o, push, 120)
            ids = chip_ids(o)
            got["chips"] = ids
            bad = [f"{n}: {v!r}" for n, v in ids.items() if not v.startswith("AMBE")]
            if bad:
                raise Refused(f"no AMBE chip: {'; '.join(bad)}")
            s.confirm(", ".join(f"{n} {v}" for n, v in ids.items()))
        with o.step("the SDR daemon asks for the parity field when it takes the chip (off, then the daemon restarted)") as s:
            off = parity(o, "off")
            if any(v != "parity off" for v in off.values()):
                raise Refused(f"the parity field not turned off: {off}")

            def restart(n, r):
                old = bench.ssh(r.host, "pidof x6x00-rtlspec || true").strip()
                bench.ssh(r.host, "killall x6x00-rtlspec 2>/dev/null; true")
                t0 = time.time()
                while time.time() - t0 < 60:        # the GUI starts it again within seconds; then it takes the chip
                    time.sleep(2)                   # (ambe_port.py gives up on a silent port in a second or two)
                    pid = bench.ssh(r.host, "pidof x6x00-rtlspec || true").strip()
                    if pid and pid != old and "AMBE" in bench.ssh(
                            r.host, "python3 /tmp/ambe_port.py id 2>/dev/null | head -1 || true"):
                        return f"{pid} after {time.time() - t0:.0f} s"
                raise Refused("the daemon not back with the chip within 60 s")
            pids = in_parallel(o, restart, 90)
            on = parity(o)
            got["parity_after_restart"] = on
            if any(v != "parity on" for v in on.values()):
                raise Refused(f"after the daemon took the chip again: {on}")
            s.confirm(f"daemons {pids}: {on}")
        for rate in ("33", "dstar"):
            with o.step(f"the speech through each chip and back at {'DMR' if rate == '33' else 'D-STAR'}'s rate "
                        f"(envelope r >= {VOC_ENV_R}, spectrum r >= {VOC_SPEC_R})") as s:
                def rt(n, r, rate=rate):
                    bench.ssh(r.host, f"cd /tmp && python3 ambe_port.py enc {rate} speech.s16 {rate}.frames && "
                                      f"python3 ambe_port.py dec {rate} {rate}.frames {rate}.dec.s16", timeout=120)
                    for f in (f"{rate}.frames", f"{rate}.dec.s16"):
                        scp_from(r.host, f"/tmp/{f}", os.path.join(work, f"{n}_{f}"))
                    fr = frames_of(os.path.join(work, f"{n}_{rate}.frames"))
                    odd = sum(1 for b, _ in fr if b != 72)
                    y = np.fromfile(os.path.join(work, f"{n}_{rate}.dec.s16"), "<i2").astype(float)
                    re, lag = env_r(x, 8000, y, 8000)
                    rs = spec_r(x, y[int(lag * 8000):])
                    res = {"frames": len(fr), "not_72_bits": odd, "env_r": round(re, 3), "spec_r": round(rs, 3),
                           "delay_ms": round(lag * 1000)}
                    if odd or len(fr) < len(x) // 160 or re < VOC_ENV_R or rs < VOC_SPEC_R:
                        raise Refused(str(res))
                    return res
                res = in_parallel(o, rt, 180)
                got[f"roundtrip_{rate}"] = res
                s.confirm("; ".join(f"{n} r {v['env_r']}/{v['spec_r']}, {v['frames']} frames" for n, v in res.items()))
            with o.step(f"each chip decodes the other's {'DMR' if rate == '33' else 'D-STAR'} frames as well") as s:
                def cross(n, r, rate=rate):
                    other = B if n == A else A
                    scp_to(r.host, [os.path.join(work, f"{other}_{rate}.frames")])
                    bench.ssh(r.host, f"cd /tmp && python3 ambe_port.py dec {rate} {other}_{rate}.frames "
                                      f"{rate}.cross.s16", timeout=120)
                    scp_from(r.host, f"/tmp/{rate}.cross.s16", os.path.join(work, f"{n}_{rate}.cross.s16"))
                    y = np.fromfile(os.path.join(work, f"{n}_{rate}.cross.s16"), "<i2").astype(float)
                    re, lag = env_r(x, 8000, y, 8000)
                    rs = spec_r(x, y[int(lag * 8000):])
                    if re < VOC_ENV_R or rs < VOC_SPEC_R:
                        raise Refused(f"the other's frames: envelope r {re:.3f}, spectrum r {rs:.3f}")
                    return {"env_r": round(re, 3), "spec_r": round(rs, 3)}
                res = in_parallel(o, cross, 180)
                got[f"cross_{rate}"] = res
                s.confirm("; ".join(f"{n} r {v['env_r']}/{v['spec_r']}" for n, v in res.items()))
        with o.step("libmbe-dvsi (dsd-fme's, the GUI's DMR) gives the same speech with the chip's parity field on and off") as s:
            def frames9(n):
                return b"".join(b for _, b in frames_of(os.path.join(work, f"{n}_33.frames")))

            def shim(n, r, tag):
                open(os.path.join(work, f"{n}_dmr9.ambe"), "wb").write(frames9(n))
                scp_to(r.host, [os.path.join(work, f"{n}_dmr9.ambe")])
                said = bench.ssh(r.host, f"cd /tmp && ./mbe_dvsi_test.arm {n}_dmr9.ambe shim_{tag}.s16 shim_{tag}_e.s16",
                                 timeout=120).strip()
                return bench.ssh(r.host, f"md5sum /tmp/shim_{tag}.s16").split()[0], said
            try:
                parity(o, "on")
                on = in_parallel(o, lambda n, r: shim(n, r, "on"), 180)
                parity(o, "off")
                off = in_parallel(o, lambda n, r: shim(n, r, "off"), 180)
            finally:
                parity(o, "on")
            got["shim"] = {"on": on, "off": off}
            for n in o.r:
                scp_from(o.r[n].host, "/tmp/shim_off.s16", os.path.join(work, f"{n}_shim_off.s16"))
                y = np.fromfile(os.path.join(work, f"{n}_shim_off.s16"), "<i2").astype(float)
                re, _ = env_r(x, 8000, y, 8000)
                if on[n][0] != off[n][0] or re < VOC_ENV_R:
                    raise Refused(f"{n}: parity on {on[n][0][:8]}, off {off[n][0][:8]}, speech r {re:.3f}")
            s.confirm("; ".join(f"{n} the same speech both ways ({on[n][1]})" for n in o.r))
        with o.step("P25 phase 1's full rate through libmbe-dvsi: a 1 kHz tone from a chip with DVSI's P25 option, "
                    "silence from one without") as s:
            def p25(n, r):
                said = bench.ssh(r.host, "cd /tmp && ./mbe_dvsi_test.arm p25 p25_chip_frames.bin p25.s16", timeout=60)
                scp_from(r.host, "/tmp/p25.s16", os.path.join(work, f"{n}_p25.s16"))
                y = np.fromfile(os.path.join(work, f"{n}_p25.s16"), "<i2").astype(float)
                tone, loud = tone_frames(y, 8000)
                has = "P25" in got["chips"][n]
                if has and tone < 20 or not has and loud:
                    raise Refused(f"{got['chips'][n]}: {tone} frames of 1 kHz, {loud} loud ({said.strip()})")
                return {"p25_option": has, "tone_frames": tone, "loud_frames": loud}
            res = in_parallel(o, p25, 90)
            got["p25"] = res
            s.confirm("; ".join(f"{n} {'1 kHz in ' + str(v['tone_frames']) + ' of 30 frames' if v['p25_option'] else 'silent (no P25 option)'}"
                                for n, v in res.items()))
        with o.step("both chips left sending their parity field, as their daemons set them") as s:
            on = parity(o)
            if any(v != "parity on" for v in on.values()):
                raise Refused(str(on))
            s.confirm(str(on))
    except Refused as e:
        return False, str(e), got
    return True, ("both chips: " + ", ".join(f"{n} {v}" for n, v in got["chips"].items()) + "; DMR and D-STAR speech "
                  "through each and the other's, libmbe-dvsi alike with the parity field off, P25 where the chip has "
                  "it"), got


def dv_calls(o, work):
    """The five calls as I/Q (2 MS/s int8, 200 kHz above centre) from the test speech: its DMR and D-STAR frames
    made by the first radio's chip; {mode: path}"""
    tools = dv_tools()
    sp = dv_speech()
    h = o.r[A].host
    scp_to(h, [os.path.join(HERE, "ambe_port.py"), sp])
    bench.ssh(h, "cd /tmp && python3 ambe_port.py enc 33 speech.s16 dv33.frames && "
                 "python3 ambe_port.py enc dstar speech.s16 dvdstar.frames", timeout=120)
    for f in ("dv33.frames", "dvdstar.frames"):
        scp_from(h, f"/tmp/{f}", os.path.join(work, f))
    for f, out in (("dv33.frames", "dmr9.ambe"), ("dvdstar.frames", "dstar9.ambe")):
        fr = frames_of(os.path.join(work, f))
        if any(b != 72 for b, _ in fr):
            raise Refused(f"{f}: frames of another length")
        open(os.path.join(work, out), "wb").write(b"".join(b for _, b in fr))
    n = len(frames_of(os.path.join(work, "dv33.frames")))
    d = os.path.join(TOP, "tools/dsdfme")
    w = lambda f: os.path.join(work, f)         # noqa: E731
    run_ok([tools["dmr_test"], "gen", w("dmr9.ambe"), w("dmr.dibits")])
    run_ok([sys.executable, f"{d}/p25_mod.py", "iq", w("dmr.dibits"), w("dmr.iq"), "200000"],
           env={"SHAPE": "rrc", "DEV": "648"})
    run_ok([sys.executable, f"{d}/dstar_gen.py", "iq", w("dstar.iq"), "200000", "6"], env={"DSTAR_VOICE": w("dstar9.ambe")})
    run_ok([tools["dmr_to_ambed"], w("dmr9.ambe")], out=w("v.ambed"))
    run_ok([sys.executable, f"{d}/ysf/ysf_gen.py", w("v.ambed"), str((n + 4) // 5)], out=w("ysf.dibits"))
    run_ok([sys.executable, f"{d}/p25_mod.py", "iq", w("ysf.dibits"), w("ysf.iq"), "200000"], env={"SHAPE": "rrc"})
    run_ok([sys.executable, f"{d}/nxdn/nxdn_gen.py", w("dmr9.ambe"), str(n)], out=w("nxdn.dibits"))
    run_ok([sys.executable, f"{d}/p25_mod.py", "iq", w("nxdn.dibits"), w("nxdn.iq"), "200000"],
           env={"BAUD": "2400", "DEV": "350", "SHAPE": "rrc"})
    run_ok([tools["p25_gen"], P25_FRAMES, "8"], out=w("p25.dibits"))
    run_ok([sys.executable, f"{d}/p25_mod.py", "iq", w("p25.dibits"), w("p25.iq"), "200000"])
    return {m: w(f"{m}.iq") for m in ("dmr", "dstar", "ysf", "nxdn", "p25")}


def dv_replay(o, n, iq, wav):
    """iq replayed into a test SDR daemon on radio n (dv_replay.py, 1.0584 MS/s): (calls said, the audio, its rate)"""
    import subprocess
    import wave
    r = subprocess.run([sys.executable, os.path.join(HERE, "dv_replay.py"), o.r[n].host, iq, "--rate", "1058400",
                        "--secs", "12", "--wav", wav], capture_output=True, text=True, timeout=240)
    calls = [l[5:] for l in r.stdout.splitlines() if l.startswith("CALL ")]
    if not os.path.exists(wav):
        raise Refused(f"no audio from the test daemon: {(r.stdout + r.stderr).strip()[-200:]}")
    w = wave.open(wav)
    return calls, np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float), w.getframerate()


_DV_CALLS = {}          # the calls made this process: {run dir: {mode: path}} (each dv test makes them once a run)


def dv_voice(o, m, parity_off=False):
    """B's digital voice on both radios, each through its own AMBE chip: a call of mode m (DMR, D-STAR, YSF, NXDN48,
    P25 phase 1) made of the test speech, replayed into a test SDR daemon beside B's (no SDR needed): the call shown
    with its IDs (DV_CALL) and its voice heard (against the speech: DV_R); P25's 1 kHz frames heard where the chip has
    DVSI's P25 option and silent where not (libmbe-dvsi's rule). parity_off: with the chips' parity field off (the
    test daemon's reading of the chip's answers through B's daemon's port), put back on after"""
    got = {}
    x = np.fromfile(dv_speech(), "<i2").astype(float)
    work = os.path.join(o.st.run_dir or DV, "dv")
    os.makedirs(work, exist_ok=True)
    try:
        with o.step(f"the {m.upper()} call made from the test speech (its voice frames by {A}'s chip)") as s:
            if work not in _DV_CALLS:
                _DV_CALLS[work] = dv_calls(o, work)
            iq = _DV_CALLS[work][m]
            ids = chip_ids(o)
            got["chips"] = ids
            s.confirm(f"{os.path.getsize(iq) / 4e6:.1f} s of I/Q; chips {ids}")
        what = f"{m.upper()} replayed into both radios' SDR" + (" with the chips' parity field off" if parity_off else "")
        with o.step(f"{what}: the call ({DV_CALL[m]}) and its voice") as s:
            def one(n, r):
                said, y, fs = dv_replay(o, n, iq, os.path.join(work, f"{n}_{m}{'_nopar' if parity_off else ''}.wav"))
                want = DV_CALL[m]
                seen = [c for c in said if want in c]
                if not seen:
                    raise Refused(f"no call '{want}' ({said[:3]})")
                if m == "p25":
                    tone, loud = tone_frames(y, fs)
                    if "P25" in ids[n]:
                        if tone < 100:
                            raise Refused(f"{tone} frames of its 1 kHz (of {loud} loud), 100 wanted")
                        return {"call": seen[0], "tone_frames": tone}
                    if tone:
                        raise Refused(f"{ids[n]} (no P25 option): {tone} frames of a tone, silence wanted")
                    return {"call": seen[0], "silent": True}
                r_, lag = env_r(x, 8000, y, fs)
                if r_ < DV_R[m]:
                    raise Refused(f"the voice against the speech r {r_:.3f} (>= {DV_R[m]} wanted)")
                return {"call": seen[0], "voice_r": round(r_, 3)}
            try:
                if parity_off:
                    p = parity(o, "off")
                    if any(v != "parity off" for v in p.values()):
                        raise Refused(f"the parity field not turned off: {p}")
                res = in_parallel(o, one, 300)
            finally:
                if parity_off:
                    parity(o, "on")
            got[m] = res
            s.confirm("; ".join(f"{n} " + (f"voice r {v['voice_r']}" if "voice_r" in v else
                                           f"{v['tone_frames']} frames of 1 kHz" if "tone_frames" in v
                                           else "silent (no P25 option)") for n, v in res.items()))
        if parity_off:
            with o.step("both chips' parity field on again") as s:
                p = parity(o)
                if any(v != "parity on" for v in p.values()):
                    raise Refused(str(p))
                s.confirm(str(p))
    except Refused as e:
        return False, str(e), got
    return True, f"{m.upper()}: " + "; ".join(f"{n} {v['call']}, " + (f"voice r {v['voice_r']}" if "voice_r" in v else
                                                                   f"{v['tone_frames']} frames of 1 kHz" if
                                                                   "tone_frames" in v else "silent")
                                              for n, v in got[m].items()), got


class Test:
    def __init__(self, name, role, what, fn, est_s, suite=None, group=None):
        self.name, self.role, self.what, self.fn, self.est_s = name, role, what, fn, est_s
        self.suite = suite      # (module, t_name, check): one of the 1:1 suite's checks (suite_pair)
        self.group = group      # (module, t_name, name): a suite batch run whole (suite_group), its rows its own


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
    Test("caps", "both RX", "every option probed on both at once (frequencies, modes, filters, IF shift, zoom, steps, "
         "AGC, NB, NR): what each takes, the differences mapped for the tests", t_caps, 120),
    Test("bands-all", "both RX", "every band both radios share (same edges), entered on both at once: same frequency "
         "and mode", t_bands_all, 60),
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
    Test("cw-quiet", "both RX", "CW 14.040, nothing sent: both from USB-D to CW three times and a stay in CW, 12 s of "
         "the cable's quiet each: neither decoder prints a character (their audio kept for tools/cw_replay)",
         t_cw_quiet, 120),
    Test("cw-noise-tone", "both RX", "the X6200's CW resonance at another key tone (600 Hz, the remote's kt, put back "
         "after): the noise peaks there, as the decoder's colour takes it",
         t_cw_noise_tone, 90),
    Test("classic-wf3d", "both RX", "the classic screen's waterfall in 3D (upstream's drawing thread) and 2D on both: "
         "lit and moving in each, the GUI's CPU said, the GUI still the same process; the screen and keys put back",
         t_classic_wf3d, 90),
    Test("cw-quiet wide", "both RX", "cw-quiet through a wide CW filter, 100-1300 Hz (the X6200's resonance taken out "
         "down to 150 Hz, the X6100's noise rising under 300 Hz): neither decoder prints a character",
         lambda o: t_cw_quiet(o, (100, 1300)), 120),
    Test("cw-noise-shape", "both RX", "CW 14.040, nothing sent: the receive noise's shape through five CW filters and CW-R on both "
         "(over the band's 25th percentile, the decoder's floor): measured, said", t_cw_noise_shape, 70),
    *pair("power-steps", "", "USB 14.200: a tone at 0.2, 0.5, 1.0 W; the other's level must rise 4.0 and 3.0 dB (2 dB)",
          lambda s, l, n: (lambda o: power_steps(o, s, l, 14200000, n)), 45),
    *pair("power-6m", "", "USB 50.200: a tone at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB): "
          "no floor under 1 W (the factory X6100's 0.8 W at 0.1 W)",
          lambda s, l, n: (lambda o: power_steps(o, s, l, 50200000, n, POWER_6M)), 45),
    *pair("power-6m-fm", "", "FM 52.525 with no audio: the carrier at 0.1, 0.3, 1.0 W; the other's level must rise "
          "4.8 and 5.2 dB (2 dB), or, from the X6100 (its base's ~1 W floor there, FLOOR_6M), stay alike with the "
          "GUI's note of it in the log", lambda s, l, n: (lambda o: power_steps(o, s, l, 52525000, n, POWER_6M, mode=7)),
          45),
    *pair("power-6m-am", "", "AM 50.400 with no audio: the carrier at 0.1, 0.3, 1.0 W; as power-6m-fm",
          lambda s, l, n: (lambda o: power_steps(o, s, l, 50400000, n, POWER_6M, mode=6)), 45),
    *pair("ft8-cq", "", "FT8 14.074: a CQ in the next slot, the other must decode exactly the message sent",
          lambda s, l, n: (lambda o: ft8_cq(o, s, l, n)), 40),
    *pair("psk31-text", "", "PSK31 on the listener's app dial: 'CQ CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % "
          "of the characters wrong", lambda s, l, n: (lambda o: digi_text(o, s, l, "psk31", n)), 45),
    *pair("rtty-text", "", "RTTY made to the listener's own settings: 'RYRY CQ DE Q1TST Q1TST K' sent, decoded with at "
          "most 5 % of the characters wrong", lambda s, l, n: (lambda o: digi_text(o, s, l, "rtty", n)), 120),
    *pair("wefax-chart", "", "WeFax: 12 s of a half-white, half-black chart; the other's saved fax must show the two "
          "halves at least 80 grey levels apart", lambda s, l, n: (lambda o: wefax_chart(o, s, l, n)), 60),
    *pair("tune-carrier", "", "TUNE at 0.1 W: on the air, heard 15 dB up, off by itself 2.5-5 s after the lease's last "
          "renewal", lambda s, l, n: (lambda o: tune_carrier(o, s, l, n)), 25),
    *pair("ft8-qso", "", "FT8: a whole QSO by the apps (Auto), logged on both, one ADIF record each (taken out after)",
          lambda s, l, n: (lambda o: ft8_qso(o, s, l, n)), 180),
    *pair("ft8-stop", "", "FT8: ft8 stop 2 s into a CQ takes the sender off the air within 1.5 s (its state's word), "
          "not again",
          lambda s, l, n: (lambda o: ft8_stop(o, s, l, n)), 35),
    Test("tci-control", "both RX", "TCI vfo and modulation on both radios' own TCI: each confirmed by its own state, "
         "the two alike", t_tci_control, 20),
    Test("rigctl", "both RX", "rigctl F/f M/m on both: confirmed by each radio's state, alike; its PTT refused",
         t_rigctl, 15),
    Test("dial-lock", "both RX", "the dial lock on both: a set refused with the dial kept; off, the set taken",
         t_dial_lock, 15),
    Test("remote-security", "both", "a stranger's token: watch only, its set/PTT/knob/control/TUNE/FT8 each refused",
         t_remote_security, 15),
    Test("rx-session-refused", "both", "the receive-only session's PTT, TUNE and FT8 CQ refused, nothing keyed",
         t_rx_session_refused, 15),
    Test("apps-alike", "both RX", "JS8, Olivia, SSTV, WeFax, NavTex, PSK each opened on both at once: the same dial",
         t_apps_alike, 60),
    *pair("ft8-reply", "", "FT8: a CQ, the other decodes it and replies; the first must decode exactly the reply",
          lambda s, l, n: (lambda o: ft8_reply(o, s, l, n)), 60),
    Test("remote-busy x6100", "X6100 TX, X6200 RX", "while the X6100 is on the air another session cannot take "
         "control, the X6200 never keyed", lambda o: remote_busy(o, A, B, "remote-busy x6100"), 20),
    Test("remote-busy x6200", "X6200 TX, X6100 RX", "while the X6200 is on the air another session cannot take "
         "control, the X6100 never keyed", lambda o: remote_busy(o, B, A, "remote-busy x6200"), 20),
    Test("band-memory", "both RX", "each band its own ATT: 20 m on, 15 m off, each back from its memory on both",
         t_band_memory, 20),
    Test("cb-channel", "both RX", "CB 19 as G's channel, then a channel up: 27.205 AM on both", t_cb_channel, 15),
    *pair("js8-text", "", "JS8: a directed text, the other's list must show it from the sender with the text exact",
          lambda s, l, n: (lambda o: js8_text(o, s, l, n)), 90),
    *pair("sstv-picture", "", "SSTV: a Robot 8 picture (two white quarters, two black) made by the GUI's own encoder; "
          "the other's saved picture must show the white quarters 80 grey levels over the black",
          lambda s, l, n: (lambda o: sstv_picture(o, s, l, n)), 60),
    *pair("olivia-text", "", "Olivia: the sender's app sends a macro 'TEST'; the other's text log must show it in the "
          "variant sent", lambda s, l, n: (lambda o: olivia_text(o, s, l, n)), 75),
    *pair("cw-kbd", "", "CW Keyboard 14.040: a stored message 'VVV VVV Q1TST' keyed by the app; the other's decoder must "
          "print Q1TST", lambda s, l, n: (lambda o: cw_kbd(o, s, l, 14040000, n)), 45),
    Test("pages-look", "both RX", "the Sentry's pages A-H on both, photographed together: the Sentry's colours on each, "
         "the two alike", pages_look, 90),
    Test("comp-mic x6100", "X6100 TX (its mic), X6200 RX (its speaker plays)", "Comp through the X6100's mic: the X6200's "
         "speaker (at 60) plays loud and soft steps 20 dB apart every 0.15 s, the X6100 keyed with PTT on G at Comp 1:1 "
         "and 4:1, its capture of each over (its monitor) read: 4:1 at least 3 dB narrower (on Xiegu's factory base: "
         "said; 11-25 none, 16.7 against 16.0 dB). The X6200's capture holds no monitor of its over: its Comp is not "
         "measured this way", lambda o: comp_mic_test(o, A, B, vol=60), 90),
    Test("g-voiceprint x6100>x6200", "X6100 TX (its mic), X6200 RX (its speaker plays)", "G's voiceprint on the air: "
         "the X6200 plays stepped tones on its speaker, the X6100 keyed on G at 0.1 W picks them up on its mic; its "
         "voiceprint (two pictures), its own capture and the X6200's receiver must show them",
         lambda o: g_voiceprint(o, A, B, "g-voiceprint x6100>x6200"), 45),
    *pair("ext-tune", "", "TUNE for an external tuner from the FT8 app's Ext tune at 0.1 W: a tone at its TX offset "
          "in the other's audio, off on the second press, the app and dial kept, off by its time limit",
          lambda s, l, n: (lambda o: ext_tune_app(o, s, l, n)), 60),
    Test("js8-close-to-g", "both RX", "JS8 closed back to G on CB 20 AM, five times on both: no GUI dies (the X6200's "
         "SIGSEGV of 20:36)", t_js8_close_to_g, 90),
    Test("system-health", "both", "battery log fresh, clock within 1.5 s, the data partition read-write, on both",
         t_system_health, 10),
    Test("rx-vfo-split", "both RX", "VFO B and back, split, RIT and XIT at +200/-200/0: each set on both, read back "
         "alike", t_rx_vfo_split, 30),
    Test("tci-receiver", "both RX", "TCI's filter, AGC, RIT, a VFO in capitals and an unknown command on both radios' "
         "TCI: each confirmed by the radio's state, the two alike", t_tci_receiver, 30),
    Test("rigctl-more", "both RX", "rigctl's levels, split (its setting refused) and state dump on both, nothing keyed",
         t_rigctl_more, 20),
    Test("remote-apps", "both RX", "the remote's apps list the same on both (more than ten); WSPR's opening refused on "
         "both", t_remote_apps, 20),
    Test("power-cap", "both RX", "20 W asked of both: each reads its own top (X6100 10 W, X6200 8 W, 5 W on a "
         "battery), nothing keyed, 0.1 W after", t_power_cap, 15),
    Test("smc-remote", "both RX", "TX Cal's S-meter Measure from the remote on both: its figures read, a Measure on "
         "the noise refused with nothing changed, PRE put a dB off and back, each read back", t_smc_remote, 30),
    Test("web-pages", "both", "every page and GET API of the web configurator: answered alike on both, none with a "
         "server error", t_web_pages, 120),
    *[Test(f"tci-{k} {n}", "both RX", w.format(n=n.upper()), (lambda o, k=k, n=n: tci_refused(o, n, B if n == A else A,
                                                                                            k, f"tci-{k} {n}")), 15)
      for k, w in (("out-of-band", "a TCI audio key from a program on the {n} on 15.000 MHz, armed: refused, off the air"),
                   ("ask", "a network TCI client's trx on the {n}, armed: asked on its screen, not keyed; the question "
                           "gone ~20 s after the client leaves"),
                   ("cw-usb", "CW text through the {n}'s TCI in USB, armed: refused, off the air, nobody asked"),
                   ("key-cw", "a TCI audio key on the {n} in CW, armed: refused, off the air"))
      for n in (A, B)],
    *[Test(name, "both", f"the 1:1 suite's {name} ({t_name[2:].replace('_', ' ')}) on both radios at once: {what}",
           (lambda o, t_name=t_name, name=name: suite_pair(o, "suite_proto", t_name, name)), 30,
           suite=("suite_proto", t_name, name))
      for t_name, name, what in SUITE_PORTED],
    *[Test(name, "both RX", f"the 1:1 suite's cover_a {name} on both radios at once (it puts back what it changes): "
                            f"{what}", (lambda o, t_name=t_name, name=name: suite_pair(o, "suite_cover_a", t_name,
                                                                                        name, alone=name == "screensaver")),
           150 if name == "screensaver" else 45, suite=("suite_cover_a", t_name, name))
      for t_name, name, what in SUITE_PORTED_COVER_A],
    *[Test(name, "both RX", f"the 1:1 suite's {module[6:]} {name} on both radios at once: {what}",
           (lambda o, module=module, t_name=t_name, name=name: suite_pair(o, module, t_name, name)), 45,
           suite=(module, t_name, name))
      for module, t_name, name, what in suite_more_ported()],
    *[Test(f"air {name}", "both ways (each direction: one sends 0.1 W, the other listens)",
           f"the 1:1 suite's {module[6:]} {name} over the air, run whole: {what}",
           (lambda o, module=module, t_name=t_name, name=name: suite_group(o, module, t_name, name)), 120,
           group=(module, t_name, name))
      for module, t_name, name, what in air_groups()],
    *[Test(f"air paths {part}", role, what, (lambda o, part=part: suite_group(
        o, "suite_more", "paths", f"paths {part}", call=lambda m, run: m.paths(run, (part,)))), est,
        group=("suite_more", "paths", f"paths {part}"))
      for part, role, what, est in (
          ("bands", "both ways", "an over on every band both radios share, 160 to 6 m, each way: heard, the receiver's "
                                 "level per band (suite_more band_proofs)", 600),
          ("walkie", "both ways", "the walkie in AM and FM on 20 m and 10 m FM, each way (bench.py radios-walkie)", 400),
          ("cw", "both ways", "CW text through TCI each way ('VVV TEST DE <test call>'): the listener's decoder copies "
                              "the words (suite_more cw_over)", 180),
          ("if-shift", "X6200 TX, X6100 RX", "the X6100's IF shift at +-2000 Hz: the tone's pitch kept, the base "
                                             "2.1 kHz the other way (ifshift.py)", 120))],
    Test("system-checks", "both", "each radio read over SSH, nothing changed: battery log and model, data partition, "
         "settings' good copy, password logins refused, Tailscale, logs, start trace, services and ports, CPU, "
         "temperature, GPU, sessions file, update state, ADIF header, libft8, clock; the same version on both",
         t_system_checks, 60),
    Test("image x6100", "X6100 TX, X6200 RX (1500 Hz below, declared)", "the X6100's opposite-sideband image and "
         "carrier leak on 20, 17, 15, 12 and 10 m at 0.1 W: each image -35 dBc or lower (its 15 m shifts, 11-22); the "
         "carrier leak said",
         lambda o: image_test(o, A, B), 120),
    Test("image x6200", "X6200 TX, X6100 RX (1500 Hz below, declared)", "the X6200's image and carrier leak, as "
         "image x6100", lambda o: image_test(o, B, A), 120),
    Test("spurs x6200", "X6200 TX, X6100 RX", "what comes with the X6200's clean 1500 Hz tone in the X6100's audio "
         "(USB-D, the FT8 dial), the X6100's AGC on and off, the tone at 0.3 and 0.9 of full scale: nothing 156.25 Hz either side over -30 dBc (the X6100's "
         "FT8 app decoded the X6200's answers 156 Hz off, 11-23 to 11-25)", lambda o: spur_test(o, B, A), 90),
    Test("skirt x6200 levels", "X6200 TX, X6100 RX", "the X6200's skirt at 0.1 W against its TCI audio's level: a tone at 0.1 "
         "to 0.95 of full scale up, then down again (a level its TCI learns over the overs told from the level itself), "
         "the power 100-155 Hz off it per 55 Hz: said, not judged (11-25: -31 dBc at 0.3, -55 at 0.9)",
         lambda o: spur_test(o, B, A, runs=tuple((3, a) for a in (0.1, 0.2, 0.4, 0.6, 0.8, 0.95, 0.8, 0.6, 0.4, 0.2,
                                                                  0.1)), judge=False), 240),
    Test("spurs x6100", "X6100 TX, X6200 RX", "the same the other way: the X6100's tone in the X6200's audio",
         lambda o: spur_test(o, A, B), 90),
    Test("tci-tail x6200", "X6200 TX, X6100 RX", "a TCI over's end on the air: the X6200's TCI plays 2 s of 1000 Hz "
         "and unkeys at once (WSJT-X's way), the X6100 must hear all of it; then with 1.5 s of silence after it "
         "(the owner's ask, 11-24: the X6200 dropped 1.2-1.4 s, the X6100 0.3-0.5 s, and the bench pads 0.8 s)",
         lambda o: tail_test(o, B, A), 90),
    Test("tci-tail x6100", "X6100 TX, X6200 RX", "the same the other way: the X6100's TCI over in the X6200's audio",
         lambda o: tail_test(o, A, B), 90),
    Test("ft8-copies x6200", "X6200 TX, X6100 RX", "the X6200's FT8 app's CQs from TX Hz 1325 (no VFO move), 1500 and "
         "2000 at 0.1 W in the X6100's audio, the power 156.25 Hz either side: said, not judged (its base's skirt at "
         "0.1 W, -25 dBc; the X6100's decoder took a copy in it for the station till 11-25)",
         lambda o: ft8_copy_test(o, B, A, judge=False), 150),
    Test("ft8-copies x6200 1w", "X6200 TX at 1 W, X6100 RX", "the X6200's FT8 CQ from 1500 Hz at 1 W: nothing 156.25 Hz "
         "either side over -30 dBc (-44 on 11-25, against -25 at 0.1 W)", lambda o: ft8_copy_test(o, B, A, (1500,), 1.0),
         90),
    Test("ft8-copies x6200 tci", "X6200 TX, X6100 RX", "the same FT8 message through the X6200's TCI (made here): whether "
         "its skirt is the FT8 waveform through its base or the app's way out (said)",
         lambda o: ft8_tci_copy_test(o, B, A), 90),
    Test("ft8-copies x6200 tci app", "X6200 TX, X6100 RX", "ft8-copies x6200 tci with its FT8 app open meanwhile: "
         "whether the app's waterfall, decoder and capture are what disturb its audio (said)",
         lambda o: ft8_tci_copy_test(o, B, A, app=True), 120),
    Test("ft8-copies x6100", "X6100 TX, X6200 RX", "the same the other way: the X6100's FT8 app's CQs in the X6200's "
         "audio", lambda o: ft8_copy_test(o, A, B), 150),
    Test("cw-timing x6100", "X6100 TX, X6200 RX", "the X6100's CW keyer as it reaches the air: PARIS x4 at 20 WPM through "
         "its TCI, read element by element in the X6200's audio: none merged or lost, each mark and gap within 25 ms of "
         "its kind (11-25: its gaps cut short by a signal, its base's key-up late)",
         lambda o: cw_timing_test(o, A, B), 90),
    Test("cw-timing x6200", "X6200 TX, X6100 RX", "the same the other way: the X6200's keyer in the X6100's audio",
         lambda o: cw_timing_test(o, B, A), 90),
    Test("cw-timing x6100 1w", "X6100 TX at 1 W, X6200 RX", "the X6100's keyer at 1 W: whether its base's late key-ups "
         "at 0.1 W are its power loop that low", lambda o: cw_timing_test(o, A, B, w=1.0), 90),
    Test("ambe-vocoder", "both RX", "each radio's AMBE chip through its SDR daemon: the parity field asked for, the "
         "speech through each chip at DMR's and D-STAR's rates and the other's frames, libmbe-dvsi alike with the "
         "parity field off, P25 full rate only on a chip with the P25 option", t_ambe_vocoder, 150),
    Test("dv-dmr", "both RX", "a DMR call of the test speech replayed into each radio's SDR daemon: TG 3100 from "
         "3112345 shown, its voice through the radio's own AMBE chip like the speech (r >= 0.9)",
         lambda o: dv_voice(o, "dmr"), 70),
    Test("dv-dmr-noparity", "both RX", "the DMR call again with both chips' parity field off: the test daemon reads "
         "the chip's answers whole through B's daemon's port; the field on again after",
         lambda o: dv_voice(o, "dmr", parity_off=True), 60),
    Test("dv-dstar", "both RX", "a D-STAR call of the test speech: CQCQCQ from N0CALL shown, its voice heard "
         "(r >= 0.4)", lambda o: dv_voice(o, "dstar"), 60),
    Test("dv-ysf", "both RX", "a YSF call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.7)",
         lambda o: dv_voice(o, "ysf"), 60),
    Test("dv-nxdn", "both RX", "an NXDN48 call of the test speech: TG 100 from 1234 shown, its voice heard "
         "(r >= 0.75)", lambda o: dv_voice(o, "nxdn"), 60),
    Test("dv-p25", "both RX", "a P25 phase 1 call (1 kHz IMBE frames): TG 4321 from 1234567 shown; the tone heard "
         "where the chip has DVSI's P25 option, silence where it has not", lambda o: dv_voice(o, "p25"), 60),
    Test("tci-audio-rate", "both RX", "each radio's TCI receiver audio at 97-103 % of real time", t_tci_audio_rate, 15),
    Test("tci-takes-control", "both RX", "a TCI client's set takes control (ctl tci) on both; gone, control back",
         t_tci_takes_control, 20),
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

# CISA's NIFOG (orch_nifog.py, 2026-11-25): F > NIFOG and the web page /nifog, receive only
import orch_nifog as _on                    # noqa: E402
_on.H = sys.modules[__name__]
TESTS += _on.tests(Test, A, B)
ORCH_COVERS.update(_on.COVERS)

# The cable's features that had no test yet (orch_cable.py, 2026-11-25): their tests, and what each covers
import orch_cable as _oc                    # noqa: E402
_oc.H = sys.modules[__name__]
TESTS += _oc.tests(Test, A, B)
ORCH_COVERS.update(_oc.COVERS)



# Other sessions put new GUIs on the radios (tools/cross_x6200/install.sh): one replaced both mid-run on 2026-11-25
# (15:43, its "no run going" check a moment before this run began). Who installs holds GUI_LOCK for the time it does
# (its pid in it) and waits while a run goes; a run waits while the lock is held by a live process
GUI_LOCK = os.path.join(bench.STATE, "gui_install.lock")


def gui_lock_wait(limit_s=1800):
    import subprocess
    t0 = time.time()
    while True:
        try:
            pid = int(open(GUI_LOCK).read().split()[0])
            os.kill(pid, 0)                     # alive: an install under way elsewhere
        except (OSError, ValueError, IndexError):
            pid = None
        if pid is None:                         # and no install.sh going without the lock
            r = subprocess.run(["pgrep", "-f", "cross_x6200/install.sh"], capture_output=True, text=True)
            pid = int(r.stdout.split()[0]) if r.stdout.split() else None
            if pid is None:
                return
        if time.time() - t0 > limit_s:
            raise SystemExit(f"{GUI_LOCK} held by {pid} for {limit_s} s: no run")
        if int(time.time() - t0) % 60 < 5:
            say(f"a GUI install under way (pid {pid}, {GUI_LOCK}): the run waits")
        time.sleep(5)


# One run at a time on the two radios, whichever session starts it (2026-11-25: runs of three sessions overlapped,
# two controllers on the radios, one on the air while the other had it listening). RUN_LOCK made with O_EXCL holds the
# run's pid; a run waits while it names a live process, takes it over from a dead one
RUN_LOCK = os.path.join(bench.STATE, "orch_run.lock")


def run_lock_take(limit_s=3600):
    import atexit
    t0, said = time.time(), 0.0
    while True:
        try:
            fd = os.open(RUN_LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            os.write(fd, f"{os.getpid()} {time.strftime('%H:%M:%S')} {' '.join(sys.argv[1:])}\n".encode())
            os.close(fd)
            atexit.register(lambda: _run_lock_drop())
            return
        except FileExistsError:
            pass
        try:
            held = open(RUN_LOCK).read().strip()
            pid = int(held.split()[0])
            os.kill(pid, 0)
        except (OSError, ValueError, IndexError):
            try:                                # its run is gone: the lock is stale
                os.remove(RUN_LOCK)
            except OSError:
                pass
            continue
        if time.time() - t0 > limit_s:
            raise SystemExit(f"another orch run holds the radios for {limit_s} s ({held}): no run")
        if time.time() - said > 60:
            say(f"another orch run holds the radios ({held}): this one waits")
            said = time.time()
        time.sleep(5)


def _run_lock_drop():
    try:
        if open(RUN_LOCK).read().split()[0] == str(os.getpid()):
            os.remove(RUN_LOCK)
    except (OSError, IndexError):
        pass


def run(names, port, lan, linger, everything=False, soak=1, squelch_cal=False, sql=None):
    run_lock_take()
    gui_lock_wait()
    kept = store_load()
    if names:
        tests = [t for t in TESTS if t.name in names or t.name.split(" ")[0] in names]
    elif everything:
        tests = list(TESTS)
    else:                       # the new ones and those that failed: what passed, or fails for a known reason, kept
        tests = [t for t in TESTS if kept.get(t.name, {}).get("result") != "PASS" and t.name not in KNOWN]
    if not tests:
        raise SystemExit("every test has passed or fails for a known reason (orch.py run --all: run them again)")
    run_names = {t.name for t in tests}
    shown = [dict(kept.get(t.name, {"result": "never run"}), name=t.name, known=KNOWN.get(t.name))
             for t in TESTS if t.name not in run_names]
    st = Status(tests, shown)
    # A console already answering on the port (the standing orch.py console): this run serves none and leaves at its
    # END. Its own bind could succeed beside it (127.0.0.1 next to the console's 0.0.0.0, SO_REUSEADDR on macOS),
    # and a finished run lingering an hour on 127.0.0.1 showed "no test running" while the next run transmitted
    # (the owner heard the radio click, 2026-11-24)
    import socket
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1.0).close()
        srv = None
    except OSError:
        try:
            srv = console(st, port, lan)
        except OSError:             # the standing console (orch.py console) has the port: it shows SNAPSHOT
            srv = None
    st.run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-orch"))
    os.makedirs(st.run_dir, exist_ok=True)
    st.log = open(os.path.join(st.run_dir, "events.jsonl"), "a")
    st.ev(f"console: http://{'0.0.0.0' if lan else '127.0.0.1'}:{port}/ "
          f"({'this run' + chr(39) + 's own' if srv else 'the standing one, orch.py console'}); run {st.run_dir}")
    o = None
    snap_on = threading.Event()

    def snap():
        while not snap_on.is_set():
            try:
                st.snapshot()
            except OSError:
                pass
            snap_on.wait(1.0)
    threading.Thread(target=snap, daemon=True).start()

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
        # The squelch held at SQL_HOLD and the volume 0 on both, before every test (ready()). Not calibrated: the TCI
        # audio the tests judge is taken before the squelch, which gates the speaker only (squelch_cal measured it,
        # 2026-11-23: 2..60 changed neither radio's quiet, -30/-48 dBFS); orch.py run --squelch-cal measures again
        if squelch_cal:
            st.phase = "squelch"
            o.squelch_cal()
        else:
            o.sql = SQL_HOLD if sql is None else sql
            o.both({"vol": 0, "sql": o.sql})
            st.ev(f"squelch {o.sql} and volume 0 on both, held before every test (in SSB the TCI audio judged is "
                  f"taken before the squelch; AM overs open it: it keeps AM audio closed seconds into a carrier)", "ok")
        st.phase = "running"
        for k in range(soak):
            if soak > 1:
                st.phase = f"soak {k + 1} of {soak}"
                st.ev(f"SOAK ROUND {k + 1} OF {soak}", "ok")
                with st.lock:
                    for x in st.tests:
                        x.update(result=None, says="", secs=None, steps=[])
            for i, t in enumerate(tests):
                with st.lock:
                    st.cur = i
                st.ev(f"TEST {i + 1}/{len(tests)} {t.name} ({t.role}): {t.what}")
                o.ready()                       # never on from radios set wrong: refused here stops the run
                o.violation = None
                o.trouble = None
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
                if o.trouble:
                    ok, says = False, f"{o.trouble}; " + says
                res = "PASS" if ok is True else "LOOK" if ok is None else "FAIL"
                with st.lock:
                    st.tests[i].update(result=res, says=says, secs=time.time() - t0, data=data)
                    if res == "PASS":
                        st.tests[i]["shots"] = []       # pictures only for what did not pass
                prev = store_load().get(t.name, {})
                code = test_code(t)
                same = prev.get("code") == code             # none yet (before the fingerprint): counts afresh
                tally = {"runs": (prev.get("runs", 0) if same else 0) + 1,
                         "passes": (prev.get("passes", 0) if same else 0) + (res == "PASS"),
                         "fails": (prev.get("fails", 0) if same else 0) + (res == "FAIL"),
                         "all_runs": prev.get("all_runs", prev.get("runs", 0)) + 1,
                         "all_passes": prev.get("all_passes", prev.get("passes", 0)) + (res == "PASS"), "code": code}
                store_put(t.name, {"result": res, "says": says, "at": time.time(), "secs": round(time.time() - t0, 1),
                                   "gui": hello(o.r[A]), "run": os.path.basename(st.run_dir), **tally})
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
        snap_on.set()
        st.snapshot()
    if srv is None:
        return
    if linger > 0:
        st.ev(f"the console stays up {linger} s")
        time.sleep(linger)
    srv.shutdown()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("list")
    lp.add_argument("--md", action="store_true", help="the tests as a Markdown table, for ORCH.md (no results)")
    rp = sub.add_parser("run")
    rp.add_argument("--only", default="")
    rp.add_argument("--port", type=int, default=8765)
    rp.add_argument("--lan", action="store_true", help="the console on the network, not only this computer")
    rp.add_argument("--linger", type=int, default=3600, help="seconds the console stays up after the run")
    rp.add_argument("--all", action="store_true", help="every test, the passed ones too")
    rp.add_argument("--squelch-cal", action="store_true", help="measure the squelch against the TCI audio first")
    rp.add_argument("--sql", type=int, default=None, help=f"the squelch to hold (default {SQL_HOLD})")
    rp.add_argument("--soak", type=int, default=1, help="the tests this many times over (each test's tally kept)")
    cp = sub.add_parser("console", help="the console alone, up until stopped: it shows each run's state as it goes "
                        "and the last run's between runs")
    cp.add_argument("--port", type=int, default=8765)
    cp.add_argument("--lan", action="store_true", help="on the network, not only this computer")
    sp = sub.add_parser("spec", help="replay the spectrum judge on an over's saved frames (*_spectrum.json)")
    sp.add_argument("file")
    sub.add_parser("coverage", help="features.json against every test's latest result, orch's and the suite's "
                                    "(runs/<time>-coverage/coverage.md)")
    a = ap.parse_args()
    if a.cmd == "coverage":
        print(os.path.join(coverage_all(), "coverage.md"))
        return
    if a.cmd == "console":
        console(None, a.port, a.lan)
        threading.Thread(target=idle_watch, daemon=True).start()
        say(f"console: http://{'0.0.0.0' if a.lan else '127.0.0.1'}:{a.port}/ (the runs' {SNAPSHOT})")
        while True:
            time.sleep(3600)
    if a.cmd == "spec":
        d = json.load(open(a.file))
        L = types.SimpleNamespace(name=d["listener"], r=types.SimpleNamespace(spectra=[tuple(x) for x in d["frames"]]))
        on, off = d["on"], d["off"]
        print(f"{d['label']}: {len(d['frames'])} frames, the listener {d['listener']}, {d['f']} Hz mode {d['mode']}")
        for t, a_, h in spec_track(L, d["f"], d["mode"], on - 0.2, off + 0.5):
            print(f"  {t - on:5.2f} s  {a_:7.0f} Hz  {h:5.1f} dB")
        print(spec_steps(L, d["f"], d["mode"], on, off, coarse=d["listener"] in SPEC_COARSE))
        return
    if a.cmd == "list" and a.md:
        print("| # | test | role | about | what passes |")
        print("|---|---|---|---|---|")
        for i, t in enumerate(TESTS, 1):
            print(f"| {i} | `{t.name}` | {t.role} | {t.est_s} s | {t.what.replace('|', '/')} |")
        return
    if a.cmd == "list":
        kept = store_load()
        for t in TESTS:
            k = kept.get(t.name, {})
            last = "KNOWN" if t.name in KNOWN else k.get("result", "never run")
            tally = f"{k.get('passes', '-')}/{k.get('runs', '-')}"
            print(f"{t.name:26s} {last:9s} {tally:>7s} {t.role:20s} ~{t.est_s:3d} s  {t.what}")
        return
    run([x for x in a.only.split(",") if x], a.port, a.lan, a.linger, everything=a.all or a.soak > 1, soak=a.soak,
        squelch_cal=a.squelch_cal, sql=a.sql)


if __name__ == "__main__":
    main()
