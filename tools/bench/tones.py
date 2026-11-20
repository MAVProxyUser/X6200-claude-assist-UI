#!/usr/bin/env python3
"""Tones across the audio range from one radio's transmitter to the other's receiver, through the radios profile's
pads, and what came out measured: each tone's frequency (to a few hundredths of a hertz), its level, its harmonics,
and with the receiver on the other sideband its image. The transmitter is fed through its own TCI server by
tci_tone_tx.py run on it (a client on the radio is not asked about on the screen); the receiver's TCI audio is
recorded here. The bench harness's rules throughout (bench.py: the owner's word on the cable, the TX gate, the watch;
the first transmission at 0.1 W). The receiver's AGC is off for the run, so the levels
are the chain's, not the AGC's (--rx-rfg and --rx-att set its gain); everything is put back after.

    tones.py TX RX [--freq HZ] [--w W] [--passes usb,lsb] [--tones 200,300,...] [--amp 0.3]

Frequency: each tone's measured minus asked is fitted to offset + slope x tone; the offset is the two radios'
oscillators apart, the slope a sample-rate error (the X6100's base before gcfix1: -676 ppm). A report (JSON, the
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
        self.frames, self.on = [], True
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()
        self.text("audio_samplerate:24000;audio_stream_sample_type:int16;audio_stream_channels:1;"
                  "audio_stream_samples:512;audio_start:0;")

    def text(self, t):
        p = t.encode()
        mask = os.urandom(4)
        n = len(p)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
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
                if b0 & 0x0F == 2 and len(p) > 64:
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
    a = ap.parse_args()
    tones = [int(t) for t in a.tones.split(",")]
    c = bench.conf()
    p, total = chain_gate(c, "radios")
    if a.w != bench.FIRST_PROBE_W:
        say(f"the session's first transmission is at {bench.FIRST_PROBE_W} W, not {a.w}: {bench.FIRST_PROBE_W} W")
        a.w = bench.FIRST_PROBE_W
    txr, rxr = Radio(a.tx, c["radios"][a.tx], True), Radio(a.rx, c["radios"][a.rx], True)
    radios = [txr, rxr]
    run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-tones") + (f"-{a.tag}" if a.tag else ""))
    os.makedirs(run_dir, exist_ok=True)
    keys = ("freq", "mode", "pg", "atu", "agc", "att", "pre", "rfg")
    was, results = {}, {"tx": a.tx, "rx": a.rx, "f": a.freq, "w": a.w, "tones": tones, "passes": {}}
    tci = None
    delay = [None]
    plan = [[0, LEAD_S], [MARK_HZ, MARK_S], [0, MARK_GAP_S]] + [x for t in tones for x in ([t, TONE_S], [0, GAP_S])]
    key_s = sum(s for _, s in plan) + 1.0
    if key_s > c["limits"]["max_key_s"] - 1:
        raise Refused(f"{key_s:.1f} s on the air is over the limit")
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
        get = (lambda: int(r.state.get(k, -1))) if k in ("freq", "mode", "agc", "att", "pre", "rfg") else None
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
        setv(txr, "mode", 2)
        setv(rxr, "mode", rx_mode)
        radio_tx_gate(txr, c, a.w)
        line = (f"BENCH TONES: {a.tx.upper()} SENDS {len(tones)} TONES USB {a.w} W, {a.rx.upper()} LISTENS "
                f"{sideband.upper()}, {a.freq / 1e6:.3f} MHZ THROUGH {total} DB")
        for r in radios:
            r.send(f"banner {line}")
        time.sleep(1.0)
        watch = Watch(radios, c, expect_dbm=10 * math.log10(a.w * 1000) - total, listener=rxr, w=a.w)
        watch.soft_power = True
        arm(txr, True)
        tx_log({"who": txr.name, "f": a.freq, "w": txr.pwr(), "what": f"tones {label}"})
        t_begin = time.time()
        proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}",
                                 f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {a.amp} {key_s:.1f}"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        txlog = []                                  # the sender's own meter through the over: (s, W, SWR, ALC)
        while proc.poll() is None:
            with txr.lock:
                if txr.tx and txr.txm:
                    txlog.append((round(time.time() - t_begin, 2),) + tuple(txr.txm))
            # a stop, the other on the air, or more power than asked (the watch's soft stop releases only the remote's
            # PTT, not a TCI transmission): the sender killed, the server unkeys within 0.5 s
            if watch.why or rxr.tx or txr.name in watch.over_power:
                subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}", "pkill -f tci_tone_tx.py"])
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
            raise Refused(f"{label}: no tone found in {a.rx}'s audio (and no earlier pass to time it from)")
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
            f"{', OVER POWER ' + str(res['over_power_w']) + ' W: ended' if res['over_power_w'] else ''}")
        for t in res["tones"]:
            say(f"   {t['tone']:5d} Hz -> {t['f']} Hz ({t['err_hz']:+} Hz)  {t['dbfs']} dBFS  "
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
        r = txr
        r.send(f"set pwr {round(a.w * 10)}")
        r.wait("the power", lambda: abs(txr.pwr() - a.w) < 0.05, 5)
        setv(rxr, "agc", 0)                        # off: the chain's levels, not the AGC's
        if a.rx_rfg is not None:
            setv(rxr, "rfg", a.rx_rfg)
        if a.rx_att is not None:
            setv(rxr, "att", a.rx_att)
        tci = TciRx(rxr.host)
        time.sleep(1.0)
        for pas in a.passes.split(","):
            one_over(pas, pas)
        summary = {}
        for label, res in results["passes"].items():
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
                for k in ("freq", "mode", "agc", "att", "pre", "rfg"):
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
