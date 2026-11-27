#!/usr/bin/env python3
"""Where the radios may not transmit (orch_tx_rules.py, 2026-11-27): the refusals the TX features promise, each tried
on both radios, nothing armed. The remote checks the frequency before anything else (remote_server.c ptt_down,
tune_cmd: radio_ptt_refused and cfg_tx_refusal_text, ext_tune_refusal) and answers a press with its reason,
"err ptt: no TX on 7.301 MHz: outside the amateur bands (Settings > TX Cal)"; that line is the judge. The Sentry's
receive-only hold stays on throughout (rxo 1, its base at 0 W): a refusal that went missing would still send nothing,
and is found by its missing line.

- tx-edges: 1 kHz outside every edge of the built-in table (ham_bands.c: 2200 m to 6 m, the Alaska channel, 60 m,
  8 m), both radios on the frequency alike, the PTT refused for the amateur bands on each; TUNE (an external tuner's)
  refused the same on three of them
- tx-rx-only x6200: the X6200's receive-only ranges (FM broadcast, the airband's start), the X6100 waiting on 14.200
  (the two declared apart): the PTT and TUNE refused as receive only
- tx-ranges-kr: Settings > TX Cal > TX ranges South Korea on both (cfg tx_ranges 1, put back after): ham-band
  frequencies outside Korea's ranges refused as outside the TX ranges
- tx-ranges-own: the own list (/mnt/tx_ranges.conf, kept and put back byte for byte) with two ranges and a line that
  is none, chosen as the web does (the configurator socket: the file read then): its count and bad line read back,
  frequencies outside it refused as outside the TX ranges
- tx-split-out: split with VFO B on 27.185 MHz (CB) and A on 14.200: the PTT refused for B's frequency, the one it
  would go out on

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import time

import bench
from bench import Refused
import orch_cov_apps as oa

H = None                        # the running orch module (orch.py: orch_tx_rules.H = sys.modules[__name__])

USB = 2
F_HOME = 14200000               # the bench's voice frequency: where a test leaves the radios
PRESS_S = 1.5                   # a press's answer awaited this long (it comes at once: the socket thread's own)

OFF_BAND = "outside the amateur bands (Settings > TX Cal)"      # tx_ranges.c tx_ranges_why
OFF_RANGE = "outside the TX ranges (Settings > TX Cal)"
RX_ONLY = "receive only"

# 1 kHz outside each edge of ham_bands.c's table, both radios tune all of them (the caps map: 0.1-54 MHz)
EDGES = [
    (134700, "2200 m low"), (138800, "2200 m high"), (471000, "630 m low"), (480000, "630 m high"),
    (1799000, "160 m low"), (2001000, "160 m high"), (3499000, "80 m low"), (4001000, "80 m high"),
    (5166000, "Alaska low"), (5169000, "Alaska high"), (5249000, "60 m low"), (5451000, "60 m high"),
    (6999000, "40 m low"), (7301000, "40 m high"), (10099000, "30 m low"), (10151000, "30 m high"),
    (13999000, "20 m low"), (14351000, "20 m high"), (18067000, "17 m low"), (18169000, "17 m high"),
    (20999000, "15 m low"), (21451000, "15 m high"), (24889000, "12 m low"), (24991000, "12 m high"),
    (27999000, "10 m low"), (29701000, "10 m high"), (39999000, "8 m low"), (45001000, "8 m high"),
    (49999000, "6 m low"), (54001000, "6 m high"),
]
TUNE_AT = (5169000, 7301000, 27999000)      # TUNE tried here too

# the X6200's receive-only ranges (the caps map: it tunes 88-118 MHz, the X6100 none of it)
RX_ONLY_F = [(88500000, "FM broadcast low"), (100000000, "FM broadcast"), (107900000, "FM broadcast high"),
             (118000000, "the airband")]

# South Korea's ranges (tx_ranges.c kr[]): in the amateur bands, outside Korea's
KR_OUT = [(1830000, "160 m past 1.825"), (3560000, "80 m past 3.550"), (3700000, "80 m between Korea's two"),
          (5357000, "60 m, none in Korea"), (7250000, "40 m past 7.200")]

TX_RANGES_FILE = "/mnt/tx_ranges.conf"      # BOARD_DATA "/tx_ranges.conf" (settings_manager_api.h)
OWN_LIST = ("# the bench's own list (orch tx-ranges-own), put back after\n"
            "7.000-7.100\n"
            "7,1-7,2\n"
            "14.000000 - 14.100   # 20 m's CW end\n")
OWN_COUNT, OWN_BAD, OWN_FIRST_BAD = 2, 1, 3
OWN_OUT = [(7150000, "40 m, its bad line's"), (7250000, "40 m past both"), (14200000, "20 m past 14.100"),
           (21100000, "15 m, not on it")]
NO_RANGES = "no TX ranges in the own list (Settings > TX Cal)"
F_EMPTY = 14074000              # with the own list empty: refused even in the middle of a band

# a press without the page's number (a page older than the radio): remote_server.c ptt_down, tune_cmd
OLD_PAGE_PTT = "this session may not transmit from a page older than the radio: reload the page"
OLD_PAGE_TUNE = "tune 1 HOLD: reload the page"

F_CB = 27185000                 # CB 19: VFO B's in tx-split-out


def mhz(f):
    """cfg_tx_mhz (settings_manager_api.cpp): 3 decimals, more where the Hz need them"""
    a = abs(int(f))
    sub, d = a % 1000000, 3
    if sub % 1000:
        d = 6
        while sub % 10 == 0:
            sub //= 10
            d -= 1
    else:
        sub //= 1000
    return f"{'-' if f < 0 else ''}{a // 1000000}.{sub:0{d}d} MHz"


def _off_air(o):
    """Nothing armed on either radio: the Sentry's receive-only hold on (rxo 1), off the air"""
    bad = {n: (r.state.get("rxo"), r.state.get("tx")) for n, r in o.r.items()
           if r.state.get("rxo") != 1 or r.state.get("tx")}
    if bad:
        for n in bad:
            try:
                bench.arm(o.r[n], False)
            except Refused:
                pass
        raise Refused(f"armed or on the air before a press: {bad} (rxo, tx)")


