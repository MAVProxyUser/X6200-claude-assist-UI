#!/usr/bin/env python3
"""The coverage report's gaps (orch.py coverage, 2026-11-28): tests for features whose latest result failed for the
test's own reasons, or that no test covered, on both radios at once, receive only.

On the Air (dialog_swl.c, APP > Modes, action 133) had only the suite's old check (suite_cover_c.py, not in the
repository): on 2026-11-23 it moved the MFK past the entries it counted on air by their hours on 9610 kHz, two, where the
list had more rows there (its list takes the days too, and EiBi has a row a target), stayed on 9610 kHz and failed. Here
the steps are counted from every row the radio's list file has on that frequency, which no selection can stay inside.

What the list must hold is worked out from the radio's own /mnt/broadcast.csv and dialog_swl.c's rules (category(),
mode_of()), never copied from the screen.

orch.py imports this module and sets H to itself; tests() gives the Test rows and COVERS the features each covers.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import os
import re
import time

import bench
from bench import Refused
import orch_cov_apps as ap

H = None                        # the running orch module (orch.py: orch_cov_gaps.H = sys.modules[__name__])


# ---- On the Air (133): the list, Tune, the MFK's press, Tune as you go, Range ----

ONAIR, ONAIR_NAME = 133, "On the Air"
ONAIR_START = 9600000           # 31 m: broadcasters on the air at any hour
NEAR_HZ = 250000                # dialog_swl.c NEAR_HZ: Range Near is the dial this far either side
AM, USB, CW = 6, 2, 4
# dialog_swl.c category(): utility names, the numbers stations' markers
_UTIL = ("navy", "naval", "air force", "usaf", "volmet", "coast guard", "military", "hfgcs", "meteo", "weather",
         "noaa", "marine", "maritime", "aero", "ldoc", "hfdl", "stanag", "fax", "army", "embassy", "diplomatic")
_MARKER = ("numbers", "spy ", "the buzzer", "squeaky wheel", "the pip")


def _category(station, lang):
    """dialog_swl.c category(): 'num' (E11, S28, V07: a capital, one to three digits, perhaps a letter, a space; or a
    channel marker's name), 'util' (a '-' language: -CW, -TY, -HF..., or a utility's name), else 'bc'"""
    n = station
    if len(n) > 1 and "A" <= n[0] <= "Z" and n[1].isdigit():
        k = 2
        while k < 4 and k < len(n) and n[k].isdigit():
            k += 1
        if k + 1 < len(n) and n[k].isalpha() and n[k + 1] == " ":
            k += 1
        if k >= len(n) or n[k] == " ":
            return "num"
    low = n.lower()
    if any(w in low for w in _MARKER):
        return "num"
    if lang.startswith("-") or any(w in low for w in _UTIL):
        return "util"
    return "bc"


def _mode_of(station, lang):
    """dialog_swl.c mode_of(): a broadcaster in AM, Morse (-CW) in CW, the rest in USB"""
    return AM if _category(station, lang) == "bc" else CW if lang == "-CW" else USB


def _entries_at(r, hz):
    """[(station, language)] of every row of the radio's /mnt/broadcast.csv on hz, whatever its hours"""
    out = ap.ssh(r, f"awk -F';' '$1+0=={hz / 1000:g}' /mnt/broadcast.csv", 20)
    rows = []
    for ln in out.splitlines():
        p = ln.split(";")
        if len(p) > 5:
            rows.append((p[4].strip(), p[5].strip()))
    return rows


def _settled(r, key="freq", quiet=0.8, limit=6.0):
    """state[key] once it has not changed for quiet s (a tune sets the band first, then its mode)"""
    end = time.time() + limit
    last, since = r.state.get(key), time.time()
    while time.time() < end:
        v = r.state.get(key)
        if v != last:
            last, since = v, time.time()
        elif time.time() - since >= quiet:
            break
        time.sleep(0.05)
    v = r.state.get(key)
    return int(v) if isinstance(v, (int, float)) else v


def _tuned(r, what):
    """(freq, mode) settled after a tune, and the list file's rows there in that mode (Refused when none)"""
    f = _settled(r)
    m = _settled(r, "mode", quiet=0.4, limit=3.0)
    rows = _entries_at(r, f)
    match = [x for x in rows if _mode_of(*x) == m]
    if not match:
        raise Refused(f"{r.name}: {what} tuned {f / 1e3:g} kHz mode {m}, and the list file has no entry there in "
                      f"that mode ({len(rows)} rows there: {sorted({_mode_of(*x) for x in rows})})")
    return f, m, match, rows


def _fk(r):
    """F1-F5's texts on one line each"""
    return [ap.flat(t) for t in ap.fkeys(r)[0]]


