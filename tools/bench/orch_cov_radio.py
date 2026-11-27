"""The radio side's features no test covered (2026-11-27, the coverage report's TX, CW, TCI, remote, web, SDR and
Sentry A/C-G/I areas, each needing nothing but the two radios, the cable or an RTL-SDR): their orch tests, each
judging itself from the remote's state, the radios' own logs and tools, or the text on a picture of the screen.

Nothing here transmits: refusals are read before any key, the apps open with nothing queued to send. Every test
ends with both radios on the Sentry's page A (home_both).

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import re
import time

import bench
from bench import Refused
import orch_cov_apps as oa

H = None                        # the running orch module (orch.py: orch_cov_radio.H = sys.modules[__name__])


def _both(o):
    return [(H.A, o.r[H.A]), (H.B, o.r[H.B])]


# ---- FT8 with no callsign, and its TX off (ft8-nocall, ft8-tx-off-dry-run) ----

def t_ft8_nocall(o):
    """The FT8 app on each radio in turn: with no callsign set its state says nocall and TX Call answers 'Call sign
    required', nothing keyed; with the callsign back, TX Call twice: TX on, then off with 'TX off: receive only until
    TX Call, CQ or a call is pressed' (nothing queued, nothing keyed all the while). The callsign put back as found
    (never said)"""
    res = {}
    names = ("FT8",)
    try:
        oa._ft8_open(o)
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: no callsign: FT8 says nocall; TX Call answers 'Call sign required', nothing keyed") as s:
                with oa.cfg_kept(r, {"callsign": ""}):
                    r.wait("FT8's nocall", lambda: r.ft8.get("nocall") == 1, 6)
                    k = oa._to_label(r, r"^TX\s*Call", names)
                    t0 = time.time()
                    oa.press(r, k, names, rest=0.4)
                    out["no_call"] = oa.wait_msg(r, t0, r"Call sign required", 4)
                    time.sleep(1.0)
                    if r.ft8.get("tx") or any(on for t, on in r.tx_hist if t >= t0):
                        raise Refused(f"{n}: TX came on (ft8 tx {r.ft8.get('tx')}) with no callsign")
                r.wait("FT8's callsign back", lambda: r.ft8.get("nocall") == 0, 6)
                s.confirm(f"nocall 1, {out['no_call']!r}, TX off, nothing keyed")
            with o.step(f"{n}: TX Call on, then off: 'TX off: receive only ...', nothing keyed") as s:
                k = oa._to_label(r, r"^TX\s*Call", names)
                t0 = time.time()
                if r.ft8.get("tx"):             # on already: the press under test turns it off
                    oa.press(r, k, names, rest=0.6)
                else:
                    oa.press(r, k, names, rest=0.6)
                    r.wait("FT8's TX on", lambda: r.ft8.get("tx") == 1, 4)
                    if r.ft8.get("next"):
                        raise Refused(f"{n}: FT8 has {r.ft8.get('next')!r} queued: not pressed again")
                    oa.press(r, k, names, rest=0.6)
                out["tx_off"] = oa.wait_msg(r, t0, r"TX off: receive only until TX Call, CQ or a call is pressed", 4)
                r.wait("FT8's TX off", lambda: r.ft8.get("tx") == 0, 4)
                if any(on for t, on in r.tx_hist if t >= t0):
                    raise Refused(f"{n}: the radio keyed while TX was on with nothing queued")
                s.confirm(out["tx_off"])
        return True, "no callsign: nocall and 'Call sign required'; TX Call on then off: 'TX off: receive only ...'; " \
                     "nothing keyed on either radio", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa._ft8_close(o)
        oa.home_both(o)


# ---- The X6200's own start: PA bias, register 32, the base's band table (pa-bias-x6200, register-32-x6200,
#      tx-x6200-base-band-table) ----

START_LINES = {
    "register-32-x6200": r"base: register 32 = 0x0a, as Xiegu's firmware",
    "pa-bias-x6200": r"base i2c: PA bias (\d+)/(\d+) \(drive/final\): (this radio's own, from the base's calibration "
                     r"block|.*fallback.*)",
    "tx-x6200-base-band-table": r"hw_x6200: TX outside the base's band table (open|shut)",
}


def t_x6200_start(o):
    """The X6200's GUI log since its start: register 32 set to 0x0a as Xiegu's firmware, its own PA bias read from the
    base's calibration block and sent (the drive and final figures in range, else the fallback said), the base's band
    table set; the X6100's has none of them (an X6200's alone)"""
    res = {}
    with o.step("the X6200's GUI log: register 32, its PA bias, the base's band table") as s:
        r = o.r[H.B]
        log = oa.ssh(r, "cat /tmp/x6100_log.txt", 20)
        for f, rx in START_LINES.items():
            m = re.search(rx, log)
            if not m:
                return False, f"x6200: no {f} line in its GUI log ({rx!r})", res
            res[f] = m.group(0)
        m = re.search(START_LINES["pa-bias-x6200"], log)
        drive, final = int(m.group(1)), int(m.group(2))
        if "own" in m.group(3) and not (2000 <= drive <= 4095 and 2000 <= final <= 4095):
            return False, f"x6200: its own PA bias {drive}/{final} out of range, not the fallback", res
        s.confirm("; ".join(res.values()))
    with o.step("the X6100's GUI log: none of the X6200's lines") as s:
        log = oa.ssh(o.r[H.A], "cat /tmp/x6100_log.txt", 20)
        bad = [f for f, rx in START_LINES.items() if re.search(rx, log)]
        if bad:
            return False, f"x6100: its log has the X6200's {bad}", res
        s.confirm("none")
    return True, "the X6200 set register 32, sent its own PA bias and set the base's band table: " + \
        "; ".join(res.values()), res


# ---- TX calibration belongs to its radio (tx-calibration-fingerprint) ----

def t_fingerprint(o):
    """Each radio's fingerprint, from x6x00-fingerprint over SSH (the hashed chip ID) and from its GUI (the remote's fp,
    radio_fingerprint, the same salt): the same on each radio, read twice the same, and the two radios' different"""
    res = {}
    with o.step("x6x00-fingerprint and the GUI's fp on both: alike on each, different between them") as s:
        for n, r in _both(o):
            a = oa.ssh(r, "x6x00-fingerprint", 15).strip()
            b = oa.ssh(r, "x6x00-fingerprint", 15).strip()
            fp = str(r.state.get("fp") or "")
            res[n] = {"tool": a[:12] + "...", "gui": fp[:12] + "..."}
            if not a or a != b:
                return False, f"{n}: x6x00-fingerprint read {a!r} then {b!r}", res
            if fp != a:
                return False, f"{n}: the GUI's fingerprint {fp[:12]}... is not the tool's {a[:12]}...", res
        if oa.ssh(o.r[H.A], "x6x00-fingerprint", 15).strip() == oa.ssh(o.r[H.B], "x6x00-fingerprint", 15).strip():
            return False, "both radios have the same fingerprint", res
        s.confirm(str(res))
    return True, f"each radio's fingerprint the same from its tool and its GUI, the two different: {res}", res


