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


def _http(host, path, headers=None, timeout=60):
    req = urllib.request.Request(f"http://{host}{path}", headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, dict(resp.headers), resp.read()


def t_nifog(o):
    """F SYSTEM > NIFOG on both radios: chosen on F's list and opened with F2 (a page open); the MFK turns a page, F3
    goes to the next section's page as the contents have it, the VFO scrolls down the page 24 px a click (the dial
    unmoved), F2 closes it (F's choice still NIFOG); a picture of it open. Then the web page: /nifog with the contents,
    /nifog.pdf whole (CISA's bytes, its SHA-256) and in a Range (206), /nifog/page/68 a PNG. Both end on page A"""
    if IDX["pages"] != 192:
        raise Refused("tools/nifog/nifog.idx: no 192 pages")
    a0 = {n: {k: bench._value(r, k) for k in ("freq", "mode")} for n, r in o.r.items()}
    with o.step("both on the Sentry's page F, F's choice NIFOG") as s:
        o.both_page("F")

        def choose(n, r):
            h = r.host
            g = _gstate(h)
            if "s.sys" not in g:
                raise Refused(f"{n}: no s.sys in its GUI's state")
            if g.get("s.nf"):
                raise Refused(f"{n}: NIFOG open already (s.nf {g['s.nf']})")
            d = (SYS_NIFOG - g["s.sys"]) % SYS_N
            d = d - SYS_N if d > SYS_N // 2 else d
            for _ in range(abs(d)):
                r.send(f"knob mfk {1 if d > 0 else -1}")
                time.sleep(0.15)
            return _gwait(h, "F's choice NIFOG", lambda g: g.get("s.sys") == SYS_NIFOG)["s.sys"]
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
    with o.step("the MFK turns a page, F3 to the next section, the VFO scrolls") as s:
        def keys(n, r):
            h, p0 = r.host, page[n]
            r.send("knob mfk 1")
            p1 = min(p0 + 1, IDX["pages"])
            _gwait(h, f"the MFK: page {p1}", lambda g: g.get("s.nf") == p1 and g.get("s.nfy") == 0)
            bench.tap(r, "F3")
            p2 = _next_section(p1)
            _gwait(h, f"F3: page {p2}", lambda g: g.get("s.nf") == p2 and g.get("s.nfy") == 0)
            for _ in range(2):
                r.send("knob vfo 1")
                time.sleep(0.2)
            g = _gwait(h, "the VFO: down the page (or on to the next)",
                       lambda g: (g.get("s.nf") == p2 and g.get("s.nfy") == 2 * NF_STEP) or g.get("s.nf") == p2 + 1)
            return f"page {p0} > MFK {p1} > F3 {p2} ({IDX['labels'].get(p2, '?')}) > VFO 2 clicks: page {g['s.nf']} " \
                   f"{g['s.nfy']} px down"
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