def _outside_on(o, n):
    if str(oa.cfg(o.r[n], "tx_outside_bands")) != "0":
        raise Refused(f"{n}: TX outside ham bands is on: the band's refusal cannot be seen")


def press(o, r, what, f, why):
    """r's PTT (what 'ptt') or TUNE ('tune') pressed and let go: the remote's err line for it, which must name f and
    why; r off the air throughout. Returns the line"""
    n0, t0, went = len(r.errs), time.time(), False
    r.send(f"{'key PTT 1' if what == 'ptt' else 'tune 1'} {o.hold()}")
    line = None
    try:
        while time.time() - t0 < PRESS_S:
            went = went or bool(r.tx) or bool(r.state.get("tune"))
            e = [x for x in r.errs[n0:] if x.startswith(f"{what}:")]
            if e:
                line = e[-1]
                time.sleep(0.2)     # a key that went out all the same shows by now
                went = went or bool(r.tx) or bool(r.state.get("tune"))
                break
            time.sleep(0.05)
    finally:
        r.send("key PTT 0" if what == "ptt" else "tune 0")
    if went:
        r.stop()
        raise Refused(f"{r.name} WENT ON THE AIR ({what}) on {mhz(f)}")
    want = f"{what}: no TX on {mhz(f)}: {why}"
    if line != want:
        raise Refused(f"{r.name}: {what} on {mhz(f)}: {line or 'no refusal'!r}, not {want!r} "
                      f"(it said {r.errs[n0:][-3:]})")
    return line


def _both_on(o, f, mode=USB):
    o.both({"freq": int(f), "mode": mode})
    for n, r in o.r.items():
        if int(r.state.get("freq", 0)) != int(f):
            raise Refused(f"{n} reads {r.state.get('freq')} after asking {f}")


def t_tx_edges(o):
    """1 kHz outside each edge of the amateur bands' table, both radios there alike: PTT refused on each for the
    amateur bands ('no TX on 7.301 MHz: outside the amateur bands'), off the air; TUNE the same at three"""
    res = {"refused": 0, "tune": 0}
    try:
        for n in o.r:
            _outside_on(o, n)
        with o.step("both on the Sentry's A, nothing armed") as s:
            oa.page_both(o, "A")
            _off_air(o)
            s.confirm("rxo 1 on both")
        for f, name in EDGES:
            with o.step(f"{name}: {mhz(f)} on both, the PTT refused on each for the amateur bands"
                        + (", TUNE too" if f in TUNE_AT else "")) as s:
                _both_on(o, f)
                _off_air(o)
                said = {}
                for n, r in o.r.items():
                    said[n] = press(o, r, "ptt", f, OFF_BAND)
                    res["refused"] += 1
                    if f in TUNE_AT:
                        press(o, r, "tune", f, OFF_BAND)
                        res["tune"] += 1
                s.confirm(said[H.A])
        return True, f"{len(EDGES)} frequencies 1 kHz outside the table's edges (2200 m to 6 m, the Alaska channel, " \
                     f"60 m, 8 m): the PTT refused for the amateur bands {res['refused']} times on the two radios, " \
                     f"TUNE {res['tune']}; neither went on the air", res
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            _both_on(o, F_HOME)
        except Refused:
            pass
        oa.home_both(o)