# ---- The digital apps hold the power at 7 W (digi-app-power-cap-7w, digital-app-power-cap) and step it
#      (digi-app-power-button) ----

APP_STEPS = [0.1, 0.2, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0]


def _pwr(r):
    return round(float(r.state.get("pwr", 0)), 1)


def t_app_power(o):
    """The FT8 app on both with the radio's power set at 8 W first: opened, the power reads 7 W ('Power is limited to
    7W'); its Power button steps 0.1, 0.2, 0.5, 1, 2, 3, 5, 7 W round (each read back); closed, the power is 8 W again.
    Nothing keyed (no TX: the power is a setting)"""
    res = {}
    names = ("FT8",)
    try:
        with o.step("both at 8 W on 20 m's FT8 dial, the app closed") as s:
            o.changing += 1
            try:
                for n, r in _both(o):
                    bench.ensure(r, {"pwr": 8.0})
            finally:
                o.changing -= 1
            s.confirm(f"{[_pwr(r) for _, r in _both(o)]} W")
        with o.step("FT8 opened on both: the power held at 7 W, said") as s:
            t0 = time.time()
            oa._ft8_open(o)
            for n, r in _both(o):
                r.wait("7 W", lambda r=r: _pwr(r) == 7.0, 5)
                res[f"{n} said"] = oa.wait_msg(r, t0, r"Power is limited to 7 ?W", 5)
            s.confirm(str(res))
        with o.step("the app's Power button on both: 0.1, 0.2, 0.5, 1, 2, 3, 5, 7 W round") as s:
            def steps(n, r):
                k = oa._to_label(r, r"^Power", names)
                seen = []
                for _ in range(len(APP_STEPS) + 1):
                    p0 = _pwr(r)
                    oa.press(r, k, names, rest=0.5)
                    try:
                        r.wait("the power stepped", lambda: _pwr(r) != p0, 3)
                    except Refused:
                        pass
                    seen.append(_pwr(r))
                return seen
            o.changing += 1
            try:
                got = oa.each(o, steps)
            finally:
                o.changing -= 1
            for n, seen in got.items():
                res[f"{n} steps"] = seen
                i = APP_STEPS.index(seen[0]) if seen and seen[0] in APP_STEPS else -1
                want = [APP_STEPS[(i + k) % len(APP_STEPS)] for k in range(len(seen))] if i >= 0 else None
                if want != seen:
                    raise Refused(f"{n}: the Power button stepped {seen}, not round {APP_STEPS}")
            s.confirm(str(got))
        with o.step("FT8 closed on both: the power 8 W again") as s:
            oa._ft8_close(o)
            for n, r in _both(o):
                r.wait("8 W again", lambda r=r: _pwr(r) == 8.0, 6)
            s.confirm("8 W")
        return True, f"the apps hold 7 W and give 8 W back; their Power button steps {APP_STEPS}: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa._ft8_close(o)
        o.changing += 1
        try:
            for n, r in _both(o):
                try:
                    bench.ensure(r, {"pwr": H.W})
                except Refused as e:
                    o.st.ev(f"{n}: power not put back: {e}", "bad")
        finally:
            o.changing -= 1
        oa.home_both(o)


