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


class LoadFromDisk(unittest.TestCase):
    """build/nifog_load: src/nifog.c's nifog_load with its share folder build/share (the Makefile's BOARD_SHARE)"""

    def setUp(self):
        import shutil
        self.dir = os.path.join(BUILD, "share", "nifog")
        shutil.rmtree(os.path.join(BUILD, "share"), ignore_errors=True)
        os.makedirs(self.dir)

    def load(self):
        return subprocess.run([os.path.join(BUILD, "nifog_load")], capture_output=True, text=True, check=True).stdout

    def test_none_without_the_contents(self):
        self.assertIn("load 0 pages 0", self.load())

    def test_none_without_its_first_page(self):
        """The contents there but no page drawn (an image whose build had no pdftoppm): F says NOT ON THIS RADIO"""
        with open(IDX) as f, open(os.path.join(self.dir, "nifog.idx"), "w") as g:
            g.write(f.read())
        self.assertIn("load 0 pages 0", self.load())

    def test_loaded_with_both(self):
        with open(IDX) as f, open(os.path.join(self.dir, "nifog.idx"), "w") as g:
            g.write(f.read())
        open(os.path.join(self.dir, "p001.png"), "wb").close()
        out = self.load()
        self.assertIn("load 1 pages 192 edition 2.02, January 2025", out)
        self.assertIn("read " + os.path.join(os.path.abspath(BUILD), "share", "nifog", "p192.png"), out)
        self.assertIn("past none", out)

    def test_parse_guards(self):
        """A short or broken index: no pages is none; entries past the last page dropped; junk lines skipped"""
        path = os.path.join(BUILD, "broken.idx")
        for text, want in (("pages 0\n", "parsed 0"), ("# nothing\n", "parsed 0"),
                           ("pages 3\nsec 2 0 Two\nsec 9 1 Past the end\nnonsense line\npage 2 ii\n", "parsed 1")):
            with open(path, "w") as f:
                f.write(text)
            out = subprocess.run([os.path.join(BUILD, "nifog_idx"), path], capture_output=True, text=True,
                                 check=True).stdout
            self.assertIn(want, out, text)
        self.assertIn("sec 2 0 Two", out)
        self.assertNotIn("Past the end", out)
        self.assertIn("toc 1", out)                         # no Table of Contents entry: the first page
        self.assertIn("page 2 label=ii", out)


def poppler():
    import shutil
    return all(shutil.which(t) for t in ("pdftoppm", "pdftotext", "pdfinfo", "pdftohtml"))


PDF = os.path.join(TOP, "third-party", "nifog", "NIFOG_2.02.pdf")


@unittest.skipUnless(poppler(), "no poppler-utils here (the image build has them: main.yml's step)")
class FromThePdf(unittest.TestCase):
    """render.py on CISA's PDF itself: the committed contents are the PDF's, the pages as the image draws them"""

    def test_the_committed_index_is_the_pdfs(self):
        out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "render.py"), "--pdf", PDF, "--index"],
                             capture_output=True, text=True, check=True).stdout
        with open(IDX) as f:
            self.assertEqual(out, f.read(), "tools/nifog/nifog.idx is not render.py --index of the PDF")

    def test_the_pages(self):
        out = os.path.join(BUILD, "pages")
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "render.py"), "--pdf", PDF, "--out", out],
                       capture_output=True, check=True)
        names = sorted(os.listdir(out))
        self.assertEqual(names, ["p%03d.png" % i for i in range(1, 193)])
        sizes = {}
        total = 0
        for n in names:
            w, h, bits, colour, rows = png_rows(os.path.join(out, n))
            self.assertEqual((bits, colour), (4, 0), n)
            self.assertLessEqual(w, render.MAX_W, n)
            sizes[(w, h)] = sizes.get((w, h), 0) + 1
            total += os.path.getsize(os.path.join(out, n))
        self.assertEqual(sorted(sizes.values()), [10, 75, 107])     # the guide's three kinds of page, as cut
        self.assertIn((780, 487), sizes)                           # the tables: the Sentry's window across
        self.assertLess(total, 6_000_000)                          # 5.2 MB on the rootfs
        w, h, _, _, rows = png_rows(os.path.join(out, "p068.png"))  # the CTCSS table: print, not paper alone
        self.assertGreater(sum(1 for r in rows for v in r if v < 8), 10000)


