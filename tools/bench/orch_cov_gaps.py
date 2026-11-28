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


COVERS = {
    "onair": (["onair-list", "onair-tune", "onair-tune-as-you-go", "onair-range"], []),
    "modes-menu": (["modes-menu"], []),
    "timesig-am-filter": (["timesig-am-filter-100hz"], []),
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
    ]
