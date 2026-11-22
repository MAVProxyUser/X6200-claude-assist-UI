#!/usr/bin/env python3
"""Tones across the audio range from one radio's transmitter to the other's receiver, through the radios profile's
pads, and what came out measured: each tone's frequency (to a few hundredths of a hertz), its level, its harmonics,
and with the receiver on the other sideband its image. The transmitter is fed through its own TCI server by
tci_tone_tx.py run on it (a client on the radio is not asked about on the screen); the receiver's TCI audio is
recorded here. The bench harness's rules throughout (bench.py: the owner's word on the cable, the TX gate, the watch;
the first transmission at 0.1 W). The receiver's AGC is off and its squelch open for the run, so the levels
are the chain's, not the AGC's (--rx-rfg and --rx-att set its gain); everything is put back after.

    tones.py TX RX [--freq HZ] [--w W] [--passes usb,lsb] [--tones 200,300,...] [--amp 0.3]
                                 --w over 0.1: every pass at 0.1 W first, then at W (limits.radio_w, 5 W at most)
    tones.py TX RX --bursts 0.05,0.1,0.2,0.4,0.8,1.6,3.2 [--burst-hz 1000] [--burst-s 0.5] [--fade 0.002]

--bursts: the transmitter's power loop on the first sound after a pause. Bursts of one tone with hard starts (2 ms),
each after the pause given (the first after the key-down); for each, the receiver's envelope over its first 80 ms
against its steady level at its end: the overshoot (dB) and how long it takes to settle within 1 dB. Xiegu's loop
raises its gain through a pause, so its bursts start hot (x6100_patch.py, fix "hold").

Frequency: each tone's measured minus asked is fitted to offset + slope x tone; the offset is the two radios'
oscillators apart, the slope a sample-rate error (the X6100's PLL error, -676 ppm by its values, does not show in
it: Xiegu's base and gcfix both within the measurement's ~30 ppm, 2026-11-20). A report (JSON, the
recording) in ~/.config/x6x00-bench/runs/<time>-tones/.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, base64, hashlib, json, math, os, socket, struct, subprocess, sys, threading, time, wave
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from bench import Radio, Watch, Refused, chain_gate, radio_tx_gate, tx_log, say  # noqa: E402

FS = 24000
TONES = [200, 300, 400, 600, 800, 1000, 1500, 2000, 2400, 2700, 2900, 3100]
TONE_S, GAP_S, LEAD_S = 0.9, 0.0, 0.4
MARK_HZ, MARK_S, MARK_GAP_S = 1000, 0.3, 0.0     # a strong first tone the sweep is timed from
SENDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tci_tone_tx.py")
# The sender stopped on the radio: by its pid file (the X6100's BusyBox has no pkill), else by name
STOP_SENDER = ("kill $(cat /tmp/tci_tone_tx.pid) 2>/dev/null || "
               "kill $(ps | grep '[t]ci_tone_tx.py' | awk '{print $1}') 2>/dev/null || echo 'no sender to stop'")


class TciRx:
    """The receiver's audio over its TCI: int16 mono 24 kHz, every frame kept with its arrival time"""

    def __init__(self, host, port=50001):
        self.s = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(1024)
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        if b" 101" not in head.split(b"\r\n")[0]:
            raise Refused(f"{host}: TCI did not answer")
        # No timeout from here: create_connection's 5 s ended the reader at the first stall that long (the X6200's
        # WiFi, 2026-11-20: every over after it "no audio"); image.py opens a new one when frames stop
        self.s.settimeout(None)
        self.frames, self.on = [], True
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()
        self.text("audio_samplerate:24000;audio_stream_sample_type:int16;audio_stream_channels:1;"
                  "audio_stream_samples:512;audio_start:0;")

    def text(self, t):
        self._send(0x1, t.encode())

    def _send(self, op, p):
        mask = os.urandom(4)
        n = len(p)
        head = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))

    def _take(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise OSError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _run(self):
        try:
            while self.on:
                b0, b1 = self._take(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._take(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._take(8))[0]
                p = self._take(n)
                if b0 & 0x0F == 9:      # the server's PING to a client silent 20 s: unanswered, it is gone at 60 s
                    self._send(0xA, p)
                elif b0 & 0x0F == 2 and len(p) > 64:
                    h = struct.unpack("<8I", p[:32])
                    if h[6] == 1 and h[2] == 0:          # RX audio, int16
                        with self.lock:
                            self.frames.append((time.time(), p[64:]))
        except OSError:
            pass

    def audio(self, t0=None, t1=None):
        with self.lock:
            fr = [(t, d) for t, d in self.frames if (t0 is None or t >= t0) and (t1 is None or t <= t1)]
        if not fr:
            return np.zeros(0), None
        x = np.frombuffer(b"".join(d for _, d in fr), dtype="<i2").astype(np.float64) / 32768.0
        return x, fr[0][0] - len(fr[0][1]) / 2 / FS

    def close(self):
        self.on = False
        try:
            self.text("audio_stop:0;")
            self.s.close()
        except OSError:
            pass


def peak(x, f0, span=60.0):
    """(frequency, level dBFS) of the strongest component within span of f0: Hann window, zero-padded FFT, a parabola
    through the log magnitudes around the top bin"""
    if len(x) < 256:
        return None, None
    w = np.hanning(len(x))
    nfft = 1 << (int(math.ceil(math.log2(len(x)))) + 3)
    X = np.abs(np.fft.rfft(x * w, nfft))
    fr = np.fft.rfftfreq(nfft, 1 / FS)
    lo, hi = np.searchsorted(fr, f0 - span), np.searchsorted(fr, f0 + span)
    if hi - lo < 3:
        return None, None
    k = lo + int(np.argmax(X[lo:hi]))
    if 0 < k < len(X) - 1:
        a, b, c = np.log(X[k - 1] + 1e-20), np.log(X[k] + 1e-20), np.log(X[k + 1] + 1e-20)
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    else:
        d = 0.0
    f = (k + d) * FS / nfft
    lvl = 20 * math.log10(X[k] / (np.sum(w) / 2) + 1e-20)
    return f, lvl


def analyse(x, t_start_rel, tones):
    """Each tone's middle 60 % measured; t_start_rel: where the first tone starts in x (s)"""
    out = []
    for i, f0 in enumerate(tones):
        a = t_start_rel + i * (TONE_S + GAP_S) + 0.2 * TONE_S
        b = a + 0.6 * TONE_S
        seg = x[int(a * FS):int(b * FS)]
        f, lvl = peak(seg, f0)
        h2 = peak(seg, 2 * f0, 30)[1] if 2 * f0 < FS / 2 - 100 else None
        h3 = peak(seg, 3 * f0, 30)[1] if 3 * f0 < FS / 2 - 100 else None
        noise = 20 * math.log10(np.sqrt(np.mean(seg ** 2)) + 1e-12) if len(seg) else None
        out.append({"tone": f0, "f": None if f is None else round(f, 3), "err_hz": None if f is None else round(f - f0, 3),
                    "dbfs": None if lvl is None else round(lvl, 1), "h2_db": None if h2 is None or lvl is None else
                    round(h2 - lvl, 1), "h3_db": None if h3 is None or lvl is None else round(h3 - lvl, 1),
                    "rms_dbfs": None if noise is None else round(noise, 1)})
    return out


def onset(x, f0=None):
    """The first sample where the audio rises 15 dB over its quiet start (50 ms frames)"""
    n = int(0.05 * FS)
    fr = [np.sqrt(np.mean(x[i:i + n] ** 2)) + 1e-9 for i in range(0, len(x) - n, n)]
    if len(fr) < 4:
        return None
    quiet = np.median(fr[:4])
    for i, v in enumerate(fr):
        if 20 * math.log10(v / quiet) > 15:
            return i * n / FS
    return None


def envelope(x, hop=24, win=48):
    """RMS over win samples every hop (1 ms frames of 2 ms at 24 kHz): (times s, levels)"""
    if len(x) < win:
        return np.zeros(0), np.zeros(0)
    c = np.concatenate(([0.0], np.cumsum(x * x)))
    i = np.arange(0, len(x) - win, hop)
    return (i + win / 2) / FS, np.sqrt((c[i + win] - c[i]) / win)


def analyse_bursts(x, on, bursts, burst_s, txlog=None, tx_t0=None, tone_hz=None):
    """bursts: [(start in the plan s, the pause before it s)]; on: the first burst's start in x (s, coarse). Each
    burst's start found in the envelope (the next 1 ms frame up through half the bursts' level, past the burst
    before), its steady level the median of its last 300 ms but 50, its overshoot the highest frame
    of its first 80 ms over that, its dip the lowest after 10 ms, its settling the last frame before its steady part
    more than 1 dB off it; pause_db: what the transmitter sent in the pause before it, under the burst's level"""
    if tone_hz:
        # the tone's band only (a zero-phase FFT mask of +-400 Hz: still 2 ms of resolution), so the transmitter's
        # broadband noise in the pauses counts less
        X = np.fft.rfft(x)
        f = np.fft.rfftfreq(len(x), 1 / FS)
        X[(f < tone_hz - 400) | (f > tone_hz + 400)] = 0
        x = np.fft.irfft(X, len(x))
    t, e = envelope(x)
    out = []
    if not len(e):
        return out
    # The bursts' level: the median of the frames within 20 dB of the loudest; their edges: where the envelope comes
    # up through half of it. Each burst is the first edge past the burst before it (the first: past the coarse onset
    # less 0.2 s), so a drift of the radios' timing never shifts the rest
    top = float(np.max(e))
    level = float(np.median(e[e > top * 0.1]))
    up = np.where((e[1:] > 0.5 * level) & (e[:-1] <= 0.5 * level))[0] + 1
    down = np.where((e[1:] <= 0.5 * level) & (e[:-1] > 0.5 * level))[0] + 1
    after = on - 0.2
    for b0, pause in bursts:
        w = up[t[up] >= after]
        if not len(w) or t[w[0]] > after + pause + 0.6:
            out.append({"pause_s": pause, "found": False})
            continue
        st = float(t[w[0]])
        # the next burst comes after this one's fall (the first past its middle: a dip early in it is no end, and a
        # start found late, on a slow rise, does not push past the next one)
        f_ = down[t[down] >= st + burst_s * 0.5]
        after = float(t[f_[0]]) if len(f_) else st + burst_s - 0.02
        m = (t >= st + burst_s - 0.3) & (t <= st + burst_s - 0.05)
        steady = float(np.median(e[m])) if m.any() else level
        head = (t >= st) & (t <= st + 0.08)
        top_b = float(np.max(e[head]))
        body = (t >= st + 0.01) & (t <= st + burst_s - 0.3)
        off = np.where((t >= st) & (t <= st + burst_s - 0.3) & (np.abs(20 * np.log10(e / steady + 1e-12)) > 1))[0]
        quiet = (t >= st - pause + 0.02) & (t <= st - 0.02)
        seg = x[int(st * FS):int((st + burst_s) * FS)]
        r = {"pause_s": pause, "found": True, "start_s": round(st, 3), "steady_dbfs": round(20 * math.log10(steady), 1),
             "overshoot_db": round(20 * math.log10(top_b / steady), 1),
             "dip_db": round(20 * math.log10(float(np.min(e[body])) / steady + 1e-12), 1) if body.any() else None,
             "settle_ms": round((float(t[off[-1]]) - st) * 1000) if len(off) else 0,
             "pause_db": round(20 * math.log10(float(np.median(e[quiet])) / steady + 1e-12), 1)
             if pause >= 0.05 and quiet.any() else None,
             "clipped": bool(len(seg) and np.max(np.abs(seg)) > 0.97)}
        if txlog and tx_t0 is not None:
            ws = [w_ for s_, w_, *_ in txlog if tx_t0 + b0 - 0.05 <= s_ <= tx_t0 + b0 + burst_s]
            r["tx_meter_max_w"] = max(ws) if ws else None
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tx")
    ap.add_argument("rx")
    ap.add_argument("--freq", type=int, default=14200000)
    ap.add_argument("--w", type=float, default=0.1)
    ap.add_argument("--passes", default="usb,lsb")
    ap.add_argument("--tones", default=",".join(map(str, TONES)))
    ap.add_argument("--amp", type=float, default=0.3)
    ap.add_argument("--tag", default="")
    ap.add_argument("--rx-rfg", type=int, default=None, help="the receiver's RF gain for the run")
    ap.add_argument("--rx-att", type=int, default=None, help="the receiver's attenuator for the run (0/1)")
    ap.add_argument("--bursts", default=None, help="the pauses (s) before each burst after the first: the loop's overshoot")
    ap.add_argument("--burst-hz", type=int, default=1000)
    ap.add_argument("--burst-s", type=float, default=0.5)
    ap.add_argument("--fade", type=float, default=None, help="each stretch of tone faded in and out (s): 0.3, bursts 0.002")
    ap.add_argument("--tx-mode", type=int, default=2, help="the transmitter's mode for the run (2 USB, 3 USB-D)")
    a = ap.parse_args()
    tones = [int(t) for t in a.tones.split(",")]
    gaps = [float(g) for g in a.bursts.split(",")] if a.bursts else None
    fade = a.fade if a.fade is not None else (0.002 if gaps else 0.3)
    if gaps:
        a.passes, tones = "usb", [a.burst_hz]
    c = bench.conf()
    p, total = chain_gate(c, "radios")
    # The session's first transmission at 0.1 W whatever is asked (bench.py's rule 4): above it, every pass runs at
    # 0.1 W first, then at the power asked (the owner, 2026-11-20: gcfix at 5 W), each its own label
    if a.w > c["limits"]["radio_w"]:
        raise Refused(f"{a.w} W is over the limit ({c['limits']['radio_w']} W)")
    powers = [bench.FIRST_PROBE_W] + ([a.w] if a.w > bench.FIRST_PROBE_W else [])
    cur_w = [bench.FIRST_PROBE_W]
    txr, rxr = Radio(a.tx, c["radios"][a.tx], True), Radio(a.rx, c["radios"][a.rx], True)
    radios = [txr, rxr]
    run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-tones") + (f"-{a.tag}" if a.tag else ""))
    os.makedirs(run_dir, exist_ok=True)
    keys = ("freq", "mode", "pg", "atu", "agc", "att", "pre", "rfg", "sql")
    was, results = {}, {"tx": a.tx, "rx": a.rx, "f": a.freq, "w": a.w, "tones": tones, "passes": {}}
    tci = None
    delay = [None]
    plan = [[0, LEAD_S], [MARK_HZ, MARK_S], [0, MARK_GAP_S]] + [x for t in tones for x in ([t, TONE_S], [0, GAP_S])]
    bursts = None
    if gaps:
        plan, bursts, at = [[0, LEAD_S]], [], LEAD_S
        for pause in [None] + gaps:                 # the first burst right after the key-down's lead
            if pause is not None:
                plan.append([0, pause])
                at += pause
            bursts.append((at, LEAD_S if pause is None else pause))
            plan.append([a.burst_hz, a.burst_s])
            at += a.burst_s
        plan.append([0, 0.6])                       # the radio's audio buffer played out before the unkey (the X6100
                                                    # cut the last burst short without it)
    key_s = sum(s for _, s in plan) + 1.0
    lim_s = bench.FULL_RATING_KEY_S if a.w > bench.HARD_MAX_W else c["limits"]["max_key_s"]
    if key_s > lim_s - 1:
        raise Refused(f"{key_s:.1f} s on the air is over the limit ({lim_s:.0f} s at {a.w} W)")
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", SENDER, f"root@{txr.host}:/tmp/tci_tone_tx.py"], check=True)

    def tap(r):
        r.send("key AB 1"); time.sleep(0.15); r.send("key AB 0")

    def arm(r, on):
        for _ in range(3):
            if (r.state.get("rxo") == 0) == on:
                return
            tap(r)
            try:
                r.wait("TX " + ("armed" if on else "locked"), lambda: (r.state.get("rxo") == 0) == on, 5)
                return
            except Refused:
                pass
        raise Refused(f"{r.name}: TX not {'armed' if on else 'locked'}")

    def setv(r, k, v):
        get = (lambda: int(r.state.get(k, -1))) if k in ("freq", "mode", "agc", "att", "pre", "rfg", "sql") else None
        for _ in range(2):
            if get and get() == v:
                return
            r.send(f"set {k} {v}")
            try:
                r.wait(f"{k} {v}", lambda: get() == v, 6)
                return
            except Refused:
                pass
        raise Refused(f"{r.name}: {k} {v} not taken")

    def one_over(sideband, label):
        rx_mode = {"usb": 2, "lsb": 0}[sideband]
        setv(txr, "mode", a.tx_mode)
        setv(rxr, "mode", rx_mode)
        w = cur_w[0]
        radio_tx_gate(txr, c, w)
        line = (f"BENCH TONES: {a.tx.upper()} SENDS {len(tones)} TONES USB {w} W, {a.rx.upper()} LISTENS "
                f"{sideband.upper()}, {a.freq / 1e6:.3f} MHZ THROUGH {total} DB")
        for r in radios:
            r.send(f"banner {line}")
        time.sleep(1.0)
        watch = Watch(radios, c, expect_dbm=10 * math.log10(w * 1000) - total, listener=rxr, w=w)
        arm(txr, True)
        tx_log({"who": txr.name, "f": a.freq, "w": txr.pwr(), "what": f"tones {label}"})
        t_begin = time.time()
        proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}",
                                 f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {a.amp} {key_s:.1f} 50001 {fade}"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        txlog = []                                  # the sender's own meter through the over: (s, W, SWR, ALC)
        while proc.poll() is None:
            with txr.lock:
                if txr.tx and txr.txm:
                    txlog.append((round(time.time() - t_begin, 2),) + tuple(txr.txm))
            # a stop or the other on the air (the watch's stop releases only the remote's PTT, not a TCI transmission):
            # the sender killed, the server unkeys within 0.5 s. More power than asked is logged, not stopped
            if watch.why or rxr.tx:
                r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}", STOP_SENDER],
                                   capture_output=True, text=True)
                say(f"{txr.name}: the sender stopped ({(r.stdout + r.stderr).strip() or 'killed'})")
                break
            time.sleep(0.05)
        out = proc.communicate(timeout=30)[0]
        t_end = time.time()
        watch.close()
        try:
            txr.wait("off the air", lambda: not txr.tx, 5)
        except Refused:
            pass
        arm(txr, False)
        start = next((float(l.split()[1]) for l in out.splitlines() if l.startswith("start ")), None)
        time.sleep(1.0)
        x, x_t0 = tci.audio(t_begin - 0.5, t_end + 1.0)
        if watch.why:
            raise Refused(f"stopped: {watch.why}")
        if not len(x):
            raise Refused(f"no audio from {a.rx}'s TCI")
        path = os.path.join(run_dir, f"{label}.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
            w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
        on = onset(x) if sideband == "usb" else None
        if on is not None and start is not None:
            delay[0] = x_t0 + on - start                # the sender's start to the tones in the receiver's audio
        elif start is not None and delay[0] is not None:
            on = start + delay[0] - x_t0                # the other sideband: timed from the USB pass's delay
        if on is None:
            results["passes"][label] = {"error": "no tone found", "sender": out.strip().splitlines()[-5:],
                                        "tx_peak_w": watch.tx_peak.get(txr.name), "txlog": txlog, "wav": path}
            say(f"{label}: sender said {out.strip().splitlines()[-3:]}, TX peak {watch.tx_peak.get(txr.name)} W, "
                f"meter log {txlog[:6]}...{txlog[-3:]}")
            raise Refused(f"{label}: no tone found in {a.rx}'s audio (and no earlier pass to time it from)")
        if bursts:
            res = {"sender": out.strip().splitlines()[-3:], "onset_s": round(on, 3), "wav": path, "fade_s": fade,
                   "tx_peak_w": watch.tx_peak.get(txr.name), "over_power_w": watch.over_power.get(txr.name),
                   "txlog": txlog, "bursts": analyse_bursts(x, on, bursts, a.burst_s, txlog,
                                                            None if start is None else start - t_begin, a.burst_hz)}
            results["passes"][label] = res
            say(f"{label}: TX peak {res['tx_peak_w']} W by its own meter"
                f"{', OVER POWER ' + str(res['over_power_w']) + ' W (logged)' if res['over_power_w'] else ''}")
            for b in res["bursts"]:
                if not b["found"]:
                    say(f"   after {b['pause_s']:5.2f} s: burst not found")
                    continue
                say(f"   after {b['pause_s']:5.2f} s: overshoot {b['overshoot_db']:+5.1f} dB, dip {b['dip_db']} dB, "
                    f"settled in {b['settle_ms']:4d} ms, steady {b['steady_dbfs']} dBFS, pause {b['pause_db']} dB"
                    f"{' CLIPPED' if b['clipped'] else ''}, TX meter max {b.get('tx_meter_max_w')} W")
            time.sleep(1.5)
            return res
        res = {"sender": out.strip().splitlines()[-3:], "onset_s": round(on, 3), "peak_dbfs":
               round(20 * math.log10(np.max(np.abs(x)) + 1e-12), 1), "tx_peak_w": watch.tx_peak.get(txr.name),
               "tones": analyse(x, on + MARK_S + MARK_GAP_S, tones), "wav": path,
               "over_power_w": watch.over_power.get(txr.name), "txlog": txlog, "delay_s": delay[0]}
        # The listener's own spectrum at each tone's RF (USB: above the dial), through the tone's middle: what went on
        # the air, whatever the listener's audio path made of it
        t_first = x_t0 + on + MARK_S + MARK_GAP_S
        for i, t in enumerate(res["tones"]):
            a0 = t_first + i * (TONE_S + GAP_S) + 0.2 * TONE_S
            rf = a.freq + t["tone"]
            t["spec_db"] = bench.spectrum_peak(rxr, rf, a0, a0 + 0.6 * TONE_S, span=120)
            t["spec_quiet_db"] = bench.spectrum_peak(rxr, rf, t_begin - 0.5, t_begin + 0.5, span=120)
        results["passes"][label] = res
        pw = [t[1] for t in txlog]
        say(f"{label}: onset {on:.2f} s, peak {res['peak_dbfs']} dBFS, TX peak {res['tx_peak_w']} W, median "
            f"{float(np.median(pw)) if pw else None} W, ALC median {float(np.median([t[3] for t in txlog])) if txlog else None}"
            f"{', OVER POWER ' + str(res['over_power_w']) + ' W (logged)' if res['over_power_w'] else ''}")
        for t in res["tones"]:
            say(f"   {t['tone']:5d} Hz -> {t['f']} Hz ({'none' if t['err_hz'] is None else format(t['err_hz'], '+')} Hz)  "
                f"{t['dbfs']} dBFS  "
                f"H2 {t['h2_db']} H3 {t['h3_db']}  spectrum {t.get('spec_quiet_db')} -> {t.get('spec_db')} dB")
        time.sleep(1.5)
        return res

    try:
        for r in radios:
            r.take_control()
            was[r.name] = {k: r.state.get(k) for k in keys}
            was[r.name]["pwr"] = r.pwr()
            if r.state.get("atu"):
                r.send("set atu 0")
                r.wait("the tuner out", lambda r=r: not r.state.get("atu"), 10)
            if r.state.get("pg") != "G":
                r.send("open sentry G")
                r.wait("G, the Walkie", lambda r=r: r.state.get("pg") == "G", 10)
            setv(r, "freq", a.freq)
        setv(rxr, "agc", 0)                        # off: the chain's levels, not the AGC's
        setv(rxr, "sql", 0)                        # open: a squelch shut on the receiver mutes its audio whole (the
                                                   # X6200's at 4, 2026-11-20: "no tone found" runs)
        if a.rx_rfg is not None:
            setv(rxr, "rfg", a.rx_rfg)
        if a.rx_att is not None:
            setv(rxr, "att", a.rx_att)
        tci = TciRx(rxr.host)
        time.sleep(1.0)
        for w in powers:
            cur_w[0] = w
            txr.send(f"set pwr {round(w * 10)}")
            txr.wait("the power", lambda: abs(txr.pwr() - w) < 0.05, 5)
            for pas in a.passes.split(","):
                one_over(pas, pas if len(powers) == 1 else f"{pas}-{w}W")
        summary = {}
        for label, res in results["passes"].items():
            if "bursts" in res:
                found = [b for b in res["bursts"] if b["found"]]
                summary[label] = {"overshoot_max_db": max((b["overshoot_db"] for b in found[1:]), default=None),
                                  "dip_min_db": min((b["dip_db"] for b in found[1:] if b["dip_db"] is not None),
                                                    default=None),
                                  "pause_max_db": max((b["pause_db"] for b in found[1:] if b["pause_db"] is not None),
                                                      default=None),
                                  "first_overshoot_db": found[0]["overshoot_db"] if found else None,
                                  "bursts_found": len(found), "bursts": len(res["bursts"])}
                continue
            ok = [t for t in res["tones"] if t["err_hz"] is not None and t["dbfs"] is not None and t["dbfs"] > -60 and
                  abs(t["err_hz"]) < 30]
            if len(ok) >= 3:
                f = np.array([t["tone"] for t in ok], float)
                e = np.array([t["err_hz"] for t in ok], float)
                slope, off = np.polyfit(f, e, 1)
                summary[label] = {"offset_hz": round(off, 3), "slope_ppm": round(slope * 1e6, 1),
                                  "residual_hz": round(float(np.std(e - (off + slope * f))), 3)}
        if "usb" in results["passes"] and "lsb" in results["passes"]:
            img = []
            for u, l in zip(results["passes"]["usb"]["tones"], results["passes"]["lsb"]["tones"]):
                if u["dbfs"] is not None and l["dbfs"] is not None:
                    img.append({"tone": u["tone"], "image_db": round(l["dbfs"] - u["dbfs"], 1)})
            summary["image"] = img
        results["summary"] = summary
        say("summary:", json.dumps(summary))
    finally:
        if tci:
            tci.close()
        for r in radios:
            try:
                r.stop()
                r.send("banner")
                v = was.get(r.name, {})
                if v.get("pwr") is not None and v["pwr"] < 99:
                    r.send(f"set pwr {round(v['pwr'] * 10)}")
                for k in ("freq", "mode", "agc", "att", "pre", "rfg", "sql"):
                    if v.get(k) is not None:
                        r.send(f"set {k} {int(v[k])}")
                        time.sleep(0.3)
                if v.get("atu"):
                    r.send("set atu 1")
                if v.get("pg") and v["pg"] != "G":
                    r.send(f"open sentry {v['pg']}")
                time.sleep(0.5)
                r.close()
            except Exception as e:  # noqa: BLE001
                say(f"{r.name}: putting it back: {e}")
        with open(os.path.join(run_dir, "report.json"), "w") as f:
            json.dump(results, f, indent=1)
        say("report", os.path.join(run_dir, "report.json"))


if __name__ == "__main__":
    main()
