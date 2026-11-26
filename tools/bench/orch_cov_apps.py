#!/usr/bin/env python3
"""The apps' features that had no test yet (the owner, 2026-11-26: the bench coverage completed): orch.py's tests of
the radio GUI's apps, on both radios, written against features.json's how_to_confirm.

orch.py imports this module and sets H to itself (its Orch, in_parallel, alike, A, B, W); tests() gives the Test rows
and COVERS the features each covers (orch's coverage map). Every test:
  - opens the same app on both radios at once, drives it through the remote's keys only in that app's window (a key is
    never pressed on a screen the test has not seen), and puts both back on one frequency and mode after each change;
  - judges itself from what the radio says: its state (win, app, freq, mode, pwr, vol, sql, tx), its message box (the
    remote's msg lines), its settings by database name (cfg?), its files over SSH and its web API; the screen's text
    only through OCR of its screenshot (tesseract, the picture kept with the test);
  - changes settings through cfg (put back as found), files only after keeping them aside (put back byte for byte);
  - never transmits but where its feature is a refusal, and then only from a set-up whose transmitter would refuse
    anyway at 0.1 W into the 80 dB cable (the orch session's Watch on both);
  - ends with every app closed and both on the Sentry's page A, on the last frequency.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import contextlib
import json
import os
import re
import shutil
import socket
import subprocess
import time
import urllib.error
import urllib.request

import bench
from bench import Refused

H = None                        # the running orch module (orch.py: orch_cov_apps.H = sys.modules[__name__])
HERE = os.path.dirname(os.path.abspath(__file__))
TESSERACT = shutil.which("tesseract") or "/opt/homebrew/bin/tesseract"


# ---- the radios: files, web, settings, messages ----

def ssh(r, cmd, timeout=40):
    return bench.ssh(r.host, cmd, timeout=timeout)


_ADDR = {}


def _addr(host):
    """The radio's IPv4 address, looked up once (a .local lookup over IPv6 costs ~5 s each time)"""
    if host not in _ADDR:
        _ADDR[host] = socket.getaddrinfo(host, 80, socket.AF_INET)[0][4][0]
    return _ADDR[host]