class Packaging(unittest.TestCase):
    """What puts NIFOG in the image and the bundle: CMake's install, the updater's paths, CI's and Docker's poppler"""

    def read(self, *p):
        with open(os.path.join(TOP, *p)) as f:
            return f.read()

    def test_cmake_installs_it(self):
        c = self.read("CMakeLists.txt")
        self.assertIn("third-party/nifog/NIFOG_2.02.pdf", c)
        self.assertIn("install(FILES ${NIFOG_PDF} DESTINATION share/x6100/nifog RENAME nifog.pdf)", c)
        self.assertIn("install(FILES tools/nifog/nifog.idx DESTINATION share/x6100/nifog)", c)
        self.assertIn('DESTINATION share/x6100/nifog FILES_MATCHING PATTERN "p[0-9][0-9][0-9].png"', c)
        self.assertIn("message(FATAL_ERROR", c[c.index("NIFOG_PAGES"):])      # no pages: the image's install stops
        self.assertIn("nifog.c", self.read("src", "CMakeLists.txt"))

    def test_the_updater_takes_its_files(self):
        spec = importlib.util.spec_from_file_location("mkbundle", os.path.join(TOP, "tools", "update", "mkbundle.py"))
        mk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mk)
        allowed = mk.updater_allowed()
        for p in ("/usr/share/x6100/nifog/nifog.pdf", "/usr/share/x6100/nifog/nifog.idx",
                  "/usr/share/x6100/nifog/p001.png", "/usr/share/x6100/nifog/p192.png"):
            self.assertFalse(mk.path_refused(p, allowed), p)

    def test_the_builds_have_poppler(self):
        main = self.read(".github", "workflows", "main.yml")
        self.assertIn("poppler-utils", main)
        self.assertLess(main.index("Install poppler (NIFOG's pages)"), main.index("- name: Build\n"))
        self.assertIn("poppler-utils", self.read("buildroot", "Dockerfile"))
        self.assertIn("command -v pdftoppm", self.read("buildroot", "local-build.sh"))

    def test_the_pdf_is_cisas(self):
        import hashlib
        with open(PDF, "rb") as f:
            data = f.read()
        readme = self.read("third-party", "nifog", "README.md")
        self.assertIn(hashlib.sha256(data).hexdigest(), readme)
        self.assertIn(f"{len(data):,} bytes", readme)


@unittest.skipIf(apps is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class WebEdges(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.join(BUILD, "edges%d" % id(self))
        os.makedirs(self.dir, exist_ok=True)
        self.addCleanup(setattr, settings, "NIFOG_DIR", settings.NIFOG_DIR)
        settings.NIFOG_DIR = self.dir

    def test_pages_without_the_pdf(self):
        """The pages and the contents there, the PDF not: the viewer, and the page says the PDF is missing"""
        with open(IDX) as f, open(os.path.join(self.dir, "nifog.idx"), "w") as g:
            g.write(f.read())
        st, body, _ = call("/nifog")
        self.assertEqual(st, 200)
        self.assertIn(b"The PDF is not on this radio", body)
        self.assertIn(b'id="viewer"', body)
        self.assertNotIn(b'href="/nifog.pdf"', body)
        self.assertEqual(call("/nifog.pdf")[0], 404)

    def test_a_broken_index(self):
        with open(os.path.join(self.dir, "nifog.idx"), "w") as f:
            f.write("pages x\nsec a b c\npage q\nsec 5 1 Past\npages 3\nsec 2 0 Fine\n")
        idx = nifog_web.read_index(self.dir)
        self.assertEqual(idx["pages"], 3)
        self.assertEqual(idx["secs"], [{"page": 2, "level": 0, "title": "Fine"}])
        self.assertIsNone(nifog_web.page_file(self.dir, 1, 3))          # no file: none
        self.assertIsNone(nifog_web.page_file(self.dir, 4, 3))          # past the last: none


if __name__ == "__main__":
    os.makedirs(BUILD, exist_ok=True)
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