def _onair_one(o, n, r):
    """One radio's On the Air walk (its window open, the dial on 9.6 MHz AM): {what it found}"""
    names, out = (ONAIR_NAME,), {}
    k = _fk(r)
    out["found"] = {"show": k[0], "range": k[1], "follow": k[2]}
    for _ in range(4):                  # Show: All (F1 steps All > Broadcast > Utility > Numbers)
        if _fk(r)[0].endswith("All"):
            break
        ap.press(r, "F1", names)
    if "Near" in _fk(r)[1]:
        ap.press(r, "F2", names)
    if _fk(r)[2].endswith("On"):
        ap.press(r, "F3", names)
    k = _fk(r)
    if not (k[0] == "Show: All" and k[1] == "Range: All" and k[2] == "Tune as you go: Off" and k[3] == "Tune"):
        raise Refused(f"{n}: On the Air's buttons {k}, not Show All, Range All, Tune as you go Off, Tune")

    # F4 Tune: the chosen row (the one nearest the dial) tuned in its mode
    ap.press(r, "F4", names)
    f0, m0, match0, rows0 = _tuned(r, "F4 Tune")
    out["tune"] = f"{f0 / 1e3:g} kHz mode {m0}: {match0[0][0]} ({len(rows0)} rows there)"

    # the MFK past every row on f0 (the list has a row an entry; no selection stays inside more turns than that):
    # nothing tuned while it turns (Tune as you go off), then its press tunes the row it stopped on, further up
    steps = min(len(rows0) + 1, 60)
    ap.turn(r, "mfk", steps, names, rest=1.0)
    still = _settled(r, quiet=0.6, limit=2.0)
    if still != f0:
        raise Refused(f"{n}: the MFK alone moved the dial to {still} with Tune as you go off (from {f0})")
    ap.press(r, "MFK", names)
    f1, m1, match1, rows1 = _tuned(r, "the MFK's press")
    if f1 <= f0:
        raise Refused(f"{n}: {steps} rows on from {f0 / 1e3:g} kHz the MFK's press tuned {f1 / 1e3:g} kHz: the list "
                      f"is not in frequency order, or the press did not tune")
    out["mfk"] = f"{steps} on, nothing tuned till the press: {f1 / 1e3:g} kHz mode {m1} ({match1[0][0]})"

    # F3 Tune as you go: the MFK alone tunes, a moment after it rests
    ap.press(r, "F3", names)
    if not _fk(r)[2].endswith("On"):
        raise Refused(f"{n}: F3 reads {_fk(r)[2]!r}, not Tune as you go: On")
    steps2 = min(len(rows1) + 1, 60)
    ap.turn(r, "mfk", steps2, names, rest=1.2)
    f2, m2, match2, _ = _tuned(r, "Tune as you go")
    if f2 <= f1:
        raise Refused(f"{n}: Tune as you go on, {steps2} rows on from {f1 / 1e3:g} kHz the dial is on "
                      f"{f2 / 1e3:g} kHz")
    out["follow"] = f"{steps2} on: {f2 / 1e3:g} kHz mode {m2} ({match2[0][0]}) with no press"
    ap.press(r, "F3", names)
    if not _fk(r)[2].endswith("Off"):
        raise Refused(f"{n}: F3 reads {_fk(r)[2]!r} after its second press, not Off")

    # F2 Range Near the dial: the list's two ends within 250 kHz of 9.6 MHz
    r.send(f"set freq {ONAIR_START}")
    r.wait("the dial on 9.6 MHz", lambda: int(r.state.get("freq", 0)) == ONAIR_START, 5)
    ap.press(r, "F2", names)
    if _fk(r)[1] != "Range: Near dial":
        raise Refused(f"{n}: F2 reads {_fk(r)[1]!r}, not Range: Near dial")
    ends = []
    for turns in ((-50,) * 4, (50,) * 8):
        for s in turns:
            ap.turn(r, "mfk", s, names, rest=0.3)
        ap.press(r, "F4", names)
        ends.append(_tuned(r, "Range Near's end")[0])
    lo, hi = ONAIR_START - NEAR_HZ, ONAIR_START + NEAR_HZ
    if not (lo <= ends[0] < ends[1] <= hi):
        raise Refused(f"{n}: Range Near the dial's ends {[x / 1e3 for x in ends]} kHz, not inside "
                      f"{lo / 1e3:g}-{hi / 1e3:g} kHz (low first)")
    out["near"] = f"ends {ends[0] / 1e3:g} and {ends[1] / 1e3:g} kHz"
    ap.press(r, "F2", names)
    if _fk(r)[1] != "Range: All":
        raise Refused(f"{n}: F2 reads {_fk(r)[1]!r} after its second press, not Range: All")
    return out


def t_onair(o):
    """On the Air on both: the head read on the screen; F4 Tune tunes the row nearest the dial in its kind's mode (the
    list file's entry there: a broadcaster AM, Morse CW, else USB); the MFK turned past every row on that frequency
    tunes nothing till its press, which tunes a row further up (frequency order); F3 Tune as you go: the MFK alone
    tunes; F2 Range Near the dial: the list's ends within 250 kHz of 9.6 MHz. The buttons read back, each put back as
    found. Both end on page A on the last frequency"""
    res = {}
    try:
        with o.step("both by hand (a VFO click on page A: the Sentry moves no dial), then on 9.600 MHz AM") as s:
            ap.page_both(o, "A")

            def by_hand(n, r):
                r.send("knob main 1")
                time.sleep(1.0)
                return r.state.get("freq")
            H.in_parallel(o, by_hand)
            o.both({"freq": ONAIR_START, "mode": AM})
            s.confirm(f"{bench._value(o.r[H.A], 'freq')} mode {bench._value(o.r[H.A], 'mode')}")
        with o.step("On the Air open on both (open 133); its head read on the screen") as s:
            res["open"] = ap.open_both(o, ONAIR, ONAIR_NAME, f=ONAIR_START, mode=AM)
            time.sleep(1.0)
            heads = {}
            for n, r in o.r.items():
                t = ap.ocr(ap.shot(o, r, "onair list"), box=(0, 110, 800, 160), psm=7)   # its head: DIALOG_Y 118 + 6
                heads[n] = " ".join(t.split())[:160]
            res["head"] = heads
            bad = [n for n, t in heads.items() if not ap.ocr_has(t, "ON THE AIR", "UTC", "STATION")]
            if bad:
                raise Refused(f"no 'ON THE AIR .. UTC .. stations' head read on {bad}: {heads}")
            s.confirm(str(heads))
        with o.step("each radio walks its list: F4, the MFK and its press, Tune as you go, Range Near") as s:
            with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                             "onair: each radio tunes the rows of its own list (the same file, at its own pace)"):
                res["walk"] = ap.each(o, lambda n, r: _onair_one(o, n, r), wait=240)
            s.confirm(str(res["walk"]))
        w = res["walk"][H.A]
        return True, (f"On the Air on both: {res['head'][H.A]!r}; F4 {w['tune']}; MFK {w['mfk']}; Tune as you go "
                      f"{w['follow']}; Range Near {w['near']}"), res
    except Refused as e:
        return False, str(e), res
    finally:
        ap.home_both(o)


# ---- The Modes menu (114): its name in the remote's win, its items, the MFK opens one, Modes again closes it ----

MODES = 114
MODES_ITEMS = ("RTTY", "JTTY", "PSK31", "PSK63", "FT8", "FT4", "FT2", "WSPR", "Olivia", "JS8", "SSTV", "WeFax",
               "NavTex", "RADE")          # dialog_modes.c modes[], in its order
MODES_PICK = 1                  # JTTY: the second item, a receive-only text decoder (WSPR and the rest left alone)


# Each tile's name read in turn these ways till one has it: (top, bottom, side margin in the tile, tesseract psm,
# scale). Tesseract reads the sony_22 names on the tiles erratically, each way missing others (2026-11-28 pictures:
# scale 2 missed SSTV alone, a narrower box at 3 JS8, SSTV and WeFax, the whole box at 4 FT8, JS8, WeFax, NavTex)
TILE_READS = ((44, 68, 10, 7, 2), (46, 66, 25, 7, 3), (36, 70, 2, 7, 4))


