#!/usr/bin/env python3
"""CISA's NIFOG (third-party/nifog) for the radio: its pages as pictures for the Sentry's F NIFOG, and its contents.

    render.py --pdf NIFOG.pdf --out DIR          the pages, DIR/p001.png ... (the GUI's build runs it: CMakeLists.txt)
    render.py --pdf NIFOG.pdf --index > FILE     the pages' printed numbers and the contents (tools/nifog/nifog.idx,
                                                 committed: made once per edition, read by the GUI and the web page)

The radio has no PDF renderer and the GUI none of its own (too heavy): poppler's pdftoppm draws every page here at
130 dpi in grey, each page cut to what its kind of page prints on (the 4.25 x 6.5 in pages and the 6.5 x 4.25 in
tables, each kind the same box, so a page does not jump against the next one), at most 780 px wide: the Sentry's
frame is 782 inside, the tables fill it and their 9 pt figures come out 16 px. 16 greys, 4 bits a pixel: 5.2 MB for
the 192 pages (2 bits: 3.0 MB, but the tables' shaded rows went white). The GUI shows them in the theme's colours.
Python's standard library only (zlib), as the build host has it; no PIL.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

DPI = 130
MAX_W = 780              # the Sentry's frame (app_sentry.c NF_W)
PAD = 3                  # px kept round what a page prints
INK = 231                # greyer than this is print (not paper)
LEVELS = 15              # 16 greys


def pgm(path):
    """A binary PGM (pdftoppm -gray): width, height, the rows' bytes"""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m or int(m.group(3)) != 255:
        sys.exit(f"{path}: not an 8-bit PGM")
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h]
    if len(px) != w * h:
        sys.exit(f"{path}: short")
    return w, h, px


INK_MAP = bytes(0 if v < INK else 255 for v in range(256))


def printed_box(w, h, px):
    """Where the page prints: (left, top, right, bottom), right and bottom past the last; None for a blank page"""
    top = bottom = None
    left, right = w, 0
    for y in range(h):
        row = px[y * w:(y + 1) * w].translate(INK_MAP)
        a = row.find(b"\0")
        if a < 0:
            continue
        top = y if top is None else top
        bottom = y + 1
        left = min(left, a)
        right = max(right, row.rfind(b"\0") + 1)
    return None if top is None else (left, top, right, bottom)


def boxes_by_kind(pages):
    """One box per page size: all its pages' print together (a full-bleed page, the cover, left out), padded,
    an even width (two pixels a byte), at most MAX_W (cut evenly both sides)"""
    union = {}
    for w, h, b in pages:
        if b is None or (b[0] == 0 and b[1] == 0) or b[2] == w:
            continue
        u = union.get((w, h))
        union[(w, h)] = b if u is None else (min(u[0], b[0]), min(u[1], b[1]), max(u[2], b[2]), max(u[3], b[3]))
    out = {}
    for (w, h), u in union.items():
        l, t, r, b = max(0, u[0] - PAD), max(0, u[1] - PAD), min(w, u[2] + PAD), min(h, u[3] + PAD)
        if r - l > MAX_W:
            cut = r - l - MAX_W
            l, r = l + cut // 2, r - (cut - cut // 2)
        if (r - l) % 2:
            if r - l < MAX_W and r < w:
                r += 1
            else:
                r -= 1
        out[(w, h)] = (l, t, r, b)
    return out


LEVEL_HI = bytes((round(v * LEVELS / 255) << 4) for v in range(256))
LEVEL_LO = bytes(round(v * LEVELS / 255) for v in range(256))