# ---- Pairing a browser (pair-code-request, pair-with-code, pair-rate-limit, pair-mirror-pause, role-admin-first) ----

PAIR_NAME = "bench-pair-test"


def _code_on_screen(o, r, label):
    """The six-digit pairing code read off r's screen (OCR), or None"""
    for k in range(3):
        p = oa.shot(o, r, f"{label} {k + 1}")
        txt = oa.ocr(p, psm=11) + "\n" + oa.ocr(p, box=(150, 100, 650, 380), psm=6, scale=3)
        m = re.findall(r"(?<!\d)(\d{3})\s?(\d{3})(?!\d)", txt)
        if m:
            return "".join(m[0])
        time.sleep(0.8)
    return None


def t_pairing(o):
    """A new browser on the X6100 (the remote's port, no token): 'pair?' shows a six-digit code on the radio for two
    minutes (read off its screen); 'pair?' again within 30 s is paused ('a new code in N s'); while the code shows a
    viewer's screen mirror stops; 'pair CODE NAME' answers 'paired TOKEN operate 1' (not admin: the radio has its first
    browser); a new connection with 'auth TOKEN' is that session ('role operate 1 NAME'); 'unpair' ends it after"""
    from remote_ws import Remote
    r = o.r[H.A]
    res, c, v, tok = {}, None, None, None
    try:
        with o.step("x6100: a viewer's screen mirror flowing, then 'pair?' from a new browser: the code shown") as s:
            v = Remote(r.host, 50002, timeout=8)
            v.send("screen 1")
            time.sleep(3.0)
            f0 = v.binary
            time.sleep(2.0)
            res["mirror_before"] = v.binary - f0
            c = Remote(r.host, 50002, timeout=8)
            c.send("pair?")
            ans = _until(c, lambda t: t.startswith(("pairing code shown", "err pairing")), 5)
            if not ans or not ans.startswith("pairing code shown"):
                raise Refused(f"x6100: 'pair?' answered {ans!r}")
            res["asked"] = ans
            code = _code_on_screen(o, r, "pairing code")
            if not code:
                raise Refused("x6100: no six-digit code read on the radio's screen after 'pair?'")
            s.confirm(f"{ans}; a code of six digits on the screen; the mirror {res['mirror_before']} frames in 2 s before")
        with o.step("x6100: 'pair?' again at once: paused, a new code in under 30 s; the mirror stopped meanwhile") as s:
            c.send("pair?")
            ans = _until(c, lambda t: t.startswith(("pairing code shown", "err pairing")), 5)
            m = re.search(r"a new code in (\d+) s", ans or "")
            if not m or int(m.group(1)) > 30:
                raise Refused(f"x6100: a second 'pair?' answered {ans!r}")
            res["again"] = ans
            f0 = v.binary
            time.sleep(2.0)
            res["mirror_during"] = v.binary - f0
            if res["mirror_before"] >= 2 and res["mirror_during"] > res["mirror_before"] // 3:
                raise Refused(f"x6100: the viewer's mirror went on while the code showed "
                              f"({res['mirror_during']} frames in 2 s, {res['mirror_before']} before)")
            s.confirm(f"{ans}; the mirror {res['mirror_during']} frames in 2 s")
        with o.step("x6100: 'pair CODE NAME': paired as an operator with TX; the token a session at a new connection") as s:
            c.send(f"pair {code} {PAIR_NAME}")
            ans = _until(c, lambda t: t.startswith(("paired ", "err pair")), 5)
            m = re.match(r"paired (\S+) (\S+) (\d)", ans or "")
            if not m:
                raise Refused(f"x6100: 'pair CODE NAME' answered {ans!r}")
            tok, role, tx = m.group(1), m.group(2), m.group(3)
            res["role"], res["tx"] = role, tx
            if role != "operate":
                raise Refused(f"x6100: a later browser paired as {role!r}, not an operator")
            c.close()
            c = Remote(r.host, 50002, timeout=8)
            c.send(f"auth {tok}")
            ans = _until(c, lambda t: t.startswith("role "), 5)
            if not ans or not ans.startswith(f"role operate {tx} {PAIR_NAME}"):
                raise Refused(f"x6100: 'auth TOKEN' at a new connection answered {ans!r}")
            res["auth"] = ans
            s.confirm(f"paired {role} {tx}; {ans}")
        return True, (f"'pair?' showed a code, a second paused ({res['again']}), the viewer's mirror stopped meanwhile "
                      f"({res['mirror_before']} -> {res['mirror_during']} frames in 2 s); paired as {res['role']} "
                      f"with TX {res['tx']}, the token a session again ({res['auth']})"), res
    except Refused as e:
        return False, str(e), res
    finally:
        try:
            if c is not None and tok:
                c.send("unpair")
                time.sleep(0.5)
        except OSError:
            pass
        for x in (c, v):
            if x is not None:
                x.close()
        oa.home_both(o)


