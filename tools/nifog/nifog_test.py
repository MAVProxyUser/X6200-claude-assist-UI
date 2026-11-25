#!/usr/bin/env python3
"""Host test of CISA's NIFOG on the radio (the Sentry's F > NIFOG, the web configurator's /nifog). See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/nifog/nifog_test.py [-v]

1. The committed index (tools/nifog/nifog.idx, render.py --index): the edition, 192 pages, every page's printed
   number in order, the contents in page order with the chapters and the sections F's keys go by (the
   interoperability channels, CTCSS) where the guide has them.
2. src/nifog.c reads it as the web page does (nifog_web.read_index): the same pages, numbers and entries; F1/F3 (by
   section), F5 (by chapter) and the caption's section as the reader's comments say.
3. render.py's page (a made-up grey page, no pdftoppm): cut to its print, 4 bits, 16 greys, rows unfiltered, and read
   back as written; the box of a kind of page at most 780 px wide and even.
4. Through bottle's WSGI app (skipped without BOTTLE): /nifog with its contents, /nifog.pdf inline and saved (a Range
   answered with 206), /nifog/page/N and a 404 past the last page, the page in the menu and on General; none of it
   with a radio that has no NIFOG.
Its files go in build/ (the Makefile empties it first, and builds build/nifog_idx).
"""
import importlib.util
import os
import struct
import subprocess
import sys
import types
import unittest
import zlib
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
IDX = os.path.join(HERE, "nifog.idx")
BUILD = os.path.join(HERE, "build")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
try:
    import cgi  # noqa: F401
except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
    cgi = types.ModuleType("cgi")
    cgi.FieldStorage = object
    sys.modules["cgi"] = cgi
from x6100_webserver import nifog_web  # noqa: E402  (no bottle in it)
try:
    import bottle  # noqa: F401
    from x6100_webserver import apps, settings  # noqa: E402
except ImportError:
    bottle = apps = settings = None

spec = importlib.util.spec_from_file_location("render", os.path.join(HERE, "render.py"))
render = importlib.util.module_from_spec(spec)
spec.loader.exec_module(render)


class CommittedIndex(unittest.TestCase):
    def setUp(self):
        self.idx = nifog_web.read_index(HERE)       # nifog.idx beside this file; no PDF here

    def test_edition_and_pages(self):
        self.assertEqual(self.idx["edition"], "2.02, January 2025")
        self.assertEqual(self.idx["pages"], 192)
        labels = self.idx["labels"]
        self.assertEqual(labels[2], "i")
        self.assertEqual([labels[p] for p in range(2, 8)], ["i", "ii", "iii", "iv", "v", "vi"])
        self.assertEqual([labels[p] for p in range(8, 193)], [str(n) for n in range(1, 186)])
        self.assertNotIn(1, labels)                 # the cover

    def test_contents(self):
        secs = self.idx["secs"]
        self.assertEqual([s["page"] for s in secs], sorted(s["page"] for s in secs))
        by = {s["title"]: s for s in secs}
        self.assertEqual(by["Table of Contents"]["page"], 4)
        self.assertEqual((by["INTEROPERABILITY CHANNELS"]["page"], by["INTEROPERABILITY CHANNELS"]["level"]), (30, 0))
        self.assertEqual(by["VHF Inland - VTAC17"]["page"], 38)        # printed 31
        self.assertEqual(by["VHF Inland - VTAC17"]["level"], 2)
        ctcss = [s for s in secs if s["title"].startswith("Continuous Tone-Coded Squelch System")]
        self.assertEqual([(s["page"], s["level"]) for s in ctcss], [(68, 1)])
        self.assertEqual(by["SHARES HF"]["level"], 1)                   # in capitals, not a chapter
        self.assertEqual(by["HOW TO USE THE NATIONAL INTEROPERABILITY FIELD OPERATIONS GUIDE"]["page"], 8)
        chapters = [s["title"] for s in secs if s["level"] == 0]
        self.assertEqual(len(chapters), 15)
        for s in secs:
            self.assertTrue(s["title"].isascii() and s["title"] == s["title"].strip(), s)


