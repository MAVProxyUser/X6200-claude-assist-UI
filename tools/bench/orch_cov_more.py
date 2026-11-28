"""More features no test covered (2026-11-27, the coverage report's eighth batch, each needing nothing but the two
radios): the web Network page's checks on a static Ethernet address, the VOL knob's press, and the direction finder's
F2 ARRAY and F5 SET panels on page I. Judged from the web page's answers, the remote's state and a picture of the
screen read with tesseract; nothing kept changed, nothing transmitted. Every test ends with both radios on the
Sentry's page A (home_both).

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import re
import time

import bench
from bench import Refused
import orch_cov_apps as oa

H = None                        # the running orch module (orch.py: orch_cov_more.H = sys.modules[__name__])


def _both(o):
    return [(H.A, o.r[H.A]), (H.B, o.r[H.B])]


# ---- The web Network page's checks on a static address (ethernet-static-validation, ethernet-no-adapter) ----

# (what is sent besides dhcp false, a word of the reason the GUI gives: src/wifi.cpp eth_check_field, eth_check)
ETH_BAD = [
    ({"ip": "10.1.2", "mask": "255.255.255.0"}, "IP address is not valid"),
    ({"ip": "127.0.0.5", "mask": "/8"}, "IP address is not valid"),
    ({"ip": "10.1.2.3", "mask": "255.0.255.0"}, "Netmask is not valid"),
    ({"ip": "10.1.2.3", "mask": "/33"}, "Netmask is not valid"),
    ({"ip": "10.1.2.0", "mask": "/24"}, "network or broadcast"),
    ({"ip": "10.1.2.255", "mask": "255.255.255.0"}, "network or broadcast"),
    ({"ip": "10.1.2.3", "mask": "/24", "gw": "10.1.3.1"}, "gateway is not in the subnet"),
    ({"ip": "10.1.2.3", "mask": "/24", "gw": "10.1.2.3"}, "gateway is not in the subnet"),
    ({"ip": "10.1.2.3", "mask": "/24", "dns": "300.1.1.1"}, "DNS is not valid"),
    ({"ip": "10.1.2.3"}, "needs an IP address and a netmask"),
]


def t_eth_validation(o):
    """On each radio, the web Network page (POST /api/network) with a static address it must refuse (ETH_BAD: not a
    dotted quad, loopback, a bad netmask, the network or broadcast address, a gateway outside the subnet or the
    address itself, a bad DNS, no netmask): each answered 409 with the GUI's reason, and GET /api/network the same
    after as before (no adapter here: 'no adapter', present 0). Only refused settings are sent; one taken would be
    put back to DHCP at once and the test fails"""
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: the Network page refuses {len(ETH_BAD)} bad static addresses, each with its reason") \
                    as s:
                st, before = oa.http_json(r, "/api/network")
                if st != 200 or not isinstance(before, dict):
                    raise Refused(f"{n}: GET /api/network {st} ({str(before)[:80]})")
                out["state"] = before.get("state")
                bad = []
                for fields, why in ETH_BAD:
                    body = {"dhcp": False, "ip": "", "mask": "", "gw": "", "dns": ""}
                    body.update(fields)
                    st, ans = oa.http_json(r, "/api/network", "POST", body)
                    said = ans.get("msg", "") if isinstance(ans, dict) else str(ans)     # api_error's {"msg"}
                    if st == 200:
                        oa.http_json(r, "/api/network", "POST", {"dhcp": True, "ip": "", "mask": "", "gw": "",
                                                                 "dns": ""})
                        raise Refused(f"{n}: the Network page took {fields} (put back to DHCP)")
                    if st != 409 or why.lower() not in str(said).lower():
                        bad.append(f"{fields} -> {st} {str(said)[:60]!r}")
                st, after = oa.http_json(r, "/api/network")
                if bad:
                    raise Refused(f"{n}: refusals without the reason: {bad}")
                if after != before:
                    raise Refused(f"{n}: GET /api/network changed: {before} -> {after}")
                if before.get("present") or before.get("state") != "no adapter":
                    out["note"] = f"an adapter is here ({before.get('state')}): its card not judged"
                s.confirm(f"{len(ETH_BAD)} refused with their reasons; {before.get('state')}, unchanged")
        return True, f"the Network page's checks on both: {res}", res
    except Refused as e:
        return False, str(e), res


# ---- The VOL knob's press (input-knob-press-choose) ----

def _vol_line(o, r, label):
    """The first knob line's control name as read ('VOLUME', 'SQUELCH', ...), or '': the knob lines over the buttons,
    VOL's the first ('Volume: 0')"""
    import os
    from PIL import Image
    p = oa.shot(o, r, label)
    if not p:
        return ""
    reads = [lambda: oa.ocr(p, box=(0, 368, 330, 394), psm=7, scale=3)]
    for thr in (70, 40):                        # drawn dim over the X6200's busier waterfall: read '' plain
        def lifted(thr=thr):
            q = p + ".lift.png"
            Image.open(p).convert("L").crop((0, 368, 200, 394)).point(lambda v: 255 if v > thr else 0).save(q)
            try:
                return oa.ocr(q, psm=7, scale=3)
            finally:
                os.remove(q)
        reads.append(lifted)
    for read in reads:
        m = re.search(r"([A-Z]{3,14})\s*:", read().upper())    # the word before the colon: the arrow reads a letter
        if m:
            return m.group(1)
    return ""


