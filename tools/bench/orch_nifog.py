#!/usr/bin/env python3
"""CISA's NIFOG on both radios (the owner, 2026-11-25): orch.py's test of F SYSTEM > NIFOG and the web page /nifog.

Receive only: nothing keyed, the dial never moved (the VFO scrolls NIFOG's page while it is open; the sync guard
watches A's dial all the while). F's NIFOG is read back from the GUI's state (/mnt/gui_state.conf, written within
2 s): s.sys F's choice, s.nf the page open (0 closed), s.nfy how far down it is. The pages, the sections and the
PDF's bytes are what the repository has (tools/nifog/nifog.idx, third-party/nifog/README.md).

orch.py imports this module and sets H to itself; tests() gives the Test rows and COVERS the features each covers.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import hashlib
import os
import re
import sys
import time
import urllib.error
import urllib.request

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_nifog.H = sys.modules[__name__])

TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(TOP, "webserver", "src"))
from x6100_webserver import nifog_web  # noqa: E402  (no bottle in it)

IDX = nifog_web.read_index(os.path.join(TOP, "tools", "nifog"))
SYS_NIFOG = 11                  # app_sentry.c sys_items[]: after TRIBAL KNOWLEDGE (START IN 12, GMRS 13, GHOSTNET 14)
PDF_SHA = re.search(r"SHA-256 `([0-9a-f]{64})`", open(os.path.join(TOP, "third-party", "nifog", "README.md")).read())
PDF_SHA = PDF_SHA.group(1) if PDF_SHA else None
NF_STEP = 24                    # app_sentry.c: px a VFO click


def _gstate(h):
    d = {}
    for ln in bench.ssh(h, "cat /mnt/gui_state.conf 2>/dev/null").splitlines():
        k, _, v = ln.partition("=")
        if v.strip().lstrip("-").isdigit():
            d[k.strip()] = int(v)
    return d


def _gwait(h, what, cond, secs=6.0):
    end = time.time() + secs
    while True:
        g = _gstate(h)
        if cond(g) or time.time() > end:
            if not cond(g):
                raise Refused(f"{h}: {what}: the GUI's state reads s.sys {g.get('s.sys')} s.nf {g.get('s.nf')} "
                              f"s.nfy {g.get('s.nfy')}")
            return g
        time.sleep(0.5)


def _next_section(p):
    """F3: the next page where a chapter or a section starts (src/nifog.c nifog_step)"""
    return min([s["page"] for s in IDX["secs"] if s["level"] <= 1 and s["page"] > p], default=p)


def _print_px(path):
    """Pixels of NIFOG's page window (app_sentry.c build_nifog: x 9-790, y 78-380 on the 800x480 picture) that differ
    from its commonest colour (the paper, the theme's background) by more than 60 in a channel: the page's print.
    A window left unpainted reads 0"""
    from PIL import Image
    im = Image.open(path).convert("RGB").crop((9, 78, 791, 381))
    px = list(im.getdata())
    bg = max(set(px[::97]), key=px[::97].count)
    return sum(1 for p in px if max(abs(p[0] - bg[0]), abs(p[1] - bg[1]), abs(p[2] - bg[2])) > 60)


_ADDR = {}


def _addr(host):
    """The radio's address, looked up once: each lookup of a .local name took 5 s from Python (mDNS), and the pages'
    test asks 384 times (suite_more.py found the same)"""
    import socket
    if host not in _ADDR:
        _ADDR[host] = socket.gethostbyname(host)
    return _ADDR[host]


def _http(host, path, headers=None, timeout=60):
    req = urllib.request.Request(f"http://{_addr(host)}{path}", headers={"Host": host, **(headers or {})})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, dict(resp.headers), resp.read()


def t_nifog(o):
    """F SYSTEM > NIFOG on both radios: chosen on F's list and opened with F2 (a page open); F4 the contents, the page
    drawn (its print counted in a picture of the window); the MFK turns a page, F3 goes to the next section's page as
    the contents have it, the VFO scrolls down the page (the dial unmoved), F2 closes it (F's choice still NIFOG), F2
    opens it again where it was left, drawn again (picture). Then the web page: /nifog with the contents,
    /nifog.pdf whole (CISA's bytes, its SHA-256) and in a Range (206), /nifog/page/68 a PNG. Both end on page A"""
    if IDX["pages"] != 192:
        raise Refused("tools/nifog/nifog.idx: no 192 pages")
    mfk_sign = {}
    with o.step("both on the Sentry's page F, F's choice NIFOG") as s:
        o.both_page("F")

        def choose(n, r):
            """F's choice turned to NIFOG by the MFK, as suite_cover_b's pick(): the plain difference first, then the
            way round; a knob the remote turns the other way on this radio is found and remembered (sign)"""
            h = r.host
            time.sleep(0.6)
            g = _gstate(h)
            cur = g.get("s.sys")
            if cur is None:
                raise Refused(f"{n}: no s.sys in its GUI's state")
            if g.get("s.nf"):
                raise Refused(f"{n}: NIFOG open already (s.nf {g['s.nf']})")
            sign = 1
            for k in range(4):
                if cur == SYS_NIFOG:
                    break
                d = SYS_NIFOG - cur
                start = cur
                for _ in range(abs(d)):
                    r.send(f"knob mfk {sign if d > 0 else -sign}")
                    time.sleep(0.15)
                end = time.time() + 5
                while time.time() < end:
                    cur = _gstate(h).get("s.sys")
                    if cur != start:
                        break
                    time.sleep(0.5)
                if cur != SYS_NIFOG and cur != start and (cur - start) * d < 0:
                    sign = -sign            # the knob went the other way on this radio
            if cur != SYS_NIFOG:
                raise Refused(f"{n}: F's choice reads {cur}, not NIFOG ({SYS_NIFOG}), after the MFK")
            if k:
                mfk_sign[n] = sign
            return cur
        got = H.in_parallel(o, choose)
        s.confirm("; ".join(f"{n} s.sys {v}" for n, v in got.items()))
    with o.step("F2 opens NIFOG: a page open, its picture") as s:
        def open_(n, r):
            bench.tap(r, "F2")
            g = _gwait(r.host, "NIFOG open", lambda g: g.get("s.nf", 0) >= 1)
            return g["s.nf"]
        page = H.in_parallel(o, open_)
        pics = {n: bench.screenshot(r, "nifog-open") for n, r in o.r.items()}
        s.confirm("; ".join(f"{n} page {p}, picture {os.path.basename(pics[n] or '-')}" for n, p in page.items()))
    with o.step("F4: the contents (page iii), the page drawn (its print in the picture)") as s:
        toc = next((x["page"] for x in IDX["secs"] if x["title"] == "Table of Contents"), 4)

        def contents(n, r):
            bench.tap(r, "F4")
            _gwait(r.host, f"F4: page {toc}", lambda g: g.get("s.nf") == toc and g.get("s.nfy") == 0)
            time.sleep(0.8)
            shot = bench.screenshot(r, "nifog-contents")
            ink = _print_px(shot) if shot else 0
            if ink < 500:
                raise Refused(f"{n}: the contents page not drawn: {ink} px of print in the window ({shot})")
            page[n] = toc
            return f"page {toc}, {ink} px of print"
        s.confirm("; ".join(f"{n} {v}" for n, v in H.in_parallel(o, contents).items()))
    with o.step("the MFK turns a page, F3 to the next section, the VFO scrolls") as s:
        # the dials as NIFOG found them (page G, orch's set-up, puts A's own dial back when left: not NIFOG's doing)
        a0 = {n: {k: bench._value(r, k) for k in ("freq", "mode")} for n, r in o.r.items()}
        def keys(n, r):
            h, p0 = r.host, page[n]
            # the MFK: a click at a time till the page changes (a radio may not take every single click, as
            # suite_cover_b's pick() finds), on to the next page or back to the one before
            p1 = None
            for _ in range(3):
                r.send(f"knob mfk {mfk_sign.get(n, 1)}")
                try:
                    p1 = _gwait(h, "the MFK", lambda g: g.get("s.nf") not in (None, 0, p0) and g.get("s.nfy") == 0,
                                3.5)["s.nf"]
                    break
                except Refused:
                    continue
            if p1 is None or abs(p1 - p0) > 2:
                raise Refused(f"{n}: the MFK did not turn NIFOG's page {p0} (now {p1})")
            bench.tap(r, "F3")
            p2 = _next_section(p1)
            _gwait(h, f"F3: page {p2}", lambda g: g.get("s.nf") == p2 and g.get("s.nfy") == 0)
            # the VFO (the remote's "main" knob): down the page, or (turned the other way here) onto the page before
            g = None
            for _ in range(3):
                r.send("knob main 2")
                try:
                    g = _gwait(h, "the VFO", lambda g: (g.get("s.nf") == p2 and g.get("s.nfy", 0) > 0)
                               or g.get("s.nf") in (p2 - 1, p2 + 1), 3.5)
                    break
                except Refused:
                    continue
            if g is None:
                raise Refused(f"{n}: the VFO did not scroll NIFOG's page {p2}")
            way = f"{g['s.nfy']} px down" if g["s.nf"] == p2 else f"onto page {g['s.nf']}"
            return f"page {p0} > MFK {p1} > F3 {p2} ({IDX['labels'].get(p2, '?')}) > VFO: {way}"
        moved = H.in_parallel(o, keys)
        H.alike(o, ("freq", "mode"))
        for n, r in o.r.items():
            a = {k: bench._value(r, k) for k in ("freq", "mode")}
            if a != a0[n]:
                raise Refused(f"{n}: the dial moved while NIFOG was open: {a0[n]} -> {a}")
        s.confirm("; ".join(f"{n}: {v}" for n, v in moved.items()))
    with o.step("F2 closes NIFOG, F's choice still NIFOG") as s:
        def close(n, r):
            bench.tap(r, "F2")
            return _gwait(r.host, "NIFOG closed", lambda g: g.get("s.nf") == 0 and g.get("s.sys") == SYS_NIFOG)["s.sys"]
        H.in_parallel(o, close)
        s.confirm("both closed")
    with o.step("F2 opens it again where it was left, the page drawn again") as s:
        def reopen(n, r):
            before = _gstate(r.host)
            bench.tap(r, "F2")
            g = _gwait(r.host, "NIFOG open again", lambda g: g.get("s.nf", 0) >= 1)
            time.sleep(0.8)
            shot = bench.screenshot(r, "nifog-reopen")
            ink = _print_px(shot) if shot else 0
            bench.tap(r, "F2")
            _gwait(r.host, "NIFOG closed", lambda g: g.get("s.nf") == 0)
            if ink < 500:
                raise Refused(f"{n}: page {g['s.nf']} not drawn when opened again: {ink} px of print ({shot})")
            return f"page {g['s.nf']} {g.get('s.nfy')} px down, {ink} px of print (s.sys was {before.get('s.sys')})"
        s.confirm("; ".join(f"{n} {v}" for n, v in H.in_parallel(o, reopen).items()))
    with o.step("the web page: /nifog, the PDF (whole and a Range), a page's PNG") as s:
        def web(n, r):
            st, _, body = _http(r.host, "/nifog")
            if st != 200 or b"VHF Inland - VTAC17" not in body:
                raise Refused(f"{n}: /nifog {st}, its contents {'there' if b'VTAC17' in body else 'missing'}")
            st, hd, pdf = _http(r.host, "/nifog.pdf", timeout=180)
            sha = hashlib.sha256(pdf).hexdigest()
            if st != 200 or not hd.get("Content-Type", "").startswith("application/pdf") or sha != PDF_SHA:
                raise Refused(f"{n}: /nifog.pdf {st} {hd.get('Content-Type')} {len(pdf)} bytes, SHA-256 {sha[:12]}")
            st, _, part = _http(r.host, "/nifog.pdf", {"Range": "bytes=1000-1999"})
            if st != 206 or part != pdf[1000:2000]:
                raise Refused(f"{n}: a Range of /nifog.pdf: {st}, {len(part)} bytes")
            st, hd, png = _http(r.host, "/nifog/page/68")
            if st != 200 or png[:8] != b"\x89PNG\r\n\x1a\n":
                raise Refused(f"{n}: /nifog/page/68 {st}")
            return f"/nifog 200; PDF {len(pdf)} bytes, CISA's SHA-256; Range 206; page 68 PNG {len(png)} bytes"
        res = H.in_parallel(o, web, wait=200)
        s.confirm("; ".join(f"{n}: {v}" for n, v in res.items()))
    with o.step("both back on page A") as s:
        o.both_page("A")
        s.confirm("page A")
    return True, ("F > NIFOG on both: opened on a page, the MFK, F3 and the VFO moved through it as its contents say, "
                  "the dial unmoved, F2 closed it; /nifog, CISA's PDF (whole, its SHA-256, and a Range) and a page's "
                  "PNG served"), {"open": page, "keys": moved, "web": res, "pictures": pics}


# ======== One test per feature (features.json f-nifog-*, web-nifog-*): the owner, 2026-11-25, "add unit tests to the
# testing bench for everything you added". Each runs on both radios at once, receive only, and ends with NIFOG closed and
# both on page A. NIFOG's words are read as the screen has them through the remote's "nifog?" (app_sentry.h
# app_sentry_nifog_t: open, page, y, maxy, cap, where, legend, keys), its page and scroll also from the GUI's state ========

WIN = (9, 78, 791, 381)         # app_sentry.c build_nifog: the page's window on the 800x480 picture
X0_PORTRAIT = (782 - 512) // 2  # where a 512 px portrait page starts in the 782 px window (nf_draw)


def _nf(r, timeout=4.0):
    """The remote's "nifog?" answer: {open, page, y, maxy, cap, where, legend, keys}"""
    import json
    t0 = time.time()
    r.send("nifog?")
    while time.time() - t0 < timeout:
        with r.lock:
            got = [x for when, x in list(r.lines) if when >= t0 and x.startswith("nifog ")]
        if got:
            return json.loads(got[-1][6:])
        time.sleep(0.05)
    raise Refused(f"{r.name}: no answer to 'nifog?' (a GUI from before it: app_sentry_nifog_state)")


def _nfw(r, what, cond, secs=6.0):
    """nifog? till cond holds (secs at most): its last answer"""
    end = time.time() + secs
    while True:
        s = _nf(r)
        if cond(s):
            return s
        if time.time() > end:
            raise Refused(f"{r.name}: {what}: nifog? reads open {s.get('open')} page {s.get('page')} y {s.get('y')} "
                          f"(maxy {s.get('maxy')}) '{s.get('cap')}'")
        time.sleep(0.25)


def _sec_of(p):
    """The caption's section on page p: the last entry of level 0 or 1 starting there or before (src/nifog.c)"""
    best = None
    for s in IDX["secs"]:
        if s["page"] <= p and s["level"] <= 1:
            best = s
    return best


def _cap(p):
    """The caption app_sentry.c nf_show writes on page p, upper case as the Sentry draws"""
    ver = IDX["edition"].split(",")[0].strip()
    lab = IDX["labels"].get(p, "")
    c = (f"NIFOG {ver}  PAGE {lab} ({p} OF {IDX['pages']})" if lab else f"NIFOG {ver}  PAGE {p} OF {IDX['pages']}")
    s = _sec_of(p)
    return (c + "  " + s["title"] if s else c).upper()


def _same_text(got, want):
    """A label's text against what it should say: whole, or cut to fit with LVGL's dots (LV_LABEL_LONG_DOT)"""
    return got == want or (got.endswith("...") and want.startswith(got[:-3].rstrip()))


def _words(s):
    return re.sub(r"[^A-Z0-9]", "", s.upper())


def _choose(o):
    """Both on F with NIFOG chosen on its list and closed: {radio: the MFK's way here (1, or -1 when the remote's
    clicks go the other way round)}"""
    o.both_page("F")
    signs = {}

    def one(n, r):
        time.sleep(0.6)
        if _nf(r).get("open"):
            bench.tap(r, "F2")
            _nfw(r, "NIFOG closed", lambda s: not s["open"])
        sign = 1
        for _ in range(6):
            cur = _gstate(r.host).get("s.sys")
            if cur == SYS_NIFOG and r.state.get("fsel") in ("NIFOG", None):
                break
            d = SYS_NIFOG - cur
            for _ in range(abs(d)):
                r.send(f"knob mfk {sign if d > 0 else -sign}")
                time.sleep(0.15)
            end = time.time() + 4
            while time.time() < end and _gstate(r.host).get("s.sys") == cur:
                time.sleep(0.4)
            now = _gstate(r.host).get("s.sys")
            if now != cur and (now - cur) * d < 0:
                sign = -sign
        if _gstate(r.host).get("s.sys") != SYS_NIFOG:
            raise Refused(f"{n}: F's choice not NIFOG after the MFK")
        signs[n] = sign
        return sign
    H.in_parallel(o, one)
    return signs


def _open(o):
    def one(n, r):
        bench.tap(r, "F2")
        return _nfw(r, "NIFOG open (F2)", lambda s: s["open"])["page"]
    return H.in_parallel(o, one)


def _finish(o):
    """NIFOG closed and both on page A, whatever the test left"""
    def one(n, r):
        try:
            if _nf(r).get("open"):
                bench.tap(r, "F2")
                _nfw(r, "NIFOG closed", lambda s: not s["open"])
        except Refused:
            pass
        return True
    try:
        H.in_parallel(o, one)
    finally:
        o.both_page("A")


def _goto(r, target, sign=1):
    """NIFOG's page to target with the MFK, a click at a time when near, ten at a time when far, read back each time
    (a radio may not take every click)"""
    for _ in range(80):
        s = _nf(r)
        if s["page"] == target:
            return s
        d = target - s["page"]
        k = max(-10, min(10, d))
        r.send(f"knob mfk {k * sign}")
        end = time.time() + 2.5
        while time.time() < end and _nf(r)["page"] == s["page"]:
            time.sleep(0.15)
    raise Refused(f"{r.name}: NIFOG's page did not come to {target} with the MFK (at {_nf(r)['page']})")


def _key_to(r, key, what, cond):
    bench.tap(r, key)
    return _nfw(r, what, cond)


def _window(path):
    """The picture's page window: (print px, the columns with print: first, last; the commonest colour, the print's
    brightest colour)"""
    from PIL import Image
    im = Image.open(path).convert("RGB").crop(WIN)
    w, h = im.size
    px = list(im.getdata())
    bg = max(set(px[::97]), key=px[::97].count)
    ink, cols, top = 0, [], (0, 0, 0)
    far = lambda p: max(abs(p[0] - bg[0]), abs(p[1] - bg[1]), abs(p[2] - bg[2])) > 60     # noqa: E731
    for i, p in enumerate(px):
        if far(p):
            ink += 1
            cols.append(i % w)
            if sum(p) > sum(top):
                top = p
    return ink, (min(cols) if cols else None), (max(cols) if cols else None), bg, top


def _accent(path):
    """The theme's accent and background on the same picture: the frame's border (x 7, y 200) and outside it (x 2)"""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im.getpixel((7, 200)), im.getpixel((2, 300))


def _near(a, b, tol=40):
    return max(abs(a[i] - b[i]) for i in range(3)) <= tol


def _shot(r, label):
    time.sleep(0.8)
    p = bench.screenshot(r, label)
    if not p:
        raise Refused(f"{r.name}: no picture of its screen")
    return p


def _shot_until(r, label, ok, tries=4):
    """A picture judged by ok(stats of _window): taken again, 4 s apart, while it is not (a message box over the
    window goes in a few seconds: G's 'The radio can't tune 462.612 MHz' covered page 30 on 2026-11-25); the last"""
    for k in range(tries):
        p = _shot(r, label)
        st = _window(p)
        if ok(st) or k == tries - 1:
            return p, st
        time.sleep(4)


def t_list(o):
    """F's list: NIFOG chosen with the MFK reads back as s.sys 11 and as the remote's fsel 'NIFOG' (its name on the
    list)"""
    try:
        with o.step("both on F, the MFK to NIFOG") as s:
            _choose(o)
            got = {n: (r.state.get("fsel"), _gstate(r.host).get("s.sys")) for n, r in o.r.items()}
            bad = [n for n, (fs, sy) in got.items() if fs != "NIFOG" or sy != SYS_NIFOG]
            if bad:
                raise Refused(f"F's choice: {got}")
            s.confirm("; ".join(f"{n} fsel {fs!r} s.sys {sy}" for n, (fs, sy) in got.items()))
    finally:
        _finish(o)
    return True, "F's list: NIFOG is its row 11 on both (s.sys 11, fsel NIFOG)", {}


def t_open_close(o):
    """F2 opens and closes NIFOG, so does the MFK's push, leaving F closes it; F's choice stays NIFOG each time"""
    try:
        with o.step("both on F, NIFOG chosen") as s:
            _choose(o)
            s.confirm("chosen")
        with o.step("F2 opens, F2 closes; the push opens, the push closes") as s:
            def one(n, r):
                seen = []
                for key in ("F2", "F2", "MFK", "MFK"):
                    want = not seen or not seen[-1]
                    st = _key_to(r, key, f"{key}: NIFOG {'open' if want else 'closed'}",
                                 lambda x, want=want: bool(x["open"]) == want)
                    seen.append(bool(st["open"]))
                if _gstate(r.host).get("s.sys") != SYS_NIFOG:
                    raise Refused(f"{n}: F's choice moved")
                return seen
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n} {v}" for n, v in got.items()))
        with o.step("the push opens it again, page A closes it, F shows its list") as s:
            def one(n, r):
                _key_to(r, "MFK", "the push: open", lambda x: x["open"])
                return True
            H.in_parallel(o, one)
            o.both_page("A")
            o.both_page("F")
            got = {n: _nfw(r, "closed on F again", lambda x: not x["open"])["open"] for n, r in o.r.items()}
            s.confirm(f"open after A and back: {got}; s.sys " +
                      ", ".join(f"{n} {_gstate(r.host).get('s.sys')}" for n, r in o.r.items()))
    finally:
        _finish(o)
    return True, "F2 and the push open and close NIFOG, page A closes it, F's choice stays NIFOG, on both", {}


def t_drawn(o):
    """The page drawn after page G has been shown: page 30 (portrait) centred with empty margins, page 68 (the CTCSS
    table, landscape) across the window; the print's colours between the theme's background and accent; the GUI's
    log says each opening's print"""
    pics = {}
    try:
        with o.step("page G, then F, NIFOG chosen and opened") as s:
            o.both_page("G")
            time.sleep(1.5)
            signs = _choose(o)
            logs0 = {n: bench.ssh(r.host, "grep -c 'NIFOG: open on page' /tmp/x6100_log.txt || true").strip()
                     for n, r in o.r.items()}
            _open(o)
            s.confirm(f"log lines before: {logs0}")
        with o.step("page 30 (portrait): drawn, centred, its margins empty, in the theme's colours") as s:
            def one(n, r):
                _goto(r, 30, signs[n])
                p, (ink, c0, c1, bg, top) = _shot_until(r, "nifog-p30", lambda w: w[1] is not None and w[1] >= X0_PORTRAIT - 4
                                                        and w[2] <= X0_PORTRAIT + 512 + 4)
                acc, back = _accent(p)
                pics[f"{n}-30"] = p
                if ink < 3000:
                    raise Refused(f"{n}: page 30 not drawn: {ink} px of print ({p})")
                if c0 < X0_PORTRAIT - 4 or c1 > X0_PORTRAIT + 512 + 4:
                    raise Refused(f"{n}: page 30's print in columns {c0}-{c1}, outside {X0_PORTRAIT}-{X0_PORTRAIT + 512}")
                if not _near(bg, back, 12) or not _near(top, acc, 60):
                    raise Refused(f"{n}: colours: paper {bg} (background {back}), brightest print {top} (accent {acc})")
                return f"{ink} px of print in columns {c0}-{c1}, paper {bg}, print up to {top}"
            got = H.in_parallel(o, one, wait=90)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
        with o.step("page 68 (the CTCSS table, landscape): across the window") as s:
            def one(n, r):
                _goto(r, 68, signs[n])
                p, (ink, c0, c1, bg, top) = _shot_until(r, "nifog-p68", lambda w: w[0] >= 10000)
                pics[f"{n}-68"] = p
                if ink < 10000 or c0 > 12 or c1 < 770:
                    raise Refused(f"{n}: page 68: {ink} px of print in columns {c0}-{c1} ({p})")
                return f"{ink} px of print in columns {c0}-{c1}"
            got = H.in_parallel(o, one, wait=90)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
        with o.step("the GUI's log: the opening's print") as s:
            got = {}
            for n, r in o.r.items():
                line = bench.ssh(r.host, "grep 'NIFOG: open on page' /tmp/x6100_log.txt | tail -1").strip()
                m = re.search(r"NIFOG: open on page (\d+), (\d+) px of print", line)
                cnt = bench.ssh(r.host, "grep -c 'NIFOG: open on page' /tmp/x6100_log.txt || true").strip()
                if not m or int(m.group(2)) <= 0 or cnt == logs0[n]:
                    raise Refused(f"{n}: the log's last NIFOG line {line!r} ({cnt} lines, {logs0[n]} before)")
                got[n] = line[line.index("NIFOG"):]
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "NIFOG's pages drawn after G: portrait centred, the table across, the theme's colours, the log's line", \
        {"pictures": pics}


def t_caption(o):
    """The caption on the cover, the contents (roman), a VHF table and the CTCSS table: as tools/nifog/nifog.idx says"""
    try:
        with o.step("both on F, NIFOG open") as s:
            signs = _choose(o)
            _open(o)
            s.confirm("open")
        for p in (1, 4, 38, 68):
            with o.step(f"page {p}: '{_cap(p)}'") as s:
                def one(n, r, p=p):
                    _goto(r, p, signs[n])
                    st = _nfw(r, f"the caption of page {p}", lambda x: _same_text(x["cap"], _cap(p)))
                    return st["cap"]
                got = H.in_parallel(o, one, wait=90)
                s.confirm("; ".join(f"{n}: {v!r}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "the caption on pages 1, 4, 38 and 68 as the contents say, on both", {}


def t_mfk(o):
    """The MFK: a page on and back from the page's top, never before 1 nor past 192"""
    try:
        with o.step("both on F, NIFOG open on page 1") as s:
            signs = _choose(o)
            _open(o)
            H.in_parallel(o, lambda n, r: _goto(r, 1, signs[n]))
            s.confirm("page 1")
        with o.step("back from 1: still 1; on: 2 at its top; back: 1") as s:
            def one(n, r):
                r.send(f"knob mfk {-signs[n]}")
                time.sleep(2)
                if _nf(r)["page"] != 1:
                    raise Refused(f"{n}: back from page 1 went to {_nf(r)['page']}")
                r.send(f"knob mfk {signs[n]}")
                _nfw(r, "page 2", lambda x: x["page"] == 2 and x["y"] == 0)
                r.send(f"knob mfk {-signs[n]}")
                _nfw(r, "page 1", lambda x: x["page"] == 1 and x["y"] == 0)
                return "1 > 1 > 2 > 1"
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n} {v}" for n, v in got.items()))
        with o.step("F5 to the last chapter (191), the MFK to 192, on from 192: still 192") as s:
            def one(n, r):
                for _ in range(20):
                    if _nf(r)["page"] == 191:
                        break
                    bench.tap(r, "F5")
                    time.sleep(0.5)
                _goto(r, 192, signs[n])
                r.send(f"knob mfk {signs[n]}")
                time.sleep(2)
                if _nf(r)["page"] != 192:
                    raise Refused(f"{n}: on from 192 went to {_nf(r)['page']}")
                return "191 > 192 > 192"
            got = H.in_parallel(o, one, wait=90)
            s.confirm("; ".join(f"{n} {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "the MFK turns NIFOG's pages, from their top, between 1 and 192, on both", {}


def t_vfo(o):
    """The VFO on page 68: 24 px a click to the page's end (MORE BELOW, END OF PAGE), then page 69's top; back: page
    68's end. The dial and F's choice unmoved"""
    try:
        with o.step("both on F, NIFOG open on page 68") as s:
            signs = _choose(o)
            _open(o)
            H.in_parallel(o, lambda n, r: _goto(r, 68, signs[n]), wait=90)
            a0 = {n: {k: bench._value(r, k) for k in ("freq", "mode")} for n, r in o.r.items()}
            s.confirm(f"page 68; the dials {a0}")
        with o.step("a click: 24 px down, MORE BELOW; on to the end: END OF PAGE; one more: page 69's top") as s:
            def one(n, r):
                st = _nf(r)
                way = 1
                r.send("knob main 1")
                st1 = _nfw(r, "a click", lambda x: x["y"] != st["y"] or x["page"] != 68)
                if st1["page"] != 68:           # the remote's main knob the other way here: back past the top
                    way = -1
                    r.send(f"knob main {-way * 2}")
                    _goto(r, 68, signs[n])
                    r.send(f"knob main {way}")
                    st1 = _nfw(r, "a click", lambda x: x["page"] == 68 and x["y"] > 0)
                if st1["y"] != 24 or "MORE BELOW" not in st1["where"]:
                    raise Refused(f"{n}: one click: y {st1['y']}, '{st1['where']}'")
                for _ in range(40):
                    s2 = _nf(r)
                    if s2["y"] >= s2["maxy"]:
                        break
                    r.send(f"knob main {way * 3}")
                    time.sleep(0.25)
                s2 = _nfw(r, "the page's end", lambda x: x["page"] == 68 and x["y"] == x["maxy"])
                if "END OF PAGE" not in s2["where"]:
                    raise Refused(f"{n}: at the end: '{s2['where']}'")
                r.send(f"knob main {way}")
                _nfw(r, "page 69's top", lambda x: x["page"] == 69 and x["y"] == 0)
                r.send(f"knob main {-way}")
                s4 = _nfw(r, "back: page 68's end", lambda x: x["page"] == 68 and x["y"] == x["maxy"])
                return f"y 24 (MORE BELOW), the end at {s2['maxy']} (END OF PAGE), 69 at 0, back 68 at {s4['y']}" + \
                    (" (the remote's main knob the other way)" if way < 0 else "")
            got = H.in_parallel(o, one, wait=120)
            H.alike(o, ("freq", "mode"))
            for n, r in o.r.items():
                a = {k: bench._value(r, k) for k in ("freq", "mode")}
                if a != a0[n]:
                    raise Refused(f"{n}: the dial moved with NIFOG's VFO: {a0[n]} -> {a}")
                if _gstate(r.host).get("s.sys") != SYS_NIFOG:
                    raise Refused(f"{n}: F's choice moved")
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()) + "; the dials and F's choice unmoved")
    finally:
        _finish(o)
    return True, "the VFO scrolls NIFOG's page 24 px a click, over its ends onto the next and the previous; no tuning", {}


def t_sections(o):
    """F1 and F3 by section from page 40: F1 35 (its section's start), F1 34, F3 35, F3 45"""
    try:
        with o.step("both on F, NIFOG open on page 40") as s:
            signs = _choose(o)
            _open(o)
            H.in_parallel(o, lambda n, r: _goto(r, 40, signs[n]), wait=90)
            s.confirm("page 40")
        with o.step("F1 35, F1 34, F3 35, F3 45") as s:
            def one(n, r):
                seen = []
                for key, want in (("F1", 35), ("F1", 34), ("F3", 35), ("F3", 45)):
                    _key_to(r, key, f"{key}: page {want}", lambda x, want=want: x["page"] == want and x["y"] == 0)
                    seen.append(f"{key} {want}")
                return ", ".join(seen)
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "F1 and F3 go by NIFOG's sections as its contents have them, on both", {}


def t_contents(o):
    """F4 from page 68: the contents, page 4 (iii), at its top"""
    try:
        with o.step("both on F, NIFOG open on page 68") as s:
            signs = _choose(o)
            _open(o)
            H.in_parallel(o, lambda n, r: _goto(r, 68, signs[n]), wait=90)
            s.confirm("page 68")
        with o.step("F4: page 4 at its top, the caption TABLE OF CONTENTS") as s:
            got = H.in_parallel(o, lambda n, r: _key_to(r, "F4", "F4: page 4",
                                                         lambda x: x["page"] == 4 and x["y"] == 0)["cap"])
            s.confirm("; ".join(f"{n}: {v!r}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "F4 opens NIFOG's contents, on both", {}


def t_chapter(o):
    """F5 from page 4: 8 (HOW TO USE ...); from 191 (the last chapter): the cover"""
    try:
        with o.step("both on F, NIFOG open on page 4") as s:
            signs = _choose(o)
            _open(o)
            H.in_parallel(o, lambda n, r: _goto(r, 4, signs[n]), wait=90)
            s.confirm("page 4")
        with o.step("F5: 8; at 191 F5: 1") as s:
            def one(n, r):
                _key_to(r, "F5", "F5: page 8", lambda x: x["page"] == 8)
                _goto(r, 191, signs[n])
                _key_to(r, "F5", "F5 after the last chapter: page 1", lambda x: x["page"] == 1)
                return "4 > 8; 191 > 1"
            got = H.in_parallel(o, one, wait=150)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "F5 goes by NIFOG's chapters and from the last to the cover, on both", {}


LEGEND = "VFO: SCROLL | MFK: PAGE, PUSH CLOSE | F1 F3: SECTION, F4: CONTENTS | "
KEYS = ("SECTION BACK", "CLOSE", "SECTION NEXT", "CONTENTS", "CHAPTER NEXT")


def t_legend_keys(o):
    """While open: the legend and the F keys' boxes as NIFOG's; closed: F's own (F2 OPEN)"""
    try:
        with o.step("both on F, NIFOG open") as s:
            _choose(o)
            _open(o)
            s.confirm("open")
        with o.step("the legend and the F keys while open, F2 OPEN when closed") as s:
            def one(n, r):
                st = _nfw(r, "NIFOG's legend", lambda x: x["legend"].startswith(LEGEND))
                if not st["legend"].endswith("| GEN: HELP") or not re.search(r"\| (RING: SCREEN|MFK\+VFO: PAGE) \|",
                                                                              st["legend"]):
                    raise Refused(f"{n}: the legend {st['legend']!r}")
                bad = [f"F{i + 1} {k!r}" for i, k in enumerate(st["keys"]) if _words(KEYS[i]) not in _words(k)]
                if bad:
                    raise Refused(f"{n}: the F keys {st['keys']}")
                bench.tap(r, "F2")
                _nfw(r, "closed", lambda x: not x["open"])
                time.sleep(1)
                return f"{st['legend']!r}; keys {st['keys']}"
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "NIFOG's legend and F keys while it is open, on both", {}


def t_url(o):
    """The line under the page: THE PDF: HTTP://<HOSTNAME>.LOCAL/NIFOG OR HTTP://<ITS ADDRESS>/NIFOG"""
    try:
        with o.step("both on F, NIFOG open") as s:
            _choose(o)
            _open(o)
            s.confirm("open")
        with o.step("the line under the page names the radio's own name and address") as s:
            def one(n, r):
                name = bench.ssh(r.host, "hostname").strip()
                addr = bench.ssh(r.host, "ip -4 -o addr show 2>/dev/null").split("\n")
                ips = [ln.split()[3].split("/")[0] for ln in addr if len(ln.split()) > 3 and
                       re.match(r"\d+: (wlan|eth)", ln)]
                want = f"THE PDF: HTTP://{name}.LOCAL/NIFOG" + (f" OR HTTP://{ips[0]}/NIFOG" if ips else "")
                st = _nf(r)
                if want.upper() not in st["where"] and not _same_text(st["where"].split("|  ", 1)[-1], want.upper()):
                    raise Refused(f"{n}: {st['where']!r}, not {want.upper()!r}")
                return st["where"]
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n}: {v!r}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "the line under NIFOG's page names the radio's .local name and its address, on both", {}


def t_reopen(o):
    """Closed on page 68 at 48 px down, opened again there, drawn"""
    try:
        with o.step("both on F, NIFOG on page 68, 2 VFO clicks down") as s:
            signs = _choose(o)
            _open(o)

            def one(n, r):
                _goto(r, 68, signs[n])
                r.send("knob main 2")
                st = _nfw(r, "scrolled", lambda x: x["page"] == 68 and x["y"] > 0)
                return st["y"]
            ys = H.in_parallel(o, one, wait=90)
            s.confirm(f"y {ys}")
        with o.step("F2 closes, F2 opens: page 68 at the same y, drawn") as s:
            def one(n, r):
                bench.tap(r, "F2")
                _nfw(r, "closed", lambda x: not x["open"])
                bench.tap(r, "F2")
                st = _nfw(r, "open again", lambda x: x["open"])
                if (st["page"], st["y"]) != (68, ys[n]):
                    raise Refused(f"{n}: opened again on {st['page']} at {st['y']}, not 68 at {ys[n]}")
                ink = _window(_shot(r, "nifog-reopen"))[0]
                if ink < 3000:
                    raise Refused(f"{n}: not drawn when opened again: {ink} px of print")
                return f"68 at {st['y']}, {ink} px of print"
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "NIFOG opens again where it was left, drawn, on both", {}


def t_sentry_closed(o):
    """NIFOG open, the Sentry closed by an app (AIRSPACE, 'open sentry H') and back ('close'): F's list, NIFOG closed,
    and it opens and draws again"""
    try:
        with o.step("both on F, NIFOG open") as s:
            _choose(o)
            _open(o)
            s.confirm("open")
        with o.step("AIRSPACE opened over it and closed: the Sentry back, F's list, NIFOG closed") as s:
            def one(n, r):
                r.send("open sentry H")
                r.wait("AIRSPACE", lambda: r.state.get("pg") == "H" or r.state.get("win") not in ("Sentry", None), 10)
                time.sleep(1.5)
                r.send("close")
                r.wait("the Sentry back", lambda: r.state.get("win") == "Sentry" and r.state.get("pg") in tuple("ABCDEFG"),
                       15)
                return r.state.get("pg")
            back = H.in_parallel(o, one)
            o.both_page("F")
            st = {n: _nfw(r, "closed on F", lambda x: not x["open"]) for n, r in o.r.items()}
            g = {n: _gwait(r.host, "s.nf 0 (the GUI's state, written within 2 s)", lambda x: x.get("s.nf") == 0)
                 .get("s.nf") for n, r in o.r.items()}
            s.confirm(f"back on {back}; F: open {[x['open'] for x in st.values()]}, s.nf {g}")
        with o.step("F2 opens it again, drawn") as s:
            def one(n, r):
                if _gstate(r.host).get("s.sys") != SYS_NIFOG:
                    raise Refused(f"{n}: F's choice not NIFOG after the Sentry came back")
                bench.tap(r, "F2")
                st = _nfw(r, "open", lambda x: x["open"])
                ink = _window(_shot(r, "nifog-after-close"))[0]
                if ink < 3000:
                    raise Refused(f"{n}: not drawn: {ink} px of print")
                return f"page {st['page']}, {ink} px of print"
            got = H.in_parallel(o, one)
            s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    finally:
        _finish(o)
    return True, "the Sentry closed under NIFOG comes back on F's list, NIFOG opens and draws again, on both", {}


PNG_CHECK = r"""
import glob, struct, sys
bad, n, total = [], 0, 0
for p in sorted(glob.glob('/usr/share/x6100/nifog/p[0-9][0-9][0-9].png')):
    d = open(p, 'rb').read()
    n += 1; total += len(d)
    w, h, depth, colour = struct.unpack('>IIBB', d[16:26])
    if d[:8] != b'\x89PNG\r\n\x1a\n' or depth != 4 or colour != 0 or w > 780 or w % 2 or h < 100:
        bad.append((p, w, h, depth, colour))
print(n, total, bad[:3])
"""


def t_files(o):
    """/usr/share/x6100/nifog on both: CISA's PDF (its SHA-256), nifog.idx as the repository's, 192 4-bit grey PNGs at
    most 780 px wide"""
    with open(os.path.join(TOP, "tools", "nifog", "nifog.idx"), "rb") as f:
        idx_sha = hashlib.sha256(f.read()).hexdigest()
    with o.step("the files on both radios") as s:
        def one(n, r):
            sums = bench.ssh(r.host, "cd /usr/share/x6100/nifog && sha256sum nifog.pdf nifog.idx")
            got = dict(reversed(ln.split()) for ln in sums.splitlines() if ln.strip())
            if got.get("nifog.pdf") != PDF_SHA:
                raise Refused(f"{n}: nifog.pdf {got.get('nifog.pdf')}, not CISA's {PDF_SHA}")
            if got.get("nifog.idx") != idx_sha:
                raise Refused(f"{n}: nifog.idx not the repository's")
            out = bench.ssh(r.host, "python3 -", input=PNG_CHECK).strip()
            m = re.match(r"(\d+) (\d+) \[(.*)\]", out)
            if not m or int(m.group(1)) != IDX["pages"] or m.group(3):
                raise Refused(f"{n}: the pages: {out}")
            return f"PDF CISA's, idx the repository's, {m.group(1)} PNGs, {int(m.group(2)) / 1e6:.1f} MB"
        got = H.in_parallel(o, one, wait=90)
        s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    return True, "NIFOG's files on both radios as the build makes them", got


def t_web_pdf(o):
    """/nifog.pdf inline (application/pdf, no attachment), ?dl=1 saved as NIFOG_2.02.pdf, a Range: 206"""
    ver = IDX["edition"].split(",")[0].strip()
    with o.step("/nifog.pdf on both") as s:
        def one(n, r):
            st, hd, pdf = _http(r.host, "/nifog.pdf", timeout=180)
            if st != 200 or not hd.get("Content-Type", "").startswith("application/pdf") or \
                    hashlib.sha256(pdf).hexdigest() != PDF_SHA or "attachment" in hd.get("Content-Disposition", ""):
                raise Refused(f"{n}: /nifog.pdf {st} {hd.get('Content-Type')} {hd.get('Content-Disposition')}")
            st, hd, _ = _http(r.host, "/nifog.pdf?dl=1", timeout=180)
            cd = hd.get("Content-Disposition", "")
            if "attachment" not in cd or f"NIFOG_{ver}.pdf" not in cd:
                raise Refused(f"{n}: ?dl=1: {cd!r}")
            st, _, part = _http(r.host, "/nifog.pdf", {"Range": "bytes=1000-1999"})
            if st != 206 or part != pdf[1000:2000]:
                raise Refused(f"{n}: Range: {st}, {len(part)} bytes")
            return f"inline, CISA's SHA-256; saved as NIFOG_{ver}.pdf; Range 206"
        got = H.in_parallel(o, one, wait=400)
        s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    return True, "/nifog.pdf inline, saved and in a Range, on both", {}


def t_web_pages(o):
    """/nifog/page/1..192: the radio's own PNGs byte for byte; 0 and 193: 404"""
    with o.step("every page's PNG over HTTP against the radio's file") as s:
        def one(n, r):
            sums = {}
            for ln in bench.ssh(r.host, "cd /usr/share/x6100/nifog && sha256sum p*.png").splitlines():
                sha, name = ln.split()
                sums[int(name[1:4])] = sha
            bad = []
            for p in range(1, IDX["pages"] + 1):
                st, hd, body = _http(r.host, f"/nifog/page/{p}")
                if st != 200 or hd.get("Content-Type") != "image/png" or hashlib.sha256(body).hexdigest() != sums.get(p):
                    bad.append(p)
            for p in (0, IDX["pages"] + 1):
                try:
                    st = _http(r.host, f"/nifog/page/{p}")[0]
                except urllib.error.HTTPError as e:
                    st = e.code
                if st != 404:
                    bad.append(f"{p}:{st}")
            if bad:
                raise Refused(f"{n}: pages not as the radio's files: {bad[:10]}")
            return f"{IDX['pages']} pages as the radio's files, 0 and {IDX['pages'] + 1} 404"
        got = H.in_parallel(o, one, wait=600)
        s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    return True, "every NIFOG page served as the radio's file, on both", {}


def t_web_viewer(o):
    """/nifog in headless Chromium: #p=68 shows page 68 and its title; Next 69, ArrowLeft 68; the section list's CTCSS
    entry; the contents' VHF Inland - VTAC17 link 38; Open the PDF and Save it"""
    from playwright.sync_api import sync_playwright
    with o.step("the viewer on each radio's /nifog") as s:
        got = {}
        for n, r in o.r.items():
            pw = sync_playwright().start()
            br = pw.chromium.launch(headless=True)
            try:
                page = br.new_page()
                errs = []
                page.on("pageerror", lambda e: errs.append(str(e)))
                page.goto(f"http://{r.host}/nifog#p=68", wait_until="load", timeout=30000)

                def shown(p):
                    page.wait_for_function(f"document.getElementById('vimg').src.endsWith('/nifog/page/{p}') && "
                                           "document.getElementById('vimg').complete && "
                                           "document.getElementById('vimg').naturalWidth > 0", timeout=15000)
                    return page.inner_text("#vt")
                t68 = shown(68)
                if t68 != f"Page {IDX['labels'][68]} (68 of {IDX['pages']})":
                    raise Refused(f"{n}: the title on 68: {t68!r}")
                page.click("#next")
                shown(69)
                page.keyboard.press("ArrowLeft")
                shown(68)
                ctcss = next(i for i, x in enumerate(IDX["secs"]) if x["title"].startswith("Continuous Tone-Coded"))
                page.click("#next")
                shown(69)
                page.select_option("#vsel", str(ctcss))
                shown(68)
                page.click("#toc a:text-is('VHF Inland - VTAC17')")
                shown(38)
                pdf = page.get_attribute("a:has-text('Open the PDF')", "href")
                dl = page.get_attribute("a:has-text('Save it')", "download")
                if pdf != "/nifog.pdf" or not (dl or "").startswith("NIFOG_"):
                    raise Refused(f"{n}: the links: {pdf!r}, {dl!r}")
                if errs:
                    raise Refused(f"{n}: the page's script: {errs[:2]}")
                got[n] = f"{t68}; Next, ArrowLeft, the section list, the contents' link; PDF {pdf}, saved as {dl}"
            finally:
                br.close()
                pw.stop()
        s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    return True, "the /nifog viewer turns pages by its buttons, keys, list and contents, on both", {}


def t_web_menu(o):
    """The web configurator's menu has NIFOG under Frequencies, General a NIFOG tile"""
    with o.step("/ and /nifog on both") as s:
        def one(n, r):
            st, _, home = _http(r.host, "/")
            st2, _, nifog = _http(r.host, "/nifog")
            menu = re.search(rb"Frequencies<svg.*?</div>\s*</div>", nifog, re.S)
            tile = b'<a class="tile" href="/nifog"' in home
            link = bool(menu) and b'href="/nifog"' in menu.group(0)
            if not tile or not link:
                raise Refused(f"{n}: General's tile {tile}, the menu's link under Frequencies {link}")
            if b'aria-current="page"' not in menu.group(0):
                raise Refused(f"{n}: /nifog's menu entry not marked as the page shown")
            return "the tile on General, the link under Frequencies (current on /nifog)"
        got = H.in_parallel(o, one)
        s.confirm("; ".join(f"{n}: {v}" for n, v in got.items()))
    return True, "NIFOG in the web menu and on General, on both", {}


COVERS = {
    "nifog": (["f-system-nifog", "web-nifog-page"], []),
    "nifog-list": (["f-nifog-list"], []),
    "nifog-open-close": (["f-nifog-open-close"], []),
    "nifog-drawn": (["f-nifog-drawn"], []),
    "nifog-caption": (["f-nifog-caption"], []),
    "nifog-mfk": (["f-nifog-mfk"], []),
    "nifog-vfo": (["f-nifog-vfo"], []),
    "nifog-sections": (["f-nifog-sections"], []),
    "nifog-contents": (["f-nifog-contents"], []),
    "nifog-chapter": (["f-nifog-chapter"], []),
    "nifog-legend-keys": (["f-nifog-legend-keys"], []),
    "nifog-url": (["f-nifog-url"], []),
    "nifog-reopen": (["f-nifog-reopen"], []),
    "nifog-sentry-closed": (["f-nifog-sentry-closed"], []),
    "nifog-files": (["f-nifog-files"], []),
    "web-nifog-pdf": (["web-nifog-pdf"], []),
    "web-nifog-pages": (["web-nifog-pages"], []),
    "web-nifog-viewer": (["web-nifog-viewer"], []),
    "web-nifog-menu": (["web-nifog-menu"], []),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    rows = [
        ("nifog", t_nifog, 90, "F > NIFOG on both: F2 opens it, the MFK, F3 and the VFO move through its pages, F2 "
         "closes; /nifog, CISA's PDF and a page's PNG served"),
        ("nifog-list", t_list, 30, "F's list: NIFOG its row 11 (s.sys, fsel)"),
        ("nifog-open-close", t_open_close, 60, "F2 and the push open and close NIFOG, page A closes it"),
        ("nifog-drawn", t_drawn, 120, "pages 30 and 68 drawn after G: centred, across, the theme's colours; the log"),
        ("nifog-caption", t_caption, 120, "the caption on pages 1, 4, 38, 68 as the contents say"),
        ("nifog-mfk", t_mfk, 120, "the MFK a page on and back, between 1 and 192"),
        ("nifog-vfo", t_vfo, 120, "the VFO 24 px a click, over the page's ends; no tuning"),
        ("nifog-sections", t_sections, 90, "F1 and F3 by section from page 40"),
        ("nifog-contents", t_contents, 90, "F4 the contents from page 68"),
        ("nifog-chapter", t_chapter, 150, "F5 by chapter, from the last to the cover"),
        ("nifog-legend-keys", t_legend_keys, 40, "NIFOG's legend and F keys while open"),
        ("nifog-url", t_url, 40, "the line under the page: the radio's name and address"),
        ("nifog-reopen", t_reopen, 90, "opened again on the page and scroll left, drawn"),
        ("nifog-sentry-closed", t_sentry_closed, 60, "the Sentry closed under NIFOG: F's list again, NIFOG draws"),
        ("nifog-files", t_files, 60, "/usr/share/x6100/nifog: the PDF, the contents, 192 PNGs"),
        ("web-nifog-pdf", t_web_pdf, 120, "/nifog.pdf inline, saved, a Range"),
        ("web-nifog-pages", t_web_pages, 300, "every page's PNG as the radio's file; 404 past the ends"),
        ("web-nifog-viewer", t_web_viewer, 90, "the /nifog viewer in headless Chromium"),
        ("web-nifog-menu", t_web_menu, 20, "NIFOG in the web menu and on General"),
    ]
    return [Test(name, "both RX", what, fn, est) for name, fn, est, what in rows]
