#!/usr/bin/env python3
"""A radio's microphone on G, through the air: stepping tones from the other radio's own speaker into its mic, on the
air in USB, and back to that speaker radio's receiver (the owner, 2026-11-22: "do a series of step tones down the TX,
and try to record them on the other radio's mic and confirm they were heard"; "it should have been the internal RX",
never an RTL stick). Two radios side by side, joined through the radios profile's pads.

    SPEAKER  plays tools' stepping tones on its own speaker (the bench player: "play /tmp/bench_steps.wav", the base
             in play mode; its spectrum still shows its receiver there, 2026-11-22: 42.7 dB against 43.8) and listens
             (receive only) on the dial through its own receiver
    MIC      on G, armed, keyed with PTT in USB at 0.1 W: its mic (its Input as set: Auto takes the built-in one with
             no hand mic plugged in) hears the speaker; what it sends is the over

Confirmed one of two ways, the report saying which. (1) Every step on SPEAKER's spectrum at dial + tone, in order (each
at least STEP_FRAMES frames 15 dB over the quiet): the X6100's spectrum resolves them (a 1000 Hz tone peaks there,
-9 dB at +-200 Hz). (2) The X6200's remote spectrum does not: a single clean tone fills its passband 0-2500 Hz alike at
any zoom or gain (2026-11-22), so with the X6200 listening the steps are confirmed in the X6100's own capture of its
over (G raises its TX monitor onto the processor's capture line) and their arrival on the X6200's receiver (its
passband RISE_DB over the quiet through PRESENT_SHARE of the steps' frames). The bench's
rules throughout (bench.py: both said whole and read back, the path proved first, the watch, a supply that holds,
both put back after and left on the Sentry's page A). A report in ~/.config/x6x00-bench/runs/<time>-acoustic/.

    acoustic.py SPEAKER MIC [--freq 14200000] [--vol 35] [--tones 2000,1700,1400,1100,800,500] [--step-s 1.0]
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, json, math, os, subprocess, sys, time, wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from bench import Radio, Watch, Refused, chain_gate, radio_tx_gate, tx_log, say  # noqa: E402

FS = 44100
LEAD_S, TAIL_S = 0.3, 0.4
LEVEL = 0.5                  # each step at half scale (-6 dBFS), 20 ms ramps
STEP_FRAMES = 3              # frames of a step, at least, on the speaker radio's spectrum
RISE_DB = 15                 # a step's bin over the quiet, at least
PRESENT_SHARE = 0.8          # the speaker radio's passband up RISE_DB through this share of the steps' frames
WAV_ON_RADIO = "/tmp/bench_steps.wav"


def steps_wav(path, tones, step_s):
    sig = [np.zeros(int(LEAD_S * FS))]
    r = int(0.02 * FS)
    for hz in tones:
        t = np.arange(int(step_s * FS)) / FS
        s = LEVEL * np.sin(2 * np.pi * hz * t)
        s[:r] *= np.linspace(0, 1, r)
        s[-r:] *= np.linspace(1, 0, r)
        sig.append(s)
    sig.append(np.zeros(int(TAIL_S * FS)))
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes((np.concatenate(sig) * 32767).astype("<i2").tobytes())
    return sum(1 for _ in tones) * step_s + LEAD_S + TAIL_S


def staircase(points, tones, tol):
    """points: [(t, offset Hz, rise dB)] of the frames whose strongest bin rose RISE_DB. Each tone: its frames (within
    tol of it), their median time; heard in order when every tone has STEP_FRAMES frames and their times rise"""
    out = []
    for hz in tones:
        ts = [t for t, off, rise in points if abs(off - hz) <= tol]
        out.append({"tone": hz, "frames": len(ts), "at": round(float(np.median(ts)), 2) if ts else None})
    heard = all(o["frames"] >= STEP_FRAMES for o in out)
    ordered = heard and all(a["at"] < b["at"] for a, b in zip(out, out[1:]))
    return out, heard, ordered


def spectrum_points(r, f, t0, t1, quiet):
    """r's spectrum frames in t0..t1: each one's strongest bin between f + 250 and f + 2700 Hz, its offset from f and
    its rise over the same bin in the quiet frames (a mean per bin)"""
    pts = []
    for t, cen, wid, v in list(r.r.spectra):
        if not (t0 <= t <= t1) or not v:
            continue
        per = wid / len(v)
        a, b = int((f + 250 - (cen - wid / 2)) / per), int((f + 2700 - (cen - wid / 2)) / per)
        a, b = max(a, 0), min(b, len(v) - 1)
        if b <= a:
            continue
        seg = np.array(v[a:b + 1], float)
        q = quiet.get((cen, wid, len(v)))
        base = q[a:b + 1] if q is not None else np.full(len(seg), np.median(seg))
        j = int(np.argmax(seg - base))
        pts.append((t, cen - wid / 2 + (a + j + 0.5) * per - f, float(seg[j] - base[j])))
    return [p for p in pts if p[2] >= RISE_DB], (per if pts else None)


def quiet_bins(r, t0, t1):
    acc = {}
    for t, cen, wid, v in list(r.r.spectra):
        if t0 <= t <= t1 and v:
            acc.setdefault((cen, wid, len(v)), []).append(np.array(v, float))
    return {k: np.mean(vs, axis=0) for k, vs in acc.items()}


def audio_points(x, tones, fs=FS):
    """The mic radio's own capture: each 0.1 s its strongest tone in 250-2700 Hz over the band's median"""
    pts, n = [], int(0.1 * fs)
    for i in range(0, len(x) - n, n):
        seg = x[i:i + n]
        sp = np.abs(np.fft.rfft(seg * np.hanning(n)))
        fr = np.fft.rfftfreq(n, 1 / fs)
        m = (fr > 250) & (fr < 2700)
        k = int(np.argmax(np.where(m, sp, 0)))
        rise = 20 * math.log10(sp[k] / (np.median(sp[m]) + 1e-9))
        if rise >= 20:
            pts.append((i / fs, float(fr[k]), rise))
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("speaker")
    ap.add_argument("mic")
    ap.add_argument("--freq", type=int, default=14200000)
    ap.add_argument("--vol", type=int, default=35, help="the speaker radio's volume while it plays the steps")
    ap.add_argument("--tones", default="2000,1700,1400,1100,800,500")
    ap.add_argument("--step-s", type=float, default=1.0)
    # the speaker radio's receiver for the over (PRE and ATT said, never taken as found)
    ap.add_argument("--rx-att", type=int, default=0)
    ap.add_argument("--rx-pre", type=int, default=0)
    ap.add_argument("--rx-rfg", type=int, default=100)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    tones = [int(t) for t in a.tones.split(",")]
    c = bench.conf()
    p, total = chain_gate(c, "radios")
    spk, mic = Radio(a.speaker, c["radios"][a.speaker], False), Radio(a.mic, c["radios"][a.mic], True)
    radios = [spk, mic]
    run_dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-acoustic") + (f"-{a.tag}" if a.tag else ""))
    os.makedirs(run_dir, exist_ok=True)
    wav = os.path.join(run_dir, "steps.wav")
    play_s = steps_wav(wav, tones, a.step_s)
    if play_s + 2.0 > c["limits"]["max_key_s"] - 1:
        raise Refused(f"{play_s:.1f} s of steps is too long for one over ({c['limits']['max_key_s']:.0f} s at most)")
    results = {"speaker": a.speaker, "mic": a.mic, "f": a.freq, "tones": tones, "vol": a.vol, "pads_db": total}
    snaps, watch, recording = {}, None, False
    for r in radios:
        r.take_control()
        snaps[r.name] = bench.snapshot(r)
    try:
        results["supply"] = {r.name: bench.listener_gate(r) for r in radios}
        for r in radios:
            bench.open_page(r, "G")
        # the speaker radio: its own receiver on the dial (USB), AGC auto, its volume up for the room; the mic radio:
        # USB at 0.1 W on the dial, out of play mode (its mics live); the path proved first
        results["speaker_rx"] = bench.ensure(spk, *bench.rx_profile(a.freq, 2, pre=a.rx_pre, att=a.rx_att,
                                                                    rfg=a.rx_rfg, agc=3))
        bench.path_proof(c, total, mic, spk, a.freq, results)
        results["speaker_rx"].update(bench.ensure(spk, {"vol": a.vol}))
        results["mic_tx"] = bench.ensure(mic, *bench.tx_profile(a.freq, 2, bench.FIRST_PROBE_W, mic=True))
        results["mic_input"] = {0: "built-in", 1: "hand", 2: "auto"}.get(mic.state.get("mic"), mic.state.get("mic"))
        if spk.simulated:
            spk.send(f"sim plan {json.dumps([[0, LEAD_S]] + [[t, a.step_s] for t in tones] + [[0, TAIL_S]])}")
        else:
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", wav, f"root@{spk.host}:{WAV_ON_RADIO}"], check=True)
        radio_tx_gate(mic, c, bench.FIRST_PROBE_W)
        for r in radios:
            r.send(f"banner BENCH MIC ON G: {spk.name.upper()} PLAYS {'-'.join(map(str, tones))} HZ ON ITS SPEAKER, "
                   f"{mic.name.upper()} SENDS ITS MIC USB {bench.FIRST_PROBE_W} W {a.freq / 1e6:.3f} MHZ, "
                   f"{spk.name.upper()} LISTENS (RX ONLY) THROUGH {total} DB")
        time.sleep(1.5)
        q0 = time.time() - 1.2
        quiet = quiet_bins(spk, q0, time.time())
        cap = mic.state.get("fp", "").startswith("x6100") and not mic.simulated   # its monitor reaches its capture
        if cap:
            bench.ssh(mic.host, "rm -f /tmp/bench_mic.raw; (parecord --server=/var/run/pulse/native -d "
                                "alsa_input.platform-sound.stereo-fallback --raw --format=s16le --channels=1 "
                                "--rate=44100 /tmp/bench_mic.raw >/dev/null 2>&1 &)")
            recording, rec0 = True, time.time()
        watch = Watch(radios, c, expect_dbm=10 * math.log10(bench.FIRST_PROBE_W * 1000) - total, listener=spk,
                      w=bench.FIRST_PROBE_W)
        bench.arm(mic, True, mode=2)
        tx_log({"who": mic.name, "f": a.freq, "w": mic.pwr(), "what": f"acoustic {a.tag}"})
        n, on_at, t_play, t_end, t0 = 0, None, None, None, time.time()
        while not watch.why and time.time() - t0 < c["limits"]["max_key_s"] - 1.5:
            n += 1
            mic.send(f"key PTT 1 {n}")                   # the lease, renewed each half second
            if on_at is None and mic.tx:
                on_at = time.time()
            if on_at and t_play is None and time.time() - on_at > 0.8:
                spk.send(f"play {WAV_ON_RADIO}")
                spk.wait("the speaker playing", lambda: spk.state.get("plays") == 1, 4)
                t_play = time.time()
            if t_play and spk.state.get("plays") == 0:
                t_end = time.time()
                break
            if on_at is None and time.time() - t0 > 6:
                raise Refused(f"{mic.name} did not come on the air with PTT")
            time.sleep(0.5)
        time.sleep(0.3)
        mic.send("key PTT 0")
        spk.send("play stop")
        t_off = time.time()
        try:
            mic.wait("off the air", lambda: not mic.tx, 5)
        except Refused:
            pass
        watch.close()
        bench.arm(mic, False)
        if recording:
            bench.ssh(mic.host, "kill $(pidof parecord) 2>/dev/null; true")
            recording = False
        if watch.why:
            raise Refused(f"stopped: {watch.why}")
        if t_play is None:
            raise Refused(f"{spk.name}'s speaker never played the steps")
        results["on_air_s"] = round(t_off - on_at, 1)
        results["mic_peak_w"] = watch.tx_peak.get(mic.name)
        # the speaker radio's receiver: the staircase on its spectrum, in order
        pts, per = spectrum_points(spk, a.freq, t_play - 0.5, t_off + 0.5, quiet)
        steps, heard, ordered = staircase(pts, tones, max(60, 1.5 * (per or 0)))
        frames = [(t, cen, wid, v) for t, cen, wid, v in list(spk.r.spectra) if q0 - 1.0 <= t <= t_off + 0.5]
        np.savez(os.path.join(run_dir, "speaker_frames.npz"), t=np.array([x[0] - t_play for x in frames]),
                 cen=np.array([x[1] for x in frames]), wid=np.array([x[2] for x in frames]),
                 v=np.array([np.array(x[3], np.int8) for x in frames]))
        results.update({"speaker_spectrum": steps, "heard": heard, "in_order": ordered, "bin_hz": per,
                        "frames_per_s": round(len(frames) / max(t_off - t_play + 1.0, 0.1), 1),
                        "points": [(round(t - t_play, 2), round(off), round(rise, 1)) for t, off, rise in pts]})
        say(f"{spk.name}'s receiver, the steps on its spectrum at the dial + tone: " + ", ".join(
            f"{s['tone']} Hz {s['frames']} frames" for s in steps) + f" -> {'HEARD' if heard else 'NOT heard'}"
            + (", in order" if ordered else ""))
        if cap:
            subprocess.run(["scp", "-q", f"root@{mic.host}:/tmp/bench_mic.raw", os.path.join(run_dir, "mic.raw")],
                           capture_output=True)
            try:
                x = np.fromfile(os.path.join(run_dir, "mic.raw"), dtype=np.int16).astype(float)
                apts = [(rec0 + t, off, rise) for t, off, rise in audio_points(x, tones)]
                msteps, mheard, mordered = staircase(apts, tones, 40)
                results.update({"mic_capture": msteps, "mic_capture_heard": mheard, "mic_capture_in_order": mordered})
                say(f"{mic.name}'s own capture of its over (its monitor): " + ", ".join(
                    f"{s['tone']} Hz {s['frames']}" for s in msteps) + f" -> {'all' if mheard else 'NOT all'} there")
            except (OSError, ValueError) as e:
                say(f"{mic.name}'s capture not read: {e}")
        # its arrival: the speaker radio's passband over its quiet through the steps (any spectrum shows that much)
        present = []
        for t, cen, wid, v in list(spk.r.spectra):
            if not (t_play + 0.4 <= t <= t_play + play_s - TAIL_S - 0.2) or not v:
                continue
            per_ = wid / len(v)
            a_, b_ = int((a.freq + 300 - (cen - wid / 2)) / per_), int((a.freq + 2700 - (cen - wid / 2)) / per_)
            q = quiet.get((cen, wid, len(v)))
            if q is not None and b_ > a_:
                present.append(float(np.max(np.array(v[a_:b_ + 1], float) - q[a_:b_ + 1])) >= RISE_DB)
        results["arrived_share"] = round(sum(present) / len(present), 2) if present else None
        arrived = bool(present) and sum(present) / len(present) >= PRESENT_SHARE
        if heard and ordered:
            results["confirmed_by"] = f"the steps in order on {spk.name}'s spectrum"
        elif arrived and results.get("mic_capture_in_order"):
            results["confirmed_by"] = (f"the steps in order in {mic.name}'s own capture of its over, and the over "
                                       f"on {spk.name}'s receiver through {results['arrived_share']:.0%} of them")
        if watch.deaf:
            results["deaf"] = watch.deaf
    finally:
        if recording:
            try:
                bench.ssh(mic.host, "kill $(pidof parecord) 2>/dev/null; true")
            except Refused:
                pass
        try:
            spk.send("play stop")
        except OSError:
            pass
        bench.finish(radios, snaps, report=results)
        if results.get("not_restored"):        # a radio not as found: the run fails, whatever it measured
            say(f"NOT put back: {results['not_restored']}")
        with open(os.path.join(run_dir, "report.json"), "w") as f:
            json.dump(results, f, indent=1, default=str)
        say("report", os.path.join(run_dir, "report.json"))
    if not results.get("confirmed_by"):
        raise Refused(f"{a.mic}'s mic over of {a.speaker}'s steps not confirmed: not in order on {a.speaker}'s spectrum"
                      f", and {'not in ' + a.mic + chr(39) + 's own capture' if not results.get('mic_capture_in_order') else 'arrived through only ' + str(results.get('arrived_share'))}")
    say(f"PASS: {a.mic}'s mic on G carried {a.speaker}'s steps: {results['confirmed_by']}")


if __name__ == "__main__":
    bench.install_stop_signals(lambda: bench.conf())
    try:
        main()
        if bench.load_json(os.path.join(bench.STATE, bench.LEFT_WRONG), {}):
            say("a radio is recorded as left not as found: bench.py restore")
            sys.exit(3)
    except Refused as e:
        say(f"REFUSED: {e}")
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