def _tile_read(path, i, word):
    """Item i's name read on its tile in the menu's icon grid (dialog_modes.c: BOX_W 796 centred, BOX_Y 70, HEAD_H 38,
    T_PAD 6, T_GAP 4, five tiles of 153 x 86 a row, the name 22 px over each tile's bottom, the popup's 2 px border):
    word found by one of TILE_READS"""
    x, y = 10 + (i % 5) * 157, 110 + (i // 5) * 90
    for top, bot, side, psm, scale in TILE_READS:
        if ap.squash(word) in ap.squash(ap.ocr(path, box=(x + side, y + top, x + 153 - side, y + bot), psm=psm,
                                               scale=scale)):
            return True
    return False


def t_modes_menu(o):
    """The Modes menu on both: 'open 114' shows it, the remote's win 'Modes' (it printed '(null)' till 2026-11-28: the
    menu has no app name); its items read on the screen; the MFK to the first item and one on (JTTY), its press opens
    JTTY; closed, the menu again (the last used marked) and 'open 114' a second time closes it"""
    res = {}
    names = ("Modes",)
    try:
        with o.step("both on page A; 'open 114': the Modes menu on both, win 'Modes'") as s:
            ap.page_both(o, "A")
            res["open"] = ap.open_both(o, MODES, "Modes", same=False)
            s.confirm(str(res["open"]))
        with o.step("its items read on both screens") as s:
            seen, missed = {}, {}
            for n, r in o.r.items():
                p = ap.shot(o, r, "modes menu")
                whole = ap.squash(ap.ocr(p))
                # each tile's name read alone too: across the whole grid the icons put tesseract off (FT8, JS8,
                # SSTV, WeFax and NavTex missed, 2026-11-28); the list layout (tiles off) is read whole
                seen[n] = [m for i, m in enumerate(MODES_ITEMS) if ap.squash(m) in whole or _tile_read(p, i, m)]
                missed[n] = [m for m in MODES_ITEMS if m not in seen[n]]
            res["items"], res["missed"] = seen, missed
            few = {n: v for n, v in missed.items() if len(v) > 2}
            if few:
                raise Refused(f"more than two of the {len(MODES_ITEMS)} items not read: missed {few}")
            s.confirm(", ".join(f"{n}: {len(v)} of {len(MODES_ITEMS)} (missed {missed[n]})" for n, v in seen.items()))
        with o.step(f"the MFK to the first item and {MODES_PICK} on, its press: {MODES_ITEMS[MODES_PICK]} opens") as s:
            def pick(n, r):
                ap.turn(r, "mfk", -len(MODES_ITEMS), names, rest=0.6)
                ap.turn(r, "mfk", MODES_PICK, names, rest=0.6)
                ap.press(r, "MFK", names, rest=0.8)
                r.wait(f"the {MODES_ITEMS[MODES_PICK]} window", lambda: ap.win(r) == MODES_ITEMS[MODES_PICK], 8)
                return ap.win(r)
            res["pick"] = ap.each(o, pick)
            s.confirm(str(res["pick"]))
        with o.step(f"{MODES_ITEMS[MODES_PICK]} closed; 'open 114' twice: the menu, then closed (the Sentry back)") as s:
            def again(n, r):
                bench.close_apps(r)
                r.take_control()
                r.send(f"open {MODES}")
                r.wait("the Modes menu", lambda: ap.win(r) == "Modes", 8)
                time.sleep(0.8)
                r.send(f"open {MODES}")
                r.wait("the menu closed", lambda: ap.win(r) in ("", "Sentry"), 8)
                return ap.win(r) or "''"
            res["again"] = ap.each(o, again)
            s.confirm(str(res["again"]))
        return True, (f"the Modes menu on both: win 'Modes', {len(res['items'][H.A])} items read, the MFK's press "
                      f"opened {MODES_ITEMS[MODES_PICK]}, 'open 114' again closed it"), res
    except Refused as e:
        return False, str(e), res
    finally:
        ap.home_both(o)


# ---- Time Signal (132) in AM: the filter passes 100 Hz, the time code's ----

TIMESIG = 132


def t_timesig_am_filter(o):
    """Time Signal on both: AM (its WWV/WWVH dial), the receiver filter's low edge (the remote's flo) at 100 Hz or below:
    the 100 Hz time code reaches the decoder (the manual: the X6200 by default; the X6100's filter too)"""
    res = {}
    try:
        with o.step("both on page A; Time Signal open on both (open 132)") as s:
            ap.page_both(o, "A")
            res["open"] = ap.open_both(o, TIMESIG, "Time Signal")
            s.confirm(str(res["open"]))
        with o.step("AM and the filter from 100 Hz or lower on both") as s:
            def edge(n, r):
                r.wait("AM", lambda: int(r.state.get("mode", -1)) == AM, 6)
                time.sleep(1.0)
                lo, hi = int(r.state.get("flo", -1)), int(r.state.get("fhi", -1))
                if lo < 0 or lo > 100:
                    raise Refused(f"{n}: Time Signal in AM with the filter {lo}-{hi} Hz: 100 Hz not passed")
                return f"{int(r.state.get('freq', 0)) / 1e6:g} MHz AM, filter {lo}-{hi} Hz"
            res["edge"] = ap.each(o, edge)
            s.confirm(str(res["edge"]))
        return True, f"Time Signal in AM passes 100 Hz on both: {res['edge']}", res
    except Refused as e:
        return False, str(e), res
    finally:
        ap.home_both(o)


# ---- D BANDS' LISTEN on the SDR: the stick's gain by app_sentry.c rx_setup_on()'s rule ----

SENTRY_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "app_sentry.c")


def _on_h(r):
    """AIRSPACE (the Sentry's H) on the screen: its own window, win 'Airspace' (app_sentry_page() names A-G and S only)"""
    return r.state.get("win") == "Airspace" or r.state.get("pg") == "H"


def _bands_n():
    """D's list length: app_sentry.c bands[]'s rows (D wraps round them: band_sel + dir modulo BANDS_N)"""
    src = open(SENTRY_SRC).read()
    body = src[src.index("static const band_t bands[] = {"):]
    body = body[:body.index("\n};")]
    n = len(re.findall(r'^\s*\{"[^"]+",\s*\d+,\s*\d+,', body, re.M))
    if n < 20:
        raise Refused(f"app_sentry.c bands[]: {n} rows read, the list's form changed")
    return n


def _sdr_gain_rule(hz):
    """app_sentry.c rx_setup_on() for the SDR, tenths of a dB: 19.7 FM (and AM) broadcast, 49.6 the weather satellites,
    the sondes and 960 MHz up, 37.2 the airband, marine and NOAA, 28.0 the rest"""
    k = hz // 1000
    if 530 <= k <= 1700 or 87500 <= k <= 108000:
        return 197
    if 137000 <= k <= 138000 or 400000 <= k <= 406000 or k >= 960000:
        return 496
    if 118000 <= k < 137000 or 156000 <= k <= 162550:
        return 372
    return 280


