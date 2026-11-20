#!/usr/bin/env python3
"""The transmitter's calibration, band by band, measured by the other radio (the owner, 2026-11-20: the image and the
carrier "feels like something we'd want to fix ... please make it so"). TX sends a steady USB tone FA; RX listens in
USB with its dial OFF Hz below (image.py's geometry: the wanted tone at OFF + FA in RX's audio, TX's carrier at OFF,
TX's image at OFF - FA; RX's own image falls in its other sideband, left out). TX's band settings are changed through
its remote:

    image     txig, txip: the band's TX I/Q balance, gain and phase (register 52, GoldCorpHF's base gcfix5 on)
    carrier   txio, txqo: the band's TX I and Q DC offsets (Xiegu's registers 50/51, Settings > TX CAL)

The base takes all four only between transmissions (its register handler does not run while it sends: values set
during an over came out at the next, 2026-11-20), so each point is an over of its own (a 1.6 s tone, its middle second
measured). The two pairs come out at their own audio frequencies and do not touch each other, so each over moves one
of each and measures both. Each pair's residual power is a bowl, P = k ((a - a0)^2 + (b - b0)^2) + floor (the
residual phasor's two parts): five points (the centre and +-D on each axis) give its bottom by a parabola on each
axis, a second pass at D/3 refines it, and the last point measures it: 11 overs a band. The session's first over is a short one at 0.1 W (bench rule 4),
then everything at --w (5 W). What is found stays in TX's band settings (the GUI saves them) when it is better than
what was there; --dry measures and puts the old values back. Afterwards every band's values are saved to
calibrations/<the radio's fingerprint>.json (txcal_store.py: tied to that radio's processor, put back on it alone). The bench rules throughout (bench.py); RX's squelch open,
AGC 3, attenuator on, everything put back after. A report in ~/.config/x6x00-bench/runs/<time>-txcal/.

    txcal.py TX RX [--bands 160,80,...] [--w 5] [--fa 1000] [--off 1500] [--dry] [--d-image N] [--d-carrier N]
                   [--passes 2] [--from-zero]
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, json, math, os, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from bench import Radio, Watch, Refused, chain_gate, radio_tx_gate, tx_log, say  # noqa: E402
from tones import FS, SENDER, STOP_SENDER, TciRx, peak  # noqa: E402
from image import BANDS, band_onset  # noqa: E402

LEAD_S, TAIL_S = 0.4, 0.6
POINT_TONE_S = 1.6                            # a carrier point's over: its tone's middle second measured
KEYS = {"carrier": ("txio", "txqo", 1500, 10000), "image": ("txig", "txip", 1500, 32767)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tx")
    ap.add_argument("rx")
    ap.add_argument("--bands", default=",".join(BANDS))
    ap.add_argument("--w", type=float, default=5.0)
    ap.add_argument("--fa", type=int, default=1000)
    ap.add_argument("--off", type=int, default=1500)
    ap.add_argument("--amp", type=float, default=0.3)
    ap.add_argument("--dry", action="store_true", help="measure only: the old values back after")
    ap.add_argument("--d-carrier", type=int, default=None, help="the carrier search's first step (1500)")
    ap.add_argument("--d-image", type=int, default=None, help="the image search's first step (800)")
    ap.add_argument("--passes", type=int, default=2, help="passes of five points before the last (2)")
    ap.add_argument("--from-zero", action="store_true",
                    help="start each band at 0, 0 (uncalibrated) instead of its present values")
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    c = bench.conf()
    p, total = chain_gate(c, "radios")
    txr, rxr = Radio(a.tx, c["radios"][a.tx], True), Radio(a.rx, c["radios"][a.rx], True)
    radios = [txr, rxr]
    run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-txcal") + (f"-{a.tag}" if a.tag else ""))
    os.makedirs(run_dir, exist_ok=True)
    keys = ("freq", "mode", "pg", "atu", "agc", "att", "pre", "rfg", "sql")
    was, results = {}, {"tx": a.tx, "rx": a.rx, "w": a.w, "fa": a.fa, "off": a.off, "bands": {}}
    lim_s = bench.FULL_RATING_KEY_S if a.w > bench.HARD_MAX_W else c["limits"]["max_key_s"]
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", SENDER, f"root@{txr.host}:/tmp/tci_tone_tx.py"], check=True)
    rx_audio = [None]

    def audio_live():
        t = rx_audio[0]
        if t is not None:
            with t.lock:
                last = t.frames[-1][0] if t.frames else 0
            if time.time() - last < 2.0:
                return t
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
            if int(r.state.get(k, -99999)) == v:
                return
            r.send(f"set {k} {v}")
            try:
                r.wait(f"{k} {v}", lambda: int(r.state.get(k, -99999)) == v, 6)
                return
            except Refused:
                pass
        raise Refused(f"{r.name}: {k} {v} not taken")

    def setw(w):
        txr.send(f"set pwr {round(w * 10)}")
        txr.wait(f"{w} W", lambda: abs(txr.pwr() - w) < 0.05, 5)

    def over(f, w, tone_s, label, during=None):
        """One transmission of the tone (tone_s seconds after a lead) with every gate and the watch; during(ctx) runs
        while it is on air. Returns (the audio, its start on this clock, ctx: the lag and the tone's start)"""
        setv(txr, "freq", f)
        setv(rxr, "freq", f - a.off)
        setv(txr, "mode", 2)
        setv(rxr, "mode", 2)
        setw(w)
        plan = [[0, LEAD_S], [a.fa, tone_s], [0, TAIL_S]]
        key_s = sum(s for _, s in plan) + 1.0
        if key_s > lim_s - 1:
            raise Refused(f"{key_s:.1f} s on the air is over the limit ({lim_s:.0f} s at {w} W)")
        radio_tx_gate(txr, c, w)
        tci = audio_live()
        for r in radios:
            r.send(f"banner BENCH TX CAL: {a.tx.upper()} SENDS A TONE {w} W, {a.rx.upper()} LISTENS, "
                   f"{f / 1e6:.3f} MHZ THROUGH {total} DB")
        watch = Watch(radios, c, expect_dbm=10 * math.log10(w * 1000) - total, listener=rxr, w=w)
        arm(txr, True)
        tx_log({"who": txr.name, "f": f, "w": txr.pwr(), "what": f"txcal {label}"})
        t_begin = time.time()
        proc = subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}",
                                 f"python3 /tmp/tci_tone_tx.py '{json.dumps(plan)}' {a.amp} {key_s:.1f} 50001 0.05"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        sent = []
        threading.Thread(target=lambda: [sent.append(l) for l in proc.stdout], daemon=True).start()
        ctx = {"tci": tci, "lag": None, "t_begin": t_begin, "watch": watch, "proc": proc}
        try:
            # the tone in RX's audio: its start there against the sender's (after its lead) is how late the audio
            # runs behind this clock (the radios keep NTP time)
            # (the audio from 0.3 s after the key: the last over's tail, a lag behind, kept out of the quiet start
            # the onset is measured from)
            t_end = time.time() + 9
            while ctx["lag"] is None:
                if time.time() > t_end or watch.why:
                    if not watch.why:
                        x, x_t0 = tci.audio(t_begin + 0.3, None)
                        say(f"   no tone: sender {[l.strip() for l in sent][-3:]}, {len(x) / FS:.1f} s of audio")
                    raise Refused("stopped: " + (watch.why or ("the sender ended" if proc.poll() is not None
                                                               else "no tone")))
                x, x_t0 = tci.audio(t_begin + 0.3, None)
                ss = next((float(l.split()[1]) for l in list(sent) if l.startswith("start ")), None)
                on = band_onset(x, a.off + a.fa) if x_t0 is not None and len(x) > FS // 2 else None
                if on is not None and ss is not None:
                    ctx["lag"] = max(0.0, (x_t0 + on) - (ss + LEAD_S))
                    ctx["tone_at"] = x_t0 + on      # the tone's start, on the audio's clock
                time.sleep(0.05)
            if during:
                during(ctx)
            proc.wait(timeout=max(5, tone_s + 5))
            time.sleep(ctx["lag"] + 0.3)
        finally:
            if proc.poll() is None:
                subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{txr.host}", STOP_SENDER],
                               capture_output=True, text=True)
                proc.wait(timeout=30)
            watch.close()
            try:
                txr.wait("off the air", lambda: not txr.tx, 5)
            except Refused:
                pass
            arm(txr, False)
        if watch.why:
            raise Refused(f"stopped: {watch.why}")
        x, x_t0 = tci.audio(t_begin - 1.0, None)
        time.sleep(0.8)
        return x, x_t0, ctx

    def level(seg, target):
        """(target over the wanted, dB; the wanted, dBFS)"""
        _, lw = peak(seg, a.off + a.fa, 40)
        _, lt = peak(seg, target, 15)
        return lt - lw, lw

    def vertex(pm, p0, pp, dd):
        curv = pm - 2 * p0 + pp
        if curv <= 0:                                # no bowl seen (noise): toward the lower side
            return dd if pp < pm else -dd
        return max(-2 * dd, min(2 * dd, dd * (pm - pp) / (2 * curv)))

    def search(band, f):
        """The band's image (txig, txip) and carrier (txio, txqo) together: they come out at their own audio
        frequencies and do not touch each other, so each over moves one of each and measures both. One over a point:
        the base takes registers 50-52 between transmissions only"""
        ki, kc = KEYS["image"], KEYS["carrier"]
        setv(txr, "freq", f)                         # the band first: its own values are the ones read and set
        time.sleep(0.5)
        si = (int(txr.state.get(ki[0], 0)), int(txr.state.get(ki[1], 0)))
        sc = (int(txr.state.get(kc[0], 0)), int(txr.state.get(kc[1], 0)))
        if a.from_zero:
            # (the state may still show the last band's values a moment after the band changed: a search from
            # there began far off on 15 and 12 m, 2026-11-20)
            si, sc = (0, 0), (0, 0)
        di, dc = a.d_image or ki[2], a.d_carrier or kc[2]
        rec = {"band": band, "w": a.w, "start": {"image": si, "carrier": sc}, "points": []}

        def clamp(v, lim):
            return int(max(-lim, min(lim, round(v))))

        def point(ia, ib, ca, cb):
            ia, ib, ca, cb = clamp(ia, ki[3]), clamp(ib, ki[3]), clamp(ca, kc[3]), clamp(cb, kc[3])
            for k, v in ((ki[0], ia), (ki[1], ib), (kc[0], ca), (kc[1], cb)):
                setv(txr, k, v)
            time.sleep(0.3)
            for tries in (1, 2, 3):                  # an over whose tone did not come through: once more
                try:
                    x, x_t0, ctx = over(f, a.w, POINT_TONE_S, f"{band}m cal")
                    break
                except Refused as e:
                    if not str(e).startswith(("stopped: the sender", "stopped: no tone")) or tries == 3:
                        raise
                    say(f"   {e}: the point once more")
                    time.sleep(2.0)
            i0 = int((ctx["tone_at"] + 0.4 - x_t0) * FS)
            seg = x[i0:i0 + int(1.0 * FS)]
            img, lw = level(seg, a.off - a.fa)
            car, _ = level(seg, a.off)
            rec["points"].append({"image": [ia, ib, round(img, 2)], "carrier": [ca, cb, round(car, 2)],
                                  "wanted_dbfs": round(lw, 1)})
            say(f"   {ki[0]}={ia} {ki[1]}={ib}: image {img:.1f} dBc | {kc[0]}={ca} {kc[1]}={cb}: carrier {car:.1f} dBc"
                f" (wanted {lw:.1f} dBFS)")
            return 10 ** (img / 10), 10 ** (car / 10)

        ia, ib = si
        ca, cb = sc
        for _ in range(a.passes):
            p0 = point(ia, ib, ca, cb)
            pp_a = point(ia + di, ib, ca + dc, cb)
            pm_a = point(ia - di, ib, ca - dc, cb)
            pp_b = point(ia, ib + di, ca, cb + dc)
            pm_b = point(ia, ib - di, ca, cb - dc)
            ia += vertex(pm_a[0], p0[0], pp_a[0], di)
            ib += vertex(pm_b[0], p0[0], pp_b[0], di)
            ca += vertex(pm_a[1], p0[1], pp_a[1], dc)
            cb += vertex(pm_b[1], p0[1], pp_b[1], dc)
            di, dc = max(di / 3, 40), max(dc / 3, 40)
        point(ia, ib, ca, cb)
        pts = rec["points"]
        out = {}
        for what, kk, st in (("image", ki, si), ("carrier", kc, sc)):
            first, last = pts[0][what], pts[-1][what]
            best = min((p[what] for p in pts), key=lambda v: v[2])
            if a.dry:
                kept = list(st)
            elif last[2] <= first[2]:
                kept = last[:2]
            else:
                kept = best[:2] if best[2] < first[2] else list(st)
            setv(txr, kk[0], int(kept[0]))
            setv(txr, kk[1], int(kept[1]))
            out[what] = {"start_db": first[2], "result": last[:2], "result_db": last[2], "best": best, "kept": kept}
            say(f"{band} m {what}: {list(st)} {first[2]} dBc -> {last[:2]} {last[2]} dBc (best seen {best[:2]} "
                f"{best[2]} dBc), kept {kept}")
        rec.update(out)
        return rec

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
        setv(rxr, "sql", 0)
        setv(rxr, "agc", 3)
        setv(rxr, "att", 1)
        audio_live()
        bands = a.bands.split(",")
        over(BANDS[bands[0]], bench.FIRST_PROBE_W, 1.0, "probe")     # the session's first over: 0.1 W
        for band in bands:
            for tries in (1, 2):
                try:
                    results["bands"][band] = search(band, BANDS[band])
                    break
                except Refused as e:
                    if not str(e).startswith(("stopped: the sender", "stopped: no tone")) or tries == 2:
                        raise
                    say(f"{band} m: {e}: once more")
        say("summary:")
        for band, r in results["bands"].items():
            say(f"   {band:>3} m: image {r['image']['start_db']} -> {r['image']['result_db']} dBc, kept "
                f"{r['image']['kept']}; carrier {r['carrier']['start_db']} -> {r['carrier']['result_db']} dBc, kept "
                f"{r['carrier']['kept']}")
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
    if not a.dry and results["bands"]:
        # the radio's calibration as it now is, every band's, into calibrations/<its fingerprint>.json
        # (txcal_store.py: tied to the radio, never put on another)
        subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "txcal_store.py"),
                        "save", a.tx])


if __name__ == "__main__":
    main()