def t_tx_rx_only(o):
    """The X6200 in its receive-only ranges (FM broadcast, the airband's start: it tunes them, the X6100 does not),
    the X6100 on 14.200 meanwhile (declared apart): the PTT and TUNE refused as receive only, off the air"""
    R, X = o.r[H.B], o.r[H.A]
    res = {}
    try:
        _outside_on(o, H.B)
        with o.step("both on the Sentry's A on 14.200 USB, nothing armed") as s:
            oa.page_both(o, "A")
            _both_on(o, F_HOME)
            _off_air(o)
            s.confirm("rxo 1 on both")
        with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                         "tx-rx-only: the X6200 in its receive-only ranges, which the X6100 does not tune; the X6100 "
                         "waits on 14.200"):
            for f, name in RX_ONLY_F:
                with o.step(f"x6200 on {mhz(f)} ({name}): PTT and TUNE refused as receive only") as s:
                    o.changing += 1
                    try:
                        bench.ensure(R, {"freq": f})
                    finally:
                        o.changing -= 1
                    if int(R.state.get("freq", 0)) != f:
                        raise Refused(f"x6200 reads {R.state.get('freq')} after asking {f}")
                    # the Sentry may show another page for VHF (AIR came up at 120 MHz, 11-26): armed is rxo 0 alone
                    if R.state.get("rxo") == 0 or R.tx:
                        raise Refused(f"x6200 armed or on the air on {mhz(f)} (rxo {R.state.get('rxo')}, page "
                                      f"{R.state.get('pg')})")
                    res[name] = press(o, R, "ptt", f, RX_ONLY)
                    press(o, R, "tune", f, RX_ONLY)
                    s.confirm(res[name])
            with o.step("x6200 back on 14.200 USB, with the X6100") as s:
                o.changing += 1
                try:
                    bench.ensure(R, {"freq": F_HOME, "mode": USB})
                finally:
                    o.changing -= 1
                s.confirm(f"x6200 {R.state.get('freq')}, x6100 {X.state.get('freq')}")
        return True, f"the X6200's receive-only ranges refused for the PTT and TUNE as receive only: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            o.changing += 1
            try:
                bench.ensure(R, {"freq": F_HOME, "mode": USB})
            finally:
                o.changing -= 1
        except Refused:
            pass
        oa.home_both(o)


def _ranges_back(o, was):
    """tx_ranges as found on each radio, chosen as the web chooses it (the configurator socket: the own list, if it
    was chosen, read again from its file as it is now), the remote's cfg if that fails; read back"""
    import suite_air_a as aa
    for n, v in was.items():
        r = o.r[n]
        try:
            ans = aa.cfg_sock(r.host, {"cmd": "disp_set", "text": "tx_ranges", "val": int(v)})
            if ans.get("t") != "ok":
                raise Refused(f"the configurator said {ans}")
        except (Refused, OSError, ValueError) as e:
            o.st.ev(f"{n}: tx_ranges not put back to {v} by the configurator ({e}): by the remote", "bad")
            try:
                bench.cfg_set(r, "tx_ranges", v)
            except Refused as e2:
                o.st.ev(f"{n}: tx_ranges not put back to {v}: {e2}", "bad")
        now = str(oa.cfg(r, "tx_ranges"))
        if now != str(v):
            o.st.ev(f"{n}: tx_ranges reads {now} after putting {v} back", "bad")


def t_tx_ranges_kr(o):
    """Settings > TX Cal > TX ranges South Korea on both (put back as found after): frequencies in the amateur bands
    but outside Korea's ranges refused on each as outside the TX ranges, off the air"""
    res = {}
    was = {}
    try:
        for n in o.r:
            _outside_on(o, n)
        with o.step("both on the Sentry's A, nothing armed; TX ranges South Korea on both") as s:
            oa.page_both(o, "A")
            _off_air(o)
            for n, r in o.r.items():
                was[n] = str(oa.cfg(r, "tx_ranges"))
                old, now = bench.cfg_set(r, "tx_ranges", 1)
                if str(now) != "1":
                    raise Refused(f"{n}: tx_ranges reads {now!r} after asking 1")
            s.confirm(f"tx_ranges as found {was}, now 1 on both")
        for f, name in KR_OUT:
            with o.step(f"{mhz(f)} ({name}) on both: the PTT refused as outside the TX ranges") as s:
                _both_on(o, f)
                _off_air(o)
                said = {n: press(o, r, "ptt", f, OFF_RANGE) for n, r in o.r.items()}
                if f == 7250000:
                    for n, r in o.r.items():
                        press(o, r, "tune", f, OFF_RANGE)
                res[name] = said[H.A]
                s.confirm(said[H.A])
        return True, f"South Korea's TX ranges on both: {len(KR_OUT)} frequencies in the amateur bands outside them " \
                     f"refused as outside the TX ranges (TUNE too on 7.250): {list(res.values())[:2]}", res
    except Refused as e:
        return False, str(e), res
    finally:
        _ranges_back(o, was)
        try:
            _both_on(o, F_HOME)
        except Refused:
            pass
        oa.home_both(o)