def _until(c, pred, secs):
    """The first text from the raw client c that pred takes, or None in secs"""
    end = time.time() + secs
    while time.time() < end:
        t = c.recv(timeout=max(0.05, end - time.time()))
        if t is None:
            if not c.alive:
                return None
            continue
        if pred(t):
            return t
    return None


# ---- The Sentry's pages read off the screen (sentry-a-smeter, c-log-columns-colours, d-bands-list, e-decode-list,
#      f-system-cpu-gpu-ram, g-state-lines, g-channel-meters, df-f2-array, df-f5-set) ----

# (page, words every radio's picture of it must hold, the features they show)
PAGE_WORDS = [
    ("C", ["FREQ", "BAND", "WHAT", "BEST", "ANTENNA", "LAST", "SEEN"], ["c-log-columns-colours"]),
    ("D", ["BAND", "BEST", "NOW", "RX"], ["d-bands-list"]),
    ("E", ["TYPE", "BAND", "BEST", "NOW"], ["e-decode-list"]),
    ("F", ["PROCESSOR", "GRAPHICS", "MEMORY"], ["f-system-cpu-gpu-ram"]),
    ("G", ["VIA"], ["g-channel-meters", "g-state-lines"]),
]


def _page_text(o, r, letter, label):
    p = oa.shot(o, r, label)
    return p, oa.ocr(p, psm=11) + "\n" + oa.ocr(p, psm=6, stretch=True)