def http(r, path, method="GET", body=None, raw=None, ctype=None, timeout=20):
    """(status, body text) of a request to r's web server (its own name as Host: the server's own-name guard)"""
    h = {"Host": r.host}
    data = None
    if body is not None:
        data, h["Content-Type"] = json.dumps(body).encode(), "application/json"
    elif raw is not None:
        data, h["Content-Type"] = raw, ctype or "application/octet-stream"
    if method != "GET":
        h["Origin"] = f"http://{r.host}"
    req = urllib.request.Request(f"http://{_addr(r.host)}{path}", data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read(1 << 20).decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read(1 << 20).decode(errors="replace")


def http_json(r, path, method="GET", body=None):
    st, text = http(r, path, method, body)
    try:
        return st, json.loads(text)
    except ValueError:
        return st, text


KEEP = r"""
f={f}; b=/tmp/bench_cov_$(echo {f} | tr / _)
if [ "$1" = save ]; then if [ -e $f ]; then cp -p $f $b; echo saved; else rm -f $b; echo none; fi
else if [ -e $b ]; then cmp -s $f $b && echo same || {{ cp -p $b $f; echo restored; }}; rm -f $b
     else rm -f $f; echo removed; fi; fi
"""


@contextlib.contextmanager
def file_kept(r, *paths):
    """r's files kept aside (or noted absent), put back byte for byte after (a file the test made: removed)"""
    for p in paths:
        ssh(r, "set -- save\n" + KEEP.format(f=p))
    try:
        yield
    finally:
        for p in paths:
            try:
                ssh(r, "set -- back\n" + KEEP.format(f=p))
            except (Refused, OSError, subprocess.SubprocessError):
                pass


def cfg(r, name, timeout=4.0):
    return bench.cfg_get(r, name, timeout)


def wait_cfg(r, what, pred, name, secs=6.0):
    """Until pred(the setting name as read now) holds: polled here, never inside r.wait (whose condition runs under
    r.lock, which the reader needs to take the answer: a cfg? in it waits its whole time and fails)"""
    end = time.time() + secs
    last = None
    while True:
        try:
            last = bench.cfg_get(r, name, timeout=3)
            if pred(last):
                return last
        except Refused:
            pass
        if time.time() > end:
            raise Refused(f"{r.name}: {what}: {name} reads {last!r}")
        time.sleep(0.3)


@contextlib.contextmanager
def cfg_kept(r, values):
    """r's settings set through the remote's cfg (each read back), every one put back as found after"""
    was = {}
    try:
        for name, v in values.items():
            old, now = bench.cfg_set(r, name, v)
            was.setdefault(name, old)
            if str(now) != str(v) and not _same_num(now, v):
                raise Refused(f"{r.name}: setting {name} reads {now!r} after asking {v!r}")
        yield was
    finally:
        for name, old in was.items():
            try:
                bench.cfg_set(r, name, old)
            except Refused:
                pass


def _same_num(a, b):
    try:
        return abs(float(a) - float(b)) < 1e-6
    except (TypeError, ValueError):
        return False


def msgs_since(r, t0):
    with r.lock:
        return [t for at, t in r.msgs if at >= t0]


def wait_msg(r, t0, pattern, secs=6.0):
    """The first message box text since t0 matching pattern (a regex, any case); Refused when none comes"""
    rx = re.compile(pattern, re.I)
    end = time.time() + secs
    while time.time() < end:
        for t in msgs_since(r, t0):
            if rx.search(t):
                return t
        time.sleep(0.1)
    raise Refused(f"{r.name}: no message like {pattern!r} within {secs:.0f} s (it said: {msgs_since(r, t0)[-4:]})")


def no_msg(r, t0, pattern, secs):
    """None of r's messages since t0 matches pattern, waited secs; the one that did: Refused"""
    rx = re.compile(pattern, re.I)
    time.sleep(secs)
    bad = [t for t in msgs_since(r, t0) if rx.search(t)]
    if bad:
        raise Refused(f"{r.name}: said {bad[0]!r}")


# ---- the screen: keys only in a window the test has seen, pictures and their text ----

def win(r):
    return r.state.get("win") or ""


def guard(r, names, what):
    names = (names,) if isinstance(names, str) else tuple(names)
    if not any(win(r).lower().startswith(n.lower()) for n in names):
        raise Refused(f"{r.name}: {what} not sent: the screen shows {win(r) or 'no window'!r}, not {names}")


def press(r, key, names, hold=False, rest=0.5):
    """KEY pressed (held 1.5 s) while one of the windows names is on the screen, else nothing (Refused)"""
    guard(r, names, f"key {key}")
    r.take_control()
    r.send(f"key {key} 1")
    time.sleep(1.5 if hold else 0.15)
    r.send(f"key {key} 0")
    time.sleep(rest)


def fkeys(r, timeout=3.0):
    """What F1-F5 show now (the remote's "fkeys?"): ([5 texts as the code gives them, '\\n' kept], [5 marks: 1 checked,
    2 greyed], hidden). Refused from a GUI without the query"""
    t0 = time.time()
    r.send("fkeys?")
    end = t0 + timeout
    while time.time() < end:
        with r.lock:
            got = [x for at, x in list(r.lines) if at >= t0 - 0.05 and x.startswith("fkeys ")]
        if got:
            d = json.loads(got[-1][6:])
            return d["t"], d["m"], bool(d["hid"])
        time.sleep(0.05)
    raise Refused(f"{r.name}: no answer to fkeys? (a GUI from before it)")


def flat(label):
    """A button's text on one line: 'Set clock:\\nAuto' -> 'Set clock: Auto'"""
    return " ".join(label.split())


def fkey_of(r, pattern):
    """The F key (F1..F5) whose button text matches pattern (a regex, any case) now, or None"""
    texts, _, hid = fkeys(r)
    rx = re.compile(pattern, re.I)
    for i, t in enumerate(texts):
        if rx.search(flat(t)):
            return f"F{i + 1}"
    return None


def press_label(r, pattern, names, hold=False, rest=0.6, pages=4):
    """The button whose text matches pattern pressed by its F key, in window names; when it is not on the page shown,
    F1 (the page key: its text '(...)') steps the app's pages, at most pages times, each read back. Its key"""
    for _ in range(pages + 1):
        k = fkey_of(r, pattern)
        if k:
            press(r, k, names, hold=hold, rest=rest)
            return k
        texts, _, _ = fkeys(r)
        if not texts[0].startswith("("):
            break
        press(r, "F1", names, rest=0.6)
    raise Refused(f"{r.name}: no button like {pattern!r} on its pages (shows {[flat(t) for t in fkeys(r)[0]]})")


def press_if(r, key, pattern, names, hold=False, rest=0.6):
    """KEY pressed only while its button still reads like pattern, read again right before (Field Ops rebuilds its
    page by itself, back to page 1, when a position or a list changes: a View press landed on Start, then on CQ, 11-26)"""
    texts = fkeys(r)[0]
    lbl = flat(texts[int(key[1]) - 1])
    if not re.search(pattern, lbl, re.I):
        raise Refused(f"{r.name}: {key} reads {lbl!r} now, not {pattern!r}: not pressed")
    press(r, key, names, hold=hold, rest=rest)


def turn(r, knob, n, names, rest=0.5):
    guard(r, names, f"knob {knob} {n}")
    r.take_control()
    r.send(f"knob {knob} {n}")
    time.sleep(rest)


def shot(o, r, label):
    """r's screen now (landscape PNG), kept with the running test (the console's To look at); its path or None. The
    bench's banner (the test's name across the screen) is taken off for it and put back: its words are read by OCR
    too ("BENCH WEFAX-BROWSER" passed for a fax reading BENCH FAX, 11-26)"""
    banner = None
    with o.st.lock:
        if o.st.cur is not None:
            t = o.st.tests[o.st.cur]
            banner = f"banner BENCH {t['name'].upper()} ({t['role'].upper()})"[:100]
    try:
        r.send("banner")
        time.sleep(0.4)
        path = bench.screenshot(r, f"{label}_{r.name}"[:70])
    except Exception as e:  # noqa: BLE001  (a picture never stops a test)
        o.st.ev(f"{r.name}: picture not taken: {e}", "radio")
        return None
    finally:
        if banner:
            r.send(banner)
    if path:
        with o.st.lock:
            if o.st.cur is not None:
                o.st.tests[o.st.cur]["shots"].append({"t": time.time(), "label": label, "paths": {r.name: path}})
        o.st.ev(f"picture: {label}: {os.path.basename(path)}", "conf")
    return path


OCR_FIX = str.maketrans({"0": "O", "1": "I", "|": "I", "5": "S", "8": "B", "@": "O", "$": "S"})


def ocr(path, box=None, scale=3, psm=11, upright=False, stretch=False):
    """The text tesseract reads on a screenshot (or its box (x0, y0, x1, y1), landscape px): the picture grey,
    inverted (the GUI's light text on dark), scaled up; upright: turned a quarter (the walkie screen's portrait text,
    which the landscape picture has running downwards); stretch: its darkest and lightest grey spread to black and
    white first (a fax shown dim, 3-71 of 255, 11-26). '' when there is no picture or no tesseract"""
    if not path or not os.path.exists(TESSERACT):
        return ""
    from PIL import Image, ImageOps
    im = Image.open(path).convert("L")
    if box:
        im = im.crop(box)
    if upright:
        im = im.rotate(-90, expand=True)
    if stretch:
        im = ImageOps.autocontrast(im, cutoff=1)
        # the row with the focus (dark text on a light bar) turned like the rest (light on dark): one pass reads all
        for y in range(im.height):
            row = im.crop((0, y, im.width, y + 1))
            if sorted(row.getdata())[im.width // 2] > 128:
                im.paste(ImageOps.invert(row), (0, y))
    big = im.resize((im.width * scale, im.height * scale), Image.LANCZOS)
    out = []
    # inverted (light text on the dark screen) and as it is (dark text on a highlighted row): both read
    for k, pic in enumerate((ImageOps.invert(big), big)):
        tmp = f"{path}.ocr{k}.png"
        pic.save(tmp)
        try:
            out.append(subprocess.run([TESSERACT, tmp, "-", "--psm", str(psm)], capture_output=True, text=True,
                                      timeout=60).stdout)
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass
    return "\n".join(out)


def squash(s):
    """Text compared loosely: upper case, letters and digits only, the digits OCR mixes with letters as letters"""
    return re.sub(r"[^A-Z0-9]", "", s.upper().translate(OCR_FIX))       # mapped first: '@' is an O


def ocr_has(text, *words):
    """Every one of words in the OCR text (compared loosely: squash)"""
    t = squash(text)
    return all(squash(w) in t for w in words)


def shot_text(o, r, label, box=None, psm=11):
    """(picture path, its OCR text)"""
    p = shot(o, r, label)
    return p, ocr(p, box, psm=psm)


POPUP_BOX = (150, 120, 790, 425)            # the apps' popups and lists over the middle of the screen


def popup_text(o, r, label, box=POPUP_BOX):
    """(picture path, its OCR text): the whole screen, then the popup's area (box) alone, stretched (its rows' dim
    text read nothing whole-screen: JS8's Freq, GhostNet's and the texts' popups, the first-start summary, 11-26)"""
    p, txt = shot_text(o, r, label)
    txt += "\n" + ocr(p, box=box, psm=6, stretch=True)
    for bar in _bars(p, box):               # the row with the focus, read on its own (its text read nowhere else)
        txt += "\n" + ocr(p, box=bar, psm=7, stretch=True, scale=2)
    return p, txt


def _bars(path, box, least=12):
    """The light bars (a focused row: dark text on gold) in box of the picture: [(x0, y0, x1, y1)] in its coordinates,
    each least px tall at least, as wide as the bar's light runs in its middle row (a waterfall beside the popup kept
    the bar's text from being read)"""
    if not path or not os.path.exists(path):
        return []
    from PIL import Image
    im = Image.open(path).convert("L").crop(box)
    out, start = [], None
    for y in range(im.height + 1):
        light = y < im.height and sorted(im.crop((0, y, im.width, y + 1)).getdata())[im.width // 2] > 110
        if light and start is None:
            start = y
        elif not light and start is not None:
            if y - start >= least:
                # the bar's own extent: the longest light run along a row near its top, above its text (a waterfall
                # beside it is light too, but broken)
                top = list(im.crop((0, start + 2, im.width, start + 3)).getdata())
                best, run0 = (0, 0), None
                for x, v in enumerate(top + [0]):
                    if v > 110 and run0 is None:
                        run0 = x
                    elif v <= 110 and run0 is not None:
                        if x - run0 > best[1] - best[0]:
                            best = (run0, x)
                        run0 = None
                out.append((box[0] + best[0], box[1] + start - 1, box[0] + best[1], box[1] + y + 1))
            start = None
    return out


WINDOW_BOX = (10, 118, 790, 418)            # an app's window under the frequency strip, above the buttons


# ---- both radios ----

def page_both(o, letter):
    """Both radios on the Sentry's page letter as one change, alike on the dial they shared"""
    o.changing += 1
    try:
        bench.open_page_both([o.r[H.A], o.r[H.B]], letter)
    finally:
        o.changing -= 1


def align(o, f=None, mode=None):
    """Both on one dial and mode: f/mode given, else the X6100's"""
    a = o.r[H.A].state
    o.both({"freq": int(f if f is not None else a.get("freq") or 14200000),
            "mode": int(mode if mode is not None else a.get("mode") or 2)})


def open_both(o, action, name, f=None, mode=None, secs=12.0, same=True):
    """App ACTION open on both radios at once (from the Sentry's A), seen as window name on each; then both on f/mode
    (the app's own dial on the X6100 when not given) inside the same change"""
    def one(n, r):
        r.take_control()
        if r.state.get("pg") not in ("A",) and (r.state.get("win") or "Sentry") == "Sentry":
            bench.open_page(r, "A")
        r.send(f"open {action}")
        r.wait(f"the {name} window", lambda: win(r).lower().startswith(name.lower()), secs)
        bench.settled(r, still=1.0, timeout=5.0)
        return win(r)
    o.changing += 1
    try:
        got = H.in_parallel(o, one, wait=secs + 15)
        if same:
            align(o, f, mode)
    finally:
        o.changing -= 1
    return got


def close_both(o):
    """Every app closed on both (each back on the dial its app had), then both alike"""
    o.changing += 1
    try:
        for r in o.r.values():
            bench.close_apps(r)
        align(o)
    finally:
        o.changing -= 1


def home_both(o):
    """The end of every test: apps closed, both on the Sentry's page A on the last frequency, alike"""
    o.changing += 1
    try:
        for r in o.r.values():
            try:
                bench.close_apps(r)
            except Refused as e:
                o.st.ev(f"{r.name}: {e}", "bad")
        try:
            page_both(o, "A")
            align(o)
        except Refused as e:
            o.st.ev(f"not both on A: {e}", "bad")
    finally:
        o.changing -= 1


def each(o, fn, wait=60):
    """fn(name, radio) on both at once (H.in_parallel): {name: its return}"""
    return H.in_parallel(o, fn, wait=wait)


# ---- Bands (119): open and close, Use band by the MFK ----

MODE = {"LSB": 0, "LSB-D": 1, "USB": 2, "USB-D": 3, "CW": 4, "CW-R": 5, "AM": 6, "NFM": 7}
# src/field/bands.c field_band_freq: (CW, phone) of each band, Region 2 (the US: the radios' band plan) where it differs
BAND_FREQ = {"160m": (1810000, 1850000), "80m": (3560000, 3860000), "60m": (5355000, 5360000),
             "40m": (7030000, 7200000), "30m": (10118000, 10118000), "20m": (14060000, 14285000),
             "17m": (18086000, 18130000), "15m": (21060000, 21285000), "12m": (24906000, 24950000),
             "10m": (28060000, 28400000), "6m": (50090000, 50150000)}
BANDS = list(BAND_FREQ)


def _bands_use(o, start, mode, steps, want_band):
    """Both from start in mode: Bands open, the MFK steps rows, its press tunes want_band's typical frequency (CW in
    CW, the mode kept; else phone in its sideband) and closes the app; its message names the band"""
    align(o, start, mode)
    open_both(o, 119, "Bands", start, mode)
    cw = mode in (4, 5)
    f = BAND_FREQ[want_band][0 if cw else 1]
    want_mode = mode if cw else (0 if f < 10000000 else 2)

    def one(n, r):
        t0 = time.time()
        turn(r, "mfk", steps, "Bands", rest=0.8)
        press(r, "MFK", "Bands", rest=0.2)
        r.wait(f"{want_band}'s {f / 1e6:.3f} MHz", lambda: int(r.state.get("freq", 0)) == f, 5)
        r.wait("the app closed", lambda: win(r) != "Bands", 4)
        said = wait_msg(r, t0, rf"{want_band}: {f / 1e6:.3f} MHz", 4)
        r.wait(f"mode {want_mode}", lambda: int(r.state.get("mode", -1)) == want_mode, 4)
        return f"{said}, mode {r.state.get('mode')}, window now {win(r)!r}"
    o.changing += 1
    try:
        got = each(o, one)
        H.alike(o, ("freq", "mode"))
    finally:
        o.changing -= 1
    return got


def t_bands(o):
    """The Bands app on both: open 119 shows the Bands window, ESC (close) closes it; MFK 2 rows down from 20 m and a
    press tunes 40 m's phone frequency in LSB (Region 2: 7.200) and closes the app, 'Use band' said; from CW the same
    gives 40 m's CW frequency with CW kept"""
    res = {}
    try:
        with o.step("both on 14.200 USB, page A; open 119 on both: the Bands window; close: gone") as s:
            page_both(o, "A")
            res["open"] = open_both(o, 119, "Bands", 14200000, 2)
            close_both(o)
            if any(win(r) == "Bands" for r in o.r.values()):
                raise Refused("Bands still open after close")
            s.confirm(f"{res['open']}, then {[win(r) for r in o.r.values()]}")
        with o.step("USB 14.200: MFK -2 (20 m -> 40 m) and its press: 7.200 LSB on both, the app closed") as s:
            res["phone"] = _bands_use(o, 14200000, 2, -2, "40m")
            s.confirm(str(res["phone"]))
        with o.step("CW 14.040: MFK -2 and its press: 7.030, CW kept, on both") as s:
            res["cw"] = _bands_use(o, 14040000, 4, -2, "40m")
            s.confirm(str(res["cw"]))
        with o.step("CW-R 7.030: MFK +2 (40 m -> 20 m) and its press: 14.060, CW-R kept") as s:
            res["cwr"] = _bands_use(o, 7030000, 5, 2, "20m")
            s.confirm(str(res["cwr"]))
        return True, (f"Bands opened and closed on both; Use band by the MFK: {res['phone']['x6100']}; from CW "
                      f"{res['cw']['x6100']}; from CW-R {res['cwr']['x6100']}"), res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- Time Signal (132): F2 Set clock Auto / Off ----

def _label_flips(o, names, pattern, states, label):
    """On both: the button like pattern pressed, its text read back after each press: it steps through states (a
    regex each, in turn from the one it shows) and back to the first. Returns what each radio showed"""
    def one(n, r):
        k = fkey_of(r, pattern)
        if not k:
            raise Refused(f"{n}: no button like {pattern!r} ({[flat(t) for t in fkeys(r)[0]]})")
        seen = [flat(fkeys(r)[0][int(k[1]) - 1])]
        start = next((i for i, x in enumerate(states) if re.search(x, seen[0], re.I)), None)
        if start is None:
            raise Refused(f"{n}: {seen[0]!r} is none of {states}")
        for j in range(1, len(states) + 1):
            press(r, k, names, rest=0.7)
            t = flat(fkeys(r)[0][int(k[1]) - 1])
            seen.append(t)
            want = states[(start + j) % len(states)]
            if not re.search(want, t, re.I):
                raise Refused(f"{n}: after {j} press(es) {k} reads {t!r}, not like {want!r} ({seen})")
        return seen
    return each(o, one)


def t_timesig(o):
    """Time Signal on both (WWV 10 MHz AM): F2 'Set clock: Auto' -> 'Set clock: Off' -> Auto again, read back from the
    buttons as drawn (the remote's fkeys?)"""
    res = {}
    try:
        with o.step("Time Signal open on both (open 132)") as s:
            res["open"] = open_both(o, 132, "Time Signal")
            s.confirm(f"{res['open']} on {o.r[H.A].state.get('freq')} mode {o.r[H.A].state.get('mode')}")
        with o.step("F2 on both: Set clock Auto -> Off -> Auto") as s:
            res["f2"] = _label_flips(o, ("Time Signal",), r"^Set clock", [r"Auto$", r"Off$"], "set clock")
            s.confirm(str(res["f2"]))
        return True, f"Set clock toggled Auto/Off/Auto on both: {res['f2'][H.A]}", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- Channels on the web (the Memories page's API): a stale change refused, Undo, a Save of what changed only ----

def _ch_list(r):
    st, d = http_json(r, "/api/channels")
    if st != 200 or not isinstance(d, dict):
        raise Refused(f"{r.name}: GET /api/channels {st}: {str(d)[:120]}")
    return d.get("channels") or []


def _ch_add(r, name, hz, mode="USB"):
    st, d = http_json(r, "/api/channels", "POST", {"name": name, "frequency": hz, "mode": mode, "agc": "AUTO",
                                                  "pre": False, "att": False})
    if st != 200:
        raise Refused(f"{r.name}: adding {name} gave {st}: {str(d)[:120]}")
    got = _ch_list(r)
    return next(i for i, c in enumerate(got) if c.get("name") == name), got


def _ch_file(r):
    return json.loads(ssh(r, "cat /mnt/channels.json"))


def t_channels_web(o):
    """The Memories page's API on both radios (the channels file kept aside, put back byte for byte): a Delete naming
    a channel that is no longer at its place (deleted meanwhile, as from another tab) changes nothing (409, the
    radio's list) and the file stays; a Delete, then Undo (restore) puts the entry back at its place as the file had
    it; a Save that changes only the frequency keeps a 40-byte name and an extra key the page does not know"""
    res = {}

    def one(n, r):
        out = {}
        with file_kept(r, "/mnt/channels.json"):
            i1, _ = _ch_add(r, "BENCH CC1", 14100000)
            i2, now = _ch_add(r, "BENCH CC2", 14200000)
            c1 = now[i1]
            st, d = http_json(r, f"/api/channels/{i1}", "DELETE", {"was": c1})
            if st != 200:
                raise Refused(f"{n}: deleting BENCH CC1 gave {st}: {str(d)[:120]}")
            before = ssh(r, "md5sum /mnt/channels.json").split()[0]
            st, d = http_json(r, f"/api/channels/{i1}", "DELETE", {"was": c1})     # the page's stale view
            after = ssh(r, "md5sum /mnt/channels.json").split()[0]
            left = [c.get("name") for c in _ch_list(r)]
            out["stale_delete"] = st
            if st != 409 or before != after or "BENCH CC2" not in left:
                raise Refused(f"{n}: a Delete of BENCH CC1 at {i1} after it went gave {st} ({str(d)[:100]}), the file "
                              f"{'unchanged' if before == after else 'CHANGED'}, CC2 {'kept' if 'BENCH CC2' in left else 'GONE'}")
            out["stale_says"] = str(d.get("error") if isinstance(d, dict) else d)[:100]
            # Delete, then Undo: back at its place as the file had it
            now = _ch_list(r)
            i2 = next(i for i, c in enumerate(now) if c.get("name") == "BENCH CC2")
            entry0 = next(c for c in _ch_file(r)["channels"] if c.get("name") == "BENCH CC2")
            st, d = http_json(r, f"/api/channels/{i2}", "DELETE", {"was": now[i2]})
            dl = d.get("deleted") if isinstance(d, dict) else None
            if st != 200 or not dl:
                raise Refused(f"{n}: deleting BENCH CC2 gave {st}: {str(d)[:120]}")
            st, d = http_json(r, "/api/channels/restore", "POST", dl)
            back = _ch_list(r)
            at = next((i for i, c in enumerate(back) if c.get("name") == "BENCH CC2"), None)
            entry1 = next((c for c in _ch_file(r)["channels"] if c.get("name") == "BENCH CC2"), None)
            out["undo"] = (st, at, i2)
            if st != 200 or at != i2 or entry1 != entry0:
                raise Refused(f"{n}: Undo gave {st}, CC2 back at {at} (was {i2}), as in the file {entry1 == entry0}")
            # a channel with a name over 31 bytes and a key of its own, written into the file as another program would
            ssh(r, "python3 -c \"import json;p='/mnt/channels.json';d=json.load(open(p));"
                   "d['channels'].append({'name':'BENCH LONG NAME 0123456789ABCDEFGHIJKLMN','frequency':14300000,"
                   "'mode':'USB','agc':'AUTO','pre':False,'att':False,'bench_extra':'kept'});"
                   "json.dump(d,open(p,'w'))\"")
            time.sleep(1.0)
            now = _ch_list(r)
            il = next((i for i, c in enumerate(now) if str(c.get("name", "")).startswith("BENCH LONG")), None)
            if il is None:
                raise Refused(f"{n}: the 40-byte channel not on the page's list ({[c.get('name') for c in now][-4:]})")
            shown = dict(now[il])
            st, d = http_json(r, f"/api/channels/{il}", "PUT", dict(shown, frequency=14310000, was=now[il]))
            got = next((c for c in _ch_file(r)["channels"] if c.get("bench_extra") == "kept"), None)
            out["partial"] = (st, got)
            if st != 200 or not got or got.get("frequency") != 14310000 or \
                    got.get("name") != "BENCH LONG NAME 0123456789ABCDEFGHIJKLMN":
                raise Refused(f"{n}: the Save of its frequency gave {st} ({str(d)[:80]}), the file's entry {got}")
            out["shown_name"] = shown.get("name")
        out["file"] = ssh(r, "md5sum /mnt/channels.json").split()[0]
        return out
    try:
        with o.step("on both: a stale Delete refused, Delete + Undo at its place, a Save of the frequency alone") as s:
            res = each(o, one, wait=90)
            s.confirm(str({n: {k: v for k, v in x.items() if k in ("stale_delete", "undo", "shown_name")}
                           for n, x in res.items()}))
        return True, (f"stale Delete refused {res[H.A]['stale_delete']} ({res[H.A]['stale_says']}); Undo back at its "
                      f"place; a 40-byte name (shown {res[H.A]['shown_name']!r}) and an extra key kept by a Save of "
                      "the frequency; both radios, their files put back"), res
    except Refused as e:
        return False, str(e), res


# ---- WSPR on the web: Hop and each band's use ----

def t_wspr_web(o):
    """The web Digital modes page's WSPR settings on both (GET/POST /api/display): Hop on, 40 m RX+TX and 6 m RX, read
    back on the page and through the remote's cfg?; then put back as found"""
    res = {}

    def one(n, r):
        st, d = http_json(r, "/api/display")
        if st != 200 or "wspr_hop" not in d or "wspr_bands" not in d:
            raise Refused(f"{n}: GET /api/display {st}: no wspr_hop/wspr_bands")
        hop0, bands0 = int(d["wspr_hop"]), int(d["wspr_bands"])
        want = (bands0 & ~(3 << 6) & ~(3 << 20)) | (2 << 6) | (1 << 20)      # 40 m (the 4th) RX+TX, 6 m (11th) RX
        try:
            for name, val in (("wspr_hop", 1), ("wspr_bands", want)):
                st, a = http_json(r, "/api/display", "POST", {"name": name, "val": val})
                if st != 200:
                    raise Refused(f"{n}: POST {name}={val}: {st} {str(a)[:80]}")
            t0 = time.time()
            while True:                 # the page's answer may lag the GUI a moment: up to 5 s, how long said
                st, d = http_json(r, "/api/display")
                page = (int(d["wspr_hop"]), int(d["wspr_bands"]))
                remote = (int(cfg(r, "wspr_hop")), int(cfg(r, "wspr_bands")))
                if (page == (1, want) and remote == (1, want)) or time.time() - t0 > 5:
                    break
                time.sleep(0.5)
            lag = round(time.time() - t0, 1)
            if page != (1, want) or remote != (1, want):
                raise Refused(f"{n}: asked hop 1 bands {want:#x}: the page reads {page}, the remote {remote}")
            return {"was": (hop0, hex(bands0)), "set": hex(want), "page_lag_s": lag}
        finally:
            http_json(r, "/api/display", "POST", {"name": "wspr_hop", "val": hop0})
            http_json(r, "/api/display", "POST", {"name": "wspr_bands", "val": bands0})
    try:
        with o.step("on both: Hop on, 40 m RX+TX, 6 m RX through the page; read back; put back") as s:
            res = each(o, one)
            s.confirm(str(res))
        return True, f"WSPR Hop and band uses set on the web page and read back on both ({res[H.A]})", res
    except Refused as e:
        return False, str(e), res


# ---- CB and HF Air Band: no TX there ----

def _no_tx_there(o, n, label):
    """n's PTT where it may not transmit (the app's frequency), with an app open (the Sentry closed under it: its
    receive-only hold gone, rxo 0, so the refusal is the band's): the remote's PTT refused with its reason (an err
    line), a program's TCI audio on the radio refused by the radio itself ('No TX on ...' and why, msg lines); n off
    the air throughout, the other radio on the same dial hears nothing. Refused unless tx_outside_bands is 0. No A/B
    is tapped: in an app it is the VFO key"""
    import subprocess
    R = o.r[n]
    X = next(x for m, x in o.r.items() if m != n)
    if str(cfg(R, "tx_outside_bands")) != "0":
        raise Refused(f"{n}: TX outside ham bands is on: no PTT tried")
    if R.state.get("rxo") != 0:
        raise Refused(f"{n}: receive only held (rxo {R.state.get('rxo')}): the band's refusal can't be seen")
    f = int(R.state.get("freq", 0))
    went, t0 = False, time.time()
    out = {}
    try:
        n0 = len(R.errs)
        R.send(f"key PTT 1 {o.hold()}")
        while time.time() - t0 < 2.5:
            went = went or bool(R.tx)
            time.sleep(0.05)
        R.send("key PTT 0")
        out["ptt"] = [e for e in R.errs[n0:] if e.startswith("ptt:")]
        cp = subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
                             f"root@{R.host}:/tmp/tci_tone_tx.py"], capture_output=True, text=True, timeout=30)
        if cp.returncode:
            raise Refused(f"{n}: tci_tone_tx.py not copied: {cp.stderr.strip()[:100]}")
        t1 = time.time()
        subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{R.host}",
                          "python3 /tmp/tci_tone_tx.py '[[1000, 2.0]]' 0.3 3.0 50001 0.05"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while time.time() - t1 < 4.5:
            went = went or bool(R.tx)
            time.sleep(0.05)
        out["tci"] = msgs_since(R, t1)
    finally:
        R.send("key PTT 0")
        try:
            bench.stop_bench_programs(R)
        finally:
            R.stop()
    if went:
        raise Refused(f"{n} WENT ON THE AIR on {f / 1e6:.3f} MHz")
    mhz = f"{f / 1e6:.3f}"
    if not out["ptt"] or mhz not in out["ptt"][-1]:
        raise Refused(f"{n}: the remote's PTT on {mhz} MHz not refused for it (it said {R.errs[n0:][-2:]})")
    if not any(f"No TX on {mhz}" in m for m in out["tci"]):
        raise Refused(f"{n}: TCI audio on {mhz} MHz: no 'No TX on {mhz} MHz' (it said {out['tci'][-3:]})")
    return {"ptt": out["ptt"][-1], "tci": [m for m in out["tci"] if "TX" in m][:3], "other_meter": X.meter}


def t_cb_refused(o):
    """CB on both (27.185 AM, CB 19): its PTT refused, by the remote ('no TX on 27.185 MHz') and by the radio for a
    program's TCI audio ('No TX on 27.185 MHz', 'Out of band: Settings > TX Cal'), each radio armed in turn, the other
    listening on the same channel; the app sets no power (PWR as before)"""
    res = {}
    try:
        pwr0 = {n: r.state.get("pwr") for n, r in o.r.items()}
        with o.step("both 0.1 W; CB open on both (open 205), on its channel") as s:
            o.both({"pwr": H.W})
            pwr0 = {n: r.state.get("pwr") for n, r in o.r.items()}
            res["open"] = open_both(o, 205, "CB")

            def tune(n, r):
                t0 = time.time()
                press(r, "MFK", "CB", rest=0.8)
                said = wait_msg(r, t0, r"MHz", 5)
                r.wait("11 m", lambda: 26000000 <= int(r.state.get("freq", 0)) <= 28000000, 4)
                return said
            res["tune"] = each(o, tune)
            align(o)
            f = int(o.r[H.A].state.get("freq", 0))
            if not 26000000 <= f <= 28000000:
                raise Refused(f"CB opened on {f}: not the 11 m band")
            pw = {n: r.state.get("pwr") for n, r in o.r.items()}
            if pw != pwr0:
                raise Refused(f"the CB app set the power: {pwr0} -> {pw}")
            s.confirm(f"{res['open']}, the MFK tuned {res['tune']}: {f / 1e6:.3f} MHz mode "
                      f"{o.r[H.A].state.get('mode')}, PWR {pw}")
        for n in (H.A, H.B):
            with o.step(f"{n} on CB (the app open: no receive-only hold): PTT and TCI audio refused, off the air") as s:
                res[n] = _no_tx_there(o, n, "cb")
                s.confirm(str(res[n]))
        return True, f"CB: PTT refused on both ({res[H.A]['ptt']}; {res[H.A]['tci']}); the app set no power", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_hfair(o):
    """HF Air Band on both: the list's first heading is VOLMET (its screen read by OCR); the MFK's press tunes the row
    in USB and says it ('<name> <kHz> kHz USB'); PTT there refused as on CB, each radio armed in turn"""
    res = {}
    try:
        with o.step("HF Air Band open on both (open 204); the list read on its screen") as s:
            res["open"] = open_both(o, 204, "HF Air Band")
            time.sleep(1.5)
            txt = {n: shot_text(o, r, "hfair list")[1] for n, r in o.r.items()}
            res["ocr"] = {n: " ".join(t.split())[:300] for n, t in txt.items()}
            first = {n: (re.search(r"[A-Z][A-Z0-9]{2,}", t.upper()) or [None])[0] for n, t in txt.items()}
            if not all("VOLMET" in t.upper() for t in txt.values()):
                raise Refused(f"no VOLMET heading read on the screens: {res['ocr']}")
            s.confirm(f"VOLMET read on both; first words {first}")
        with o.step("the MFK's press on both: the row tuned in USB, said") as s:
            def tune(n, r):
                t0 = time.time()
                press(r, "MFK", "HF Air Band", rest=0.8)
                said = wait_msg(r, t0, r"\d+ kHz USB", 5)
                k = int(re.search(r"(\d+) kHz USB", said).group(1))
                r.wait(f"{k} kHz in USB", lambda: abs(int(r.state.get("freq", 0)) - k * 1000) < 1000 and
                       int(r.state.get("mode", -1)) == 2, 4)
                return said
            res["tune"] = each(o, tune)
            H.alike(o, ("freq", "mode"))
            s.confirm(str(res["tune"]))
        for n in (H.A, H.B):
            with o.step(f"{n} there (the app open: no receive-only hold): PTT and TCI audio refused") as s:
                res[n] = _no_tx_there(o, n, "hfair")
                s.confirm(str(res[n]))
        return True, f"HF Air Band: VOLMET first, {res['tune'][H.A]!r}; PTT refused ({res[H.A]['ptt']})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- Zello and SVXConnect without an account: Talk via, the links' squelch rule, one link at a time ----

LINKS = ("zello_to_rf", "zello_to_zello", "svx_to_rf", "svx_to_svx")


def _links_off(o):
    for n, r in o.r.items():
        on = {k: cfg(r, k) for k in LINKS}
        if any(str(v) != "0" for v in on.values()):
            raise Refused(f"{n}: a link is on ({on}): the owner's, left alone")


def _links_put_back(o):
    for r in o.r.values():
        for k in LINKS:
            try:
                if str(cfg(r, k)) != "0":
                    bench.cfg_set(r, k, 0)
            except Refused:
                pass


def _to_label(r, pattern, names):
    """The app's page that has a button like pattern (F1 stepping its pages, each read back): its key"""
    for _ in range(5):
        k = fkey_of(r, pattern)
        if k:
            return k
        texts = fkeys(r)[0]
        if not texts[0].startswith("("):
            break
        press(r, "F1", names, rest=0.6)
    raise Refused(f"{r.name}: no button like {pattern!r} ({[flat(t) for t in fkeys(r)[0]]})")


def _label(r, key):
    return flat(fkeys(r)[0][int(key[1]) - 1])


def t_zello(o):
    """Zello on both, no account, nothing connected (no traffic): Talk via (page 2) steps Hand mic -> Bluetooth ->
    Hand mic, zello_talk_bt following, Talk reading 'No BT mic' meanwhile; with SQL 0 neither Zello>RF nor RF>Zello
    turns on (each says why, zello_to_rf / zello_to_zello stay 0); with SVXConnect's SVX>RF on, Zello>RF stays off
    ('SVXConnect link is on'), and the other way round in SVXConnect. Every link put back off"""
    res = {}
    names = ("Zello",)
    try:
        H.test_calls(o)                     # Zello>RF's check of the callsign (its CW ID) comes before the squelch's
        _links_off(o)
        with o.step("Zello open on both (open 202)") as s:
            res["open"] = open_both(o, 202, "Zello")
            s.confirm(str(res["open"]))
        with o.step("Talk via on both: Bluetooth (zello_talk_bt 1, Talk 'No BT mic'), then Hand mic again") as s:
            def via(n, r):
                k = _to_label(r, r"^Talk via", names)
                was = _label(r, k)
                if "Hand mic" not in was:
                    raise Refused(f"{n}: Talk via reads {was!r}, not Hand mic (the owner's?)")
                press(r, k, names, rest=0.8)
                bt = (_label(r, k), cfg(r, "zello_talk_bt"))
                kt = _to_label(r, r"^(Talk|No ?BT ?mic)$", names)
                talk = _label(r, kt)
                k = _to_label(r, r"^Talk via", names)
                press(r, k, names, rest=0.8)
                back = (_label(r, k), cfg(r, "zello_talk_bt"))
                if "Bluetooth" not in bt[0] or str(bt[1]) != "1" or "No BT" not in talk:
                    raise Refused(f"{n}: after one press Talk via {bt[0]!r}, zello_talk_bt {bt[1]}, Talk {talk!r}")
                if "Hand mic" not in back[0] or str(back[1]) != "0":
                    raise Refused(f"{n}: after the second press {back}")
                return {"bt": bt, "talk": talk, "back": back}
            res["via"] = each(o, via)
            s.confirm(str(res["via"]))
        with o.step("SQL 0 on both: Zello>RF and RF>Zello refused for the squelch, both still off") as s:
            o.both({"sql": 0})

            def sql0(n, r):
                out = {}
                for pat, setting, why in ((r"^Zello>RF", "zello_to_rf", r"Zello to RF: set the squelch"),
                                          (r"^RF>Zello", "zello_to_zello", r"RF to Zello: set the squelch")):
                    k = _to_label(r, pat, names)
                    t0 = time.time()
                    press(r, k, names, rest=0.8)
                    out[setting] = wait_msg(r, t0, why, 4)
                    if str(cfg(r, setting)) != "0":
                        raise Refused(f"{n}: {setting} turned on with SQL 0")
                return out
            res["sql0"] = each(o, sql0)
            s.confirm(str(res["sql0"]))
        with o.step("SQL 10, SVXConnect's SVX>RF on (cfg): Zello>RF refused, 'SVXConnect link is on'") as s:
            o.both({"sql": 10})

            def other(n, r):
                with cfg_kept(r, {"svx_to_rf": 1}):
                    k = _to_label(r, r"^Zello>RF", names)
                    t0 = time.time()
                    press(r, k, names, rest=0.8)
                    said = wait_msg(r, t0, r"SVXConnect link is on", 4)
                    if str(cfg(r, "zello_to_rf")) != "0":
                        raise Refused(f"{n}: Zello>RF on beside SVX>RF")
                    return said
            res["one_link"] = each(o, other)
            s.confirm(str(res["one_link"]))
        close_both(o)
        with o.step("Zello's Zello>RF on (cfg), SVXConnect open on both: SVX>RF refused, 'Zello link is on'") as s:
            def other2(n, r):
                with cfg_kept(r, {"zello_to_rf": 1}):
                    r.send("open 206")
                    r.wait("SVXConnect", lambda: win(r).startswith("SVXConnect"), 10)
                    bench.settled(r, still=1.0, timeout=5.0)
                    k = _to_label(r, r"^SVX>RF", ("SVXConnect",))
                    t0 = time.time()
                    press(r, k, ("SVXConnect",), rest=0.8)
                    said = wait_msg(r, t0, r"Zello link is on", 4)
                    if str(cfg(r, "svx_to_rf")) != "0":
                        raise Refused(f"{n}: SVX>RF on beside Zello>RF")
                    return said
            o.changing += 1
            try:
                res["one_link_svx"] = each(o, other2)
                align(o)
            finally:
                o.changing -= 1
            s.confirm(str(res["one_link_svx"]))
        return True, (f"Zello: Talk via {res['via'][H.A]['bt'][0]!r} and back, Talk {res['via'][H.A]['talk']!r}; "
                      f"SQL 0: {res['sql0'][H.A]['zello_to_rf']!r}; one link at a time both ways: "
                      f"{res['one_link'][H.A]!r} / {res['one_link_svx'][H.A]!r}"), res
    except Refused as e:
        return False, str(e), res
    finally:
        _links_put_back(o)
        try:
            o.both({"sql": o.sql if o.sql is not None else 0})
        except Refused:
            pass
        home_both(o)


def t_svx_talk(o):
    """SVXConnect on both, not set up (no reflector: nothing started, no traffic): Talk via (page 2) steps Hand mic ->
    Bluetooth -> Hand mic, svx_talk_bt following"""
    res = {}
    names = ("SVXConnect",)
    try:
        _links_off(o)
        with o.step("SVXConnect open on both (open 206)") as s:
            res["open"] = open_both(o, 206, "SVXConnect")
            s.confirm(str(res["open"]))
        with o.step("Talk via on both: Bluetooth (svx_talk_bt 1), then Hand mic (0)") as s:
            def via(n, r):
                k = _to_label(r, r"^Talk via", names)
                was = (_label(r, k), cfg(r, "svx_talk_bt"))
                press(r, k, names, rest=0.8)
                bt = (_label(r, k), cfg(r, "svx_talk_bt"))
                press(r, k, names, rest=0.8)
                back = (_label(r, k), cfg(r, "svx_talk_bt"))
                if str(was[1]) == str(bt[1]) or back != was:
                    raise Refused(f"{n}: Talk via {was} -> {bt} -> {back}")
                return {"was": was, "bt": bt}
            res["via"] = each(o, via)
            s.confirm(str(res["via"]))
        return True, f"SVXConnect's Talk via toggled and back on both ({res['via'][H.A]})", res
    except Refused as e:
        return False, str(e), res
    finally:
        _links_put_back(o)
        home_both(o)


# ---- Walkie Talkie mode (Settings > Walkie): the portrait screen, its WLK pages, its channels, its bandscope ----

def _in_walkie(r):
    return win(r) == "" and str(r.cfgv.get("walkie")) == "1"


def wpress(r, key, hold=False, rest=0.7):
    """A key on the walkie screen only (no app window over it, walkie on as last read)"""
    cfg(r, "walkie")
    if not _in_walkie(r):
        raise Refused(f"{r.name}: not on the walkie screen (window {win(r)!r}, walkie {r.cfgv.get('walkie')}): {key} "
                      "not pressed")
    r.take_control()
    r.send(f"key {key} 1")
    time.sleep(1.6 if hold else 0.15)
    r.send(f"key {key} 0")
    time.sleep(rest)


def _wlk_page(r, n):
    """The walkie's buttons on page WLK n (APP shows WLK 1; F1 steps on), read back"""
    texts = fkeys(r)[0]
    if not texts[0].startswith("(WLK"):
        wpress(r, "APP")
    for _ in range(4):
        texts = fkeys(r)[0]
        if texts[0].startswith(f"(WLK {n}:"):
            return texts
        wpress(r, "F1")
    raise Refused(f"{r.name}: no WLK {n} page ({[flat(t) for t in fkeys(r)[0]]})")


@contextlib.contextmanager
def walkie_both(o, files=()):
    """Both radios on the classic screen in Walkie Talkie mode (cfg walkie 1, read back, its message said), on 14.200
    USB (walkie mode takes no digital mode: a USB-D dial would come back USB); after, whatever happened on the way in:
    walkie off (and its microphone back), the files kept put back, page A"""
    import orch_cable as oc
    align(o, 14200000, 2)
    keep = {"freq": 14200000, "mode": 2}
    for r in o.r.values():
        for f in files:
            ssh(r, "set -- save\n" + KEEP.format(f=f))
    try:
        o.changing += 1
        try:
            def enter(n, r):
                oc.classic(r)
                t0 = time.time()
                old, now = bench.cfg_set(r, "walkie", 1)
                if str(now) != "1":
                    raise Refused(f"{n}: walkie reads {now} after asking 1")
                if str(old) == "1":
                    raise Refused(f"{n}: walkie mode was on already (left by an earlier test?)")
                said = wait_msg(r, t0, r"Hold APP to leave walkie mode", 6)
                # entering, as leaving, the radio says nothing for a few seconds (11-26): it answers again first
                wait_cfg(r, "walkie mode answering", lambda v: str(v) == "1", "walkie", 15)
                time.sleep(1.0)
                return said
            said = each(o, enter)
            bench.sync_pair([o.r[H.A], o.r[H.B]], keep)
        finally:
            o.changing -= 1
        yield said
    finally:
        o.changing += 1
        try:
            for n, r in o.r.items():
                if str(r.cfgv.get("walkie")) == "0":
                    continue                    # left already (APP held)
                o.planned[n] = "planned: walkie mode left, the screen turning back to landscape"
                try:
                    r.take_control()
                    r.send("cfg walkie 0")
                    end = time.time() + 15
                    while time.time() < end:
                        try:
                            if str(bench.cfg_get(r, "walkie", timeout=3)) == "0":
                                break
                        except Refused:
                            pass
                        time.sleep(0.5)
                    else:
                        o.st.ev(f"{n}: walkie not turned off", "bad")
                    try:
                        r.wait("its state again", lambda: not r.stale(1.5), 10)
                    except Refused:
                        pass
                finally:
                    o.planned.pop(n, None)
            time.sleep(1.0)
            for r in o.r.values():
                for f in files:
                    try:
                        ssh(r, "set -- back\n" + KEEP.format(f=f))
                    except (Refused, OSError, subprocess.SubprocessError):
                        pass
        finally:
            o.changing -= 1
        home_both(o)


def t_walkie_mode(o):
    """Walkie Talkie mode on both (cfg walkie 1 on the classic screen): 'Hold APP to leave walkie mode' said, no app
    window opens from it ('open 101' refused: 'Not in walkie mode'), the VOL knob is the volume (3 clicks: vol +3, back),
    APP shows the walkie's own pages (WLK 1:4), APP held leaves it (walkie 0)"""
    res = {}
    try:
        with walkie_both(o) as said:
            res["said"] = said
            with o.step("'open 101' (FT8) on both inside walkie mode: no app window opens") as s:
                def no_app(n, r):
                    t0 = time.time()
                    r.send("open 101")
                    time.sleep(2.5)
                    w = win(r)
                    if w:
                        r.send("close")
                        raise Refused(f"{n}: {w!r} opened inside walkie mode")
                    return wait_msg(r, t0, r"Not in walkie mode", 3)
                o.changing += 1
                try:
                    res["open"] = each(o, no_app)
                finally:
                    o.changing -= 1
                H.alike(o, ("freq", "mode"))
                s.confirm(str(res["open"]))
            with o.step("the VOL knob 3 clicks up on both: the volume +3, then back") as s:
                def vol(n, r):
                    v0 = int(r.state.get("vol", 0))
                    r.send("knob vol 3")
                    r.wait("vol +3", lambda: int(r.state.get("vol", -1)) == v0 + 3, 4)
                    r.send("knob vol -3")
                    r.wait("vol back", lambda: int(r.state.get("vol", -1)) == v0, 4)
                    return v0
                res["vol"] = each(o, vol)
                s.confirm(f"from {res['vol']}")
            with o.step("APP on both: the walkie's page WLK 1:4") as s:
                res["wlk1"] = each(o, lambda n, r: [flat(t) for t in _wlk_page(r, 1)])
                s.confirm(str(res["wlk1"]))
            with o.step("APP held on both: walkie mode left (walkie 0)") as s:
                def leave(n, r):
                    # the screen turns back to landscape: each GUI says nothing for ~3 s (11-26: same process)
                    o.planned[n] = "planned: walkie mode left, the screen turning back to landscape"
                    try:
                        wpress(r, "APP", hold=True, rest=1.5)
                        end = time.time() + 15
                        while True:
                            try:
                                if str(bench.cfg_get(r, "walkie", timeout=3)) == "0":
                                    break
                            except Refused:
                                pass
                            if time.time() > end:
                                raise Refused(f"{n}: walkie still on 15 s after APP held")
                            time.sleep(0.5)
                        r.wait("its state again", lambda: not r.stale(1.5), 10)
                    finally:
                        o.planned.pop(n, None)
                    return cfg(r, "walkie")
                o.changing += 1
                try:
                    res["left"] = each(o, leave)
                    align(o)
                finally:
                    o.changing -= 1
                s.confirm(str(res["left"]))
        return True, f"walkie mode: {res['said'][H.A]!r}, no app inside, VOL the volume, WLK pages, APP held left", res
    except Refused as e:
        return False, str(e), res


def _st(r, k):
    v = r.state.get(k)
    return int(v) if isinstance(v, (int, float)) else v


def t_walkie_buttons(o):
    """The walkie's WLK pages on both, each change read back and undone: WLK 1 Band + and Band - (the next band and
    back), MEM/VFO (walkie_mem); WLK 2 AM/FM (the mode), PRE (pre), PRE held (att); WLK 3 AGC (agc), AGC held (split),
    A/B (the VFO), A/B again, Step (the step), Lock (the dial lock). ATU is never pressed (held, it transmits)"""
    res = {}
    try:
        with walkie_both(o):
            def step(n, r, page, key, what, keyname, hold=False, again=True):
                _wlk_page(r, page)
                lbl = flat(fkeys(r)[0][int(key[1]) - 1])
                if not re.search(keyname, lbl, re.I):
                    raise Refused(f"{n}: WLK {page} {key} reads {lbl!r}, not {keyname!r}")
                v0 = _st(r, what) if what != "walkie_mem" else cfg(r, "walkie_mem")
                wpress(r, key, hold=hold)
                get = (lambda: cfg(r, "walkie_mem")) if what == "walkie_mem" else (lambda: _st(r, what))
                end = time.time() + 4
                while get() == v0 and time.time() < end:
                    time.sleep(0.2)
                v1 = get()
                if v1 == v0:
                    raise Refused(f"{n}: WLK {page} {lbl} {'held' if hold else 'pressed'}: {what} stayed {v0}")
                if again:                       # undone the same way (Band + by Band -)
                    pass
                return v0, v1

            def run(n, r):
                out = {}
                f0 = _st(r, "freq")
                out["band+"] = step(n, r, 1, "F3", "freq", r"^Band \+")
                out["band-"] = step(n, r, 1, "F4", "freq", r"^Band -")
                if _st(r, "freq") != f0:
                    bench.ensure(r, {"freq": f0})
                out["mem"] = step(n, r, 1, "F2", "walkie_mem", r"MEM")
                step(n, r, 1, "F2", "walkie_mem", r"MEM")
                m0 = _st(r, "mode")
                out["amfm"] = step(n, r, 2, "F2", "mode", r"AM")
                bench.ensure(r, {"mode": m0})
                out["pre"] = step(n, r, 2, "F5", "pre", r"^PRE")
                step(n, r, 2, "F5", "pre", r"^PRE")
                out["att"] = step(n, r, 2, "F5", "att", r"^PRE", hold=True)
                step(n, r, 2, "F5", "att", r"^PRE", hold=True)
                a0 = _st(r, "agc")
                out["agc"] = step(n, r, 3, "F2", "agc", r"^AGC")
                bench.ensure(r, {"agc": a0})
                out["split"] = step(n, r, 3, "F2", "split", r"^AGC", hold=True)
                step(n, r, 3, "F2", "split", r"^AGC", hold=True)
                out["ab"] = step(n, r, 3, "F3", "vfo", r"^A/B")
                step(n, r, 3, "F3", "vfo", r"^A/B")
                s0 = _st(r, "step")
                out["step"] = step(n, r, 3, "F4", "step", r"^Step")
                if _st(r, "step") != s0:
                    r.send(f"set step {s0}")
                out["lock"] = step(n, r, 3, "F5", "ulock", r"^Lock")
                step(n, r, 3, "F5", "ulock", r"^Lock")
                return out
            with o.step("each WLK button on both, read back and undone") as s:
                o.changing += 1
                try:
                    res = each(o, run, wait=150)
                    align(o)
                finally:
                    o.changing -= 1
                s.confirm(str(res))
        return True, "WLK 1-3 on both: Band +/-, MEM/VFO, AM/FM, PRE, ATT, AGC, split, A/B, Step, Lock, each read back", res
    except Refused as e:
        return False, str(e), res


WALKIE_FILE = "/mnt/walkie_channels.json"


def t_walkie_channels(o):
    """The walkie's 32 channels on both (their file kept aside, put back): a file with channel 5 named BENCH5 on
    14.250 USB is read on entering; Store (WLK 4) puts the dial and mode into the channel shown (walkie_ch) in the file
    and turns MEM on; in MEM the main knob steps the channels (walkie_ch) and a stored one tunes the radio, its name
    and number on the screen (OCR 'CH 05 - BENCH5'); Clear held empties the channel shown"""
    res = {}
    try:
        doc = {"version": 1, "channels": [{"ch": 5, "name": "BENCH5", "frequency": 14250000, "mode": "USB",
                                           "agc": "AUTO", "pre": False, "att": False}]}
        for r in o.r.values():
            ssh(r, "set -- save\n" + KEEP.format(f=WALKIE_FILE))
            ssh(r, f"cat > {WALKIE_FILE} <<'EOF'\n{json.dumps(doc)}\nEOF")
        with walkie_both(o):
            def run(n, r):
                out = {}
                with cfg_kept(r, {"walkie_ch": 3, "walkie_mem": 0}):     # channel 4, empty
                    _wlk_page(r, 4)
                    # Store froze both GUIs ~3 s (no word at all, the same process: 11-26): said, waited for
                    o.planned[n] = "planned: walkie Store (the GUI says nothing for ~3 s)"
                    t_st = time.time()
                    try:
                        wpress(r, "F2")                                   # Store
                        end = time.time() + 15
                        while True:
                            try:
                                if str(bench.cfg_get(r, "walkie_mem", timeout=3)) == "1":
                                    break
                            except Refused:
                                pass
                            if time.time() > end:
                                raise Refused(f"{n}: MEM not on 15 s after Store")
                        r.wait("its state again", lambda: not r.stale(1.5), 10)
                    finally:
                        o.planned.pop(n, None)
                    out["store_s"] = round(time.time() - t_st, 1)
                    d = json.loads(ssh(r, f"cat {WALKIE_FILE}"))
                    c4 = next((c for c in d["channels"] if c.get("ch") == 4), None)
                    f = _st(r, "freq")
                    if not c4 or int(c4.get("frequency", 0)) != f:
                        raise Refused(f"{n}: Store wrote {c4} for the dial {f}")
                    out["stored"] = c4
                    r.send("knob main 1")                                 # channel 5: BENCH5, 14.250 USB
                    wait_cfg(r, "channel 5", lambda v: str(v) == "4", "walkie_ch", 4)
                    r.wait("BENCH5's 14.250", lambda: _st(r, "freq") == 14250000, 4)
                    time.sleep(0.8)
                    p = shot(o, r, "walkie channel 5")
                    t = ocr(p, upright=True)                    # the walkie screen is portrait
                    out["ocr"] = " ".join(t.split())[:160]
                    if not ocr_has(t, "CH 05", "BENCH5"):
                        raise Refused(f"{n}: the screen does not read 'CH 05 - BENCH5' ({out['ocr']!r})")
                    r.send("knob main -1")                                # channel 4 again: the stored dial
                    wait_cfg(r, "channel 4", lambda v: str(v) == "3", "walkie_ch", 4)
                    r.wait("channel 4's dial", lambda: _st(r, "freq") == f, 4)
                    _wlk_page(r, 4)
                    wpress(r, "F3", hold=True)                            # Clear held
                    time.sleep(0.8)
                    d = json.loads(ssh(r, f"cat {WALKIE_FILE}"))
                    if any(c.get("ch") == 4 for c in d["channels"]):
                        raise Refused(f"{n}: Clear held left channel 4 in the file")
                    out["cleared"] = True
                return out
            with o.step("Store, the knob through channels 4 and 5, Clear held, on both") as s:
                o.changing += 1
                try:
                    res = each(o, run, wait=90)
                    align(o)
                finally:
                    o.changing -= 1
                s.confirm(str(res))
        return True, f"walkie channels on both: stored {res[H.A]['stored']}, CH 05 BENCH5 tuned, Clear held", res
    except Refused as e:
        return False, str(e), res
    finally:
        for r in o.r.values():
            try:
                ssh(r, "set -- back\n" + KEEP.format(f=WALKIE_FILE))
            except (Refused, OSError, subprocess.SubprocessError):
                pass


def _region_moves(a, b, box):
    """The share of the box's pixels that differ between two screenshots (by more than 24 in any channel)"""
    from PIL import Image, ImageChops
    x = ImageChops.difference(Image.open(a).convert("RGB").crop(box), Image.open(b).convert("RGB").crop(box))
    px = list(x.getdata())
    return sum(1 for p in px if max(p) > 24) / max(1, len(px))


WALKIE_SCOPE = (232, 8, 410, 366)   # the bandscope in the walkie screen's picture (landscape px, 800 x 480)
WALKIE_SPAN = (408, 8, 440, 366)    # its band edges and span, written downwards beside it


def t_walkie_scope(o):
    """The walkie's bandscope on both with the main spectrum turned off (spectrum_show 0, put back): two pictures a
    second apart differ (the trace moves), the zoom one step up changes the span under it (OCR)"""
    res = {}
    try:
        with cfg_kept(o.r[H.A], {"spectrum_show": 0}), cfg_kept(o.r[H.B], {"spectrum_show": 0}):
            with walkie_both(o):
                time.sleep(2.0)
                with o.step("two pictures a second apart on both: the screen moves") as s:
                    def moving(n, r):
                        a = shot(o, r, "walkie scope 1")
                        time.sleep(1.0)
                        b = shot(o, r, "walkie scope 2")
                        share = _region_moves(a, b, WALKIE_SCOPE)     # the scope's own box
                        if share < 0.003:
                            raise Refused(f"{n}: the walkie bandscope did not move ({share:.4f} of it differs)")
                        return round(share, 4), ocr(b)
                    got = each(o, moving, wait=60)
                    res["moves"] = {n: g[0] for n, g in got.items()}
                    s.confirm(str(res["moves"]))
                with o.step("the zoom one step up on both: the span text changes") as s:
                    def zoom(n, r):
                        z0 = _st(r, "zoom")
                        t0 = ocr(shot(o, r, "walkie zoom before"), WALKIE_SPAN, upright=True, psm=6)
                        z1 = z0 // 2 if z0 > int(r.state.get("zmin", 1) or 1) else z0 * 2     # x8 is the top
                        r.send(f"set zoom {z1}")
                        r.wait("zoom", lambda: _st(r, "zoom") != z0, 4)
                        time.sleep(1.5)
                        t1 = ocr(shot(o, r, "walkie zoom after"), WALKIE_SPAN, upright=True, psm=6)
                        r.send(f"set zoom {z0}")
                        r.wait("zoom back", lambda: _st(r, "zoom") == z0, 4)
                        span0 = re.findall(r"\d+(?:[.,]\d+)?\s*k", t0)
                        span1 = re.findall(r"\d+(?:[.,]\d+)?\s*k", t1)
                        if span0 == span1:
                            raise Refused(f"{n}: the span read {span0} before and {span1} after the zoom")
                        return span0, span1
                    res["zoom"] = each(o, zoom, wait=60)
                    s.confirm(str(res["zoom"]))
        return True, f"walkie bandscope with the spectrum off: moving ({res['moves']}), span {res['zoom'][H.A]}", res
    except Refused as e:
        return False, str(e), res


# ---- FT8: Time Sync, Log Map, a typed dial ----

def _clock_off(r):
    """r's clock minus this computer's (s), from a round trip over SSH (its middle)"""
    t0 = time.time()
    t = float(ssh(r, "python3 -c 'import time;print(time.time())'"))
    t1 = time.time()
    return t - (t0 + t1) / 2


def _ft8_open(o):
    H.ft8_both(o, 14074000)
    for r in o.r.values():
        r.wait("the FT8 window", lambda r=r: win(r) == "FT8", 5)


def _ft8_close(o):
    for r in o.r.values():
        r.send("ft8 close")
    for r in o.r.values():
        try:
            r.wait("FT8 closed", lambda r=r: not r.ft8.get("open"), 8)
        except Refused:
            pass


def _slot_start(period=15.0, lead=0.05):
    """Waits for this computer's next slot start (FT8's 15 s), lead s early"""
    now = time.time()
    nxt = (int(now // period) + 1) * period
    time.sleep(max(0.0, nxt - now - lead))
    return nxt


def t_ft8_time_sync(o):
    """FT8's Time Sync (page 4) on both: with NTP keeping the clock it is left alone ('Clock in sync (NTP...)', the
    clock still this computer's within 0.3 s); then on each radio in turn, NTP stopped and its clock put 3 s ahead,
    Time Sync pressed at a slot start moves it to the slot ('Clock moved', within 0.5 s of this computer's), and NTP
    started again (the clock right after, checked)"""
    res = {}
    names = ("FT8",)
    try:
        _ft8_open(o)
        with o.step("Time Sync on both, NTP keeping the clocks: 'Clock in sync', nothing moved") as s:
            def kept(n, r):
                off0 = _clock_off(r)
                t0 = time.time()
                press_label(r, r"^Time\s*Sync", names)
                said = wait_msg(r, t0, r"Clock in sync|GPS keeps the clock", 5)
                off1 = _clock_off(r)
                if abs(off1 - off0) > 0.3:
                    raise Refused(f"{n}: its clock moved {off1 - off0:+.2f} s though NTP keeps it")
                return {"said": said, "off": round(off1, 3)}
            res["kept"] = each(o, kept)
            s.confirm(str(res["kept"]))
        for n in (H.A, H.B):
            r = o.r[n]
            with o.step(f"{n}: NTP stopped, its clock 3 s ahead; Time Sync at a slot start: moved back to the slot") as s:
                try:
                    ssh(r, "/etc/init.d/S49ntp stop >/dev/null 2>&1; "
                           "python3 -c 'import time;time.clock_settime(time.CLOCK_REALTIME, time.time() + 3)'")
                    off_set = _clock_off(r)
                    if not 2.0 < off_set < 4.0:
                        raise Refused(f"{n}: its clock {off_set:+.2f} s off after setting it 3 s ahead")
                    _slot_start(lead=0.08)
                    t0 = time.time()
                    r.take_control()
                    k = fkey_of(r, r"^Time\s*Sync")
                    if not k:
                        raise Refused(f"{n}: Time Sync not on the page shown")
                    press(r, k, names, rest=0.3)
                    said = wait_msg(r, t0, r"Clock moved|on the slot already|can't be set", 6)
                    off_after = _clock_off(r)
                    res[n] = {"set": round(off_set, 2), "said": said, "after": round(off_after, 3)}
                    if "moved" not in said or abs(off_after) > 0.5:
                        raise Refused(f"{n}: 3 s ahead, Time Sync said {said!r}, its clock now {off_after:+.2f} s off")
                finally:
                    ssh(r, "/etc/init.d/S49ntp start >/dev/null 2>&1; true")
                time.sleep(3.0)
                res[n]["ntp_back"] = round(_clock_off(r), 3)
                s.confirm(str(res[n]))
        return True, (f"Time Sync: {res['kept'][H.A]['said']!r} with NTP; 3 s ahead it moved {H.A}'s clock to "
                      f"{res[H.A]['after']:+.2f} s, {H.B}'s to {res[H.B]['after']:+.2f} s; NTP started again"), res
    except Refused as e:
        return False, str(e), res
    finally:
        for r in o.r.values():
            try:
                ssh(r, "pidof ntpd >/dev/null || /etc/init.d/S49ntp start >/dev/null 2>&1; true")
            except (Refused, OSError, subprocess.SubprocessError):
                pass
        _ft8_close(o)
        home_both(o)


def _changed(a, b, box=(0, 0, 800, 480)):
    return _region_moves(a, b, box)


def t_ft8_log_map(o):
    """FT8's Log Map (page 5) on both: pressed, the map ('QSO map - N stations' read on its picture); pressed again,
    the list back (no map caption)"""
    res = {}
    names = ("FT8",)
    try:
        _ft8_open(o)

        def one(n, r):
            _to_label(r, r"^Log\s*Map", names)
            a = shot(o, r, "ft8 before the log map")
            k = fkey_of(r, r"^Log\s*Map")
            press(r, k, names, rest=2.0)
            b = shot(o, r, "ft8 log map")
            press(r, k, names, rest=1.5)
            c = shot(o, r, "ft8 after the log map")
            # the map's own caption ("QSO map - N stations") on its picture, gone again after (the screen is dark
            # either way: a share of changed pixels says little, 10 % on 2026-11-26)
            before, during, after = (" ".join(ocr(x).split()) for x in (a, b, c))
            cap = re.compile(r"map\s*-\s*\S+\s*stat", re.I)        # "QSO map - 0 stations" (OCR: "OS0 map - @ stati")
            if not cap.search(during) or cap.search(after) or cap.search(before):
                raise Refused(f"{n}: the map's caption read before {bool(cap.search(before))}, open "
                              f"{bool(cap.search(during))}, after {bool(cap.search(after))} ({during[:120]!r})")
            return cap.search(during).group(0)
        with o.step("Log Map pressed twice on both: the map, then the list again") as s:
            res = each(o, one, wait=90)
            s.confirm(str(res))
        return True, f"FT8's Log Map opened and closed on both ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        _ft8_close(o)
        home_both(o)


def t_ft8_custom_freq(o):
    """FT8's Freq (page 3) on both: a press lists the mode's presets (one chosen: its label said); held, a dial typed
    in kHz (14091) -> 14.091000 USB-D, 'FT8 Custom 14.091', the button 'Freq: 14.091'; a band key goes back to the
    presets (the button without the custom dial); back on 20 m"""
    res = {}
    names = ("FT8",)
    try:
        _ft8_open(o)
        with o.step("Freq held on both, 14091 typed: 14.091 USB-D, said, on the button") as s:
            def typed(n, r):
                k = _to_label(r, r"^Freq", names)
                t0 = time.time()
                press(r, k, names, hold=True, rest=1.0)
                r.send("type 14091")
                said = wait_msg(r, t0, r"FT8 Custom 14\.091", 5)
                r.wait("14.091 USB-D", lambda: int(r.state.get("freq", 0)) == 14091000 and
                       int(r.state.get("mode", -1)) == 3, 4)
                lbl = _label(r, k)
                if "14.091" not in lbl:
                    raise Refused(f"{n}: the Freq button reads {lbl!r}")
                return {"said": said, "label": lbl}
            o.changing += 1
            try:
                res["typed"] = each(o, typed)
            finally:
                o.changing -= 1
            H.alike(o, ("freq", "mode"))
            s.confirm(str(res["typed"]))
        with o.step("BAND_UP then BAND_DOWN on both: the presets again (20 m's 14.074), the button without 14.091") as s:
            def back(n, r):
                for key in ("BAND_UP", "BAND_DOWN"):
                    f0 = int(r.state.get("freq", 0))
                    bench.tap(r, key)
                    r.wait("the band moved", lambda: int(r.state.get("freq", 0)) != f0, 5)
                    time.sleep(0.8)
                k = _to_label(r, r"^Freq", names)
                lbl = _label(r, k)
                f = int(r.state.get("freq", 0))
                if f != 14074000 or "14.091" in lbl:
                    raise Refused(f"{n}: after the band keys on {f}, the button {lbl!r}")
                return lbl
            o.changing += 1
            try:
                res["presets"] = each(o, back)
            finally:
                o.changing -= 1
            H.alike(o, ("freq", "mode"))
            s.confirm(str(res["presets"]))
        with o.step("Freq pressed on both: the preset list, the MFK's press on its row: the preset said") as s:
            def preset(n, r):
                k = _to_label(r, r"^Freq", names)
                t0 = time.time()
                press(r, k, names, rest=1.0)
                press(r, "MFK", names, rest=0.8)
                said = wait_msg(r, t0, r"FT8|20m|14\.074", 4)
                return said
            o.changing += 1
            try:
                res["preset"] = each(o, preset)
            finally:
                o.changing -= 1
            H.alike(o, ("freq", "mode"))
            s.confirm(str(res["preset"]))
        return True, (f"FT8's typed dial {res['typed'][H.A]['said']!r}, button {res['typed'][H.A]['label']!r}; band "
                      f"keys back to the presets; a preset chosen ({res['preset'][H.A]!r})"), res
    except Refused as e:
        return False, str(e), res
    finally:
        _ft8_close(o)
        home_both(o)


# ---- PSK: its squelch on the quality ----

def _psk_lines(r, since_lines):
    day = time.strftime("%Y-%m-%d", time.gmtime())
    out = ssh(r, f"tail -n +{since_lines + 1} /mnt/digi_text/{day}.txt 2>/dev/null | grep -c ' PSK.. RX ' || true")
    return int(out.strip() or 0)


def _psk_new_q(r, since_lines):
    """The quality (Q%) of each PSK RX line the text log got after since_lines"""
    day = time.strftime("%Y-%m-%d", time.gmtime())
    out = ssh(r, f"tail -n +{since_lines + 1} /mnt/digi_text/{day}.txt 2>/dev/null | grep ' PSK.. RX ' || true")
    return [int(m.group(1)) for m in re.finditer(r" RX Q(\d+)%", out)]


def _psk_count(r):
    day = time.strftime("%Y-%m-%d", time.gmtime())
    return int((ssh(r, f"wc -l < /mnt/digi_text/{day}.txt 2>/dev/null || echo 0").strip() or 0))


def t_psk_squelch(o):
    """PSK31's squelch on both (page 2): its press gives the MFK the threshold ('Knob: squelch on the quality'), 3
    clicks down read on the button (68 -> 65) and kept in /mnt/psk.conf when the knob is given back; held, off ('PSK31
    squelch off') and on again; with the squelch on the quiet cable writes no RX text in 30 s, off it writes the
    noise's; everything put back (the file kept aside)"""
    import suite_air_b as ab
    res = {}
    names = ("PSK31", "PSK63")
    tl0 = {n: ab.tl_sizes(r.host) for n, r in o.r.items()}
    try:
        with file_kept(o.r[H.A], "/mnt/psk.conf"), file_kept(o.r[H.B], "/mnt/psk.conf"), \
                cfg_kept(o.r[H.A], {"textlog": 1}), cfg_kept(o.r[H.B], {"textlog": 1}):
            with o.step("PSK open on both (open 121), each where its app last was (it holds that dial), alike") as s:
                res["open"] = open_both(o, 121, "PSK")
                H.alike(o, ("freq", "mode"))
                s.confirm(f"{res['open']} on {o.r[H.A].state.get('freq')}")
            with o.step("30 s, the squelch at its threshold as found: RX text only at or over it (the cable quiet)") as s:
                c0 = {n: _psk_count(r) for n, r in o.r.items()}
                q0 = {}
                for n, r in o.r.items():
                    m = re.search(r"SQL (\d+)", _label(r, _to_label(r, r"^SQL", names)))
                    q0[n] = int(m.group(1)) if m else None
                time.sleep(30)
                on = {n: _psk_new_q(r, c0[n]) for n, r in o.r.items()}
                res["lines_on"] = {n: (q0[n], qs) for n, qs in on.items()}
                low = {n: [q for q in qs if q0[n] is not None and q < q0[n]] for n, qs in on.items()}
                if any(low.values()):
                    raise Refused(f"RX text under the squelch's threshold written: {res['lines_on']}")
                s.confirm(f"threshold and the lines' Q: {res['lines_on']}")
            with o.step("SQL on both: the knob takes it, 3 clicks down, given back: the button and psk.conf") as s:
                def sql(n, r):
                    k = _to_label(r, r"^SQL", names)
                    v0 = _label(r, k)
                    m = re.search(r"SQL (\d+)", v0)
                    if not m:
                        raise Refused(f"{n}: the SQL button reads {v0!r} (off?)")
                    q0 = int(m.group(1))
                    t0 = time.time()
                    press(r, k, names, rest=0.6)
                    said = wait_msg(r, t0, r"squelch on the quality", 4)
                    turn(r, "mfk", -3, names, rest=0.8)
                    lbl = _label(r, k)
                    press(r, k, names, rest=1.0)                 # the knob given back: saved
                    conf = ssh(r, "cat /mnt/psk.conf 2>/dev/null")
                    if f"SQL {q0 - 3}" not in lbl or f"squelch_q={q0 - 3}" not in conf:
                        raise Refused(f"{n}: 3 clicks down from {q0}: the button {lbl!r}, psk.conf {conf!r}")
                    return {"q0": q0, "said": said, "label": lbl}
                res["sql"] = each(o, sql)
                s.confirm(str(res["sql"]))
            with o.step("SQL held on both: off ('PSK31 squelch off'); 30 s: the noise's text written") as s:
                def off(n, r):
                    k = _to_label(r, r"^SQL", names)
                    t0 = time.time()
                    press(r, k, names, hold=True, rest=0.8)
                    said = wait_msg(r, t0, r"squelch off", 4)
                    if "off" not in _label(r, k).lower():
                        raise Refused(f"{n}: the button reads {_label(r, k)!r} after the hold")
                    return said
                res["off"] = each(o, off)
                c1 = {n: _psk_count(r) for n, r in o.r.items()}
                time.sleep(30)
                noise = {n: _psk_lines(r, c1[n]) for n, r in o.r.items()}
                res["lines_off"] = noise
                if not all(noise.values()):
                    raise Refused(f"the squelch off, no RX text in 30 s: {noise}")
                s.confirm(f"{res['off']}; lines {noise}")
            with o.step("SQL held again on both: on at the threshold set; 3 clicks up: as found") as s:
                def back(n, r):
                    k = _to_label(r, r"^SQL", names)
                    press(r, k, names, hold=True, rest=0.8)
                    press(r, k, names, rest=0.6)
                    turn(r, "mfk", 3, names, rest=0.8)
                    press(r, k, names, rest=1.0)
                    return _label(r, k)
                res["back"] = each(o, back)
                s.confirm(str(res["back"]))
        return True, (f"PSK31's squelch: {res['sql'][H.A]['said']!r}, 3 clicks {res['sql'][H.A]['label']!r} kept in "
                      f"psk.conf; on: no text in 30 s, off: {res['lines_off']} lines of noise"), res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        for n, r in o.r.items():
            try:
                H.textlog_back(r.host, tl0[n])          # the noise's lines out of the radio's text log
            except Exception as e:  # noqa: BLE001
                o.st.ev(f"{n}: its text log not cut back: {e}", "bad")


# ---- RADE: its frequency list and its power ----

def _tci_hold(o):
    """A TCI client on each radio (RADE opens only with headphones, TCI or the browser): {name: session}"""
    return {n: H.tci_open(r.host)[0] for n, r in o.r.items()}


def t_rade(o):
    """RADE on both (a TCI client connected to each, as RADE wants): Freq. from 14.236 USB: its row chosen, 'FreeDV
    14236.0 kHz', 14.236 USB; a row up (the list in frequency order): 40 m's 7197 in LSB; Power (page 2) steps
    0.5/1/2/3 W round, rade_pwr in
    tenths following, kept after closing and opening again; put back as found"""
    res = {}
    names = ("RADE",)
    tci = {}
    try:
        tci = _tci_hold(o)
        with o.step("RADE open on both (open 128), the dial 14.236 USB") as s:
            res["open"] = open_both(o, 128, "RADE", 14236000, 2)
            s.confirm(str(res["open"]))
        with o.step("Freq. on both: 14.236's row, then a row up: 7.197 LSB") as s:
            def freqs(n, r):
                out = {}
                k = _to_label(r, r"^Freq", names)
                t0 = time.time()
                press(r, k, names, rest=1.0)
                press(r, "MFK", names, rest=0.8)
                out["usb"] = wait_msg(r, t0, r"FreeDV 14236\.0 kHz", 4)
                r.wait("14.236 USB", lambda: int(r.state.get("freq", 0)) == 14236000 and
                       int(r.state.get("mode", -1)) == 2, 4)
                t1 = time.time()
                press(r, k, names, rest=1.0)
                turn(r, "mfk", -1, names, rest=0.6)          # the list in frequency order: 14236's row up one is 7197
                press(r, "MFK", names, rest=0.8)
                said = wait_msg(r, t1, r"FreeDV \d+\.\d kHz", 4)
                khz = float(re.search(r"FreeDV (\d+\.\d) kHz", said).group(1))
                if not 7000 <= khz <= 7300:
                    raise Refused(f"{n}: one row up from 14236 gave {said!r}, not a 40 m row")
                r.wait(f"{khz} kHz in LSB", lambda: int(r.state.get("freq", 0)) == int(khz * 1000) and
                       int(r.state.get("mode", -1)) == 0, 4)
                out["lsb"] = said
                return out
            o.changing += 1
            try:
                res["freq"] = each(o, freqs)
            finally:
                o.changing -= 1
            H.alike(o, ("freq", "mode"))
            s.confirm(str(res["freq"]))
        with o.step("Power on both: 0.5/1/2/3 W round, rade_pwr following; kept over a close and an open") as s:
            def power(n, r):
                k = _to_label(r, r"^Power", names)
                seen = [(_label(r, k), cfg(r, "rade_pwr"))]
                for _ in range(4):
                    press(r, k, names, rest=0.6)
                    seen.append((_label(r, k), cfg(r, "rade_pwr")))
                vals = [int(v) for _, v in seen]
                if vals[0] != vals[-1] or sorted(set(vals)) != [5, 10, 20, 30]:
                    raise Refused(f"{n}: Power stepped {seen}")
                for lbl, v in seen:
                    w = int(v) / 10
                    if f"{w:g} W" not in lbl:
                        raise Refused(f"{n}: the button {lbl!r} with rade_pwr {v}")
                press(r, k, names, rest=0.6)                    # one step on: kept over a close and an open?
                want = cfg(r, "rade_pwr")
                return {"seen": seen, "want": want}
            got = each(o, power)
            close_both(o)
            open_both(o, 128, "RADE")
            for n, r in o.r.items():
                k = _to_label(r, r"^Power", names)
                now = cfg(r, "rade_pwr")
                if now != got[n]["want"] or f"{int(now) / 10:g} W" not in _label(r, k):
                    raise Refused(f"{n}: after a close and an open rade_pwr {now}, the button {_label(r, k)!r} "
                                  f"(set {got[n]['want']})")
                for _ in range(3):                              # round to the one found
                    if cfg(r, "rade_pwr") == got[n]["seen"][0][1]:
                        break
                    press(r, k, names, rest=0.6)
            res["power"] = got
            s.confirm(str({n: g["seen"] for n, g in got.items()}))
        return True, (f"RADE: {res['freq'][H.A]['usb']!r}, then {res['freq'][H.A]['lsb']!r} in LSB; Power "
                      f"{[x[0] for x in res['power'][H.A]['seen']]} kept over a reopen"), res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        for t in tci.values():
            try:
                t.close()
            except OSError:
                pass


# ---- WeFax: its stations and the WEFAX channels, View, the fax browser, Power ----

WEFAX_NAMES = ("WeFax",)


def _wefax_pick(o, dial, pattern):
    """Both on dial (USB), Freq. (page 2) listed, the row the dial's (the list starts on it) taken by the MFK's press:
    what each radio said; then both on that row's dial"""
    align(o, dial, 2)

    def one(n, r):
        k = _to_label(r, r"^Freq", WEFAX_NAMES)
        t0 = time.time()
        press(r, k, WEFAX_NAMES, rest=1.2)
        p, txt = shot_text(o, r, "wefax freq list")
        press(r, "MFK", WEFAX_NAMES, rest=0.8)
        said = wait_msg(r, t0, pattern, 4)
        r.wait("its dial in USB", lambda: int(r.state.get("freq", 0)) == dial and int(r.state.get("mode", -1)) == 2,
               4)
        return {"said": said, "ocr": " ".join(txt.split())[:400]}
    o.changing += 1
    try:
        got = each(o, one)
    finally:
        o.changing -= 1
    H.alike(o, ("freq", "mode"))
    return got


def t_wefax_freq(o):
    """WeFax's Freq. list on both: a station row (DDK3 7880.0: the dial 1.5 kHz under it, 7.8785 USB, said 'DDK3
    7880.0 kHz'); a channel 'WEFAX BENCH' at 7793.5 kHz added on the Memories page shows under the list's Channels by
    the rest of its name, its row the channel's dial (said 'BENCH 7795.0 kHz', 7.7935 USB); the channel taken out
    again (the channels file put back)"""
    res = {}
    try:
        for r in o.r.values():
            ssh(r, "set -- save\n" + KEEP.format(f="/mnt/channels.json"))
        with o.step("'WEFAX BENCH' at 7793.5 kHz added on both radios' Memories pages") as s:
            res["added"] = each(o, lambda n, r: _ch_add(r, "WEFAX BENCH", 7793500)[0])
            s.confirm(str(res["added"]))
        with o.step("WeFax open on both (open 109)") as s:
            res["open"] = open_both(o, 109, "WeFax")
            s.confirm(str(res["open"]))
        with o.step("Freq. from 7.8785 USB: DDK3's row, 'DDK3 7880.0 kHz'") as s:
            res["station"] = _wefax_pick(o, 7878500, r"DDK3 7880\.0 kHz")
            s.confirm(str({n: g["said"] for n, g in res["station"].items()}))
        with o.step("Freq. from 7.7935 USB: the channel's row, 'BENCH 7795.0 kHz', on the list's screen") as s:
            res["channel"] = _wefax_pick(o, 7793500, r"BENCH 7795\.0 kHz")
            s.confirm(str({n: g["said"] for n, g in res["channel"].items()}))
        return True, (f"WeFax: {res['station'][H.A]['said']!r}; the WEFAX channel listed and tuned as "
                      f"{res['channel'][H.A]['said']!r} (dial 1.5 kHz under the station)"), res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        for r in o.r.values():
            try:
                ssh(r, "set -- back\n" + KEEP.format(f="/mnt/channels.json"))
            except (Refused, OSError, subprocess.SubprocessError):
                pass


def t_wefax_view(o):
    """WeFax's View (page 2) on both: Big at its open; Full Size, Small and Big again a press each, read on the button,
    the screen changing with each (pictures)"""
    res = {}
    try:
        with o.step("WeFax open on both: View reads Big") as s:
            open_both(o, 109, "WeFax")
            got = each(o, lambda n, r: _label(r, _to_label(r, r"^View", WEFAX_NAMES)))
            if not all("Big" in v for v in got.values()):
                raise Refused(f"View at the open: {got}")
            s.confirm(str(got))
        with o.step("View pressed three times on both: Full Size, Small, Big; the screen changes each time") as s:
            def views(n, r):
                k = _to_label(r, r"^View", WEFAX_NAMES)
                seen, shots = [_label(r, k)], [shot(o, r, "wefax view Big")]
                for _ in range(3):
                    press(r, k, WEFAX_NAMES, rest=1.2)
                    seen.append(_label(r, k))
                    shots.append(shot(o, r, f"wefax view {seen[-1]}"))
                words = [re.sub(r"^View\s*", "", x) for x in seen]
                if set(words[:3]) != {"Big", "Small", "Full Size"} or words[3] != words[0]:
                    raise Refused(f"{n}: View stepped {words}")
                moves = [round(_region_moves(a, b, (0, 0, 800, 400)), 3) for a, b in zip(shots, shots[1:])]
                # with no fax yet Big and Full Size look alike (11-26: 0.0); Small brings the main waterfall back
                if max(moves) < 0.02:
                    raise Refused(f"{n}: the screen hardly changed between views ({moves})")
                return {"views": words, "moves": moves}
            res = each(o, views, wait=60)
            s.confirm(str(res))
        return True, f"WeFax View Big -> Small -> Full Size -> Big on both, each a different screen ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


WEFAX_TEST_PNG = "wefax-20371231-235959.png"   # newest by its name, yet inside the radio's 32-bit time_t


def _test_png(path, text, size=(800, 600)):
    """A picture with text big enough to read back by OCR"""
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("RGB", size, "white")
    d = ImageDraw.Draw(im)
    font = None
    for f in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Helvetica.ttc"):
        if os.path.exists(f):
            font = ImageFont.truetype(f, 110)
            break
    d.text((40, size[1] // 3), text, fill="black", font=font or ImageFont.load_default())
    im.save(path)
    return path


def t_wefax_browser(o):
    """WeFax's fax browser on both (All Charts, page 1): a test fax put in /mnt/wefax (newest) is in it: the browser's
    keys (Open, Newest, Delete, < Fax), Newest and Open show it (its big text 'QXZ 4729' read back by OCR); the test
    fax and its thumbnail taken out after"""
    import tempfile
    res = {}
    png = _test_png(os.path.join(tempfile.mkdtemp(), WEFAX_TEST_PNG), "QXZ 4729")
    try:
        for r in o.r.values():
            subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", png, f"root@{r.host}:/mnt/wefax/{WEFAX_TEST_PNG}"],
                           check=True, timeout=60)
        with o.step("WeFax open on both; All Charts: the browser's keys") as s:
            open_both(o, 109, "WeFax")

            def browse(n, r):
                k = _to_label(r, r"^All\s*Charts$", WEFAX_NAMES)
                press(r, k, WEFAX_NAMES, rest=2.0)
                texts = [flat(t) for t in fkeys(r)[0]]
                if not any(t.startswith("Open") for t in texts) or not any("Fax" in t for t in texts):
                    raise Refused(f"{n}: the browser's keys not shown ({texts})")
                return texts
            res["keys"] = each(o, browse)
            s.confirm(str(res["keys"]))
        with o.step("Newest, then Open on both: the test fax on the screen ('BENCH FAX' read)") as s:
            def show(n, r):
                press_label(r, r"^Newest", WEFAX_NAMES, rest=1.0)
                press_label(r, r"^Open", WEFAX_NAMES, rest=2.5)
                p, txt = shot_text(o, r, "wefax browser open", psm=6)
                # the fax pane alone, its dim picture stretched (QXZ 4729 shown in 3-71 of 255: not read whole-screen)
                txt += "\n" + ocr(p, box=(15, 120, 405, 420), psm=6, stretch=True)
                if not ocr_has(txt, "QXZ") and not ocr_has(txt, "4729"):
                    raise Refused(f"{n}: the opened fax does not read QXZ 4729 ({' '.join(txt.split())[:120]!r})")
                return " ".join(txt.split())[:80]
            res["open"] = each(o, show, wait=60)
            s.confirm(str(res["open"]))
        return True, f"WeFax's browser lists the saved fax and opens it on both ({res['open']})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        for r in o.r.values():
            try:
                ssh(r, f"rm -f /mnt/wefax/{WEFAX_TEST_PNG} /mnt/wefax/thumbs/{WEFAX_TEST_PNG}* "
                       f"/mnt/wefax/thumbs/*20371231-235959*")
            except (Refused, OSError, subprocess.SubprocessError):
                pass


def _power_steps(o, names, label):
    """The app's Power button on both from 0.1 W: three presses step 0.2, 0.5, 1 W, the state's pwr and the button
    alike; then 0.1 W again"""
    o.both({"pwr": H.W})

    def one(n, r):
        k = _to_label(r, r"^Power", names)
        seen = [(float(r.state.get("pwr", 0)), _label(r, k))]
        for _ in range(3):
            p0 = float(r.state.get("pwr", 0))
            press(r, k, names, rest=0.8)
            r.wait("the power stepped", lambda: float(r.state.get("pwr", 0)) != p0, 4)
            seen.append((float(r.state.get("pwr", 0)), _label(r, k)))
        if [x[0] for x in seen] != [0.1, 0.2, 0.5, 1.0]:
            raise Refused(f"{n}: {label}'s Power stepped {seen}")
        bad = [x for x in seen if f"{x[0]:g} W" not in x[1]]
        if bad:
            raise Refused(f"{n}: the button and the state differ: {bad}")
        return seen
    try:
        return each(o, one)
    finally:
        o.both({"pwr": H.W})


def t_wefax_power(o):
    """WeFax's Power (page 3) on both: 0.1 -> 0.2 -> 0.5 -> 1 W a press each, the state's pwr and the button alike (the
    apps' own steps); 0.1 W again after"""
    res = {}
    try:
        open_both(o, 109, "WeFax")
        with o.step("Power pressed three times on both") as s:
            res = _power_steps(o, WEFAX_NAMES, "WeFax")
            s.confirm(str(res))
        return True, f"WeFax's Power stepped {[x[0] for x in res[H.A]]} W on both, the button alike", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- SSTV: its picture browser and Delete, its TX folders and the web Gallery ----

SSTV_TEST = "sstv-20991231-235959"
SSTV_NAMES = ("SSTV",)


def _gallery(r, kind):
    st, d = http_json(r, f"/api/gallery?kind={kind}")
    if st != 200:
        raise Refused(f"{r.name}: GET /api/gallery?kind={kind}: {st}")
    items = d if isinstance(d, list) else d.get("items") or d.get("files") or []
    return [x.get("name") for x in items]


def t_sstv_browser(o):
    """SSTV's RX Pics Browser on both (page 2): two test pictures (a picture and its _dn) put in /mnt/sstv; the web
    Gallery lists both, _dn first; the browser opens ('Close Browser'), its first row (the newest: the _dn) taken by
    the knob, Delete picture deletes that file at once; the Gallery's DELETE takes the other; nothing else touched"""
    import tempfile
    res = {}
    d = tempfile.mkdtemp()
    files = [f"{SSTV_TEST}.png", f"{SSTV_TEST}_dn.png"]
    for f in files:
        _test_png(os.path.join(d, f), "BENCH", (320, 256))
    try:
        others0 = {n: sorted(set(ssh(r, "ls /mnt/sstv/ | grep '^sstv-' || true").split()) - set(files))
                   for n, r in o.r.items()}
        for r in o.r.values():
            subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes"] + [os.path.join(d, f) for f in files] +
                           [f"root@{r.host}:/mnt/sstv/"], check=True, timeout=60)
        with o.step("the web Gallery on both: both test pictures, the _dn first") as s:
            def gal(n, r):
                names = _gallery(r, "sstv")
                mine = [x for x in names if x and x.startswith(SSTV_TEST)]
                if mine != [files[1], files[0]]:
                    raise Refused(f"{n}: the Gallery lists {mine} of the test pictures ({names[:4]})")
                return mine
            res["gallery"] = each(o, gal)
            s.confirm(str(res["gallery"]))
        with o.step("SSTV open on both; RX Pics Browser: 'Close Browser'; the knob on row 1; Delete picture") as s:
            open_both(o, 112, "SSTV")

            def delete(n, r):
                k = _to_label(r, r"RX Pics", SSTV_NAMES)
                press(r, k, SSTV_NAMES, rest=1.5)
                if "Close" not in _label(r, k):
                    raise Refused(f"{n}: the browser's key reads {_label(r, k)!r}")
                turn(r, "mfk", 1, SSTV_NAMES, rest=0.8)
                kd = fkey_of(r, r"^Delete")
                if not kd or fkeys(r)[1][int(kd[1]) - 1] & 2:
                    raise Refused(f"{n}: Delete picture not there or greyed ({[flat(t) for t in fkeys(r)[0]]})")
                p = shot(o, r, "sstv browser before delete")
                press(r, kd, SSTV_NAMES, rest=1.5)
                left = ssh(r, f"ls /mnt/sstv/ | grep '^{SSTV_TEST}' || true").split()
                if len(left) != 1 or left[0] not in files:
                    raise Refused(f"{n}: after Delete picture /mnt/sstv has {left} of the test pictures")
                press(r, k, SSTV_NAMES, rest=1.0)               # the browser closed
                return left
            res["deleted"] = each(o, delete, wait=60)
            s.confirm(str(res["deleted"]))
        with o.step("the Gallery's DELETE on both: the other test picture gone; the owner's pictures untouched") as s:
            def gdel(n, r):
                st, txt = http(r, f"/api/gallery/sstv/{res['deleted'][n][0]}", "DELETE")
                left = ssh(r, f"ls /mnt/sstv/ | grep '^{SSTV_TEST}' || true").split()
                others = sorted(set(ssh(r, "ls /mnt/sstv/ | grep '^sstv-' || true").split()) - set(files))
                if st != 200 or left or others != others0[n]:
                    raise Refused(f"{n}: DELETE {st} {txt[:80]}, left {left}, the others changed {others != others0[n]}")
                return st
            res["gdel"] = each(o, gdel)
            s.confirm(str(res["gdel"]))
        return True, "SSTV's browser and Delete picture on both; the Gallery lists _dn first and deletes", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        for r in o.r.values():
            try:
                ssh(r, f"rm -f /mnt/sstv/{SSTV_TEST}*.png")
            except (Refused, OSError, subprocess.SubprocessError):
                pass


def _upload(r, path, url):
    """A multipart upload (field 'file') to r's web server: (status, text)"""
    import uuid
    b = uuid.uuid4().hex
    name = os.path.basename(path)
    body = (f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
            f"Content-Type: image/png\r\n\r\n").encode() + open(path, "rb").read() + f"\r\n--{b}--\r\n".encode()
    return http(r, url, "POST", raw=body, ctype=f"multipart/form-data; boundary={b}")


def t_sstv_tx_files(o):
    """SSTV's TX folders on both: /mnt/sstv/tx_pics and tx_text hold the defaults; a PNG uploaded on the web Gallery
    (POST /api/sstv/tx_pics) lands in tx_pics and is listed (kind sstv_tx); taken out again by its DELETE"""
    import tempfile
    res = {}
    png = _test_png(os.path.join(tempfile.mkdtemp(), "bench_tx.png"), "BENCH TX", (320, 256))
    try:
        def one(n, r):
            pics = ssh(r, "ls /mnt/sstv/tx_pics").split()
            texts = ssh(r, "ls /mnt/sstv/tx_text").split("\n")
            if not pics or not [t for t in texts if t.strip()]:
                raise Refused(f"{n}: tx_pics {pics}, tx_text {texts}")
            st, txt = _upload(r, png, "/api/sstv/tx_pics?unique=1")
            if st != 200:
                raise Refused(f"{n}: the upload gave {st}: {txt[:100]}")
            now = ssh(r, "ls /mnt/sstv/tx_pics").split()
            new = [x for x in now if x not in pics]
            listed = _gallery(r, "sstv_tx")
            if len(new) != 1 or new[0] not in listed:
                raise Refused(f"{n}: after the upload tx_pics has {new} new, the Gallery lists {listed[:6]}")
            st2, _ = http(r, f"/api/gallery/sstv_tx/{new[0]}", "DELETE")
            gone = new[0] not in ssh(r, "ls /mnt/sstv/tx_pics").split()
            if st2 != 200 or not gone:
                raise Refused(f"{n}: its DELETE {st2}, gone {gone}")
            return {"defaults": (len(pics), len([t for t in texts if t.strip()])), "uploaded": new[0]}
        with o.step("on both: the default TX pictures and texts; an upload lands in tx_pics, listed, deleted") as s:
            res = each(o, one, wait=60)
            s.confirm(str(res))
        return True, f"SSTV's TX folders and the Gallery's upload on both ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        for r in o.r.values():
            try:
                ssh(r, "rm -f /mnt/sstv/tx_pics/bench_tx*.png")
            except (Refused, OSError, subprocess.SubprocessError):
                pass


# ---- JS8 and GhostNet ----

JS8_NAMES = ("JS8",)
JS8_KEEP = ("js8_auto", "js8_hb", "js8_ghostnet", "js8_log_prompt", "js8_log_activation", "js8_rx_all", "js8_speed")


@contextlib.contextmanager
def js8_kept(o, files=("/mnt/js8_texts.txt", "/mnt/ghostnet.conf")):
    """JS8's switches (those cfg reaches) and its text files as found, put back after"""
    was = {n: {k: cfg(r, k) for k in JS8_KEEP} for n, r in o.r.items()}
    for r in o.r.values():
        for f in files:
            ssh(r, "set -- save\n" + KEEP.format(f=f))
    try:
        yield was
    finally:
        for n, r in o.r.items():
            for k, v in was[n].items():
                try:
                    if str(cfg(r, k)) != str(v):
                        bench.cfg_set(r, k, v)
                except Refused:
                    pass
            for f in files:
                try:
                    ssh(r, "set -- back\n" + KEEP.format(f=f))
                except (Refused, OSError, subprocess.SubprocessError):
                    pass


def _js8_settings_row(o, r, back, what):
    """JS8's Settings... (page 3) opened on r, the knob back from its Close row by back rows (rows 18..9 follow Close
    in its key group whatever comes before them), the row pressed; the list's screen read first (its title)"""
    k = _to_label(r, r"^Settings", JS8_NAMES)
    press(r, k, JS8_NAMES, rest=1.0)
    p, txt = popup_text(o, r, f"js8 settings for {what}")
    if not (ocr_has(txt, "Clear the list") or ocr_has(txt, "Send a heartbeat")):       # its title scrolls off
        raise Refused(f"{r.name}: JS8 Settings not on the screen ({' '.join(txt.split())[:100]!r}): nothing pressed")
    turn(r, "mfk", -back, JS8_NAMES, rest=0.8)
    press(r, "MFK", JS8_NAMES, rest=1.2)


def _js8_freq_popup(o, r):
    """JS8's Freq popup (Settings row 9), seen on its screen"""
    _js8_settings_row(o, r, 10, "Freq")
    p, txt = popup_text(o, r, "js8 freq popup")
    # the row with the focus (dark on gold) may not read: GhostNet's rows and one of the other two
    if not ocr_has(txt, "GhostNet", "nets") or not (ocr_has(txt, "Custom") or ocr_has(txt, "JS8Call frequencies")):
        raise Refused(f"{r.name}: the Freq popup not on the screen ({' '.join(txt.split())[:140]!r})")
    return txt


FQ = {"JS8": 0, "GHOSTNET": 1, "GN_MORE": 2, "CUSTOM": 3, "CLOSE": 4}    # js8_window.c enum freq_choice


def _fq_now(r):
    """The Freq popup's row its focus starts on: the list in use (Custom, GhostNet, else JS8Call's)"""
    custom = ssh(r, "sqlite3 -readonly /mnt/params.db \"select val from params where name='js8_custom_on'\" "
                    "|| true").strip()
    if custom == "1":
        return FQ["CUSTOM"]
    return FQ["GHOSTNET"] if str(cfg(r, "js8_ghostnet")) == "1" else FQ["JS8"]


GN_SAID = r"(: reminder set|: no reminder| marked, Reminder off)"     # a GhostNet net's row pressed


def _js8_custom_on(r):
    return ssh(r, "sqlite3 -readonly /mnt/params.db \"select val from params where name='js8_custom_on'\" "
                  "|| true").strip() == "1"


def _js8_custom_back(o, r, was):
    """JS8's Custom dial off again (Freq > JS8Call frequencies) when a test left it on and it was off before; JS8
    still open on r. Said, never raised"""
    if was or not _js8_custom_on(r) or win(r) != "JS8":
        return
    o.changing += 1
    try:
        _js8_freq_popup(o, r)
        _fq_go(r, FQ["JS8"])
        o.st.ev(f"{r.name}: JS8's Custom dial left on by a failed step: JS8Call frequencies chosen again", "radio")
    except Refused as e:
        o.st.ev(f"{r.name}: JS8's Custom dial left on ({e})", "bad")
    finally:
        o.changing -= 1


def _fq_go(r, row):
    """From the row in use to row in the open Freq popup, then pressed"""
    turn(r, "mfk", row - _fq_now(r), JS8_NAMES, rest=0.6)
    press(r, "MFK", JS8_NAMES, rest=1.2)


def _js8_open(o, dial=14078000):
    """JS8 open on both, brought to dial by the band keys (an active JS8 holds its dial: the remote's set refused)"""
    H.test_calls(o)
    o.changing += 1
    try:
        got = open_both(o, 111, "JS8", same=False)
        each(o, lambda n, r: bench.ft8_to_band(r, dial), wait=60)
    finally:
        o.changing -= 1
    H.alike(o, ("freq", "mode"))
    o.both({"pwr": H.W})
    return got


def _js8_alone(o, n, dial=14078000):
    """JS8 open on n alone (the other radio on the Sentry's A, following n's dial: an active JS8 holds its own, so the
    other cannot follow it under a JS8 of its own), brought to dial by the band keys"""
    r = o.r[n]
    other = next(x for m, x in o.r.items() if m != n)
    H.test_calls(o)
    o.changing += 1
    try:
        bench.close_apps(other)
        bench.open_page(other, "A")
        r.take_control()
        if win(r) != "JS8":
            if (r.state.get("win") or "Sentry") == "Sentry" and r.state.get("pg") != "A":
                bench.open_page(r, "A")
            r.send("open 111")
            r.wait("JS8", lambda: win(r) == "JS8", 12)
            bench.settled(r, still=1.0, timeout=5.0)
        bench.ft8_to_band(r, dial)
        bench.ensure(r, {"pwr": H.W})
        bench.ensure(other, {"freq": int(r.state.get("freq", 0)), "mode": int(r.state.get("mode", 3))})
    finally:
        o.changing -= 1
    return r, other


def _follow(o, r, other):
    """The other radio on r's dial again (r's JS8 moved it), inside one change"""
    f = settled(r)
    bench.ensure(other, {"freq": f, "mode": int(r.state.get("mode", 3))})
    return f


def t_js8_freq(o):
    """JS8's Freq (Settings row 9) on each radio in turn, JS8 open on it alone, the other radio following its dial from
    the Sentry's A, in a TX session (a row of Settings sends a heartbeat: never pressed, but the Watch is on): GhostNet
    mode: 7.107 USB-D, 'GhostNet 40m', @GHOSTNET and @GSTFLASH in GROUPS=, BAND_UP to 14.107; Custom kHz... typed
    14080: 14.080, 'JS8 14080 kHz'; JS8Call frequencies again: its preset. Its switches and files put back"""
    import orch_cable as oc
    res = {}
    try:
        with js8_kept(o):
            for n in (H.A, H.B):
                r, other = _js8_alone(o, n)
                out = {}
                t_on = time.time()
                custom0 = _js8_custom_on(r)
                stack = contextlib.ExitStack()
                stack.callback(_js8_custom_back, o, r, custom0)     # a step that failed left Custom's dial on
                with stack, oc.session(o, f"js8-freq {n}", [n], int(r.state.get("freq", 0))) as watch:
                    oc.role(o, n)
                    with o.step(f"{n}: Freq > GhostNet mode: 7.107 USB-D, its groups; the other follows") as s:
                        o.changing += 1
                        try:
                            _js8_freq_popup(o, r)
                            t0 = time.time()
                            _fq_go(r, FQ["GHOSTNET"])
                            out["gn"] = wait_msg(r, t0, r"GhostNet 40m", 5)
                            r.wait("7.107 USB-D", lambda: int(r.state.get("freq", 0)) == 7107000 and
                                   int(r.state.get("mode", -1)) == 3, 5)
                            _follow(o, r, other)
                        finally:
                            o.changing -= 1
                        g = ssh(r, "grep '^GROUPS=' /mnt/js8_texts.txt || true")
                        if "@GHOSTNET" not in g or "@GSTFLASH" not in g:
                            raise Refused(f"{n}: GhostNet mode's groups not in js8_texts.txt ({g!r})")
                        out["groups"] = g.strip()
                        s.confirm(f"{out['gn']!r}; {out['groups']}")
                    with o.step(f"{n}: BAND_UP in GhostNet mode: 14.107") as s:
                        o.changing += 1
                        try:
                            bench.tap(r, "BAND_UP")
                            r.wait("14.107", lambda: int(r.state.get("freq", 0)) == 14107000, 5)
                            _follow(o, r, other)
                        finally:
                            o.changing -= 1
                        s.confirm(str(r.state.get("freq")))
                    with o.step(f"{n}: Freq > Custom kHz... typed 14080: 14.080, 'JS8 14080 kHz'") as s:
                        o.changing += 1
                        try:
                            _js8_freq_popup(o, r)
                            t0 = time.time()
                            _fq_go(r, FQ["CUSTOM"])
                            r.send("type 14080")
                            out["custom"] = wait_msg(r, t0, r"JS8 14080 kHz", 5)
                            r.wait("14.080", lambda: int(r.state.get("freq", 0)) == 14080000, 5)
                            _follow(o, r, other)
                        finally:
                            o.changing -= 1
                        s.confirm(repr(out["custom"]))
                    with o.step(f"{n}: Freq > JS8Call frequencies: a preset ('JS8 20m')") as s:
                        o.changing += 1
                        try:
                            _js8_freq_popup(o, r)
                            t0 = time.time()
                            _fq_go(r, FQ["JS8"])
                            out["js8"] = wait_msg(r, t0, r"JS8 \d+m", 5)
                            f = _follow(o, r, other)
                        finally:
                            o.changing -= 1
                        if str(cfg(r, "js8_ghostnet")) != "0":
                            raise Refused(f"{n}: js8_ghostnet still on after JS8Call frequencies")
                        s.confirm(f"{out['js8']!r} on {f}")
                if oc.tx_spans(r, t_on):
                    raise Refused(f"{n} went on the air during its Freq steps")
                res[n] = out
                o.changing += 1
                try:
                    bench.close_apps(r)
                    bench.ensure(other, {"freq": int(r.state.get("freq", 0)), "mode": int(r.state.get("mode", 2))})
                finally:
                    o.changing -= 1
        return True, (f"JS8 Freq on both: {res[H.A]['gn']!r} with {res[H.A]['groups']}, BAND_UP 14.107, "
                      f"{res[H.A]['custom']!r}, {res[H.A]['js8']!r}"), res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def settled(r, key="freq", quiet=1.0, limit=6.0):
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


def t_ghostnet_popup(o):
    """GhostNet's popup (JS8 Freq > GhostNet: nets, reminder, RTTY...) on each radio in turn, in a TX session: the
    first net row pressed sets its reminder (said, ghostnet.conf's nets names it) and again clears it; RTTY 7.077 MHz,
    45.45 baud closes JS8 and opens RTTY on 7.077 USB, 'GhostNet RTTY: 7.077 MHz, 45.45 baud, 170 Hz', the stored RTTY
    rate, shift and reverse left as they were; RTTY closed: the radio back where it was"""
    import orch_cable as oc
    res = {}
    rtty_q = ("sqlite3 -readonly /mnt/params.db \"select name||'='||val from params where name in "
              "('rtty_rate','rtty_shift','rtty_reverse') order by name\"")
    try:
        with js8_kept(o):
            for n in (H.A, H.B):
                r, other = _js8_alone(o, n)             # the other on A, free to follow the RTTY entry's dial
                out = {}
                with oc.session(o, f"ghostnet {n}", [n], int(r.state.get("freq", 0))) as watch:
                    oc.role(o, n)
                    t_on = time.time()
                    with o.step(f"{n}: the first net's row pressed twice: its reminder set, then cleared") as s:
                        _js8_freq_popup(o, r)
                        _fq_go(r, FQ["GN_MORE"])                        # GhostNet: nets, reminder, RTTY...
                        p, txt = popup_text(o, r, "ghostnet popup")
                        # the row with the focus may not read: two of its rows
                        if not (ocr_has(txt, "Reminder") or ocr_has(txt, "Translate")) or not ocr_has(txt, "RTTY"):
                            raise Refused(f"{n}: GhostNet's popup not on the screen ({' '.join(txt.split())[:140]!r})")
                        t0 = time.time()
                        turn(r, "mfk", 5, JS8_NAMES, rest=0.6)          # Reminder -> the first net
                        press(r, "MFK", JS8_NAMES, rest=1.0)
                        # '<net>: reminder set' / '<net>: no reminder', or with GhostNet's Reminder off '<net> marked,
                        # Reminder off' (js8_window.c gn_item_cb)
                        said = wait_msg(r, t0, GN_SAID, 4)
                        net = re.split(r":| marked", said)[0].strip()
                        conf = ssh(r, "grep '^nets=' /mnt/ghostnet.conf 2>/dev/null || true").strip()
                        t1 = time.time()
                        press(r, "MFK", JS8_NAMES, rest=1.0)
                        said2 = wait_msg(r, t1, GN_SAID, 4)
                        conf2 = ssh(r, "grep '^nets=' /mnt/ghostnet.conf 2>/dev/null || true").strip()
                        if ("reminder set" in said or "marked" in said) and net not in conf:
                            raise Refused(f"{n}: {said!r} but ghostnet.conf has {conf!r}")
                        if said2 == said or conf2 == conf:
                            raise Refused(f"{n}: the second press said {said2!r}, nets {conf!r} -> {conf2!r}")
                        out["net"] = (said, conf, said2, conf2)
                        s.confirm(str(out["net"]))
                    with o.step(f"{n}: RTTY 7.077 MHz, 45.45 baud: RTTY on 7.077 USB, said; the stored RTTY "
                                "settings as they were") as s:
                        rtty0 = ssh(r, rtty_q).split()
                        f0 = int(r.state.get("freq", 0))
                        o.changing += 1
                        try:
                            t0 = time.time()
                            turn(r, "mfk", -2, JS8_NAMES, rest=0.6)     # the first net -> RTTY (5 - 2 = 3)
                            press(r, "MFK", JS8_NAMES, rest=1.5)
                            out["rtty"] = wait_msg(r, t0, r"GhostNet RTTY: 7\.077 MHz, 45\.45 baud, 170 Hz", 6)
                            r.wait("RTTY on 7.077 USB", lambda: r.state.get("app") == "RTTY" and
                                   int(r.state.get("freq", 0)) == 7077000 and int(r.state.get("mode", -1)) == 2, 6)
                            bench.ensure(other, {"freq": 7077000, "mode": 2})
                        finally:
                            o.changing -= 1
                        rtty1 = ssh(r, rtty_q).split()
                        if rtty1 != rtty0:
                            raise Refused(f"{n}: the stored RTTY settings changed: {rtty0} -> {rtty1}")
                        s.confirm(f"{out['rtty']!r}; stored {rtty0}")
                    with o.step(f"{n}: RTTY closed: the radio back where it was, the stored RTTY settings the same") as s:
                        o.changing += 1
                        try:
                            bench.close_apps(r, keep_dial=False)
                            f1 = settled(r, limit=6)
                            bench.ensure(other, {"freq": int(r.state.get("freq", 0)),
                                                 "mode": int(r.state.get("mode", 2))})
                        finally:
                            o.changing -= 1
                        out["back"] = (f0, f1, ssh(r, rtty_q).split())
                        if out["back"][2] != rtty0:
                            raise Refused(f"{n}: after RTTY closed the stored RTTY settings read {out['back'][2]}")
                        s.confirm(str(out["back"]))
                    if oc.tx_spans(r, t_on):
                        raise Refused(f"{n} went on the air during GhostNet's popup")
                res[n] = out
        return True, (f"GhostNet's nets toggled ({res[H.A]['net'][0]!r}), its RTTY entry: {res[H.A]['rtty']!r}, the "
                      "stored RTTY settings untouched, the radio back after"), res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_ghostnet_reminder(o):
    """GhostNet's reminder on both, from the Sentry's A (any screen): an own net written in /mnt/ghostnet.conf four
    minutes ahead with a 5-minute reminder: the message 'GhostNet <net> in N min, JS8 7.107' comes within 60 s;
    the conf put back"""
    res = {}
    try:
        with js8_kept(o, files=("/mnt/ghostnet.conf",)):
            with o.step("an own net 4 min ahead, its reminder 5 min, on both: the reminder said within 60 s") as s:
                def one(n, r):
                    # the radio's own local time (its zone: /mnt/tz.conf, as the GUI takes it), four minutes on
                    wd, hh, mm = (int(x) for x in ssh(r, "TZ=\"$(cat /mnt/tz.conf 2>/dev/null || echo UTC0)\" python3 -c "
                                                         "'import time;t=time.localtime(time.time()+240);"
                                                         "print(t.tm_wday,t.tm_hour,t.tm_min)'").split())
                    dow = (wd + 1) % 7                           # 0 = Sunday
                    conf = (f"remind=1\nremind_min=5\nnets=OWN\nown_on=1\nown_name=BENCH\nown_day={dow}\n"
                            f"own_time={hh:02d}:{mm:02d}\nown_min=30\nown_khz=7107\nown_mode=JS8\n")
                    t0 = time.time()
                    ssh(r, f"cat > /mnt/ghostnet.conf <<'EOF'\n{conf}EOF")
                    return wait_msg(r, t0, r"GhostNet .* in \d+ min", 60)
                res = each(o, one, wait=90)
                s.confirm(str(res))
        return True, f"GhostNet's reminder said on both ({res[H.A]!r})", res
    except Refused as e:
        return False, str(e), res


def t_js8_open_off(o):
    """JS8 opened on both with AUTO and HB left on (cfg): both off at the open (js8 JSON auto/hb/acq 0, cfg? 0);
    nothing sent in 30 s"""
    res = {}
    try:
        with js8_kept(o, files=()):
            H.test_calls(o)
            with o.step("AUTO and HB on (cfg) on both, JS8 closed; JS8 opened: both off") as s:
                for r in o.r.values():
                    bench.cfg_set(r, "js8_auto", 1)
                    bench.cfg_set(r, "js8_hb", 1)
                t0 = time.time()
                _js8_open(o)
                time.sleep(1.5)
                got = {n: ({k: r.state.get(k) for k in ()} | {k: (r.lines and None) for k in ()} |
                           {"cfg_auto": cfg(r, "js8_auto"), "cfg_hb": cfg(r, "js8_hb")}) for n, r in o.r.items()}
                js = {}
                for n, r in o.r.items():
                    with r.lock:
                        last = [x for at, x in list(r.lines) if x.startswith("js8 {")]
                    js[n] = json.loads(last[-1][4:]) if last else {}
                    got[n].update({k: js[n].get(k) for k in ("auto", "hb", "acq")})
                bad = {n: g for n, g in got.items() if any(str(v) not in ("0", "None") for v in g.values())}
                if bad or not all(js.values()):
                    raise Refused(f"after the open: {got}")
                s.confirm(str(got))
            with o.step("30 s: neither sends") as s:
                time.sleep(30)
                import orch_cable as oc
                sent = {n: len(oc.tx_spans(r, t0)) for n, r in o.r.items()}
                if any(sent.values()):
                    raise Refused(f"sent by itself: {sent}")
                s.confirm(str(sent))
            res = got
        return True, f"JS8 opened with AUTO and HB off on both ({res[H.A]}); nothing sent in 30 s", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_js8_nocall(o):
    """JS8 without a callsign, each radio in turn (the call blank by cfg before the open, put back after), in a TX
    session: its CQ refused ('JS8: set your callsign first'), its 'js8 send' refused ('Not sent: set your callsign
    first'), off the air"""
    import orch_cable as oc
    res = {}
    try:
        H.test_calls(o)
        for n in (H.A, H.B):
            r = o.r[n]
            with o.step(f"{n}: callsign blank, JS8 opened: CQ and a send refused, off the air") as s:
                call = cfg(r, "callsign")
                try:
                    r.send("cfg callsign ")
                    wait_cfg(r, "the callsign blank", lambda v: not v.strip(), "callsign", 6)
                    _js8_open(o, 14078000)
                    with oc.session(o, f"js8-nocall {n}", [n], 14078000) as watch:
                        oc.role(o, n)
                        t0 = time.time()
                        r.send("js8 cq")
                        said = wait_msg(r, t0, r"set your callsign first", 5)
                        r.send("js8 send BENCH")
                        time.sleep(1.5)
                        with r.lock:
                            notes = [x for at, x in list(r.lines) if at >= t0 and x.startswith("js8 {")]
                        note = json.loads(notes[-1][4:]).get("note", "") if notes else ""
                        time.sleep(2.0)
                        if oc.tx_spans(r, t0):
                            raise Refused(f"{n} WENT ON THE AIR with no callsign")
                    if "callsign" not in note:
                        raise Refused(f"{n}: 'js8 send' noted {note!r}")
                    res[n] = {"cq": said, "send": note}
                finally:
                    bench.close_apps(r)
                    bench.cfg_set(r, "callsign", call)
                s.confirm(str(res[n]))
        return True, f"JS8 with no callsign: {res[H.A]['cq']!r}, {res[H.A]['send']!r}; off the air on both", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_js8_texts(o):
    """JS8's Settings > Groups, INFO, STATUS, Relay... on both, one radio at a time in a TX session: INFO typed
    ('BENCH INFO'), in /mnt/js8_texts.txt; JS8 closed and opened again: the popup shows it (OCR); the file put back"""
    import orch_cable as oc
    res = {}
    try:
        with js8_kept(o, files=("/mnt/js8_texts.txt",)):
            _js8_open(o, 14078000)
            for n in (H.A, H.B):
                r = o.r[n]
                with oc.session(o, f"js8-texts {n}", [n], 14078000) as watch:
                    oc.role(o, n)
                    with o.step(f"{n}: Settings row 17, INFO: 'BENCH INFO' typed, in js8_texts.txt") as s:
                        _js8_settings_row(o, r, 2, "texts")
                        p, txt = popup_text(o, r, "js8 texts popup")
                        # INFO's row has the focus (dark on gold): read or not, STATUS and Relay below it
                        if not ocr_has(txt, "STATUS") or not (ocr_has(txt, "INFO") or ocr_has(txt, "Relay")):
                            raise Refused(f"{n}: the texts popup not on the screen ({' '.join(txt.split())[:120]!r})")
                        press(r, "MFK", JS8_NAMES, rest=1.0)            # its first row: INFO
                        r.send("type BENCH INFO")
                        time.sleep(1.5)
                        line = ssh(r, "grep '^INFO=' /mnt/js8_texts.txt || true").strip()
                        if line != "INFO=BENCH INFO":
                            raise Refused(f"{n}: js8_texts.txt reads {line!r}")
                        s.confirm(line)
                res[n] = line
            with o.step("JS8 closed and opened on both: the popup shows BENCH INFO") as s:
                close_both(o)
                _js8_open(o, 14078000)
                shown = {}
                for n in (H.A, H.B):
                    r = o.r[n]
                    with oc.session(o, f"js8-texts {n} again", [n], 14078000) as watch:
                        oc.role(o, n)
                        _js8_settings_row(o, r, 2, "texts again")
                        p, txt = popup_text(o, r, "js8 texts popup again")
                        shown[n] = ocr_has(txt, "BENCH INFO")
                        k = _to_label(r, r"^Settings", JS8_NAMES)
                        press(r, k, JS8_NAMES, rest=0.8)               # the popup closed again
                if not all(shown.values()):
                    raise Refused(f"BENCH INFO not read on the reopened popup: {shown}")
                s.confirm(str(shown))
        return True, f"JS8's INFO typed and kept on both ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_js8_map(o):
    """JS8's Map (page 4) on both: the log's stations on the FT8 map (the screen changes), pressed again: closed"""
    res = {}
    try:
        _js8_open(o, 14078000)

        def one(n, r):
            _to_label(r, r"^Map", JS8_NAMES)
            a = shot(o, r, "js8 before the map")
            k = fkey_of(r, r"^Map")
            press(r, k, JS8_NAMES, rest=2.0)
            b = shot(o, r, "js8 map")
            press(r, k, JS8_NAMES, rest=1.5)
            c = shot(o, r, "js8 after the map")
            before, during, after = (" ".join(ocr(x).split()) for x in (a, b, c))
            cap = re.compile(r"\S+\s*heard\s+\S+\s*hear\s*me", re.I)    # JS8's map: "4 heard 0 hear me"
            if not cap.search(during) or cap.search(after) or cap.search(before):
                raise Refused(f"{n}: the map's header read before {bool(cap.search(before))}, open "
                              f"{bool(cap.search(during))}, after {bool(cap.search(after))} ({during[:120]!r})")
            return cap.search(during).group(0)
        with o.step("Map pressed twice on both: the map, then JS8 again") as s:
            res = each(o, one, wait=90)
            s.confirm(str(res))
        return True, f"JS8's Map opened and closed on both ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- Field Ops (118): the radio's own reference lists, positions from the remote's "pos", its state file ----

FIELD_NAMES = ("Field Ops",)
TILES = ["LOG", "NOTA", "SOTA", "POTA", "WWBOTA", "HOTA", "WWFF", "TOTA", "WCA", "MOTA", "LH", "TILES", "LLOTA",
         "XOTA", "BANDS"]
PROG = {"SOTA": "S", "POTA": "P", "WWBOTA": "B", "HOTA": "H", "WWFF": "F", "TOTA": "T", "WCA": "C", "MOTA": "M",
        "LH": "L", "TILES": "G", "LLOTA": "K", "XOTA": "X", "NOTA": "N"}
NEEDS = {"S": "/ 4", "P": "/ 10", "B": "/ 25", "H": "CALLS / 5", "F": "/ 44", "T": "/ 13", "C": "/ 50",
         "M": "CALLS / 4", "L": "CALLS / 4", "G": "/ 1", "K": "/ 10", "X": "/ 1"}
CB_TEST_CALL = "9QT999"         # a CB call's shape (cb.c cb_callsign): Field Ops spots nothing for one, says so
REFS_CACHE = os.path.join(bench.STATE, "cache", "refs.tsv")
_REFS = {}


def _refs(r):
    """The radio's reference lists (refs.tsv, kept here once by its etag): {type: [(ref, name, lat, lon)]}"""
    etag = ssh(r, "cat /mnt/field/refs.etag 2>/dev/null || true").strip()
    if _REFS.get("etag") == etag and _REFS.get("by"):
        return _REFS["by"]
    os.makedirs(os.path.dirname(REFS_CACHE), exist_ok=True)
    tag_file = REFS_CACHE + ".etag"
    if not (os.path.exists(REFS_CACHE) and os.path.exists(tag_file) and open(tag_file).read() == etag):
        subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", f"root@{r.host}:/mnt/field/refs.tsv", REFS_CACHE],
                       check=True, timeout=300)
        open(tag_file, "w").write(etag)
    by = {}
    for line in open(REFS_CACHE, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 5:
            continue
        try:
            lat, lon = float(f[3]), float(f[4])
        except ValueError:
            lat = lon = None
        by.setdefault(f[0], []).append((f[1], f[2], lat, lon))
    _REFS.update(etag=etag, by=by)
    return by


def _km(a, b):
    import math
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 2 * 6371.0 * math.asin(math.sqrt(h))


def _nearest(refs, t, pos, k=3):
    rows = [(x, _km(pos, (x[2], x[3]))) for x in refs.get(t, []) if x[2] is not None]
    rows.sort(key=lambda y: y[1])
    return rows[:k]


def _pair_place(refs, t):
    """A place of type t with a neighbour within 3 km, the next one clearly further (the nearest two can't swap):
    (a position 50 m from the first, [first, second])"""
    for x in refs.get(t, []):
        if x[2] is None or abs(x[2]) > 60:
            continue
        pos = (x[2] + 0.00045, x[3])                    # 50 m north of it
        near = _nearest(refs, t, pos, 3)
        if len(near) == 3 and near[0][0] == x and near[1][1] < 3.0 and near[2][1] > near[1][1] * 1.3 + 0.2:
            return pos, [near[0][0], near[1][0]]
    raise Refused(f"no {t} place with a clear neighbour in the radio's list")


def _fstate(r):
    d = {}
    for line in ssh(r, "cat /mnt/field/state 2>/dev/null || true").splitlines():
        k, _, v = line.partition("=")
        d[k] = v
    if "ref" in d:
        d["ref_f"] = d["ref"].split("\t")
    return d


def _fwait(r, what, cond, secs=8.0):
    end = time.time() + secs
    while True:
        st = _fstate(r)
        if cond(st):
            return st
        if time.time() > end:
            raise Refused(f"{r.name}: {what}: Field Ops' state reads {({k: v for k, v in st.items() if k != 'ref_f'})}")
        time.sleep(0.4)


def _pos(o, pos, acc=20):
    for r in o.r.values():
        r.send(f"pos {pos[0]:.6f} {pos[1]:.6f} {acc}")


def _pos_forget(o):
    """The phone's position made one the radio ignores (coarser than 1 km): there is no clear"""
    for r in o.r.values():
        r.send("pos 0 0 100000")


def _field_open(o):
    """Field Ops open on both, on its home screen (Back from wherever it opened)"""
    H.test_calls(o)
    open_both(o, 118, "Field Ops")

    def home(n, r):
        for _ in range(3):
            st = _fstate(r)
            if st.get("screen") == "0":
                return st.get("home")
            k = fkey_of(r, r"^Back$")
            if not k:
                raise Refused(f"{n}: Field Ops on screen {st.get('screen')} without a Back ({[flat(t) for t in fkeys(r)[0]]})")
            press(r, k, FIELD_NAMES, rest=1.0)
        raise Refused(f"{n}: Field Ops not home")
    return each(o, home)


def _field_tile(o, r, name):
    """r from Field Ops' home to the tile name (the MFK from the tile last opened, which the state file names), its
    press: the screen it opens read back from the state file"""
    st = _fstate(r)
    if st.get("screen") != "0":
        raise Refused(f"{r.name}: Field Ops not home (screen {st.get('screen')})")
    cur = TILES.index(st.get("home", "LOG").upper()) if st.get("home", "LOG").upper() in TILES else 0
    want = TILES.index(name)
    d = (want - cur) % len(TILES)
    d = d if d <= len(TILES) // 2 else d - len(TILES)
    if d:
        turn(r, "mfk", d, FIELD_NAMES, rest=0.8)
    press(r, "MFK", FIELD_NAMES, rest=1.2)
    screen = {"LOG": "3", "BANDS": "2"}.get(name, "1")
    return _fwait(r, f"the {name} tile open", lambda x: x.get("screen") == screen and
                  (screen != "1" or x.get("prog") == PROG[name]), 8)


def _field_back(o, r):
    k = fkey_of(r, r"^Back$") or _to_label(r, r"^Back$", FIELD_NAMES)
    press(r, k, FIELD_NAMES, rest=1.0)
    return _fwait(r, "home", lambda x: x.get("screen") == "0", 6)


def _field_type(r, button, text, names=FIELD_NAMES):
    """A Field Ops button that opens its text window (Ref..., Log QSO), TEXT typed and taken (the remote's type)"""
    k = _to_label(r, button, names)
    press_if(r, k, button, names, rest=0.8)
    r.send(f"type {text}")
    time.sleep(1.2)


def _adif(r, path):
    """The records of an ADIF file on r: [{TAG: value}]"""
    text = ssh(r, f"cat '{path}' 2>/dev/null || true")
    body = text.split("<eoh>", 1)[-1] if "<eoh>" in text.lower() else text
    recs = []
    for chunk in re.split(r"<eor>", body, flags=re.I):
        rec, i = {}, 0
        for m in re.finditer(r"<([A-Za-z_]+):(\d+)(?::[A-Za-z])?>", chunk):
            n = int(m.group(2))
            rec[m.group(1).upper()] = chunk[m.end():m.end() + n]
        if rec:
            recs.append(rec)
    return recs


def _start(o, r, ref):
    """Start (reference page 1) on r: 'Activation started: REF', active=1 and its log file named"""
    t0 = time.time()
    k = _to_label(r, r"^Start$", FIELD_NAMES)
    press_if(r, k, r"^Start$", FIELD_NAMES, rest=1.0)
    said = wait_msg(r, t0, rf"Activation started: {re.escape(ref)}", 5)
    st = _fwait(r, "the activation", lambda x: x.get("active") == "1" and "/" in x.get("log", ""), 6)
    return said, st["log"]


def _log_qso(o, r, line, expect=r"QSO \d+:"):
    t0 = time.time()
    _field_type(r, r"^Log\s*QSO", line)
    return wait_msg(r, t0, expect, 5)


def _end(o, r):
    """End (activation page 2) pressed twice: 'Press End again', then 'Activation complete'; active=0"""
    t0 = time.time()
    k = _to_label(r, r"^End", FIELD_NAMES)
    press_if(r, k, r"^End", FIELD_NAMES, rest=0.8)
    first = wait_msg(r, t0, r"Press End again", 4)
    k = fkey_of(r, r"^End") or k
    press_if(r, k, r"^End", FIELD_NAMES, rest=1.0)
    done = wait_msg(r, t0, r"Activation complete", 5)
    _fwait(r, "the activation ended", lambda x: x.get("active") == "0", 6)
    return first, done


@contextlib.contextmanager
def field_kept(o):
    """Field Ops as found after the test: the state file as it was (the GUI keeps it in memory: the home tile it
    names opened and Back, the file compared; a difference put back by a planned restart), the test's activation
    logs removed, xota.tsv as found, the phone's position made one to ignore"""
    was = {n: ssh(r, "cat /mnt/field/state 2>/dev/null || true") for n, r in o.r.items()}
    logs0 = {n: set(ssh(r, "ls /mnt/field/logs 2>/dev/null || true").split()) for n, r in o.r.items()}
    for r in o.r.values():
        ssh(r, "set -- save\n" + KEEP.format(f="/mnt/field/xota.tsv"))
    try:
        yield was
    finally:
        _pos_forget(o)
        for n, r in o.r.items():
            try:
                new = set(ssh(r, "ls /mnt/field/logs 2>/dev/null || true").split()) - logs0[n]
                for f in new:
                    ssh(r, f"rm -f '/mnt/field/logs/{f}'")
                ssh(r, "set -- back\n" + KEEP.format(f="/mnt/field/xota.tsv"))
            except (Refused, OSError, subprocess.SubprocessError) as e:
                o.st.ev(f"{n}: Field Ops' test logs not all removed: {e}", "bad")
        # the home tile it had, opened and left: the file the GUI writes then is as found
        home0 = {n: dict(x.split("=", 1) for x in w.splitlines() if "=" in x).get("home", "LOG").upper()
                 for n, w in was.items()}
        try:
            if (win(o.r[H.A]) or "") != "Field Ops" or (win(o.r[H.B]) or "") != "Field Ops":
                open_both(o, 118, "Field Ops")

            def back(n, r):
                st = _fstate(r)
                if st.get("active") == "1":
                    raise Refused(f"{n}: an activation still running")
                if st.get("screen") != "0":
                    _field_back(o, r)
                if home0[n] in TILES and _fstate(r).get("home", "").upper() != home0[n]:
                    _field_tile(o, r, home0[n])
                    _field_back(o, r)
                return _fstate(r)
            each(o, back, wait=60)
        except Refused as e:
            o.st.ev(f"Field Ops not walked back home: {e}", "bad")
        home_both(o)
        for n, r in o.r.items():
            now = ssh(r, "cat /mnt/field/state 2>/dev/null || true")
            if now != was[n]:
                o.st.ev(f"{n}: Field Ops' state differs from as found ({now!r} vs {was[n]!r}): put back by a restart",
                        "radio")
                gui_restart(o, n, "Field Ops' state put back as found",
                            f"printf '%s' '{was[n].replace(chr(39), '')}' > /mnt/field/state")


def gui_restart(o, n, why, between="true"):
    """n's GUI stopped, the command between run, started again (one SSH command: never a stop alone), a planned
    restart said on the console; the session connected again, both alike"""
    import orch_cable as oc
    r = o.r[n]
    o.st.ev(f"{n}: its GUI restarts ({why}, planned)", "radio")
    o.planned[n] = f"planned: {why}"
    o.changing += 1
    try:
        # one command (never a stop alone): a between of several lines (KEEP's) goes on lines of its own
        ssh(r, f"/etc/init.d/S95gui stop >/dev/null 2>&1\n{between}\n/etc/init.d/S95gui start >/dev/null 2>&1", 120)
        time.sleep(8)
        oc._reconnect(o, n, why)
        r.take_control()
        other = next(x for m, x in o.r.items() if m != n)
        bench.ensure(r, {"freq": int(other.state.get("freq", 0)), "mode": int(other.state.get("mode", 0))})
    finally:
        o.changing -= 1
        o.planned.pop(n, None)


def t_field_home(o):
    """Field Ops' home on both: its buttons (Select, Start here, Re-spot, Radio); the row of fifteen tiles: fifteen
    MFK steps come round to the same tile (its screen opens again), LOG's tile opens the logbook, Bands is one step
    left of LOG (the BANDS screen); left as found"""
    res = {}
    try:
        with field_kept(o):
            with o.step("Field Ops open on both, home: its buttons") as s:
                res["home"] = _field_open(o)
                keys = each(o, lambda n, r: [flat(t) for t in fkeys(r)[0]])
                for n, k in keys.items():
                    if not all(any(w in t for t in k) for w in ("Select", "Start here", "Re-spot", "Radio")):
                        raise Refused(f"{n}: home's buttons {k}")
                shot(o, o.r[H.A], "field home")
                s.confirm(f"{res['home']}; {keys[H.A]}")
            with o.step("15 MFK steps and a press on both: the tile it started on opens") as s:
                def round_(n, r):
                    st0 = _fstate(r)
                    turn(r, "mfk", 15, FIELD_NAMES, rest=0.8)
                    press(r, "MFK", FIELD_NAMES, rest=1.2)
                    home = st0.get("home", "LOG").upper()
                    screen = {"LOG": "3", "BANDS": "2"}.get(home, "1")
                    st = _fwait(r, f"{home} open", lambda x: x.get("screen") == screen, 8)
                    _field_back(o, r)
                    return f"{home}: screen {st.get('screen')} prog {st.get('prog')}"
                res["round"] = each(o, round_)
                s.confirm(str(res["round"]))
            with o.step("LOG's tile: the logbook; from LOG one step left: BANDS") as s:
                def log_bands(n, r):
                    _field_tile(o, r, "LOG")
                    _field_back(o, r)
                    turn(r, "mfk", -1, FIELD_NAMES, rest=0.8)
                    press(r, "MFK", FIELD_NAMES, rest=1.2)
                    st = _fwait(r, "BANDS open", lambda x: x.get("screen") == "2", 8)
                    _field_back(o, r)
                    return st.get("screen")
                res["bands"] = each(o, log_bands, wait=60)
                s.confirm(str(res["bands"]))
        return True, f"Field Ops' tile row on both: round in 15 steps ({res['round'][H.A]}), LOG, Bands left of it", res
    except Refused as e:
        return False, str(e), res


def t_field_sota(o):
    """A SOTA activation on both, end to end (a summit of the radio's own list, the phone's position 50 m from it):
    the nearest summit taken, the next by the MFK, one typed by Ref...; PHONE in the header, QTH when the phone's
    position is coarser than 1 km; Start ('Activation started', its ADIF named); the counter 'QSOs / 4'; Log QSO
    'Q9TST 59 57' (59 sent, 57 received, MY_SOTA_REF) in USB (SSB), on 14.074 (FT8), on 14.110 USB-D (no mode, said;
    the web Logs page sets it); a QSO onto a read-only log folder: 'QSO not saved'; the LOG tile's logbook lists them;
    End twice; the phone's position '(old)' after 150 s. Field Ops, the logs and the position put back"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        pos, (s1, s2) = _pair_place(refs, "S")
        s3 = _nearest(refs, "S", pos, 5)[3][0]
        res["refs"] = [s1[0], s2[0], s3[0]]
        with field_kept(o):
            align(o, 14285000, 2)
            t_pos = time.time()
            _pos(o, pos, 20)
            _field_open(o)
            with o.step(f"SOTA on both: the nearest ({s1[0]}) taken, PHONE; the MFK's next ({s2[0]}); Ref... "
                        f"{s3[0]}, then {s1[0]}") as s:
                def refsteps(n, r):
                    out = {}
                    _field_tile(o, r, "SOTA")
                    _fwait(r, f"{s1[0]} taken", lambda x: (x.get("ref_f") or ["", ""])[1] == s1[0], 20)
                    p, txt = shot_text(o, r, "field sota reference")
                    out["phone"] = ocr_has(txt, "PHONE")
                    if not out["phone"]:
                        raise Refused(f"{n}: no PHONE on the reference screen ({' '.join(txt.split())[:120]!r})")
                    turn(r, "mfk", 1, FIELD_NAMES, rest=0.8)
                    press(r, "MFK", FIELD_NAMES, rest=1.0)
                    _fwait(r, f"{s2[0]} chosen", lambda x: (x.get("ref_f") or ["", ""])[1] == s2[0], 6)
                    for ref in (s3[0], s1[0]):
                        _field_type(r, r"^Ref", ref)
                        _fwait(r, f"{ref} typed", lambda x, ref=ref: (x.get("ref_f") or ["", ""])[1] == ref, 6)
                    return out
                res["ref"] = each(o, refsteps, wait=90)
                s.confirm(str(res["ref"]))
            with o.step("Start on both: 'Activation started', its ADIF; the counter 'QSOs / 4'; Log QSO 'Q9TST 59 57' "
                        "in USB: SSB, 59 sent, 57 received, MY_SOTA_REF") as s:
                def start(n, r):
                    said, log = _start(o, r, s1[0])
                    p, txt = shot_text(o, r, "field sota activation")
                    if not re.search(r"/\s*4\b", txt):
                        raise Refused(f"{n}: no '/ 4' read on the activation screen ({' '.join(txt.split())[:120]!r})")
                    q = _log_qso(o, r, "Q9TST 59 57", r"QSO 1: Q9TST 59 57")
                    rec = [x for x in _adif(r, log) if x.get("CALL") == "Q9TST"]
                    if not rec or rec[-1].get("RST_SENT") != "59" or rec[-1].get("RST_RCVD") != "57" or \
                            rec[-1].get("MY_SOTA_REF") != s1[0] or rec[-1].get("MODE") != "SSB":
                        raise Refused(f"{n}: the ADIF record {rec[-1] if rec else None}")
                    return {"said": said, "log": log, "qso": q}
                res["start"] = each(o, start, wait=60)
                s.confirm(str({n: (x["said"], x["qso"]) for n, x in res["start"].items()}))
            with o.step("both on 14.074 USB: Log QSO Q8TST: MODE FT8") as s:
                align(o, 14074000, 2)

                def ft8(n, r):
                    _log_qso(o, r, "Q8TST")
                    rec = [x for x in _adif(r, res["start"][n]["log"]) if x.get("CALL") == "Q8TST"]
                    if not rec or rec[-1].get("MODE") != "FT8":
                        raise Refused(f"{n}: the ADIF record {rec[-1] if rec else None}")
                    return rec[-1].get("MODE")
                res["ft8"] = each(o, ft8)
                s.confirm(str(res["ft8"]))
            # (14.080 is 20 m's FT4 dial: a QSO there is logged FT4, 11-26: 14.110 is no digital mode's)
            with o.step("both on 14.110 USB-D: Log QSO Q7TST: no mode, said; the web Logs page sets PSK31") as s:
                align(o, 14110000, 3)

                def data(n, r):
                    said = _log_qso(o, r, "Q7TST", r"logged without a mode")
                    log = res["start"][n]["log"]
                    rec = [x for x in _adif(r, log) if x.get("CALL") == "Q7TST"]
                    if not rec or rec[-1].get("MODE"):
                        raise Refused(f"{n}: the ADIF record {rec[-1] if rec else None}")
                    st, d = http_json(r, "/api/logs")
                    mine = [q for q in (d.get("qsos") or []) if q.get("file") == os.path.basename(log) and
                            q.get("call") == "Q7TST"] if isinstance(d, dict) else []
                    if not mine:
                        raise Refused(f"{n}: /api/logs ({st}) has no Q7TST in {os.path.basename(log)}")
                    q = mine[-1]
                    st, a = http_json(r, f"/api/logs/{q['file']}/{q['n']}?id={q.get('id', '')}", "PUT",
                                      {"mode": "PSK31"})
                    rec = [x for x in _adif(r, log) if x.get("CALL") == "Q7TST"]
                    if st != 200 or not rec or (rec[-1].get("SUBMODE") or rec[-1].get("MODE")) != "PSK31":
                        raise Refused(f"{n}: PUT mode PSK31 {st} {str(a)[:80]}; the record {rec[-1] if rec else None}")
                    return {"said": said, "after": {k: rec[-1].get(k) for k in ("MODE", "SUBMODE")}}
                res["data"] = each(o, data, wait=60)
                s.confirm(str(res["data"]))
            with o.step("the log folder read-only (a tmpfs over it) on both: Log QSO 'QSO not saved: ...'") as s:
                def ro(n, r):
                    ssh(r, "mount -t tmpfs -o ro,size=64k bench_ro /mnt/field/logs")
                    try:
                        return _log_qso(o, r, "Q6TST", r"QSO not saved: ")
                    finally:
                        ssh(r, "umount /mnt/field/logs || umount -l /mnt/field/logs; true")
                res["ro"] = each(o, ro)
                for n, r in o.r.items():
                    if "bench_ro" in ssh(r, "mount"):
                        raise Refused(f"{n}: the read-only tmpfs still mounted")
                s.confirm(str(res["ro"]))
            with o.step("Back, the LOG tile on both: 'SOTA LOGBOOK <ref>', TEST, the QSOs; back to the activation") as s:
                def logbook(n, r):
                    _field_back(o, r)
                    _field_tile(o, r, "LOG")
                    time.sleep(1.0)
                    p, txt = shot_text(o, r, "field logbook")
                    txt += "\n" + ocr(p, box=(10, 85, 790, 175), psm=6, stretch=True)       # the rows alone
                    found = [c for c in ("Q9TST", "Q8TST", "Q7TST") if _call_read(txt, c)]
                    if not ocr_has(txt, "LOGBOOK") or len(found) < 2:
                        raise Refused(f"{n}: the logbook reads {' '.join(txt.split())[:160]!r}")
                    _field_back(o, r)
                    _field_tile(o, r, "SOTA")
                    return found
                res["logbook"] = each(o, logbook, wait=60)
                s.confirm(str(res["logbook"]))
            with o.step("End twice on both: 'Press End again', 'Activation complete'; the log file stays") as s:
                def end(n, r):
                    first, done = _end(o, r)
                    log = res["start"][n]["log"]
                    if not ssh(r, f"ls '{log}' 2>/dev/null || true").strip():
                        raise Refused(f"{n}: {log} gone after End")
                    return done
                res["end"] = each(o, end)
                s.confirm(str(res["end"]))
            with o.step("the phone's position 150 s old: '(old)'; coarser than 1 km: QTH; good again: PHONE") as s:
                time.sleep(max(0.0, 155 - (time.time() - t_pos)))

                def old(n, r):
                    out = {}
                    out["old"] = ocr_has(shot_text(o, r, "field position old")[1], "old")
                    r.send(f"pos {pos[0]:.6f} {pos[1]:.6f} 5000")
                    time.sleep(2.5)
                    out["qth"] = ocr_has(shot_text(o, r, "field position coarse")[1], "QTH")
                    r.send(f"pos {pos[0]:.6f} {pos[1]:.6f} 20")
                    time.sleep(2.5)
                    out["phone"] = ocr_has(shot_text(o, r, "field position phone")[1], "PHONE")
                    if not all(out.values()):
                        raise Refused(f"{n}: {out}")
                    return out
                res["pos"] = each(o, old, wait=60)
                s.confirm(str(res["pos"]))
        return True, (f"SOTA on both: {res['refs']} nearest/MFK/typed, PHONE, Start, QSOs SSB/FT8/no mode (PSK31 by "
                      f"the web), not saved on a read-only folder, the logbook, End, (old) and QTH"), res
    except Refused as e:
        return False, str(e), res


def t_field_beacon_rules(o):
    """Field Ops' BANDS on both: Test beacon in USB 'switch to CW first'; in CW one press listens on 40 m (7.030) and
    with no second press 'not sent, the radio stays on 7.0300'; a press for 20 m then the dial turned: 'not sent'
    (tuned); the dial locked and another band: 'the frequency is locked', the dial kept; Use band in CW: 7.030 CW
    kept, in USB: 7.200 LSB, the screen still BANDS. Never two presses: nothing sent (the guard stops either radio
    that would)"""
    res = {}

    def beacon(n, r, pattern, secs=4.0):
        t0 = time.time()
        k = _to_label(r, r"^Test\s*beacon", FIELD_NAMES)
        press(r, k, FIELD_NAMES, rest=0.5)
        return wait_msg(r, t0, pattern, secs), t0
    try:
        with field_kept(o):
            _field_open(o)
            align(o, 14200000, 2)
            each(o, lambda n, r: _field_tile(o, r, "BANDS"))
            with o.step("USB: Test beacon on both: 'switch to CW first'") as s:
                res["usb"] = each(o, lambda n, r: beacon(n, r, r"switch to CW first")[0])
                s.confirm(str(res["usb"]))
            with o.step("CW 14.040: 40 m's row, one press: listens on 7.030; no second: not sent, stays") as s:
                align(o, 14040000, 4)

                def once(n, r):
                    turn(r, "mfk", -2, FIELD_NAMES, rest=0.8)
                    said, t0 = beacon(n, r, r"Listen first: 40m, 7\.030 MHz")
                    r.wait("7.030", lambda: int(r.state.get("freq", 0)) == 7030000, 4)
                    later = wait_msg(r, t0, r"not sent", 14)
                    import orch_cable as oc
                    if oc.tx_spans(r, t0):
                        raise Refused(f"{n} SENT after one press")
                    if "7.0300" not in later:
                        raise Refused(f"{n}: {later!r}")
                    return said, later
                o.changing += 1
                try:
                    res["once"] = each(o, once, wait=40)
                finally:
                    o.changing -= 1
                H.alike(o, ("freq", "mode"))
                s.confirm(str(res["once"]))
            with o.step("20 m's row, one press, the dial turned: not sent (tuned)") as s:
                def tuned(n, r):
                    turn(r, "mfk", 2, FIELD_NAMES, rest=0.8)
                    said, t0 = beacon(n, r, r"Listen first: 20m")
                    r.send("knob main 1")
                    later = wait_msg(r, t0, r"not sent.*tuned|tuned.*not sent", 6)
                    return later
                o.changing += 1
                try:
                    res["tuned"] = each(o, tuned, wait=40)
                    align(o, 14040000, 4)
                finally:
                    o.changing -= 1
                s.confirm(str(res["tuned"]))
            with o.step("the dial locked on both, 40 m's row: 'the frequency is locked', the dial kept") as s:
                def locked(n, r):
                    r.send("set lock 1")
                    r.wait("locked", lambda: r.state.get("ulock") == 1 or r.state.get("lock") == 1, 4)
                    try:
                        turn(r, "mfk", -2, FIELD_NAMES, rest=0.8)
                        said, _ = beacon(n, r, r"frequency is locked")
                        if int(r.state.get("freq", 0)) != 14040000:
                            raise Refused(f"{n}: the dial moved to {r.state.get('freq')} though locked")
                        return said
                    finally:
                        r.send("set lock 0")
                        r.wait("unlocked", lambda: not r.state.get("ulock"), 4)
                res["locked"] = each(o, locked)
                s.confirm(str(res["locked"]))
            with o.step("Use band on 40 m's row: CW: 7.030 CW; USB: 7.200 LSB; still the BANDS screen") as s:
                def use(n, r):
                    out = {}
                    t0 = time.time()
                    press_label(r, r"^Use\s*band", FIELD_NAMES, rest=0.8)
                    r.wait("7.030 CW", lambda: int(r.state.get("freq", 0)) == 7030000 and
                           int(r.state.get("mode", -1)) == 4, 4)
                    out["cw"] = wait_msg(r, t0, r"40m: 7\.030 MHz", 3)
                    return out
                o.changing += 1
                try:
                    res["use"] = each(o, use)
                    align(o, 7030000, 2)

                    def use_usb(n, r):
                        t0 = time.time()
                        press_label(r, r"^Use\s*band", FIELD_NAMES, rest=0.8)
                        r.wait("7.200 LSB", lambda: int(r.state.get("freq", 0)) == 7200000 and
                               int(r.state.get("mode", -1)) == 0, 4)
                        st = _fstate(r)
                        if st.get("screen") != "2" or win(r) != "Field Ops":
                            raise Refused(f"{n}: after Use band the screen {st.get('screen')}, window {win(r)!r}")
                        return wait_msg(r, t0, r"40m: 7\.200 MHz", 3)
                    res["use_usb"] = each(o, use_usb)
                finally:
                    o.changing -= 1
                H.alike(o, ("freq", "mode"))
                s.confirm(f"{res['use']} {res['use_usb']}")
            each(o, lambda n, r: _field_back(o, r))
        return True, (f"Test beacon: {res['usb'][H.A]!r}; one press {res['once'][H.A][1]!r}; tuned {res['tuned'][H.A]!r}; "
                      f"locked {res['locked'][H.A]!r}; Use band 7.030 CW, 7.200 LSB"), res
    except Refused as e:
        return False, str(e), res


def _ref_of(refs, t, pattern=None, skip=0):
    rows = [x for x in refs.get(t, []) if not pattern or re.fullmatch(pattern, x[0])]
    if len(rows) <= skip:
        raise Refused(f"no {t} reference like {pattern} in the list")
    return rows[skip]


# the screen's slashed zero reads as 4 (WCA's 'QSOs / 50' as '/ 54', the clock's 05:55 as 45:55, 11-26)
LOOKALIKE = {"5": "56S", "6": "65", "0": "0O@4", "1": "1Il", "3": "38", "8": "836B", "4": "4A", "2": "2Z",
             "9": "9og?S", "7": "7?r"}


def _call_read(txt, call):
    """call (a test call, Q9TST) somewhere in the OCR text, its digit's look-alikes taken, any case (the logbook's
    Q9TST read 'QoTst', Q7TST 'QrTsT', Q8TST 'Q6TST', 11-26)"""
    pat = "".join(f"[{re.escape(LOOKALIKE.get(c, c))}]" if c.isdigit() else re.escape(c) for c in call)
    return re.search(pat, txt, re.I) is not None


def _counter_read(txt, need):
    """'QSOs / 25' (or 'CALLS / 5') read on the screen, OCR's digit look-alikes taken (it read 25 as 26, 11-26)"""
    num = need.split("/")[-1].strip()
    word = "CALLS" if need.startswith("CALLS") else ""
    pat = r"/\s*" + "".join(f"[{re.escape(LOOKALIKE.get(c, c))}]" for c in num) + r"(?![0-9])"
    return bool(re.search(pat, txt)) and (not word or ocr_has(txt, word))


def _activation(o, tile, ref, qso_line, want, typed=None, said_ref=None):
    """On both: tile opened (its nearest taken first), Ref... typed (typed, else ref), Start, Log QSO qso_line, the
    record holding want ({tag: value}); the counter its programme needs read; End twice; Back. Its message after
    Start and the record"""
    prog = PROG[tile]

    def one(n, r):
        out = {}
        _field_tile(o, r, tile)
        try:
            _fwait(r, "its nearest taken", lambda x: (x.get("ref_f") or [""])[0] == prog, 20)
        except Refused:
            pass                                    # none near (castles without positions): typed below
        t0 = time.time()
        _field_type(r, r"^Ref", typed or ref)
        _fwait(r, f"{ref} chosen", lambda x: (x.get("ref_f") or ["", ""])[1] == ref, 6)
        out["ref_said"] = msgs_since(r, t0)[-2:]
        t1 = time.time()
        out["start"], log = _start(o, r, ref)
        time.sleep(1.5)
        out["after_start"] = msgs_since(r, t1)
        txt = shot_text(o, r, f"field {tile.lower()} activation")[1]
        need = NEEDS[prog]
        if not _counter_read(txt, need):
            raise Refused(f"{n}: no '{need}' on the {tile} activation ({' '.join(txt.split())[:120]!r})")
        out["qso"] = _log_qso(o, r, qso_line)
        rec = [x for x in _adif(r, log) if x.get("CALL") == qso_line.split()[0]]
        bad = {k: (rec[-1].get(k) if rec else None) for k, v in want.items() if not rec or rec[-1].get(k) != v}
        if bad:
            raise Refused(f"{n}: the {tile} record {rec[-1] if rec else None}: wanted {want}")
        out["rec"] = {k: rec[-1].get(k) for k in list(want) + ["MY_SIG", "MY_SIG_INFO", "SIG", "SIG_INFO"]}
        _to_label(r, r"^End", FIELD_NAMES)
        _end(o, r)
        _field_back(o, r)
        return out
    return each(o, one, wait=120)


def t_field_programmes(o):
    """Field Ops' TOTA, WCA, MOTA, LH and LLOTA on both (none spots by itself): each a reference typed (WCA's number
    without its zeros, 'DL-1156' -> DL-01156, and one the list lacks 'taken as it is'; LH's 'bel 2' -> BEL 002),
    started, its counter (TOTA / 13, WCA / 50, MOTA and LH CALLS / 4, LLOTA / 10), a QSO with another reference of the
    programme logged (MY_SIG and MY_SIG_INFO ours, SIG and SIG_INFO theirs), ended"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        with field_kept(o):
            _pos_forget(o)
            _field_open(o)
            with o.step("WCA: 'DL-9999' taken as it is; 'DL-1156' -> DL-01156, started, a QSO with DL-01157") as s:
                def unknown(n, r):
                    _field_tile(o, r, "WCA")
                    t0 = time.time()
                    _field_type(r, r"^Ref", "DL-9999")          # padded to DL-09999, a castle not in the list
                    said = wait_msg(r, t0, r"not in the castle list, taken as it is", 4)
                    _field_back(o, r)
                    return said
                res["wca_unknown"] = each(o, unknown, wait=60)
                res["wca"] = _activation(o, "WCA", "DL-01156", "Q9TST 59 59 DL-01157",
                                         {"MY_SIG": "WCA", "MY_SIG_INFO": "DL-01156", "SIG": "WCA",
                                          "SIG_INFO": "DL-01157"}, typed="DL-1156")
                s.confirm(f"{res['wca_unknown']}; {({n: x['rec'] for n, x in res['wca'].items()})}")
            with o.step("LH: 'bel 2' -> BEL 002, started, a QSO with BEL-007 (SIG ARLHS)") as s:
                res["lh"] = _activation(o, "LH", "BEL 002", "Q9TST 59 59 BEL-007",
                                        {"MY_SIG": "ARLHS", "MY_SIG_INFO": "BEL 002", "SIG": "ARLHS",
                                         "SIG_INFO": "BEL 007"}, typed="bel 2")
                s.confirm(str({n: x["rec"] for n, x in res["lh"].items()}))
            for tile, t in (("TOTA", "T"), ("MOTA", "M"), ("LLOTA", "K")):
                a, b = _ref_of(refs, t)[0], _ref_of(refs, t, skip=1)[0]
                sig = {"TOTA": "TOTA", "MOTA": "MOTA", "LLOTA": "LLOTA"}[tile]
                with o.step(f"{tile}: {a} typed, started, a QSO with {b}") as s:
                    res[tile] = _activation(o, tile, a, f"Q9TST 59 59 {b}",
                                            {"MY_SIG": sig, "MY_SIG_INFO": a, "SIG": sig, "SIG_INFO": b})
                    spot = {n: [m for m in x["after_start"] if "spot" in m.lower()] for n, x in res[tile].items()}
                    if any(spot.values()):
                        raise Refused(f"{tile}: a spot said after Start: {spot}")
                    s.confirm(str({n: x["rec"] for n, x in res[tile].items()}))
        return True, ("TOTA, WCA, MOTA, LH and LLOTA on both: references typed (DL-1156 padded, bel 2 as BEL 002, an "
                      "unknown castle taken), counters, MY_SIG/SIG in the records, nothing spotted"), res
    except Refused as e:
        return False, str(e), res


def _locator(lat, lon):
    lon += 180
    lat += 90
    a = chr(65 + int(lon // 20)) + chr(65 + int(lat // 10))
    b = str(int(lon % 20 // 2)) + str(int(lat % 10))
    c = chr(97 + int(lon % 2 * 12)) + chr(97 + int(lat % 1 * 24))
    return a + b + c


def t_field_tiles(o):
    """Field Ops' Tiles on both: the phone's position in a 6-character square: that square the reference; a position in
    the next square east: the reference follows; started, a QSO with a tile after the call (MY_SIG TILES with our
    square, their tile as SIG_INFO or GRIDSQUARE), ended"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        pos, _ = _pair_place(refs, "S")
        loc = _locator(*pos)
        pos2 = (pos[0], pos[1] + 5 / 60)                   # a subsquare is 5' of longitude: the next one east
        loc2 = _locator(*pos2)
        far = _locator(pos[0] + 1, pos[1] + 2)
        with field_kept(o):
            _pos(o, pos, 20)
            _field_open(o)
            with o.step(f"Tiles on both: {loc} the reference; moved east: {loc2}") as s:
                def follow(n, r):
                    _field_tile(o, r, "TILES")
                    _fwait(r, f"{loc}", lambda x: (x.get("ref_f") or ["", ""])[1].upper() == loc.upper(), 20)
                    r.send(f"pos {pos2[0]:.6f} {pos2[1]:.6f} 20")
                    _fwait(r, f"{loc2}", lambda x: (x.get("ref_f") or ["", ""])[1].upper() == loc2.upper(), 25)
                    return loc2
                res["follow"] = each(o, follow, wait=90)
                s.confirm(str(res["follow"]))
            with o.step(f"Start on both, a QSO 'Q9TST 59 59 {far}': MY_SIG TILES {loc2}; End") as s:
                def act(n, r):
                    said, log = _start(o, r, loc2.upper() if False else _fstate(r)["ref_f"][1])
                    q = _log_qso(o, r, f"Q9TST 59 59 {far}")
                    rec = [x for x in _adif(r, log) if x.get("CALL") == "Q9TST"]
                    got = rec[-1] if rec else {}
                    theirs = (got.get("SIG_INFO") or got.get("GRIDSQUARE") or "").upper()
                    if got.get("MY_SIG") != "TILES" or (got.get("MY_SIG_INFO") or "").upper() != loc2.upper() or \
                            theirs != far.upper():
                        raise Refused(f"{n}: the record {got}")
                    _end(o, r)
                    _field_back(o, r)
                    return {k: got.get(k) for k in ("MY_SIG", "MY_SIG_INFO", "SIG", "SIG_INFO", "GRIDSQUARE",
                                                    "MY_GRIDSQUARE")}
                res["act"] = each(o, act, wait=90)
                s.confirm(str(res["act"]))
        return True, f"Tiles on both: {loc} -> {loc2} followed, the T3 QSO {res['act'][H.A]}", res
    except Refused as e:
        return False, str(e), res


def t_field_xota_nota(o):
    """XOTA and NOTA on both: without xota.tsv the XOTA screen says to copy one; a one-line xota.tsv near the phone's
    position: its place taken; Spot: 'XOTA is a list of your own: no spot'; started, a QSO: MY_SIG XOTA, MY_SIG_INFO
    the place; ended. NOTA: every list's nearest; Spot: 'press the row to open it in its programme first'; a press on
    its first row opens that row in its own programme. xota.tsv as found after"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        pos, _ = _pair_place(refs, "S")
        line = f"X\tXOTA-BENCH1\tBunker: bench test\t{pos[0] + 0.0002:.5f}\t{pos[1]:.5f}"
        with field_kept(o):
            for r in o.r.values():
                ssh(r, "rm -f /mnt/field/xota.tsv")
            _pos(o, pos, 20)
            _field_open(o)
            with o.step("XOTA on both with no xota.tsv: 'XOTA is your own list: copy your xota.tsv'") as s:
                def none(n, r):
                    _field_tile(o, r, "XOTA")
                    time.sleep(2.0)
                    txt = shot_text(o, r, "field xota no list")[1]
                    if not ocr_has(txt, "your xota"):            # (OCR read "xota.tsy", then "opy your", 11-26)
                        raise Refused(f"{n}: the screen reads {' '.join(txt.split())[:140]!r}")
                    return True
                res["none"] = each(o, none, wait=60)
                s.confirm(str(res["none"]))
            with o.step("a one-line xota.tsv on both: XOTA-BENCH1 taken; Spot: no spot; Start, a QSO, End") as s:
                def xota(n, r):
                    out = {}
                    ssh(r, f"printf '%s\\n' '{line}' > /mnt/field/xota.tsv")
                    _fwait(r, "XOTA-BENCH1 taken", lambda x: (x.get("ref_f") or ["", ""])[1] == "XOTA-BENCH1", 30)
                    t0 = time.time()
                    k = _to_label(r, r"^Spot", FIELD_NAMES)
                    press_if(r, k, r"^Spot$", FIELD_NAMES, rest=0.8)
                    out["spot"] = wait_msg(r, t0, r"XOTA is a list of your own: no spot|Press Spot again", 4)
                    if "again" in out["spot"]:
                        # Spot asks twice before it posts anything (dialog_field.c spot_cb); the second press reaches
                        # spot_to, where an XOTA reference stops before any posting ('no spot')
                        t0 = time.time()
                        press_if(r, k, r"^Spot", FIELD_NAMES, rest=0.8)
                        out["spot"] = wait_msg(r, t0, r"XOTA is a list of your own: no spot", 4)
                    said, log = _start(o, r, "XOTA-BENCH1")
                    _log_qso(o, r, "Q9TST")
                    rec = [x for x in _adif(r, log) if x.get("CALL") == "Q9TST"]
                    got = rec[-1] if rec else {}
                    if got.get("MY_SIG") != "XOTA" or got.get("MY_SIG_INFO") != "XOTA-BENCH1":
                        raise Refused(f"{n}: the record {got}")
                    out["rec"] = {k: got.get(k) for k in ("MY_SIG", "MY_SIG_INFO")}
                    _end(o, r)
                    _field_back(o, r)
                    return out
                res["xota"] = each(o, xota, wait=120)
                s.confirm(str(res["xota"]))
            with o.step("NOTA on both: Spot 'press the row...'; the first row's press: its own programme") as s:
                def nota(n, r):
                    _field_tile(o, r, "NOTA")
                    time.sleep(3.0)
                    txt = shot_text(o, r, "field nota list")[1]
                    t0 = time.time()
                    k = _to_label(r, r"^Spot", FIELD_NAMES)
                    press_if(r, k, r"^Spot$", FIELD_NAMES, rest=0.8)
                    said = wait_msg(r, t0, r"NOTA: press the row|Press Spot again", 4)
                    if "again" in said:                     # the second press: NOTA stops before any posting
                        t0 = time.time()
                        press_if(r, k, r"^Spot", FIELD_NAMES, rest=0.8)
                        said = wait_msg(r, t0, r"NOTA: press the row", 4)
                    press(r, "MFK", FIELD_NAMES, rest=1.5)
                    st = _fwait(r, "a programme of its own", lambda x: x.get("prog") not in ("N", None), 6)
                    _field_back(o, r)
                    return {"spot": said, "prog": st.get("prog"), "rows": " ".join(txt.split())[:100]}
                res["nota"] = each(o, nota, wait=60)
                s.confirm(str(res["nota"]))
        return True, (f"XOTA: the copy message, the own place taken, {res['xota'][H.A]['spot']!r}, MY_SIG XOTA; NOTA: "
                      f"{res['nota'][H.A]['spot']!r}, a row opened in programme {res['nota'][H.A]['prog']}"), res
    except Refused as e:
        return False, str(e), res


def _tile_xy(lat, lon, z):
    import math
    n = 2 ** z
    x = int((lon + 180) / 360 * n)
    y = int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)
    return x, y


def t_field_map(o):
    """Field Ops' map on both, from test tiles copied to /mnt/field/tiles (zooms 13, 15, 17 around a summit, a credit
    line; taken out after): the reference screen gets its third page; 20 m from the summit 'You are there: Start to
    activate it'; 3 km away the distance and heading under the map; View steps map+list, map+info, map only (each a
    different screen); Zoom + changes it; a summit far from the tiles: 'No map here'"""
    import tempfile
    from PIL import Image, ImageDraw
    res = {}
    try:
        refs = _refs(o.r[H.A])
        pos, (s1, s2) = _pair_place(refs, "S")
        far = next(x for x in refs["S"] if x[2] is not None and _km(pos, (x[2], x[3])) > 300)
        d = tempfile.mkdtemp()
        for z in (13, 15, 17):
            cx, cy = _tile_xy(s1[2], s1[3], z)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    p = os.path.join(d, "tiles", str(z), str(cx + dx))
                    os.makedirs(p, exist_ok=True)
                    im = Image.new("RGB", (256, 256), ((z * 40 + dx * 60) % 256, (120 + dy * 50) % 256, 90))
                    ImageDraw.Draw(im).rectangle((0, 0, 255, 255), outline="white")
                    im.save(os.path.join(p, f"{cy + dy}.jpg"), quality=80)
        open(os.path.join(d, "tiles", "credit.txt"), "w").write("bench test tiles\n")
        with field_kept(o):
            for n, r in o.r.items():
                if ssh(r, "ls -d /mnt/field/tiles 2>/dev/null || true").strip():
                    raise Refused(f"{n} has map tiles of its own: left alone")
            for r in o.r.values():
                subprocess.run(["scp", "-4", "-q", "-r", "-o", "BatchMode=yes", os.path.join(d, "tiles"),
                                f"root@{r.host}:/mnt/field/"], check=True, timeout=120)
            try:
                _pos(o, (s1[2] + 0.00018, s1[3]), 20)        # 20 m from the summit
                _field_open(o)
                with o.step("SOTA on both: a third page; 'You are there: Start to activate it'") as s:
                    def there(n, r):
                        _field_tile(o, r, "SOTA")
                        _fwait(r, f"{s1[0]} taken", lambda x: (x.get("ref_f") or ["", ""])[1] == s1[0], 25)
                        time.sleep(2.0)
                        p1 = flat(fkeys(r)[0][0])
                        txt = shot_text(o, r, "field map there")[1]
                        if "3" not in p1 or not ocr_has(txt, "You are there"):
                            raise Refused(f"{n}: page key {p1!r}; the screen {' '.join(txt.split())[:140]!r}")
                        return p1
                    res["there"] = each(o, there, wait=60)
                    s.confirm(str(res["there"]))
                with o.step("3 km away on both: the distance and heading under the map") as s:
                    _pos(o, (s1[2] + 0.027, s1[3]), 20)
                    time.sleep(13.0)            # the nearest list rebuilt for the new place (10 s timer): page 1 again
                    got = {n: " ".join(shot_text(o, r, "field map 3 km")[1].split()) for n, r in o.r.items()}
                    res["km"] = {n: re.findall(r"\d+[.,]\d+\s*km", t) for n, t in got.items()}
                    if not all(res["km"].values()):
                        raise Refused(f"no 'N.N km' read: {({n: t[:120] for n, t in got.items()})}")
                    s.confirm(str(res["km"]))
                with o.step("page 3 on both: View three times (map+list, map+info, map only, back), Zoom +") as s:
                    def view(n, r):
                        k = _to_label(r, r"^View", FIELD_NAMES)
                        seen, shots = [_label(r, k)], [shot(o, r, "field map view 0")]
                        for i in range(3):
                            press_if(r, k, r"^View", FIELD_NAMES, rest=1.2)
                            seen.append(_label(r, k))
                            shots.append(shot(o, r, f"field map view {i + 1}"))
                        if len(set(seen[:3])) != 3 or seen[3] != seen[0]:
                            raise Refused(f"{n}: View stepped {seen}")
                        moves = [round(_region_moves(a, b, (0, 40, 800, 400)), 3) for a, b in zip(shots, shots[1:])]
                        if min(moves) < 0.02:
                            raise Refused(f"{n}: the views look alike ({moves})")
                        kz = _to_label(r, r"^Zoom\s*\+", FIELD_NAMES)
                        a = shot(o, r, "field map zoom before")
                        press_if(r, kz, r"^Zoom\s*\+", FIELD_NAMES, rest=1.5)
                        b = shot(o, r, "field map zoom +")
                        zm = round(_region_moves(a, b, (0, 40, 800, 400)), 3)
                        if zm < 0.02:
                            raise Refused(f"{n}: Zoom + changed {zm:.1%} of the map")
                        return {"views": seen, "moves": moves, "zoom": zm}
                    res["view"] = each(o, view, wait=90)
                    s.confirm(str(res["view"]))
                # The map is on the list's row the MFK is on, else the reference in use (dialog_field.c map_target): a
                # typed far reference left the map on the nearest row (11-26). The phone's position by far's summit
                # instead: the list is then its summits, none on the tiles
                with o.step(f"the phone's position by {far[0]} (far from the tiles) on both: 'No map here'") as s:
                    _pos(o, (far[2] + 0.0005, far[3]), 20)
                    time.sleep(13.0)                         # the nearest looked up again (its page back to 1)

                    def nomap(n, r):
                        txt = shot_text(o, r, "field no map")[1]
                        if not ocr_has(txt, "No map here"):
                            raise Refused(f"{n}: the screen {' '.join(txt.split())[:140]!r}")
                        _field_back(o, r)
                        return True
                    res["nomap"] = each(o, nomap, wait=60)
                    s.confirm(str(res["nomap"]))
            finally:
                for r in o.r.values():
                    try:
                        ssh(r, "rm -rf /mnt/field/tiles")
                    except (Refused, OSError, subprocess.SubprocessError):
                        pass
        return True, f"Field Ops' map on both: You are there, {res['km'][H.A]}, views {res['view'][H.A]['views']}, " \
                     f"zoom, No map here", res
    except Refused as e:
        return False, str(e), res


def t_field_spotting_programmes(o):
    """WWFF, WWBOTA and HOTA on both with a CB call (Field Ops spots nothing for one and says so: nothing posted):
    each a reference typed, started (its automatic spot refused: 'No spot with a CB callsign'), its counter (WWFF /
    44, WWBOTA / 25, HOTA CALLS / 5), a QSO with another of its references (MY_SIG and SIG of the programme), ended;
    the test call back after"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        H.test_calls(o)
        with field_kept(o), cfg_kept(o.r[H.A], {"callsign": CB_TEST_CALL}), \
                cfg_kept(o.r[H.B], {"callsign": CB_TEST_CALL}):
            _pos_forget(o)
            _field_open(o)
            for tile, t, sig, pat in (("WWFF", "F", "WWFF", r"[A-Z0-9]+FF-\d{4}"),
                                      ("WWBOTA", "B", "WWBOTA", r"B/[A-Z0-9]+-\d{4}"),
                                      ("HOTA", "H", "HOTA", r"[A-Z0-9]+-H\d{4}")):
                a, b = _ref_of(refs, t, pat)[0], _ref_of(refs, t, pat, skip=1)[0]
                if t == "B" and re.match(r"B/(S5|Z3)-", a):
                    a, b = _ref_of(refs, t, pat, skip=2)[0], _ref_of(refs, t, pat, skip=3)[0]
                with o.step(f"{tile}: {a} typed, started (no spot: a CB call), a QSO with {b}, ended") as s:
                    got = _activation(o, tile, a, f"Q9TST 59 59 {b}",
                                      {"MY_SIG": sig, "MY_SIG_INFO": a, "SIG": sig, "SIG_INFO": b})
                    for n, x in got.items():
                        if not any("No spot with a CB callsign" in m for m in x["after_start"]):
                            raise Refused(f"{n}: {tile}'s Start said {x['after_start']} (no CB refusal of its spot)")
                    res[tile] = {n: x["rec"] for n, x in got.items()}
                    s.confirm(str(res[tile]))
        return True, "WWFF, WWBOTA and HOTA on both: typed, started (spots refused for the CB call), counters, the " \
                     "records' MY_SIG and SIG", res
    except Refused as e:
        return False, str(e), res


def t_field_persist(o):
    """Field Ops across a GUI restart on both (planned, one radio after the other): a SOTA activation running and
    Start here on (field_boot): after the restart Field Ops is on the screen at once, the activation as it was (the
    state file's log and reference); Start here off again, the activation ended, Field Ops as found"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        pos, (s1, _) = _pair_place(refs, "S")
        with field_kept(o):
            _pos(o, pos, 20)
            _field_open(o)
            with o.step(f"a SOTA activation on {s1[0]} on both; Start here on") as s:
                def start(n, r):
                    _field_tile(o, r, "SOTA")
                    _field_type(r, r"^Ref", s1[0])
                    _fwait(r, f"{s1[0]}", lambda x: (x.get("ref_f") or ["", ""])[1] == s1[0], 6)
                    said, log = _start(o, r, s1[0])
                    return log
                res["log"] = each(o, start, wait=60)
                for r in o.r.values():
                    bench.cfg_set(r, "field_boot", 1)
                s.confirm(str(res["log"]))
            try:
                for n in (H.A, H.B):
                    r = o.r[n]
                    with o.step(f"{n}'s GUI restarted (planned): Field Ops at once, the activation as it was") as s:
                        gui_restart(o, n, "Field Ops across a restart")
                        r.wait("Field Ops on the screen", lambda: win(r) == "Field Ops", 15)
                        st = _fstate(r)
                        if st.get("active") != "1" or st.get("log") != res["log"][n] or \
                                (st.get("ref_f") or ["", ""])[1] != s1[0]:
                            raise Refused(f"{n}: after the restart {st}")
                        txt = shot_text(o, r, "field after restart")[1]
                        res[n] = {"screen": st.get("screen"), "ref_on_screen": ocr_has(txt, s1[0].split("/")[-1])}
                        s.confirm(str(res[n]))
            finally:
                for r in o.r.values():
                    try:
                        bench.cfg_set(r, "field_boot", 0)
                    except Refused:
                        pass
            with o.step("the activation ended on both") as s:
                def end(n, r):
                    if win(r) != "Field Ops":
                        r.send("open 118")
                        r.wait("Field Ops", lambda: win(r) == "Field Ops", 10)
                    return _end(o, r)[1]
                res["end"] = each(o, end, wait=60)
                s.confirm(str(res["end"]))
        return True, f"Field Ops on both after a restart: Start here opened it on the running activation ({res})", res
    except Refused as e:
        return False, str(e), res


# ---- Channels: not tuned while an app holds the dial (the hand mic's V/M over FT8) ----

def t_channels_app_holds(o):
    """The Channels window opened by the hand mic's V/M (the remote's hkey) over FT8 on both (V/M opens it over FT8,
    not over the decoders that have the screen): a channel pressed is not tuned, 'Channel not tuned: the app holds the
    frequency', the window still there (its buttons: Close, Add...), the dial as FT8 has it; Close closes it"""
    res = {}
    try:
        H.ft8_both(o, 14074000)
        with o.step("V/M on both over FT8: the Channels window (its buttons)") as s:
            def vm(n, r):
                r.take_control()
                r.send("hkey VM 1")
                time.sleep(0.2)
                r.send("hkey VM 0")
                time.sleep(1.5)
                texts = [flat(t) for t in fkeys(r)[0]]
                if not any(t.startswith("Close") for t in texts) or not any(t.startswith("Add") for t in texts):
                    raise Refused(f"{n}: no Channels window after V/M ({texts})")
                return texts
            res["vm"] = each(o, vm)
            s.confirm(str(res["vm"]))
        with o.step("a channel pressed on both: 'Channel not tuned: the app holds ...', the dial kept, the window there") as s:
            def press_ch(n, r):
                f0 = int(r.state.get("freq", 0))
                t0 = time.time()
                r.take_control()
                r.send("knob mfk 1")
                time.sleep(0.6)
                r.send("key MFK 1")
                time.sleep(0.15)
                r.send("key MFK 0")
                said = wait_msg(r, t0, r"Channel not tuned", 4)
                time.sleep(0.8)
                texts = [flat(t) for t in fkeys(r)[0]]
                if int(r.state.get("freq", 0)) != f0:
                    raise Refused(f"{n}: the dial moved {f0} -> {r.state.get('freq')} under FT8")
                if not any(t.startswith("Add") for t in texts):
                    raise Refused(f"{n}: the window gone after the refusal ({texts})")
                return said
            res["refused"] = each(o, press_ch)
            s.confirm(str(res["refused"]))
        with o.step("Close (F1) on both: the window gone, FT8 on top again with its buttons") as s:
            def close(n, r):
                if fkey_of(r, r"^Close") != "F1":
                    raise Refused(f"{n}: F1 is not Close ({[flat(t) for t in fkeys(r)[0]]})")
                r.take_control()
                bench.tap(r, "F1")
                time.sleep(1.0)
                texts = [flat(t) for t in fkeys(r)[0]]
                if any(t.startswith("Add") for t in texts) or win(r) != "FT8" or "FT8" not in texts[0]:
                    raise Refused(f"{n}: after Close the window {win(r)!r}, buttons {texts}")
                return texts
            res["closed"] = each(o, close)
            s.confirm(str(res["closed"]))
        with o.step("FT8 closed on both: the dial free again (14.200 USB taken)") as s:
            _ft8_close(o)
            home_both(o)
            align(o, 14200000, 2)
            s.confirm(f"{o.r[H.A].state.get('freq')}")
        return True, f"Channels over FT8 on both: {res['refused'][H.A]!r}, the window kept, closed", res
    except Refused as e:
        return False, str(e), res
    finally:
        _ft8_close(o)
        home_both(o)