class GuiReadsAsTheWeb(unittest.TestCase):
    """build/nifog_idx: src/nifog.c's parse of the same file"""

    @classmethod
    def setUpClass(cls):
        out = subprocess.run([os.path.join(BUILD, "nifog_idx"), IDX], capture_output=True, text=True, check=True)
        cls.lines = out.stdout.splitlines()
        cls.idx = nifog_web.read_index(HERE)

    def field(self, name):
        return next(x for x in self.lines if x.startswith(name + " "))[len(name) + 1:]

    def test_same_index(self):
        self.assertEqual(self.field("parsed"), "1")
        self.assertEqual(int(self.field("pages")), self.idx["pages"])
        self.assertEqual(self.field("edition"), self.idx["edition"])
        self.assertEqual(int(self.field("toc")), 4)
        secs = [x.split(" ", 3) for x in self.lines if x.startswith("sec ")]
        self.assertEqual([(int(p), int(lv), t) for _, p, lv, t in secs],
                         [(s["page"], s["level"], s["title"]) for s in self.idx["secs"]])

    def test_pages_and_keys(self):
        secs = self.idx["secs"]
        starts = sorted({s["page"] for s in secs if s["level"] <= 1})
        chapters = sorted({s["page"] for s in secs if s["level"] == 0})
        for x in (x for x in self.lines if x.startswith("page ")):
            parts = dict(kv.split("=", 1) for kv in x.split()[2:])
            p = int(x.split()[1])
            self.assertEqual(parts["label"], self.idx["labels"].get(p, ""))
            # the caption's section: the last entry of level 0 or 1 starting here or before
            want = max((i for i, s in enumerate(secs) if s["page"] <= p and s["level"] <= 1), default=-1)
            self.assertEqual(int(parts["sec"]), want, x)
            # F1: the section's start, else the one before; F3: the next one; F5: the next chapter (itself at the end)
            self.assertEqual(int(parts["back"]), max([s for s in starts if s < p], default=p), x)
            self.assertEqual(int(parts["next"]), min([s for s in starts if s > p], default=p), x)
            self.assertEqual(int(parts["chapter"]), min([s for s in chapters if s > p], default=p), x)


def png_rows(path):
    """A grey PNG of render.py's: (w, h, bits, rows of levels)"""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        assert struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0] == zlib.crc32(kind + body) & 0xffffffff
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, bits, colour = ihdr[:4]
    raw = zlib.decompress(idat)
    rb = (w * bits + 7) // 8
    rows = []
    for y in range(h):
        line = raw[y * (rb + 1):(y + 1) * (rb + 1)]
        assert line[0] == 0                         # no filter
        rows.append([v for b in line[1:] for v in (b >> 4, b & 15)][:w])
    return w, h, bits, colour, rows


class RenderedPage(unittest.TestCase):
    def test_a_page(self):
        w, h = 40, 30
        px = bytearray([255] * (w * h))             # paper, print from (5,4) to (30,20): one black, one grey row
        for x in range(5, 31):
            px[4 * w + x] = 0
            px[20 * w + x] = 136                    # 136/255*15 = 8
        box = render.boxes_by_kind([(w, h, render.printed_box(w, h, bytes(px)))])[(w, h)]
        self.assertEqual(box[1], 4 - render.PAD)
        self.assertEqual((box[2] - box[0]) % 2, 0)
        path = os.path.join(BUILD, "p001.png")
        render.png4(path, w, h, bytes(px), box)
        pw, ph, bits, colour, rows = png_rows(path)
        self.assertEqual((pw, ph, bits, colour), (box[2] - box[0], box[3] - box[1], 4, 0))
        self.assertEqual(rows[4 - box[1]][5 - box[0]], 0)
        self.assertEqual(rows[20 - box[1]][10 - box[0]], 8)
        self.assertEqual(rows[0][0], 15)

    def test_wide_pages_cut_to_the_frame(self):
        w, h = 900, 50
        b = render.boxes_by_kind([(w, h, (10, 5, 890, 45))])[(w, h)]
        self.assertLessEqual(b[2] - b[0], render.MAX_W)
        self.assertEqual((b[2] - b[0]) % 2, 0)


