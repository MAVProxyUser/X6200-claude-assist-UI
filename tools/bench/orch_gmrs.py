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


# ---- the radio's own receiver: a transverter taking GMRS in, the WR12's stand-in on both radios ----

XV = {"transverter_1_from": 462500000, "transverter_1_to": 467800000, "transverter_1_shift": 434000000,
      "transverter_1_pwr_max": 0.1}         # 462.5-467.8 MHz on a 28.5-33.8 MHz IF, 0.1 W at most there


def _apart_ok(o, keys=("freq", "mode")):
    H.alike(o, keys)


def t_gmrs_xvtr(o):
    """GMRS on the radio's own receiver, through a transverter set on both radios for the test (462.5-467.8 MHz on a
    28.5-33.8 MHz IF: what the X6200's WR12 does, which neither radio has today), put back after: F's GMRS panel says
    A hears it (gmrx 2) and its F1 tunes A to the chosen channel in NFM on both; A/B there arms nothing; the radio's
    own PTT there is refused with GMRS's words, nothing keyed; A's F1 on the GMRS band scans the channels in frequency
    order (each radio's own dial, declared apart while it steps); G's GMRS group is the radio's own: CH + steps it,
    A/B arms nothing there; 'set gsdr' refused off G and nothing without an SDR on G. Receive only throughout"""
    import orch_cable as oc
    _need_state(o)
    A, B = H.A, H.B
    res = {}
    with o.step("both on 14.200 USB, page A; 'set gsdr' refused off page G") as s:
        _page(o, "A")
        o.both({"freq": 14200000, "mode": 2})
        r = o.r[B]
        n0 = len(r.errs)
        r.send(f"set gsdr {CH[20]}")
        r.wait("the refusal", lambda: any("gsdr: only while the Sentry shows G" in e for e in r.errs[n0:]), 5)
        s.confirm(r.errs[-1])
    with oc.cfg_kept(o.r[A], XV), oc.cfg_kept(o.r[B], XV):
        try:
            with o.step("the transverter on both; F's GMRS panel: heard on A (gmrx 2), both on one channel") as s:
                _page(o, "F")

                def panel(n, r):
                    r.send("set gmp 1")
                    r.wait("the panel", lambda: r.state.get("gmp") == 1, 5)
                    r.wait("A hears GMRS", lambda: r.state.get("gmrx") == 2, 5)
                    return r.state.get("gmhz")
                got = H.in_parallel(o, panel)
                want = got[A]
                rb = o.r[B]
                for _ in range(len(ROWS)):                  # the X6200's choice to the X6100's, the VFO a row a click
                    if rb.state.get("gmhz") == want:
                        break
                    rb.send("knob main 1")
                    time.sleep(0.4)
                if rb.state.get("gmhz") != want:
                    raise Refused(f"{B}'s panel choice {rb.state.get('gmhz')} never reached {A}'s {want}")
                res["channel"] = NAMES[want]
                s.confirm(f"both on {NAMES[want]}, gmrx 2")
            with o.step(f"F1 (TUNE) on both: A on {res['channel']} in NFM, alike") as s:
                def tune(n, r):
                    bench.tap(r, "F1")
                    r.wait("A on it", lambda: r.state.get("pg") == "A" and int(r.state.get("freq", 0)) == want and
                           int(r.state.get("mode", -1)) == 7, 8)
                    return int(r.state.get("freq"))
                got = H.in_parallel(o, tune)
                _apart_ok(o)
                s.confirm(str(got))
            res["rx"] = {n: {k: r.state.get(k) for k in ("pre", "att", "agc")} | {"meter": r.meter}
                         for n, r in o.r.items()}           # the transverter band's own receiver set-up, its noise
            with o.step("A/B on A at GMRS on both: nothing armed") as s:
                for r in o.r.values():
                    bench.tap(r, "AB")
                time.sleep(1.5)
                bad = [n for n, r in o.r.items() if r.state.get("rxo") != 1 or r.state.get("tx")]
                if bad:
                    for n in bad:
                        bench.arm(o.r[n], False)
                    raise Refused(f"A/B armed {bad} on GMRS")
                s.confirm("rxo 1, tx 0 on both")
            with o.step("the radio's own PTT at GMRS on each: refused with GMRS's words, off the air") as s:
                said = {}
                for n, r in o.r.items():
                    n0, t0, went = len(r.errs), time.time(), False
                    r.send(f"key PTT 1 {o.hold()}")
                    while time.time() - t0 < 2.5:
                        went = went or bool(r.tx)
                        time.sleep(0.05)
                    r.send("key PTT 0")
                    r.stop()
                    if went:
                        raise Refused(f"{n} WENT ON THE AIR at GMRS")
                    e = [x for x in r.errs[n0:] if x.startswith("ptt:")]
                    if not e or "GMRS" not in e[-1]:
                        raise Refused(f"{n}: PTT at GMRS not refused for GMRS (it said {r.errs[n0:]})")
                    said[n] = e[-1]
                res["ptt"] = said
                s.confirm(str(said))
            res["a_scan"] = {}
            for n, r in o.r.items():
                other = H.B if n == H.A else H.A
                with o.step(f"A's F1 on {n} (the other on 14.200 USB meanwhile, declared apart): GMRS's channels in "
                            f"frequency order, F1 ends it, neither radio keyed") as s:
                    hops, loud, keyed = [], [], []
                    with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                                     f"A's GMRS scan on {n}: its dial steps GMRS's channels, {other} waits on 14.200"):
                        # A change under way while the scan moves the dial by itself (the guard's RF alarm paused: the
                        # transverter band's noise reads near its -100 dBm, gmrs-xvtr 20:09 against -115 tuned there
                        # straight); this step watches for a key itself
                        o.changing += 1
                        try:
                            bench.ensure(o.r[other], {"freq": 14200000, "mode": 2})
                            start, moved = int(r.state.get("freq", 0)), False
                            bench.tap(r, "F1")
                            t0 = time.time()
                            while time.time() - t0 < 45 and len(hops) < 6:
                                f = int(r.state.get("freq", 0))
                                moved = moved or f != start     # the channel it starts on is no hop of the scan's
                                if moved and f in SCAN_ORDER and (not hops or hops[-1] != f):
                                    hops.append(f)
                                keyed += [x for x, y in o.r.items() if y.tx]
                                if r.meter is not None and r.meter > H.RF_ALARM_DBM:
                                    loud.append(round(r.meter, 1))
                                time.sleep(0.1)
                            for _ in range(3):          # F1 again, till the dial stays put (a key a stalled link
                                bench.tap(r, "F1")      # dropped is pressed again)
                                f0, t1, still = r.state.get("freq"), time.time(), True
                                while time.time() - t1 < 5:
                                    if r.state.get("freq") != f0:
                                        still = False
                                        break
                                    time.sleep(0.1)
                                if still:
                                    break
                            else:
                                raise Refused(f"{n}: A's scan did not stop on F1")
                        finally:
                            o.changing -= 1
                        o.both({"freq": CH[20], "mode": 7})
                    if keyed:
                        raise Refused(f"{sorted(set(keyed))} ON THE AIR during A's GMRS scan")
                    order = [SCAN_ORDER.index(f) for f in hops]
                    skips = [(a, b) for a, b in zip(order, order[1:]) if b != (a + 1) % len(SCAN_ORDER)]
                    if len(hops) < 4 or skips:
                        raise Refused(f"{n}: A's GMRS scan went {[NAMES[f] for f in hops]}")
                    res["a_scan"][n] = [NAMES[f] for f in hops]
                    res.setdefault("scan_meter_over_alarm", {})[n] = loud[:10]
                    s.confirm(", ".join(res["a_scan"][n]) + (f"; its meter over {H.RF_ALARM_DBM} dBm at times "
                                                             f"({max(loud)}), nothing keyed" if loud else ""))
            with o.step("G on GMRS 20 through the radio on both; CH + (BAND UP): GMRS 21 on both") as s:
                _page(o, "G")
                o.both({"freq": CH[20], "mode": 7})
                r = o.r[B]
                r.send(f"set gsdr {CH[20]}")            # no SDR: G stays the radio's
                time.sleep(1.0)
                if r.state.get("ghz"):
                    raise Refused(f"{B}: G went to an SDR channel with no SDR ({r.state.get('ghz')})")

                def up(n, r):
                    bench.tap(r, "BAND_UP")
                    r.wait("GMRS 21", lambda: int(r.state.get("freq", 0)) == CH[21], 5)
                    return int(r.state.get("freq"))
                got = H.in_parallel(o, up)
                _apart_ok(o)
                s.confirm(str(got))
            with o.step("A/B on G at GMRS on both: nothing armed") as s:
                for r in o.r.values():
                    bench.tap(r, "AB")
                time.sleep(1.5)
                bad = [n for n, r in o.r.items() if r.state.get("rxo") != 1 or r.state.get("tx")]
                if bad:
                    for n in bad:
                        bench.arm(o.r[n], False)
                    raise Refused(f"A/B on G armed {bad} on GMRS")
                s.confirm("rxo 1, tx 0 on both")
        finally:
            for r in o.r.values():
                if r.state.get("gmp") == 1:
                    r.send("set gmp 0")
            o.both({"freq": 14200000, "mode": 2})   # off GMRS before the transverter goes
            _page(o, "A")
    return True, (f"through a GMRS transverter on both: A on {res.get('channel')} in NFM, A/B and the PTT refused "
                  f"({res.get('ptt', {}).get(A, '')}), A's scan {res.get('a_scan', {}).get(A)}, G's CH + on GMRS"), res


COVERS = {
    "gmrs-b": (["sentry-b-gmrs-channels", "remote-sdr-state"], ["sentry-b-f2-preset"]),
    "gmrs-scan": (["sentry-b-gmrs-scan"], []),
    "gmrs-g": (["g-gmrs-groups", "g-gmrs-no-tx"], ["g-sdr-tx-refusals"]),
    "gmrs-f": (["f-system-gmrs"], []),
    "gmrs-xvtr": (["a-gmrs-wr12", "tx-gmrs-never"], ["g-gmrs-groups", "g-gmrs-no-tx", "f-system-gmrs"]),
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
        Test("gmrs-xvtr", "both RX", "a GMRS transverter on both (the WR12's stand-in): F's panel tunes A in NFM, "
             "A/B and the PTT refused as GMRS, A's scan of the 22, G's GMRS group on the radio", t_gmrs_xvtr, 120),
    ]