# ---- Birdie marks: red ticks at the top of the waterfall ----

BIRDIE_FILE = "/mnt/birdies.txt"


def _red_ticks(a, b, y0=40, y1=420):
    """The columns where b has red that a has not (the birdie ticks), grouped: [(x, y_top)]"""
    from PIL import Image
    A, B = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    w, h = B.size
    pa, pb = A.load(), B.load()
    cols = {}
    for y in range(y0, min(y1, h)):
        for x in range(w):
            r_, g_, b_ = pb[x, y]
            if r_ > 170 and r_ - g_ > 60 and r_ - b_ > 60 and pa[x, y] != pb[x, y]:     # TC(err): red to salmon
                cols.setdefault(x, []).append(y)
    xs = sorted(cols)
    groups, cur = [], []
    for x in xs:
        if cur and x - cur[-1] > 2:
            groups.append(cur)
            cur = []
        cur.append(x)
    if cur:
        groups.append(cur)
    ticks = [((g[0] + g[-1]) / 2, min(min(cols[x]) for x in g), len(g)) for g in groups if len(g) >= 2]
    if not ticks:
        return []
    from collections import Counter
    top = Counter(t[1] for t in ticks).most_common(1)[0][0]     # the ticks share the waterfall's top row
    return [t for t in ticks if abs(t[1] - top) <= 1]