def t_sentry_pages(o):
    """The Sentry's pages C, D, E, F and G on both radios, read off their pictures: C LOG's columns, D BANDS' and E
    DECODE's heads, F SYSTEM's PROCESSOR/GRAPHICS/MEMORY bars with figures, G's channel panel naming its radio (VIA
    X6100 / VIA X6200) and its state lines (the battery and the processor's temperature); page A's S-meter line
    ('S<n> -NNN.N DBM') within 3 dB of the remote's S-meter"""
    res = {}
    try:
        for letter, words, _ in PAGE_WORDS:
            with o.step(f"both on page {letter}: {', '.join(words)} on each picture") as s:
                oa.page_both(o, letter)
                time.sleep(1.5)
                for n, r in _both(o):
                    p, txt = _page_text(o, r, letter, f"page {letter}")
                    miss = [w for w in words if not oa.ocr_has(txt, w)]
                    if letter == "G" and not re.search(r"VIA\s*X\s*6\s*[12I][0O]\s*[0O]", txt.upper()):
                        miss.append(f"VIA {n.upper()}")
                    if letter == "F" and not re.search(r"\d+\s*MHZ", txt.upper()):
                        miss.append("a clock in MHz")
                    res[f"{n} {letter}"] = "ok" if not miss else f"missing {miss}"
                    if miss:
                        raise Refused(f"{n}: page {letter} reads no {miss} ({' '.join(txt.split())[:160]!r})")
                s.confirm(", ".join(f"{n} ok" for n, _ in _both(o)))
        with o.step("both on page A: the S-meter line against the remote's S-meter") as s:
            oa.page_both(o, "A")
            time.sleep(1.5)
            for n, r in _both(o):
                p = oa.shot(o, r, "page A smeter")
                txt = oa.ocr(p, psm=11) + "\n" + oa.ocr(p, psm=6, stretch=True)
                m = re.search(r"S\s?([1-9I])\S*\s+-\s?(\d{2,3}[.,]\d)\s?DBM", txt.upper().replace("O", "0"))
                dbm = float(r.meter) if r.meter is not None else None
                if not m:
                    raise Refused(f"{n}: page A shows no 'S<n> -NNN.N DBM' line ({' '.join(txt.split())[:120]!r})")
                shown = -float(m.group(2).replace(",", "."))
                res[f"{n} smeter"] = (shown, dbm)
                if dbm is not None and abs(shown - dbm) > 3.0:
                    raise Refused(f"{n}: page A's S-meter says {shown} dBm, the remote's {dbm}")
            s.confirm(str({k: v for k, v in res.items() if k.endswith("smeter")}))
        return True, f"pages C, D, E, F, G and A's S-meter read right on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)



# ---- The main screen's pages as the radio lists them (radio-core-gen-pages-defaults, app-button-pages) ----

GEN_PAGES = {
    "VOL 1:2": ["Volume", "Squelch", "RF gain", "TX power"],
    "VOL 2:2": ["Brightness", "PRE", "ATT", "ATU"],
    "APP 1:6": ["Modes", "Cluster", "SWR Scan", "RX Recorder"],
    "APP 2:6": ["Settings", "CW Trainer", "Field Ops", "Channels"],
}