def _d_gain_walk(o, n, r, count):
    """One radio round D's whole list, F2 LISTEN on each band: {"sdr": [(B's Hz, the gain read, the rule, the band's
    name as the state's dsel had it)], "a"/"h": how many went to the X6200 (A) or AIRSPACE (H), "d": the bands whose
    LISTEN left the radio on D}"""
    out = {"sdr": [], "a": 0, "h": 0, "d": []}
    for i in range(count):
        bench.open_page(r, "D")
        time.sleep(0.8)
        if i:
            r.take_control()
            r.send("knob mfk 1")
            time.sleep(0.8)
        name = r.state.get("dsel") or "?"
        bench.tap(r, "F2")
        try:                            # H (ADS-B) opens AIRSPACE by an async call, its receiver started: up to 15 s
            r.wait(f"D's LISTEN on {name} to its page", lambda: r.state.get("pg") in ("A", "B") or _on_h(r), 15)
        except Refused:
            out["d"].append(name)       # said at the end with the rest, the walk goes on
            continue
        time.sleep(1.5)                 # the stick tuned and its gain set (rtl_spectrum_want)
        pg = r.state.get("pg")
        if pg == "B":
            hz = int(r.state.get("bhz") or 0)
            g = int(float(ap.cfg(r, "rtl_gain")))
            out["sdr"].append((hz, g, _sdr_gain_rule(hz), name))
        elif _on_h(r):
            out["h"] += 1
        else:
            out["a"] += 1
    return out


def t_d_sdr_gain(o):
    """D BANDS on both, every band's F2 LISTEN in turn (D wraps: as many presses as bands[] has rows): where it puts B's
    SDR on the band (pg B), the stick's gain (cfg rtl_gain, tenths of a dB) is app_sentry.c's rule for B's channel
    (19.7 dB FM broadcast, 49.6 weather satellites and 960 MHz up, 37.2 airband, marine and NOAA, 28.0 the rest); at least
    three of the four seen. The bands the X6200 takes (A) and ADS-B (H, AIRSPACE's own gain) counted, not judged. What
    the speaker plays (heard) put back as found; both end on page A on the last frequency"""
    res = {}
    heard0 = {n: r.state.get("heard") for n, r in o.r.items()}
    try:
        count = _bands_n()
        with o.step(f"both round D's {count} bands, F2 LISTEN on each; B's gain read where it goes to the SDR") as s:
            with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                             "d-sdr-gain: each radio listens round D's list at its own pace"):
                res["walk"] = ap.each(o, lambda n, r: _d_gain_walk(o, n, r, count), wait=count * 12)
            s.confirm(", ".join(f"{n}: {len(w['sdr'])} on the SDR, {w['a']} on A, {w['h']} on H"
                                for n, w in res["walk"].items()))
        with o.step("the gain on every SDR band as the rule says, three of its four values seen") as s:
            bad, seen = [], {}
            for n, w in res["walk"].items():
                for hz, g, want, name in w["sdr"]:
                    seen.setdefault(n, set()).add(want)
                    if g != want:
                        bad.append(f"{n} {name} ({hz / 1e6:.4f} MHz) gain {g / 10:g} dB, the rule {want / 10:g}")
            stayed = {n: w["d"] for n, w in res["walk"].items() if w["d"]}
            if stayed:                  # D's F2 LISTEN must leave D for A, B or H (d-f2-listen)
                bad.append(f"F2 LISTEN left these on D past 15 s: {stayed}")
            if bad:
                raise Refused("; ".join(bad))
            few = {n: sorted(v) for n, v in seen.items() if len(v) < 3}
            if len(seen) < 2 or few:
                raise Refused(f"too few kinds of SDR band reached to judge the rule: {few or seen}")
            s.confirm(", ".join(f"{n}: {sorted(x / 10 for x in v)} dB" for n, v in seen.items()))
        w = res["walk"][H.A]
        return True, (f"D's LISTEN on the SDR sets the rule's gain on both: {len(w['sdr'])} bands each, "
                      f"{sorted({x[1] / 10 for x in w['sdr']})} dB"), res
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for n, r in o.r.items():
                if heard0.get(n) in (0, 1, 2, 3) and r.state.get("heard") != heard0[n]:
                    r.send(f"set heard {heard0[n]}")
        finally:
            o.changing -= 1
        ap.home_both(o)


# ---- E DECODE's OPEN for the SDR's types: B on the type's channel at the rule's gain, ACARS and ADS-B to H ----

E_ON_B = {"NOAA ALERTS": "f_same", "FM RDS": "f_rds", "DIGITAL VOICE": "f_dv", "METEOR": "f_meteor"}
E_ON_H = {"ACARS": "f_acars", "ADS-B": "f_adsb"}


def _first_hz(array):
    """The first channel of app_sentry.c's frequency list array (E's type opens its row 0)"""
    m = re.search(r"static const int32_t %s\[\] = \{(\d+)" % array, open(SENTRY_SRC).read())
    if not m:
        raise Refused(f"app_sentry.c: no {array}[]")
    return int(m.group(1))


def _decoders_n():
    """E's types: app_sentry.c decoders[]'s rows (F3/F4 step them round)"""
    src = open(SENTRY_SRC).read()
    body = src[src.index("static const decoder_t decoders[] = {"):]
    body = body[:body.index("\n};")]
    return len(re.findall(r'^\s*\{"[A-Z0-9 -]+", "', body, re.M))


def _e_choose(r, name, n):
    """E shown with type name chosen: F4 (the next type) till the state's esel says it, at most a round"""
    bench.open_page(r, "E")
    r.wait("E's type in the state", lambda: bool(r.state.get("esel")), 6)
    for _ in range(n + 1):
        if r.state.get("esel") == name:
            return
        bench.tap(r, "F4")
        time.sleep(0.5)
    raise Refused(f"{r.name}: E's F4 went round {n + 1} times and never reached {name} (esel {r.state.get('esel')!r})")


def _e_sdr_walk(o, n, r, count):
    """One radio: E's six SDR types opened in turn: {type: what it did}"""
    out = {}
    for name, arr in list(E_ON_B.items()) + list(E_ON_H.items()):
        _e_choose(r, name, count)
        hz = _first_hz(arr)
        bench.tap(r, "F2")
        if name in E_ON_B:
            r.wait(f"{name}: B on {hz / 1e6:g} MHz", lambda: r.state.get("pg") == "B" and
                   int(r.state.get("bhz") or 0) == hz, 8)
            time.sleep(1.0)
            g = int(float(ap.cfg(r, "rtl_gain")))
            if g != _sdr_gain_rule(hz):
                raise Refused(f"{n}: E's {name} put B on {hz / 1e6:g} MHz with the gain {g / 10:g} dB, the rule "
                              f"{_sdr_gain_rule(hz) / 10:g}")
            out[name] = f"B {hz / 1e6:g} MHz {r.state.get('bmode')} gain {g / 10:g} dB heard {r.state.get('heard')}"
        else:
            r.wait(f"{name}: AIRSPACE (H)", lambda: _on_h(r), 15)
            time.sleep(1.0)
            if name == "ACARS":         # the airband's gain set on B's SDR before H opens on its ACARS view
                g = int(float(ap.cfg(r, "rtl_gain")))
                if g != _sdr_gain_rule(hz):
                    raise Refused(f"{n}: E's ACARS opened H with the SDR's gain {g / 10:g} dB, the airband's "
                                  f"{_sdr_gain_rule(hz) / 10:g}")
                out[name] = f"H, gain {g / 10:g} dB"
            else:
                out[name] = "H"
    return out