def t_birdie_marks(o):
    """Birdie marks on both: a test birdies list (five lines 2 kHz apart around 14.200) put in place of the radio's
    own, each GUI restarted to read it (planned); the classic screen on 14.200 USB: Marks off, then on (birdie_marks
    1): red ticks come at the top of the waterfall, five of them, evenly spaced; Marks as found, the radio's own list
    back (restarted again)"""
    res = {}
    hz = [14196000, 14198000, 14200000, 14202000, 14204000]
    try:
        for r in o.r.values():
            ssh(r, "set -- save\n" + KEEP.format(f=BIRDIE_FILE))
        try:
            for n, r in o.r.items():
                gui_restart(o, n, "a test birdies list read",
                            f"printf '%s\\n' {' '.join(str(x) for x in hz)} > {BIRDIE_FILE}")
            with o.step("both on the classic screen on 14.200 USB; Marks off, then on: the ticks") as s:
                import orch_cable as oc
                o.changing += 1
                try:
                    for r in o.r.values():
                        oc.classic(r)
                    align(o, 14200000, 2)
                finally:
                    o.changing -= 1

                def ticks(n, r):
                    with cfg_kept(r, {"birdie_marks": 0}):
                        time.sleep(1.0)
                        a = shot(o, r, "birdie marks off")
                        bench.cfg_set(r, "birdie_marks", 1)
                        time.sleep(1.5)
                        b = shot(o, r, "birdie marks on")
                    t = _red_ticks(a, b)
                    xs = sorted(x for x, _, _ in t)
                    gaps = [round(q - p, 1) for p, q in zip(xs, xs[1:])]
                    # 2 kHz apart: every gap one or two of the smallest (the centre one may sit under the VFO line)
                    g = min(gaps) if gaps else 0
                    odd = [x for x in gaps if g <= 0 or min(abs(x - g), abs(x - 2 * g)) > 3]
                    if len(xs) < len(hz) - 1 or odd:
                        raise Refused(f"{n}: red ticks at {xs} (gaps {gaps}), {len(hz)} 2 kHz apart wanted")
                    return {"x": xs, "top": [y for _, y, _ in t]}
                res = each(o, ticks, wait=60)
                s.confirm(str(res))
        finally:
            for n, r in o.r.items():
                try:
                    gui_restart(o, n, "the radio's own birdies list back",
                                "set -- back\n" + KEEP.format(f=BIRDIE_FILE))
                except Refused as e:
                    o.st.ev(f"{n}: {e}", "bad")
        return True, f"birdie marks on both: five red ticks evenly spaced at the waterfall's top ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- CW Keyboard: NOT CW, and no TX where the radio may not send ----