def _pages(r):
    """{page name: [its buttons]} from the remote's buttons? (button_bar.h buttons_layout_text)"""
    n0 = len(r.lines)
    r.send("buttons?")
    end = time.time() + 4
    while time.time() < end:
        for _, t in list(r.lines)[n0:]:
            if t.startswith("buttons "):
                out = {}
                for part in t[len("buttons "):].split("|"):
                    bits = [b.strip() for b in part.split(";")]
                    if bits and bits[0].startswith("("):
                        out[bits[0].strip("()")] = [b.lstrip("~") for b in bits[1:] if b]
                return out
        time.sleep(0.1)
    raise Refused(f"{r.name}: no answer to buttons?")


def t_gen_pages(o):
    """Each radio's main-screen pages as its remote lists them (buttons?): VOL 1:2 Volume, Squelch, RF gain, TX power;
    VOL 2:2 Brightness, PRE, ATT, ATU; MFK 1:4 to 4:4 there; APP 1:6 to 6:6, the first two Modes, Cluster, SWR Scan,
    RX Recorder and Settings, CW Trainer, Field Ops, Channels"""
    res = {}
    with o.step("buttons? on both: the GEN pages' defaults, MFK 1:4-4:4, APP 1:6-6:6") as s:
        for n, r in _both(o):
            pages = _pages(r)
            res[n] = {k: v for k, v in pages.items() if k in GEN_PAGES}
            for page, want in GEN_PAGES.items():
                if pages.get(page, [])[:len(want)] != want:
                    return False, f"{n}: {page} lists {pages.get(page)}, not {want}", res
            miss = [p for p in [f"MFK {i}:4" for i in range(1, 5)] + [f"APP {i}:6" for i in range(1, 7)]
                    if p not in pages]
            if miss:
                return False, f"{n}: no page {miss} in {list(pages)}", res
        s.confirm(str(res[H.A]))
    return True, f"both list the GEN pages' defaults, MFK 1:4-4:4 and APP 1:6-6:6: {res[H.A]}", res


# ---- What needs control and what does not (control-stop-needs-none, control-injected-keys-keep) ----

def t_control_rules(o):
    """On the X6100, a second connection of the bench's own session (operate, not holding control): 'set freq' is
    refused ('controlled by ... take control first'), 'ft8 stop' is not (stopping the apps' sending needs operate
    only). Then, with the FT8 app open and decoding, sixty main-knob detents sent by the remote (thirty each way): the
    bench keeps control all along and the GUI's log has no 'control: to the radio's operator' line"""
    from remote_ws import Remote
    r = o.r[H.A]
    res, c = {}, None
    try:
        with o.step("x6100: a second connection without control: set freq refused, ft8 stop taken") as s:
            r.take_control()
            c = Remote(r.host, 50002, timeout=8)
            c.send(f"auth {r._tok}")
            if not _until(c, lambda t: t.startswith("role "), 5):
                raise Refused("x6100: the second connection got no role")
            c.send(f"set freq {int(r.state.get('freq', 14200000)) + 1000}")
            ans = _until(c, lambda t: t.startswith("err"), 4)
            if not ans or "take control first" not in ans:
                raise Refused(f"x6100: 'set freq' without control answered {ans!r}")
            res["set_freq"] = ans
            c.send("ft8 stop")
            bad = _until(c, lambda t: t.startswith("err"), 2.5)
            if bad:
                raise Refused(f"x6100: 'ft8 stop' without control answered {bad!r}")
            res["ft8_stop"] = "no error"
            s.confirm(f"{ans}; ft8 stop taken")
        with o.step("x6100: FT8 decoding, sixty detents from the remote: control kept, no claim at the radio") as s:
            oa._ft8_open(o)
            n0 = int(oa.ssh(r, "grep -c \"control: to the radio's operator\" /tmp/x6100_log.txt || true").strip() or 0)
            r.take_control()
            o.changing += 1
            try:
                for i in range(30):
                    r.send("knob main 1")
                    time.sleep(0.15)
                    r.send("knob main -1")
                    time.sleep(0.15)
                    if r.ctl_who == "local":
                        raise Refused(f"x6100: control went to the radio's operator after {2 * i + 2} detents")
            finally:
                o.changing -= 1
            time.sleep(1.0)
            n1 = int(oa.ssh(r, "grep -c \"control: to the radio's operator\" /tmp/x6100_log.txt || true").strip() or 0)
            if not r.ctl_mine or n1 != n0:
                raise Refused(f"x6100: after the detents control is {r.ctl_who} (mine {r.ctl_mine}), the log's claims "
                              f"{n0} -> {n1}")
            res["detents"] = f"60, control kept ({r.ctl_who})"
            s.confirm(res["detents"])
        return True, f"set freq needs control, ft8 stop does not; the remote's detents under FT8 keep control: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        if c is not None:
            c.close()
        oa._ft8_close(o)
        oa.home_both(o)