def t_tx_ranges_own(o):
    """The own list on both: /mnt/tx_ranges.conf (kept, put back byte for byte after) with 7.000-7.100, a line with a
    decimal comma (not a range) and 14.000-14.100 with spaces and a comment; Own list chosen as the web chooses it (the
    configurator socket: the file read then), its answer 2 ranges, 1 bad line, the 3rd; frequencies outside it refused
    on each as outside the TX ranges; tx_ranges put back as found"""
    import contextlib
    import suite_air_a as aa
    res = {}
    was = {}
    try:
        for n in o.r:
            _outside_on(o, n)
        with contextlib.ExitStack() as keep:
            for r in o.r.values():
                keep.enter_context(oa.file_kept(r, TX_RANGES_FILE))
            with o.step("both on the Sentry's A, nothing armed; the own list written and chosen on both") as s:
                oa.page_both(o, "A")
                _off_air(o)
                for n, r in o.r.items():
                    was[n] = str(oa.cfg(r, "tx_ranges"))
                    body = OWN_LIST.replace("'", "")
                    oa.ssh(r, f"printf '%s' '{body}' > {TX_RANGES_FILE} && sync")
                    ans = aa.cfg_sock(r.host, {"cmd": "disp_set", "text": "tx_ranges", "val": 2})
                    if ans.get("t") != "ok":
                        raise Refused(f"{n}: Own list not chosen: {ans}")
                    got = aa.cfg_sock(r.host, {"cmd": "tx_ranges_read"})
                    res[n] = {k: got.get(k) for k in ("tx_own_count", "tx_own_bad", "tx_own_first_bad",
                                                      "tx_own_missing")}
                    want = {"tx_own_count": OWN_COUNT, "tx_own_bad": OWN_BAD, "tx_own_first_bad": OWN_FIRST_BAD,
                            "tx_own_missing": 0}
                    if res[n] != want:
                        raise Refused(f"{n}: the own list read as {res[n]}, not {want}")
                    if str(oa.cfg(r, "tx_ranges")) != "2":
                        raise Refused(f"{n}: tx_ranges reads {oa.cfg(r, 'tx_ranges')} after Own list")
                s.confirm(f"as found {was}; read {res[H.A]}")
            for f, name in OWN_OUT:
                with o.step(f"{mhz(f)} ({name}) on both: the PTT refused as outside the TX ranges") as s:
                    _both_on(o, f)
                    _off_air(o)
                    said = {n: press(o, r, "ptt", f, OFF_RANGE) for n, r in o.r.items()}
                    res[name] = said[H.A]
                    s.confirm(said[H.A])
            with o.step(f"the own list empty (comments only), chosen again on both: no ranges, the PTT on "
                        f"{mhz(F_EMPTY)} refused for it") as s:
                for n, r in o.r.items():
                    oa.ssh(r, f"printf '# nothing\\n\\n' > {TX_RANGES_FILE} && sync")
                    ans = aa.cfg_sock(r.host, {"cmd": "disp_set", "text": "tx_ranges", "val": 2})
                    if ans.get("t") != "ok":
                        raise Refused(f"{n}: Own list not chosen again: {ans}")
                    got = aa.cfg_sock(r.host, {"cmd": "tx_ranges_read"})
                    if got.get("tx_own_count") != 0 or got.get("tx_own_bad") != 0:
                        raise Refused(f"{n}: the empty own list read as {got}")
                _both_on(o, F_EMPTY)
                _off_air(o)
                said = {n: press(o, r, "ptt", F_EMPTY, NO_RANGES) for n, r in o.r.items()}
                res["empty"] = said[H.A]
                s.confirm(said[H.A])
        return True, f"the own list on both (2 ranges, the decimal comma's line not read, the 3rd): " \
                     f"{len(OWN_OUT)} frequencies outside it refused as outside the TX ranges; empty, " \
                     f"{mhz(F_EMPTY)} refused for no ranges", res
    except Refused as e:
        return False, str(e), res
    finally:
        _ranges_back(o, was)        # after the file went back (the ExitStack): the list as found read again if chosen
        try:
            _both_on(o, F_HOME)
        except Refused:
            pass
        oa.home_both(o)


