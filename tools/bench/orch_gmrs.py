#!/usr/bin/env python3
"""GMRS on the Sentry (the owner's GMRS licence, 2026-11-25): orch.py's tests of B's GMRS preset and channels, B's scan
of the 22 channels and G's GMRS groups, receive only.

GMRS (47 CFR Part 95 Subpart E) may be sent on only by radios certified for it: neither radio (through a transverter)
nor the HackRF or bladeRF is. These tests never transmit and never arm anything but G's SDR on a GMRS channel, which
must be refused (its transmitter stays locked, the radios' too).

orch.py imports this module and sets H to itself (A, B, in_parallel, alike); tests() gives the Test rows and COVERS
the features each covers. B's channel and G's SDR channel come from the remote's "set bch HZ" / "set gsdr HZ" (only
while B or G shows) and are read back from its state: bhz, bpre, bmode, bname, bscan (0 off, 1 the band, 2 the channel
list, 3 EMERGENCY's, 4 GMRS's 22), ghz, garm, gno (why G's SDR may not send there), sdr (an SDR on HOST); F SYSTEM's
GMRS panel by its keys, read back from fsel, gmp, gmhz and gmrx.

Neither radio may have an SDR on its HOST port (2026-11-25: the X6200's holds the AMBE stick). B's channel and preset
are the GUI's own either way; B's scan then runs on a replay of noise (x6x00-rtlspec -i) put in place of the X6200's
own daemon for the test and taken out after (the GUI starts its own again within seconds); G's SDR groups need a real
SDR (an RTL-SDR, HackRF or bladeRF) and the test says so.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import time

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_gmrs.H = sys.modules[__name__])


def _hz(ch):
    """GMRS channel ch's frequency as 47 CFR 95.1763 gives it, written out (not read from the radio's table)"""
    if ch <= 7:
        return 462562500 + (ch - 1) * 25000
    if ch <= 14:
        return 467562500 + (ch - 8) * 25000
    return 462550000 + (ch - 15) * 25000


CH = {ch: _hz(ch) for ch in range(1, 23)}
INPUT = {ch: CH[ch] + 5000000 for ch in range(15, 23)}
NAMES = {**{hz: f"GMRS {ch}" for ch, hz in CH.items()}, **{hz: f"GMRS {ch} IN" for ch, hz in INPUT.items()}}
SCAN_ORDER = sorted(CH.values())        # B's scan: by frequency, the stick moving least
NO_TX = "GMRS: ONLY RADIOS CERTIFIED FOR IT (47 CFR 95E)"
REPLAY = "/tmp/gmrs_noise.iq"


def _page(o, letter):
    """Both radios on the Sentry's page letter as one change, set alike again on the dial and mode they shared
    (bench.open_page_both): leaving G gives each radio its own kept A dial back, and the two kept ones differ"""
    o.changing += 1
    try:
        bench.open_page_both([o.r[H.A], o.r[H.B]], letter)
    finally:
        o.changing -= 1
    bad = [n for n, r in o.r.items() if r.state.get("pg") != letter]
    if bad:
        raise Refused(f"{', '.join(bad)} not on the Sentry's page {letter}")


def _need_state(o):
    for n, r in o.r.items():
        if "bhz" not in r.state:
            raise Refused(f"{n}: its GUI's state has no bhz (B's channel): a GUI from before the GMRS work")


def _b_state(r):
    return {k: r.state.get(k) for k in ("bhz", "bpre", "bmode", "bname", "bscan")}


def t_gmrs_b(o):
    """B on GMRS on both radios: GMRS 20 (462.675, a repeater's output), GMRS 8 (467.5625, a narrow one) and GMRS 20's
    repeater input (467.675) as B's channel, each read back from both: B's preset GMRS (not UHF LMR's, which holds it
    too), narrow FM, the channel named; A's dial and mode untouched and alike all the while (receive only: B's SDR
    never sends)"""
    _need_state(o)
    with o.step("'set bch' refused off page B") as s:
        r = o.r[H.B]
        if r.state.get("pg") == "B":
            _page(o, "A")
        n0 = len(r.errs)
        r.send(f"set bch {CH[20]}")
        r.wait("the refusal", lambda: any("bch: only while the Sentry shows B" in e for e in r.errs[n0:]), 5)
        s.confirm(r.errs[-1])
    with o.step("both on the Sentry's page B") as s:
        _page(o, "B")
        a0 = {k: bench._value(o.r[H.A], k) for k in ("freq", "mode")}
        s.confirm(f"A's dial {a0}")
    seen = {}
    for hz in (CH[20], CH[8], INPUT[20]):
        name = NAMES[hz]
        with o.step(f"B's channel {name} ({hz / 1e6:.4f} MHz) on both: preset GMRS, NFM, named {name}") as s:
            def one(n, r, hz=hz, name=name):
                r.send(f"set bch {hz}")
                r.wait(f"B on {name}", lambda: r.state.get("bhz") == hz and r.state.get("bpre") == "GMRS" and
                       r.state.get("bmode") == "NFM" and r.state.get("bname") == name, 8)
                return _b_state(r)
            got = H.in_parallel(o, one)
            H.alike(o, ("freq", "mode"))
            a = {k: bench._value(o.r[H.A], k) for k in ("freq", "mode")}
            if a != a0:
                raise Refused(f"A's dial moved with B's channel: {a0} -> {a}")
            seen[name] = got
            s.confirm("; ".join(f"{n} {g['bpre']} {g['bname']} {g['bhz']} {g['bmode']}" for n, g in got.items()))
    return True, (f"B on GMRS 20, GMRS 8 and GMRS 20 IN on both: preset GMRS, NFM, each named; A's dial "
                  f"{a0['freq'] / 1e6:.3f} MHz untouched"), seen


# ---- B's scan: a real SDR, or the X6200's daemon replaying noise ----

def _sdr_radio(o):
    """The radio with an SDR on its HOST port (the state's sdr), the X6200 first; None for neither"""
    for n in (H.B, H.A):
        if o.r[n].state.get("sdr") == 1:
            return n
    return None


def _daemon_args(host, wait=15.0):
    """The SDR daemon B's GUI runs (not a watch channel's): its pid and arguments. It may be between two (the GUI
    restarts it as B's device or rate changes: "daemon ends", a new pid a moment later), so looked for up to wait s"""
    end = time.time() + wait
    while True:
        for line in bench.ssh(host, "ps w | grep '[x]6x00-rtlspec' || true").splitlines():
            if " -W" in line or " -S " in line:
                continue
            parts = line.split()
            i = next((k for k, p in enumerate(parts) if p.endswith("x6x00-rtlspec")), None)
            if i is not None:
                return parts[0], parts[i + 1:]
        if time.time() > end:
            raise Refused(f"{host}: no SDR daemon running for B")
        time.sleep(1.0)


def _replay_on(o, n):
    """n's SDR daemon replaced by one replaying noise (unsigned 8-bit I/Q, 3 s, over and over) at the same settings:
    B receives (its state ON) with nothing in it; the AMBE port is the replay daemon's meanwhile. Returns its pid"""
    r = o.r[n]
    pid, args = _daemon_args(r.host)
    keep = []
    skip = False
    for a in args:                      # the GUI's settings; -d/-K (a stick by its number or serial) mean nothing here
        if skip:
            skip = False
            continue
        if a in ("-d", "-K"):
            skip = True
            continue
        keep.append(a)
    rate = int(keep[keep.index("-s") + 1]) if "-s" in keep else 2116800
    o.st.ev(f"{n}: its SDR daemon (pid {pid}) replaced by a replay of noise for B's scan (planned)", "radio")
    # the old one gone first (closing its device takes a moment; a new one meanwhile says RTLSPEC RUNNING and ends),
    # then this one at once: the GUI waits 3 s after its daemon left before it starts one of its own
    bench.ssh(r.host, f"head -c {rate * 2 * 3} /dev/urandom > {REPLAY} && kill {pid} && i=0; "
                      f"while kill -0 {pid} 2> /dev/null && [ $i -lt 200 ]; do usleep 20000; i=$((i + 1)); done; "
                      f"/usr/bin/x6x00-rtlspec -i {REPLAY} {' '.join(keep)} < /dev/null > /dev/null 2>> /tmp/rtlspec.log",
              timeout=40)
    time.sleep(1.0)
    new, nargs = _daemon_args(r.host)
    if new == pid or "-i" not in nargs:
        raise Refused(f"{n}: the replay daemon did not take the old one's place (running: {new} {' '.join(nargs)})")
    return new


def _replay_off(o, n, pid):
    """The replay daemon ended and its file taken out: the GUI starts its own again (its device and AMBE as before)"""
    r = o.r[n]
    bench.ssh(r.host, f"kill {pid} 2> /dev/null; rm -f {REPLAY}", timeout=20)
    t0 = time.time()
    while time.time() - t0 < 20:
        try:
            p, _ = _daemon_args(r.host, wait=0)
            if p != pid:
                o.st.ev(f"{n}: its own SDR daemon back (pid {p})", "radio")
                return p
        except Refused:
            pass
        time.sleep(1.0)
    raise Refused(f"{n}: its GUI did not start its own SDR daemon again within 20 s")


def t_gmrs_scan(o):
    """B's F1 on the GMRS preset goes down GMRS's 22 channels: B's scan kind 4 (GMRS's, not the band sweep), every
    channel it stops at a GMRS channel by its name, at least six of them, each the next of the 22 in frequency order
    (462.550, 462.5625 ... 462.725, 467.5625 ... 467.7125, round), then F1 again ends it. On the radio with an SDR, else
    on the X6200 with its daemon replaying noise; the other radio's B set alike and A's dial alike on both"""
    _need_state(o)
    n = _sdr_radio(o)
    replay = None
    with o.step("both on the Sentry's page B, B's channel GMRS 15 (462.550): its preset GMRS") as s:
        _page(o, "B")

        def one(_, r):
            r.send(f"set bch {CH[15]}")
            r.wait("B on GMRS 15", lambda: r.state.get("bhz") == CH[15] and r.state.get("bpre") == "GMRS", 8)
            return _b_state(r)
        got = H.in_parallel(o, one)
        s.confirm(str(got))
    if n is None:
        n = H.B
        with o.step(f"no SDR on either HOST port: {n}'s daemon replaying noise in its place") as s:
            replay = _replay_on(o, n)
            s.confirm(f"replay daemon pid {replay}")
    r = o.r[n]
    hops = []
    try:
        with o.step(f"F1 on {n}'s B: its scan goes down GMRS's channels (bscan 4)") as s:
            if r.state.get("bscan"):
                raise Refused(f"{n}: B's scan already on ({r.state.get('bscan')})")
            bench.tap(r, "F1")
            r.wait("B's scan on", lambda: r.state.get("bscan") != 0, 5)
            k = r.state.get("bscan")
            if k != 4:
                bench.tap(r, "F1")
                raise Refused(f"{n}: B's F1 scans kind {k}, not GMRS's channels (4): SCAN CFG's F1 SCANS must be THE "
                              "BAND, no EMERGENCY scan")
            s.confirm("bscan 4")
        with o.step(f"{n}'s B steps through GMRS's channels in frequency order, each named") as s:
            t0, last = time.time(), None
            while time.time() - t0 < 45 and len(hops) < 8:
                hz = r.state.get("bhz")
                if hz != last:
                    last = hz
                    hops.append((hz, r.state.get("bname")))
                time.sleep(0.1)
            bad = [(hz, nm) for hz, nm in hops if hz not in SCAN_ORDER or nm != NAMES.get(hz)]
            if bad:
                raise Refused(f"{n}: B's scan on channels that are not GMRS's or misnamed: {bad[:4]}")
            order = [SCAN_ORDER.index(hz) for hz, _ in hops]
            skips = [(a, b) for a, b in zip(order, order[1:]) if b != (a + 1) % len(SCAN_ORDER)]
            if len(hops) < 6:
                raise Refused(f"{n}: B's scan stepped {len(hops)} channels in 45 s (a signal held it?): {hops}")
            if skips:
                raise Refused(f"{n}: B's scan out of order: {[nm for _, nm in hops]}")
            s.confirm(", ".join(nm for _, nm in hops))
        with o.step(f"F1 again on {n}'s B: its scan ends") as s:
            bench.tap(r, "F1")
            r.wait("B's scan off", lambda: r.state.get("bscan") == 0, 5)
            H.alike(o, ("freq", "mode"))
            s.confirm(f"stopped on {r.state.get('bname')}")
    finally:
        if r.state.get("bscan"):
            bench.tap(r, "F1")
        if replay:
            with o.step(f"{n}'s own SDR daemon back") as s:
                s.confirm(f"pid {_replay_off(o, n, replay)}")
    via = "its SDR" if replay is None else "a replay of noise"
    return True, f"{n}'s B scanned GMRS through {via}: {', '.join(nm for _, nm in hops)}", hops


# ---- G: GMRS's groups through the SDR, never sent on ----

def t_gmrs_g(o):
    """G on GMRS through the SDR of the radio that has one: GMRS 20 and GMRS 20's repeater input as G's SDR channel,
    each read back with the reason it is not sent on (GMRS's own, whatever the SDR); A/B tapped there must not arm the
    SDR (garm 0) nor the radio (rxo 1); a ham channel (446.000) gets no GMRS reason; G back on the radio's own
    channel, A's dial alike on both all the while"""
    _need_state(o)
    n = _sdr_radio(o)
    if n is None:
        raise Refused("needs an SDR (RTL-SDR, HackRF or bladeRF) on a radio's HOST port for G's SDR groups: neither "
                      "radio has one (the X6200's holds the AMBE stick)")
    r = o.r[n]
    with o.step("both on page G, G's channel CB 19 (27.185 AM) on both") as s:
        _page(o, "G")
        o.both({"wch": (27185000, 6)})
        o.both({"freq": 27185000, "mode": 6})
        s.confirm(f"{bench._value(r, 'freq')} {bench._value(r, 'mode')}")
    said = {}
    try:
        for hz in (CH[20], INPUT[20]):
            name = NAMES[hz]
            with o.step(f"{n}'s G on {name} through its SDR: refused there for GMRS's own reason") as s:
                r.send(f"set gsdr {hz}")
                r.wait(f"G on {name}", lambda: r.state.get("ghz") == hz, 8)
                if r.state.get("gno") != NO_TX:
                    raise Refused(f"{n}: G on {name} says {r.state.get('gno')!r}, not {NO_TX!r}")
                H.alike(o, ("freq", "mode"))
                said[name] = r.state.get("gno")
                s.confirm(said[name])
            with o.step(f"A/B on {n}'s G at {name}: nothing armed") as s:
                if r.state.get("ghz") != hz or r.state.get("rxo") != 1:
                    raise Refused(f"{n}: not on {name} with its transmitter locked: no A/B tapped")
                bench.tap(r, "AB")
                time.sleep(1.5)
                if r.state.get("garm") or r.state.get("rxo") != 1 or r.state.get("tx"):
                    raise Refused(f"{n}: A/B on {name} armed something: garm {r.state.get('garm')}, rxo "
                                  f"{r.state.get('rxo')}, tx {r.state.get('tx')}")
                s.confirm("garm 0, rxo 1, tx 0")
        with o.step(f"{n}'s G on 70 CM's calling channel (446.000): no GMRS reason there") as s:
            r.send("set gsdr 446000000")
            r.wait("G on 446.000", lambda: r.state.get("ghz") == 446000000, 8)
            if "GMRS" in (r.state.get("gno") or ""):
                raise Refused(f"{n}: 446.000 says {r.state.get('gno')!r}")
            s.confirm(repr(r.state.get("gno")))
    finally:
        if r.state.get("garm"):
            bench.tap(r, "AB")
        with o.step(f"{n}'s G back on its own channel (CB 19), both alike") as s:
            r.send("set gsdr 0")
            r.wait("G on the radio", lambda: r.state.get("ghz") == 0, 8)
            H.alike(o, ("freq", "mode"))
            s.confirm(f"{bench._value(r, 'freq')}")
    return True, f"{n}'s G on GMRS 20 and its input: {NO_TX}; A/B armed nothing", said


# ---- F SYSTEM's GMRS panel ----

F_ITEM = "GMRS (LISTEN)"                # sys_items[]'s name, the state's fsel
ROWS = [CH[c] for c in range(1, 23)] + [INPUT[c] for c in range(15, 23)]    # the panel's 30 rows


def t_gmrs_f(o):
    """F SYSTEM's GMRS on both radios: 'set gmp 1' chooses it on F's list (fsel read back: the item there by its name)
    and opens its panel (gmp 1) on a GMRS channel, the receiver its F1 would tune said (gmrx: 0 none, 1 B's SDR, 2 the
    radio's own through the WR12); the VFO moves the choice a row (its 30: channels 1-22, the inputs of 15-22: the
    remote's MFK does not reach F on the X6100, its knob shared with VOL); F4 the tones (no channel chosen) and back;
    F1: on B's SDR the channel on B (page B, bhz), with no receiver nothing tuned; F5 closes it; A's dial alike and
    untouched all the while (receive only: nothing armed or keyed)"""
    _need_state(o)
    with o.step("'set gmp' refused off page F") as s:
        r = o.r[H.B]
        if r.state.get("pg") == "F":
            _page(o, "A")
        n0 = len(r.errs)
        r.send("set gmp 1")
        r.wait("the refusal", lambda: any("gmp: only while the Sentry shows F" in e for e in r.errs[n0:]), 5)
        s.confirm(r.errs[-1])
    with o.step("both on the Sentry's page F") as s:
        _page(o, "F")
        a0 = {k: bench._value(o.r[H.A], k) for k in ("freq", "mode")}
        s.confirm(f"A's dial {a0}")
    out = {}
    try:
        with o.step(f"'set gmp 1' on both: F's choice {F_ITEM}, the GMRS panel open on a GMRS channel") as s:
            def one(n, r):
                r.send("set gmp 1")
                r.wait("the GMRS panel", lambda: r.state.get("gmp") == 1, 5)
                if r.state.get("fsel") != F_ITEM:
                    raise Refused(f"{n}: F's choice reads {r.state.get('fsel')!r}, not {F_ITEM!r}")
                if r.state.get("gmhz") not in ROWS:
                    raise Refused(f"{n}: the panel's choice {r.state.get('gmhz')} is no GMRS channel")
                return {k: r.state.get(k) for k in ("gmhz", "gmrx")}
            got = H.in_parallel(o, one, wait=40)
            out["open"] = got
            s.confirm(str(got))
        with o.step("the VFO a click on both: the panel's next row (or the one before), named as the table has it") as s:
            def move(n, r):
                was = ROWS.index(r.state.get("gmhz"))
                if r.state.get("gmp") != 1:     # the VFO tunes the dial without the panel: never sent then
                    raise Refused(f"{n}: the panel closed before the VFO")
                r.send("knob main 1")
                r.wait("the choice moved", lambda: r.state.get("gmhz") != ROWS[was], 4)
                now = ROWS.index(r.state.get("gmhz"))
                if now not in ((was + 1) % len(ROWS), (was - 1) % len(ROWS)):
                    raise Refused(f"{n}: the choice went from row {was} to {now}")
                return NAMES[ROWS[now]]
            got = H.in_parallel(o, move)
            s.confirm(str(got))
        with o.step("F4 on both: the tones (no channel chosen), F4 again: the channels") as s:
            def tones(n, r):
                bench.tap(r, "F4")
                r.wait("the tones", lambda: r.state.get("gmhz") == 0 and r.state.get("gmp") == 1, 4)
                bench.tap(r, "F4")
                r.wait("the channels", lambda: r.state.get("gmhz") in ROWS, 4)
                return r.state.get("gmhz")
            s.confirm(str(H.in_parallel(o, tones)))
        with o.step("F1 (TUNE) on both: on B's SDR the channel on B; with no receiver nothing tuned") as s:
            def tune(n, r):
                hz, rx = r.state.get("gmhz"), r.state.get("gmrx")
                if rx == 2:             # the radio's own (the WR12): its dial would part the radios
                    return f"{NAMES[hz]}: the WR12 tunes it on A (not done here: the two radios would part)"
                bench.tap(r, "F1")
                if rx == 1:
                    r.wait("B on it", lambda: r.state.get("pg") == "B" and r.state.get("bhz") == hz and
                           r.state.get("bpre") == "GMRS", 8)
                    return f"{NAMES[hz]} on B"
                time.sleep(1.5)
                if r.state.get("pg") != "F" or r.state.get("gmp") != 1:
                    raise Refused(f"{n}: no receiver, yet F1 left the panel (page {r.state.get('pg')})")
                return f"{NAMES[hz]}: no receiver here, nothing tuned"
            got = H.in_parallel(o, tune)
            H.alike(o, ("freq", "mode"))
            a = {k: bench._value(o.r[H.A], k) for k in ("freq", "mode")}
            if a != a0:
                raise Refused(f"A's dial moved: {a0} -> {a}")
            if any(r.state.get("tx") or r.state.get("rxo") != 1 for r in o.r.values()):
                raise Refused("a radio armed or keyed")
            out["tune"] = got
            s.confirm(str(got))
        with o.step("F5 on the radios still in the panel: it closes") as s:
            def close(n, r):
                if r.state.get("gmp") == 1:
                    bench.tap(r, "F5")
                    r.wait("the panel closed", lambda: r.state.get("gmp") == 0, 4)
                return r.state.get("pg")
            s.confirm(str(H.in_parallel(o, close)))
    finally:
        for r in o.r.values():
            if r.state.get("gmp") == 1:
                r.send("set gmp 0")
    return True, f"F's GMRS panel on both: {out.get('tune')}", out


COVERS = {
    "gmrs-b": (["sentry-b-gmrs-channels", "remote-sdr-state"], ["sentry-b-f2-preset"]),
    "gmrs-scan": (["sentry-b-gmrs-scan"], []),
    "gmrs-g": (["g-gmrs-groups", "g-gmrs-no-tx"], ["g-sdr-tx-refusals"]),
    "gmrs-f": (["f-system-gmrs"], []),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("gmrs-b", "both RX", "B's channel GMRS 20, GMRS 8 and GMRS 20 IN on both: preset GMRS, NFM, named; A's "
             "dial untouched and alike", t_gmrs_b, 40),
        Test("gmrs-scan", "both RX", "B's F1 on GMRS: GMRS's 22 channels in frequency order, each named (an SDR, "
             "else the X6200's daemon replaying noise)", t_gmrs_scan, 90),
        Test("gmrs-g", "both RX", "G on GMRS 20 and its input through an SDR: refused for GMRS's reason, A/B arms "
             "nothing; G back on the radio", t_gmrs_g, 40),
        Test("gmrs-f", "both RX", "F SYSTEM's GMRS panel on both: chosen, opened, the MFK, the tones, F1 tunes on the "
             "receiver there (none: nothing), F5 closes; A's dial untouched", t_gmrs_f, 60),
    ]