# ---- A transverter's band and its power limit (transverter-waterfall-band, transverter-limit-caps-pwr) ----

XV = {"transverter_0_from": 144000000, "transverter_0_to": 148000000, "transverter_0_shift": 116000000,
      "transverter_0_pwr_max": 0.5}


def t_xvtr(o):
    """A 2 m transverter on both (144-148 MHz through 28 MHz, its power limit 0.5 W): the dial on 144.300 USB, the
    power the radio would send held at the limit (5 W asked, 0.5 read), the classic screen's scale reading the band
    tuned (144.3), not the IF; back on 20 m the power as asked; the transverter put back as found. Nothing keyed"""
    import orch_cable as oc
    res = {}
    try:
        with oa.cfg_kept(o.r[H.A], XV), oa.cfg_kept(o.r[H.B], XV):
            with o.step("both on 144.300 USB through the transverter, 5 W asked: 0.5 W held") as s:
                o.changing += 1
                try:
                    for n, r in _both(o):
                        oc.classic(r)
                        bench.ensure(r, {"freq": 144300000, "mode": 2})
                        r.send("set pwr 50")        # 5 W asked (tenths): the band's limit holds it at 0.5
                finally:
                    o.changing -= 1
                for n, r in _both(o):
                    try:
                        r.wait("0.5 W held", lambda r=r: _pwr(r) <= 0.5, 4)
                    except Refused:
                        raise Refused(f"{n}: on the limited transverter band the power reads {_pwr(r)} W, not 0.5")
                    res[f"{n} pwr"] = _pwr(r)
                s.confirm(str(res))
            with o.step("both: the classic screen's scale reads 144.3, the band tuned") as s:
                for n, r in _both(o):
                    p = oa.shot(o, r, "transverter scale")
                    txt = oa.ocr(p, psm=11) + oa.ocr(p, box=(0, 100, 800, 330), psm=6, stretch=True)
                    if not re.search(r"144[.,]?[23]", txt):
                        raise Refused(f"{n}: the classic screen reads no 144.3 on the transverter's band "
                                      f"({' '.join(txt.split())[:120]!r})")
                    res[f"{n} scale"] = "144.3"
                s.confirm("144.3 on both")
            with o.step("both back on 14.200 USB: the power as asked again") as s:
                o.changing += 1
                try:
                    for n, r in _both(o):
                        bench.ensure(r, {"freq": 14200000, "mode": 2, "pwr": H.W})
                finally:
                    o.changing -= 1
                s.confirm("14.2 USB")
        return True, f"the transverter's band held the power at its limit and showed the band tuned: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        o.changing += 1
        try:
            for n, r in _both(o):
                try:
                    bench.ensure(r, {"freq": 14200000, "mode": 2, "pwr": H.W})
                except Refused as e:
                    o.st.ev(f"{n}: not back on 20 m: {e}", "bad")
        finally:
            o.changing -= 1
        oa.home_both(o)


# ---- CW's Train outside the bands (cw-train-out-of-band) ----

