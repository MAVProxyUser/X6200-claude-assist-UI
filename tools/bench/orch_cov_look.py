"""The classic screen's look settings no test covered (2026-11-27, the coverage report's display, settings-display and
settings-general features needing nothing but the two radios): Hide buttons and Keep knob lines, Knob info, the
waterfall's mode and speed, the 12-hour clock, the frequency and info pop-ups and the icon menus. Each set through
its own path (the remote's cfg for the stored settings, the web configurator's Display page for the older ones),
judged from the remote's state and a picture of the screen measured against itself, and put back as found.

Nothing here transmits. Every test ends with both radios on the Sentry's page A (home_both).

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import re
import time

import bench
from bench import Refused
import orch_cov_apps as oa

H = None                        # the running orch module (orch.py: orch_cov_look.H = sys.modules[__name__])

BTN_BOX = (0, 428, 800, 480)    # the five buttons at the foot of the classic screen
KNOB_BOX = (0, 368, 330, 420)   # the two knob lines over them, left ('Volume: 0', 'Zoom: x1')
SCALE_BOX = (0, 118, 800, 148)  # the frequency scale under the dial row
CLOCK_BOX = (628, 0, 800, 38)   # the clock at the top right
MIDDLE_BOX = (150, 170, 650, 340)   # where the pop-ups come
POPUP_BOX = (245, 165, 555, 212)    # the frequency pop-up's digits (Popups Freq), under the band plan strip
VALUE_BOX = (108, 372, 132, 392)    # the first knob line's value ('Volume: 1'), drawn dim: its digit alone


def _both(o):
    return [(H.A, o.r[H.A]), (H.B, o.r[H.B])]


def _classic(o, n, r):
    import orch_cable as oc
    with o.step(f"{n}: the classic screen") as s:
        oc.classic(r)
        time.sleep(1.5)
        s.confirm("classic")


def _btn_words(path):
    """Whether the buttons' words are on the screen: VOL 1:2's (F1's page key) or the MFK's / APP's page key. Hidden,
    the waterfall grows into their place, so their box's light is no judge (2026-11-27: 11.9% -> 5.9% lit)"""
    t = " ".join(oa.ocr(path, box=BTN_BOX, psm=11, scale=2).upper().split())
    return re.search(r"\b(VOL|MFK|APP|MEM|KEY|DFN|DFL)\s*\d\s*[:.]\s*\d", t) is not None


def _knob_words(path):
    """Whether the knob lines are on the screen: 'Volume' read on the first. Their box's light is no judge: the
    spectrum's bright baseline runs through it and moves down when the buttons hide (2026-11-27: kept lines read
    3.5% lit against 8.6%)"""
    return "VOLUME" in oa.ocr(path, box=KNOB_BOX, psm=6, scale=3).upper()


def _shown(r):
    """The buttons shown again if a test left them hidden (a key brings them back; F1's first press only shows)"""
    try:
        if oa.fkeys(r)[2]:
            bench.tap(r, "F1")
            time.sleep(0.6)
    except Refused:
        pass


# ---- Hide buttons, Keep knob lines (display-hide-buttons, settings-display-hide-buttons, display-keep-knob-lines) ----

def t_hide_buttons(o):
    """On each radio's classic screen: Hide buttons after 3 s (buttons_hide 1): 5 s on, untouched, the remote says the
    buttons are hidden and the picture's buttons and knob lines are dark; F1 brings them back and does nothing else
    (the F keys' texts as before: F1 is MFK 1:4's page key); Keep knob lines on (knob_lines_stay 1): the buttons go,
    the knob lines stay. Put back as found"""
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            _classic(o, n, r)
            with o.step(f"{n}: Hide buttons after 3 s: the buttons and knob lines go; F1's first press only shows "
                        f"them") as s:
                t0, _, hid0 = oa.fkeys(r)
                if hid0:
                    raise Refused(f"{n}: the buttons were hidden before the test")
                p0 = oa.shot(o, r, "hide as found")
                b0, k0 = _btn_words(p0), _knob_words(p0)
                if not b0 or not k0:
                    raise Refused(f"{n}: no buttons or knob lines to hide (page key read {b0}, Volume read {k0})")
                try:
                    with oa.cfg_kept(r, {"buttons_hide": 1}):
                        time.sleep(5.0)
                        _, _, hid1 = oa.fkeys(r)
                        p1 = oa.shot(o, r, "hidden")
                        b1, k1 = _btn_words(p1), _knob_words(p1)
                        if not hid1 or b1 or k1:
                            raise Refused(f"{n}: 5 s after Hide buttons 3 s: hidden {hid1}, the buttons' page key "
                                          f"{'still read' if b1 else 'gone'}, the knob lines "
                                          f"{'still' if k1 else 'gone'}")
                        bench.tap(r, "F1")
                        time.sleep(0.8)
                        t2, _, hid2 = oa.fkeys(r)
                        b2 = _btn_words(oa.shot(o, r, "shown by F1"))
                        if hid2 or not b2:
                            raise Refused(f"{n}: F1 did not bring the buttons back (hidden {hid2}, page key read {b2})")
                        if t2 != t0:
                            raise Refused(f"{n}: F1's first press acted as well: the F keys {t0} -> {t2}")
                finally:
                    _shown(r)
                out["hide"] = "the buttons' words and the knob lines gone, the buttons back with F1"
                s.confirm(out["hide"])
            with o.step(f"{n}: Keep knob lines: the knob lines stay while the buttons hide") as s:
                try:
                    with oa.cfg_kept(r, {"buttons_hide": 1, "knob_lines_stay": 1}):
                        time.sleep(5.0)
                        _, _, hid3 = oa.fkeys(r)
                        p3 = oa.shot(o, r, "hidden, knob lines kept")
                        b3, k3 = _btn_words(p3), _knob_words(p3)
                        if not hid3 or b3 or not k3:
                            raise Refused(f"{n}: Keep knob lines: hidden {hid3}, the buttons' page key "
                                          f"{'still read' if b3 else 'gone'}, the knob lines "
                                          f"{'kept' if k3 else 'gone'}")
                finally:
                    _shown(r)
                out["keep"] = "the buttons' words gone, the knob lines kept"
                s.confirm(out["keep"])
        return True, f"Hide buttons and Keep knob lines on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- Knob info (display-knob-info-lines, settings-display-knob-info) ----

def t_knob_info(o):
    """On each radio's classic screen: Knob info on (as found, the default) shows the knob lines naming VOL's control
    ('Volume: N', read); a turn of VOL changes the value there (one detent, turned back: its pixels, against what
    they do by themselves, as the value is drawn dim and OCR read 0 and 1 as @ and 4, 2026-11-27); Knob info off: the
    knob lines go dark. Put back as found"""
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            _classic(o, n, r)
            with o.step(f"{n}: Knob info: the knob lines say VOL's control and value, follow a turn, go when off") as s:
                with oa.cfg_kept(r, {"knob_info": 1}):
                    time.sleep(1.0)
                    p0 = oa.shot(o, r, "knob info on")
                    time.sleep(0.6)
                    p0b = oa.shot(o, r, "knob info on b")
                    if not _knob_words(p0):
                        raise Refused(f"{n}: the knob lines do not say the volume")
                    v0 = int(r.state.get("vol") or 0)
                    step = 1 if v0 < 5 else -1
                    r.take_control()
                    r.send(f"knob vol {step}")
                    time.sleep(0.8)
                    try:
                        p1 = oa.shot(o, r, "knob info turned")
                        v1 = int(r.state.get("vol") or 0)
                    finally:
                        r.send(f"knob vol {-step}")
                        time.sleep(0.8)
                    from orch_cov_ui import _diff
                    alone, moved = _diff(p0, p0b, VALUE_BOX), _diff(p0b, p1, VALUE_BOX)
                    if v1 == v0 or moved < 3 * alone + 0.03:
                        raise Refused(f"{n}: VOL {v0} -> {v1}: the knob line's value changed {moved:.1%} (by itself "
                                      f"{alone:.1%})")
                with oa.cfg_kept(r, {"knob_info": 0}):
                    time.sleep(1.0)
                    k2 = _knob_words(oa.shot(o, r, "knob info off"))
                if k2:
                    raise Refused(f"{n}: Knob info off left the knob lines on the screen (Volume read)")
                out["knob info"] = f"VOLUME {v0} -> {v1}: its digit {moved:.0%} changed (by itself {alone:.0%}); " \
                                   f"off, gone"
                s.confirm(out["knob info"])
        return True, f"Knob info on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- The waterfall's mode and speed (settings-display-waterfall-mode, settings-display-waterfall-speed) ----

def _vfo_x(path):
    """The x of the VFO's passband (the lightest columns' middle) in the waterfall's rows"""
    from PIL import Image
    im = Image.open(path).convert("L").crop((0, 180, 800, 400))
    w, h = im.size
    px = im.load()
    col = [sum(px[x, y] for y in range(0, h, 2)) for x in range(w)]
    sm = [sum(col[max(0, x - 6):x + 7]) for x in range(w)]
    return max(range(w), key=lambda x: sm[x])


def t_wf_mode(o):
    """On each radio's classic screen at 14.200 MHz: Waterfall Mode Centered (wf_mode 0): tuned 5 kHz up the scale
    under the dial moves and the VFO's passband stays in the middle; Fixed (1): the scale stands still and the
    passband moves right. Tuned back, put back as found"""
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            _classic(o, n, r)
            for mode, label in ((0, "Centered"), (1, "Fixed")):
                with o.step(f"{n}: Waterfall {label}: tuned 5 kHz up, the scale "
                            f"{'moves, the VFO stays' if mode == 0 else 'stays, the VFO moves'}") as s:
                    o.changing += 1
                    try:
                        with oa.cfg_kept(r, {"wf_mode": mode}):
                            bench.ensure(r, {"freq": 14200000})
                            time.sleep(2.0)
                            a = oa.shot(o, r, f"wf {label} at 14.200")
                            a2 = oa.shot(o, r, f"wf {label} at 14.200 b")
                            bench.ensure(r, {"freq": 14205000})
                            time.sleep(2.0)
                            b = oa.shot(o, r, f"wf {label} at 14.205")
                    finally:
                        bench.ensure(r, {"freq": 14200000})
                        o.changing -= 1
                    from orch_cov_ui import _diff
                    alone, scale = _diff(a, a2, SCALE_BOX), _diff(a, b, SCALE_BOX)
                    dx = _vfo_x(b) - _vfo_x(a)
                    out[label] = {"scale": round(scale, 3), "alone": round(alone, 3), "vfo_dx": dx}
                    if mode == 0 and (scale < max(0.01, 3 * alone) or abs(dx) > 10):
                        raise Refused(f"{n}: Centered, 5 kHz up: the scale changed {scale:.1%} (by itself "
                                      f"{alone:.1%}), the VFO moved {dx} px")
                    if mode == 1 and (scale > max(0.004, 3 * alone) or dx < 15):
                        raise Refused(f"{n}: Fixed, 5 kHz up: the scale changed {scale:.1%} (by itself {alone:.1%}), "
                                      f"the VFO moved {dx} px")
                    s.confirm(f"scale {scale:.1%}, VFO {dx:+d} px")
        return True, f"the waterfall's Centered and Fixed modes on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


def t_wf_speed(o):
    """On each radio's classic screen: Waterfall Speed 5 (30 lines/s) changes the waterfall between two pictures a
    second apart more than Speed 1 (5 lines/s): three pairs each, their middle compared. Put back as found"""
    from orch_cov_ui import _diff
    box = (0, 175, 800, 400)
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            _classic(o, n, r)
            with o.step(f"{n}: Waterfall Speed 5 moves the waterfall more than Speed 1") as s:
                moved = {}
                for speed in (1, 5):
                    with oa.cfg_kept(r, {"waterfall_speed": speed}):
                        time.sleep(2.0)
                        d = []
                        for k in range(3):
                            a = oa.shot(o, r, f"wf speed {speed} {k}a")
                            time.sleep(1.0)
                            b = oa.shot(o, r, f"wf speed {speed} {k}b")
                            d.append(_diff(a, b, box, thr=20))
                        moved[speed] = sorted(d)[1]
                out.update({f"speed {k}": round(v, 4) for k, v in moved.items()})
                if moved[5] < 1.3 * moved[1] + 0.002:
                    raise Refused(f"{n}: the waterfall changed {moved[1]:.2%} a second at Speed 1, {moved[5]:.2%} at "
                                  f"Speed 5")
                s.confirm(f"Speed 1 {moved[1]:.2%}, Speed 5 {moved[5]:.2%} a second")
        return True, f"the waterfall's speed on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- The 12-hour clock (settings-general-12h-clock) ----

def t_clock_12h(o):
    """On each radio's classic screen: 12-hour clock on (the web Display page's clock_12h): the clock at the top right
    says AM or PM; off: 24-hour hours, minutes and seconds with neither. Put back as found"""
    import orch_cable as oc
    res = {}
    try:
        for n, r in _both(o):
            _classic(o, n, r)
            with o.step(f"{n}: the 12-hour clock says AM or PM, the 24-hour clock neither") as s:
                with oc.disp_kept(r, {"clock_12h": 1}):
                    time.sleep(1.5)
                    on = oa.ocr(oa.shot(o, r, "clock 12h"), box=CLOCK_BOX, psm=7, scale=4).upper()
                with oc.disp_kept(r, {"clock_12h": 0}):
                    time.sleep(1.5)
                    off = oa.ocr(oa.shot(o, r, "clock 24h"), box=CLOCK_BOX, psm=7, scale=4).upper()
                res[n] = {"12h": " ".join(on.split())[:24], "24h": " ".join(off.split())[:24]}
                m12 = re.search(r"\b(\d{1,2})\s?:.*?\b([AP])\s?M\b", on)
                m24 = re.search(r"\b(\d{1,2})\s?:", off)
                # the minutes' digits are the pixel font's ('16: @4:A3', 2026-11-27): the hours judge
                same = m12 and m24 and \
                    int(m12.group(1)) % 12 + (12 if m12.group(2) == "P" else 0) == int(m24.group(1))
                if not m12 or not m24 or re.search(r"\b[AP]\s?M\b", off) or not same:
                    raise Refused(f"{n}: the clock read {res[n]['12h']!r} with 12-hour on, {res[n]['24h']!r} off")
                s.confirm(f"{res[n]['12h']!r} / {res[n]['24h']!r}")
        return True, f"the 12-hour clock on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- Pop-ups (settings-display-popups-freq, settings-display-popups-info) ----

def _popup(o, r, label, act, undo, n=3):
    """n pictures, each right after act of its own (a pop-up is brief: three pictures after one act caught it once,
    and none in a link stall, 2026-11-27), undo(n) run after: the most MIDDLE_BOX changed against a picture just
    before, and the pictures"""
    from orch_cov_ui import _diff
    a = oa.shot(o, r, f"{label} before")
    shots = []
    try:
        for k in range(n):
            act()
            time.sleep(0.15)
            shots.append(oa.shot(o, r, f"{label} {k + 1}"))
    finally:
        undo(n)
    return max(_diff(a, b, MIDDLE_BOX) for b in shots), shots


def _frame(path):
    """Whether the pop-up's frame is on the picture: its left, right and bottom edges lit (100-170 grey with one, 10-14
    without, faded or not, 2026-11-27)"""
    from PIL import Image
    px = Image.open(path).convert("L").load()
    left = max(sum(px[x, y] for y in range(175, 212)) / 37 for x in range(234, 244))
    right = max(sum(px[x, y] for y in range(175, 212)) / 37 for x in range(556, 566))
    bottom = max(sum(px[x, y] for x in range(260, 540)) / 280 for y in range(210, 222))
    return min(left, right, bottom) > 60


def _popup_read(paths, box):
    """What tesseract reads in box on any of the pictures, its dim pixels (a fading pop-up) lifted to white first"""
    import os
    from PIL import Image
    out = []
    for p in paths:
        q = p + ".lift.png"
        Image.open(p).convert("L").crop(box).point(lambda v: 255 if v > 60 else 0).save(q)
        try:
            out.append(oa.ocr(q, psm=6, scale=2).upper().replace(" ", ""))
        finally:
            os.remove(q)
    return out


def t_popups(o):
    """On each radio's classic screen: Popups Freq on (mag_freq): a detent of the main knob puts the new frequency big
    in the middle (its frame in a picture taken right after, three detents and pictures, and the frequency read there
    with its fading digits lifted), off neither; Popups Info on (mag_info): A/B (VFO B, then back to A, a picture after
    each) puts its note (VFO-B) in the same frame, off no frame. The dial put back, the settings as found"""
    import orch_cable as oc
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            _classic(o, n, r)
            with o.step(f"{n}: Popups Freq: the frequency big in the middle while tuning, not when off") as s:
                o.changing += 1
                try:
                    bench.ensure(r, {"freq": 14200000})
                    r.take_control()
                    turn = (lambda: r.send("knob main 1"))
                    back = (lambda k: (r.send(f"knob main -{k}"), time.sleep(1.5)))
                    with oc.disp_kept(r, {"mag_freq": 1}):
                        time.sleep(0.8)
                        on, ps_on = _popup(o, r, "popup freq on", turn, back)
                    with oc.disp_kept(r, {"mag_freq": 0}):
                        time.sleep(0.8)
                        off, ps_off = _popup(o, r, "popup freq off", turn, back)
                finally:
                    bench.ensure(r, {"freq": 14200000})
                    o.changing -= 1
                # the frequency read big in its box on, not off (lifted: '14,201,BBB'; the waterfall moves with the
                # dial, so the middle's change is no judge alone: 4.6% on, 1.9% off, 2026-11-27)
                FREQ = r"14[.,]\S{2,3}[.,]"
                big, plain = _popup_read(ps_on, POPUP_BOX), _popup_read(ps_off, POPUP_BOX)
                f_on, f_off = sum(map(_frame, ps_on)), sum(map(_frame, ps_off))
                out["freq"] = {"frames on": f_on, "frames off": f_off}
                if not f_on or f_off or not any(re.search(FREQ, t) for t in big):
                    raise Refused(f"{n}: Popups Freq: the pop-up's frame in {f_on}/3 pictures on, {f_off}/3 off; read "
                                  f"on {[t[:20] for t in big]}, off {[t[:20] for t in plain]}")
                s.confirm(f"its frame in {f_on}/3 pictures on, {f_off}/3 off; the frequency read")
            with o.step(f"{n}: Popups Info: a note in the middle when A/B switches the VFO, not when off") as s:
                keep = {k: int(r.state[k]) for k in ("vfo", "freq", "mode") if r.state.get(k) is not None}
                o.changing += 1
                try:
                    ab = (lambda: bench.tap(r, "AB"))
                    ab_back = (lambda k: ([bench.tap(r, "AB") for _ in range(k % 2)], time.sleep(2.5)))
                    with oc.disp_kept(r, {"mag_info": 1}):
                        time.sleep(0.8)
                        on, qs_on = _popup(o, r, "popup info on", ab, ab_back, n=2)     # B, then A again
                    with oc.disp_kept(r, {"mag_info": 0}):
                        time.sleep(0.8)
                        off, qs_off = _popup(o, r, "popup info off", ab, ab_back, n=2)
                finally:
                    bench.ensure(r, keep)
                    o.changing -= 1
                # its note is the VFO's name ('VFO-B', screen_main.c tiny_vfo), read in the middle on, not off
                # judged by the pop-up's frame: its 'VFO-B', drawn as it fades in, read 'VOU-B', 'YPU-B' and 'NE'
                # (2026-11-27); what was read is said
                note = _popup_read(qs_on, POPUP_BOX)
                vfo = r"[A-Z0-9@][-—]+[AB](?![A-Z])"
                f_on, f_off = sum(map(_frame, qs_on)), sum(map(_frame, qs_off))
                out["info"] = {"frames on": f_on, "frames off": f_off,
                               "read": any(re.search(vfo, t) for t in note)}
                if not f_on or f_off:
                    raise Refused(f"{n}: Popups Info at A/B: the pop-up's frame in {f_on}/2 pictures on, {f_off}/2 "
                                  f"off (read on {[t[:20] for t in note]})")
                s.confirm(f"its frame in {f_on}/2 pictures on, {f_off}/2 off; VFO read: {out['info']['read']}")
        return True, f"the frequency and info pop-ups on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- Icon menus (settings-display-icon-menus, app-icon-grid) ----

TILES = ("CLUSTER", "FIELD OPS", "BANDS", "DX SPOTS", "SOTA", "POTA", "WALKIE", "BIRDIE", "WEFAX", "RECORDER")


def t_icon_menus(o):
    """On each radio's classic screen: Icon menus on (icon_menus): APP opens the grid of app tiles (three of TILES read:
    Cluster, Field Ops, Bands, DX Spots... as the first screen of it shows them), APP closes it; off: APP gives the
    buttons' app pages instead (the F keys' texts change, no tiles), GEN goes back. Put back as found"""
    import orch_cable as oc
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            _classic(o, n, r)
            with o.step(f"{n}: Icon menus on: APP opens the app tiles, APP closes them; off: the button pages") as s:
                t0, _, _ = oa.fkeys(r)
                r.take_control()
                with oc.disp_kept(r, {"icon_menus": 1}):
                    time.sleep(0.8)
                    bench.tap(r, "APP")
                    time.sleep(1.2)
                    p = oa.shot(o, r, "icon grid")
                    grid = oa.ocr(p, psm=11, scale=2).upper()
                    bench.tap(r, "APP")
                    time.sleep(1.0)
                    closed = oa.ocr(oa.shot(o, r, "icon grid closed"), psm=11, scale=2).upper()
                tiles = [w for w in TILES if w in grid]
                left = [w for w in TILES if w in closed]
                if len(tiles) < 3 or len(left) >= 2:
                    raise Refused(f"{n}: APP with icon menus showed tiles {tiles} (read "
                                  f"{' '.join(grid.split())[:80]!r}), after APP again {left}")
                with oc.disp_kept(r, {"icon_menus": 0}):
                    time.sleep(0.8)
                    bench.tap(r, "APP")
                    time.sleep(1.2)
                    t1, _, _ = oa.fkeys(r)
                    plain = oa.ocr(oa.shot(o, r, "app pages"), box=(0, 60, 800, 420), psm=11, scale=2).upper()
                    bench.tap(r, "GEN")
                    time.sleep(1.0)
                grid_off = [w for w in TILES if w in plain]
                if t1 == t0 or len(grid_off) >= 2:
                    raise Refused(f"{n}: APP without icon menus: the F keys {'unchanged' if t1 == t0 else 'changed'}, "
                                  f"tiles {grid_off}")
                out["tiles"], out["pages"] = tiles, [oa.flat(x) for x in t1]
                s.confirm(f"tiles {tiles}; without: {out['pages']}")
        return True, f"the icon menus on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


COVERS = {
    "look-hide-buttons": (["display-hide-buttons", "settings-display-hide-buttons", "display-keep-knob-lines"], []),
    "look-knob-info": (["display-knob-info-lines", "settings-display-knob-info"], []),
    "look-wf-mode": (["settings-display-waterfall-mode"], []),
    "look-wf-speed": (["settings-display-waterfall-speed"], []),
    "look-clock-12h": (["settings-general-12h-clock"], []),
    "look-popups": (["settings-display-popups-freq", "settings-display-popups-info"], []),
    "look-icon-menus": (["settings-display-icon-menus", "app-icon-grid"], []),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("look-hide-buttons", "both", "Hide buttons after 3 s: buttons and knob lines go, F1 only brings them "
             "back; Keep knob lines keeps those", t_hide_buttons, 60),
        Test("look-knob-info", "both", "Knob info: the knob lines say 'Volume: N' and follow a VOL turn; off, they go",
             t_knob_info, 40),
        Test("look-wf-mode", "both", "the waterfall Centered (the scale moves with the dial) and Fixed (the VFO line "
             "moves)", t_wf_mode, 60),
        Test("look-wf-speed", "both", "Waterfall Speed 5 moves the waterfall more than Speed 1", t_wf_speed, 60),
        Test("look-clock-12h", "both", "the 12-hour clock says AM or PM at the top right, the 24-hour one neither",
             t_clock_12h, 30),
        Test("look-popups", "both", "Popups Freq: the frequency big in the middle while tuning; Popups Info: a note at "
             "A/B; neither when off", t_popups, 70),
        Test("look-icon-menus", "both", "Icon menus: APP opens the app tiles and closes them; off, the button pages",
             t_icon_menus, 50),
    ]