def png4(path, w, h, px, box):
    """The box of the page as a 4-bit grey PNG (no filter: the rows deflate as well as with PIL's best, 2026-11-25)"""
    l, t, r, b = box
    cw = r - l
    rows = b"".join(px[y * w + l:y * w + r] for y in range(t, b))
    hi = rows[0::2].translate(LEVEL_HI)
    lo = rows[1::2].translate(LEVEL_LO)
    packed = (int.from_bytes(hi, "big") | int.from_bytes(lo, "big")).to_bytes(len(hi), "big")
    rb = cw // 2
    raw = b"".join(b"\0" + packed[i:i + rb] for i in range(0, len(packed), rb))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    with open(path + ".new", "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw, b - t, 4, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    os.replace(path + ".new", path)


def render(pdf, out, pdftoppm):
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="nifog.")
    try:
        subprocess.run([pdftoppm, "-gray", "-r", str(DPI), pdf, os.path.join(tmp, "p")], check=True)
        names = sorted(n for n in os.listdir(tmp) if n.endswith(".pgm"))
        if not names:
            sys.exit("pdftoppm drew no page")
        pages = []
        for n in names:
            w, h, px = pgm(os.path.join(tmp, n))
            pages.append((w, h, printed_box(w, h, px)))
        boxes = boxes_by_kind(pages)
        for i, n in enumerate(names):
            w, h, px = pgm(os.path.join(tmp, n))
            box = boxes.get((w, h))
            if box is None:              # a size with only full-bleed pages: the middle MAX_W
                cw = min(w, MAX_W) // 2 * 2
                box = ((w - cw) // 2, 0, (w - cw) // 2 + cw, h)
            png4(os.path.join(out, "p%03d.png" % (i + 1)), w, h, px, box)
        for n in os.listdir(out):        # pages of a longer edition before
            m = re.fullmatch(r"p(\d{3})\.png", n)
            if m and int(m.group(1)) > len(names):
                os.remove(os.path.join(out, n))
        print(f"NIFOG: {len(names)} pages in {out}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---- the contents (--index) ----

ASCII = {"–": "-", "—": "-", "‘": "'", "’": "'", "“": '"', "”": '"', "•": "*",
         " ": " ", "™": "", "®": "", "§": "S", "°": " deg"}
ROMAN = re.compile(r"^(i|ii|iii|iv|v|vi|vii|viii|ix|x)$")


def ascii_only(s):
    s = "".join(ASCII.get(c, c) for c in s)
    return s.encode("ascii", "ignore").decode()


def page_text(pdf, pdftotext, n):
    return subprocess.run([pdftotext, "-layout", "-f", str(n), "-l", str(n), pdf, "-"], check=True,
                          capture_output=True, text=True).stdout


def index(pdf, pdftotext, pdfinfo, pdftohtml):
    info = subprocess.run([pdfinfo, pdf], check=True, capture_output=True, text=True).stdout
    n = int(re.search(r"^Pages:\s+(\d+)", info, re.M).group(1))
    labels, texts = {}, {}
    for p in range(1, n + 1):
        t = page_text(pdf, pdftotext, p)
        texts[p] = t
        lines = [x for x in t.splitlines() if x.strip()]
        last = lines[-1].split()[-1] if lines else ""
        if last.isdigit() or ROMAN.match(last):
            labels[p] = last
    first = {}                           # a printed number's page: the last page with it (the cover's "1" too)
    for p, lab in labels.items():
        first[lab] = p
    for p in list(labels):
        if first[labels[p]] != p:
            del labels[p]
    entry = re.compile(r"^(\s*)(.+?)\s*\.{3,}\s*(\d+|[ivx]+)\s*$")
    toc_pages = []                       # the page headed TABLE OF CONTENTS and those that go on with its entries
    for p in range(1, n + 1):
        entries = sum(1 for x in texts[p].splitlines() if entry.match(x))
        if (not toc_pages and "TABLE OF CONTENTS" in texts[p]) or (toc_pages and toc_pages[-1] == p - 1 and entries >= 3):
            toc_pages.append(p)
    # The chapters: the contents' entries in bold (pdftohtml marks them), by the page they link to. Capitals alone
    # did not tell: SHARES HF is a section
    bold = set()
    if toc_pages:
        xml = subprocess.run([pdftohtml, "-xml", "-i", "-stdout", "-f", str(toc_pages[0]), "-l", str(toc_pages[-1]),
                              pdf], check=True, capture_output=True, text=True).stdout
        bold = {int(m.group(1)) for m in re.finditer(r'<a href="[^"#]*#(\d+)"><b>[^<]*[A-Za-z]', xml)}
    secs = []
    if toc_pages:
        secs.append((toc_pages[0], 0, "Table of Contents"))
    for p in toc_pages:
        pending = ""                     # an entry's first line, its title going on on the next (HOW TO USE ...)
        for line in texts[p].splitlines():
            m = entry.match(line)
            if not m:
                s = line.strip()
                pending = s if s and not ROMAN.match(s) and not s.isdigit() and "TABLE OF CONTENTS" not in s else ""
                continue
            title = re.sub(r"\s+", " ", ascii_only(pending + " " + m.group(2))).strip().rstrip("*").strip()
            pending = ""
            to = first.get(m.group(3))
            if not to or not title:
                continue
            letters = re.sub(r"[^A-Za-z]", "", title)
            level = 2 if len(m.group(1)) >= 5 else 0 if to in bold and letters == letters.upper() else 1
            secs.append((to, level, title))
    secs.sort(key=lambda s: s[0])        # stable: the contents' own order within a page
    ver = re.search(r"Version\s+([\d.]+)", texts[1])
    when = re.search(r"\b(January|February|March|April|May|June|July|August|September|October|November|December)"
                     r"\s+(\d{4})\b", texts[1], re.I)
    edition = ", ".join(x for x in (ver.group(1) if ver else "",
                                    f"{when.group(1).capitalize()} {when.group(2)}" if when else "") if x)
    out = ["# CISA's NIFOG, its pages' printed numbers and its contents, for the Sentry's F NIFOG (src/nifog.c)",
           "# and the web page /nifog (webserver apps.py): tools/nifog/render.py --index, from the PDF's own contents.",
           "# pages N: the PDF's pages; page P LABEL: PDF page P is printed LABEL; sec P LEVEL TITLE: the contents'",
           "# entry on PDF page P (level 0 a chapter, 1 a section, 2 a part of one)",
           f"edition {edition}", f"pages {n}"]
    out += [f"page {p} {labels[p]}" for p in sorted(labels)]
    out += [f"sec {p} {lv} {t}" for p, lv, t in secs]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pdf", required=True)
    ap.add_argument("--out", help="the pages' folder")
    ap.add_argument("--index", action="store_true", help="the contents to stdout")
    ap.add_argument("--pdftoppm", default="pdftoppm")
    a = ap.parse_args()
    if a.index:
        d = os.path.dirname(shutil.which(a.pdftoppm) or "")
        tool = lambda n: os.path.join(d, n) if d else n      # poppler's others, beside its pdftoppm
        sys.stdout.write(index(a.pdf, tool("pdftotext"), tool("pdfinfo"), tool("pdftohtml")))
    elif a.out:
        render(a.pdf, a.out, a.pdftoppm)
    else:
        ap.error("--out or --index")


if __name__ == "__main__":
    main()