def t_cwkbd_refusals(o):
    """CW Keyboard on both with a CW message of the test's (added on the web page, taken out after): in USB its state
    reads NOT CW (its screen, OCR) and the message pressed says 'Set CW mode to transmit', nothing keyed; in CW on
    27.185 (no amateur band, TX outside them off) the message pressed: 'No TX on 27.185 MHz', nothing keyed. One
    radio presses at a time (the guard stops either that would send)"""
    import orch_cable as oc
    res = {}
    mids = {}
    try:
        for n, r in o.r.items():
            if str(cfg(r, "tx_outside_bands")) != "0":
                raise Refused(f"{n}: TX outside ham bands is on: nothing pressed")
            mids[n] = oc._cw_msg_add(r, "BENCH")[0]
        o.both({"pwr": H.W})
        with o.step("CW Keyboard open on both in USB 14.200: NOT CW on its screen") as s:
            align(o, 14200000, 2)                   # before: the dial set under the app is refused
            open_both(o, 117, "CW Keyboard")
            got = {n: ocr_has(shot_text(o, r, "cwkbd usb")[1], "NOT CW") for n, r in o.r.items()}
            if not all(got.values()):
                raise Refused(f"NOT CW not read: {got}")
            s.confirm(str(got))
        for n in (H.A, H.B):
            r = o.r[n]
            with o.step(f"{n}: the message BENCH pressed in USB: 'Set CW mode to transmit', nothing keyed") as s:
                t0 = time.time()
                press_label(r, r"^BENCH", ("CW Keyboard",), rest=1.0)
                said = wait_msg(r, t0, r"Set CW mode to transmit", 4)
                time.sleep(2.0)
                if oc.tx_spans(r, t0):
                    raise Refused(f"{n} keyed in USB")
                res[f"{n}_usb"] = said
                s.confirm(said)
        with o.step("both in CW on 27.185 (the app closed for it, opened again)") as s:
            close_both(o)
            align(o, 27185000, 4)
            open_both(o, 117, "CW Keyboard")
            H.alike(o, ("freq", "mode"))
            s.confirm(f"{o.r[H.A].state.get('freq')} mode {o.r[H.A].state.get('mode')}")
        for n in (H.A, H.B):
            r = o.r[n]
            with o.step(f"{n}: the message pressed on 27.185 CW: 'No TX on 27.185 MHz', nothing keyed") as s:
                t0 = time.time()
                press_label(r, r"^BENCH", ("CW Keyboard",), rest=1.0)
                said = wait_msg(r, t0, r"No TX on 27\.185", 5)
                time.sleep(2.0)
                bench.tap(r, "F3") if fkey_of(r, r"^Stop") == "F3" else None
                if oc.tx_spans(r, t0):
                    raise Refused(f"{n} KEYED on 27.185")
                res[f"{n}_cb"] = said
                s.confirm(said)
        return True, f"CW Keyboard: {res[H.A + '_usb']!r} in USB, {res[H.A + '_cb']!r} on 27.185, nothing keyed", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        try:
            o.both({"freq": 14200000, "mode": 2})
        except Refused:
            pass
        for n, m in mids.items():
            try:
                oc._cw_msg_del(o.r[n], m)
            except Exception:  # noqa: BLE001
                pass