def _is_volume(name):
    """Whether a knob line's name read is VOLUME: dim over the X6200's waterfall it read 'NOTUME' (2026-11-27)"""
    import difflib
    return difflib.SequenceMatcher(None, name, "VOLUME").ratio() >= 0.6


def t_knob_press(o):
    """On each radio's classic screen, Knob info on: VOL pressed (Knob press: next control off, the default) chooses
    the control: a turn then takes VOL to its next one (the knob line names another, the volume unchanged), the turn
    back to Volume; pressed again it changes the value (a turn moves the volume, turned back). With Knob press: next
    control on, a short press takes the next control at once (the volume unchanged); put back with the press off,
    choosing and turning back to Volume. Settings as found"""
    import orch_cable as oc
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: the classic screen") as s:
                oc.classic(r)
                time.sleep(1.5)
                s.confirm("classic")
            with o.step(f"{n}: VOL pressed chooses the control, a turn takes the next, pressed again changes the "
                        f"value") as s:
                with oa.cfg_kept(r, {"knob_info": 1, "knob_press_next": 0}):
                    time.sleep(1.0)
                    r.take_control()
                    name0, v0 = _vol_line(o, r, "knob line as found"), int(r.state.get("vol") or 0)
                    if not _is_volume(name0):
                        raise Refused(f"{n}: VOL's knob line names {name0!r}, not VOLUME, as found")
                    try:
                        bench.tap(r, "VOL")             # choosing
                        time.sleep(0.6)
                        r.send("knob vol 1")
                        time.sleep(0.8)
                        name1, v1 = _vol_line(o, r, "knob line chose next"), int(r.state.get("vol") or 0)
                        r.send("knob vol -1")           # back to Volume
                        time.sleep(0.8)
                        name2 = _vol_line(o, r, "knob line chose back")
                    finally:
                        bench.tap(r, "VOL")             # changing again
                        time.sleep(0.6)
                    step = 1 if v0 < 5 else -1
                    r.send(f"knob vol {step}")
                    time.sleep(0.8)
                    v3 = int(r.state.get("vol") or 0)
                    r.send(f"knob vol {-step}")
                    time.sleep(0.8)
                    v4 = int(r.state.get("vol") or 0)
                out["choose"] = {"next": name1, "volume": [v0, v1, v3, v4], "back": name2}
                if not name1 or _is_volume(name1) or v1 != v0:
                    raise Refused(f"{n}: choosing, a turn named {name1!r} with the volume {v0} -> {v1}")
                if not _is_volume(name2):
                    raise Refused(f"{n}: choosing, the turn back named {name2!r}, not VOLUME")
                if v3 != v0 + step or v4 != v0:
                    raise Refused(f"{n}: changing again, the volume {v0} -> {v3} -> {v4} for {step:+d} and back")
                s.confirm(f"VOLUME -> {name1} (volume {v0} kept) -> VOLUME; then volume {v0} -> {v3} -> {v4}")
            with o.step(f"{n}: Knob press: next control on: a short press takes the next control") as s:
                v0 = int(r.state.get("vol") or 0)
                try:
                    with oa.cfg_kept(r, {"knob_info": 1, "knob_press_next": 1}):
                        time.sleep(1.0)
                        bench.tap(r, "VOL")
                        time.sleep(1.0)                 # a held press is told apart from a short one first
                        name5, v5 = _vol_line(o, r, "knob line next control"), int(r.state.get("vol") or 0)
                finally:
                    with oa.cfg_kept(r, {"knob_press_next": 0}):
                        time.sleep(0.6)
                        if not _is_volume(_vol_line(o, r, "knob line before put back")):
                            bench.tap(r, "VOL")         # choosing
                            time.sleep(0.6)
                            r.send("knob vol -1")
                            time.sleep(0.8)
                            bench.tap(r, "VOL")         # changing
                            time.sleep(0.6)
                    name6 = _vol_line(o, r, "knob line put back")
                out["next"] = {"after press": name5, "volume": [v0, v5], "put back": name6}
                if not name5 or _is_volume(name5) or v5 != v0:
                    raise Refused(f"{n}: Knob press: next control: a press named {name5!r}, the volume {v0} -> {v5}")
                if not _is_volume(name6):
                    raise Refused(f"{n}: VOL left on {name6!r} after the test")
                s.confirm(f"a press took {name5} (volume {v0} kept); put back on VOLUME")
        return True, f"the VOL knob's press on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- Page I's F2 ARRAY and F5 SET panels (df-f2-array, df-f5-set, df-daemon-lifecycle) ----