def t_e_sdr_open(o):
    """E DECODE on both, its six types for the SDR each chosen (F4 till the state's esel names it) and opened (F2): NOAA
    ALERTS, FM RDS, DIGITAL VOICE and METEOR put B on the type's first channel (app_sentry.c's lists) at the rule's gain;
    ACARS opens H (AIRSPACE's ACARS view) with the airband's gain on the SDR; ADS-B opens H. What the speaker plays put
    back as found; both end on page A on the last frequency"""
    res = {}
    heard0 = {n: r.state.get("heard") for n, r in o.r.items()}
    try:
        count = _decoders_n()
        with o.step(f"E's SDR types opened in turn on both ({', '.join(list(E_ON_B) + list(E_ON_H))})") as s:
            with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                             "e-sdr-open: each radio opens E's types at its own pace"):
                res["open"] = ap.each(o, lambda n, r: _e_sdr_walk(o, n, r, count), wait=240)
            s.confirm(str(res["open"]))
        return True, f"E's SDR types open where they should on both: {res['open'][H.A]}", res
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for n, r in o.r.items():
                if heard0.get(n) in (0, 1, 2, 3) and r.state.get("heard") != heard0[n]:
                    r.send(f"set heard {heard0[n]}")
        finally:
            o.changing -= 1
        ap.home_both(o)


# ---- G WALKIE's SDR groups (an RTL-SDR on HOST): the amateur FM groups and DMR, received only ----

def _g_sdr_plan():
    """app_sentry.c w_sdr_plan[]: {Hz: [its groups]} (446.500 is 70 CM FM's S2 and DMR's S2)"""
    src = open(SENTRY_SRC).read()
    body = src[src.index("} w_sdr_plan[] = {"):]
    body = body[:body.index("\n};")]
    plan = {}
    for grp, hz in re.findall(r'\{"([^"]+)",\s*"[^"]*",\s*(\d+)\}', body):
        plan.setdefault(int(hz), []).append(grp)
    if len({g for v in plan.values() for g in v}) < 5:
        raise Refused(f"app_sentry.c w_sdr_plan[]: its groups not read ({plan})")
    return plan


def _g_sdr_walk(o, n, r, plan):
    """One radio on G, the MFK through its groups (they wrap) till every SDR group of the plan was reached: {group: (its
    channel, the state's gno there)}; then A/B on 2 M's calling channel arms nothing"""
    groups = {g for v in plan.values() for g in v}
    seen = {}
    bench.open_page(r, "G")
    r.wait("page G", lambda: r.state.get("pg") == "G", 6)
    time.sleep(1.0)
    for _ in range(60):
        hz = int(r.state.get("ghz") or 0)
        for g in plan.get(hz, ()):
            seen.setdefault(g, (hz, r.state.get("gno") or ""))
        if groups <= set(seen):
            break
        r.take_control()
        r.send("knob mfk 1")
        time.sleep(0.8)
    missing = sorted(groups - set(seen))
    if missing:
        raise Refused(f"{n}: G's MFK went round 60 groups and never reached {missing} (reached {sorted(seen)})")
    bad = {g: v for g, v in seen.items() if not v[1].endswith("ONLY RECEIVES")}
    if bad:
        raise Refused(f"{n}: G on the RTL-SDR's channels not refused as receive only: {bad}")
    r.send("set gsdr 146520000")
    r.wait("G on 2 M's calling channel", lambda: int(r.state.get("ghz") or 0) == 146520000, 8)
    bench.tap(r, "AB")
    time.sleep(1.5)
    if r.state.get("garm") or r.state.get("tx") or r.state.get("rxo") != 1:
        raise Refused(f"{n}: A/B on 146.520 armed something: garm {r.state.get('garm')}, tx {r.state.get('tx')}, rxo "
                      f"{r.state.get('rxo')}")
    return {g: f"{hz / 1e6:g} MHz" for g, (hz, _) in sorted(seen.items())}


def t_g_sdr_groups(o):
    """G WALKIE on both with an RTL-SDR on HOST: the MFK's groups (they wrap) include each of app_sentry.c's SDR groups
    (2 M, 1.25 M, 70 CM, 33 CM, 23 CM FM and DMR), on each the state's gno 'THE RTL-SDR ONLY RECEIVES'; A/B on 2 M's
    calling channel arms nothing (garm 0, tx 0, rxo 1). G back on the radio's own channel as found (set gsdr 0, set
    wch); both end on page A on the last frequency"""
    res = {}
    wch0 = {n: (r.state.get("wch"), r.state.get("wchm")) for n, r in o.r.items()}
    try:
        plan = _g_sdr_plan()
        with o.step("an SDR on HOST on both (the state's sdr)") as s:
            ap.page_both(o, "A")
            none = [n for n, r in o.r.items() if not r.state.get("sdr")]
            if none:
                raise Refused(f"no SDR on HOST on {none}: G has no SDR groups there")
            s.confirm("sdr 1 on both")
        with o.step("G on both, the MFK through the groups: every SDR group reached, receive only; A/B arms nothing") as s:
            with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                             "g-sdr-groups: each radio's G steps its groups at its own pace"):
                res["walk"] = ap.each(o, lambda n, r: _g_sdr_walk(o, n, r, plan), wait=120)
            s.confirm(str(res["walk"]))
        return True, f"G's SDR groups on both, receive only on the RTL-SDR: {res['walk'][H.A]}", res
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for n, r in o.r.items():
                if r.state.get("garm"):
                    bench.tap(r, "AB")
                if r.state.get("pg") == "G" and int(r.state.get("ghz") or 0):
                    r.send("set gsdr 0")
                    try:
                        r.wait("G on the radio", lambda: int(r.state.get("ghz") or 0) == 0, 8)
                    except Refused as e:
                        o.st.ev(f"{n}: {e}", "bad")
                hz, m = wch0[n]
                if hz and (r.state.get("wch"), r.state.get("wchm")) != (hz, m):
                    r.send(f"set wch {hz} {m}")
        finally:
            o.changing -= 1
        ap.home_both(o)


# ---- Tuning a page puts its own radio on the speaker (A the X6200, B the SDR) ----

HEARD_X6200, HEARD_SDR = 0, 1           # the remote's heard: 0 the radio's own receiver, 1 the SDR at B's channel