def call(path, headers=None):
    env = {"REQUEST_METHOD": "GET", "PATH_INFO": path.split("?")[0], "HTTP_HOST": "192.168.1.10",
           "QUERY_STRING": path.split("?", 1)[1] if "?" in path else ""}
    env.update(headers or {})
    setup_testing_defaults(env)
    got = {}

    def start(status, hdrs, exc=None):
        got["status"] = int(status.split()[0])
        got["headers"] = dict(hdrs)

    out = apps.app(env, start)
    body = b"".join(out)
    if hasattr(out, "close"):
        out.close()
    return got["status"], body, got["headers"]


@unittest.skipIf(apps is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class WebPages(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.join(BUILD, "share%d" % id(self))
        os.makedirs(self.dir, exist_ok=True)
        self.addCleanup(setattr, settings, "NIFOG_DIR", settings.NIFOG_DIR)
        settings.NIFOG_DIR = self.dir

    def fill(self):
        with open(IDX) as f, open(os.path.join(self.dir, "nifog.idx"), "w") as g:
            g.write(f.read())
        self.pdf = b"%PDF-1.7\n" + bytes(range(256)) * 40 + b"\n%%EOF\n"
        with open(os.path.join(self.dir, "nifog.pdf"), "wb") as f:
            f.write(self.pdf)
        render.png4(os.path.join(self.dir, "p001.png"), 4, 2, bytes([0, 255, 0, 255, 255, 0, 255, 0]), (0, 0, 4, 2))

    def test_the_page(self):
        self.fill()
        st, body, _ = call("/nifog")
        self.assertEqual(st, 200)
        page = body.decode()
        self.assertIn('href="/nifog.pdf"', page)
        self.assertIn("version 2.02, January 2025", page)
        self.assertIn("VHF Inland - VTAC17", page)      # the contents, as JSON for its script
        self.assertNotIn("</script><", page.split("const IDX = ")[1].split(";")[0])
        self.assertIn('<a href="/nifog"', page)          # in the menu
        st, body, _ = call("/")
        self.assertIn(b'<a class="tile" href="/nifog"', body)

    def test_the_pdf(self):
        self.fill()
        st, body, h = call("/nifog.pdf")
        self.assertEqual((st, body), (200, self.pdf))
        self.assertTrue(h["Content-Type"].startswith("application/pdf"))
        self.assertNotIn("Content-Disposition", h)
        st, body, h = call("/nifog.pdf?dl=1")
        self.assertIn('filename="NIFOG_2.02.pdf"', h["Content-Disposition"])
        st, body, h = call("/nifog.pdf", {"HTTP_RANGE": "bytes=100-199"})
        self.assertEqual((st, body), (206, self.pdf[100:200]))

    def test_the_pages(self):
        self.fill()
        st, body, h = call("/nifog/page/1")
        self.assertEqual(st, 200)
        self.assertEqual(h["Content-Type"], "image/png")
        self.assertEqual(body[:8], b"\x89PNG\r\n\x1a\n")
        for n in (0, 2, 193, -1):
            self.assertEqual(call("/nifog/page/%d" % n)[0], 404, n)

    def test_none_on_the_radio(self):
        st, body, _ = call("/nifog")
        self.assertEqual(st, 200)
        self.assertIn(b"Not on this radio", body)
        self.assertEqual(call("/nifog.pdf")[0], 404)
        self.assertEqual(call("/nifog/page/1")[0], 404)


if __name__ == "__main__":
    os.makedirs(BUILD, exist_ok=True)
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
