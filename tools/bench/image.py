#!/usr/bin/env python3
"""Where a transmitter's opposite-sideband image and carrier leak land, and how strong they are, band by band (the
owner, 2026-11-20: "you MUST prove this via testing"). TX sends USB tones fa through the radios profile's pads; RX
listens in USB with its dial OFF Hz below TX's. In RX's audio, then:

    the wanted tone (TX dial + fa)               at OFF + fa
    TX's carrier leak (TX dial)                  at OFF
    TX's image, if it is the tone mirrored
    around TX's dial (TX dial - fa)              at OFF - fa

RX's own image of anything mirrors it around RX's dial into RX's other sideband, which its USB demodulator leaves
out, so what the audio shows below OFF is TX's. Proof of the place: with two tones and two offsets, the image's
measured frequency against 2 x carrier - wanted (the mirror around TX's oscillator, whatever its error) in hertz.
Besides: RX's panadapter across its whole width, against its quiet frames before the over, for anything else the
over brought up, each named (wanted, image, carrier, RX's own image of the wanted at 2 x RX's centre - wanted) or
left unexplained.

The bench rules throughout (bench.py: the owner's word on the cable, the TX gate at each power, the watch; the
session's first over at 0.1 W). RX's squelch open, its AGC and attenuator as given, all put back after. A report
(JSON and each over's recording) in ~/.config/x6x00-bench/runs/<time>-image/.

    image.py TX RX [--bands 160,80,40,30,20,17,15,12,10,6] [--w 5] [--tones 600,900] [--offsets 1500]
                   [--proof-band 20,80] [--proof-offsets 1500,1200] [--amp 0.3] [--rx-agc 3] [--rx-att 1] [--rx-rfg N]
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, json, math, os, subprocess, sys, time, wave
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from bench import Radio, Watch, Refused, chain_gate, radio_tx_gate, tx_log, say  # noqa: E402
from tones import FS, SENDER, STOP_SENDER, TciRx, peak  # noqa: E402

BANDS = {"160": 1900000, "80": 3700000, "40": 7150000, "30": 10130000, "20": 14200000, "17": 18130000,
         "15": 21300000, "12": 24950000, "10": 28500000, "6": 50200000}
LEAD_S, TONE_S, TAIL_S, FADE_S = 0.4, 2.0, 0.6, 0.05
RISE_DB = 10            # a panadapter bin this much over its quiet level: something the over brought up


def segs(on, tones):
    """Each tone's middle 60 % in the recording (s), the first tone starting at on"""
    return [(on + i * TONE_S + 0.2 * TONE_S, on + i * TONE_S + 0.8 * TONE_S) for i in range(len(tones))]


def band_onset(x, f0, rise_db=15):
    """Where the component at f0 (+-60 Hz) first rises rise_db over its first 0.3 s (25 ms frames): the AGC on,
    the receiver's hiss comes up between overs, so the whole audio's level hardly moves"""
    n = int(0.025 * FS)
    fr = np.fft.rfftfreq(4 * n, 1 / FS)
    m = (fr > f0 - 60) & (fr < f0 + 60)
    e = []
    for i in range(0, len(x) - n, n):
        X = np.abs(np.fft.rfft(x[i:i + n] * np.hanning(n), 4 * n))
        e.append(10 * math.log10(float(np.sum(X[m] ** 2)) + 1e-20))
    if len(e) < 14:
        return None
    quiet = float(np.median(e[:12]))
    for i, v in enumerate(e):
        if v - quiet > rise_db:
            return i * n / FS
    return None


def floor_db(x):
    """The audio's noise floor (dBFS per FFT bin, median over 150-3000 Hz) for the image's and carrier's margins"""
    w = np.hanning(len(x))
    X = np.abs(np.fft.rfft(x * w)) / (np.sum(w) / 2)
    fr = np.fft.rfftfreq(len(x), 1 / FS)
    m = (fr > 150) & (fr < 3000)
    return 20 * math.log10(float(np.median(X[m])) + 1e-20)


def others(x, known, top_db):
    """Components in 150-3000 Hz within 70 dB of the top, away from the known ones (+-40 Hz): (Hz, dB under top)"""
    w = np.hanning(len(x))
    nfft = 1 << (int(math.ceil(math.log2(len(x)))) + 1)
    X = 20 * np.log10(np.abs(np.fft.rfft(x * w, nfft)) / (np.sum(w) / 2) + 1e-20)
    fr = np.fft.rfftfreq(nfft, 1 / FS)
    out = []
    for k in range(2, len(X) - 2):
        if not 150 < fr[k] < 3000 or X[k] < top_db - 70 or X[k] < max(X[k - 2:k + 3]):
            continue
        if any(abs(fr[k] - f) < 40 for f in known):
            continue
        out.append((round(float(fr[k]), 1), round(float(X[k] - top_db), 1)))
    out.sort(key=lambda t: -t[1])
    return out[:6]