def t_train_oob(o):
    """On each radio, CW on 15.000 MHz (outside the bands) with KEY Train on: the remote's CW text is refused as any
    CW there ('No TX on 15.000 MHz'), the radio never keyed; KEY Train and the dial put back"""
    res = {}
    try:
        for n, r in _both(o):
            with o.step(f"{n}: CW on 15.000 MHz, KEY Train on: cwsend refused, nothing keyed") as s:
                with oa.cfg_kept(r, {"key_train": 1}):
                    o.changing += 1
                    try:
                        bench.ensure(r, {"freq": 15000000, "mode": 4})
                        t0 = time.time()
                        r.send("cwsend E")
                        said = oa.wait_msg(r, t0, r"No (TX|CW) on 15\.000", 5)
                        time.sleep(1.0)
                        if any(on for t, on in r.tx_hist if t >= t0):
                            raise Refused(f"{n}: the radio keyed for Train outside the bands")
                    finally:
                        bench.ensure(r, {"freq": 14200000, "mode": 2})
                        o.changing -= 1
                res[n] = said
                s.confirm(said)
        return True, f"Train outside the bands refused as any CW on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


COVERS = {
    "ft8-nocall": (["ft8-nocall", "ft8-tx-off-dry-run"], []),
    "x6200-start": (["register-32-x6200", "pa-bias-x6200", "tx-x6200-base-band-table"], []),
    "tx-fingerprint": (["tx-calibration-fingerprint"], []),
    "app-power-cap": (["digi-app-power-cap-7w", "digital-app-power-cap", "digi-app-power-button"], []),
    "remote-pairing": (["pair-code-request", "pair-with-code", "pair-rate-limit", "pair-mirror-pause"],
                       ["role-admin-first"]),
    "gen-pages": (["radio-core-gen-pages-defaults", "app-button-pages"], []),
    "control-rules": (["control-stop-needs-none", "control-injected-keys-keep"], []),
    "xvtr-limit": (["transverter-limit-caps-pwr", "transverter-waterfall-band"], []),
    "train-oob": (["cw-train-out-of-band"], []),
    "sentry-pages-text": (["c-log-columns-colours", "d-bands-list", "e-decode-list", "f-system-cpu-gpu-ram",
                           "g-channel-meters", "g-state-lines", "sentry-a-smeter"], []),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("ft8-nocall", "both RX", "FT8 with no callsign: nocall, 'Call sign required'; TX Call on and off: 'TX "
             "off: receive only ...'; nothing keyed", t_ft8_nocall, 50),
        Test("x6200-start", "both", "the X6200's GUI log: register 32 = 0x0a, its own PA bias from the base's "
             "calibration block, the base's band table; none of it on the X6100", t_x6200_start, 10),
        Test("tx-fingerprint", "both", "each radio's fingerprint the same from x6x00-fingerprint and its GUI, the two "
             "radios' different (TX calibration belongs to its radio)", t_fingerprint, 15),
        Test("app-power-cap", "both RX", "FT8 opened at 8 W: 7 W and said; its Power button steps 0.1 to 7 W round; "
             "closed: 8 W again; nothing keyed", t_app_power, 60),
        Test("remote-pairing", "X6100", "a new browser's pair?: a code on the screen, a second paused, a viewer's "
             "mirror stopped meanwhile; paired as an operator with TX; its token a session again; unpaired after",
             t_pairing, 40),
        Test("gen-pages", "both", "the main screen's pages as the remote lists them: VOL 1:2/2:2 defaults, MFK 1:4-4:4, "
             "APP 1:6-6:6 with the first two pages' apps", t_gen_pages, 10),
        Test("control-rules", "X6100", "set freq needs control, ft8 stop does not; sixty remote detents under FT8's "
             "decoding keep control (no claim at the radio in the log)", t_control_rules, 50),
        Test("xvtr-limit", "both RX", "a 2 m transverter with a 0.5 W limit: 144.300 holds the power at 0.5 W and the "
             "classic screen's scale reads 144.3; nothing keyed", t_xvtr, 40),
        Test("train-oob", "both RX", "CW Train outside the bands (15.000 MHz): the remote's CW refused as any CW, "
             "nothing keyed", t_train_oob, 30),
        Test("sentry-pages-text", "both RX", "pages C, D, E, F, G read off the screen (their columns, bars and "
             "radio), page A's S-meter line against the remote's", t_sentry_pages, 60),
    ]