def t_tx_old_page(o):
    """A PTT or TUNE press without the page's number (a page open since before an update): refused on each radio,
    told to reload ('err this session may not transmit from a page older than the radio: reload the page', 'err tune 1
    HOLD: reload the page'), off the air; on 14.200, where the band would let it"""
    res = {}
    try:
        with o.step("both on the Sentry's A on 14.200 USB, nothing armed") as s:
            oa.page_both(o, "A")
            _both_on(o, F_HOME)
            _off_air(o)
            s.confirm("rxo 1 on both")
        for n, r in o.r.items():
            with o.step(f"{n}: 'key PTT 1' and 'tune 1' with no number: refused, reload the page") as s:
                for cmd, release, want in (("key PTT 1", "key PTT 0", OLD_PAGE_PTT), ("tune 1", "tune 0",
                                                                                      OLD_PAGE_TUNE)):
                    n0, t0, went = len(r.errs), time.time(), False
                    r.send(cmd)
                    while time.time() - t0 < PRESS_S and want not in r.errs[n0:]:
                        went = went or bool(r.tx) or bool(r.state.get("tune"))
                        time.sleep(0.05)
                    time.sleep(0.2)
                    went = went or bool(r.tx) or bool(r.state.get("tune"))
                    r.send(release)
                    if went:
                        r.stop()
                        raise Refused(f"{n} WENT ON THE AIR on '{cmd}' with no number")
                    if want not in r.errs[n0:]:
                        raise Refused(f"{n}: '{cmd}' with no number: not {want!r} (it said {r.errs[n0:][-3:]})")
                    res.setdefault(n, []).append(want)
                s.confirm(str(res[n]))
        return True, f"a press with no number refused on both and told to reload, the PTT's and TUNE's: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


F_SHIFT = 29620000              # 10 m FM, where a repeater's shift lives
SHIFT = -100000                 # -100 kHz, as a 10 m repeater's
F_OTHER_BAND = 24940000         # 12 m: out of the shifted band and back
# The web takes a shift the hardware sends on wherever the band lands (band_use: HF 0.1-55 MHz, the transverters):
# +1000 kHz on 10 m (29.0-30.7 MHz) is taken, and VFO B on 30.500 then is outside the amateur bands, where the PTT is
# refused; +2000 kHz on 6 m (52-56 MHz) is past the hardware's 55 MHz and refused by the web itself
SHIFT_OUT = 1000000
F_SHIFT_OUT = 29500000
F_6M, SHIFT_6M = 50100000, 2000000


def _band_at(r, f):
    """The radio's active band (web /api/bands) f is in, None when none"""
    st, bands = oa.http_json(r, "/api/bands")
    if st != 200 or not isinstance(bands, list):
        raise Refused(f"{r.name}: the web's band list not read (HTTP {st})")
    return next((b for b in bands if b.get("type") == 1 and b["start_freq"] <= f < b["stop_freq"]), None)


def _shift_post(r, b, shift):
    """The band's TX shift set as the web Bands page sets it: (HTTP status, its text)"""
    edges = {k: b[k] for k in ("name", "start_freq", "stop_freq", "type")}
    return oa.http(r, f"/api/bands/{b['id']}", "POST", dict(edges, params={"tx_shift": shift}))


def _txf_is(o, want_f, want_txf, want_split, what, secs=3.0):
    """Both radios read freq, TX freq and split as wanted within secs: {name: (freq, txf, split)}"""
    end = time.time() + secs
    while True:
        got = {n: (int(r.state.get("freq", 0)), int(r.state.get("txf") or 0), int(r.state.get("split", -1)))
               for n, r in o.r.items()}
        if all(v == (want_f, want_txf, want_split) for v in got.values()):
            return got
        if time.time() > end:
            raise Refused(f"{what}: (freq, TX freq, split) {got}, not {(want_f, want_txf, want_split)}")
        time.sleep(0.1)


