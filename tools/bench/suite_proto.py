"""More of the 1:1 suite (suite_more.py): the protocols the radios speak and the guards on them, and the radios' own
system read over SSH. One test per feature of features.json, on each radio and 1:1 (suite_more.per_radio: its own
session, the radio put back and left on page A). Nothing here keys a radio: the starts that must be refused are in
suite_more's safety batch, on the cable at 0.1 W under the Watch.

    control: tci-proto (dds, start/stop, the fixed answers, the commands left unanswered, CW speed asked and set,
             the S-meter sensors, the audio format and stream), tci-drive, tci-guards (four clients, the Origin),
             panel (hello, keys, bands and plan, ping, the clock, the S-meter's rate, the battery, kt/cwrise, AM's
             filter, a refused set, play's guard, the wide spectrum), control-owner (control needed, lost with the
             link, the first to tune takes it, a TCI tune takes it and a question does not), panel-apps (FT8 holding
             the dial, its mode/TX Hz/HOLD, WeFax), screen (JPEG frames, the half-size picture, screen 0, four
             viewers, the Origin)
    system:  every radio read over SSH (battery log, DATA's fsck, the good copy of the settings, SSH keys only,
             Tailscale, the run logs kept, the start trace, the services, the processor, temperatures, the GPU, the
             sessions file, the update state, the ADIF header, the clock)

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import json
import os
import socket
import struct
import subprocess
import time

import bench
from remote_ws import Remote
from suite_more import F_TEST, per_radio, say, tci_session

HERE = os.path.dirname(os.path.abspath(__file__))


# ---- helpers ----

def texts(t, secs):
    """Every text a Remote got within secs"""
    out, end = [], time.time() + secs
    while time.time() < end:
        m = t.recv(timeout=0.2)
        if m:
            out.append(m)
    return out


def expect(t, want, secs=3.0):
    """The first text equal to want (a "name:" prefix when want ends in ':'), or None; texts before it are dropped"""
    end = time.time() + secs
    while time.time() < end:
        m = t.recv(timeout=0.2)
        if m is None:
            continue
        if (m.lower().startswith(want.lower()) if want.endswith(":") else m.lower() == want.lower()):
            return m
    return None


def line_after(r, t0, prefix, secs=3.0):
    """The first text from the bench's session r since t0 that starts with prefix, waiting up to secs"""
    end = time.time() + secs
    while True:
        with r.lock:
            hit = next((x for at, x in list(r.lines) if at >= t0 and x.startswith(prefix)), None)
        if hit or time.time() > end:
            return hit
        time.sleep(0.05)


def fresh(host, port=50002, headers=None, secs=2.0):
    """A new connection that says nothing and stops its screen frames: (Remote, the texts it was greeted with)"""
    c = Remote(host, port, headers=headers)
    if port == 50002:
        c.send("screen 0")
    return c, texts(c, secs)


def jpeg_size(b):
    """A JPEG's own width and height from its SOF marker (b: its first bytes), or None"""
    i = 2
    while i + 9 < len(b):
        if b[i] != 0xFF:
            return None
        m, ln = b[i + 1], struct.unpack_from(">H", b, i + 2)[0]
        if m in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack_from(">HH", b, i + 5)
            return w, h
        i += 2 + ln
    return None


def refused_ws(host, port, headers):
    """Whether a WebSocket with these headers is refused at the handshake (no 101)"""
    try:
        c = Remote(host, port, timeout=6, headers=headers)
    except (ConnectionError, OSError) as e:
        return True, str(e)[:80]
    c.close()
    return False, c.status


# ---- control: TCI ----

TCI_STUBS = [("tx_enable:0;", "tx_enable:0,true;"), ("rx_enable:0;", "rx_enable:0,true;"),
             ("lock:0;", "lock:0,false;"), ("split_enable:0;", "split_enable:0,false;"),
             ("xit_enable:0;", "xit_enable:0,false;"), ("mute;", "mute:0,false;"), ("rx_mute:0;", "rx_mute:0,false;"),
             ("xit_offset:0;", "xit_offset:0,0;"), ("volume;", "volume:0;"), ("mon_volume;", "mon_volume:0;"),
             ("rx_volume:0,0;", "rx_volume:0,0,0;"), ("rx_balance:0,0;", "rx_balance:0,0,0;"),
             ("tune:0;", "tune:0,false;")]
TCI_UNANSWERED = ["cw_msg:0,Q9TST;", "cw_terminal:0,false;", "cw_macros_delay:100;", "cw_macros_speed_up:5;",
                  "cw_macros_speed_down:5;", "keyer:0,false;"]


def greeted(greet, name):
    """A greeting line's value ("audio_samplerate:48000;" -> "48000")"""
    g = next((x for x in greet if x.lower().startswith(name + ":")), None)
    return g.split(":", 1)[1].rstrip(";") if g else None


