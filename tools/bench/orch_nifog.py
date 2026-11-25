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
import urllib.request

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_nifog.H = sys.modules[__name__])

TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(TOP, "webserver", "src"))
from x6100_webserver import nifog_web  # noqa: E402  (no bottle in it)

IDX = nifog_web.read_index(os.path.join(TOP, "tools", "nifog"))
SYS_NIFOG, SYS_N = 11, 13       # app_sentry.c sys_items[]: NIFOG after TRIBAL KNOWLEDGE, START IN last
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


def _http(host, path, headers=None, timeout=60):
    req = urllib.request.Request(f"http://{host}{path}", headers=headers or {})
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


COVERS = {
    "nifog": (["f-system-nifog", "web-nifog-page"], []),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("nifog", "both RX", "F > NIFOG on both: F2 opens it, the MFK, F3 and the VFO move through its pages, F2 "
             "closes; /nifog, CISA's PDF and a page's PNG served", t_nifog, 90),
    ]
