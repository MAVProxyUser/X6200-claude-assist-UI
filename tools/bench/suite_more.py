#!/usr/bin/env python3
"""More of the two-radio suite (suite.py run rx|control|look|safety|paths): one test per feature, run on each radio and
compared 1:1, each naming the features of features.json it covers; suite.py's report adds the coverage of every
feature, covered or not (the owner, 2026-11-22: "a comprehensive set of unit tests to cover EVERYTHING 1:1 on both
radios ... EVERYTHING needs validation"). Every test works through bench.py's state layer: each radio said whole and
read back (ensure), the path proved before a transmission is measured (path_proof), every transmission watched, and
each radio put back and left on the Sentry's page A (finish).

    rx        no RF: every receiver setting the remote sets, set and read back; a band's own memory; D's LISTEN and E's
              OPEN setting ATT, PRE and AGC as their rule says (rx_setup), on every band and every type
    control   no RF: TCI (the VFO, the mode, a query), Hamlib's NET rigctl on 4532 (frequency, mode, VFO, its PTT
              refused, its identity), every page of the web server
    safety    the cable, 0.1 W at most: the transmitter locked in the Sentry till A/B arms it, the browser PTT's lease
              ending an over, PTT refused outside the amateur bands, a receive-only session's PTT refused
    paths     the cable, 0.1 W: the path proof on every amateur band both ways, tones USB and LSB both ways, the mic
              on G both ways (acoustic.py), RTTY and PSK31 into the other radio's text log (digimodes.py), CW into its
              decoder (tci_cw_tx.py), AM and FM both ways on G (radios-walkie)
    look      every Settings tab on both, photographed side by side and compared pixel by pixel
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import json
import os
import socket
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from remote_ws import Remote  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
F_TEST = 14200000               # the receiver tests' dial (20 m, amateur, both radios tune it)


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def per_radio(run, batch, test, fn, covers, want_tx=False):
    """fn(r) on each radio (its own session, snapshot before, finish after: put back, page A): (ok, says) each, a
    result per radio and one for the pair (1:1: both pass, or both the same refusal)"""
    out = {}
    for n in run.names:
        r = None
        try:
            r = bench.Radio(n, run.c["radios"][n], want_tx)
            r.take_control()
            snap = bench.snapshot(r)
            rep = {}
            try:
                bench.open_page(r, "G")             # the Walkie keeps the dial where it is put (A's scan retunes)
                ok, says = fn(r)
            except bench.Refused as e:
                ok, says = False, f"refused: {e}" + (f" (the radio said: {r.errs[-3:]})" if r.errs else "")
            finally:
                bench.finish([r], {n: snap}, report=rep)
            if rep.get("not_restored"):
                says += f" (NOT put back: {rep['not_restored'][n]})"
                ok = False if ok else ok
        except (bench.Refused, OSError) as e:
            ok, says = False, f"no session: {e}"
            if r:
                r.close()
        out[n] = (ok, says)
        run.result(batch, f"{test} {n}", ok, says, radio=n, covers=covers)
    if len(out) == 2:
        (a, (oa, sa)), (b, (ob, sb)) = out.items()
        run.result(batch, f"{test} 1:1", oa is True and ob is True, f"{a} {'pass' if oa else 'FAIL'}, "
                   f"{b} {'pass' if ob else 'FAIL'}", covers=covers)
    return out


# ---- rx: the receiver's settings ----

RX_KEYS = [                     # (key, values tried, the features)
    ("att", [1, 0], ["panel-rx-switches", "radio-core-pre-att-per-band"]),
    ("pre", [1, 0], ["panel-rx-switches", "radio-core-pre-att-per-band"]),
    ("agc", [0, 1, 2, 3], ["panel-rx-switches", "tci-agc-mode"]),
    ("nb", [1, 0], ["panel-rx-switches"]),
    ("nr", [1, 0], ["panel-rx-switches"]),
    ("sql", [20, 0], ["panel-levels"]),
    ("rfg", [50, 100], ["panel-levels"]),
    ("vol", [5, 15], ["panel-levels"]),
    ("zoom", [2, 1], ["panel-set-step-zoom"]),
    ("filter", [(300, 2400), (100, 3000)], ["panel-filter-ssb-dig", "panel-filter-limits"]),
    ("rit", [200, 0], ["tci-rit"]),
    ("xit", [200, 0], []),
    ("split", [1, 0], ["panel-set-vfo"]),
    ("vfo", [1, 0], ["panel-set-vfo"]),
]
MODES = [(0, "LSB"), (1, "LSB-D"), (2, "USB"), (3, "USB-D"), (4, "CW"), (5, "CW-R"), (6, "AM"), (7, "NFM")]


def t_rx_keys(run):
    for key, vals, covers in RX_KEYS:
        def fn(r, key=key, vals=vals):
            bench.ensure(r, {"freq": F_TEST, "mode": 2})
            got = []
            for v in vals:
                got.append(bench.ensure(r, {key: v})[key])
            return True, f"{key}: " + " -> ".join(str(g) for g in got) + " (each read back)"
        per_radio(run, "rx", f"set-{key}", fn, covers)

    def modes(r):
        ok_, said = True, []
        for m, name in MODES:
            try:
                bench.ensure(r, {"freq": F_TEST, "mode": m})
                said.append(name)
            except bench.Refused as e:
                if "not on this radio" in str(e):
                    said.append(f"{name} (not on it)")
                else:
                    ok_ = False
                    said.append(f"{name} FAILED")
        return ok_, "modes " + ", ".join(said)
    per_radio(run, "rx", "set-mode", modes, ["panel-set-mode", "tci-modulation"])

    def band_memory(r):
        bench.ensure(r, {"freq": 14200000, "mode": 2, "att": 1, "pre": 0})
        bench.ensure(r, {"freq": 21300000})
        other = (r.state.get("att"), r.state.get("pre"))
        bench.ensure(r, {"freq": 14200000})
        time.sleep(0.6)
        back = (int(r.state.get("att")), int(r.state.get("pre")), int(r.state.get("mode")))
        return back == (1, 0, 2), (f"20 m set ATT 1 PRE 0 USB, 15 m had its own (ATT {other[0]} PRE {other[1]}), "
                                   f"back on 20 m: ATT {back[0]} PRE {back[1]} mode {back[2]}")
    per_radio(run, "rx", "band-memory", band_memory, ["radio-core-pre-att-per-band", "radio-core-band-last-frequency"])


D_BANDS = 43                    # D's list (app_sentry.c bands[], 2026-11-22)


def rule_of(freq, cw_or_prop=False, listening=False):
    """D's and E's rule (app_sentry.c rx_setup_on): PRE on from 8 MHz but on AM and FM broadcast, ATT off; AGC AUTO
    for D's listening, FAST for CW and Propagation, SLOW for a decoder app"""
    k = freq // 1000
    bc = 530 <= k <= 1700 or 87500 <= k <= 108000
    return {"att": 0, "pre": 0 if (bc or freq < 8000000) else 1, "agc": 3 if listening else 2 if cw_or_prop else 1}