PANEL_BOX = (60, 55, 740, 470)  # the panel over page I (app_df.c: PANEL_W wide, from y 60, its footer at 448)
# The panel's footer: page I's own boxes say ARRAY AGAINST THE BAND and ANTENNAS too, so closed is judged by it alone
PANEL_FOOT = r"CHANGE\s*IT|DO\s*IT\s*/\s*CLOSE|MFK\W+A\s*ROW"
ARRAY_WORDS = ["BAND", "ARRAY", "HEADING", "ANTENNAS"]      # and RADIUS, SPACING, UNIT or LAYOUT
SET_WORDS = ["RATE", "GAIN", "SQUELCH", "CALIBRATE", "SOURCE"]


def _panel_words(o, r, label, words):
    """The panel's words read, whether its footer is there, and the text"""
    p = oa.shot(o, r, label)
    t = (oa.ocr(p, box=PANEL_BOX, psm=6, scale=2) + "\n" + oa.ocr(p, box=PANEL_BOX, psm=11, scale=2)).upper()
    return [w for w in words if w in t], re.search(PANEL_FOOT, t) is not None, t


def t_df_panels(o):
    """On each radio, the Sentry's page I (the direction finder, no Kraken here): x6x00-df running while it is open;
    F2 opens the ARRAY panel (BAND, ARRAY, the size row: RADIUS, SPACING, UNIT or LAYOUT, HEADING, ANTENNAS read), F2
    again closes it; F5 opens the SET panel (RATE, GAIN, SQUELCH, CALIBRATE, SOURCE), F5 again closes it. F3 and F4
    (method, calibrate) never pressed. Back on page A, x6x00-df ends within 30 s"""
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: page I: the direction finder's daemon runs") as s:
                # page I is a window of its own (ACTION_APP_DF), not one of the Sentry's pages: the remote's win names
                # it ('Direction finder'), its pg is empty
                r.take_control()
                r.send("open sentry I")
                r.wait("page I (the direction finder)", lambda: oa.win(r).lower().startswith("direction"), 10)
                time.sleep(3.0)
                pid = oa.ssh(r, "pidof x6x00-df; true").strip()
                out["df running"] = bool(pid)
                if not pid:
                    raise Refused(f"{n}: page I open, no x6x00-df running")
                s.confirm(f"x6x00-df {pid}")
            for key, words, size in (("F2", ARRAY_WORDS, True), ("F5", SET_WORDS, False)):
                with o.step(f"{n}: {key} opens its panel ({', '.join(words)}), {key} again closes it") as s:
                    r.take_control()
                    bench.tap(r, key)
                    time.sleep(1.2)
                    seen, foot, t = _panel_words(o, r, f"df {key} panel", words)
                    has_size = bool(re.search(r"RADIUS|SPACING|UNIT|LAYOUT", t)) if size else True
                    bench.tap(r, key)
                    time.sleep(1.2)
                    _, foot_after, _ = _panel_words(o, r, f"df {key} closed", words)
                    out[key] = {"read": seen, "footer": foot, "footer after closing": foot_after}
                    if len(seen) < len(words) - 1 or not has_size or not foot:
                        raise Refused(f"{n}: {key}'s panel read {seen} of {words}"
                                      f"{'' if has_size else ', no size row'}{'' if foot else ', no footer'} "
                                      f"({' '.join(t.split())[:100]!r})")
                    if foot_after:
                        raise Refused(f"{n}: {key} again left the panel up (its footer still read)")
                    s.confirm(f"read {seen}; closed")
            with o.step(f"{n}: back on page A: x6x00-df ends") as s:
                bench.open_page(r, "A")
                end, pid = time.time() + 30, "?"
                while time.time() < end:
                    pid = oa.ssh(r, "pidof x6x00-df; true").strip()
                    if not pid:
                        break
                    time.sleep(2.0)
                out["df after"] = pid or "ended"
                if pid:
                    raise Refused(f"{n}: x6x00-df still running 30 s after leaving page I ({pid})")
                s.confirm("ended")
        return True, f"page I's panels on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- The themes, switched live (theme-vision-layout, theme-black-metal-layout, theme-button-text) ----