def t_tx_shift_rules(o):
    """A TX shift of -100 kHz on 10 m from the web's Bands page on both (put back to 0 after): on 29.620 NFM split
    comes on with VFO B 100 kHz down; the VFO key gives REV (listening on 29.520, sending on 29.620) and back; A tuned
    moves B with it; split off pauses the shift, on again brings it; out of the band (12 m) it goes, back in it comes.
    A shift of +1000 kHz (taken: the hardware sends on 29.0-30.7 MHz) puts B on 30.500 from 29.500, outside the
    amateur bands: the PTT refused for 30.500 on each. The web refuses +2000 kHz on 6 m (56 MHz, past the hardware's
    55), 6 m's shift kept as found"""
    res = {}
    band = {}
    try:
        with o.step("both on the Sentry's A on 14.200 USB, nothing armed; 10 m's band on each, no shift as found") as s:
            oa.page_both(o, "A")
            _both_on(o, F_HOME)
            _off_air(o)
            for n, r in o.r.items():
                b = _band_at(r, F_SHIFT)
                if not b:
                    raise Refused(f"{n}: no active band at {mhz(F_SHIFT)}")
                p = b.get("params", {})
                if p.get("tx_shift") or p.get("split"):
                    raise Refused(f"{n}: {b['name']} has TX shift {p.get('tx_shift')} / split {p.get('split')} as "
                                  f"found: left alone")
                band[n] = b
            s.confirm(", ".join(f"{n} {b['name']} {mhz(b['start_freq'])}-{mhz(b['stop_freq'])}"
                                for n, b in band.items()))
        with o.step(f"the shift {SHIFT // 1000} kHz from the web on both; on {mhz(F_SHIFT)} NFM: split on, VFO B "
                    f"{mhz(F_SHIFT + SHIFT)}") as s:
            for n, r in o.r.items():
                st, text = _shift_post(r, band[n], SHIFT)
                if st != 200:
                    raise Refused(f"{n}: the web took no shift: HTTP {st} {text[:120]}")
            _both_on(o, F_SHIFT, 7)
            res["on"] = _txf_is(o, F_SHIFT, F_SHIFT + SHIFT, 1, "the shift's band tuned")
            s.confirm(str(res["on"]))
        with o.step("the VFO key on both: REV (listening on B's frequency, sending on A's), then back") as s:
            o.both({"vfo": 1})
            res["rev"] = _txf_is(o, F_SHIFT + SHIFT, F_SHIFT, 1, "REV")
            o.both({"vfo": 0})
            res["back"] = _txf_is(o, F_SHIFT, F_SHIFT + SHIFT, 1, "REV off")
            s.confirm(f"REV {res['rev'][H.A]}, back {res['back'][H.A]}")
        with o.step("A tuned to 29.600: B follows 100 kHz down") as s:
            _both_on(o, F_SHIFT - 20000, 7)
            res["follow"] = _txf_is(o, F_SHIFT - 20000, F_SHIFT - 20000 + SHIFT, 1, "A tuned")
            s.confirm(str(res["follow"][H.A]))
        with o.step("split off: the shift paused (TX on the dial); split on: back") as s:
            o.both({"split": 0})
            res["paused"] = _txf_is(o, F_SHIFT - 20000, F_SHIFT - 20000, 0, "split off")
            o.both({"split": 1})
            res["again"] = _txf_is(o, F_SHIFT - 20000, F_SHIFT - 20000 + SHIFT, 1, "split on again")
            s.confirm(f"paused {res['paused'][H.A]}, again {res['again'][H.A]}")
        with o.step(f"out of the band ({mhz(F_OTHER_BAND)}): no shift; back on {mhz(F_SHIFT)}: the shift again") as s:
            _both_on(o, F_OTHER_BAND, USB)
            res["out"] = _txf_is(o, F_OTHER_BAND, F_OTHER_BAND, 0, "out of the band")
            _both_on(o, F_SHIFT, 7)
            res["in"] = _txf_is(o, F_SHIFT, F_SHIFT + SHIFT, 1, "the band again")
            s.confirm(f"out {res['out'][H.A]}, in {res['in'][H.A]}")
        with o.step(f"{SHIFT_OUT // 1000:+d} kHz from the web on both (out of the band first): on "
                    f"{mhz(F_SHIFT_OUT)} B is on {mhz(F_SHIFT_OUT + SHIFT_OUT)}, the PTT refused for it") as s:
            _both_on(o, F_HOME)
            time.sleep(3.0)                 # 10 m's settings written as the radios left it, before the web's
            for n, r in o.r.items():
                st, text = _shift_post(r, band[n], SHIFT_OUT)
                if st != 200:
                    raise Refused(f"{n}: the web did not take {SHIFT_OUT // 1000:+d} kHz: HTTP {st} {text[:120]}")
            _both_on(o, F_SHIFT_OUT, 7)
            res["out_shift"] = _txf_is(o, F_SHIFT_OUT, F_SHIFT_OUT + SHIFT_OUT, 1, "the shift past 29.7 MHz")
            _off_air(o)
            res["refused"] = {n: press(o, r, "ptt", F_SHIFT_OUT + SHIFT_OUT, OFF_BAND) for n, r in o.r.items()}
            s.confirm(res["refused"][H.A])
        with o.step(f"the web asked for {SHIFT_6M // 1000:+d} kHz on 6 m on both: refused (past 55 MHz), 6 m's "
                    f"shift as found") as s:
            said = {}
            for n, r in o.r.items():
                b6 = _band_at(r, F_6M)
                if not b6:
                    raise Refused(f"{n}: no active band at {mhz(F_6M)}")
                was6 = int(b6.get("params", {}).get("tx_shift") or 0)
                st, text = _shift_post(r, b6, SHIFT_6M)
                if st == 200:
                    _shift_post(r, b6, was6)
                    raise Refused(f"{n}: the web took {SHIFT_6M // 1000:+d} kHz on {b6['name']}")
                if "does not transmit" not in text:
                    raise Refused(f"{n}: refused without the reason: HTTP {st} {text[:160]}")
                kept = int((_band_at(r, F_6M) or {}).get("params", {}).get("tx_shift") or 0)
                if kept != was6:
                    raise Refused(f"{n}: 6 m's shift reads {kept} after the refused one (was {was6})")
                said[n] = f"HTTP {st}"
            res["web_6m"] = said
            s.confirm(str(said))
        return True, f"10 m with a {SHIFT // 1000} kHz shift on both: split on with B {SHIFT // 1000} kHz off, REV " \
                     f"and back, B following A, split off pausing it, out of the band and back; " \
                     f"{SHIFT_OUT // 1000:+d} kHz put B on {mhz(F_SHIFT_OUT + SHIFT_OUT)}, the PTT refused there; " \
                     f"the web refused {SHIFT_6M // 1000:+d} kHz on 6 m", res
    except Refused as e:
        return False, str(e), res
    finally:
        # out of the band first (its settings written as the radio leaves it), then the shift 0 from the web, the band
        # tuned once more to see it gone
        try:
            _both_on(o, F_HOME)
            time.sleep(3.0)
            for n, b in band.items():
                st, text = _shift_post(o.r[n], b, 0)
                if st != 200:
                    o.st.ev(f"{n}: the shift not put back to 0: HTTP {st} {text[:120]}", "bad")
            if band:
                _both_on(o, F_SHIFT, 7)
                res["after"] = _txf_is(o, F_SHIFT, F_SHIFT, 0, "the shift put back to 0")
                o.both({"mode": USB})
                _both_on(o, F_HOME)
        except Refused as e:
            o.st.ev(f"the TX shift not seen put back: {e}", "bad")
        oa.home_both(o)