def t_d_rule(run):
    def fn(r):
        seen, bad, n = [], [], 0
        for i in range(D_BANDS + 2):            # round D's list once and a little (a band twice is checked once)
            bench.open_page(r, "D")
            time.sleep(1.0)
            if i:
                r.send("knob mfk 1")
                time.sleep(1.0)
            bench.tap(r, "F2")
            time.sleep(2.5)
            f = int(r.state.get("freq", 0))
            if r.state.get("pg") != "A":            # an SDR's band (B), AIRSPACE: not the radio's receiver
                continue
            if f in seen:
                continue
            seen.append(f)
            want = rule_of(f, listening=True)
            have = {k: int(r.state.get(k, -1)) for k in want}
            n += 1
            if have != want:
                bad.append(f"{f} Hz {have} (rule {want})")
        return (n > 0 and not bad), f"{n} bands, each as the rule says" if not bad else "NOT: " + "; ".join(bad)
    per_radio(run, "rx", "d-listen-rule", fn, ["sentry-d-listen", "radio-core-pre-att-per-band"])


def t_e_rule(run):
    def fn(r):
        bad, n, skipped = [], 0, 0
        for i in range(18):
            bench.open_page(r, "E")
            time.sleep(1.0)
            bench.tap(r, "F4")
            time.sleep(1.2)
            f0 = int(r.state.get("freq", 0))
            bench.tap(r, "F2")
            time.sleep(3.0)
            f, mode = int(r.state.get("freq", 0)), int(r.state.get("mode", -1))
            what = r.state.get("app") or r.state.get("win") or ""
            if r.state.get("pg") in ("B", "H") or "irspace" in what or (f == f0 and what in ("", "Sentry")):
                skipped += 1                    # an SDR's type (none plugged in), AIRSPACE: not the radio's receiver
                bench.close_apps(r)
                continue
            want = rule_of(f, cw_or_prop=mode in (4, 5) or "Propag" in what)
            have = {k: int(r.state.get(k, -1)) for k in want}
            n += 1
            if have != want:
                bad.append(f"{what or 'page ' + str(r.state.get('pg'))} {f} Hz {have} (rule {want})")
            bench.close_apps(r)
            time.sleep(1.0)
        return n > 0 and not bad, (f"{n} types, each as the rule says ({skipped} not on its own receiver)" if not bad
                                   else "NOT: " + "; ".join(bad))
    per_radio(run, "rx", "e-open-rule", fn, ["e-open-ft8", "e-open-digital-apps", "e-open-wspr", "e-open-rtty-cw",
                                             "e-open-navtex-time-beacons"])


def t_g(run):
    """G's own keys, a key a second: the arrows a whole step up and back off a channel, CB's channels by them, F5
    through the group's modes, the dial held while armed (nothing keyed)"""
    def arrows(r):
        bench.ensure(r, {"freq": 14203000, "mode": 2})
        bench.tap(r, "F2")
        time.sleep(1.0)
        up = int(r.state.get("freq", 0))
        bench.tap(r, "F1")
        time.sleep(1.0)
        down = int(r.state.get("freq", 0))
        ok = up > 14203000 and down < up and up % 1000 == 0
        return ok, f"14.203 MHz: STEP + -> {up}, STEP - -> {down}"
    per_radio(run, "rx", "g-arrows", arrows, ["g-vfo-arrows"])

    def cb(r):
        bench.ensure(r, {"freq": 27185000, "mode": 6})
        time.sleep(1.0)
        bench.tap(r, "F2")
        time.sleep(1.0)
        up = int(r.state.get("freq", 0))
        bench.tap(r, "F1")
        time.sleep(1.0)
        back = int(r.state.get("freq", 0))
        return up == 27205000 and back == 27185000, f"CB 19 (27.185 AM): CH + -> {up} (CH 20 27205000), CH - -> {back}"
    per_radio(run, "rx", "g-cb-channels", cb, ["g-vfo-arrows", "g-groups-channels"])

    def f5(r):
        bench.ensure(r, {"freq": 27185000, "mode": 6})
        time.sleep(1.0)
        seq = [int(r.state.get("mode", -1))]
        for _ in range(3):
            bench.tap(r, "F5")
            time.sleep(1.2)
            seq.append(int(r.state.get("mode", -1)))
        return len(set(seq)) >= 3, f"CB 19, F5 three times: modes {seq}"
    per_radio(run, "rx", "g-f5-modes", f5, ["g-f5-modes"])

    def held(r):
        bench.ensure(r, {"freq": 14203000, "mode": 2})
        bench.arm(r, True, mode=2)
        bench.tap(r, "F2")
        time.sleep(1.0)
        f_armed = int(r.state.get("freq", 0))
        bench.arm(r, False)
        bench.tap(r, "F2")
        time.sleep(1.0)
        f_locked = int(r.state.get("freq", 0))
        return f_armed == 14203000 and f_locked != 14203000, (f"armed: STEP + left the dial at {f_armed}; locked again: "
                                                              f"it went to {f_locked}")
    per_radio(run, "rx", "g-dial-held", held, ["g-ab-arm", "sentry-ab-arm"], want_tx=True)


def rx(run, only):
    for name, fn in (("keys", t_rx_keys), ("g", t_g), ("d", t_d_rule), ("e", t_e_rule)):
        if not only or name in only:
            run.banner(f"SUITE rx: {name} (receive only, nothing sent)")
            fn(run)


# ---- control: TCI, rigctl, the web ----

def tci_session(host):
    t = Remote(host, 50001)
    greet, end = [], time.time() + 6
    while time.time() < end:
        m = t.recv(timeout=0.5)
        if m is None:
            continue
        greet.append(m)
        if m.startswith("ready"):
            break
    return t, greet


def tci_wait(t, prefix, secs=3.0):
    end = time.time() + secs
    while time.time() < end:
        m = t.recv(timeout=0.3)
        if m and m.lower().startswith(prefix):
            return m
    return None