# ---- Propagation, Flights, Weather: what they show of the kept file and the position ----

def t_prop_conditions(o):
    """Propagation's Conditions on both: the SFI and K of /mnt/conditions.txt (as the radio has it, read over SSH) on
    its screen (OCR), with its age in the header"""
    res = {}
    try:
        with o.step("Propagation open on both: the file's SFI and K on the screen") as s:
            open_both(o, 126, "Propagation")
            time.sleep(3.0)

            def one(n, r):
                kv = dict(x.split("|", 1) for x in ssh(r, "cat /mnt/conditions.txt").splitlines() if "|" in x)
                vals = {key: kv.get(key, "").strip() for key in ("sfi", "ssn", "a", "k")}
                txt = shot_text(o, r, "propagation conditions")[1]
                flat_txt = " ".join(txt.split())
                read = [key for key, v in vals.items() if v and re.search(rf"(?<!\d){re.escape(v)}(?!\d)", flat_txt)]
                # OCR misreads a digit now and then (SFI 87 read '6?', 11-26): the labels and two of the file's figures
                if not ocr_has(txt, "SFI") or not ocr_has(txt, "Sunspots") or len(read) < 2:
                    raise Refused(f"{n}: the file's {vals} not on the screen ({flat_txt[:160]!r})")
                return {"file": vals, "read": read}
            res = each(o, one, wait=60)
            s.confirm(str(res))
        return True, f"Propagation shows the kept conditions on both ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_flights(o):
    """Flights on both: Range steps 100 -> 200 -> 400 -> 25 -> 50 -> 100 km (the button); the header names the
    position it uses: the QTH locator, then the phone's ('pos', coarse to the app's 0.1 degree) as Phone"""
    res = {}
    names = ("Flights",)
    try:
        refs = _refs(o.r[H.A])
        pos, _ = _pair_place(refs, "S")
        _pos_forget(o)
        with o.step("Flights open on both: the header 'Locator'; Range round the five") as s:
            open_both(o, 203, "Flights")
            time.sleep(2.0)

            def rng(n, r):
                txt = shot_text(o, r, "flights locator")[1]
                if not ocr_has(txt, "Locator"):
                    raise Refused(f"{n}: no Locator in the header ({' '.join(txt.split())[:120]!r})")
                k = _to_label(r, r"^Range", names)
                seen = [_label(r, k)]
                for _ in range(5):
                    press(r, k, names, rest=0.8)
                    seen.append(_label(r, k))
                km = [int(re.search(r"(\d+) km", x).group(1)) for x in seen]
                if sorted(set(km)) != [25, 50, 100, 200, 400] or km[0] != km[-1]:
                    raise Refused(f"{n}: Range stepped {seen}")
                return km
            res["range"] = each(o, rng, wait=60)
            s.confirm(str(res["range"]))
        with o.step("the phone's position on both: the header Phone") as s:
            _pos(o, pos, 20)
            close_both(o)
            open_both(o, 203, "Flights")
            time.sleep(2.0)
            got = {n: ocr_has(shot_text(o, r, "flights phone")[1], "Phone") for n, r in o.r.items()}
            if not all(got.values()):
                raise Refused(f"no Phone in the header: {got}")
            s.confirm(str(got))
        return True, f"Flights: Range {res['range'][H.A]}; the header Locator, then Phone", res
    except Refused as e:
        return False, str(e), res
    finally:
        _pos_forget(o)
        home_both(o)


def t_weather(o):
    """Weather on both (where from: the radio's own): the header names the QTH locator ('Locator'), then with the
    phone's position 'Phone'"""
    res = {}
    try:
        refs = _refs(o.r[H.A])
        pos, _ = _pair_place(refs, "S")
        _pos_forget(o)
        if any(str(cfg(r, "meteo_from")) != "0" for r in o.r.values()):
            raise Refused("Weather's 'where from' is not the radio's own on both")
        with o.step("Weather open on both: 'Locator' in the header") as s:
            open_both(o, 200, "Weather")
            time.sleep(2.5)
            got = {n: ocr_has(shot_text(o, r, "weather locator")[1], "Locator") for n, r in o.r.items()}
            if not all(got.values()):
                raise Refused(f"no Locator in the header: {got}")
            s.confirm(str(got))
        with o.step("the phone's position on both: 'Phone' in the header") as s:
            _pos(o, pos, 20)
            close_both(o)
            open_both(o, 200, "Weather")
            time.sleep(2.5)
            got = {n: ocr_has(shot_text(o, r, "weather phone")[1], "Phone") for n, r in o.r.items()}
            if not all(got.values()):
                raise Refused(f"no Phone in the header: {got}")
            s.confirm(str(got))
        return True, "Weather's header on both: Locator, then Phone with the phone's position", res
    except Refused as e:
        return False, str(e), res
    finally:
        _pos_forget(o)
        home_both(o)


# ---- CW Trainer (115): its groups and calls keyed in train mode (sidetone, 0 W), answered by TCI CW on the radio ----

CWT_NAMES = ("CW Trainer",)
CWT_FILE = "/mnt/cw_trainer.json"


def _cwt_stats(r):
    try:
        return json.loads(ssh(r, f"cat {CWT_FILE} 2>/dev/null || echo {{}}"))
    except ValueError:
        return {}