def _tune_heard_one(o, n, r, page, setting, before, after):
    """On page, its LISTEN (setting: sentry_hear_a A's, sentry_hear_f B's; 0 the X6200, 1 the SDR) set to before and the
    speaker with it, then a VFO click: LISTEN and the speaker (heard) both after (app_sentry.c a_tuned, b_tuned)"""
    r.take_control()
    with ap.cfg_kept(r, {setting: before}):
        r.send(f"set heard {before}")
        r.wait(f"heard {before}", lambda: r.state.get("heard") == before, 5)
        time.sleep(0.8)
        r.send("knob main 1")
        r.wait(f"heard {after} after the VFO on {page}", lambda: r.state.get("heard") == after, 5)
        got = ap.wait_cfg(r, f"{page}'s LISTEN {after}", lambda v: str(v) == str(after), setting, 4)
    return f"{page}: LISTEN {before}, a VFO click: LISTEN {got}, heard {r.state.get('heard')}"


def t_tune_heard(o):
    """Tuning a screen puts its own radio on the speaker, on both (an RTL-SDR on HOST each): on A with A's LISTEN the
    SDR (sentry_hear_a 1) a VFO click makes it the X6200 and the speaker with it (heard 0); on B with B's LISTEN the
    X6200 (sentry_hear_f 0) a VFO click makes it the SDR (heard 1). LISTEN set through the remote's cfg (set heard
    alone changes only what plays now: the rule reads LISTEN), each put back as found, and what was heard; both end on
    page A on the last frequency"""
    res = {}
    heard0 = {n: r.state.get("heard") for n, r in o.r.items()}
    try:
        with o.step("an SDR on HOST on both (the state's sdr)") as s:
            ap.page_both(o, "A")
            none = [n for n, r in o.r.items() if not r.state.get("sdr")]
            if none:
                raise Refused(f"no SDR on HOST on {none}")
            s.confirm("sdr 1 on both")
        with bench.apart({"freq": bench.STEPPED}, "tune-heard: each radio's VFO clicked once on A, once on B"):
            with o.step("A: the SDR heard, a VFO click: the X6200 heard") as s:
                res["A"] = ap.each(o, lambda n, r: _tune_heard_one(o, n, r, "A", "sentry_hear_a", HEARD_SDR,
                                                                    HEARD_X6200))
                s.confirm(str(res["A"]))
            with o.step("B: the X6200 heard, a VFO click: the SDR heard") as s:
                ap.page_both(o, "B")
                res["B"] = ap.each(o, lambda n, r: _tune_heard_one(o, n, r, "B", "sentry_hear_f", HEARD_X6200,
                                                                    HEARD_SDR))
                s.confirm(str(res["B"]))
        return True, "tuning A gave the speaker to the X6200, tuning B to the SDR, on both", res
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for n, r in o.r.items():
                if heard0.get(n) in (0, 1, 2, 3) and r.state.get("heard") != heard0[n]:
                    r.send(f"set heard {heard0[n]}")
        finally:
            o.changing -= 1
        ap.home_both(o)


# ---- VOL's press leaves an app and a menu open ----

def _vol_presses(o, n, r, name):
    """In window name: VOL pressed short, then held 2 s: the window still open after each"""
    for held in (False, True):
        r.take_control()
        r.send("key VOL 1")
        time.sleep(2.0 if held else 0.15)
        r.send("key VOL 0")
        time.sleep(1.5)
        if ap.win(r) != name:
            raise Refused(f"{n}: VOL {'held' if held else 'pressed'} in {name}: the screen shows {ap.win(r)!r}")
    return f"{name} open after VOL pressed and held"


def t_vol_press(o):
    """VOL's press closes no window, on both: Time Signal (an app) and the Modes menu each stay open after VOL pressed
    short and held 2 s (it chooses VOL's control: more-knob-press). Both end on page A on the last frequency"""
    res = {}
    try:
        with o.step("Time Signal open on both: VOL pressed and held, still open") as s:
            ap.page_both(o, "A")
            ap.open_both(o, TIMESIG, "Time Signal")
            res["app"] = ap.each(o, lambda n, r: _vol_presses(o, n, r, "Time Signal"))
            s.confirm(str(res["app"]))
        with o.step("the Modes menu on both: VOL pressed and held, still open") as s:
            ap.close_both(o)
            ap.page_both(o, "A")
            ap.open_both(o, MODES, "Modes", same=False)
            res["menu"] = ap.each(o, lambda n, r: _vol_presses(o, n, r, "Modes"))
            s.confirm(str(res["menu"]))
        return True, "VOL's press, short and held, left Time Signal and the Modes menu open on both", res
    except Refused as e:
        return False, str(e), res
    finally:
        ap.home_both(o)


# ---- Leaving the Sentry gives the speaker back to the X6200 ----

def _b_heard(r):
    """r on page B with B's SDR on the speaker: heard 1 and the base in play mode (play 1)"""
    bench.open_page(r, "B")
    r.wait("page B", lambda: r.state.get("pg") == "B", 6)
    r.take_control()
    r.send(f"set heard {HEARD_SDR}")
    r.wait("B's SDR on the speaker (heard 1, play 1)", lambda: r.state.get("heard") == HEARD_SDR and
           int(r.state.get("play") or 0) == 1, 6)


def _f_choose(r, name, n=20):
    """F shown with item name chosen: the MFK one at a time till the state's fsel says it"""
    bench.open_page(r, "F")
    r.wait("F's choice in the state", lambda: bool(r.state.get("fsel")), 6)
    for _ in range(n):
        if r.state.get("fsel") == name:
            return
        r.take_control()
        r.send("knob mfk 1")
        time.sleep(0.5)
    raise Refused(f"{r.name}: F's MFK went round and never chose {name} (fsel {r.state.get('fsel')!r})")


def _leave_one(o, n, r):
    out = {}
    # H AIRSPACE: the radio's own receiver heard (the SDR is ADS-B's there)
    _b_heard(r)
    r.send("open sentry H")
    r.wait("H, the speaker the X6200's (play 0)", lambda: _on_h(r) and int(r.state.get("play") or 0) == 0, 15)
    out["H"] = f"play {r.state.get('play')}"
    # an app opened from B
    _b_heard(r)
    r.send(f"open {TIMESIG}")
    r.wait("Time Signal, the speaker the X6200's (play 0)", lambda: ap.win(r) == "Time Signal" and
           int(r.state.get("play") or 0) == 0, 8)
    out["app"] = f"play {r.state.get('play')}"
    bench.close_apps(r)
    # F SYSTEM's CLASSIC RADIO: the Sentry closed, the classic screen
    _b_heard(r)
    _f_choose(r, "CLASSIC RADIO")
    bench.tap(r, "F2")
    r.wait("the classic screen, the speaker the X6200's (play 0)", lambda: not r.state.get("pg") and
           int(r.state.get("play") or 0) == 0, 8)
    out["classic"] = f"play {r.state.get('play')}, win {ap.win(r)!r}"
    r.send("open sentry A")
    r.wait("the Sentry back on A", lambda: r.state.get("pg") == "A", 8)
    return out