def t_tci(run):
    def fn(r):
        t, greet = tci_session(r.host)
        try:
            dev = next((g for g in greet if g.lower().startswith("device:")), None)
            if not any(g.startswith("ready") for g in greet):
                return False, f"no TCI greeting ending in ready ({greet[:4]})"
            t.send("vfo:0,0,14074000;")
            r.wait("the dial from TCI", lambda: int(r.state.get("freq", 0)) == 14074000, 5)
            t.send("vfo:0,0;")
            q = tci_wait(t, "vfo:0,0,")
            t.send("modulation:0,usb;")
            r.wait("USB from TCI", lambda: int(r.state.get("mode", -1)) == 2, 5)
            return True, f"{dev}; vfo set and read ({q}); modulation set"
        finally:
            t.close()
    per_radio(run, "control", "tci", fn, ["tci-greeting", "tci-vfo", "tci-modulation", "tci-queries-no-control"])


def t_tci_more(run):
    """TCI's receiver commands, each read back on the remote's state: the filter, AGC, RIT on and off, the commands in
    capitals (TCI is case-insensitive), an unknown command answered by nothing and the session going on"""
    def fn(r):
        t, greet = tci_session(r.host)
        said = []
        try:
            bench.ensure(r, {"freq": F_TEST, "mode": 2, "rit": 0})
            for cmd, cond, what in (
                    ("rx_filter_band:0,300,2400;", lambda: (r.state.get("flo"), r.state.get("fhi")) == (300, 2400),
                     "filter 300-2400"),
                    ("agc_mode:0,fast;", lambda: r.state.get("agc") == 2, "AGC fast"),
                    ("agc_mode:0,slow;", lambda: r.state.get("agc") == 1, "AGC slow"),
                    ("rit_offset:0,200;", lambda: r.state.get("rit") == 200, "RIT +200"),
                    ("rit_enable:0,false;", lambda: r.state.get("rit") == 0, "RIT off"),
                    ("VFO:0,0,14100000;", lambda: int(r.state.get("freq", 0)) == 14100000, "VFO in capitals")):
                t.send(cmd)
                r.wait(what, cond, 5)
                said.append(what)
            t.send("no_such_command:0,1;")
            t.send("vfo:0,0;")
            back = tci_wait(t, "vfo:0,0,")
            if not back:
                return False, f"{', '.join(said)}; after an unknown command the session answered nothing"
            return True, ", ".join(said) + f"; an unknown command ignored, the session going on ({back})"
        finally:
            t.close()
    per_radio(run, "control", "tci-receiver", fn, ["tci-rx-filter-band", "tci-agc-mode", "tci-rit",
                                                   "tci-case-insensitive", "tci-unknown-silent"])