# A theme's own marks on the classic screen, read: (box, a pattern each line of marks must match) (2026-11-27's
# pictures: VISION's S-meter row as bars under the dial; Black Metal's two state rows, its S/SWR/ALC scales, the other
# VFO and its buttons in capitals on one line)
THEME_MARKS = {
    "VISION": [((0, 118, 610, 172), r"\b9\s*\+20")],          # its +40 read 440
    "Black Metal": [((0, 0, 800, 55), r"STEP"), ((0, 60, 330, 155), r"SWR.*ALC|ALC"), ((640, 55, 800, 90), r"\bB\b"),
                    ((0, 428, 800, 480), r"\b(RIT|XIT|VOLUME|SQUELCH)\s*\+?\s*\d")],    # a name and its value on
    # one line, in capitals, whichever page the buttons are on ('RIT +0', 'VOLUME 0'); two-line themes read the names
    # first, then the values
    "Total Black": [((0, 0, 800, 55), r"STEP"), ((0, 60, 330, 155), r"SWR.*ALC|ALC")],
}


def t_themes(o):
    """On each radio's classic screen, every theme the web Display page offers set live (its theme, read back): the
    screen's top changes (15% or more of its pixels' colours, against the theme as found), the F keys do the same
    (fkeys? as before), and VISION, Black Metal and Total Black show their own layout's marks (THEME_MARKS, read).
    Back to the theme as found, the screen's top as it was"""
    import orch_cable as oc
    import suite_cover_a as ca
    from orch_cov_look import _rgb_diff
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: the classic screen, the theme as found") as s:
                oc.classic(r)
                time.sleep(1.5)
                found = ca.disp(r.host)
                themes = [t for t in found.get("themes", []) if t != found.get("theme")]
                if not found.get("theme") or not themes:
                    raise Refused(f"{n}: the Display page names no theme ({str(found)[:80]})")
                p0 = oa.shot(o, r, f"theme {found['theme']}")
                keys0 = oa.fkeys(r)[0]
                s.confirm(f"{found['theme']}; {len(themes)} others")
            try:
                for th in themes:
                    with o.step(f"{n}: theme {th}: the screen changes, the F keys the same"
                                f"{', its marks read' if th in THEME_MARKS else ''}") as s:
                        if ca.disp_set(r.host, "theme", th) != 200:
                            raise Refused(f"{n}: the Display page refused theme {th}")
                        time.sleep(3.5)
                        if ca.disp(r.host).get("theme") != th:
                            raise Refused(f"{n}: theme {th} not read back")
                        p = oa.shot(o, r, f"theme {th}")
                        # on the screen's top (no waterfall moving there): RF.Guru keeps the Sentry's layout, its
                        # colours and type its own (34% of the top, 18% of the whole screen, 2026-11-27)
                        changed = _rgb_diff(p0, p, (0, 0, 800, 118))
                        keys = oa.fkeys(r)[0]
                        marks = {}
                        for box, pat in THEME_MARKS.get(th, []):
                            t = " ".join(oa.ocr(p, box=box, psm=6, scale=2).upper().split())
                            marks[pat] = bool(re.search(pat, t))
                        out[th] = {"changed": round(changed, 2), "marks": marks}
                        if changed < 0.15:
                            raise Refused(f"{n}: theme {th} changed {changed:.0%} of the screen's top")
                        if keys != keys0:
                            raise Refused(f"{n}: theme {th}: the F keys {keys0} -> {keys}")
                        if not all(marks.values()):
                            raise Refused(f"{n}: theme {th}: its marks not read: {marks}")
                        s.confirm(f"{changed:.0%} changed{'; marks read' if marks else ''}")
            finally:
                ca.disp_set(r.host, "theme", found["theme"])
                time.sleep(3.5)
            with o.step(f"{n}: back to {found['theme']}: the screen's top as it was") as s:
                back = _rgb_diff(p0, oa.shot(o, r, f"theme {found['theme']} back"), (0, 0, 800, 118))
                out["back"] = round(back, 3)
                if ca.disp(r.host).get("theme") != found["theme"] or back > 0.03:
                    raise Refused(f"{n}: back on {found['theme']}, the top {back:.1%} off the first picture")
                s.confirm(f"top {back:.1%} off")
        return True, f"the themes on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- The settings file's recovery at start (settings-restore-damaged) ----