def t_tci_proto(run):
    def answers(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        t, greet = tci_session(r.host)
        bad, said = [], []
        try:
            t.send("dds:0;")
            m = expect(t, "dds:")
            t.send("dds:0,7100000;")
            m2 = expect(t, "dds:")
            time.sleep(0.6)
            if m != f"dds:0,{F_TEST};" or m2 != f"dds:0,{F_TEST};" or int(r.state.get("freq", 0)) != F_TEST:
                bad.append(f"dds {m!r}, set {m2!r}, the dial {r.state.get('freq')}")
            else:
                said.append("dds answered with the dial, a dds set changing nothing")
            for c in ("start;", "stop;"):
                t.send(c)
                if expect(t, "start;") is None:
                    bad.append(f"{c} not answered start;")
            if int(r.state.get("freq", 0)) == F_TEST and "stop; not answered start;" not in bad:
                said.append("start; and stop; both answered start; (the radio running)")
            wrong = []
            for c, want in TCI_STUBS:
                t.send(c)
                got = expect(t, want.split(":")[0] + ":")
                if got != want:
                    wrong.append(f"{c} -> {got!r}")
            if wrong:
                bad.append("fixed answers: " + "; ".join(wrong))
            else:
                said.append(f"{len(TCI_STUBS)} fixed answers as the code has them")
            for c in TCI_UNANSWERED:
                t.send(c)
            heard = [x for x in texts(t, 1.5) if x.split(":")[0].lower() in
                     {c.split(":")[0] for c in TCI_UNANSWERED}]
            if heard:
                bad.append(f"answered what TCI leaves unanswered: {heard}")
            else:
                said.append("cw_msg, cw_terminal, cw_macros_delay, speed_up/down and keyer left unanswered")
            return not bad, "; ".join(said + bad)
        finally:
            t.close()
    per_radio(run, "control", "tci-answers", answers, ["tci-dds", "tci-start-stop", "tci-stub-answers",
                                                       "tci-cw-unsupported"])

    def speed(r):
        t, greet = tci_session(r.host)
        orig = greeted(greet, "cw_macros_speed")
        if orig is None or not orig.isdigit():
            t.close()
            return False, f"no cw_macros_speed in the greeting ({greet[:6]})"
        orig = int(orig)
        new = orig - 1 if orig > 6 else orig + 1
        said = []
        try:
            t.send("cw_keyer_speed;")
            asked = expect(t, "cw_keyer_speed:")
            time.sleep(0.8)
            kept = r.ctl_mine
            said.append(f"asked: {asked!r}, control {'stays with the bench' if kept else 'TAKEN by the question'}")
            t1 = time.time()
            t.send(f"cw_macros_speed:{new};")
            a = expect(t, f"cw_macros_speed:{new};", 4)
            b = expect(t, f"cw_keyer_speed:{new};", 2) if a else None
            took = line_after(r, t1, "ctl tci", 3) is not None       # the panel's word comes within its 50 ms tick
            said.append(f"set {orig} -> {new}: both heard {bool(a and b)}, control to TCI {took}")
            t.send("cw_keyer_speed:99;")
            top = expect(t, "cw_macros_speed:", 4)
            said.append(f"99 asked: {top!r} (clamped to the radio's top)")
            ok = asked == f"cw_keyer_speed:{orig};" and kept and bool(a and b) and took and top and \
                int(top.split(":")[1].rstrip(";")) <= 60
            return ok, "; ".join(said)
        finally:
            t.send(f"cw_keyer_speed:{orig};")
            back = expect(t, f"cw_keyer_speed:{orig};", 4) or expect(t, f"cw_macros_speed:{orig};", 2)
            t.close()
            if not back:
                say(f"{r.name}: KEY Speed not seen back at {orig} WPM: check it")
            time.sleep(0.5)
            r.take_control()
    per_radio(run, "control", "tci-cw-speed", speed, ["tci-cw-speed"])

    def sensors(r):
        """rx_smeter goes to every client ten a second; rx_channel_sensors only after rx_sensors_enable:true"""
        t, greet = tci_session(r.host)
        try:
            before = texts(t, 1.5)
            t.send("rx_sensors_enable:true;")
            ans = expect(t, "rx_sensors_enable:")
            on = texts(t, 3.0)
            t.send("rx_sensors_enable:false;")
            expect(t, "rx_sensors_enable:")
            time.sleep(0.8)
            off = texts(t, 2.0)

            def count(lines, name):
                return [x for x in lines if x.lower().startswith(name + ":")]
            sm_before, ch_before = count(before, "rx_smeter"), count(before, "rx_channel_sensors")
            ch_on, ch_off, sm_off = count(on, "rx_channel_sensors"), count(off, "rx_channel_sensors"), \
                count(off, "rx_smeter")
            dbm = None
            for x in ch_on:
                try:
                    dbm = float(x.rstrip(";").split(",")[-1])
                except ValueError:
                    pass
            ok = ans == "rx_sensors_enable:true;" and len(sm_before) >= 10 and not ch_before and len(ch_on) >= 20 \
                and dbm is not None and -160 < dbm < 0 and not ch_off and len(sm_off) >= 10
            return ok, (f"rx_smeter always: {len(sm_before)} in 1.5 s before, {len(sm_off)} in 2 s after; "
                        f"rx_channel_sensors: {len(ch_before)} before, {len(ch_on)} in 3 s enabled (the last "
                        f"{ch_on[-1] if ch_on else None!r}, {dbm} dBm), {len(ch_off)} after disabling")
        finally:
            t.close()
    per_radio(run, "control", "tci-rx-sensors", sensors, ["tci-rx-smeter"])

    def audio(r):
        t, greet = tci_session(r.host)
        try:
            defaults = {k: greeted(greet, k) for k in ("audio_samplerate", "audio_stream_sample_type",
                                                       "audio_stream_channels", "audio_stream_samples")}
            want_def = {"audio_samplerate": "48000", "audio_stream_sample_type": "float32",
                        "audio_stream_channels": "2", "audio_stream_samples": "2048"}
            sets = [("audio_samplerate:24000;", "audio_samplerate:24000;"),
                    ("audio_stream_sample_type:int16;", "audio_stream_sample_type:int16;"),
                    ("audio_stream_channels:1;", "audio_stream_channels:1;"),
                    ("audio_stream_samples:512;", "audio_stream_samples:512;")]
            echoed = []
            for c, want in sets:
                t.send(c)
                echoed.append(expect(t, want, 3) is not None)
            n0 = t.binary
            t.send("audio_start:0;")
            started = expect(t, "audio_start:0;") is not None
            time.sleep(2.0)
            heads = [f for f in list(t.frames) if f[0] > time.time() - 2.0]
            n_on = t.binary - n0
            t.send("audio_stop:0;")
            stopped = expect(t, "audio_stop:0;") is not None
            time.sleep(0.8)
            n1 = t.binary
            time.sleep(1.5)
            n_after = t.binary - n1
            hdr = None
            if heads:
                at, ln, head = heads[-1]
                rcv, rate, fmt, codec, crc, length, typ, ch = struct.unpack_from("<8I", head, 0)
                hdr = dict(rate=rate, fmt=fmt, length=length, type=typ, ch=ch, bytes=ln)
            ok = defaults == want_def and all(echoed) and started and stopped and n_on >= 10 and n_after == 0 and \
                hdr and hdr["rate"] == 24000 and hdr["fmt"] == 0 and hdr["ch"] == 1 and hdr["length"] == 512 and \
                hdr["type"] == 1 and hdr["bytes"] == 64 + 512 * 2
            return ok, (f"greeting {defaults} (the defaults: 48k float32 stereo 2048); 24k int16 mono 512 set and "
                        f"echoed {all(echoed)}; audio_start: {n_on} frames in 2 s (24k / 512 is 47), the last "
                        f"{hdr}; audio_stop: {n_after} frames after")
        finally:
            t.close()
    per_radio(run, "control", "tci-audio", audio, ["tci-rx-audio-stream", "tci-audio-format"])


def t_tci_drive(run):
    """TCI's drive asked and set (lower: never more power than the radio had), read back on the remote's state;
    finish() puts the power back"""
    def fn(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2, "pwr": 1.0})
        t, greet = tci_session(r.host)
        try:
            d0 = greeted(greet, "drive")
            d0 = int(d0.split(",")[1]) if d0 and "," in d0 else None
            t.send("drive:0;")
            asked = expect(t, "drive:")
            if d0 is None or d0 < 2:
                return False, f"drive in the greeting {d0!r}: nothing lower to set"
            p0 = r.pwr()
            t.send(f"drive:0,{d0 - 1};")
            echo = expect(t, f"drive:0,{d0 - 1};", 4)
            r.wait("the power lower", lambda: r.pwr() < p0, 5)
            return echo is not None, (f"1 W reads drive {d0} % (asked: {asked!r}); drive {d0 - 1} set: echoed "
                                      f"{echo is not None}, the power {p0} -> {r.pwr()} W; nothing keyed")
        finally:
            t.close()
            time.sleep(0.5)
            r.take_control()
    per_radio(run, "control", "tci-drive", fn, ["tci-drive"], want_tx=True)