def t_remote(run):
    """The remote's own: a band step, the user's lock refusing a tune, an app that would transmit by opening refused,
    the apps list, the power held to the radio's own top (nothing keyed)"""
    def band(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        r.send("set band 1")
        r.wait("the next band", lambda: int(r.state.get("freq", 0)) != F_TEST, 6)
        return True, f"set band 1: {F_TEST} -> {r.state.get('freq')} Hz"
    per_radio(run, "control", "remote-band", band, ["panel-set-band", "radio-core-band-keys"])

    def lock(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        r.send("set lock 1")
        r.wait("locked", lambda: r.state.get("ulock") in (1, True), 5)
        n0 = len(r.errs)
        r.send("set freq 14100000")
        time.sleep(1.5)
        refused = int(r.state.get("freq", 0)) == F_TEST and any("locked" in e for e in r.errs[n0:])
        r.send("set lock 0")
        r.wait("unlocked", lambda: r.state.get("ulock") in (0, False), 5)
        return refused, f"locked, a tune {'refused (' + r.errs[-1] + ')' if refused else 'TAKEN'}; unlocked again"
    per_radio(run, "control", "remote-lock", lock, ["panel-user-lock"])

    def apps(r):
        r.send("apps")
        r.wait("the apps list", lambda: r.apps is not None, 5)
        wspr = next((a for a in r.apps if "WSPR" in a[1].upper()), None)
        n0 = len(r.errs)
        if wspr:
            r.send(f"open {wspr[0]}")
            time.sleep(1.5)
        refused = wspr is None or (any("open" in e for e in r.errs[n0:]) and r.state.get("app") != "WSPR")
        return len(r.apps) > 10 and refused, f"{len(r.apps)} apps; opening WSPR (it transmits by opening) " + \
            ("refused" if refused else "TAKEN")
    per_radio(run, "control", "remote-apps", apps, ["panel-apps-list", "panel-open-refused"])

    def power_cap(r):
        # its top: 10 W the X6100, 8 W the X6200; on its battery alone 5 W (radio.c supply_cb, BOARD_PWR_MAX_BAT)
        sup = bench.listener_gate_read(r)
        on_bat = sup is not None and sup["dc_in"] < 5.0
        top = 5.0 if on_bat else {"x6200": 8.0, "x6100": 10.0}.get(r.name, 10.0)
        r.send("set pwr 200")               # 20 W asked
        time.sleep(1.5)
        got = r.pwr()
        return abs(got - top) < 0.05, (f"20 W asked: the setting reads {got} W (its top {top} W, "
                                       f"{'on its battery' if on_bat else 'on a supply'}"
                                       f"{', DC in ' + str(sup['dc_in']) + ' V' if sup else ''}); nothing keyed")
    per_radio(run, "control", "power-cap", power_cap, ["tx-power-cap-per-radio", "panel-pwr-range"], want_tx=True)


def rig(host, lines, secs=4.0):
    s = socket.create_connection((host, 4532), timeout=secs)
    out = []
    try:
        for ln in lines:
            s.sendall((ln + "\n").encode())
            time.sleep(0.4)
            s.settimeout(1.5)
            try:
                out.append(s.recv(4096).decode(errors="replace").strip())
            except socket.timeout:
                out.append("")
    finally:
        s.close()
    return out


def t_rigctl(run):
    def fn(r):
        out = rig(r.host, ["F 14074000", "f", "M USB 2400", "m", "v", "T 1", "t", "\\get_info"])
        f_ok = out[0].startswith("RPRT 0") and out[1].split()[0] == "14074000"
        m_ok = out[3].split()[0] == "USB" if out[3] else False
        ptt_refused = out[5].startswith("RPRT -") and out[6].split()[:1] == ["0"] and not r.tx
        info = out[7]
        ok = f_ok and m_ok and ptt_refused and r.name[1:] in info.replace("X", "x")
        return ok, (f"F/f {out[0]!r} {out[1]!r}; M/m {out[2]!r} {out[3]!r}; v {out[4]!r}; T 1 {out[5]!r} (PTT stays "
                    f"off: t {out[6]!r}); get_info {info!r}")
    per_radio(run, "control", "rigctl", fn, ["rigctl-get-freq", "rigctl-set-freq", "rigctl-mode", "rigctl-vfo",
                                             "rigctl-ptt-refused"])

    def more(r):
        out = rig(r.host, ["l STRENGTH", "l PREAMP", "l AF", "s", "S 1 VFOB", "\\dump_state"])
        strength = out[0].lstrip("-").isdigit()
        split_refused = out[3].split()[:1] == ["0"] and out[4].startswith("RPRT -")
        dump = out[5].startswith("1") or out[5].startswith("0")
        return strength and split_refused and dump, (f"l STRENGTH {out[0]!r}, l PREAMP {out[1]!r}, l AF {out[2]!r}; "
                                                     f"s {out[3]!r}, S 1 VFOB {out[4]!r}; dump_state "
                                                     f"{out[5][:30]!r}")
    per_radio(run, "control", "rigctl-more", more, ["rigctl-levels", "rigctl-split-off", "rigctl-dump-state"])


WEB_SKIP = {"/api/backup/download", "/api/gallery/zip", "/api/update/check", "/api/flights", "/api/meteo",
            "/api/boot_picture/png", "/files/"}      # big downloads, the internet, binaries


def web_routes():
    import re
    seen = set()
    for root, _, files in os.walk(os.path.join(HERE, "..", "..", "webserver", "src")):
        for f in files:
            if f.endswith(".py"):
                for m in re.finditer(r"@(?:app|[a-z_]+)\.(?:route|get)\(['\"]([^'\"]+)['\"]", open(os.path.join(root, f)).read()):
                    if "<" not in m.group(1):
                        seen.add(m.group(1))
    return sorted(seen - WEB_SKIP)


def features_getting(path):
    """The features of features.json whose check gets path (its how_to_confirm: "GET /path" or "GET http://...path")"""
    import re
    try:
        feats = json.load(open(os.path.join(HERE, "features.json")))
    except (OSError, ValueError):
        return []
    pat = re.compile(r"GET\s+(?:https?://<?[a-z0-9.>-]*>?)?" + re.escape(path) + r"(?![a-z0-9_/])")
    return [f["id"] for f in feats if pat.search(f.get("how_to_confirm", ""))]


def t_web(run):
    codes = {}
    for n in run.names:
        host = run.c["radios"][n]["host"]
        ip = socket.gethostbyname(host)     # once: each name lookup of x6100.local took 5 s (mDNS), 11 minutes in all
        for path in web_routes():
            try:
                req = urllib.request.Request(f"http://{ip}{path}", headers={"Host": host})
                with urllib.request.urlopen(req, timeout=8) as resp:
                    codes.setdefault(path, {})[n] = resp.status
            except urllib.error.HTTPError as e:
                codes.setdefault(path, {})[n] = e.code
            except (urllib.error.URLError, OSError) as e:
                codes.setdefault(path, {})[n] = f"no answer ({e})"
    for path, by in codes.items():
        vals = list(by.values())
        ok = all(v == 200 for v in vals)
        same = len(set(map(str, vals))) == 1
        # a page served is part of a feature (its controls are more): the features whose check begins by getting it
        run.result("control", f"web {path}", True if ok else (None if same else False),
                   ", ".join(f"{k} {v}" for k, v in by.items()) + ("" if same else " (NOT alike)"),
                   covers=["web-configurator-reachable"] if path == "/" else [], partial=features_getting(path))


def control(run, only):
    for name, fn in (("tci", t_tci), ("tci-receiver", t_tci_more), ("rigctl", t_rigctl), ("remote", t_remote),
                     ("web", t_web)):
        if not only or name in only:
            run.banner(f"SUITE control: {name} (nothing sent)")
            fn(run)


# ---- safety: the transmitter's locks (the cable, 0.1 W at most) ----

def watched(run, r, secs, press):
    """press() then secs watched: whether r went on the air (a Watch over both radios, the other listening)"""
    other = [n for n in run.names if n != r.name]
    lis = bench.Radio(other[0], run.c["radios"][other[0]], False) if other else None
    radios = [r] + ([lis] if lis else [])
    watch = bench.Watch(radios, run.c, w=bench.FIRST_PROBE_W, listener=lis)
    went, t0 = False, time.time()
    try:
        press()
        while time.time() - t0 < secs and not watch.why:
            went = went or r.tx
            time.sleep(0.05)
    finally:
        r.send("key PTT 0")
        watch.close()
        if lis:
            lis.close()
    return went, watch


def t_safety(run, only=()):
    p, total = bench.chain_gate(run.c, "radios")
    real_per_radio = per_radio

    def per_radio_(run_, batch, test, fn, covers, want_tx=False):     # --only: these tests alone
        if only and test not in only:
            return {}
        return real_per_radio(run_, batch, test, fn, covers, want_tx)

    def locked(r):
        bench.ensure(r, *bench.tx_profile(F_TEST, 2, bench.FIRST_PROBE_W))
        bench.arm(r, False)
        went, w = watched(run, r, 2.5, lambda: r.send("key PTT 1 1"))
        return not went and not w.why, f"PTT with the transmitter locked (rxo {r.state.get('rxo')}): " + \
            ("stayed off the air" if not went else "WENT ON THE AIR")
    per_radio_(run, "safety", "tx-locked", locked, ["sentry-tx-locked", "panel-rx-only-hold"], want_tx=True)

    def ab(r):
        bench.arm(r, True)
        armed = r.state.get("rxo") == 0
        bench.arm(r, False)
        return armed and r.state.get("rxo") == 1, f"A/B on G: armed (rxo 0) {armed}, locked again (rxo 1) " \
            f"{r.state.get('rxo') == 1}"
    per_radio_(run, "safety", "ab-arm", ab, ["sentry-ab-arm"], want_tx=True)

    def lease(r):
        bench.ensure(r, *bench.tx_profile(F_TEST, 2, bench.FIRST_PROBE_W))
        bench.radio_tx_gate(r, run.c, bench.FIRST_PROBE_W)
        bench.arm(r, True, mode=2)
        bench.tx_log({"who": r.name, "f": F_TEST, "w": r.pwr(), "what": "suite safety: the PTT lease"})
        t0 = time.time()
        r.send("key PTT 1 7")                   # once, never renewed
        on = off = None
        while time.time() - t0 < 6:
            if r.tx and on is None:
                on = time.time() - t0
            if on is not None and not r.tx:
                off = time.time() - t0
                break
            time.sleep(0.05)
        r.send("key PTT 0")
        bench.arm(r, False)
        return off is not None and off < 4.0, f"one PTT, never renewed: on the air at {on and round(on, 1)} s, " \
            f"off at {off and round(off, 1)} s (the lease: 3 s)"
    per_radio_(run, "safety", "ptt-lease", lease, ["panel-ptt-lease", "panel-ptt-release"], want_tx=True)

    def out_of_band(r):
        bench.ensure(r, {"freq": 15000000, "mode": 2})        # WWV's, outside the amateur bands
        bench.arm(r, True)
        went, w = watched(run, r, 2.5, lambda: r.send("key PTT 1 9"))
        bench.arm(r, False)
        return not went, f"PTT armed on 15.000 MHz: " + ("refused, off the air" if not went else "WENT ON THE AIR")
    per_radio_(run, "safety", "ptt-out-of-band", out_of_band, ["tx-out-of-band-ptt-refused", "panel-ptt-refused-band",
                                                             "tx-refuse-out-of-amateur-hf"], want_tx=True)

    def tci_out_of_band(r):
        import subprocess
        bench.ensure(r, {"freq": 15000000, "mode": 3, "pwr": bench.FIRST_PROBE_W})
        bench.arm(r, True, mode=3)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
                        f"root@{r.host}:/tmp/tci_tone_tx.py"], check=True)

        def press():
            subprocess.Popen(["ssh", "-n", "-o", "BatchMode=yes", f"root@{r.host}",
                              "python3 /tmp/tci_tone_tx.py '[[1000, 2.0]]' 0.3 3.0 50001 0.05"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        went, w = watched(run, r, 4.0, press)
        # the sender gone before anything is put back: a TCI client holds control while it is connected, and the dial
        # was left on 15 MHz under it once (2026-11-22)
        subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{r.host}",
                        "kill $(cat /tmp/tci_tone_tx.pid) 2>/dev/null; sleep 1; true"], capture_output=True)
        time.sleep(1.0)
        if not r.ctl_mine:
            r.take_control()
        bench.arm(r, False)
        return not went, "a TCI transmission (a program on the radio) on 15.000 MHz, armed: " + \
            ("refused, off the air" if not went else "WENT ON THE AIR")
    per_radio_(run, "safety", "tci-out-of-band", tci_out_of_band, ["tci-key-refused-band", "tx-ranges-all-paths"],
              want_tx=True)

    def tci_ask(r):
        bench.ensure(r, *bench.tx_profile(F_TEST, 3, bench.FIRST_PROBE_W))
        bench.radio_tx_gate(r, run.c, bench.FIRST_PROBE_W)
        bench.arm(r, True, mode=3)
        t, greet = tci_session(r.host)          # a client on the network: the operator's to allow
        asked = False

        def press():
            t.send("trx:0,true;")
        try:
            went, w = watched(run, r, 4.0, press)
            asked = r.state.get("ask") == 1
        finally:
            t.close()
        # the question outlives its connection on purpose (WSJT-X and JTDX hang up and come back, 2026-11-21): it
        # stays its TCI_ASK_S (20 s) unanswered, then goes as a No
        t0 = time.time()
        while time.time() - t0 < 24 and r.state.get("ask") == 1:
            time.sleep(0.5)
        gone_s = round(time.time() - t0, 1)
        bench.arm(r, False)
        gone = r.state.get("ask") == 0
        return asked and not went and gone, (f"a network TCI client's trx:0,true: asked on the screen {asked}, "
                                             f"{'off the air' if not went else 'WENT ON THE AIR'}; unanswered, the "
                                             f"question gone {gone_s} s after the client left (its 20 s): {gone}")
    per_radio_(run, "safety", "tci-ask", tci_ask, ["tci-ask-prompt", "tci-ask-timeout"], want_tx=True)

    def rx_session(r):
        went, w = watched(run, r, 2.5, lambda: r.send("key PTT 1 11"))
        return not went and r.state.get("rxo") == 1, \
            f"a receive-only session's PTT (rxo {r.state.get('rxo')}): " + ("refused" if not went else "WENT ON THE AIR")
    per_radio_(run, "safety", "rx-session-ptt", rx_session, ["panel-ptt-refused-no-tx", "role-view-refusals"])