GOOD_DB = "/var/lib/x6100/params-good.db"   # params.db's last good copy, on the system partition (params/db.c)
# Compared by equality only (callsign and QTH never said): what the good copy and the live file must agree on
RECOVERY_KEYS = ["callsign", "qth", "theme_name", "wifi_enabled", "sentry_home", "first_start_done", "rtl_gain",
                 "smeter_color", "language"]
# The live files kept aside while the GUI is stopped, the file broken only once they are: random bytes are no SQLite
# database (SQLITE_NOTADB, file_damaged)
BREAK = r"""
d={d}; i=0; while pidof x6100_gui >/dev/null && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
mkdir -p $d && ls /mnt/params-unusable-* > $d/aside_before 2>/dev/null
cp -p /mnt/params.db $d/ && cp -p {good} $d/ \
  && {{ [ -e /mnt/params.db-wal ] && cp -p /mnt/params.db-wal $d/ || true; }} \
  && {{ [ -e /mnt/params.db-shm ] && cp -p /mnt/params.db-shm $d/ || true; }} && [ -s $d/params.db ] && sync \
  && {{ rm -f /mnt/params.db-wal /mnt/params.db-shm; head -c 86016 /dev/urandom > /mnt/params.db; sync; \
        echo BROKEN; }} \
  || echo BACKUP_FAILED
"""
PUT_BACK = r"""
d={d}; i=0; while pidof x6100_gui >/dev/null && [ $i -lt 40 ]; do sleep 0.5; i=$((i+1)); done
if [ -s $d/params.db ]; then
  rm -f /mnt/params.db-wal /mnt/params.db-shm
  cp -p $d/params.db /mnt/params.db && for x in wal shm; do [ -e $d/params.db-$x ] && cp -p $d/params.db-$x /mnt/; done
  cp -p $d/params-good.db {good}
  for f in /mnt/params-unusable-*; do [ -e "$f" ] && ! grep -qx "$f" $d/aside_before 2>/dev/null && rm -f "$f"; done
  sync; echo PUT_BACK
else echo NO_BACKUP; fi
"""