def t_tci_guards(run):
    def four(r):
        held, refused_at = [], None
        try:
            for k in range(5):
                try:
                    t, greet = tci_session(r.host)
                except (ConnectionError, OSError):
                    refused_at = k + 1
                    break
                if not any(g.startswith("ready") for g in greet):
                    refused_at = k + 1
                    t.close()
                    break
                held.append(t)
            return refused_at is not None and len(held) <= 4, \
                f"{len(held)} TCI clients taken, the next one refused ({refused_at}th of ours; 4 in all, " \
                f"{4 - len(held)} held by others)"
        finally:
            for t in held:
                t.close()
    per_radio(run, "control", "tci-four", four, ["tci-max-four-clients"])

    def origin(r):
        h = r.host
        evil, e1 = refused_ws(h, 50001, {"Origin": "http://evil.example"})
        rebind, e2 = refused_ws(h, 50001, {"Host": "evil.example:50001", "Origin": "http://evil.example"})
        pat, e3 = refused_ws(h, 50001, {"Origin": f"http://{h}:8080"})
        own, e4 = refused_ws(h, 50001, {"Origin": f"http://{h}"})
        prog, e5 = refused_ws(h, 50001, {})
        ok = evil and rebind and pat and not own and not prog
        return ok, (f"a page from another site refused {evil}, a rebound name refused {rebind}, Pat's mailbox (8080) "
                    f"refused {pat}; the radio's own page taken {not own} ({e4}), a program without Origin taken "
                    f"{not prog}")
    per_radio(run, "control", "tci-origin", origin, ["tci-origin-check"])


# ---- control: the remote's own protocol ----