def safety(run, only):
    run.close()
    run.radios = {}
    t_safety(run, only)


# ---- CW both ways: the sender's own keyer (TCI cw_macros, run on it), the listener's decoder ----
F_CW = 14040000
CW_TEXT = "VVV TEST DE Q1TST"    # the V's: the decoder finds the speed first; about 12 s at 20 wpm (one over: 16)


def cw_over(run, s_name, l_name):
    """s_name keys CW_TEXT at 0.1 W through its own TCI (tci_cw_tx.py on it), l_name's CW decoder must give it back
    (the remote's "cw" lines): both said whole, the path proved first, watched, put back on page A"""
    c = run.c
    p, total = bench.chain_gate(c, "radios")
    snd, lis = bench.Radio(s_name, c["radios"][s_name], True), bench.Radio(l_name, c["radios"][l_name], False)
    radios, snaps, rep, watch = [snd, lis], {}, {}, None
    for r in radios:
        r.take_control()
        snaps[r.name] = bench.snapshot(r)
    try:
        bench.listener_gate(lis)
        for r in radios:
            bench.open_page(r, "G")
        bench.ensure(lis, *bench.rx_profile(F_CW, 4, pre=0, att=0, rfg=100, agc=2))
        bench.path_proof(c, total, snd, lis, F_CW, rep)
        bench.ensure(lis, *bench.rx_profile(F_CW, 4, pre=0, att=0, rfg=100, agc=2))
        bench.ensure(snd, *bench.tx_profile(F_CW, 4, bench.FIRST_PROBE_W))
        bench.radio_tx_gate(snd, c, bench.FIRST_PROBE_W)
        import subprocess
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_cw_tx.py"),
                        f"root@{snd.host}:/tmp/tci_cw_tx.py"], check=True)
        for r in radios:
            r.send(f"banner BENCH CW: {s_name.upper()} KEYS '{CW_TEXT}' {bench.FIRST_PROBE_W} W {F_CW / 1e6:.3f} MHZ, "
                   f"{l_name.upper()} DECODES (RX ONLY) THROUGH {total} DB")
        watch = bench.Watch(radios, c, w=bench.FIRST_PROBE_W, listener=lis)
        bench.arm(snd, True, mode=4)            # arming puts CW into the voice mode: CW again
        bench.tx_log({"who": s_name, "f": F_CW, "w": snd.pwr(), "what": "suite CW"})
        lis.cw = ""
        out = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{snd.host}",
                              f"python3 /tmp/tci_cw_tx.py '{CW_TEXT}' 15"], capture_output=True, text=True,
                             timeout=40).stdout
        time.sleep(1.5)
        watch.close()
        bench.arm(snd, False)
        got = " ".join(lis.cw.split())
        words = [w for w in ("TEST", "Q1TST") if w in got.upper()]
        ok = not watch.why and bool(words)
        return ok, (f"{s_name} keyed {CW_TEXT!r} ({out.strip().splitlines()[-1:] or 'no word'}), {l_name}'s decoder: "
                    f"{got[-80:]!r} (copied: {', '.join(words) or 'neither TEST nor Q1TST'})"
                    + (f"; STOPPED: {watch.why}" if watch.why else ""))
    except bench.Refused as e:
        return False, f"refused: {e}"
    finally:
        if watch:
            watch.close()
        bench.finish(radios, snaps, report=rep)
        if rep.get("not_restored"):         # a radio not as found fails the test (returned from here: it decides)
            return False, f"NOT put back: {rep['not_restored']}"