def t_leave_speaker(o):
    """Leaving the Sentry gives the speaker back to the X6200, on both (an RTL-SDR on HOST each): from B with B's SDR
    heard (heard 1, the base in play mode), H AIRSPACE, an app (Time Signal) and F SYSTEM's CLASSIC RADIO each take
    play mode off (play 0). What was heard put back as found; both end on page A on the last frequency"""
    res = {}
    heard0 = {n: r.state.get("heard") for n, r in o.r.items()}
    try:
        with o.step("an SDR on HOST on both (the state's sdr)") as s:
            ap.page_both(o, "A")
            none = [n for n, r in o.r.items() if not r.state.get("sdr")]
            if none:
                raise Refused(f"no SDR on HOST on {none}")
            s.confirm("sdr 1 on both")
        with o.step("from B heard: H, an app, CLASSIC RADIO each give the speaker to the X6200 (play 0)") as s:
            with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                             "leave-speaker: each radio leaves B three ways at its own pace"):
                res["leave"] = ap.each(o, lambda n, r: _leave_one(o, n, r), wait=120)
            s.confirm(str(res["leave"]))
        return True, f"leaving B for H, an app or the classic screen took play mode off on both: {res['leave'][H.A]}", res
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for n, r in o.r.items():
                if not r.state.get("pg"):           # left on the classic screen by a failure: the Sentry again
                    bench.close_apps(r)
                    r.send("open sentry A")
                if heard0.get(n) in (0, 1, 2, 3) and r.state.get("heard") != heard0[n]:
                    r.send(f"set heard {heard0[n]}")
        finally:
            o.changing -= 1
        ap.home_both(o)


# ---- The Scanner (127): Source CB, Start by the MFK's press, Pause, Resume, Skip; closing puts the dial back ----

SCANNER = 127
CB_LO, CB_HI = 26900000, 27500000       # the CB app's plan (US 40: 26.965-27.405 MHz) as the Scanner's CB source


def _run_label(r):
    return _fk(r)[2]                    # F3: Start / Pause / Resume (dialog_scan.c labels_refresh)


def _scanner_one(o, n, r, f0, m0):
    names = ("Scanner",)
    out = {}
    for _ in range(6):                  # Source steps Channels > Airband > HF Air > CB > Segment (empty ones passed)
        t0 = time.time()
        ap.press_label(r, r"^Source$", names)
        said = ap.wait_msg(r, t0, r"^Source: ", 4)
        if said.startswith("Source: CB"):
            out["source"] = said
            break
    else:
        raise Refused(f"{n}: Source never came to CB (last {said!r})")
    if _run_label(r) != "Start":
        raise Refused(f"{n}: F3 reads {_run_label(r)!r} before the start, not Start")
    ap.press(r, "MFK", names, rest=1.0)         # the MFK's press: Start (its click taken as ENTER, 2026-11-28)
    r.wait("Pause on F3 (scanning)", lambda: _run_label(r) == "Pause", 4)
    r.wait("the dial on a CB channel", lambda: CB_LO <= int(r.state.get("freq") or 0) <= CB_HI, 4)
    out["start"] = f"the MFK's press: scanning, on {int(r.state.get('freq')) / 1e6:.3f} MHz"
    ap.press(r, "MFK", names, rest=1.0)         # its press again: Pause
    r.wait("Resume on F3 (paused)", lambda: _run_label(r) == "Resume", 4)
    fp = int(r.state.get("freq") or 0)
    time.sleep(2.5)
    if int(r.state.get("freq") or 0) != fp:
        raise Refused(f"{n}: paused on {fp / 1e6:.3f} MHz, the dial moved on to {int(r.state.get('freq')) / 1e6:.3f}")
    out["pause"] = f"paused on {fp / 1e6:.3f} MHz, still 2.5 s later"
    ap.press_label(r, r"^Skip$", names)          # Skip from a pause: the next one
    r.wait("Skip: the next channel", lambda: int(r.state.get("freq") or 0) != fp, 4)
    fs = int(r.state.get("freq") or 0)
    if not CB_LO <= fs <= CB_HI:
        raise Refused(f"{n}: Skip went to {fs / 1e6:.3f} MHz, not a CB channel")
    out["skip"] = f"{fp / 1e6:.3f} -> {fs / 1e6:.3f} MHz, {_run_label(r)} on F3"
    r.send("close")                             # ESC: the Scanner closed, the dial and mode as before it
    r.wait("the Scanner closed", lambda: ap.win(r) != "Scanner", 6)
    r.wait(f"back on {f0} mode {m0}", lambda: (int(r.state.get("freq") or 0), int(r.state.get("mode", -1)))
           == (f0, m0), 6)
    out["close"] = f"closed, back on {f0 / 1e6:.4f} MHz mode {m0}"
    return out


def t_scanner(o):
    """The Scanner on both, receive only: Source stepped to CB (the message box says it); the MFK's press starts it (F3
    Pause, the dial on CB channels), its press again pauses (F3 Resume, the dial still), Skip goes on to the next CB
    channel; closing puts each radio back on the dial and mode it had (14.200 USB). Both end on page A on the last
    frequency"""
    res = {}
    try:
        with o.step("both by hand on page A, on 14.200 MHz USB") as s:
            ap.page_both(o, "A")
            H.in_parallel(o, lambda n, r: (r.send("knob main 1"), time.sleep(1.0)))
            o.both({"freq": 14200000, "mode": USB})
            s.confirm("14.200 USB on both")
        with o.step("the Scanner open on both (open 127)") as s:
            res["open"] = ap.open_both(o, SCANNER, "Scanner", same=False)
            s.confirm(str(res["open"]))
        with o.step("each: Source CB, the MFK starts and pauses it, Skip, closed back on 14.200 USB") as s:
            with bench.apart({"freq": bench.STEPPED, "mode": bench.STEPPED},
                             "scanner: each radio scans the CB channels at its own pace"):
                res["scan"] = ap.each(o, lambda n, r: _scanner_one(o, n, r, 14200000, USB), wait=90)
            s.confirm(str(res["scan"]))
        return True, f"the Scanner on both: {res['scan'][H.A]}", res
    except Refused as e:
        return False, str(e), res
    finally:
        ap.home_both(o)


# ---- Winlink refuses to key with RIT or XIT set (simplex only) ----

WL_REFUSALS = (("rit", 500, "RIT is set (+500 Hz)"), ("xit", 100, "XIT is set (+100 Hz)"))   # winlink.c tx_refused