def t_armed_knob_lock(o):
    """The Sentry's A on both, TX armed (A/B: rxo 0, nothing keyed, 0.1 W): the VFO knob and the MFK turned on each
    leave the dial where it is (app_sentry.c dial_held: 'DIAL LOCKED: LOCK TX (A/B) TO TUNE' on its OSD, which the
    remote does not carry); locked again (A/B), the VFO knob tunes"""
    res = {}
    armed = False
    try:
        with o.step("both on the Sentry's A on 14.200 USB at 0.1 W, nothing armed") as s:
            oa.page_both(o, "A")
            o.both({"freq": F_HOME, "mode": USB, "pwr": H.W})
            _off_air(o)
            s.confirm("rxo 1 on both")
        with o.step("TX armed on both (A/B): rxo 0, off the air") as s:
            o.changing += 1
            try:
                armed = True
                for r in o.r.values():
                    bench.arm(r, True)
            finally:
                o.changing -= 1
            bad = {n: (r.state.get("rxo"), r.state.get("tx")) for n, r in o.r.items()
                   if r.state.get("rxo") != 0 or r.tx}
            if bad:
                raise Refused(f"not armed and off the air: {bad} (rxo, tx)")
            s.confirm("rxo 0, tx 0 on both")
        for knob, n_steps in (("main", 5), ("mfk", 2)):
            with o.step(f"armed: the {knob.upper()} knob {n_steps} steps on both, the dial kept") as s:
                for r in o.r.values():
                    r.send(f"knob {knob} {n_steps}")
                time.sleep(1.5)
                got = {n: int(r.state.get("freq", 0)) for n, r in o.r.items()}
                went = [n for n, r in o.r.items() if r.tx]
                if went:
                    raise Refused(f"{went} WENT ON THE AIR")
                if any(f != F_HOME for f in got.values()):
                    raise Refused(f"armed, the {knob} knob moved the dial: {got}")
                res[knob] = got
                s.confirm(str(got))
        with o.step("TX locked on both (A/B): rxo 1") as s:
            o.changing += 1
            try:
                for r in o.r.values():
                    bench.arm(r, False)
                armed = False
            finally:
                o.changing -= 1
            s.confirm("rxo 1 on both")
        with o.step("locked: the VFO knob 5 steps on both tunes (declared apart while it moves), then 14.200 "
                    "again") as s:
            with bench.apart({"freq": bench.STEPPED}, "armed-knob-lock: the VFO knob turned on both once locked, "
                                                      "each radio's own step"):
                o.changing += 1
                try:
                    for r in o.r.values():
                        r.send("knob main 5")
                    time.sleep(1.5)
                    res["locked"] = {n: int(r.state.get("freq", 0)) for n, r in o.r.items()}
                finally:
                    o.changing -= 1
                _both_on(o, F_HOME)
            if any(f == F_HOME for f in res["locked"].values()):
                raise Refused(f"locked, the VFO knob did not tune: {res['locked']}")
            s.confirm(str(res["locked"]))
        return True, f"armed on both, the VFO knob and the MFK left the dial on {mhz(F_HOME)}; locked, the VFO knob " \
                     f"tuned ({res['locked']})", res
    except Refused as e:
        return False, str(e), res
    finally:
        if armed:
            o.changing += 1
            try:
                for r in o.r.values():
                    try:
                        bench.arm(r, False)
                    except Refused as e:
                        o.st.ev(f"{r.name}: not locked again: {e}", "bad")
            finally:
                o.changing -= 1
        oa.home_both(o)