# ---- every band both ways: the path proof (0.1 W, 1000 Hz) on each amateur band, each radio sending ----
PROOF_BANDS = [("160m", 1900000), ("80m", 3700000), ("60m", 5357000), ("40m", 7150000), ("30m", 10130000),
               ("20m", 14200000), ("17m", 18130000), ("15m", 21300000), ("12m", 24940000), ("10m", 28400000),
               ("6m", 50125000)]


def band_proofs(run):
    """Each band, each way: the listener on the dial (PRE and ATT off, RF gain 100, AGC auto), the sender's 0.1 W
    tone 20 dB over its quiet at least, its meter read; both put back and on page A"""
    c = run.c
    p, total = bench.chain_gate(c, "radios")
    for s_name, l_name in ((run.names[0], run.names[1]), (run.names[1], run.names[0])):
        snd, lis = bench.Radio(s_name, c["radios"][s_name], True), bench.Radio(l_name, c["radios"][l_name], False)
        radios, snaps, rep = [snd, lis], {}, {}
        for r in radios:
            r.take_control()
            snaps[r.name] = bench.snapshot(r)
        try:
            bench.listener_gate(lis)
            for r in radios:
                bench.open_page(r, "G")
            for band, f in PROOF_BANDS:
                try:
                    bench.ensure(lis, *bench.rx_profile(f, 2, pre=0, att=0, rfg=100, agc=3))
                    res = bench.path_proof(c, total, snd, lis, f, rep)
                    run.result("paths", f"band {band} {s_name}->{l_name}", True,
                               f"{l_name} sees {s_name}'s 0.1 W tone {res['rise_db']} dB over its quiet "
                               f"({s_name}'s meter {res['sender_peak_w']} W)",
                               covers=["path-tx-power-readback", "path-rx-level-parity", "tx-bands-each"])
                except bench.Refused as e:
                    run.result("paths", f"band {band} {s_name}->{l_name}", False, str(e)[:300],
                               covers=["path-tx-power-readback", "tx-bands-each"])
        finally:
            bench.finish(radios, snaps, report=rep)
            run.result("paths", f"band put back {s_name}->{l_name}", not rep.get("not_restored"),
                       "both as found after the bands" if not rep.get("not_restored") else
                       f"NOT put back: {rep['not_restored']}")


def open_app(r, name):
    """r's app whose name starts with name (any case): from the remote's apps list, else E's types one by one (the
    RTTY panel is no app of the list: E's OPEN opens it), each opened and closed again till it is the one"""
    r.send("apps")
    r.wait("the apps list", lambda: r.apps is not None, 5)
    is_it = lambda: (r.state.get("app") or r.state.get("win") or "").lower().startswith(name)
    app = next((a for a in r.apps if a[1].lower().startswith(name)), None)
    if app:
        r.send(f"open {app[0]}")
        try:
            r.wait(f"its {app[1]}", is_it, 10)
            return True
        except bench.Refused:
            pass
    for _ in range(26):                         # E's types, round once
        bench.open_page(r, "E")
        time.sleep(1.0)
        bench.tap(r, "F4")
        time.sleep(1.2)
        bench.tap(r, "F2")
        time.sleep(3.0)
        if is_it():
            return True
        bench.close_apps(r)
    return False


# ---- keyboard modes both ways: RTTY and PSK31 made here (digimodes.py), sent by one radio's TCI at 0.1 W, decoded by
# the other's own app into its text log ----
DIGI_TEXT = {"rtty": "RYRY CQ DE Q1TST Q1TST K", "psk31": "CQ CQ DE Q1TST Q1TST K"}
DIGI_APP = {"rtty": "rtty", "psk31": "psk"}