def t_winlink_no_tx(o, s, l):
    """Pat on s calls l over ARDOP on 14.105 (APP > Mail's path, orch_cable.PatSession) with RIT +500 Hz set on s, then
    with XIT +100 Hz: s never keys (no over in its tx history from the Connect on) and says 'Winlink: no TX, RIT is set
    (+500 Hz)' / 'XIT is set (+100 Hz)' (winlink.c: Winlink is simplex). RIT and XIT put back to 0; Winlink as found;
    both end on page A on the last frequency"""
    import orch_cable as oc
    S = o.r[s]
    call = H.TEST_CALLS[l]
    res = {}
    try:
        oc.ensure_alike(o, oc.WL_DIAL, 3)
        oc.classic(S)       # Winlink keys from the classic screen (the Sentry holds the radio receive only)
        with oc.winlink_on(o, "ardop"):
            with oc.session(o, "winlink-no-tx", [s], oc.WL_DIAL):
                oc.role(o, s)
                for key, val, word in WL_REFUSALS:
                    with o.step(f"{s} with {key.upper()} {val:+d} Hz: Pat's call refused, nothing keyed") as st:
                        with bench.apart({key: bench.STEPPED}, f"winlink-no-tx: {s}'s {key.upper()} set to be refused"):
                            try:
                                n0 = len(S.errs)
                                S.send(f"set {key} {val}")
                                S.wait(f"{key} {val} or its refusal", lambda: int(S.state.get(key) or 0) == val or
                                       any(e.startswith(f"{key}:") for e in S.errs[n0:]), 5)
                                no = [e for e in S.errs[n0:] if e.startswith(f"{key}:")]
                                if no:          # the X6200's base has no XIT: nothing there to be refused
                                    res[key] = {"skipped": no[0]}
                                    st.confirm(f"skipped: {no[0]}")
                                    continue
                                t0 = time.time()
                                ps = oc.PatSession(S, f"ardop:///{call}?freq={oc.WL_DIAL // 1000}")
                                try:
                                    said = ap.wait_msg(S, t0, r"^Winlink: no TX, ", 60)
                                    time.sleep(3.0)
                                finally:
                                    ps.close()
                                keyed = oc.tx_spans(S, t0)
                            finally:
                                S.send(f"set {key} 0")
                                S.wait(f"{key} 0", lambda: int(S.state.get(key) or 0) == 0, 5)
                        if key in res:
                            continue
                        res[key] = {"said": said, "keyed": keyed}
                        if keyed:
                            raise Refused(f"{s} keyed with {key.upper()} set: {keyed}")
                        if word not in said:
                            raise Refused(f"{s} said {said!r}, not the {key.upper()} reason ({word!r})")
                        st.confirm(f"{said!r}, nothing keyed")
        return True, (f"{s}'s Winlink refused, nothing keyed: " + "; ".join(
            f"{k.upper()} {v['said']!r}" if "said" in v else f"{k.upper()} {v['skipped']}" for k, v in res.items())), res
    except Refused as e:
        return False, str(e), res
    finally:
        oc.home(o, S)


COVERS = {
    "onair": (["onair-list", "onair-tune", "onair-tune-as-you-go", "onair-range"], []),
    "modes-menu": (["modes-menu"], []),
    "timesig-am-filter": (["timesig-am-filter-100hz"], []),
    "d-sdr-gain": (["d-e-listen-sets-sdr-gain"], []),
    "e-sdr-open": (["e-open-sdr-types"], ["d-e-listen-sets-sdr-gain"]),
    "g-sdr-groups": (["g-sdr-groups"], []),
    "tune-heard": (["sentry-tune-own-radio-heard"], []),
    "vol-press": ([], ["input-vol-press"]),
    "leave-speaker": (["sentry-leave-speaker-x6200"], []),
    "scanner-keys": (["scanner-open-close-restore"], ["scanner-start-pause-skip", "scanner-sources"]),
    "winlink-no-tx": ([], ["tx-shift-winlink-refused", "winlink-no-tx-reasons"]),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("onair", "both RX", "On the Air on both: the head read, F4 Tune in the entry's mode, the MFK tunes only "
             "at its press and further up the list, Tune as you go, Range Near the dial (9.6 MHz +-250 kHz)",
             t_onair, 150),
        Test("modes-menu", "both RX", "the Modes menu on both: win 'Modes', its items read, the MFK's press opens JTTY, "
             "'open 114' twice opens and closes it", t_modes_menu, 60),
        Test("timesig-am-filter", "both RX", "Time Signal in AM on both: the filter's low edge at 100 Hz or below",
             t_timesig_am_filter, 40),
        Test("d-sdr-gain", "both RX", "D BANDS' LISTEN round the whole list on both: on every band B's SDR takes, the "
             "stick's gain by the rule (19.7 FM, 49.6 WX SAT/960 MHz+, 37.2 air/marine/NOAA, 28.0 the rest)",
             t_d_sdr_gain, 300),
        Test("e-sdr-open", "both RX", "E DECODE's SDR types on both: NOAA ALERTS, FM RDS, DIGITAL VOICE, METEOR put B "
             "on their first channel at the rule's gain; ACARS opens H with the airband gain; ADS-B opens H",
             t_e_sdr_open, 150),
        Test("g-sdr-groups", "both RX", "G WALKIE's MFK groups on both with an RTL-SDR: 2 M, 1.25 M, 70 CM, 33 CM, 23 CM "
             "FM and DMR reached, each 'THE RTL-SDR ONLY RECEIVES'; A/B on 146.520 arms nothing", t_g_sdr_groups, 120),
        Test("tune-heard", "both RX", "tuning a page gives the speaker to its own radio on both: A's VFO the X6200, "
             "B's the SDR", t_tune_heard, 40),
        Test("vol-press", "both RX", "VOL pressed short and held 2 s on both: Time Signal and the Modes menu stay open",
             t_vol_press, 50),
        Test("leave-speaker", "both RX", "from B with its SDR heard, on both: H, an app and CLASSIC RADIO each give the "
             "speaker back to the X6200 (play mode off)", t_leave_speaker, 90),
        Test("scanner-keys", "both RX", "the Scanner on both: Source CB, the MFK's press starts and pauses it, Skip, closed "
             "back on the dial and mode it had", t_scanner, 70),
    ] + [Test(f"winlink-no-tx {s}>{l}", f"{s.upper()} TX, {l.upper()} RX", f"Pat on {s} calls {l} over ARDOP with "
              f"RIT +500 Hz, then XIT +100 Hz set: refused ('Winlink: no TX, RIT/XIT is set'), nothing keyed",
              lambda o, s=s, l=l: t_winlink_no_tx(o, s, l), 120) for s, l in ((B, A), (A, B))]
