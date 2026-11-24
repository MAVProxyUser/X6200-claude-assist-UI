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
        self.events = collections.deque(maxlen=300)
        self.radios = {}
        self.sync = {"ok": True, "since": time.time(), "violations": 0, "last": None, "changing": False}
        self.run_dir = None

    def ev(self, text, kind="info"):
        with self.lock:
            self.events.append({"t": time.time(), "kind": kind, "text": text})
        say(text)

    def to_json(self):
        with self.lock:
            done = [t for t in self.tests if t["result"] is not None]
            left = sum(t["est_s"] for t in self.tests if t["result"] is None)
            counts = collections.Counter(t["result"] for t in done)
            return json.dumps({
                "now": time.time(), "t0": self.t0, "phase": self.phase, "cur": self.cur, "tests": self.tests,
                "events": list(self.events)[-120:], "radios": self.radios, "sync": self.sync,
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
</style></head><body>
<header><h1>Two-radio bench</h1><span id="phase"></span><span id="counts" class="muted"></span><span id="clock" class="muted"></span></header>
<main>
<section><h2>Radios</h2><div id="sync"></div><div class="radios" id="radios" style="margin-top:8px"></div></section>
<section><h2>Now</h2><div id="now"></div><div class="bar"><div id="prog" style="width:0"></div></div></section>
<section class="wide"><h2>Tests</h2><table><thead><tr><th>#</th><th>test</th><th>role</th><th>result</th><th>secs</th><th>what it found</th></tr></thead><tbody id="tests"></tbody></table></section>
<section class="wide"><h2>Events</h2><div class="ev" id="ev"></div></section>
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
$("ev").innerHTML=d.events.slice().reverse().map(e=>`<div class="${e.kind}">${t(e.t)} ${esc(e.text)}</div>`).join("");}
tick();setInterval(tick,700);
</script></body></html>"""


def console(status, port, lan):
    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/status"):
                body, ctype = status.to_json().encode(), "application/json"
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
        self.roles = {A: "RX", B: "RX"}
        self.changing = 0                   # changes to both under way: the guard looks away meanwhile
        self.violation = None               # this test's first sync violation
        self._diff_since = None
        self.on = True
        threading.Thread(target=self._guard, daemon=True).start()

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
            if diff:
                raise Refused("the two read differently after the set: " + ", ".join(f"{k} {a}/{b}"
                                                                                     for k, (a, b) in diff.items()))
        finally:
            self.changing -= 1
        return {k: bench._value(self.r[A], k) for k in want}

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
        self.on = False
        for r in self.r.values():
            try:
                r.stop()
            except OSError:
                pass
        for n, r in self.r.items():
            try:
                if r.state.get("rxo") == 0:
                    bench.arm(r, False)
                bench.close_apps(r)
                bench.open_page(r, "A")
            except (Refused, OSError) as e:
                self.st.ev(f"{n}: not on the Sentry's page A at the end: {e}", "bad")
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


def over(o, sender, listener, f, mode, label, body):
    """One transmission from sender to listener, both on f in mode (set alike and confirmed first): the gates, the
    Watch, the transmitter armed for it and locked after; body(watch) sends. Returns the Watch (closed)"""
    S, L = o.r[sender], o.r[listener]
    p, total = bench.chain_gate(o.c, "radios")
    with o.step(f"both on {f / 1e6:.4f} MHz mode {mode}, alike"):
        o.both_page("G")
        # the same filter on both in SSB (each radio's own differed: the X6100 300-2700, the X6200 100-3000; a step
        # heard 380 Hz high went past the X6100's top); AM and FM keep their own, a width of their own kind
        want, checks = bench.rx_profile(f, mode, pre=0, att=0, rfg=100, agc=0, sql=0,
                                        filt=(100, 3000) if mode in (0, 1, 2, 3) else None)
        o.both(dict(want, pwr=W, atu=0, xit=0), checks)
    with o.step(f"{sender} may send, {listener} may listen") as s:
        bench.ensure(S, *bench.tx_profile(f, mode, W))
        bench.radio_tx_gate(S, o.c, W)
        sup = bench.listener_gate(L)
        s.confirm(f"{listener}'s supply {sup}" if sup else "")
    o.roles = {sender: "TX", listener: "RX"}
    watch = bench.Watch([S, L], o.c, expect_dbm=10 * math.log10(W * 1000) - total, listener=L, w=W, label=label)
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


TESTS = [
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
]


def run(names, port, lan, linger):
    tests = [t for t in TESTS if not names or t.name in names or t.name.split(" ")[0] in names]
    if not tests:
        raise SystemExit(f"no test named {names}")
    st = Status(tests)
    srv = console(st, port, lan)
    st.run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-orch"))
    os.makedirs(st.run_dir, exist_ok=True)
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
        st.phase = "running"
        for i, t in enumerate(tests):
            with st.lock:
                st.cur = i
            o.violation = None
            st.ev(f"TEST {i + 1}/{len(tests)} {t.name} ({t.role})")
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