def digi_over(run, mode, s_name, l_name):
    import subprocess
    import digimodes
    c = run.c
    p, total = bench.chain_gate(c, "radios")
    snd, lis = bench.Radio(s_name, c["radios"][s_name], True), bench.Radio(l_name, c["radios"][l_name], False)
    radios, snaps, rep, watch = [snd, lis], {}, {}, None
    for r in radios:
        r.take_control()
        snaps[r.name] = bench.snapshot(r)
    try:
        bench.listener_gate(lis)
        if not open_app(lis, DIGI_APP[mode]):
            return False, f"{l_name}: its {mode} app did not open (the remote's apps, then E's types)"
        app = (None, lis.state.get("app") or lis.state.get("win"))
        time.sleep(2.0)
        f, m = int(lis.state.get("freq", 0)), int(lis.state.get("mode", -1))
        rule = rule_of(f)
        bench.ensure(lis, {"att": rule["att"], "pre": rule["pre"], "agc": rule["agc"], "sql": 0, "nb": 0, "nr": 0})
        # the listener's own settings for RTTY (its centre, shift, rate, reverse), the signal made to them
        cfg = {}
        if mode == "rtty":
            rows = bench.ssh(lis.host, "sqlite3 /mnt/params.db \"select name,val from params where name like 'rtty%'\"")
            cfg = {k: int(v) for k, v in (ln.split("|") for ln in rows.split() if "|" in ln)}
            usb = m in (2, 3)
            mark_low = usb == bool(cfg.get("rtty_reverse", 1))   # rtty.c tx_start: mark_hi = USB != reverse
            x = digimodes.rtty_audio(DIGI_TEXT[mode], center=cfg.get("rtty_center", 800),
                                     shift=cfg.get("rtty_shift", 450), baud=cfg.get("rtty_rate", 5000) / 100,
                                     mark_low=mark_low)
        else:
            x = digimodes.psk31_audio(DIGI_TEXT[mode])
        local = os.path.join(run.dir, f"{mode}_{s_name}.raw")
        secs = digimodes.write_raw(x, local)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", local, f"root@{snd.host}:/tmp/bench_digi.raw"], check=True)
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(HERE, "tci_tone_tx.py"),
                        f"root@{snd.host}:/tmp/tci_tone_tx.py"], check=True)
        log0 = bench.ssh(lis.host, "f=$(ls -t /mnt/digi_text/*.txt 2>/dev/null | head -1); [ -n \"$f\" ] && wc -c < $f "
                                   "|| echo 0").strip()
        bench.open_page(snd, "G")
        bench.ensure(snd, *bench.tx_profile(f, m, bench.FIRST_PROBE_W))
        bench.radio_tx_gate(snd, c, bench.FIRST_PROBE_W)
        for r in radios:
            r.send(f"banner BENCH {mode.upper()}: {s_name.upper()} SENDS '{DIGI_TEXT[mode]}' {bench.FIRST_PROBE_W} W "
                   f"{f / 1e6:.4f} MHZ, {l_name.upper()}'S APP DECODES (RX ONLY) THROUGH {total} DB")
        watch = bench.Watch(radios, c, w=bench.FIRST_PROBE_W, listener=lis)
        bench.arm(snd, True, mode=m)
        bench.tx_log({"who": s_name, "f": f, "w": snd.pwr(), "what": f"suite {mode}"})
        lines, t_on, t_off = bench.tci_send(snd, None, max_s=secs + 4, watch=watch, file="/tmp/bench_digi.raw",
                                            amp=0.5)
        watch.close()
        bench.arm(snd, False)
        time.sleep(2.0)
        # the app closed: PSK31 decodes the noise after the signal and its line never pauses, so only the app's closing
        # ends it onto the card (digi_textlog_end); RTTY's pause ends it sooner. Then looked for up to 30 s
        bench.close_apps(lis)
        # the app's line ends at its pause and goes onto the card after (digi_textlog.c): looked for up to 30 s
        got, t0 = "", time.time()
        while time.time() - t0 < 30 and "Q1TST" not in got.upper():
            time.sleep(3.0)
            got = bench.ssh(lis.host, f"f=$(ls -t /mnt/digi_text/*.txt | head -1); tail -c +{int(log0 or 0) + 1} $f")
        ok = not watch.why and "Q1TST" in got.upper()
        return ok, (f"{s_name} sent {mode} ({secs:.1f} s at {f} Hz, mode {m}{', ' + str(cfg) if cfg else ''}); "
                    f"{l_name}'s {app[1]} wrote: {' '.join(got.split())[-120:]!r}"
                    + (f"; STOPPED: {watch.why}" if watch.why else ""))
    except bench.Refused as e:
        return False, f"refused: {e}"
    finally:
        if watch:
            watch.close()
        bench.finish(radios, snaps, report=rep)
        if rep.get("not_restored"):         # a radio not as found fails the test (returned from here: it decides)
            return False, f"NOT put back: {rep['not_restored']}"


# ---- paths: the radios' signal paths both ways (the bench's own tests, as children) ----

def paths(run, only):
    from suite import run_child
    run.close()
    run.radios = {}
    a, b = run.names[0], run.names[1]
    tests = [("tones", lambda s, l: [os.path.join(HERE, "tones.py"), s, l, "--passes", "usb,lsb", "--tones",
                                     "2000,1700,1400,1100,800,500", "--tx-mode", "3", "--tag", "suite"],
              ["path-usb-tones", "path-lsb-tones", "path-rx-level-parity"]),
             ("mic-on-g", lambda s, l: [os.path.join(HERE, "acoustic.py"), l, s, "--tag", "suite"],
              ["path-mic-voice", "sentry-g-ptt", "panel-mic-ptt"])]
    for name, cmd, covers in tests:
        if only and name not in only:
            continue
        for s, l in ((a, b), (b, a)):
            r = run_child([sys.executable, "-B"] + cmd(s, l), 400)
            out = (r.stdout + r.stderr).strip().splitlines()
            with open(os.path.join(run.dir, f"{name}_{s}_{l}.txt"), "w") as f:
                f.write("\n".join(out) + "\n")
            keep = [x for x in out if any(w in x for w in ("path proof", "PASS", "REFUSED", "STOP", "summary", " Hz ->",
                                                           "steps on its", "own capture", "NOT put back"))]
            run.result("paths", f"{name} {s}->{l}", r.returncode == 0,
                       " | ".join(x.split(" ", 1)[-1] for x in (keep or out[-3:]))[:900], covers=covers)
    if not only or "bands" in only:
        band_proofs(run)
    for mode in ("rtty", "psk31"):
        if not only or mode in only:
            for s_, l_ in ((a, b), (b, a)):
                ok, says = digi_over(run, mode, s_, l_)
                run.result("paths", f"{mode} {s_}->{l_}", ok, says,
                           covers=[f"path-{mode}-both-ways", "rtty-decode" if mode == "rtty" else "psk-mode-txrx",
                                   "settings-general-text-log"])
    if not only or "cw" in only:
        for s_, l_ in ((a, b), (b, a)):
            ok, says = cw_over(run, s_, l_)
            run.result("paths", f"cw {s_}->{l_}", ok, says, covers=["path-cw-both-ways", "cw-decoder-copy",
                                                                    "tci-cw-macros"])
    if not only or "walkie" in only:
        r = run_child([sys.executable, "-B", os.path.join(HERE, "bench.py"), "radios-walkie", a, b, "--bands",
                       "20m,10m-fm", "--modes", "am,fm"], 600)
        out = (r.stdout + r.stderr).strip().splitlines()
        with open(os.path.join(run.dir, "walkie.txt"), "w") as f:
            f.write("\n".join(out) + "\n")
        for x in out:
            if x.split(" ", 1)[-1].startswith(("PASS ", "FAIL ")):
                run.result("paths", "walkie " + x.split(" ", 3)[2:4][0] + " " + x.split(" ")[3], x.split(" ", 1)[-1]
                           .startswith("PASS"), x.split(" ", 1)[-1][:300], covers=["path-am-voice", "path-fm-voice",
                                                                                     "sentry-g-walkie"])
        run.result("paths", "walkie", r.returncode == 0, (out[-1] if out else "")[:300],
                   covers=["path-am-voice", "path-fm-voice"])