def _tci_cw(r, text, max_s=40):
    """text keyed by r's own keyer through its TCI, from a program on the radio (tci_cw_tx.py): returns at once"""
    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_cw_tx.py"),
                    f"root@{r.host}:/tmp/tci_cw_tx.py"], check=True, timeout=30)
    return subprocess.Popen(["ssh", "-4", "-n", "-o", "BatchMode=yes", f"root@{r.host}",
                             f"python3 /tmp/tci_cw_tx.py '{text}' {max_s}"], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def _cwt_start(r):
    """Start, then at once Pause: the group (or call) still comes a second after the press, and after its result
    nothing follows by itself (Learn and Calls play on otherwise, the next one a second after a right answer: a
    picture or a slow poll then fell into its keying, and the decoder misread it, 11-26). Nothing on the radio meanwhile:
    a picture taken while the group plays made the decoder read 'EÄMM' for KMM"""
    k = _to_label(r, r"^Start", CWT_NAMES)
    t_press = time.time()
    press(r, k, CWT_NAMES, rest=0.05)

    def label_is(pat, what):
        end = time.time() + 2.0
        while not re.match(pat, _label(r, k)):
            if time.time() > end:
                raise Refused(f"{r.name}: after {what} the key reads {_label(r, k)!r}")
            time.sleep(0.1)
    label_is(r"^Pause", "Start")
    # the decoder's text from just before the group (PRESS_MS, a second after the press): the receiver's noise
    # decoded while the keys were pressed ('EIIEEI' before a call, 11-26) is not the group's
    time.sleep(max(0.0, t_press + 0.85 - time.time()))
    n_start = len(r.cw)
    press(r, k, CWT_NAMES, rest=0.05)
    label_is(r"^Start", "Pause")
    return n_start


def _cwt_state(r, want, limit, what):
    """Until the trainer's round (the remote's cfg? cwt_state) is one of want: its state then. A few reads a second
    over the bench's own remote connection, nothing over SSH (a new SSH session every 0.5 s, on the radio's CPU, made
    the decoder hear 'MMM' as 'EE', 11-26)"""
    end, last = time.time() + limit, None
    while True:
        try:
            last = str(bench.cfg_get(r, "cwt_state", timeout=3))
        except Exception as e:          # a reply lost: read again
            last = f"? ({e})"
        if last in want:
            return last
        if time.time() > end:
            raise Refused(f"{r.name}: {what}: the trainer's round {last!r} after {limit:.0f} s")
        time.sleep(0.3)


def _cwt_round(o, r, answer=None, start=True, speed=None, n0=None, on_start=None):
    """One group or call: Start (or the trainer's own next one); the group from the trainer itself (the remote's
    cwt_target once cwt_state says it has been keyed: the radio's decoder copying its own sidetone gave 'MMM' as
    'EE', 'E1M' for a level-2 group on the X6100 and nothing on the X6200, 11-26: the test then keyed back what the
    decoder made of it); answered by TCI (answer: a function of the group, None: the group itself); the trainer's
    result awaited, the stats file's tries read once after. Returns (group, answer, stats)"""
    n0 = len(r.cw) if n0 is None else n0
    tries0 = _cwt_stats(r).get("tries", 0)
    if start:
        n0 = _cwt_start(r)
    if on_start:
        on_start()
    _cwt_state(r, ("listen",), 60 if speed and speed < 10 else 30, "the group keyed")
    g = str(bench.cfg_get(r, "cwt_target", timeout=3) or "")
    if not g:
        raise Refused(f"{r.name}: 'Your turn' with no group (cwt_target empty)")
    time.sleep(1.0)                     # the group's last character out of the decoder (about 8 dots), then the answer
    dec = re.sub(r"\s+", " ", r.cw[n0:]).strip()
    a = answer(g) if answer else g
    n1 = len(r.cw)
    # Nothing else on the radio while the answer is keyed: the keyer's end awaited, then the trainer's result
    p = _tci_cw(r, a, 60)
    try:
        p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
    heard = lambda: re.sub(r"\s+", " ", r.cw[n1:]).strip()
    try:
        _cwt_state(r, ("result", "ready"), 20 if not speed or speed >= 10 else 40, "the answer keyed")
    except Refused as e:
        raise Refused(f"{e}: the answer {a!r} to {g!r} not taken (the decoder heard {heard()!r})")
    st = _cwt_stats(r)
    if st.get("tries", 0) == tries0:    # the file written a moment after the result
        time.sleep(1.5)
        st = _cwt_stats(r)
    o.st.ev(f"{r.name}: trainer group {g!r} (the decoder's copy of the sidetone {dec!r}), keyed back {a!r}, the "
            f"decoder heard {heard()!r}", "conf")
    return g, a, st


@contextlib.contextmanager
def trainer_on(o, n, label, settings, f=14040000):
    """n's CW Trainer open in a TX session (its sidetone keys the base at 0 W: the Watch on, n's role TX, the other
    radio on the same dial listening) on f, CW; its settings and stats file as found after"""
    import orch_cable as oc
    r = o.r[n]
    align(o, f, 4)
    with file_kept(r, CWT_FILE), cfg_kept(r, settings), oc.session(o, label, [n], f) as watch:
        watch.shots_on_tx = False       # train mode's keying is an over to the Watch: no screen grab into the sidetone
        oc.role(o, n)
        o.changing += 1
        try:
            r.send("open 115")
            r.wait("the CW Trainer", lambda: win(r) == "CW Trainer", 10)
            time.sleep(1.0)
            # leaving the Sentry gave back the classic screen's own dial (14.074 came up, 11-26): f again, the other
            # radio with it
            bench.ensure(r, {"freq": f, "mode": 4})
            other = next(x for m, x in o.r.items() if m != n)
            bench.ensure(other, {"freq": int(r.state.get("freq", 0)), "mode": int(r.state.get("mode", 4))})
        finally:
            o.changing -= 1
        try:
            yield r, watch
        finally:
            o.changing += 1
            try:
                bench.close_apps(r)
            finally:
                o.changing -= 1


def t_cwtrainer_learn(o):
    """The CW Trainer's Learn on each radio in turn (level 2: K and M, groups of 3, 20 WPM): Start says 'Listen...'
    and the group comes a second later; its characters the level's only; keyed back by TCI CW, the decoder checks it:
    right (the stats file's tries and right +1, each character's last 20 tries kept); then an answer with a prosign
    (|AR|): '<?> = not a character' on the screen, wrong; at 5 WPM the group is still the level's (an M, not TT) and
    keyed back right"""
    res = {}
    try:
        errs = {}
        for n in (H.A, H.B):
            out = {}
            try:
                with o.step(f"{n}: Learn at level 2, 20 WPM: 'Listen...', a group of K and M, keyed back: right") as s:
                    with trainer_on(o, n, f"cwtrainer-learn {n}",
                                    {"cwt_level": 2, "cwt_group": 3, "key_speed": 20}) as (r, watch):
                        k = _to_label(r, r"^Start", CWT_NAMES)
                        st0 = _cwt_stats(r)
                        # no picture while the group plays: the screen's capture (and the banner taken off and put back)
                        # held the radio long enough for the decoder to read 'EÄMM' for KMM (11-26); 'Listen...' last
                        g, a, st = _cwt_round(o, r, start=True)
                        if set(g) - set("KM"):
                            raise Refused(f"{n}: level 2's group decoded {g!r}: not K and M only")
                        if st.get("tries", 0) != st0.get("tries", 0) + 1 or st.get("right", 0) != st0.get("right", 0) + 1:
                            raise Refused(f"{n}: answered {a!r} to {g!r}: tries {st0.get('tries')} -> {st.get('tries')}, "
                                          f"right {st0.get('right')} -> {st.get('right')}")
                        lens = {c: (v[0] if isinstance(v, list) else v) for c, v in (st.get("chars") or {}).items()}
                        if any(isinstance(x, int) and x > 20 for x in lens.values()):
                            raise Refused(f"{n}: a character keeps more than its last 20 tries: {lens}")
                        out["right"] = (g, a)
                        s.confirm(str(out))
                        with o.step(f"{n}: the next group answered with a prosign: '<?> = not a character', wrong") as s2:
                            st0 = _cwt_stats(r)
                            g2, a2, st2 = _cwt_round(o, r, answer=lambda grp: grp[:-1] + "|AR|")
                            time.sleep(0.8)
                            txt = shot_text(o, r, "cw trainer prosign")[1]
                            if st2.get("tries", 0) != st0.get("tries", 0) + 1 or st2.get("right") != st0.get("right"):
                                raise Refused(f"{n}: {a2!r} to {g2!r}: tries {st0.get('tries')} -> {st2.get('tries')}, "
                                              f"right {st0.get('right')} -> {st2.get('right')}")
                            if not ocr_has(txt, "not a character"):
                                raise Refused(f"{n}: no 'not a character' on the screen ({' '.join(txt.split())[:140]!r})")
                            out["prosign"] = (g2, a2)
                            s2.confirm(str(out["prosign"]))
                        with o.step(f"{n}: 5 WPM: a group still K and M, keyed back right") as s3:
                            bench.cfg_set(r, "key_speed", 5)
                            bench.cfg_set(r, "cwt_group", 2)
                            st0 = _cwt_stats(r)
                            time.sleep(1.0)
                            g3, a3, st3 = _cwt_round(o, r, start=True, speed=5)
                            if set(g3) - set("KM"):
                                raise Refused(f"{n}: at 5 WPM the group decoded {g3!r}")
                            if st3.get("right", 0) <= st0.get("right", 0):
                                raise Refused(f"{n}: at 5 WPM {a3!r} to {g3!r} not right (stats {st0.get('right')} -> "
                                              f"{st3.get('right')})")
                            out["slow"] = (g3, a3)
                            s3.confirm(str(out["slow"]))
                        with o.step(f"{n}: Start once more: 'Listen...' on the screen before the group") as s4:
                            time.sleep(1.5)
                            k = _to_label(r, r"^Start", CWT_NAMES)
                            press(r, k, CWT_NAMES, rest=0.1)
                            out["listen"] = ocr_has(shot_text(o, r, "cw trainer after Start")[1], "Listen")
                            if not out["listen"]:
                                raise Refused(f"{n}: no 'Listen...' on the screen right after Start")
                            s4.confirm("Listen... read")
            except Refused as e:     # the other radio still tried: what each did is known
                errs[n] = str(e)
            res[n] = out
        if errs:
            return False, "; ".join(errs.values()), res
        return True, f"CW Trainer Learn on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_cwtrainer_calls(o):
    """The CW Trainer's Calls on each radio in turn (KEY Speed 20): a call keyed, keyed back by TCI CW with a space
    inside it: right, the next call 1 WPM faster (TX speed Follows: KEY Speed follows it); TX speed Fixed (page 2):
    KEY Speed back to 20 and staying there after another right call, the hint 'Key back the callsign at your 20 WPM';
    Follows again and out of Calls: KEY Speed back to 20"""
    res = {}
    try:
        errs = {}
        for n in (H.A, H.B):
            out = {}
            try:
                with trainer_on(o, n, f"cwtrainer-calls {n}",
                                {"key_speed": 20, "cwt_tx_fixed": 0}) as (r, watch):
                    with o.step(f"{n}: Mode to Calls; a call keyed back with a space inside: right, KEY Speed 21") as s:
                        k = _to_label(r, r"^Mode", CWT_NAMES)
                        for _ in range(3):
                            if "Calls" in _label(r, k):
                                break
                            press(r, k, CWT_NAMES, rest=0.8)
                        if "Calls" not in _label(r, k):
                            raise Refused(f"{n}: Mode reads {_label(r, k)!r}")
                        g, a, st = _cwt_round(o, r, answer=lambda c: c[:2] + " " + c[2:])
                        o.st.ev(f"{n}: call {g!r} keyed back as {a!r}", "conf")
                        # right: the next call 1 WPM faster (wrong: slower). Follows: the next call 1 WPM faster, KEY Speed with it as it starts (start_call)
                        sp = {}
                        g1, a1, st1 = _cwt_round(o, r, on_start=lambda: sp.update(
                            k=wait_cfg(r, "KEY Speed 21 for the next call", lambda v: str(v) == "21", "key_speed", 4)))
                        out["follows"] = (g, a, g1, a1, cfg(r, "key_speed"))
                        s.confirm(str(out["follows"]))
                    with o.step(f"{n}: TX speed Fixed: KEY Speed 20 again, kept after a right call; the hint") as s:
                        _to_label(r, r"^TX speed", CWT_NAMES)
                        press_label(r, r"^TX speed", CWT_NAMES, rest=0.8)
                        if str(cfg(r, "cwt_tx_fixed")) != "1" or str(cfg(r, "key_speed")) != "20":
                            raise Refused(f"{n}: Fixed: cwt_tx_fixed {cfg(r, 'cwt_tx_fixed')}, KEY Speed "
                                          f"{cfg(r, 'key_speed')}")
                        hint = ocr_has(shot_text(o, r, "cw trainer fixed hint")[1], "at your 20 WPM")
                        _to_label(r, r"^\(CWT 1", CWT_NAMES)
                        g2, a2, st2 = _cwt_round(o, r)
                        time.sleep(3.0)
                        if str(cfg(r, "key_speed")) != "20":
                            raise Refused(f"{n}: Fixed, KEY Speed moved to {cfg(r, 'key_speed')}")
                        out["fixed"] = (g2, a2, hint)
                        if not hint:
                            raise Refused(f"{n}: the hint 'Key back the callsign at your 20 WPM' not read")
                        s.confirm(str(out["fixed"]))
                    with o.step(f"{n}: Follows again, Mode on out of Calls: KEY Speed 20, Fixed kept as set") as s:
                        press_label(r, r"^TX speed", CWT_NAMES, rest=0.8)
                        k = _to_label(r, r"^Mode", CWT_NAMES)
                        press(r, k, CWT_NAMES, rest=1.0)
                        out["out"] = (cfg(r, "key_speed"), cfg(r, "cwt_tx_fixed"), _label(r, k))
                        if str(out["out"][0]) != "20":
                            raise Refused(f"{n}: out of Calls KEY Speed {out['out'][0]}")
                        s.confirm(str(out["out"]))
            except Refused as e:     # the other radio still tried: what each did is known
                errs[n] = str(e)
            res[n] = out
        if errs:
            return False, "; ".join(errs.values()), res
        return True, f"CW Trainer Calls on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_cwtrainer_out_of_band(o):
    """The CW Trainer where the radio may not transmit, each radio in turn (its group at 0 W in train mode, the Watch
    on): with Settings > TX Cal > TX ranges on South Korea (cfg tx_ranges 1, put back after) on 7.250 MHz (US 40 m,
    outside Korea's 7.000-7.200) Start says 'No TX on 7.250 MHz' and 'TX ranges: Settings > TX Cal', nothing keyed or
    decoded. Not here: the trainer sounding outside the amateur bands (0.600 MHz): the bench keys nothing there, not
    even at 0 W (its TX gate: '600000 Hz is outside the amateur bands'); the X6200's airband (receive only): set to
    120 MHz CW under the trainer, the X6200 showed the Sentry's AIR page instead (11-26)"""
    import orch_cable as oc
    res = {}
    try:
        for n in (H.A, H.B):
            out = {}
            with o.step(f"{n}: TX ranges South Korea, 7.250 MHz CW: Start refused ('No TX on 7.250', 'TX ranges'), "
                        f"nothing keyed") as s:
                with cfg_kept(o.r[n], {"tx_ranges": 1}):
                    with trainer_on(o, n, f"cwtrainer-out-of-band {n} 7.250 KR", {"cwt_level": 2, "cwt_group": 2},
                                    f=7250000) as (r, watch):
                        t0, n0 = time.time(), len(r.cw)
                        _cwt_start(r)
                        said = wait_msg(r, t0, r"No TX on 7\.250", 6)
                        why = wait_msg(r, t0, r"TX ranges", 4)
                        time.sleep(2.0)
                        # nothing keyed (the decoder is no judge: with the trainer open it reads the receiver's noise
                        # too, 'IHES' with nothing sent, 11-26)
                        if oc.tx_spans(r, t0) or r.state.get("tx"):
                            raise Refused(f"{n}: refused, yet it keyed")
                        out["kr"] = (said, why)
                        s.confirm(str(out["kr"]))
            res[n] = out
        return True, f"the CW Trainer outside the bands and where it may not transmit: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)
        try:
            align(o, 14040000, 4)
        except Refused:
            pass


# ---- the first-start guide: a radio with no callsign, a restart ----

FS_NAMES = ("First start",)


def t_first_start(o):
    """The first-start guide on both: the callsign blank and first_start_done 0 (cfg), the GUI restarted (planned):
    the guide opens by itself ('FIRST START', step 1 Language); Next and Skip walk it: the keyboard step skipped
    (nothing scanned), WiFi's line names the network, Networks opens the WiFi window and GEN brings the guide back;
    Callsign typed (the test call, read back), Locator typed (the radio's own, read back); Country's band plan line;
    the Clock step set by NTP; the summary says Skipped and gives the web address; Done: first_start_done 1; another
    restart: the guide does not open again. Callsign, locator, country, the zone file as found after"""
    res = {}
    keep = {}
    try:
        H.test_calls(o)
        for n, r in o.r.items():
            keep[n] = {k: cfg(r, k) for k in ("callsign", "qth", "country", "first_start_done")}
            ssh(r, "set -- save\n" + KEEP.format(f="/mnt/tz.conf"))
        with o.step("the callsign blank, first_start_done 0, each GUI restarted from the classic screen: the guide "
                    "opens by itself") as s:
            import orch_cable as oc
            for n, r in o.r.items():
                r.send("cfg callsign ")
                wait_cfg(r, "the callsign blank", lambda v: not v.strip(), "callsign", 6)
                bench.cfg_set(r, "first_start_done", 0)
                # The guide waits while a window is up (first_start.c start_cb): a GUI restarted on the Sentry (its
                # home, gui_state's place) brings the Sentry back first, and the guide never comes. From the classic
                # screen the place is the main screen, with no window over it
                o.changing += 1
                try:
                    oc.classic(r)
                    time.sleep(1.5)                     # gui_state's place written (every 0.5 s)
                finally:
                    o.changing -= 1
                gui_restart(o, n, "the first-start guide (no callsign)")
                r.wait("the guide", lambda: win(r) == "First start", 15)
            got = {n: " ".join(popup_text(o, r, box=WINDOW_BOX, label="first start step 1")[1].split())[:120] for n, r in o.r.items()}
            if not all(ocr_has(t, "FIRST START") and ocr_has(t, "Language") for t in got.values()):
                raise Refused(f"step 1 not read: {got}")
            res["open"] = got
            s.confirm(str(got))

        def nxt(r, label="Next"):
            press_label(r, rf"^{label}$", FS_NAMES, rest=1.2)

        lang_q = "sqlite3 -readonly /mnt/params.db \"select val from params where name='language'\" || true"
        with o.step("Language on both: the next language taken at once (params.db names it, the guide on step 1 "
                    "again, in it), then English again") as s:
            def lang(n, r):
                was = ssh(r, lang_q).strip()
                if was not in ("", "en"):
                    raise Refused(f"{n}: its language is {was!r}, not English: left alone")
                press_label(r, r"^Language$", FS_NAMES, rest=1.0)
                turn(r, "mfk", 1, FS_NAMES, rest=0.6)
                press(r, "MFK", FS_NAMES, rest=3.0)
                r.wait("the guide again", lambda: win(r) == "First start", 10)
                other = ssh(r, lang_q).strip()
                t = " ".join(popup_text(o, r, box=WINDOW_BOX, label="first start other language")[1].split())
                press_label(r, r"^Language$", FS_NAMES, rest=1.0)
                turn(r, "mfk", -1, FS_NAMES, rest=0.6)
                press(r, "MFK", FS_NAMES, rest=3.0)
                r.wait("the guide again", lambda: win(r) == "First start", 10)
                back = ssh(r, lang_q).strip()
                t2 = " ".join(popup_text(o, r, box=WINDOW_BOX, label="first start english again")[1].split())
                if other in ("", "en") or back not in ("", "en"):
                    raise Refused(f"{n}: language {was!r} -> {other!r} -> {back!r}")
                if not ocr_has(t2, "Language") or ocr_has(t, "The language of the screens"):
                    raise Refused(f"{n}: in {other!r} the screen read {t[:100]!r}; back {t2[:100]!r}")
                return {"other": other, "back": back}
            try:
                res["lang"] = each(o, lang, wait=90)
            finally:
                for n, r in o.r.items():             # never left in another language: written back, read at a restart
                    if ssh(r, lang_q).strip() not in ("", "en"):
                        o.st.ev(f"{n}: its language left {ssh(r, lang_q).strip()!r}: English written back", "bad")
                        ssh(r, "sqlite3 /mnt/params.db \"update params set val='en' where name='language'\"")
            s.confirm(str(res["lang"]))

        with o.step("Next, Skip (the keyboard step, nothing scanned), WiFi: its line, Networks and GEN back") as s:
            def wifi(n, r):
                nxt(r)                                   # 2: the keyboard
                t = popup_text(o, r, box=WINDOW_BOX, label="first start keyboard")[1]
                if not ocr_has(t, "Keyboard") and not ocr_has(t, "Bluetooth"):
                    raise Refused(f"{n}: step 2 reads {' '.join(t.split())[:100]!r}")
                nxt(r, "Skip")                           # 3: WiFi
                t = " ".join(popup_text(o, r, box=WINDOW_BOX, label="first start wifi")[1].split())
                ssid = ssh(r, "iw dev wlan0 link 2>/dev/null | sed -n 's/.*SSID: //p'").strip()
                if not ocr_has(t, "Networks") or (ssid and not ocr_has(t, ssid)):
                    raise Refused(f"{n}: the WiFi step reads {t[:140]!r} (network {ssid!r})")
                press_label(r, r"^Networks$", FS_NAMES, rest=1.5)
                r.wait("the WiFi window", lambda: win(r) == "WiFi", 6)
                bench.tap(r, "GEN")
                r.wait("the guide again", lambda: win(r) == "First start", 6)
                return {"ssid_read": bool(ssid)}
            res["wifi"] = each(o, wifi, wait=60)
            s.confirm(str(res["wifi"]))
        with o.step("Next: Callsign typed (the test call), Locator typed (the radio's own), read back") as s:
            def station(n, r):
                nxt(r)                                   # 4: callsign and locator
                call, qth = H.TEST_CALLS[n], keep[n]["qth"]
                press_label(r, r"^Callsign$", FS_NAMES, rest=0.8)
                r.send(f"type {call}")
                wait_cfg(r, "the callsign", lambda v: v.strip() == call, "callsign", 5)
                press_label(r, r"^Locator$", FS_NAMES, rest=0.8)
                r.send(f"type {qth}")
                wait_cfg(r, "the locator", lambda v: v.strip().upper() == qth.upper(), "qth", 5)
                return call
            res["station"] = each(o, station, wait=60)
            s.confirm(str(res["station"]))
        with o.step("Next: the Country step's band plan line; Next: the Clock step set by NTP") as s:
            def country_clock(n, r):
                nxt(r)                                   # 5: country
                t5 = " ".join(popup_text(o, r, box=WINDOW_BOX, label="first start country")[1].split())
                nxt(r)                                   # 6: clock
                t6 = " ".join(popup_text(o, r, box=WINDOW_BOX, label="first start clock")[1].split())
                if not ocr_has(t5, "Region") and not ocr_has(t5, "Band plan"):
                    raise Refused(f"{n}: the Country step reads {t5[:140]!r}")
                if not ocr_has(t6, "NTP"):
                    raise Refused(f"{n}: the Clock step reads {t6[:140]!r}")
                return {"country": t5[:60], "clock": t6[:60]}
            res["cc"] = each(o, country_clock, wait=60)
            s.confirm(str(res["cc"]))
        with o.step("Next: the summary (Skipped, the web address); Done: the guide closed, first_start_done 1") as s:
            def done(n, r):
                nxt(r)                                   # the summary
                p, t = popup_text(o, r, box=WINDOW_BOX, label="first start summary")
                # the web configurator's row alone (two lines: the whole window's read skipped it; its zeros read 8)
                web = ocr(p, box=(210, 290, 790, 345), psm=6, stretch=True)
                t = " ".join((t + " " + web).split())
                if not ocr_has(t, "Skipped") or not re.search(r"x6\w{3}\s*\.\s*local", web, re.I):
                    raise Refused(f"{n}: the summary reads {t[:160]!r}")
                nxt(r, "Done")
                r.wait("the guide closed", lambda: win(r) != "First start", 6)
                wait_cfg(r, "first_start_done 1", lambda v: str(v) == "1", "first_start_done", 4)
                return t[:80]
            res["done"] = each(o, done, wait=60)
            s.confirm(str(res["done"]))
        with o.step("each GUI restarted again (a callsign now): the guide does not open") as s:
            for n, r in o.r.items():
                gui_restart(o, n, "the first-start guide not again")
                t0 = time.time()
                while time.time() - t0 < 8:
                    if win(r) == "First start":
                        raise Refused(f"{n}: the guide opened again with a callsign set")
                    time.sleep(0.3)
            s.confirm("not opened on either")
        return True, "the first-start guide on both: opened by itself, walked (WiFi and back, callsign and locator "  \
                     "typed, NTP), Skipped and the web address in the summary, Done, not again", res
    except Refused as e:
        return False, str(e), res
    finally:
        for n, r in o.r.items():
            if n not in keep:
                continue
            try:
                if win(r) == "First start":
                    r.send("close")
                for k in ("callsign", "qth", "country"):
                    if cfg(r, k).strip() != str(keep[n][k]).strip():
                        if k == "callsign" and not str(keep[n][k]).strip():
                            r.send("cfg callsign ")
                        else:
                            bench.cfg_set(r, k, keep[n][k])
                bench.cfg_set(r, "first_start_done", 1)
                ssh(r, "set -- back\n" + KEEP.format(f="/mnt/tz.conf"))
            except (Refused, OSError, subprocess.SubprocessError) as e:
                o.st.ev(f"{n}: the first-start settings not all put back: {e}", "bad")
        home_both(o)


# ---- Zello and SVXConnect: the PTT key is the app's while it is open (the cable, 0.1 W) ----

def _ptt_captured(o, n, action, name):
    """n on the classic screen, the app open: its PTT (the remote's) keys nothing (the app has it); the app closed: the
    same PTT keys the radio at 0.1 W, heard by the other radio (the session's Watch). The other radio on the Sentry's
    A on the same dial"""
    import orch_cable as oc
    r = o.r[n]
    out = {}
    o.changing += 1
    try:
        oc.classic(r)
        r.send(f"open {action}")
        r.wait(name, lambda: win(r) == name, 10)
        bench.settled(r, still=1.0, timeout=5.0)
        other = next(x for m, x in o.r.items() if m != n)
        bench.ensure(r, {"freq": 14200000, "mode": 2})
        bench.ensure(other, {"freq": 14200000, "mode": 2})
        r.send(f"set pwr {int(round(H.W * 10))}")
    finally:
        o.changing -= 1
    with oc.session(o, f"{name} PTT {n}", [n], 14200000) as watch:
        oc.role(o, n)
        t0 = time.time()
        hold = o.hold()
        r.send(f"key PTT 1 {hold}")
        time.sleep(1.0)
        r.send(f"key PTT 1 {hold}")
        time.sleep(1.0)
        r.send("key PTT 0")
        time.sleep(0.8)
        out["in_app"] = len(oc.tx_spans(r, t0))
        if out["in_app"]:
            raise Refused(f"{n}: the PTT keyed the radio with {name} open")
        o.changing += 1
        try:
            r.send("close")
            r.wait(f"{name} closed", lambda: win(r) != name, 6)
            bench.ensure(r, {"freq": 14200000, "mode": 2})
        finally:
            o.changing -= 1
        t1 = time.time()
        hold = o.hold()
        r.send(f"key PTT 1 {hold}")
        r.wait("on the air", lambda: r.tx or watch.why, 4)
        time.sleep(1.0)
        r.send(f"key PTT 1 {hold}")
        time.sleep(0.8)
        r.send("key PTT 0")
        r.wait("off the air", lambda: not r.tx, 4)
        out["closed"] = len(oc.tx_spans(r, t1))
        w = [x[1] for x in oc.t_frames(r, t1)]
        out["w"] = max(w) if w else None
    if not out["closed"]:
        raise Refused(f"{n}: {name} closed, the PTT did not key the radio")
    return out


def t_zello_ptt(o):
    """Zello open on each radio in turn (classic screen, 14.200 USB): its PTT keys nothing; Zello closed: the PTT keys
    the radio at 0.1 W into the cable"""
    res = {}
    try:
        H.test_calls(o)
        align(o, 14200000, 2)
        for n in (H.A, H.B):
            with o.step(f"{n}: Zello open, PTT: nothing keyed; Zello closed, PTT: on the air at 0.1 W") as s:
                res[n] = _ptt_captured(o, n, 202, "Zello")
                s.confirm(str(res[n]))
            home_both(o)
        return True, f"Zello holds the PTT while open on both, the radio keys again once closed ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


def t_svx_ptt(o):
    """SVXConnect open on each radio in turn: its PTT keys nothing; closed: the PTT keys the radio at 0.1 W"""
    res = {}
    try:
        H.test_calls(o)
        align(o, 14200000, 2)
        for n in (H.A, H.B):
            with o.step(f"{n}: SVXConnect open, PTT: nothing keyed; closed, PTT: on the air at 0.1 W") as s:
                res[n] = _ptt_captured(o, n, 206, "SVXConnect")
                s.confirm(str(res[n]))
            home_both(o)
        return True, f"SVXConnect holds the PTT while open on both ({res})", res
    except Refused as e:
        return False, str(e), res
    finally:
        home_both(o)


# ---- FT8 over the cable: Show, the POTA tag, the rows' kinds, the Live Map ----

CELL = {"msg": 2, "cq": 3, "to_me": 4, "tx": 5}     # src/ft8/decode_list.h ft8_cell_type_t


def _rows_with(L, n0, text, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        got = [r for r in L.rows[n0:] if text in bench.msg(r)]
        if got:
            return got
        time.sleep(0.3)
    return []


def _map_heard(o, r, label):
    """The Live Map's head line on r's screen: (its 'N heard', None unread; the text read). Its box alone, stretched,
    at twice the size: the whole screen at three times read the slashed zero of '0 heard' as 4 (11-26)"""
    p, txt = shot_text(o, r, label)
    head = ocr(p, box=(18, 134, 520, 158), psm=7, stretch=True, scale=2)
    m = re.search(r"([0-9@Oo]+)\s*heard", head)
    n = int(m.group(1).translate(str.maketrans("@Oo", "000"))) if m else None
    return n, " ".join((head + " " + txt).split())


def t_ft8_show_pota(o, s, l):
    """FT8 over the cable at 0.1 W, s sending messages through its TCI, l's list (the remote's ft8r rows: kind t, POTA
    p): Show: CQ drops a message to a third call; Show: All lists it (t RX_MSG); 'CQ POTA Q9TST JO22' is a CQ with
    the POTA tag, a plain CQ none; a message to l is its own (t TO_ME, red); l's Live Map then counts the stations
    heard with a grid ('N heard', DX Q9TST) and a band change empties it ('0 heard'). Show as found after"""
    import orch_cable as oc
    S, L = o.r[s], o.r[l]
    res = {}
    third = "Q8TST Q9TST JO22"
    try:
        with cfg_kept(L, {"ft8_show_all": 0}):
            with oc.ft8_pair(o, f"ft8-show {s}>{l}", senders=[s]) as watch:
                oc.role(o, s)
                with o.step(f"Show: CQ on {l}: '{third}' (to a third call) not listed") as st:
                    n0 = len(L.rows)
                    oc.ft8_tci_msg(o, S, third, watch)
                    time.sleep(4.0)
                    got = _rows_with(L, n0, "Q8TST Q9TST", 2)
                    if got:
                        raise Refused(f"Show: CQ listed {[bench.msg(r) for r in got]}")
                    st.confirm("not listed")
                with o.step(f"Show: All on {l}: the same message listed, an ordinary row") as st:
                    bench.cfg_set(L, "ft8_show_all", 1)
                    n0 = len(L.rows)
                    oc.ft8_tci_msg(o, S, third, watch)
                    got = _rows_with(L, n0, "Q8TST Q9TST", 12)
                    if not got or got[-1].get("t") != CELL["msg"]:
                        raise Refused(f"Show: All: rows {[(bench.msg(r), r.get('t')) for r in got]}")
                    res["all"] = got[-1].get("t")
                    st.confirm(f"listed, t {res['all']}")
                with o.step(f"'CQ POTA Q9TST JO22': a CQ with the POTA tag; 'CQ Q9TST JO22': a CQ without") as st:
                    out = {}
                    for text, want in (("CQ POTA Q9TST JO22", 1), ("CQ Q9TST JO22", 0)):
                        n0 = len(L.rows)
                        oc.ft8_tci_msg(o, S, text, watch)
                        got = [r for r in _rows_with(L, n0, "Q9TST", 12) if bench.msg(r).startswith("CQ")]
                        if not got:
                            raise Refused(f"{l} did not decode {text!r}")
                        out[text] = (got[-1].get("t"), got[-1].get("p"))
                        if got[-1].get("t") != CELL["cq"] or int(got[-1].get("p") or 0) != want:
                            raise Refused(f"{text!r}: t {got[-1].get('t')}, POTA {got[-1].get('p')} ({want} wanted)")
                    res["pota"] = out
                    st.confirm(str(out))
                with o.step(f"a message to {l}: its own row (t TO_ME)") as st:
                    to_me = f"{H.TEST_CALLS[l]} Q9TST JO22"
                    n0 = len(L.rows)
                    oc.ft8_tci_msg(o, S, to_me, watch)
                    got = _rows_with(L, n0, to_me, 12)
                    if not got or got[-1].get("t") != CELL["to_me"] or not got[-1].get("me"):
                        raise Refused(f"the message to {l}: rows {[(bench.msg(r), r.get('t'), r.get('me')) for r in got]}")
                    res["to_me"] = got[-1].get("t")
                    st.confirm(f"t {res['to_me']}")
                with o.step(f"{l}'s Live Map: the stations heard with a grid; a band change empties it") as st:
                    k = _to_label(L, r"^Live\s*Map", ("FT8",))
                    press(L, k, ("FT8",), rest=2.0)
                    n1, t1 = _map_heard(o, L, "ft8 live map")
                    press(L, k, ("FT8",), rest=1.0)
                    if n1 is None or n1 < 1 or not _call_read(t1, "Q9TST"):
                        raise Refused(f"the Live Map reads {t1[:140]!r}")
                    res["live"] = f"{n1} heard"
                    def band(key):
                        o.changing += 1
                        try:
                            for r in o.r.values():
                                f0 = int(r.state.get("freq", 0))
                                bench.tap(r, key)
                                r.wait("the band moved", lambda r=r, f0=f0: int(r.state.get("freq", 0)) != f0, 5)
                            time.sleep(0.8)
                        finally:
                            o.changing -= 1
                        H.alike(o, ("freq", "mode"))
                    # read on the band it moved to (as the feature has it: BAND_UP, the map again: empty); back on 20 m
                    # after, where a decode due may come again (11-26: '4 heard' read back on 20 m)
                    band("BAND_UP")
                    try:
                        _to_label(L, r"^Live\s*Map", ("FT8",))
                        press(L, k, ("FT8",), rest=2.0)
                        n2, t2 = _map_heard(o, L, "ft8 live map emptied")
                        press(L, k, ("FT8",), rest=1.0)
                    finally:
                        band("BAND_DOWN")
                    if n2 != 0:
                        raise Refused(f"after a band change the Live Map reads {t2[:140]!r}")
                    res["emptied"] = f"{n2} heard"
                    st.confirm(f"{res['live']}, then {res['emptied']}")
        return True, (f"{l}: Show CQ drops a third call's message, All lists it; CQ POTA tagged, a plain CQ not; its own "
                      f"row; Live Map {res['live']} then {res['emptied']}"), res
    except Refused as e:
        return False, str(e), res


# ---- PSK31 over the cable: the listener's cursor off the signal, the main knob ----

PSK_OFF_HZ = 40


def _psk_center(r):
    """r's PSK cursor (its audio frequency, Hz) as its status button says it ('1000 Hz Q 0%', page 1 F5)"""
    for t in fkeys(r)[0]:
        m = re.match(r"^(?:TX\s+)?(\d+)\s*Hz", flat(t))
        if m:
            return int(m.group(1))
    raise Refused(f"{r.name}: no PSK status button ({[flat(t) for t in fkeys(r)[0]]})")


def _psk_to(r, hz, names=("PSK31", "PSK63")):
    """r's PSK cursor brought to hz: the main knob a click at a time (5 Hz; a turn of more clicks in one goes 3 or 6
    times as far each: 8 clicks moved it 240 Hz, 11-26), the rest by the MFK's 1 Hz; the status read back each time.
    The cursor is the app's for the GUI's run (psk_tx.c, 1000 at its start), so tests left it anywhere"""
    for _ in range(80):
        d = hz - _psk_center(r)
        if d == 0:
            return
        if abs(d) >= 15:
            turn(r, "main", 3 if d > 0 else -3, names, rest=0.6)        # 15 Hz: three clicks, no faster
        elif abs(d) >= 5:
            turn(r, "main", 1 if d > 0 else -1, names, rest=0.6)
        else:
            turn(r, "mfk", d, names, rest=0.6)
    raise Refused(f"{r.name}: its PSK cursor at {_psk_center(r)} Hz, not brought to {hz}")


def t_psk_cursor(o, s, l):
    """PSK31 at 0.1 W: s's macro 'CQ CQ DE <call> ...' (added on its web page, taken out after) on its cursor (1000 Hz,
    brought there first); l's cursor 30 Hz off it before the over, then one click of the main knob (5 Hz) while it
    sends: PSK snaps onto the signal when the knob stops (within 45 Hz), and l's text log has the call at the
    sender's audio frequency (within 15 Hz)"""
    import orch_cable as oc
    import suite_air_b as ab
    S, L = o.r[s], o.r[l]
    call = H.TEST_CALLS[s]
    # once (about 8 s at PSK31): three times ran past the Watch's 16 s for one over (11-26)
    text = f"CQ CQ DE {call} {call} K"
    res = {}
    ip = ab.web_ip(S.host)
    mid = None
    tl0 = ab.tl_sizes(L.host)
    names = ("PSK31", "PSK63")
    try:
        st, b_ = ab.http(ip, "/api/macros/psk", method="POST", body={"name": "BENCH", "text": text})
        if st != 200:
            raise Refused(f"{s}: the PSK macro not added ({st})")
        listed = json.loads(b_)["macros"]
        i_m = max(i for i, m in enumerate(listed) if m.get("text") == text)
        mid = listed[i_m].get("id")
        with o.step("both PSK31 apps open on one dial; s's cursor on 1000 Hz, l's 30 Hz above") as stp:
            o.changing += 1
            try:
                o.both_page("G")
                for r in (L, S):
                    ab.open_listed(r, "psk31", "psk")
                oc._same_dial(o, S, L, "PSK31")
                bench.pair_align(S, L)
            finally:
                o.changing -= 1
            _psk_to(S, oc.PSK_TX_HZ)
            _psk_to(L, oc.PSK_TX_HZ + 30)
            time.sleep(1.0)                                     # the snap after the knob (nothing to snap onto)
            res["cursors"] = (_psk_center(S), _psk_center(L))
            if res["cursors"] != (oc.PSK_TX_HZ, oc.PSK_TX_HZ + 30):
                raise Refused(f"the cursors at {res['cursors']}")
            stp.confirm(f"dial {L.state.get('freq')}, cursors {res['cursors']}")
        with oc.session(o, "psk-cursor", [s]) as watch:
            oc.role(o, s)
            ab.Pages(S).macro(i_m)
            S.wait("its PSK on the air", lambda: S.tx or watch.why, 8)
            time.sleep(1.0)
            turn(L, "main", 1, names, rest=0.5)
            S.wait("its end", lambda: not S.tx or watch.why, 40)
        time.sleep(2)
        lines = H.textlog_lines(L.host, tl0)
        res["textlog"] = str(lines)[-160:]
        dial = int(L.state.get("freq") or 0)
        m = next((re.search(r"\s(\d+\.\d{6})\s+PSK\w*\s+RX\s+Q(\d+)%", x) for x in lines if call in x), None)
        if not m:
            return False, f"{l} decoded no {call} after the knob ({res['textlog']!r})", res
        res.update(audio_hz=round(float(m.group(1)) * 1e6) - dial, quality=int(m.group(2)))
        if abs(res["audio_hz"] - oc.PSK_TX_HZ) > 15:
            return False, f"{l} decoded at {res['audio_hz']} Hz, not snapped onto {oc.PSK_TX_HZ}", res
        return True, f"{l}'s cursor 40 Hz off, a knob click: snapped to {res['audio_hz']} Hz, {call} decoded " \
                     f"(Q {res['quality']}%)", res
    except Refused as e:
        return False, str(e), res
    finally:
        H.textlog_back(L.host, tl0)
        if mid is not None:
            ab.http(ip, f"/api/macros/psk/{mid}", method="DELETE")
        for r in (S, L):                    # the cursors where the GUI starts them: psk-look sends from 1000 Hz
            try:
                if win(r) in names:
                    _psk_to(r, oc.PSK_TX_HZ)
            except Refused as e:
                o.st.ev(f"{r.name}: its PSK cursor not put back on {oc.PSK_TX_HZ} Hz: {e}", "bad")
        home_both(o)


# ---- SSTV over the cable: REPLY MODE, Reply and Replay to a typed target ----

SSTV_R8, SSTV_R36 = 1, 2                    # src/sstv/sstv_mode.h


def _sstv_menu_row(S, row, watch, picks=6):
    """S's TX menu (page 1 F5) at row, then the MFK through its steps until S is on the air: the time it keyed"""
    import orch_cable as oc
    return oc._sstv_tx_menu(S, row, watch, picks)


def _sstv_set_target(S, call):
    """SET TARGET CALLSIGN (menu row 5): the text window given call; the TX menu it comes back to closed (F5, then
    'Cancel TX', back to 'TX'): left open, the next menu's F5 cancelled it and its MFK presses went nowhere (11-26)"""
    press(S, "F5", ("SSTV",), rest=1.0)
    S.send("knob mfk -10")
    time.sleep(0.5)
    S.send("knob mfk 5")
    time.sleep(0.5)
    S.send("key MFK 1")
    time.sleep(0.15)
    S.send("key MFK 0")
    time.sleep(1.0)
    S.send(f"type {call}")
    time.sleep(1.0)
    if _label(S, "F5") != "TX":
        press(S, "F5", ("SSTV",), rest=1.0)
    if _label(S, "F5") != "TX":
        raise Refused(f"{S.name}: SSTV's F5 reads {_label(S, 'F5')!r} after the target, not TX")


def _sstv_wait_pics(L, n0, secs=25):
    """The pictures L saved since n0 (a set of its /mnt/sstv/*.png): polled from the over's end until one is there
    (and its _dn copy a moment later), at most secs. The X6200 writes its picture about 6 s after the X6100 does
    (decoded, then denoised): a fixed 4 s found none there (sstv-reply x6200>x6100, 11-26)"""
    import orch_cable as oc
    end = time.time() + secs
    while time.time() < end:
        got = oc._sstv_pics(L) - n0
        if any(not x.endswith("_dn.png") for x in got):
            time.sleep(3.0)
            return oc._sstv_pics(L) - n0
        time.sleep(1.0)
    return oc._sstv_pics(L) - n0


def _sstv_auto_rx(r):
    """r's SSTV on Auto RX (page 1: 'Start Auto RX' pressed, read back 'Stop Auto RX'); left on when it was. A receiver
    that never started saved nothing (sstv-reply, sstv-restart, 11-26); TX ends with it on again (app_sstv_tx.c)"""
    if fkey_of(r, r"^Stop Auto RX"):
        return
    k = press_label(r, r"^Start Auto RX", ("SSTV",), rest=0.8)
    if not re.match(r"^Stop Auto RX", _label(r, k)):
        raise Refused(f"{r.name}: Auto RX not started ({_label(r, k)!r})")


def t_sstv_reply(o, s, l):
    """SSTV over the cable at 0.1 W: s's CQ picture in Robot 8 received by l; l's TX Mode Robot 36, its target s's call
    typed (SET TARGET CALLSIGN): REPLY TO with REPLY MODE AS RECEIVED goes out in Robot 8 (the over ends by itself in
    under 11 s) and s saves it; REPLAY TO (the miniature of what l received) likewise; REPLY MODE TX MODE goes out in
    Robot 36 (still on the air at 11 s: halted, under the Watch's 16 s). Settings as found, the pictures taken off"""
    import orch_cable as oc
    import suite_more
    S, L = o.r[s], o.r[l]
    res = {}
    before = {n: oc._sstv_pics(r) for n, r in o.r.items()}
    try:
        with cfg_kept(S, {"sstv_tx_mode": SSTV_R8}), \
                cfg_kept(L, {"sstv_tx_mode": SSTV_R36, "sstv_reply_rx": 1}):
            with o.step("both SSTV apps open, one dial") as st:
                o.changing += 1
                try:
                    for r in (L, S):
                        if not suite_more.open_app(r, "sstv"):
                            raise Refused(f"{r.name}'s SSTV app did not open")
                        time.sleep(1.0)
                    oc._same_dial(o, S, L, "SSTV")
                finally:
                    o.changing -= 1
                for r in (L, S):                    # each receives in turn: the CQ, then the replies
                    _sstv_auto_rx(r)
                st.confirm(str(S.state.get("freq")))
            with oc.session(o, f"sstv-reply {s}>{l}", [s, l]) as watch:
                with o.step(f"{s}'s CQ in Robot 8: {l} saves a picture") as st:
                    oc.role(o, s)
                    n0 = oc._sstv_pics(L)
                    _sstv_menu_row(S, 0, watch)
                    S.wait("its end", lambda: not S.tx or watch.why, 20)
                    got = _sstv_wait_pics(L, n0)
                    if not got:
                        raise Refused(f"{l} saved no picture of {s}'s CQ")
                    st.confirm(str(sorted(os.path.basename(x) for x in got)))
                _sstv_set_target(L, H.TEST_CALLS[s])
                for row, what in ((1, "REPLY TO"), (2, "REPLAY TO")):
                    with o.step(f"{l}'s {what} {H.TEST_CALLS[s]}, AS RECEIVED: out in Robot 8 (under 11 s), "
                                f"{s} saves it") as st:
                        oc.role(o, l)
                        n0 = oc._sstv_pics(S)
                        t0 = _sstv_menu_row(L, row, watch)
                        L.wait("its end", lambda: not L.tx or watch.why, 20)
                        secs = round(time.time() - t0, 1)
                        got = _sstv_wait_pics(S, n0)
                        if secs > 11 or not got:
                            raise Refused(f"{what}: on the air {secs} s, {s} saved {len(got)} picture(s)")
                        res[what] = {"secs": secs, "saved": sorted(os.path.basename(x) for x in got)}
                        st.confirm(str(res[what]))
                        time.sleep(5.0)                 # s done with it (denoised), listening for the next
                with o.step(f"{l}'s REPLY MODE TX MODE, REPLY TO: Robot 36 (still on at 11 s), halted") as st:
                    bench.cfg_set(L, "sstv_reply_rx", 0)
                    t0 = _sstv_menu_row(L, 1, watch)
                    time.sleep(max(0.0, 11.0 - (time.time() - t0)))
                    on = bool(L.tx)
                    press_label(L, r"^Halt", ("SSTV",), rest=0.2)
                    L.wait("off the air", lambda: not L.tx, 3)
                    res["tx_mode"] = {"on_at_11s": on}
                    if not on:
                        raise Refused(f"TX MODE (Robot 36): {l} off the air before 11 s")
                    st.confirm(str(res["tx_mode"]))
        return True, f"SSTV reply mode: as received {res['REPLY TO']['secs']} s (Robot 8), replay " \
                     f"{res['REPLAY TO']['secs']} s, TX MODE still on at 11 s (Robot 36)", res
    except Refused as e:
        return False, str(e), res
    finally:
        for n, r in o.r.items():
            new = oc._sstv_pics(r) - before[n]
            if new:
                ssh(r, "rm -f " + " ".join(new) + " " + " ".join(x.replace(".png", "_dn.png") for x in new))
        home_both(o)


# ---- SSTV over the cable: Auto RX Restart ----

def _sstv_restart_round(o, S, L, watch, label):
    """S's last complete picture again (REPEAT LAST TX: one press, on the air in about 2 s), halted 2 s in, and again
    at once: the second header comes inside the first's Robot 8 (8 s). CALL CQ's steps (picture, colours, text) took
    13 s between the two starts, and the first picture was over before the second came (11-26). (When each keyed,
    the pictures L saved, each with the time it was written)"""
    import orch_cable as oc
    n0 = oc._sstv_pics(L)
    t1 = _sstv_menu_row(S, 4, watch)
    time.sleep(max(0.0, 2.0 - (time.time() - t1)))
    press_label(S, r"^Halt", ("SSTV",), rest=0.2)
    S.wait("off the air", lambda: not S.tx, 3)
    t2 = _sstv_menu_row(S, 4, watch)
    S.wait("its end", lambda: not S.tx or watch.why, 20)
    new = sorted(_sstv_wait_pics(L, n0))
    new = [x for x in new if not x.endswith("_dn.png")]
    when = {x: float(ssh(L, f"python3 -c 'import os;print(os.path.getmtime(\"{x}\"))'")) for x in new}
    time.sleep(8.0)         # L listening again before the next round: a header while it decodes is never seen
    return t1, t2, when


def t_sstv_restart(o, s, l):
    """SSTV over the cable at 0.1 W, l on Auto RX: s's CQ (Robot 8) sent whole, then again (REPEAT LAST TX) halted 2 s
    in and sent again at once. With
    Auto RX Restart ON (page 2) l restarts its recording at the second header: the picture it saves is the second
    send's (written about a picture's length after the second start); OFF: the first's (written a picture's length
    after the first start, the second header ignored). Restart as found after, the pictures taken off"""
    import orch_cable as oc
    import suite_more
    S, L = o.r[s], o.r[l]
    res = {}
    before = {n: oc._sstv_pics(r) for n, r in o.r.items()}
    names = ("SSTV",)
    try:
        with cfg_kept(S, {"sstv_tx_mode": SSTV_R8}):
            with o.step("both SSTV apps open, one dial; l's Auto RX Restart read") as st:
                o.changing += 1
                try:
                    for r in (L, S):
                        if not suite_more.open_app(r, "sstv"):
                            raise Refused(f"{r.name}'s SSTV app did not open")
                        time.sleep(1.0)
                    oc._same_dial(o, S, L, "SSTV")
                finally:
                    o.changing -= 1
                _sstv_auto_rx(L)
                k = _to_label(L, r"^Auto RX Restart", names)
                was = _label(L, k)
                st.confirm(was)
            with oc.session(o, f"sstv-restart {s}>{l}", [s]) as watch:
                oc.role(o, s)
                with o.step(f"{s}'s CQ in Robot 8 whole: the picture REPEAT LAST TX sends again") as st:
                    _to_label(S, r"^TX$", names)
                    n0 = oc._sstv_pics(L)
                    _sstv_menu_row(S, 0, watch)
                    S.wait("its end", lambda: not S.tx or watch.why, 20)
                    _sstv_wait_pics(L, n0)
                    time.sleep(8.0)                 # L done with that picture (decoded, denoised), listening again
                    st.confirm("sent")
                for want in ("ON", "OFF"):
                    with o.step(f"Restart {want} on {l}: the CQ halted and sent again") as st:
                        k = _to_label(L, r"^Auto RX Restart", names)
                        if not _label(L, k).endswith(want):
                            press(L, k, names, rest=0.8)
                        if not _label(L, k).endswith(want):
                            raise Refused(f"{l}: Restart reads {_label(L, k)!r}")
                        _to_label(S, r"^TX$", names)            # page 1: its TX menu on F5
                        t1, t2, when = _sstv_restart_round(o, S, L, watch, want)
                        if not when:
                            raise Refused(f"Restart {want}: {l} saved no picture")
                        last = max(when.values())
                        res[want] = {"gap_s": round(t2 - t1, 1), "after_first_s": round(last - t1, 1),
                                     "after_second_s": round(last - t2, 1), "pictures": len(when)}
                        st.confirm(str(res[want]))
                    time.sleep(2)
                # ON: written a picture's length after the SECOND start; OFF: after the FIRST (before the second's)
                on, off = res["ON"], res["OFF"]
                if not on["after_second_s"] > 5 or not off["after_first_s"] < on["after_first_s"] - 2:
                    raise Refused(f"ON {on}, OFF {off}: the ON picture not the second send's")
            k = _to_label(L, r"^Auto RX Restart", names)
            if _label(L, k) != was:
                press(L, k, names, rest=0.8)
        return True, f"Auto RX Restart on {l}: ON saved the second send's picture ({res['ON']}), OFF the first's " \
                     f"({res['OFF']})", res
    except Refused as e:
        return False, str(e), res
    finally:
        for n, r in o.r.items():
            new = oc._sstv_pics(r) - before[n]
            if new:
                ssh(r, "rm -f " + " ".join(new) + " " + " ".join(x.replace(".png", "_dn.png") for x in new))
        home_both(o)


# ---- what each test covers, its rows ----

COVERS = {
    "bands-app": (["bands-app-open", "bands-use-band"], []),
    "timesig-f2": (["timesig-f2-set-clock"], []),
    "channels-api": (["channels-web-concurrency", "channels-web-partial-save"], []),
    "wspr-web": (["wspr-hop-web-settings"], []),
    "cb-refused": (["cb-tx-refused"], []),
    "hfair": (["hfair-list-eibi", "hfair-tx-refused"], []),
    "zello-offline": (["zello-talk-via", "simplex-link-one-at-a-time"], ["zello-link-needs-squelch"]),
    "svx-talk": ([], ["svx-ptt-captured"]),
    "walkie-mode": (["app-walkie-mode", "app-walkie-vol-knob"], []),
    "walkie-buttons": (["app-walkie-buttons"], []),
    "walkie-channels": (["app-walkie-channels"], []),
    "walkie-scope": (["app-walkie-bandscope"], []),
    "ft8-time-sync": (["ft8-time-sync"], []),
    "ft8-log-map": (["ft8-log-map"], []),
    "ft8-custom-freq": (["ft8-custom-freq"], []),
    "psk-squelch": (["psk-squelch"], []),
    "rade-app": (["rade-freq-list", "rade-power-setting"], []),
    "wefax-freq": (["wefax-station-list", "channels-wefax-prefix"], []),
    "wefax-view": (["wefax-view"], []),
    "wefax-browser": (["wefax-fax-browser"], []),
    "wefax-power": (["wefax-power"], []),
    "sstv-browser": (["sstv-pics-browser-delete"], []),
    "sstv-tx-files": (["sstv-tx-files"], []),
    "js8-freq": (["js8-freq", "ghostnet-mode"], []),
    "ghostnet-popup": (["ghostnet-net-list", "ghostnet-rtty-window"], []),
    "ghostnet-reminder": ([], ["ghostnet-reminder"]),
    "js8-open-off": (["js8-auto-off-at-open"], []),
    "js8-nocall": (["js8-needs-callsign"], []),
    "js8-texts": (["js8-settings-texts"], []),
    "js8-map": (["js8-map"], []),
    "field-home": (["field-home-tile-row"], []),
    "field-sota": (["field-reference-nearest", "field-position-source", "field-start-activation", "field-log-qso",
                    "field-qso-not-saved", "field-logged-mode", "field-end-activation", "field-logbook"],
                   ["field-activation-counters"]),
    "field-beacon-rules": (["field-test-beacon-no-second-press", "field-use-band"], ["field-test-beacon-refusals"]),
    "field-programmes": (["field-tota-wca-mota", "field-lighthouses", "field-llota", "field-activation-counters"], []),
    "field-tiles": (["field-tiles-programme"], []),
    "field-xota-nota": (["field-xota"], ["field-nota"]),
    "field-map": (["field-map-tiles", "field-map-view-zoom"], []),
    "field-spotting": ([], ["field-wwff", "field-wwbota-hota", "field-activation-counters"]),
    "field-persist": (["field-state-persists", "field-start-here"], []),
    "channels-app-holds": (["channels-refused-app-holds"], ["channels-hand-mic-keys"]),
    "birdie-marks": (["birdie-marks"], []),
    "cwkbd-refusals": (["cwkbd-not-cw-refusals"], []),
    "prop-conditions": (["prop-conditions-display"], []),
    "flights": (["flights-range"], ["flights-position-source"]),
    "weather": ([], ["weather-location-source"]),
    "cwtrainer-learn": (["cwtrainer-speed-5-50", "cwtrainer-unreadable"], ["cwtrainer-learn-koch",
                                                                        "cwtrainer-answer-timing"]),
    "cwtrainer-calls": (["cwtrainer-calls", "cwtrainer-tx-speed"], []),
    "cwtrainer-out-of-band": ([], ["cwtrainer-out-of-band"]),     # not: outside the bands (bench gate), the airband
    "first-start": (["first-start-open", "first-start-wifi", "first-start-language"],
                    ["first-start-callsign-locator", "first-start-country-tz", "first-start-clock-tz",
                     "first-start-done-skip"]),
    "zello-ptt": ([], ["zello-ptt-captured"]),          # not: a PTT held as the app opens stays the radio's
    "svx-ptt": ([], ["svx-ptt-captured"]),              # not: the pause while the radio sends (a connection)
    "ft8-show": (["ft8-show-filter", "ft8-pota-tag", "ft8-live-map"], ["ft8-row-colours"]),
    "psk-cursor": (["psk-cursor-knob"], []),
    "sstv-reply": (["sstv-reply-mode"], ["sstv-tx-reply-replay"]),     # the miniature not judged
    "sstv-restart": (["sstv-auto-rx-restart"], []),
}

ROWS = [
    # (name, fn, est s, "what")
    ("bands-app", t_bands, 60, "the Bands app opened and closed on both; the MFK and its press tune 40 m's phone "
     "frequency in LSB, from CW its CW frequency in CW, from CW-R 20 m's in CW-R"),
    ("timesig-f2", t_timesig, 40, "Time Signal's F2 Set clock: Auto, Off, Auto again, as the button reads"),
    ("channels-api", t_channels_web, 60, "the Memories API: a stale Delete refused, Delete and Undo at its place, a "
     "Save of the frequency alone keeps a 40-byte name and an extra key (the files put back)"),
    ("wspr-web", t_wspr_web, 20, "WSPR's Hop and band uses set through the web page, read back there and by cfg?"),
    ("cb-refused", t_cb_refused, 60, "CB's PTT refused on both (the remote's and a program's TCI audio), each "
     "armed in turn; the app sets no power"),
    ("hfair", t_hfair, 70, "HF Air Band: VOLMET first, the MFK tunes a row in USB, PTT refused there"),
    ("zello-offline", t_zello, 90, "Zello with no account: Talk via and No BT mic, SQL 0 refuses the links, one link "
     "at a time with SVXConnect both ways"),
    ("svx-talk", t_svx_talk, 40, "SVXConnect not set up: Talk via toggles svx_talk_bt"),
    ("walkie-mode", t_walkie_mode, 60, "Walkie Talkie mode: no app inside, VOL the volume, WLK pages, APP held leaves"),
    ("walkie-buttons", t_walkie_buttons, 120, "the WLK buttons on both, each read back and undone (never ATU)"),
    ("walkie-channels", t_walkie_channels, 80, "the walkie channels: Store, the knob in MEM, CH 05 named, Clear held"),
    ("walkie-scope", t_walkie_scope, 60, "the walkie bandscope runs with the main spectrum off; the zoom's span"),
    ("ft8-time-sync", t_ft8_time_sync, 120, "FT8's Time Sync: left alone under NTP; 3 s ahead (NTP stopped) moved to "
     "the slot, NTP started again"),
    ("ft8-log-map", t_ft8_log_map, 70, "FT8's Log Map opens and closes, judged on its caption (OCR)"),
    ("ft8-custom-freq", t_ft8_custom_freq, 90, "FT8's typed dial 14091 (USB-D, said, on the button), band keys back "
     "to the presets, a preset chosen"),
    ("psk-squelch", t_psk_squelch, 120, "PSK31's squelch: the knob, psk.conf, off and on; no RX text on with it, "
     "noise's text off"),
    ("rade-app", t_rade, 90, "RADE's Freq. list (14.236 USB, 40 m in LSB) and Power steps kept over a reopen"),
    ("wefax-freq", t_wefax_freq, 70, "WeFax's Freq.: DDK3 tuned 1.5 kHz under; a WEFAX channel listed by its name"),
    ("wefax-view", t_wefax_view, 60, "WeFax's View: Big, Full Size, Small, Big, each a different screen"),
    ("wefax-browser", t_wefax_browser, 60, "WeFax's browser lists and opens a test fax (OCR)"),
    ("wefax-power", t_wefax_power, 40, "WeFax's Power steps 0.1/0.2/0.5/1 W, the button and the state alike"),
    ("sstv-browser", t_sstv_browser, 70, "SSTV's RX Pics Browser and Delete picture; the Gallery lists and deletes"),
    ("sstv-tx-files", t_sstv_tx_files, 30, "SSTV's TX folders; a Gallery upload lands in tx_pics"),
    ("js8-freq", t_js8_freq, 150, "JS8's Freq: GhostNet mode (7.107, its groups, BAND_UP 14.107), Custom 14080, "
     "JS8Call's again; one radio at a time in a TX session"),
    ("ghostnet-popup", t_ghostnet_popup, 150, "GhostNet's popup: a net's reminder set and cleared, its RTTY entry "
     "(7.077 USB, the stored RTTY settings untouched)"),
    ("ghostnet-reminder", t_ghostnet_reminder, 80, "GhostNet's reminder of an own net 4 min ahead said on any screen"),
    ("js8-open-off", t_js8_open_off, 60, "JS8 opens with AUTO and HB off; nothing sent in 30 s"),
    ("js8-nocall", t_js8_nocall, 90, "JS8 with no callsign: CQ and send refused, off the air"),
    ("js8-texts", t_js8_texts, 120, "JS8's INFO typed, in js8_texts.txt, shown again after a reopen"),
    ("js8-map", t_js8_map, 70, "JS8's Map opens and closes"),
    ("field-home", t_field_home, 60, "Field Ops' home: its buttons, the row round in 15 steps, LOG, Bands left of LOG"),
    ("field-sota", t_field_sota, 300, "a SOTA activation end to end: nearest/MFK/typed, PHONE and QTH, Start, QSOs "
     "(SSB, FT8, no mode set on the web), not saved on a read-only folder, the logbook, End, (old)"),
    ("field-beacon-rules", t_field_beacon_rules, 120, "Field Ops' Test beacon: CW first, one press sends nothing, "
     "tuned, locked; Use band in CW and USB"),
    ("field-programmes", t_field_programmes, 300, "TOTA, WCA, MOTA, LH, LLOTA: typed refs, counters, MY_SIG/SIG"),
    ("field-tiles", t_field_tiles, 120, "Tiles: the square followed, a T3 QSO's MY_SIG TILES"),
    ("field-xota-nota", t_field_xota_nota, 150, "XOTA: the copy message, an own place, no spot, MY_SIG XOTA; NOTA's "
     "Spot message and a row into its programme"),
    ("field-map", t_field_map, 150, "Field Ops' map from test tiles: You are there, km, View, Zoom, No map here"),
    ("field-spotting", t_field_spotting_programmes, 240, "WWFF, WWBOTA, HOTA with a CB call: no spot (said), "
     "counters, MY_SIG/SIG"),
    ("field-persist", t_field_persist, 180, "Field Ops over a GUI restart: Start here, the activation as it was"),
    ("channels-app-holds", t_channels_app_holds, 70, "Channels by the hand mic's V/M over FT8: not tuned, said, kept"),
    ("birdie-marks", t_birdie_marks, 200, "birdie marks: five red ticks evenly spaced at the waterfall's top "
     "(test list, GUI restarts)"),
    ("cwkbd-refusals", t_cwkbd_refusals, 80, "CW Keyboard: NOT CW and 'Set CW mode' in USB, 'No TX' on 27.185"),
    ("prop-conditions", t_prop_conditions, 40, "Propagation shows the kept file's SFI on both"),
    ("flights", t_flights, 60, "Flights' Range round the five; the header Locator, then Phone"),
    ("weather", t_weather, 60, "Weather's header: Locator, then Phone"),
    ("cwtrainer-learn", t_cwtrainer_learn, 240, "CW Trainer Learn: Listen..., K/M groups keyed back right by TCI CW, "
     "a prosign <?> wrong, 5 WPM right (one radio at a time, 0 W sidetone)"),
    ("cwtrainer-calls", t_cwtrainer_calls, 240, "CW Trainer Calls: right with a space inside, +1 WPM (Follows), Fixed "
     "keeps KEY Speed, back after Calls"),
    ("cwtrainer-out-of-band", t_cwtrainer_out_of_band, 150, "CW Trainer: refused outside South Korea's ranges "
     "(7.250): 'No TX on 7.250 MHz', nothing keyed"),
    ("first-start", t_first_start, 300, "the first-start guide opens with no callsign (GUI restarts), walked, Done, "
     "not again"),
    ("zello-ptt", t_zello_ptt, 90, "Zello holds the PTT while open; closed, the PTT keys at 0.1 W (cable)"),
    ("svx-ptt", t_svx_ptt, 90, "SVXConnect holds the PTT while open; closed, the PTT keys at 0.1 W (cable)"),
]


def _vfo_kept(fn):
    """fn with each radio's VFO (A or B) put back as found after it: an app's A/B key is the VFO's. Two radios on
    different VFOs at the start (one left on B by an earlier test): the X6100's made the X6200's first, said"""
    def run(o):
        for n, r in o.r.items():            # a GUI from before the bench hooks (another session's install): said once
            try:
                fkeys(r)
            except Refused:
                return False, (f"{n}'s GUI has no fkeys? (a build from before main's 0e0b7579 put on by another "
                               f"install): every key of these tests reads the page first"), {}
        v = {n: r.state.get("vfo") for n, r in o.r.items()}
        if None not in v.values() and v[H.A] != v[H.B]:
            o.st.ev(f"the radios on different VFOs ({v}): the {H.A}'s made the {H.B}'s", "bad")
            o.changing += 1
            try:
                o.r[H.A].send("set vfo 1")
                o.r[H.A].wait("its VFO", lambda: o.r[H.A].state.get("vfo") == v[H.B], 4)
                align(o, int(o.r[H.B].state.get("freq", 0)), int(o.r[H.B].state.get("mode", 2)))
            finally:
                o.changing -= 1
            v = {n: r.state.get("vfo") for n, r in o.r.items()}
        try:
            return fn(o)
        finally:
            o.changing += 1
            try:
                for n, r in o.r.items():
                    if v[n] is not None and r.state.get("vfo") != v[n]:
                        o.st.ev(f"{n}: its VFO {r.state.get('vfo')} after the test, put back to {v[n]}", "radio")
                        r.send("set vfo 1")
                        try:
                            r.wait("its VFO back", lambda: r.state.get("vfo") == v[n], 4)
                        except Refused as e:
                            o.st.ev(str(e), "bad")
                align(o)
            finally:
                o.changing -= 1
    return run


# The rows that may key a radio (each radio in turn, the other listening on the same dial); the rest receive only
ROLES = {
    "zello-ptt": "each TX in turn (0.1 W), the other RX",
    "svx-ptt": "each TX in turn (0.1 W), the other RX",
    "cwtrainer-learn": "each in turn keyed at 0 W (train), the other RX",
    "cwtrainer-calls": "each in turn keyed at 0 W (train), the other RX",
    "cwtrainer-out-of-band": "each in turn keyed at 0 W (train), the other RX",
    "cb-refused": "both RX (PTT refused)",
    "hfair": "both RX (PTT refused)",
    "cwkbd-refusals": "both RX (CW refused)",
    "js8-freq": "both RX (each in a TX session)",
    "ghostnet-popup": "both RX (each in a TX session)",
    "js8-nocall": "both RX (CQ refused, a TX session)",
    "js8-texts": "both RX (each in a TX session)",
}


# Both ways over the cable: (name, fn(o, s, l), est s, what): "name x6100>x6200" and "name x6200>x6100"
PAIRS = [
    ("psk-cursor", t_psk_cursor, 90, "PSK31 at 0.1 W: the listener's cursor 40 Hz off, a main-knob click snaps it "
     "onto the signal, the call decoded there"),
    ("sstv-reply", t_sstv_reply, 150, "SSTV at 0.1 W: Reply and Replay to a typed target as received (Robot 8), "
     "TX MODE in Robot 36 (halted at 11 s)"),
    ("sstv-restart", t_sstv_restart, 120, "SSTV at 0.1 W: a CQ halted and sent again; Auto RX Restart ON saves the "
     "second, OFF the first"),
    ("ft8-show", t_ft8_show_pota, 150, "FT8 at 0.1 W: Show CQ/All, the POTA tag, the own row, the Live Map heard and "
     "emptied by a band change"),
]


def tests(Test, A, B):
    """orch's Test rows of this module (both radios each, receive only unless ROLES says otherwise; PAIRS one way
    each, the sender at 0.1 W)"""
    out = [Test(name, ROLES.get(name, "both RX"), what, _vfo_kept(fn), est) for name, fn, est, what in ROWS]
    for name, fn, est, what in PAIRS:
        for s, l in ((A, B), (B, A)):
            out.append(Test(f"{name} {s}>{l}", f"{s.upper()} TX, {l.upper()} RX", what,
                            _vfo_kept(lambda o, fn=fn, s=s, l=l: fn(o, s, l)), est))
    return out
