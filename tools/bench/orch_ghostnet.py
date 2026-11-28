#!/usr/bin/env python3
"""GhostNet on the Sentry (the owner, 2026-11-27, from S2 Underground): orch.py's tests of F SYSTEM > GHOSTNET (LISTEN)
and of TRIBAL KNOWLEDGE's GhostNet entry, on both radios at once, receive only.

The page's words are read as the screen has them through the remote's "ghostnet?" (app_sentry.h app_sentry_ghostnet_t:
open, on, head, tune, how, ant, band, sched_head, sched, watch, foot, legend, keys), TRIBAL KNOWLEDGE's page through
"tribal?" (open, page, pages, text); F's choice from the GUI's state (/mnt/gui_state.conf s.sys) and the remote's
fsel. What the page must say is worked out here from the repository and the radio's own files, never copied: the
antenna from app_sentry.c's bands[] (the band 7.107 MHz is in), the nets from the schedule the radio reads
(/mnt/ghostnet_nets.txt, else /usr/share/x6100/ghostnet_nets.txt), their next starts computed in UTC.

Nothing is keyed: F1 TUNE only moves A's dial (to 7.107 MHz USB-D, on both radios together), and the test checks
tx 0 and rxo 1 after it. Both end on page A on 7.107 MHz, the last frequency (never put back).

orch.py imports this module and sets H to itself; tests() gives the Test rows and COVERS the features each covers.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import datetime
import json
import os
import re
import time

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_ghostnet.H = sys.modules[__name__])

TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SRC = os.path.join(TOP, "src", "app_sentry.c")
SYS_TRIBAL, SYS_GHOSTNET, SYS_N = 10, 14, 15    # app_sentry.c sys_items[]: GHOSTNET last, after GMRS (13)
F_ITEM = "GHOSTNET (LISTEN)"    # sys_items[]'s name, the state's fsel
GN_HZ, USB_D = 7107000, 3       # TUNE's dial and mode (radio_mode_usb_dig)
AWAY = {"freq": 14200000, "mode": 2}    # where both start, off the net: TUNE must move them
DAYS = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]


def _band_at(khz):
    """bands[]'s row (name, ant, when) that khz is in, read from app_sentry.c as band_for() takes it"""
    src = open(SRC).read()
    body = src[src.index("static const band_t bands[] = {"):]
    body = body[:body.index("\n};")]
    rows = re.findall(r'\{"([^"]+)",\s*(\d+),\s*(\d+),\s*"[^"]*",\s*"([^"]*)",\s*\'(\w)\'', body)
    for name, lo, hi, ant, when in rows:
        if int(lo) <= khz <= int(hi):
            return name, ant, when
    raise Refused(f"app_sentry.c bands[]: no band holds {khz} kHz")