def t_panel(run):
    seen = {}

    def greeting(r):
        c, g = fresh(r.host)
        c.close()
        c2, g2 = fresh(r.host)
        c2.send("auth " + "0" * 32)
        g2 += texts(c2, 1.5)
        c2.close()
        hello = next((x for x in g if x.startswith("hello ")), "")
        hello2 = next((x for x in g2 if x.startswith("hello ")), "")
        boot, boot2 = hello.rsplit(" ", 1)[-1], hello2.rsplit(" ", 1)[-1]
        keys = next((x for x in g if x.startswith("keys")), "").split()[1:]
        role = next((x for x in g if x.startswith("role ")), "")
        wrong = [x for x in g2 if x.startswith(("err unknown session", "role "))]
        size = next((x for x in g if x.startswith("size ")), "")
        x6200 = r.name == "x6200"
        no_keys = {"PRE", "VM", "AGC", "LOCK"}
        keys_ok = (not (no_keys & set(keys))) if x6200 else no_keys <= set(keys)
        seen.setdefault("version", {})[r.name] = hello.rsplit(" ", 1)[0][6:]
        ok = hello.startswith("hello GoldCorpHF") and boot == boot2 and boot.isdigit() and keys_ok and \
            role.startswith("role view 0") and any(x.startswith("err unknown session") for x in wrong) and \
            any(x.startswith("role view") for x in wrong) and size
        return ok, (f"{hello!r} (the same boot {boot} on a second connection: {boot == boot2}); {size!r}; "
                    f"{len(keys)} keys, {'no' if x6200 else 'with'} PRE/VM/AGC/LOCK: {keys_ok}; unpaired: {role!r}; "
                    f"an unknown token: {wrong}")
    per_radio(run, "control", "panel-greeting", greeting, ["panel-hello-boot", "panel-keys-list", "auth-token"])

    def plan(r):
        c, g = fresh(r.host)
        c.send("panel 1")
        g += texts(c, 3.0)
        c.close()
        bands = next((x for x in g if x.startswith("bands [")), None)
        pl = next((x for x in g if x.startswith("plan [")), None)
        try:
            nb, npl = len(json.loads(bands[6:])), len(json.loads(pl[5:]))
        except (TypeError, ValueError):
            nb = npl = 0
        return nb > 5 and npl > 5, f"a panel client gets the band list ({nb}) and the band plan ({npl} rows)"
    per_radio(run, "control", "panel-bands-plan", plan, ["panel-bands-plan"])

    def view_only(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        v0 = int(r.state.get("vol", -1))
        c, g = fresh(r.host)
        try:
            c.send("set freq 14100000")
            c.send("knob vol 2")
            c.send("control take")
            got = texts(c, 2.0)
        finally:
            c.close()
        errs = [x for x in got if x.startswith("err view only")]
        same = int(r.state.get("freq", 0)) == F_TEST and int(r.state.get("vol", -1)) == v0 and r.ctl_mine
        return len(errs) >= 3 and same, (f"an unpaired page's set, knob and control take: {len(errs)} refused "
                                         f"({errs[:1]}), the radio unchanged and the bench's control kept: {same}")
    per_radio(run, "control", "panel-view-only", view_only, ["panel-view-only-no-send"])

    def ping_clock(r):
        t0 = time.time()
        r.send("ping 4242")
        pong = line_after(r, t0, "pong ")
        ms = int(time.time() * 1000)
        r.send(f"clk? {ms}")
        clk = line_after(r, t0, f"clk {ms} ")
        drift = None
        if clk:
            back = time.time() * 1000
            ours = int(clk.split()[2])
            drift = ours - (ms + back) / 2
        r.send(f"clk {int(time.time() * 1000)} 1500")
        slow = line_after(r, t0, "clk slow ")
        r.send("clk 5 1")
        bad = line_after(r, t0, "err clk")
        ok = pong == "pong 4242" and clk and drift is not None and abs(drift) < 2000 and slow and bad
        return ok, (f"ping 4242 -> {pong!r}; clk? -> the radio's clock {drift and round(drift)} ms from the Mac's; "
                    f"a 1.5 s round trip refused {slow!r} (the clock left alone); a phone time before 2023 {bad!r}")
    per_radio(run, "control", "panel-ping-clock", ping_clock, ["link-stall-ping", "panel-clock-query",
                                                               "panel-clock-refusals"])

    def meter(r):
        time.sleep(1.0)
        t0 = time.time()
        time.sleep(3.0)
        with r.lock:
            m = [x for at, x in list(r.lines) if at >= t0 and x.startswith("m ")]
        bat = r.state.get("bat")
        ok = 20 <= len(m) <= 40 and isinstance(bat, int) and -1 <= bat <= 100
        return ok, f"{len(m)} S-meter readings in 3 s (ten a second), the last {m[-1] if m else None!r}; " \
            f"the battery in the state: {bat} %" + (" (-1: on external power)" if bat == -1 else "")
    per_radio(run, "control", "panel-meter", meter, ["panel-smeter", "panel-battery"])

    def tone(r):
        kt, rise = r.state.get("kt"), r.state.get("cwrise")
        want = 1 if r.name == "x6200" else 0
        return isinstance(kt, int) and 300 <= kt <= 1500 and rise == want, \
            f"key tone {kt} Hz; cwrise {rise} (the X6200's CW passband on the other side: 1 there, 0 on the X6100)"
    per_radio(run, "control", "panel-kt-cwrise", tone, ["panel-kt-cwrise"])

    def am_filter(r):
        bench.ensure(r, {"freq": 7200000, "mode": 6})
        r.send("set filter 300 4000")
        try:
            r.wait("AM's filter", lambda: r.state.get("fhi") == 4000, 5)
        except bench.Refused:
            pass
        return r.state.get("flo") == 0 and r.state.get("fhi") == 4000, \
            f"AM, 300-4000 asked: {r.state.get('flo')}-{r.state.get('fhi')} Hz (the low edge always 0)"
    per_radio(run, "control", "panel-filter-am", am_filter, ["panel-filter-am-fm"])

    def refused_set(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        t0 = time.time()
        r.send("set freq 1")
        err = line_after(r, t0, "err ")
        with r.lock:
            te = next((at for at, x in list(r.lines) if at >= t0 and x == err), None)
        st = line_after(r, te, "state ", 3) if te else None
        return err is not None and st is not None and int(r.state.get("freq", 0)) == F_TEST, \
            f"set freq 1: {err!r}, then the whole state again {st is not None}; the dial kept {r.state.get('freq')}"
    per_radio(run, "control", "panel-refused-set", refused_set, ["panel-optimistic-revert"])

    def play_guard(r):
        t0 = time.time()
        r.send("play /tmp/../etc/passwd")
        e1 = line_after(r, t0, "err play")
        t1 = time.time()
        r.send("play /tmp/bench_not_there.wav")
        e2 = line_after(r, t1, "err play")
        r.send("play stop")
        return bool(e1 and e2) and not r.state.get("plays"), \
            f"a path out of /tmp refused {bool(e1)}, a bench file not there refused {bool(e2)}; nothing playing"
    per_radio(run, "control", "panel-play-guard", play_guard, ["remote-play-file"])

    def wide(r):
        t0 = time.time()
        n0 = len(r.r.frames)
        r.send("wide 1")
        try:
            if r.name == "x6100":
                e = line_after(r, t0, "err wide")
                return e is not None, f"wide 1: {e!r} (its base has no spectrum of its own)"
            time.sleep(2.0)
            fr = [f for f in list(r.r.frames) if f[0] >= t0 and f[2][:2] == b"\xfe\xff"]
            if not fr:
                return False, f"wide 1: no 0xFFFE frames in 2 s ({len(r.r.frames) - n0} frames of any kind)"
            nb, cen, wid = struct.unpack_from("<HII", fr[-1][2], 2)
            return 300000 <= wid <= 400000, f"wide 1: {len(fr)} frames in 2 s, {nb} bins, {wid / 1000:.0f} kHz wide " \
                f"around {cen / 1e6:.3f} MHz"
        finally:
            r.send("wide 0")
    per_radio(run, "control", "panel-wide", wide, ["remote-wide-spectrum"])


def t_control_owner(run):
    def needed(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        v0 = int(r.state.get("vol", -1))
        c, g = fresh(r.host)
        try:
            c.send(f"auth {r._tok}")
            texts(c, 1.0)
            c.send(f"set vol {max(0, v0 - 1)}")
            got = texts(c, 1.5)
        finally:
            c.close()
        err = next((x for x in got if "take control first" in x), None)
        return err is not None and int(r.state.get("vol", -1)) == v0 and r.ctl_mine, \
            f"a paired page without control: set vol {err!r}; the volume kept {r.state.get('vol')}"
    per_radio(run, "control", "control-needed", needed, ["control-required"])

    def lost_and_free(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        said = []
        c, g = fresh(r.host)
        c.send(f"auth {r._tok}")
        texts(c, 1.0)
        c.send("control take")
        r.wait("control gone from the bench", lambda: not r.ctl_mine, 5)
        t0 = time.time()
        c.s.shutdown(socket.SHUT_RDWR)          # the link gone, no goodbye
        c.s.close()
        none = line_after(r, t0, "ctl none", 6)
        said.append(f"a page took control, its link cut: {none!r}")
        c2, g2 = fresh(r.host)
        try:
            c2.send(f"auth {r._tok}")
            texts(c2, 1.0)
            t1 = time.time()
            c2.send(f"set freq {F_TEST}")
            took = line_after(r, t1, "ctl browser", 4)
            said.append(f"control free, another page tuned: {took!r}")
        finally:
            c2.close()
        time.sleep(0.5)
        r.take_control()
        return none is not None and took is not None, "; ".join(said)
    per_radio(run, "control", "control-lost-free", lost_and_free, ["control-lost-on-disconnect",
                                                                   "control-free-first-takes"])

    def tci_takes(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        t, greet = tci_session(r.host)
        try:
            t.send("vfo:0,0;")
            q = expect(t, "vfo:")
            time.sleep(1.0)
            kept = r.ctl_mine
            t0 = time.time()
            t.send("vfo:0,0,14100000;")
            took = line_after(r, t0, "ctl tci", 5)
            r.wait("the dial from TCI", lambda: int(r.state.get("freq", 0)) == 14100000, 5)
        finally:
            t.close()
        time.sleep(0.5)
        r.take_control()
        return kept and took is not None, f"vfo:0,0; asked ({q!r}): control kept {kept}; vfo set: {took!r}"
    per_radio(run, "control", "control-tci", tci_takes, ["control-tci-takes"])


FT8_APPS = ("FT8", "FT4", "FT2")


def pair_ok(vals):
    """A pair's 1:1: True when both pass, None when one is to look at and none failed, else False"""
    vals = list(vals)
    return False if False in vals else None if None in vals else True


def t_panel_apps(run):
    def ft8(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        said, ok = [], True
        r.send("ft8 open")
        try:
            # the state names the app by its protocol: FT8, FT4 or FT2 (the X6100's was on FT4, 2026-11-22)
            r.wait("FT8 open", lambda: r.state.get("app") in FT8_APPS and r.ft8.get("open") == 1, 10)
            r.wait("the dial held", lambda: r.state.get("lock") == 1, 5)
            ulock = r.state.get("ulock")
            f0 = int(r.state.get("freq", 0))
            t0 = time.time()
            r.send("set freq 14100000")
            err = line_after(r, t0, "err ", 3)
            held = int(r.state.get("freq", 0)) == f0 and err is not None and ulock == 0
            ok &= held
            said.append(f"FT8 open on {f0}: lock 1, ulock {ulock}; a tune {err!r}")
            p0, tx0, h0 = r.ft8.get("proto"), r.ft8.get("txf"), r.ft8.get("hold")
            steps = []
            try:
                for _ in range(3):
                    p = r.ft8.get("proto")
                    r.send("ft8 mode")
                    r.wait("FT8's mode stepped", lambda: r.ft8.get("proto") != p, 5)
                    steps.append(r.ft8.get("proto"))
                stepped = steps[-1] == p0 and len(set(steps)) == 3
                ok &= stepped
                said.append(f"MODE x3: {p0} -> {steps} (back where it was {stepped})")
                new_tx = 1234 if tx0 != 1234 else 1300
                r.send(f"ft8 txfreq {new_tx}")
                r.wait("FT8's TX Hz", lambda: r.ft8.get("txf") == new_tx, 5)
                r.send("ft8 hold")
                r.wait("HOLD", lambda: r.ft8.get("hold") != h0, 5)
                said.append(f"TX Hz {tx0} -> {new_tx}, HOLD {h0} -> {r.ft8.get('hold')}")
            finally:
                while r.ft8.get("proto") != p0 and len(steps) < 6:
                    p = r.ft8.get("proto")
                    r.send("ft8 mode")
                    r.wait("FT8's mode back", lambda: r.ft8.get("proto") != p, 5)
                    steps.append(r.ft8.get("proto"))
                if tx0 is not None and r.ft8.get("txf") != tx0:
                    r.send(f"ft8 txfreq {tx0}")
                    r.wait("FT8's TX Hz back", lambda: r.ft8.get("txf") == tx0, 5)
                if r.ft8.get("hold") != h0:
                    r.send("ft8 hold")
                    r.wait("HOLD back", lambda: r.ft8.get("hold") == h0, 5)
                said.append(f"put back: mode {r.ft8.get('proto')}, TX Hz {r.ft8.get('txf')}, HOLD {r.ft8.get('hold')}")
        finally:
            r.send("ft8 close")
            r.wait("FT8 closed", lambda: r.state.get("app") not in FT8_APPS, 10)
        return ok, "; ".join(said)
    per_radio(run, "control", "panel-ft8", ft8, ["panel-app-holds-freq", "panel-ft8-mode-txfreq"])

    def wefax(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        r.send("wefax open")
        try:
            r.wait("WeFax open", lambda: r.state.get("app") == "WeFax", 10)
            opened = True
        except bench.Refused:
            opened = False
        r.send("wefax close")
        try:
            r.wait("WeFax closed", lambda: r.state.get("app") != "WeFax", 10)
            closed = True
        except bench.Refused:
            closed = False
        return opened and closed, f"wefax open: the state's app WeFax {opened}; wefax close: gone {closed}"
    per_radio(run, "control", "panel-wefax", wefax, ["panel-wefax-cmds"])


def t_screen(run):
    def jpeg(r):
        c, g = fresh(r.host, secs=0.5)
        try:
            c.send("screen 1")
            time.sleep(3.0)
            fr = [f for f in list(c.frames) if f[2][8:10] == b"\xff\xd8"]
            sharp = fr[-1] if fr else None
            c.send("picture fast")
            time.sleep(2.5)
            fast = [f for f in list(c.frames) if f[0] > time.time() - 2.0 and f[2][8:10] == b"\xff\xd8"]
            c.send("screen 0")
            time.sleep(1.0)
            n1 = c.binary
            time.sleep(2.0)
            after = c.binary - n1
        finally:
            c.close()
        if not sharp or not fast:
            return False, f"JPEG frames: {len(fr)} sharp, {len(fast)} fast"
        w, h, _ = struct.unpack_from("<HHI", sharp[2], 0)          # the header: the screen's size, always
        js, jf = jpeg_size(sharp[2][8:]), jpeg_size(fast[-1][2][8:])
        ok = len(fr) >= 3 and w in (800, 480) and js == (w, h) and jf == (w // 2, h // 2) and \
            fast[-1][1] < sharp[1] and after == 0
        return ok, (f"{len(fr)} JPEG frames in 3 s, the screen {w}x{h}, the picture {js} {sharp[1] // 1000} kB; "
                    f"picture fast: {jf} {fast[-1][1] // 1000} kB; screen 0: {after} frames after")
    per_radio(run, "control", "screen-stream", jpeg, ["screen-jpeg-stream", "screen-picture-quality"])

    def viewers(r):
        held, refused_at = [], None
        try:
            for k in range(4):
                try:
                    c = Remote(r.host, 50002, timeout=6)
                    c.send("screen 0")
                    if not texts(c, 1.0) and not c.alive:
                        raise ConnectionError("closed")
                    held.append(c)
                except (ConnectionError, OSError):
                    refused_at = k + 1
                    break
        finally:
            for c in held:
                c.close()
        mine = 1 + (r.name in run.radios)   # this test's session and the suite's own (its banner)
        return refused_at is not None and len(held) + mine <= 4, \
            f"{len(held)} more viewers taken beside the suite's {mine}, the next refused: four in all"
    per_radio(run, "control", "screen-viewers", viewers, ["remote-max-viewers"])

    def origin(r):
        h = r.host
        evil, _ = refused_ws(h, 50002, {"Origin": "http://evil.example"})
        rebind, _ = refused_ws(h, 50002, {"Host": "evil.example:50002", "Origin": "http://evil.example"})
        pat, _ = refused_ws(h, 50002, {"Origin": f"http://{h}:8080"})
        own, e = refused_ws(h, 50002, {"Origin": f"http://{h}"})
        ok = evil and rebind and pat and not own
        return ok, (f"the remote: another site refused {evil}, a rebound name refused {rebind}, Pat's mailbox refused "
                    f"{pat}, the radio's own page taken {not own} ({e})")
    per_radio(run, "control", "remote-origin", origin, ["ws-origin-check", "ws-own-names"])


# ---- control: the web configurator's guards (every change sent is one it must refuse: an invalid TCI port) ----

def http(ip, path, host=None, method="GET", body=None, headers=None):
    """(status, the body's first 300 characters) of a request to the radio's web server"""
    import urllib.error
    import urllib.request
    h = {"Host": host or ip}
    h.update(headers or {})
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    req = urllib.request.Request(f"http://{ip}{path}", data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read(300).decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read(300).decode(errors="replace")


def by_radio(run, batch, test, covers, fn):
    """fn(name, host, ip) on each radio without a remote session (the web, SSH): (ok, says) each, then 1:1"""
    res = {}
    for n in run.names:
        h = run.host(n)
        try:
            ok, says = fn(n, h, socket.gethostbyname(h))
        except (OSError, ValueError, KeyError) as e:
            ok, says = False, f"no answer: {type(e).__name__} {e}"
        ok = ok if ok is None else bool(ok)
        res[n] = ok
        run.result(batch, f"{test} {n}", ok, says, radio=n, covers=covers)
    if len(res) == 2:
        run.result(batch, f"{test} 1:1", pair_ok(res.values()),
                   ", ".join(f"{k} {'pass' if v else 'look' if v is None else 'FAIL'}" for k, v in res.items()), covers=covers)


def t_web_guards(run):
    def tci_page(n, h, ip):
        st, before = http(ip, "/api/tci")
        conf = json.loads(before)
        bad = [http(ip, "/api/tci", method="POST", body=b) for b in
               ({"enabled": conf.get("enabled"), "port": 1023}, {"enabled": conf.get("enabled"), "port": 99999},
                {"enabled": conf.get("enabled"), "port": 50001, "trust": "x"})]
        st2, after = http(ip, "/api/tci")
        t, greet = tci_session(h)
        t.close()
        ok = st == 200 and set(conf) >= {"enabled", "port", "trust", "tx_confirm"} and \
            all(c == 400 and "Invalid port" in b or c == 400 and "trust" in b for c, b in bad) and \
            json.loads(after) == conf and any(g.startswith("ready") for g in greet)
        return ok, (f"GET /api/tci {conf}; port 1023, port 99999, trust 'x': "
                    f"{[(c, json.loads(b).get('msg')) for c, b in bad]}; unchanged after {json.loads(after) == conf}; "
                    f"TCI still answers {any(g.startswith('ready') for g in greet)}")
    by_radio(run, "control", "web-tci", ["web-tci-page", "tci-enable-port", "web-save-failure-reasons"], tci_page)

    def guards(n, h, ip):
        bad = {"enabled": True, "port": 1023}          # refused by the handler too, should a guard let it through
        rebind = http(ip, "/api/tci", host="evil.example", method="POST", body=bad)
        cross = http(ip, "/api/tci", method="POST", body=bad, headers={"Origin": "http://evil.example"})
        fetch = http(ip, "/api/tci", method="POST", body=bad, headers={"Sec-Fetch-Site": "cross-site"})
        pat = http(ip, "/api/tci", method="POST", body=bad, headers={"Origin": f"http://{ip}:8080"})
        ssh_rd = http(ip, "/api/ssh", host="evil.example")
        funnel = http(ip, "/", headers={"Tailscale-Funnel-Request": "?1"})
        own = http(ip, "/api/tci", method="POST", body=bad, headers={"Origin": f"http://{ip}"})
        ok = rebind[0] == 403 and cross[0] == 403 and fetch[0] == 403 and pat[0] == 403 and ssh_rd[0] == 403 and \
            funnel[0] == 403 and own[0] == 400
        return ok, (f"a change by a rebound name {rebind[0]}, from another site {cross[0]}, marked cross-site "
                    f"{fetch[0]}, from Pat's mailbox {pat[0]}; the SSH page by a rebound name {ssh_rd[0]}; any page "
                    f"through Funnel {funnel[0]}; the radio's own page {own[0]} (the handler's 'Invalid port')")
    by_radio(run, "control", "web-guards", ["own-names-dns-rebinding", "web-own-names-guard", "ssh-page-own-name",
                                            "funnel-configurator-403"], guards)

    def funnel_ws(n, h, ip):
        """A WebSocket marked as coming through Funnel says nothing before a key and goes after 5 s; nothing is sent
        to it (a text would be taken as a wrong key, which counts towards Funnel's pause)"""
        out = {}
        for port in (50001, 50002):
            c = Remote(h, port, headers={"Tailscale-Funnel-Request": "?1"})
            got = texts(c, 2.0)
            t0 = time.time()
            while c.alive and time.time() - t0 < 6:
                time.sleep(0.2)
            out[port] = (got, not c.alive, round(time.time() - t0 + 2.0, 1))
            c.close()
        ok = all(not g and gone for g, gone, _ in out.values())
        return ok, "; ".join(f"{p}: {len(g)} texts before a key, closed {gone} after {s} s"
                             for p, (g, gone, s) in out.items())
    by_radio(run, "control", "funnel-sim", ["tci-funnel-refused", "funnel-ws-key-first"], funnel_ws)


# ---- control: the Panel's Log tab (the owner, 2026-11-22: "Yes, log Q9TST") ----
# The phone's log writes the day's log on DATA (/mnt/field/logs/YYYYMMDD.adi, the QSO's UTC date), kicks Wavelog
# (off without /mnt/field/wavelog.conf) and sends the QSO as WSJT-X UDP "QSO Logged" when ft8_udp.conf says
# enabled=1, read again for each QSO while the FT8 window is closed. So the test switches the UDP off for its QSOs
# (none of Q9TST reaches a PC logger, GridTracker or LoTW) and puts the file back byte for byte; it takes its QSOs
# out of the day's log after (the file and its folder gone when they were not there before).
LOG_CALL = "Q9TST"
LOG_PREP = r"""
d=/mnt/field/logs; f=$d/$(date -u +%Y%m%d).adi
echo "dir=$([ -d $d ] && echo yes || echo no) file=$([ -f $f ] && echo yes || echo no) path=$f"
echo "wavelog=$([ -f /mnt/field/wavelog.conf ] && grep -c '^enabled=1' /mnt/field/wavelog.conf || echo 0)"
if [ -f /mnt/ft8_udp.conf ]; then
    cp -p /mnt/ft8_udp.conf /tmp/bench_ft8_udp.conf && sed -i 's/^enabled=1/enabled=0/' /mnt/ft8_udp.conf && echo udp=off
else
    echo udp=none
fi
"""


def adif_records(text):
    """The records of an ADIF file after its header: [{FIELD: value}]"""
    import re
    body = re.split(r"<eoh>", text, flags=re.I)[-1]
    out = []
    for rec in re.split(r"<eor>", body, flags=re.I):
        fields = {}
        for m in re.finditer(r"<([A-Za-z_]+):(\d+)(?::[A-Za-z])?>", rec):
            fields[m.group(1).upper()] = rec[m.end():m.end() + int(m.group(2))]
        if fields:
            out.append(fields)
    return out


def t_log(run):
    def fn(r):
        import random
        prep = dict(x.split("=", 1) for x in bench.ssh(r.host, LOG_PREP, 30).split() if "=" in x)
        said, ok = [], True
        try:
            if prep.get("wavelog") not in ("0", ""):
                return False, "Wavelog is on: the test QSOs would go to the online log; not run"
            if prep.get("udp") not in ("off", "none"):
                return False, f"the WSJT-X UDP not switched off ({prep.get('udp')}); not run"
            bench.close_apps(r)                 # the FT8 window owns the UDP socket while open
            bench.ensure(r, {"freq": F_TEST, "mode": 2})
            t0 = time.time()
            r.send(f"log {LOG_CALL}|57|58|BENCH|TEST QTH|FN00|")
            a = line_after(r, t0, "logged ", 5)
            ok &= a == "logged ok"
            said.append(f"log in USB: {a!r}")
            bench.ensure(r, {"mode": 4})
            qid = random.randrange(10 ** 9, 10 ** 12)
            q = f"{LOG_CALL}|599|579"
            answers = []
            for cmd in (f"logq {qid} {q}", f"logq {qid} {q}", f"logq? {qid} {q}", f"logq {qid} {LOG_CALL}|599|599",
                        f"logq? {qid + 1} {q}"):
                t1 = time.time()
                r.send(cmd)
                answers.append(line_after(r, t1, "logged ", 5))
            want = [f"logged {qid} ok", f"logged {qid} ok", f"logged {qid} ok", f"logged {qid} conflict",
                    f"logged {qid + 1} unknown"]
            ok &= answers == want
            said.append("logq: " + ", ".join(repr(x) for x in answers) + " (sent, the same again, asked, another QSO "
                        "under its number, a number never sent)")
            time.sleep(1.0)
            text = bench.ssh(r.host, f"cat {prep['path']} 2>/dev/null", 20)
            mine = [x for x in adif_records(text) if x.get("CALL") == LOG_CALL]
            modes = [(x.get("MODE"), x.get("RST_SENT"), x.get("RST_RCVD")) for x in mine]
            first = mine[0] if mine else {}
            ok &= len(mine) == 2 and modes == [("SSB", "57", "58"), ("CW", "599", "579")] and \
                first.get("NAME") == "BENCH" and first.get("GRIDSQUARE") == "FN00" and \
                abs(float(first.get("FREQ", 0)) - F_TEST / 1e6) < 0.001
            head = text[:200]
            hdr = "<ADIF_VER:5>3.1.8" in head and "GoldCorpHF" in head
            if prep.get("file") == "no":
                ok &= hdr
            said.append(f"the day's log: {len(mine)} {LOG_CALL} records {modes} (written once each), name "
                        f"{first.get('NAME')!r}, grid {first.get('GRIDSQUARE')!r}, {first.get('FREQ')} MHz; its "
                        f"header ADIF 3.1.8 by GoldCorpHF {hdr}")
            return ok, "; ".join(said)
        finally:
            # the test's QSOs out of the day's log, the UDP setting back as it was
            if prep.get("file") == "no":
                clean = f"rm -f {prep['path']}" + ("; rmdir /mnt/field/logs 2>/dev/null" if prep.get("dir") == "no"
                                                   else "")
            else:
                clean = (f"python3 - {prep['path']} <<'PY'\nimport re, sys\np = sys.argv[1]\ns = open(p).read()\n"
                         f"parts = re.split(r'(<eor>)', s, flags=re.I)\nkeep = ''.join(a + b for a, b in "
                         f"zip(parts[0::2], parts[1::2] + ['']) if '<CALL:{len(LOG_CALL)}>{LOG_CALL}' not in a.upper())"
                         f"\nopen(p + '.tmp', 'w').write(keep)\nPY\nmv {prep['path']}.tmp {prep['path']}")
            if prep.get("udp") == "off":
                clean += ("; cp -p /tmp/bench_ft8_udp.conf /mnt/ft8_udp.conf && cmp /tmp/bench_ft8_udp.conf "
                          "/mnt/ft8_udp.conf && rm /tmp/bench_ft8_udp.conf && echo udp_back")
            out = bench.ssh(r.host, clean + f"; grep -c '{LOG_CALL}' {prep['path']} 2>/dev/null || true", 30)
            say(f"{r.name}: the test QSOs taken out, the UDP setting back: {out.strip()!r}")
            if prep.get("udp") == "off" and "udp_back" not in out:
                raise bench.Refused(f"{r.name}: /mnt/ft8_udp.conf NOT put back (/tmp/bench_ft8_udp.conf has it)")
    per_radio(run, "control", "panel-log", fn, ["panel-log-qso", "panel-logq-idempotent", "panel-logq-query",
                                                 "panel-log-mode", "log-quick-qso-fields"])


def control(run, only):
    for name, fn in (("tci-proto", t_tci_proto), ("tci-drive", t_tci_drive), ("tci-guards", t_tci_guards),
                     ("panel", t_panel), ("control-owner", t_control_owner), ("panel-apps", t_panel_apps),
                     ("screen", t_screen), ("web-guards", t_web_guards), ("log", t_log)):
        if not only or name in only:
            run.banner(f"SUITE control: {name} (nothing sent)")
            fn(run)


# ---- system: each radio read over SSH, nothing changed ----

SYS_READ = r"""
echo "now=$(date +%s)"
echo "bat=$(tail -1 /mnt/battery_log.csv)"
echo "fsck=$(tail -1 /mnt/data_fsck.log 2>/dev/null)"
echo "good=$(date -r /var/lib/x6100/params-good.db +%s 2>/dev/null) $(sqlite3 -readonly /var/lib/x6100/params-good.db 'pragma integrity_check' 2>&1 | head -1)"
p=$(pidof x6100_gui)
[ -n "$p" ] && echo "gui_start=$(( $(date +%s) - $(cut -d. -f1 /proc/uptime) + $(awk '{print $22}' /proc/$p/stat) / 100 ))"
echo "logs=$(ls /mnt/app_logs/x6100_log_*.txt 2>/dev/null | wc -l)"
echo "trace=$(grep -c . /mnt/app_logs/start_trace.txt 2>/dev/null)"
for w in 'GUI start, GoldCorpHF' 'settings read' 'base answered' 'screen built' 'services started' 'first frame'; do
    grep -q "$w" /mnt/app_logs/start_trace.txt && echo "trace_has=$w"
done
echo "ts=$(x6100-tailscale status 2>&1 | tr '\n' ' ' | cut -c1-200)"
for p in x6100_gui gpsd NetworkManager pulseaudio dropbear sshd; do
    pidof $p > /dev/null && echo "proc=$p"
done
for p in x6x00-gui-watch x6100_webserver x6100_daemon; do
    ps | grep -v grep | grep -q "$p" && echo "proc=$p"
done
echo "gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"
echo "cputemp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
for d in /sys/bus/iio/devices/iio:device*; do
    [ -f "$d/in_temp_raw" ] && echo "iio=$(cat $d/name) $(cat $d/in_temp_raw) $(cat $d/in_temp_offset 2>/dev/null) $(cat $d/in_temp_scale 2>/dev/null)"
done
grep -m1 'gpu: Mali400 draws' /tmp/x6100_log.txt > /dev/null && echo "gpu=draws"
grep -m1 'turned onto the panel by the GPU' /tmp/x6100_log.txt > /dev/null && echo "gpu=layer"
echo "sess=$(grep -c . /mnt/remote_sessions.conf) $(grep -cE '^[0-9a-f]{64}' /mnt/remote_sessions.conf) $([ -s /mnt/remote_sessions.conf.bak ] && echo bak || echo nobak) $(grep -cF -f /tmp/bench_tokens /mnt/remote_sessions.conf 2>/dev/null; rm -f /tmp/bench_tokens)"
echo "upd=$(x6100-update status 2>/dev/null | head -c 600)"
echo "adif=$(head -c 400 /mnt/ft_log.adi 2>/dev/null | tr '\n' ' ')"
echo "adif_day=$(ls -t /mnt/field/logs/*.adi 2>/dev/null | head -1 | xargs -r head -c 400 | tr '\n' ' ')"
echo "drift=$([ -s /mnt/rtc-drift.json ] && echo yes)"
"""


def sys_read(host, tokens):
    """SYS_READ's answers; the bench's own tokens go to the radio in a file (never on a command line) for the check
    that the sessions file keeps none of them in plain"""
    subprocess.run(["ssh", "-o", "BatchMode=yes", f"root@{host}", "cat > /tmp/bench_tokens"],
                   input="\n".join(t for t in tokens if t) + "\n", text=True, capture_output=True, timeout=30)
    return sys_parse(bench.ssh(host, SYS_READ, 60))


def sys_parse(out):
    """SYS_READ's lines as {key: value}; the keys that come more than once as lists"""
    kv = {}
    for ln in out.splitlines():
        k, _, v = ln.partition("=")
        if k in ("trace_has", "proc", "iio", "gpu"):
            kv.setdefault(k, []).append(v)
        else:
            kv[k] = v
    kv["mac_now"] = time.time()
    return kv


def clock_offset(host):
    """The radio's clock less the Mac's: its date read as the SSH command ends (the connection's set-up, most of the
    time an SSH call takes, comes before it), the best of three"""
    best = None
    for _ in range(3):
        t0 = time.time()
        out = bench.ssh(host, "python3 -c 'import time; print(time.time())'", 20)     # busybox's date has no %N
        t1 = time.time()
        off = float(out.strip()) - (t1 - 0.05)
        if best is None or t1 - t0 < best[0]:
            best = (t1 - t0, off)
    return best[1]


def ports_open(host, ports):
    ip = socket.gethostbyname(host)
    got = {}
    for p in ports:
        try:
            socket.create_connection((ip, p), timeout=4).close()
            got[p] = True
        except OSError:
            got[p] = False
    return got


def password_refused(host):
    """SSH asked for a password login: (refused, the methods it offers)"""
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "PubkeyAuthentication=no", "-o",
                        "PreferredAuthentications=password,keyboard-interactive", "-o", "ConnectTimeout=8",
                        "-o", "NumberOfPasswordPrompts=0", f"root@{host}", "true"], capture_output=True, text=True,
                       timeout=30)
    err = (r.stderr or "").strip().splitlines()
    last = err[-1] if err else ""
    methods = last[last.find("(") + 1:last.rfind(")")] if "(" in last else ""
    return r.returncode != 0 and "password" not in methods and "keyboard" not in methods, last[-100:]


def system(run, only):
    checks = {}

    def check(name, covers, fn):
        if only and name not in only:
            return
        res = {}
        for n in run.names:
            try:
                ok, says = fn(n, data[n])
            except (KeyError, ValueError, IndexError, TypeError) as e:
                ok, says = False, f"could not read it: {type(e).__name__} {e}"
            ok = ok if ok is None else bool(ok)
            res[n] = ok
            run.result("system", f"{name} {n}", ok, says, radio=n, covers=covers)
        if len(res) == 2:
            run.result("system", f"{name} 1:1", pair_ok(res.values()),
                       ", ".join(f"{k} {'pass' if v else 'look' if v is None else 'FAIL'}" for k, v in res.items()), covers=covers)
        checks[name] = res

    run.banner("SUITE system: each radio read over SSH (nothing changed)")
    data = {}
    for n in run.names:
        h = run.host(n)
        try:
            toks = bench.load_json(os.path.join(bench.STATE, "tokens.json"), {}).get(n, {}).values()
            data[n] = sys_read(h, list(toks))
            data[n]["ports"] = ports_open(h, [22, 80, 443, 4532, 50001, 50002])
            data[n]["pw"] = password_refused(h)
            data[n]["clock_off"] = clock_offset(h)
        except (bench.Refused, OSError, ValueError, subprocess.TimeoutExpired) as e:
            data[n] = {"error": str(e)}

    def battery_log(n, d):
        t = int(d["bat"].split(",")[0])
        age = int(d["now"]) - t
        return 0 <= age <= 150, f"the last line {age} s old (a line a minute): {d['bat']}"
    check("battery-log", ["system-battery-log"], battery_log)

    def fsck(n, d):
        return "exit 0" in d["fsck"] or "left" in d["fsck"], f"data_fsck.log ends {d['fsck']!r}"
    check("data-fsck", ["data-fsck-log"], fsck)

    def good_copy(n, d):
        mt, integ = d["good"].split(" ", 1)
        newer = int(mt) >= int(d["gui_start"]) - 5 or int(d["now"]) - int(d["gui_start"]) < 90
        return integ.strip() == "ok" and newer, (f"params-good.db integrity {integ.strip()!r}, written "
                                                 f"{int(d['now']) - int(mt)} s ago, the GUI started "
                                                 f"{int(d['now']) - int(d['gui_start'])} s ago (a copy a minute after "
                                                 f"each start): {newer}")
    check("settings-good-copy", ["settings-good-copy"], good_copy)

    def ssh_keys(n, d):
        ok, last = d["pw"]
        return ok, f"a password login asked: refused, offering {last!r}"
    check("ssh-keys-only", ["ssh-keys-only"], ssh_keys)

    def tailscale(n, d):
        return "state=" in d["ts"] and "version=" in d["ts"], f"x6100-tailscale status: {d['ts'][:120]!r}"
    check("tailscale-status", ["tailscale-cli-status"], tailscale)

    def logs(n, d):
        k = int(d["logs"])
        return k <= 50, f"{k} run logs in /mnt/app_logs (the newest 50 kept)"
    check("app-logs", ["app-logs-retention"], logs)

    def trace(n, d):
        have = d.get("trace_has", [])
        return len(have) == 6, f"start_trace.txt ({d['trace']} lines) has {len(have)} of the 6 steps: {have}"
    check("start-trace", ["start-trace-gui"], trace)

    def services(n, d):
        procs = set(d.get("proc", []))
        need = {"x6100_gui", "x6x00-gui-watch", "gpsd", "NetworkManager", "pulseaudio", "x6100_webserver"}
        ports = d["ports"]
        ok = need <= procs and all(ports.values())
        return ok, f"ports {ports}; running {sorted(procs)}" + (f"; MISSING {sorted(need - procs)}" if need - procs
                                                                  else "")
    check("services", ["system-services-up"], services)

    def cpu(n, d):
        gov, mhz = d["gov"].split()
        return gov == "performance" and (int(mhz) == 1008000 or n == "x6200"), f"governor {gov}, {int(mhz) // 1000} MHz"
    check("cpu-speed", ["cpu-speed"], cpu)

    def temps(n, d):
        if n == "x6200":
            c = int(d["cputemp"]) / 1000
            return 15 < c < 90, f"the processor {c:.1f} C (thermal zone 0)"
        temps_c = []
        for x in d.get("iio", []):
            raw, off, scale = x.split()[-3:]       # "<name> raw offset scale": (raw + offset) x scale m°C
            temps_c.append(round((int(raw) + int(off)) * float(scale) / 1000, 1))
        return any(5 < c < 90 for c in temps_c), f"the power chip's temperature: {temps_c} C"
    check("temperature", ["cpu-temp-x6200", "pmu-temp-x6100"], temps)

    def gpu(n, d):
        g = d.get("gpu", [])
        return "draws" in g, f"the GUI's log: Mali400 draws {'draws' in g}, the GUI's layer by the GPU {'layer' in g}"
    check("gpu", ["gpu-mali-draws", "gpu-gui-layer"], gpu)

    def sessions(n, d):
        lines, hashed, bak, plain = (d["sess"].split() + ["", "", "", ""])[:4]
        return int(lines) <= 16 and int(hashed) >= 1 and bak == "bak" and plain == "0", \
            f"remote_sessions.conf: {lines} lines, {hashed} SHA-256 hashes, a .bak {bak == 'bak'}; the bench's own " \
            f"tokens there in plain: {plain}"
    check("sessions-file", ["sessions-file-hashed", "sessions-file-bak"], sessions)

    def update(n, d):
        j = json.loads(d["upd"])
        last = j.get("last") or {}
        return j.get("pending") is None and last.get("result") == "confirmed", \
            f"running {j.get('running')}, pending {j.get('pending')}, the last {last.get('result')} " \
            f"{last.get('version')}"
    check("update-state", ["system-update-pending-confirm"], update)

    def adif(n, d):
        a = d.get("adif_day") or d["adif"]
        which = "the newest day's log" if d.get("adif_day") else "ft_log.adi"
        if "<ADIF_VER:5>3.1.8" not in a or "<PROGRAMID:" not in a:
            return False, f"{which} starts {a[:100]!r}"
        # a header is written once, with the file: one from before the name GoldCorpHF keeps the old name
        return (True if "GoldCorpHF" in a else None), f"{which} starts {a[:110]!r}"
    check("adif-header", ["adif-logs-header"], adif)

    def clock(n, d):
        off = d["clock_off"]
        return abs(off) < 1.5 and d.get("drift") == "yes", f"the radio's clock {off:+.2f} s from the Mac's; the " \
            f"chip's drift learnt (/mnt/rtc-drift.json) {d.get('drift') == 'yes'}"
    check("clock", ["rtc-drift-correction"], clock)