def pan(rxr, t0, t1, q0, q1):
    """RX's panadapter: the bins that rose RISE_DB over the quiet frames (q0..q1) in t0..t1, grouped: (Hz, dB, rise)"""
    on, quiet, cen, wid = [], [], None, None
    for t, c_, w_, v in list(rxr.r.spectra):
        if not v:
            continue
        if t0 <= t <= t1:
            on.append(v); cen, wid = c_, w_
        elif q0 <= t <= q1:
            quiet.append(v)
    if not on or not quiet or len({len(v) for v in on + quiet}) != 1:
        return None, None, []
    a, q = np.mean(np.array(on, float), 0), np.median(np.array(quiet, float), 0)
    per = wid / len(a)
    up = np.where(a - q >= RISE_DB)[0]
    groups, cur = [], []
    for i in up:
        if cur and i != cur[-1] + 1:
            groups.append(cur); cur = []
        cur.append(i)
    if cur:
        groups.append(cur)
    out = []
    for g in groups:
        k = g[int(np.argmax(a[g]))]
        out.append((round(cen - wid / 2 + (k + 0.5) * per), round(float(a[k]), 1), round(float(a[k] - q[k]), 1)))
    return cen, wid, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tx")
    ap.add_argument("rx")
    ap.add_argument("--bands", default=",".join(BANDS))
    ap.add_argument("--w", type=float, default=5.0)
    ap.add_argument("--tones", default="600,900")
    ap.add_argument("--offsets", default="1500")
    ap.add_argument("--proof-band", default="20,80")
    ap.add_argument("--proof-offsets", default="1500,1200")
    ap.add_argument("--amp", type=float, default=0.3)
    ap.add_argument("--rx-agc", type=int, default=3)
    ap.add_argument("--rx-att", type=int, default=1)
    ap.add_argument("--rx-rfg", type=int, default=None, help="the receiver's RF gain for the run (as it is if not given)")
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    tones = [int(t) for t in a.tones.split(",")]
    c = bench.conf()
    p, total = chain_gate(c, "radios")
    txr, rxr = Radio(a.tx, c["radios"][a.tx], True), Radio(a.rx, c["radios"][a.rx], True)
    radios = [txr, rxr]
    run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-image") + (f"-{a.tag}" if a.tag else ""))
    os.makedirs(run_dir, exist_ok=True)
    keys = ("freq", "mode", "pg", "atu", "agc", "att", "pre", "rfg", "sql")
    was = {}
    results = {"tx": a.tx, "rx": a.rx, "w": a.w, "tones": tones, "pads_db": total, "overs": []}
    plan = [[0, LEAD_S]] + [[t, TONE_S] for t in tones] + [[0, TAIL_S]]
    key_s = sum(s for _, s in plan) + 1.0
    lim_s = bench.FULL_RATING_KEY_S if a.w > bench.HARD_MAX_W else c["limits"]["max_key_s"]
    if key_s > lim_s - 1:
        raise Refused(f"{key_s:.1f} s on the air is over the limit ({lim_s:.0f} s at {a.w} W)")
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", SENDER, f"root@{txr.host}:/tmp/tci_tone_tx.py"], check=True)
    rx_audio = [None]

    def audio_live():
        """The receiver's TCI audio, a new connection if nothing came in the last 2 s (its WiFi stalls)"""
        t = rx_audio[0]
        if t is not None:
            with t.lock:
                last = t.frames[-1][0] if t.frames else 0
            if time.time() - last < 2.0:
                return t
            say(f"{a.rx}: no TCI audio for {time.time() - last:.0f} s: connecting again")
            t.close()
        rx_audio[0] = TciRx(rxr.host)
        for _ in range(30):
            time.sleep(0.1)
            with rx_audio[0].lock:
                if rx_audio[0].frames:
                    break
        return rx_audio[0]

    def arm(r, on):
        for _ in range(3):
            if (r.state.get("rxo") == 0) == on:
                return
            r.send("key AB 1"); time.sleep(0.15); r.send("key AB 0")
            try:
                r.wait("TX " + ("armed" if on else "locked"), lambda: (r.state.get("rxo") == 0) == on, 5)
                return
            except Refused:
                pass
        raise Refused(f"{r.name}: TX not {'armed' if on else 'locked'}")

    def setv(r, k, v):
        for _ in range(2):
            if int(r.state.get(k, -1)) == v:
                return
            r.send(f"set {k} {v}")
            try:
                r.wait(f"{k} {v}", lambda: int(r.state.get(k, -1)) == v, 6)
                return
            except Refused:
                pass
        raise Refused(f"{r.name}: {k} {v} not taken")

    def setw(w):
        txr.send(f"set pwr {round(w * 10)}")
        txr.wait(f"{w} W", lambda: abs(txr.pwr() - w) < 0.05, 5)

    def over(band, f, off, w):
        setv(txr, "freq", f)
        setv(rxr, "freq", f - off)
        setv(txr, "mode", 2)
        setv(rxr, "mode", 2)
        setw(w)
        radio_tx_gate(txr, c, w)
        tci = audio_live()
        label = f"{band}m-{w}W-off{off}"
        for r in radios:
            r.send(f"banner BENCH IMAGE: {a.tx.upper()} SENDS USB TONES {w} W, {a.rx.upper()} LISTENS {off} HZ BELOW, "
                   f"{f / 1e6:.3f} MHZ THROUGH {total} DB")
        time.sleep(1.5)
        q0 = time.time() - 1.2
        watch = Watch(radios, c, expect_dbm=10 * math.log10(w * 1000) - total, listener=rxr, w=w)
        arm(txr, True)
        tx_log({"who": txr.name, "f": f, "w": txr.pwr(), "what": f"image {label}"})
        t_begin = time.time()
        proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}",
                                 f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {a.amp} {key_s:.1f} 50001 {FADE_S}"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        txlog = []
        while proc.poll() is None:
            with txr.lock:
                if txr.tx and txr.txm:
                    txlog.append((round(time.time() - t_begin, 2),) + tuple(txr.txm))
            if watch.why or rxr.tx or txr.name in watch.over_power:
                r_ = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}", STOP_SENDER],
                                    capture_output=True, text=True)
                say(f"{txr.name}: the sender stopped ({(r_.stdout + r_.stderr).strip() or 'killed'})")
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
        time.sleep(1.0)
        if watch.why:
            raise Refused(f"stopped: {watch.why}")
        x, x_t0 = tci.audio(t_begin - 0.5, t_end + 1.0)
        if not len(x):
            raise Refused(f"no audio from {a.rx}'s TCI")
        path = os.path.join(run_dir, f"{label}.wav")
        with wave.open(path, "wb") as wf:
            wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(FS)
            wf.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
        on = band_onset(x, off + tones[0])
        res = {"band": band, "f": f, "rx_dial": f - off, "off": off, "w_asked": w, "wav": path,
               "peak_dbfs": round(20 * math.log10(np.max(np.abs(x)) + 1e-12), 1), "sender": out.strip().splitlines()[-2:],
               "tx_meter_w": round(float(np.median([t[1] for t in txlog])), 2) if txlog else None,
               "tx_meter_alc": round(float(np.median([t[3] for t in txlog])), 2) if txlog else None,
               "tx_peak_w": watch.tx_peak.get(txr.name), "rx_meter_dbm": watch.rx_peak, "tones": []}
        if on is None:
            res["error"] = "no tone found"
            results["overs"].append(res)
            say(f"{label}: no tone found in {a.rx}'s audio")
            return res
        for fa, (s0, s1) in zip(tones, segs(on, tones)):
            seg = x[int(s0 * FS):int(s1 * FS)]
            if len(seg) < int(0.5 * FS):            # the receiver's TCI audio stalled (its WiFi) or the over cut
                res["tones"].append({"fa": fa, "error": f"{len(seg) / FS:.2f} s of audio in the tone's middle"})
                continue
            fw, lw = peak(seg, off + fa, 40)
            fc, lc = peak(seg, off, 40)
            fi, li = peak(seg, off - fa, 40) if off - fa > 120 else (None, None)
            fl = floor_db(seg)
            t = {"fa": fa, "wanted_hz": None if fw is None else round(fw, 2), "wanted_dbfs": None if lw is None else
                 round(lw, 1), "floor_dbfs": round(fl, 1)}
            if fw is not None and fc is not None:
                t["carrier_hz"], t["carrier_dbc"] = round(fc, 2), round(lc - lw, 1)
                t["carrier_over_floor_db"] = round(lc - fl, 1)
            if fw is not None and fi is not None:
                t["image_hz"], t["image_dbc"] = round(fi, 2), round(li - lw, 1)
                t["image_over_floor_db"] = round(li - fl, 1)
                # TX's oscillator in RX's audio: the carrier if it shows, else the wanted's place less fa
                lo = fc if fc is not None and lc - fl > 10 else fw - fa
                t["image_mirror_err_hz"] = round(fi - (2 * lo - fw), 2)
                t["image_rf_hz"] = round(f - off + fi)
            if fw is not None:
                t["others"] = others(seg, [fw, fc or off, fi or (off - fa)], lw)
            # the panadapter through the tone's middle against the quiet second before the over
            cen, wid, ups = pan(rxr, x_t0 + s0, x_t0 + s1, q0, q0 + 1.0)
            if cen and ups:
                # The panadapter's bins (about 100 Hz) run the wanted, carrier and image into one group, and its
                # frequencies sit a few bins off the audio's: the group with the strongest bin is the channel, the
                # rest is measured from it. Its dB against that strongest bin
                main = max(ups, key=lambda u: u[1])
                named = []
                for hz, db, rise in ups:
                    if abs(hz - main[0]) <= 3000:
                        continue
                    d = hz - main[0] + fa                     # from TX's dial, by the wanted's place
                    what = "RX's own image of the wanted" if abs(hz - (2 * cen - main[0])) <= 400 else "far"
                    named.append({"rf_from_tx_dial_hz": d, "dbc": round(db - main[1], 1), "rise_db": rise,
                                  "rx_centre_from_tx_dial_hz": cen - f, "is": what})
                t["pan"] = {"centre": cen, "width": wid, "channel_db": main[1], "up": named}
            res["tones"].append(t)
        results["overs"].append(res)
        say(f"{label}: TX meter {res['tx_meter_w']} W (ALC {res['tx_meter_alc']}), RX peak {res['peak_dbfs']} dBFS")
        for t in res["tones"]:
            pn = t.get("pan", {}).get("up", [])
            say(f"   {t['fa']:4d} Hz: image {t.get('image_dbc')} dBc at RF {t.get('image_rf_hz')} "
                f"(mirror err {t.get('image_mirror_err_hz')} Hz, {t.get('image_over_floor_db')} dB over the floor), "
                f"carrier {t.get('carrier_dbc')} dBc; others {t.get('others')}; panadapter "
                f"{[(u['rf_from_tx_dial_hz'], u['is'], u['dbc']) for u in pn]}")
        time.sleep(3.0)                             # the pad cools between overs
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
        setv(rxr, "sql", 0)                         # open: a shut squelch mutes the X6200's audio whole
        setv(rxr, "agc", a.rx_agc)
        setv(rxr, "att", a.rx_att)
        if a.rx_rfg is not None:
            setv(rxr, "rfg", a.rx_rfg)
        audio_live()
        time.sleep(1.0)
        bands = a.bands.split(",")
        first = bands[0]
        over(first, BANDS[first], int(a.offsets.split(",")[0]), bench.FIRST_PROBE_W)   # the session's first: 0.1 W
        for band in bands:
            offs = a.proof_offsets if band in a.proof_band.split(",") else a.offsets
            for off in [int(o) for o in offs.split(",")]:
                # an over the receiver's audio did not carry whole (its TCI over WiFi stalls) once more; a stop by
                # the watch ends the run
                for tries in (1, 2):
                    try:
                        res = over(band, BANDS[band], off, a.w)
                    except Refused as e:
                        if str(e).startswith("stopped:"):
                            raise
                        say(f"{band} m, offset {off}: {e}")
                        results["overs"].append({"band": band, "off": off, "w_asked": a.w, "error": str(e), "tones": []})
                        res = None
                    if res and not res.get("error") and all("error" not in t for t in res["tones"]):
                        break
                    if tries == 1:
                        say(f"{band} m, offset {off}: once more")
        summary = []
        for o in results["overs"]:
            for t in o.get("tones", []):
                summary.append({"band": o["band"], "w": o["w_asked"], "off": o["off"], "fa": t["fa"],
                                "image_dbc": t.get("image_dbc"), "image_rf_from_dial": None if t.get("image_rf_hz") is
                                None else t["image_rf_hz"] - o["f"], "mirror_err_hz": t.get("image_mirror_err_hz"),
                                "carrier_dbc": t.get("carrier_dbc"), "tx_w": o.get("tx_meter_w"),
                                "pan_far": [(u["rf_from_tx_dial_hz"], u["dbc"]) for u in t.get("pan", {}).get("up", [])
                                            if u["is"] == "far"]})
        results["summary"] = summary
        say("summary:")
        for s in summary:
            say(f"   {s['band']:>3}m {s['w']:>4} W off {s['off']} fa {s['fa']}: image {s['image_dbc']} dBc at dial "
                f"{s['image_rf_from_dial']} Hz (err {s['mirror_err_hz']}), carrier {s['carrier_dbc']} dBc, TX {s['tx_w']} W"
                f"{', far on the panadapter ' + str(s['pan_far']) if s['pan_far'] else ''}")
    finally:
        if rx_audio[0]:
            rx_audio[0].close()
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