def _db_vals(r, path):
    """RECOVERY_KEYS' values in the settings database at path on r (sqlite3 on the radio), {key: value}"""
    q = "select name||'='||val from params where name in (" + ",".join(f"'{k}'" for k in RECOVERY_KEYS) + ")"
    out = oa.ssh(r, f'sqlite3 {path} "{q}" 2>/dev/null; true', 20)
    return dict(x.split("=", 1) for x in out.splitlines() if "=" in x)


def t_settings_recovery(o):
    """On each radio, one at a time (the owner's word, 2026-11-27): its last good copy of the settings checked sound
    and with WiFi on first (a start on the defaults turns these radios' WiFi off and cuts the bench off: those cases
    are not run). Then, its GUI stopped (planned): the live params.db and its journals and the good copy kept in
    /mnt/bench_backups, params.db replaced with random bytes, the GUI started: its log says the file was put aside
    and the last good copy loaded, the damaged file lies beside as params-unusable-<time>Z.db, and the settings agree
    with the good copy (RECOVERY_KEYS, compared, never said); its note on the screen recorded if still up. Then the
    GUI stopped again, the live files put back as they were, the test's put-aside files removed, started: the
    settings as before the test"""
    import orch_cov_ui as ou
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            d = f"/mnt/bench_backups/params_recovery_{time.strftime('%Y%m%d-%H%M%S')}"
            with o.step(f"{n}: the last good copy sound, WiFi on in it; the live settings read") as s:
                check = oa.ssh(r, f'sqlite3 {GOOD_DB} "pragma quick_check" 2>&1; true', 20).strip()
                good = _db_vals(r, GOOD_DB)
                live0 = _db_vals(r, "/mnt/params.db")
                if check != "ok" or good.get("wifi_enabled") != "1":
                    raise Refused(f"{n}: the good copy {'not sound (' + check[:40] + ')' if check != 'ok' else ''}"
                                  f"{'' if good.get('wifi_enabled') == '1' else ' has WiFi off'}: not run")
                if live0.get("wifi_enabled") != "1" or not live0:
                    raise Refused(f"{n}: the live settings not read ({len(live0)} keys): not run")
                out["good copy"] = f"sound, {len(good)} keys"
                aside0 = set(oa.ssh(r, "ls /mnt/params-unusable-*.db 2>/dev/null; true").split())
                s.confirm(f"good copy sound with WiFi on; {len(live0)} live keys read")
            broken = False
            try:
                with o.step(f"{n}: params.db broken across a GUI restart: put aside, the last good copy loaded") as s:
                    said = ou._restart(o, n, "the settings file's recovery: params.db broken, kept in "
                                             "/mnt/bench_backups", BREAK.format(d=d, good=GOOD_DB))
                    broken = "BROKEN" in said
                    if not broken:
                        raise Refused(f"{n}: the live files not kept aside ({said.strip()[-60:]!r}): nothing broken")
                    p = oa.shot(o, r, "settings recovered")
                    note = oa.ocr(p, psm=6, scale=2).upper()
                    log = oa.ssh(r, "grep 'params.db' /tmp/x6100_log.txt | tail -4; true")
                    # the lists compared here: busybox's grep -v with an empty -f file prints nothing (2026-11-27)
                    aside = " ".join(sorted(set(oa.ssh(r, "ls /mnt/params-unusable-*.db 2>/dev/null; true").split())
                                            - aside0))
                    now = _db_vals(r, "/mnt/params.db")
                    same = [k for k in good if now.get(k) == good[k]]
                    out["recovered"] = {"log": "put aside" in log and "last good copy" in log,
                                        "aside": len(aside.split()), "keys same as the good copy": f"{len(same)}/"
                                        f"{len(good)}", "note on the screen": "SETTINGS FILE DAMAGED" in note}
                    if "put aside" not in log or "last good copy" not in log:
                        raise Refused(f"{n}: the GUI's log says no recovery ({log.strip()[-120:]!r})")
                    if not aside.strip():
                        raise Refused(f"{n}: no params-unusable-*.db put aside")
                    if len(same) != len(good):
                        raise Refused(f"{n}: the settings agree with the good copy on {len(same)} of {len(good)} keys")
                    s.confirm(f"put aside ({aside.split()[0].rsplit('/', 1)[-1]}), the good copy loaded, "
                              f"{len(same)}/{len(good)} keys; note on screen: {out['recovered']['note on the screen']}")
            finally:
                if broken:
                    with o.step(f"{n}: the live settings put back as they were") as s:
                        said = ou._restart(o, n, "the settings file's recovery: the live params.db put back",
                                           PUT_BACK.format(d=d, good=GOOD_DB))
                        live1 = _db_vals(r, "/mnt/params.db")
                        same = [k for k in live0 if live1.get(k) == live0[k]]
                        out["put back"] = f"{len(same)}/{len(live0)} keys as before"
                        if "PUT_BACK" not in said or len(same) != len(live0):
                            raise Refused(f"{n}: putting back said {said.strip()[-40:]!r}; {len(same)} of "
                                          f"{len(live0)} keys as before (the backup: {d})")
                        s.confirm(out["put back"])
        return True, f"the settings file's recovery on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