def _nets(host):
    """The JS8 windows on 7.107 in the schedule the radio reads (the DATA partition's copy first): [(name, wday,
    hh, mm, minutes)]"""
    txt = bench.ssh(host, "cat /mnt/ghostnet_nets.txt 2>/dev/null || cat /usr/share/x6100/ghostnet_nets.txt")
    names, segs = {}, []
    for ln in txt.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        p = ln.split()
        if p[0] == "net" and len(p) >= 3:
            names[p[1]] = " ".join(p[2:])
        elif len(p) >= 6 and p[1].upper() in DAYS and p[4] == str(GN_HZ // 1000) and p[5] == "JS8":
            hh, mm = map(int, p[2].split(":"))
            segs.append((p[0], DAYS.index(p[1].upper()), hh, mm, int(p[3])))
    return [(names.get(n, n).upper(), d, hh, mm, m) for n, d, hh, mm, m in segs]


def _next_utc(now, wday, hh, mm, minutes):
    """ghostnet_next_utc: the next start of the weekly window, or the start of the one going on"""
    day0 = now.replace(hour=0, minute=0, second=0, microsecond=0)
    py_wday = (now.weekday() + 1) % 7         # Python's Monday 0 to the schedule's Sunday 0
    start = day0 + datetime.timedelta(days=(wday - py_wday) % 7, hours=hh, minutes=mm)
    if start - now > datetime.timedelta(days=7) - datetime.timedelta(minutes=minutes):
        start -= datetime.timedelta(days=7)   # one that began before now and is still on
    if start + datetime.timedelta(minutes=minutes) <= now:
        start += datetime.timedelta(days=7)
    return start


def _expected_rows(host):
    """The page's rows as they must read now, the next first: (name, "FRI 01:00 UTC")"""
    now = datetime.datetime.now(datetime.timezone.utc)
    w = [(_next_utc(now, d, hh, mm, m), n) for n, d, hh, mm, m in _nets(host)]
    w.sort()
    return [(n, f"{DAYS[(t.weekday() + 1) % 7]} {t:%H:%M} UTC") for t, n in w[:5]]


def _ask(r, what, prefix, timeout=4.0):
    """The remote's answer to `what` ("ghostnet?", "tribal?") as a dict"""
    t0 = time.time()
    r.send(what)
    while time.time() - t0 < timeout:
        with r.lock:
            got = [x for when, x in list(r.lines) if when >= t0 and x.startswith(prefix + " ")]
        if got:
            return json.loads(got[-1][len(prefix) + 1:])
        time.sleep(0.05)
    raise Refused(f"{r.name}: no answer to '{what}' (a GUI from before it)")


def _gn(r):
    return _ask(r, "ghostnet?", "ghostnet")


def _tr(r):
    return _ask(r, "tribal?", "tribal")


def _wait(r, what, ask, cond, secs=6.0):
    end = time.time() + secs
    while True:
        s = ask(r)
        if cond(s):
            return s
        if time.time() > end:
            raise Refused(f"{r.name}: {what}: reads {json.dumps(s)[:300]}")
        time.sleep(0.25)


def _choose(r, item):
    """F's choice turned to item by the MFK (suite_cover_b's pick: the plain difference, then the way round)"""
    import suite_cover_b as cb
    time.sleep(0.6)
    return cb.pick(r, r.host, "s.sys", item, SYS_N)


def _page_check(n, r, s, band, rows):
    """What the open page must say: refused naming the first that it does not"""
    name, ant, when = band
    best = {"N": "NIGHT", "D": "DAY", "A": "ANY"}.get(when, "ALWAYS")     # app_sentry.c best_name()
    if not s.get("open"):
        raise Refused(f"{n}: the GHOSTNET page is not open (ghostnet? open 0)")
    if r.state.get("fsel") != F_ITEM:
        raise Refused(f"{n}: F's choice reads {r.state.get('fsel')!r}, not {F_ITEM!r}")
    if "7.107" not in s["head"] or "JS8CALL" not in s["head"] or "RECEIVE ONLY" not in s["head"]:
        raise Refused(f"{n}: the head reads {s['head']!r}")
    if not s["tune"].startswith("TUNE: A ON 7.107 MHZ USB-D"):
        raise Refused(f"{n}: TUNE's line reads {s['tune']!r}")
    if s["ant"] != f"ANTENNA: {ant}":
        raise Refused(f"{n}: the antenna reads {s['ant']!r}, not D's {name} {ant!r}")
    if not s["band"].startswith(f"D'S {name}: BEST {best}") or ", NOW " not in s["band"]:
        raise Refused(f"{n}: D's band reads {s['band']!r} (expected {name}, BEST {best}..., NOW)")
    got = [x for x in s["sched"] if x]
    if len(got) != len(rows):
        raise Refused(f"{n}: {len(got)} schedule rows, the schedule has {len(rows)} JS8 windows on 7.107: {got}")
    for want, line in zip(rows, got):
        if not line.startswith(want[0][:30]) or want[1] not in line or not re.search(r"\d+ MIN  (IN |ON NOW)", line):
            raise Refused(f"{n}: schedule row {line!r}, expected {want[0]} at {want[1]}, the next first")
    if "AROUND THE CLOCK" not in s["watch"] or "@GHOSTNET" not in s["watch"]:
        raise Refused(f"{n}: the line on who listens reads {s['watch']!r}")
    if "NO TX" not in s["foot"] or "F1/PUSH: TUNE" not in s["foot"]:
        raise Refused(f"{n}: the keys' line reads {s['foot']!r}")
    if not s["legend"].startswith("VFO: - | MFK: -, PUSH TUNE |"):
        raise Refused(f"{n}: the legend reads {s['legend']!r}, not the page's (VFO: - | MFK: -, PUSH TUNE ...)")
    keys = [" ".join(k.split()).upper() for k in s["keys"]]      # show_keys: "F1 TUNE", "F2 " for none
    if keys != ["F1 TUNE", "F2", "F3", "F4", "F5 BACK"]:
        raise Refused(f"{n}: the F keys read {keys}, not F1 TUNE, F2-F4 nothing, F5 BACK")
    return f"{s['ant']}; {s['band']}; next {got[0].split('  ')[0].strip()} {rows[0][1]}"


def t_ghostnet_f(o):
    """F SYSTEM > GHOSTNET (LISTEN) on both radios: 'set gnp' refused off F; both on 14.200 USB, then on F its row
    (s.sys 14, fsel) chosen with the MFK and opened with F2; the page read ('ghostnet?'): the head, TUNE's line, D's
    antenna and band for 7.107 as app_sentry.c's bands[] has them, the schedule's JS8 windows on 7.107 in the order of
    their next starts as computed here from the radio's own schedule file, who listens, the keys (F1 TUNE, F5 BACK,
    nothing else); a picture of each; F1 on both: page A, 7.107 MHz USB-D, alike, nothing keyed; F and F2 again: the
    page says A is on it, the VFO and the MFK a click move neither the dial nor F's choice, F5 closes it. A is taken
    by hand first (AUTO and SEMI lock it on signals). Both end on page A on 7.107"""
    band = _band_at(GN_HZ // 1000)
    rows = {n: _expected_rows(r.host) for n, r in o.r.items()}
    if not all(rows.values()):
        raise Refused(f"no JS8 window on 7.107 in a radio's GhostNet schedule: {rows}")
    out = {}
    with o.step("'set gnp 1' refused off page F") as s:
        r = o.r[H.B]
        if r.state.get("pg") == "F":
            o.both_page("A")
        n0 = len(r.errs)
        r.send("set gnp 1")
        r.wait("the refusal", lambda: any("gnp: only while the Sentry shows F" in e for e in r.errs[n0:]), 5)
        s.confirm(r.errs[-1])
    with o.step(f"both by hand (a VFO click on page A: the Sentry stops moving the dial), then on "
                f"{AWAY['freq'] / 1e6:.3f} MHz USB, off the net (TUNE must move them)") as s:
        # AUTO and SEMI lock A on the signals in its picture on any page: on 2026-11-27 they took the two radios to
        # 14.1897 and 14.180 MHz two seconds after 14.200 was set, while F's page was open
        o.both_page("A")

        def by_hand(n, r):
            r.send("knob main 1")
            time.sleep(1.0)
            return r.state.get("freq")
        H.in_parallel(o, by_hand)
        o.both(AWAY)
        time.sleep(3.0)                     # long enough for a lock to show, were A still the Sentry's
        H.alike(o, ("freq", "mode"))
        if int(bench._value(o.r[H.A], "freq")) != AWAY["freq"]:
            raise Refused(f"A's dial left {AWAY['freq']} by itself: {bench._value(o.r[H.A], 'freq')}")
        s.confirm(f"{bench._value(o.r[H.A], 'freq')} mode {bench._value(o.r[H.A], 'mode')}, still after 3 s")
    try:
        with o.step(f"both on page F, its choice {F_ITEM} by the MFK (s.sys {SYS_GHOSTNET}), F2 opens the page") as s:
            o.both_page("F")

            def open_(n, r):
                got = _choose(r, SYS_GHOSTNET)
                r.wait("fsel", lambda: r.state.get("fsel") == F_ITEM, 4)
                bench.screenshot(r, "ghostnet-f-list")     # F's 15 rows, GHOSTNET chosen at the bottom
                bench.tap(r, "F2")
                _wait(r, "the page open", _gn, lambda g: g.get("open") == 1)
                return f"s.sys {got}, fsel {r.state.get('fsel')!r}"
            s.confirm("; ".join(f"{n} {v}" for n, v in H.in_parallel(o, open_, wait=60).items()))
        with o.step("the page read on both: head, TUNE, D's antenna and band, the nets on 7.107 the next first, "
                    "who listens, the keys; a picture") as s:
            def read(n, r):
                g = _wait(r, "the page filled", _gn, lambda g: g.get("open") == 1 and g.get("ant"))
                said = _page_check(n, r, g, band, rows[n])
                pic = bench.screenshot(r, "ghostnet-page")
                return f"{said}; picture {os.path.basename(pic or '-')}"
            got = H.in_parallel(o, read)
            out["page"] = got
            s.confirm(" | ".join(f"{n}: {v}" for n, v in got.items()))
        with o.step("F1 (TUNE) on both: page A, 7.107 MHz USB-D, alike; nothing keyed") as s:
            def tune(n, r):
                bench.tap(r, "F1")
                r.wait("A on the net", lambda: r.state.get("pg") == "A" and int(r.state.get("freq", 0)) == GN_HZ
                       and int(r.state.get("mode", -1)) == USB_D, 8)
                if _gn(r).get("open"):
                    raise Refused(f"{n}: the page still open after TUNE")
                return f"page {r.state.get('pg')} {r.state.get('freq')} mode {r.state.get('mode')}"
            got = H.in_parallel(o, tune)
            H.alike(o, ("freq", "mode"))
            if any(r.state.get("tx") or r.state.get("rxo") != 1 for r in o.r.values()):
                raise Refused("a radio armed or keyed")
            out["tune"] = got
            s.confirm(str(got))
        with o.step("F and F2 again on both: the page says A is on the net; the VFO and the MFK a click: the "
                    "dial and F's choice unmoved, the page open; F5 closes it") as s:
            def again(n, r):
                bench.open_page(r, "F")
                time.sleep(0.6)
                if r.state.get("fsel") != F_ITEM:
                    raise Refused(f"{n}: F's choice reads {r.state.get('fsel')!r} after TUNE, not {F_ITEM!r}")
                bench.tap(r, "F2")
                g = _wait(r, "the page open", _gn, lambda g: g.get("open") == 1 and g.get("tune"))
                if not g.get("on") or "(A IS ON IT)" not in g["tune"]:
                    raise Refused(f"{n}: on the net, yet the page says on {g.get('on')}, {g['tune']!r}")
                r.send("knob main 1")
                r.send("knob mfk 1")
                time.sleep(1.5)
                if int(r.state.get("freq", 0)) != GN_HZ or int(r.state.get("mode", -1)) != USB_D:
                    raise Refused(f"{n}: a knob under the page moved A to {r.state.get('freq')} mode "
                                  f"{r.state.get('mode')}")
                if not _gn(r).get("open") or r.state.get("fsel") != F_ITEM:
                    raise Refused(f"{n}: a knob closed the page or moved F's choice ({r.state.get('fsel')!r})")
                bench.tap(r, "F5")
                _wait(r, "the page closed", _gn, lambda g: not g.get("open"))
                return g["tune"]
            s.confirm(str(H.in_parallel(o, again)))
            H.alike(o, ("freq", "mode"))
    finally:
        for r in o.r.values():
            if r.state.get("pg") == "F":
                r.send("set gnp 0")
        o.both_page("A")
    return True, f"F's GHOSTNET on both: {out.get('tune')}", out


def t_ghostnet_tribal(o):
    """TRIBAL KNOWLEDGE's GhostNet entry on both radios: the radio's own tribal.txt has it (S2 Underground's word on
    the JS8Call net on 7.107 MHz); F's TRIBAL KNOWLEDGE chosen with the MFK (s.sys 10), F2 opens it, the MFK turns
    the pages ('tribal?') to the one with the entry's heading, which says 7.107 MHz, 01:00 UTC, Thursday nights,
    monitored 24/7 and credits S2 Underground; F2 closes it. Both end on page A"""
    head = "GHOSTNET: THE JS8CALL NET ON 7.107 MHZ"
    words = ("S2 UNDERGROUND (@S2_UNDERGROUND)", "7.107 MHZ", "01:00 UTC", "THURSDAY NIGHTS", "24/7", "USB-D")
    with o.step("the radio's own tribal.txt has the GhostNet entry (both)") as s:
        def has(n, r):
            c = bench.ssh(r.host, f"grep -c -i '^{head}$' /usr/share/x6100/tribal.txt").strip()
            if c != "1":
                raise Refused(f"{n}: /usr/share/x6100/tribal.txt has no '{head}' heading (an image from before it)")
            return "has it"
        s.confirm(str(H.in_parallel(o, has)))
    try:
        with o.step("both on page F, TRIBAL KNOWLEDGE chosen by the MFK (s.sys 10), F2 opens it") as s:
            o.both_page("F")

            def open_(n, r):
                _choose(r, SYS_TRIBAL)
                pic = bench.screenshot(r, "ghostnet-tribal-list")     # F's 15 rows, TRIBAL KNOWLEDGE chosen
                bench.tap(r, "F2")
                t = _wait(r, "TRIBAL KNOWLEDGE open", _tr, lambda t: t.get("open") == 1 and t.get("pages", 0) > 0)
                return f"page {t['page']} of {t['pages']}, F's list {os.path.basename(pic or '-')}"
            s.confirm(str(H.in_parallel(o, open_, wait=60)))
        with o.step("the MFK turns the pages to GhostNet's entry on both: its words read") as s:
            def find(n, r):
                import suite_cover_b as cb
                t = _tr(r)
                pages, sign = t["pages"], 1
                for _ in range(pages + 4):
                    if head in t["text"]:
                        break
                    was = t["page"]
                    cb.clicks(r, "mfk", sign)
                    try:
                        t = _wait(r, "the next page", _tr, lambda x: x.get("page") != was, 3)
                    except Refused:
                        if was in (1, pages):       # at an end, the knob turned the way the list does not go
                            sign = -sign
                            continue
                        raise
                    if t["page"] < was and sign > 0 or t["page"] > was and sign < 0:
                        sign = -sign                # the knob goes the other way on this radio: back the right way
                if head not in t["text"]:
                    raise Refused(f"{n}: no page of {pages} has '{head}'")
                missing = [w for w in words if w not in t["text"]]
                if missing:
                    raise Refused(f"{n}: page {t['page']} has the heading but not {missing}")
                return f"page {t['page']} of {pages}"
            got = H.in_parallel(o, find, wait=90)
            s.confirm(str(got))
        with o.step("F2 closes it on both") as s:
            def close(n, r):
                bench.tap(r, "F2")
                _wait(r, "closed", _tr, lambda t: not t.get("open"))
                return "closed"
            s.confirm(str(H.in_parallel(o, close)))
    finally:
        o.both_page("A")
    return True, f"TRIBAL KNOWLEDGE's GhostNet entry on both: {got}", {"pages": got}


COVERS = {
    "ghostnet-f": (["f-system-ghostnet"], []),
    "ghostnet-tribal": (["f-system-tribal-ghostnet"], ["f-system-tribal-knowledge"]),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("ghostnet-f", "both RX", "F SYSTEM's GHOSTNET on both: chosen, opened, its page read (D's antenna for "
             "7.107, the nets the next first), the VFO unmoved, F1 TUNE: A on 7.107 USB-D, nothing keyed",
             t_ghostnet_f, 90),
        Test("ghostnet-tribal", "both RX", "TRIBAL KNOWLEDGE's GhostNet entry on both: in the radio's tribal.txt, "
             "found by the MFK's pages, S2 Underground's word on 7.107", t_ghostnet_tribal, 90),
    ]