# ---- look: every Settings tab on both, photographed side by side and compared ----
SETTINGS_TABS = [("general", 0, "F2"), ("display", 0, "F3"), ("audio", 0, "F4"), ("bluetooth", 1, "F2"),
                 ("keys", 1, "F3"), ("cluster", 1, "F4"), ("txcal", 1, "F5"), ("info", 2, "F2"), ("walkie", 2, "F3")]
# (Wireless left out: its tab scans for networks as it opens, the bench's own link to the radios)


def look(run, only):
    from suite import screens_pair
    names = {}
    for n in run.names:
        r = run.radio(n)
        r.take_control()
        r.send("apps")
        r.wait("the apps list", lambda r=r: r.apps is not None, 5)
        names[n] = next((a[0] for a in r.apps if a[1].lower().startswith("settings")), None)
    if None in names.values():
        run.result("look", "settings", False, f"no Settings app on {[n for n, v in names.items() if v is None]}")
        return
    done = []
    try:
        for tab, page, key in SETTINGS_TABS:
            if only and tab not in only:
                continue

            def setup(r, page=page, key=key):
                r.send(f"open {names[r.name]}")
                r.wait("Settings", lambda: (r.state.get("win") or "").lower().startswith("settings"), 10)
                time.sleep(1.0)
                for _ in range(page):
                    bench.tap(r, "F1")
                    time.sleep(1.0)
                bench.tap(r, key)
                time.sleep(1.5)
            screens_pair(run, f"settings_{tab}", setup,
                         lambda r: (r.state.get("win") or "").lower().startswith("settings"))
            done.append(tab)
            for n in run.names:
                bench.close_apps(run.radio(n))
    finally:
        for n in run.names:
            r = run.radio(n)
            bench.close_apps(r)
            bench.open_page(r, "A")
    alike = [x for x in run.results if x["test"].startswith("alike settings_")]
    run.result("look", "settings-each-tab", bool(done) and all(x["ok"] is True for x in alike),
               f"{len(done)} tabs photographed on both: " + ", ".join(f"{x['test'][15:]} {x['says'].split()[0]}"
                                                                   for x in alike),
               covers=["lookfeel-settings-each-tab", "lookfeel-settings-alike"])


# ---- coverage: every feature, covered or not ----

def coverage(run):
    """features.json against what this run's results cover: covered and passed, covered and failed, not covered yet;
    by area, into coverage.md beside the report"""
    path = os.path.join(HERE, "features.json")
    if not os.path.exists(path):
        return
    feats = json.load(open(path))
    by, part = {}, {}
    for x in run.results:
        for fid in x.get("covers") or []:
            by.setdefault(fid, []).append(x["ok"])
        for fid in x.get("partial") or []:
            part.setdefault(fid, []).append(x["ok"])
    # the features the inventory says an older test of the suite covers (its "suite_has": "core/screen app101_FT8",
    # "cable/ft8", "core/screen app*", "core/screen sentry_A..sentry_H"), when this run had that test
    import re
    for f in feats:
        sh = f.get("suite_has")
        if not sh or f["id"] in by:
            continue
        batch, _, test = sh.partition("/")
        test = re.sub(r"\s*\(.*$", "", test).strip()
        if ".." in test:
            test = test.split("..")[0].rstrip("ABCDEFGHI")
        prefix = test.split("*")[0]
        hits = [x["ok"] for x in run.results if x["batch"] == batch and (x["test"].startswith(prefix) if prefix
                                                                         else True)]
        if hits:
            (by if f.get("coverage", "full") == "full" else part).setdefault(f["id"], []).extend(hits)
    known = {f["id"] for f in feats}
    unknown = sorted((set(by) | set(part)) - known)
    areas = {}
    for f in feats:
        st = by.get(f["id"])
        k = ("partial" if f["id"] in part and False not in part[f["id"]] else "FAIL" if f["id"] in part
             else "not covered") if st is None else "FAIL" if False in st else "pass" if all(s is True for s in st) \
            else "look"
        areas.setdefault(f["area"], {}).setdefault(k, []).append(f)
    lines = [f"# Coverage of {len(feats)} features ({time.strftime('%Y-%m-%d %H:%M')})", "",
             "partial: a test covers part of it (a page served, not its controls)", "",
             "| area | features | pass | fail | look | partial | not covered |", "|---|---|---|---|---|---|---|"]
    for area, ks in sorted(areas.items()):
        lines.append(f"| {area} | {sum(len(v) for v in ks.values())} | {len(ks.get('pass', []))} | "
                     f"{len(ks.get('FAIL', []))} | {len(ks.get('look', []))} | {len(ks.get('partial', []))} | "
                     f"{len(ks.get('not covered', []))} |")
    lines += ["", "## Not covered yet", ""]
    for area, ks in sorted(areas.items()):
        for f in ks.get("not covered", []):
            lines.append(f"- `{f['id']}` ({area}, needs {f['needs']}): {f['feature']}")
    if unknown:
        lines += ["", "Tests name features the list does not have: " + ", ".join(unknown)]
    with open(os.path.join(run.dir, "coverage.md"), "w") as fo:
        fo.write("\n".join(lines) + "\n")
    tot = {k: sum(len(a.get(k, [])) for a in areas.values()) for k in ("pass", "FAIL", "look", "partial", "not covered")}
    say(f"coverage: {tot['pass']} features pass, {tot['FAIL']} fail, {tot['look']} to look at, {tot['partial']} in part, "
        f"{tot['not covered']} not covered yet: {os.path.join(run.dir, 'coverage.md')}")


def coverage_all():
    """The coverage of every suite run so far: the latest result of each test across ~/.config/x6x00-bench/runs/*/
    report.json, into a coverage run of its own (runs/<time>-coverage/coverage.md)"""
    import glob

    latest = {}
    for path in sorted(glob.glob(os.path.join(bench.STATE, "runs", "*", "report.json"))):
        try:
            rows = json.load(open(path))
        except (OSError, ValueError):
            continue
        if not isinstance(rows, list):
            continue                        # a bench test's own report, not the suite's
        for x in rows:
            if isinstance(x, dict) and "batch" in x and "test" in x:
                latest[(x["batch"], x["test"])] = dict(x, run=os.path.basename(os.path.dirname(path)))

    class Run:
        pass
    run = Run()
    run.dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-coverage"))
    os.makedirs(run.dir, exist_ok=True)
    run.results = list(latest.values())
    with open(os.path.join(run.dir, "results.json"), "w") as f:
        json.dump(run.results, f, indent=1)
    coverage(run)
    return run.dir