def t_tx_split_out(o):
    """Split on both with VFO A on 14.200 and VFO B on 27.185 (CB): the PTT refused for B's frequency, the one it
    would go out on ('no TX on 27.185 MHz'), off the air; VFO B and split put back as found"""
    res = {}
    b0 = {}
    try:
        for n in o.r:
            _outside_on(o, n)
        with o.step("both on the Sentry's A on 14.200 USB, nothing armed; VFO B's frequency as found") as s:
            oa.page_both(o, "A")
            _both_on(o, F_HOME)
            _off_air(o)
            o.both({"vfo": 1})
            b0 = {n: (int(r.state.get("freq", 0)), int(r.state.get("mode", USB))) for n, r in o.r.items()}
            s.confirm(f"VFO B as found {b0}")
        with o.step("VFO B on 27.185 USB, VFO A again, split on: both alike") as s:
            _both_on(o, F_CB)
            o.both({"vfo": 0})
            o.both({"split": 1})
            res["state"] = {n: (r.state.get("freq"), r.state.get("txf"), r.state.get("split"))
                            for n, r in o.r.items()}
            bad = {n: v for n, v in res["state"].items() if int(v[1] or 0) != F_CB or int(v[0] or 0) != F_HOME}
            if bad:
                raise Refused(f"split not as set (freq, TX freq, split): {bad}")
            s.confirm(str(res["state"]))
        with o.step("the PTT on each: refused for 27.185 MHz, B's") as s:
            _off_air(o)
            res["said"] = {n: press(o, r, "ptt", F_CB, OFF_BAND) for n, r in o.r.items()}
            s.confirm(res["said"][H.A])
        return True, f"split with VFO B on 27.185: the PTT refused for B's frequency on both " \
                     f"({res['said'][H.A]})", res
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            o.both({"split": 0})
            if b0:
                o.both({"vfo": 1})
                f, m = b0[H.A]
                o.both({"freq": f, "mode": m})
                o.both({"vfo": 0})
        except Refused as e:
            o.st.ev(f"split / VFO B not put back: {e}", "bad")
        oa.home_both(o)


COVERS = {
    "tx-edges": (["tx-band-edges-refused", "ext-tune-refused-out-of-band"], ["tx-refuse-apps-cw-tuners"]),
    "tx-rx-only": (["tx-receive-only-ranges"], ["tx-refuse-receive-only"]),
    "tx-ranges-kr": (["tx-ranges-south-korea"], []),
    "tx-ranges-own": (["tx-ranges-own-list-file", "tx-ranges-own-list"], []),
    "tx-split-out": (["tx-split-other-vfo-red"], []),
    "tx-old-page": (["panel-ptt-refused-old-page"], ["panel-ext-tune-refused"]),
    "tx-shift-rules": (["tx-shift-reverse", "tx-shift-band-scope", "tx-shift-out-of-band-refused"],
                       ["tx-shift-setting", "tx-shift-vfo-b-rules", "settings-general-tx-shift"]),
    "armed-knob-lock": ([], ["sentry-armed-knob-lock"]),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    role = "both RX (the PTT and TUNE refused, nothing armed)"
    return [
        Test("tx-edges", role, "1 kHz outside each edge of the amateur bands (2200 m to 6 m, the Alaska channel, 60 m, "
             "8 m) on both: the PTT refused for the amateur bands on each, TUNE too at three", t_tx_edges, 150),
        Test("tx-rx-only x6200", role, "the X6200 in its receive-only ranges (FM broadcast, 118 MHz), the X6100 on "
             "14.200: the PTT and TUNE refused as receive only", t_tx_rx_only, 40),
        Test("tx-ranges-kr", role, "TX ranges South Korea on both: ham-band frequencies outside Korea's refused as "
             "outside the TX ranges (TUNE too on 7.250); put back", t_tx_ranges_kr, 50),
        Test("tx-ranges-own", role, "the own list's file (two ranges, a bad line) chosen as the web does: read back 2 "
             "ranges and the bad line, frequencies outside it refused; the file and the choice put back",
             t_tx_ranges_own, 60),
        Test("tx-split-out", role, "split with VFO B on 27.185 (CB), A on 14.200: the PTT refused for B's frequency on "
             "both; VFO B and split put back", t_tx_split_out, 40),
        Test("tx-old-page", role, "a PTT or TUNE press with no number (a page older than the radio) refused on both, "
             "told to reload", t_tx_old_page, 25),
        Test("tx-shift-rules", role, "a -100 kHz TX shift on 10 m from the web on both: split on, REV and back, B "
             "following A, split off pausing it, out of the band and back; +1000 kHz puts B on 30.500, the PTT "
             "refused there; +2000 kHz on 6 m refused by the web; put back to 0", t_tx_shift_rules, 75),
        Test("armed-knob-lock", "both RX (armed, nothing keyed)", "the Sentry's A with TX armed on both: the VFO knob "
             "and the MFK leave the dial; locked again, the VFO knob tunes", t_armed_knob_lock, 40),
    ]