COVERS = {
    "more-eth-validation": (["ethernet-static-validation"], ["ethernet-no-adapter"]),
    "more-knob-press": (["input-knob-press-choose"], []),
    "more-df-panels": (["df-f2-array", "df-f5-set"], ["df-daemon-lifecycle"]),
    "more-themes": ([], ["theme-vision-layout", "theme-black-metal-layout", "theme-button-text"]),
    "more-settings-recovery": (["settings-restore-damaged"], []),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("more-eth-validation", "both", "the web Network page refuses ten bad static addresses with the GUI's "
             "reasons, nothing changed", t_eth_validation, 20),
        Test("more-knob-press", "both", "VOL pressed chooses its control (a turn takes the next, the volume kept), "
             "pressed again changes the value; Knob press: next control takes the next at once", t_knob_press, 60),
        Test("more-df-panels", "both", "page I: x6x00-df runs; F2 ARRAY's and F5 SET's panels open and close; the "
             "daemon ends after", t_df_panels, 70),
        Test("more-themes", "both", "every theme set live changes the classic screen, the F keys the same; VISION's "
             "and Black Metal's layouts read; back as found", t_themes, 150),
        Test("more-settings-recovery", "both, one at a time", "params.db broken across a GUI restart: put aside, the "
             "last good copy loaded; the live settings put back after (the owner's word, 2026-11-27)",
             t_settings_recovery, 240),
    ]
