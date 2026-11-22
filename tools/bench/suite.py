#!/usr/bin/env python3
"""GoldCorpHF's end-to-end suite for the X6100 and the X6200 side by side: what works on each, and whether the two
look and act alike ("1:1"), so a tagged release breaks neither. Run from the computer beside the radios, through the
bench harness's sessions (bench.py: the same configuration, gates and logs), SSH for what the remote does not say.

    suite.py list                          the batches, what each needs plugged in, their tests
    suite.py run BATCH[,BATCH] [--only T]  run them; a report in ~/.config/x6x00-bench/runs/<time>-<batches>/
    suite.py release                       core, then every batch bench.json's "attached" names: the gate for a tag
    suite.py align FROM TO [--undo FILE]   FROM's look-and-behaviour settings onto TO (its GUI stopped meanwhile),
                                           TO's old values kept in a file for --undo: the radios set alike

Batches, by the hardware they need (the hardware ones in a batch of their own, run when it is on the bench):

    core     both radios on the network, any antenna or none; receive-only sessions, nothing keyed
    rtl      an RTL-SDR on each radio's HOST port, their antennas side by side
    cable    the two radios' antenna jacks joined through >= 60 dB of pads (bench.py chain-ok radios first)
    update   a release published: the updater's check and its bundle's parts, nothing installed
    (the HackRF's tests run from bench.py: hackrf-ft8, hackrf-iq; there is no gps batch)

Every test puts back the settings it changed (the app open, the receiver's settings, the bands it tuned through) and
the suite leaves both radios on the Sentry's page A at its end, whatever page it found them on (the owner, 2026-11-22:
"when tests finish they should always leave on screen A"); the cable batch's transmissions run in bench.py, which
says each radio whole, proves the path first and checks what it puts back. Screens are
compared as pictures: each pair side by side in the report, and each picture's colours counted against the Sentry
theme's gold on black: cyan, blue or purple (the old themes' accents) is what an old look leaves behind; the warm
gradient (the waterfall's dark orange to gold) and green status words are the Sentry's own.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import colorsys
import concurrent.futures
import gzip
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import suite_more  # noqa: E402
import suite_proto  # noqa: E402

try:
    from PIL import Image, ImageDraw
except ImportError:            # the pictures need Pillow; the rest runs without
    Image = None

PANEL = (480, 800)
NEVER_OPEN = {102, 116, 131}   # SWR scan and WSPR put the radio on the air by opening; the first-start guide
NEEDS = {128: "Bluetooth headphones or a TCI client (RADE opens only with one)"}    # apps that may stay shut
PAGES = "ABCDEFG"
OFF_PALETTE_FLAG = 1.0         # % of a screen's pixels cyan, blue or purple (an old theme's accents): flagged
# Settings that are each radio's own by nature (where it is tuned, its levels), not its look or its behaviour
OWN = re.compile(r"(freq|vfo|band|_hz$|^vol|volume|rfg|rfgain|^pre|^att|^agc|filter|^s\.|rtl_|sentry_hear|"
                 r"_cal|smeter|pa_bias|tx_|^pwr|power|^atu|calib|offset|^qth|callsign|^ver|first_start|wifi|"
                 r"lhz|_time$|_at$|last_|gain|^step|zoom|mode_|split|rit|xit|key_|_dac)", re.I)


# For the pictures (the owner, 2026-11-19: A and B "weren't even tuned to the same frequency", the Direction finder's
# "resting state ... differed", "that should all be sync'd up when you take photos"): everything copied but what is
# the radio's own hardware or identity. The dial, the bands, the volumes and the receiver's switches, each app's
# frequency and choices go too, and the GUI's state file (B's frequency, the Sentry's pages, the Direction finder's
# target and array: gui_state.conf), undone after the pictures
PHOTO_OWN = re.compile(r"(_cal|calib|smeter|pa_bias|tx_|^pwr|power|^atu|offset|^qth|callsign|^ver|first_start|wifi|"
                       r"_time$|_at$|last_|key_|_dac)", re.I)
STATE_OWN = re.compile(r"^(tries|paused|place|app|s\.scr)$")


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Run:
    """One run: its radios, its folder, its results"""

    def __init__(self, names, batches):
        self.c = bench.conf()
        self.names = names
        self.dir = os.path.join(bench.STATE, "runs", time.strftime("%Y%m%d-%H%M%S-") + "-".join(batches))
        os.makedirs(os.path.join(self.dir, "shots"), exist_ok=True)
        self.results = []          # {"batch", "test", "ok": True|False|None, "says", "radio"?}
        self.radios = {}
        self.shown = {}            # each radio's banner text now ("banner TEXT")

    def host(self, name):
        return self.c["radios"][name]["host"]

    def radio(self, name):
        if name not in self.radios:
            self.radios[name] = bench.Radio(name, self.c["radios"][name], want_tx=False)
        return self.radios[name]

    def banner(self, text, names=None):
        """The line across each radio's screen while the suite runs (the owner, 2026-11-19: which radio sends, which
        listens, which test): the radio puts its own TX or RX in front; it goes when the suite's connection does"""
        for n in names or self.names:
            try:
                self.radio(n).send(f"banner {text}")
                self.shown[n] = text
            except (bench.Refused, OSError):
                pass

    def result(self, batch, test, ok, says, **kw):
        self.results.append(dict(batch=batch, test=test, ok=ok, says=says, **kw))
        say(("PASS " if ok else "WARN " if ok is None else "FAIL ") + f"{batch}/{test}: {says}")

    def close(self):
        for r in self.radios.values():
            try:
                r.close()
            except OSError:
                pass

    # ---- pictures ----

    def shot(self, name, label):
        """The radio's screen as it is now, landscape, into shots/<label>_<name>.png; its path, or None"""
        if Image is None:
            return None
        host = self.host(name)
        text = self.shown.get(name) if name in self.radios else None
        if text:                                        # the screen as the radio has it, without the suite's banner
            self.radios[name].send("banner")
            time.sleep(0.4)
        cmd = ("rm -f /tmp/screen.raw /tmp/suite.raw.gz; kill -USR1 $(pidof x6100_gui); "
               "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -f /tmp/screen.raw ] && break; sleep 0.2; done; "
               "sleep 0.3; gzip -1 -c /tmp/screen.raw")
        try:
            r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                               capture_output=True, timeout=90)
            data = gzip.decompress(r.stdout)
        except (subprocess.TimeoutExpired, OSError, EOFError) as e:
            say(f"{name}: no screenshot ({e})")
            return None
        finally:
            if text:
                self.radios[name].send(f"banner {text}")
        if len(data) != PANEL[0] * PANEL[1] * 4:
            say(f"{name}: a screenshot of {len(data)} bytes, not a whole panel")
            return None
        im = Image.frombuffer("RGBA", PANEL, data, "raw", "BGRA", 0, 1).convert("RGB").rotate(-90, expand=True)
        path = os.path.join(self.dir, "shots", f"{label}_{name}.png")
        im.save(path)
        return path

    def pair(self, label, paths):
        """The radios' pictures of one screen side by side, each named, into shots/<label>.png"""
        ims = [(n, Image.open(p)) for n, p in paths.items() if p]
        if not ims:
            return None
        w = sum(im.width for _, im in ims) + 8 * (len(ims) - 1)
        out = Image.new("RGB", (w, ims[0][1].height + 22), (40, 40, 40))
        d = ImageDraw.Draw(out)
        x = 0
        for n, im in ims:
            out.paste(im, (x, 22))
            d.text((x + 6, 4), f"{n.upper()}  {label}", fill=(255, 255, 255))
            x += im.width + 8
        path = os.path.join(self.dir, "shots", f"{label}.png")
        out.save(path)
        return path


def palette(path):
    """A picture's colours against the Sentry's gold on black, in % of its pixels: dark (black), warm (the gold and
    the waterfall's gradient down to its dark orange and red, hues 0-65 and 345-360 degrees), grey (no saturation:
    greys and white), status (green, the theme's OK: D's OPEN) and off (cyan, blue, purple: RF.Guru's and the other
    old themes' accents, what a screen not in the Sentry's look shows)"""
    im = Image.open(path).convert("RGB").resize((400, 240))
    n = dark = warm = grey = status = off = 0
    for r, g, b in im.getdata():
        n += 1
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        deg = h * 360
        if v < 0.12:
            dark += 1
        elif s < 0.25:
            grey += 1
        elif deg <= 65 or deg >= 345:
            warm += 1
        elif 90 <= deg <= 150:
            status += 1
        else:
            off += 1
    return {k: round(100 * x / n, 1) for k, x in (("dark", dark), ("warm", warm), ("grey", grey), ("status", status),
                                                   ("off", off))}


# ---- core: both radios, no hardware ----

def ssh(run, name, cmd, timeout=60):
    return bench.ssh(run.host(name), cmd, timeout)


def t_identity(run):
    got = {}
    for n in run.names:
        out = ssh(run, n, "cat /etc/openx6x00-image; uname -r; strings /usr/sbin/x6100_gui | grep -m1 '^GoldCorpHF - v';"
                          " cat /usr/share/x6100/app-version")
        lines = [x.strip() for x in out.splitlines() if x.strip()]
        kv = dict(x.split("=", 1) for x in lines if "=" in x)
        rest = [x for x in lines if "=" not in x]
        got[n] = {"model": kv.get("model"), "level": kv.get("level"), "image": kv.get("version"),
                  "kernel": rest[0] if rest else "", "gui": rest[1] if len(rest) > 1 else "",
                  "bundle": rest[2] if len(rest) > 2 else ""}
        run.result("core", "identity", True, f"{n}: {json.dumps(got[n])}", radio=n)
    guis = {g["gui"] for g in got.values()}
    run.result("core", "same-version", len(guis) == 1, f"the GUIs' versions: {sorted(guis)}")
    return got


PORTS = {22: "ssh", 80: "web", 443: "web (https)", 4532: "rigctl", 50001: "WSJT-X/UDP?", 50002: "browser remote"}
PROCS = ("x6100_gui", "x6x00-gui-watch", "x6100_webserver", "gpsd", "NetworkManager", "pulseaudio", "x6x00-rtlspec")


def t_services(run):
    seen = {}
    for n in run.names:
        out = ssh(run, n, "netstat -ltn 2>/dev/null; echo ---; ps w")
        net, _, ps = out.partition("---")
        ports = {int(m) for m in re.findall(r":(\d+)\s+\S+\s+LISTEN", net)}
        procs = {p for p in PROCS if p in ps}
        seen[n] = (ports, procs)
        miss_p = [f"{p} ({w})" for p, w in PORTS.items() if p not in ports]
        miss_r = [p for p in PROCS if p not in procs and p != "x6x00-rtlspec"]
        run.result("core", "services", not miss_p and not miss_r,
                   f"{n}: " + ("all up" if not miss_p and not miss_r else
                               f"missing ports {miss_p} processes {miss_r}"), radio=n)
    if len(run.names) == 2:
        a, b = run.names
        diff = sorted(seen[a][0] ^ seen[b][0])
        run.result("core", "same-ports", not diff, f"listening on one radio only: {diff}" if diff else "the same ports")


def params(run, n):
    out = ssh(run, n, "sqlite3 -separator '|' /mnt/params.db 'select name,val from params'")
    return dict(line.split("|", 1) for line in out.splitlines() if "|" in line)


MODE_ROWS = ("filter_low", "filter_high")   # per mode (mode_params): the AM filter 0-5000 on one, 0-4000 on the other


def mode_params(run, n):
    """The per-mode rows compared and aligned as well, as "mode<N>.<name>" (the X6200's AM filter 0-5000 against the
    X6100's 0-4000, 2026-11-18, was a value kept on one radio, not a default: params alone did not show it)"""
    names = ",".join(f"'{x}'" for x in MODE_ROWS)
    out = ssh(run, n, f"sqlite3 -separator '|' /mnt/params.db \"select mode,name,val from mode_params where name in "
                      f"({names})\"")
    rows = {}
    for line in out.splitlines():
        parts = line.split("|")
        if len(parts) == 3 and parts[0].isdigit():
            rows[f"mode{parts[0]}.{parts[1]}"] = parts[2]
    return rows


def t_settings(run):
    if len(run.names) != 2:
        return
    a, b = run.names
    pa, pb = params(run, a), params(run, b)
    keys = sorted(set(pa) | set(pb))
    diff = [(k, pa.get(k, "(default)"), pb.get(k, "(default)")) for k in keys
            if pa.get(k) != pb.get(k) and not OWN.search(k)]
    ma, mb = mode_params(run, a), mode_params(run, b)
    diff += [(k, ma.get(k, "(default)"), mb.get(k, "(default)")) for k in sorted(set(ma) | set(mb))
             if ma.get(k) != mb.get(k)]
    with open(os.path.join(run.dir, "settings_diff.json"), "w") as f:
        json.dump([{"name": k, a: x, b: y} for k, x, y in diff], f, indent=1)
    run.result("core", "same-settings", None if diff else True,
               f"{len(diff)} look/behaviour settings differ (settings_diff.json): " +
               ", ".join(f"{k}={x}/{y}" for k, x, y in diff[:12]) + (" ..." if len(diff) > 12 else ""))


def t_apps(run):
    lists = {}
    for n in run.names:
        r = run.radio(n)
        r.apps = None
        r.send("apps")
        r.wait("its apps", lambda r=r: r.apps is not None, 8)
        lists[n] = {int(i): name for i, name in r.apps}
        run.result("core", "apps", True, f"{n}: {len(lists[n])} apps", radio=n)
    if len(run.names) == 2:
        a, b = run.names
        only = sorted(set(lists[a]) ^ set(lists[b]))
        run.result("core", "same-apps", not only,
                   "the same apps" if not only else f"on one radio only: {[lists[a].get(i) or lists[b].get(i) for i in only]}")
    return lists


def gate_rx(r):
    """The radio held receive only while the suite has control: every way to the air refused there"""
    if r.may_tx or int(r.state.get("tx", 0)) or int(r.state.get("rxo", 0)) != 1:
        raise bench.Refused(f"{r.name}: not held receive only (rxo {r.state.get('rxo')}, tx {r.state.get('tx')})")


DIFF_LEVEL = 48                 # a pixel's brightness apart (0-255) for it to count as different
DIFF_SHARE = 0.15               # more of the screen than this different: shown to look at


def picture_diff(paths):
    """The share of two pictures' pixels whose brightness differs by more than DIFF_LEVEL (None without two)"""
    if Image is None or len(paths) != 2:
        return None
    a, b = (Image.open(p).convert("L") for p in paths)
    if a.size != b.size:
        b = b.resize(a.size)
    pa, pb = a.getdata(), b.getdata()
    n = sum(1 for x, y in zip(pa, pb) if abs(x - y) > DIFF_LEVEL)
    return n / max(len(pa), 1)


def screens_pair(run, label, setup, check):
    """setup(r) on each radio; check(r) true on each within 25 s (the X6100's SSTV took 9-17 s to open, 2026-11-18,
    and over 15 s on 2026-11-22, when the 15 s wait called it shut although its picture showed it open); both
    photographed, side by side, colours counted"""
    paths, shut = {}, []
    run.banner(f"SUITE core: screen {label} on both, photographed side by side (receive only, nothing sent)")
    for n in run.names:
        r = run.radio(n)
        setup(r)
    for n in run.names:
        r = run.radio(n)
        try:
            r.wait(label, lambda r=r: check(r), 25)
        except bench.Refused:
            shut.append(n)
        gate_rx(r)
    time.sleep(1.5)                                     # drawn once more after it opened
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        futs = {n: ex.submit(run.shot, n, label) for n in run.names}
        paths = {n: f.result() for n, f in futs.items()}
    pair = run.pair(label, paths)
    pal = {n: palette(p) for n, p in paths.items() if p}
    off = {n: p["off"] for n, p in pal.items()}
    flag = [n for n, v in off.items() if v > OFF_PALETTE_FLAG]
    m = re.match(r"app(\d+)_", label)
    need = NEEDS.get(int(m.group(1))) if m else None
    if shut:
        run.result("core", f"screen {label}", None if need else False,
                   f"did not open on {shut}" + (f": it needs {need}" if need else ""),
                   shot=os.path.relpath(pair, run.dir) if pair else None, palette=pal)
        return
    run.result("core", f"screen {label}", None if flag else True,
               f"cool colour (old look) {off}" + (f": {flag} not all gold on black" if flag else ""),
               shot=os.path.relpath(pair, run.dir) if pair else None, palette=pal)
    # 1:1 as pictures, not only as colours (the owner, 2026-11-22: "EVERYTHING 1:1 ... from GUI look and feel"): the
    # share of the screen whose brightness differs by more than DIFF_LEVEL between the radios' pictures; the model's
    # name, the clock, live spectra and S-meters differ always, so over DIFF_SHARE is shown to look at, not failed
    d = picture_diff([p for p in paths.values() if p])
    if d is not None:
        run.result("core", f"alike {label}", True if d <= DIFF_SHARE else None,
                   f"{d:.1%} of the screen differs between the radios' pictures (look at it over {DIFF_SHARE:.0%})",
                   shot=os.path.relpath(pair, run.dir) if pair else None, covers=["lookfeel-pair-pixel-diff"])


def t_screens(run, apps):
    names = {}
    for lst in apps.values():
        names.update(lst)
    for i, name in sorted(names.items()):
        if i in NEVER_OPEN or i == 129 or not all(i in apps[n] for n in run.names):
            continue                  # never opened here, the Sentry (its pages below), or on one radio only
        label = re.sub(r"[^A-Za-z0-9]+", "_", f"app{i}_{name or 'menu'}").strip("_")
        # a window with no name (the Modes menu): opened when the Sentry, the home screen, is no longer what shows
        screens_pair(run, label, lambda r, i=i: r.send(f"open {i}"),
                     lambda r, name=name: r.state.get("win") == name if name else r.state.get("win") != "Sentry")
        for n in run.names:
            run.radio(n).send("close")
        time.sleep(1.0)


def t_pages(run):
    for p in PAGES + "H":
        screens_pair(run, f"sentry_{p}", lambda r, p=p: r.send(f"open sentry {p}"),
                     (lambda r: r.state.get("win") == "Airspace") if p == "H" else
                     (lambda r, p=p: r.state.get("pg") == p))
    for n in run.names:
        run.radio(n).send("open sentry A")


def run_child(cmd, timeout):
    """A bench test as a child: past timeout it gets SIGINT first, so its emergency stop and its finish() still put the
    radios back (a plain timeout killed it outright: nothing back, the 2026-11-22 audit); 60 s later, killed"""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGINT)
        try:
            out, err = p.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            p.kill()
            out, err = p.communicate()
        err = (err or "") + f"\n(stopped after {timeout} s)"
    return subprocess.CompletedProcess(cmd, p.returncode, out or "", err or "")


def t_gesture(run):
    """The X6100's stand-in for the X6200's MFK inner ring: the MFK held, the main dial turned one click, let go: the
    next page, the dial not tuned, no push. Through the remote's keys and knobs, which reach the GUI as the radio's"""
    for n in run.names:
        if run.c["radios"][n].get("model") != "x6100":
            continue
        r = run.radio(n)
        r.send("open sentry A")
        r.wait("page A", lambda: r.state.get("pg") == "A", 8)
        f0 = int(r.state.get("freq", 0))
        r.send("key MFK 1")
        time.sleep(0.3)
        r.send("knob main 1")
        time.sleep(0.6)
        r.send("key MFK 0")
        time.sleep(1.2)
        pg, f1 = r.state.get("pg"), int(r.state.get("freq", 0))
        run.result("core", "mfk-held-dial", pg == "B" and f1 == f0,
                   f"{n}: page A -> {pg} (B wanted), the dial {f0} -> {f1} (unchanged wanted)", radio=n)
        r.send("open sentry A")
        if f1 != f0:            # the test failed by tuning: the dial back (the 2026-11-22 audit)
            r.send(f"set freq {f0}")
            r.wait("the dial back", lambda: int(r.state.get("freq", 0)) == f0, 6)


def t_web(run):
    for n in run.names:
        url = f"http://{run.host(n)}/"
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"Host": run.host(n)}), timeout=15) as resp:
                ok, says = resp.status == 200, f"{url} {resp.status}"
        except OSError as e:
            ok, says = False, f"{url}: {e}"
        run.result("core", "web", ok, f"{n}: {says}", radio=n)


def t_hold(run):
    for n in run.names:
        r = run.radio(n)
        r.take_control()
        time.sleep(0.5)
        run.result("core", "rx-hold", r.state.get("rxo") == 1 and not r.may_tx,
                   f"{n}: receive-only session in control, the radio's hold rxo={r.state.get('rxo')}", radio=n)


def core(run, only):
    tests = [("identity", t_identity), ("services", t_services), ("settings", t_settings), ("web", t_web),
             ("hold", t_hold)]
    for name, fn in tests:
        if not only or name in only:
            run.banner(f"SUITE core: {name} (both radios receive only, nothing sent)")
            fn(run)
    if not only or {"apps", "screens", "pages", "gesture"} & set(only):
        for n in run.names:
            run.radio(n).take_control()
        run.banner("SUITE core: the apps each radio has (receive only)")
        apps = t_apps(run)
        keep = None
        if len(run.names) == 2 and (not only or {"pages", "screens"} & set(only)):
            src, dst = ("x6200", "x6100") if set(run.names) == {"x6100", "x6200"} else run.names
            run.banner(f"SUITE core: {dst} made like {src} for the pictures (its own back after; receive only)")
            keep = photo_sync(run, src, dst)
        try:
            if not only or "pages" in only:
                t_pages(run)
            if not only or "gesture" in only:
                t_gesture(run)
            if not only or "screens" in only:
                t_screens(run, apps)
        finally:
            if keep is not None:
                photo_undo(run, dst, keep)


# ---- the hardware batches: the bench harness's tests ----

def rtl(run, only):
    """AIRSPACE on both at once for 90 s: aircraft and messages; the sticks' antennas side by side"""
    for n in run.names:
        run.radio(n).take_control()
        run.radio(n).send("open sentry H")
    run.banner("SUITE rtl: AIRSPACE on both for 90 s, the RTL-SDRs side by side (receive only, nothing sent)")
    time.sleep(90)
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        paths = {n: f.result() for n, f in {n: ex.submit(run.shot, n, "rtl_airspace") for n in run.names}.items()}
    pair = run.pair("rtl_airspace", paths)
    run.result("rtl", "airspace", None, "both radios' AIRSPACE after 90 s (the rates in the picture)",
               shot=os.path.relpath(pair, run.dir) if pair else None)
    for n in run.names:
        run.radio(n).send("open sentry A")


def back(run, n, secs=90):
    """n's GUI up and its remote answering again after a GUI restart, within secs"""
    end = time.time() + secs
    time.sleep(10)
    while time.time() < end:
        try:
            if ssh(run, n, "pidof x6100_gui x6200_gui >/dev/null && echo up", timeout=15).strip() == "up":
                time.sleep(5)                           # its remote server's port
                return True
        except (bench.Refused, OSError, subprocess.SubprocessError):
            pass
        time.sleep(5)
    return False


def cable(run, only):
    """radios-ft8 both ways with the receivers alike, at 0.1 W: test callsigns set first and any ATU off (it tunes by
    transmitting), all put back after"""
    run.close()
    run.radios = {}

    class A:
        pass

    calls, atu = {}, {}
    try:
        # the GUIs restart for the test callsigns: the banner comes with each step's own connection (bench.py's
        # radios-ft8 sets its own: which radio sends, which listens)
        for n, call in zip(run.names, ("Q1TST", "Q2TST")):
            a = A()
            a.name, a.call = n, call
            bench.cmd_testcall(run.c, a)
            calls[n] = True
        for n in run.names:                             # the GUIs started again; the network too (TODO 10)
            if not back(run, n):
                raise bench.Refused(f"{n} not back on the network 90 s after its GUI restart (TODO 10)")
        for n in run.names:
            r = bench.Radio(n, run.c["radios"][n], want_tx=True)
            time.sleep(1)
            r.take_control()
            atu[n] = r.state.get("atu")
            if atu[n]:
                r.send("set atu 0")
                r.wait("ATU off", lambda r=r: not r.state.get("atu"), 5)
            r.close()
        for tx, rx in ((run.names[0], run.names[1]), (run.names[1], run.names[0])):
            cmd = [sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench.py"),
                   "radios-ft8", tx, rx, "--band", "20m", "--rx-alike"]
            r = run_child(cmd, 600)
            out = (r.stdout + r.stderr).strip().splitlines()
            with open(os.path.join(run.dir, f"ft8_{tx}_{rx}.txt"), "w") as f:      # the whole run, kept
                f.write("\n".join(out) + "\n")
            # both transmissions' levels and decodes (the last three lines alone lost the CQ's, 2026-11-19)
            keep = [x for x in out if " sent " in x or " decoded " in x or "PASS" in x or "STOP" in x or "Refused" in x]
            run.result("cable", f"ft8 {tx}->{rx}", r.returncode == 0 and any("PASS" in x for x in out),
                       " | ".join(x.split(" ", 1)[-1] for x in (keep or out[-3:])))
    finally:
        # each radio put back on its own: one off the network must not leave the other with a test callsign
        for n in calls:
            a = A()
            a.name, a.call = n, "restore"
            try:
                bench.cmd_testcall(run.c, a)
            except (bench.Refused, OSError, subprocess.SubprocessError) as e:
                run.result("cable", "restore", False, f"{n}: its own callsign not put back ({e}): "
                           f"bench.py testcall {n} restore", radio=n)
        for n in calls:
            back(run, n)
        for n, was in atu.items():
            if was:
                try:
                    r = bench.Radio(n, run.c["radios"][n], want_tx=True)
                    time.sleep(1)
                    r.take_control()
                    r.send("set atu 1")
                    r.close()
                except (bench.Refused, OSError) as e:
                    run.result("cable", "restore", False, f"{n}: ATU not switched on again ({e})", radio=n)


def band_rows(run, n):
    """Each band's memories (band_params: the VFOs' frequency and mode on it...), as "band<ID>.<name>": an app that
    goes to its band takes them (SSTV opened on 14.0627 CW on one radio, 14.230 USB on the other, 2026-11-19)"""
    out = ssh(run, n, "sqlite3 -separator '|' /mnt/params.db 'select bands_id,name,val from band_params'")
    rows = {}
    for line in out.splitlines():
        parts = line.split("|")
        if len(parts) == 3 and parts[0].isdigit():
            rows[f"band{parts[0]}.{parts[1]}"] = parts[2]
    return rows


def gui_state(run, n):
    out = ssh(run, n, "cat /mnt/gui_state.conf 2>/dev/null")
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def photo_sync(run, src, dst):
    """dst made like src for the pictures (PHOTO_OWN, STATE_OWN): its params rows, per-mode rows and GUI state
    written with its GUI stopped (it writes both back as it ends), then started again and back on the network; what
    it had kept for photo_undo"""
    ps, pd = params(run, src), params(run, dst)
    rows = [(k, pd.get(k), ps.get(k)) for k in sorted(set(ps) | set(pd))
            if ps.get(k) != pd.get(k) and not PHOTO_OWN.search(k)]
    ms, md = mode_params(run, src), mode_params(run, dst)
    rows += [(k, md.get(k), ms.get(k)) for k in sorted(set(ms) | set(md)) if ms.get(k) != md.get(k)]
    bs, bd = band_rows(run, src), band_rows(run, dst)
    rows += [(k, bd.get(k), bs.get(k)) for k in sorted(set(bs) | set(bd))
             if bs.get(k) != bd.get(k) and not PHOTO_OWN.search(k.split(".", 1)[1])]
    gs, gd = gui_state(run, src), gui_state(run, dst)
    state = {k: v for k, v in gs.items() if not STATE_OWN.match(k)}
    keep = {"rows": [{"name": k, "was": was, "to": to} for k, was, to in rows], "state": gd}
    with open(os.path.join(run.dir, f"photo_sync_{dst}_undo.json"), "w") as f:
        json.dump(keep, f, indent=1)
    write_rows(run, dst, keep["rows"], dict(gd, **state))
    say(f"{dst}: made like {src} for the pictures: {len(rows)} settings, {len(state)} GUI state keys")
    return keep


def photo_undo(run, dst, keep):
    """dst's own values back after the pictures"""
    write_rows(run, dst, [{"name": r["name"], "was": r["to"], "to": r["was"]} for r in keep["rows"]], keep["state"])
    say(f"{dst}: its own settings and GUI state back")


def write_rows(run, dst, rows, state=None):
    """rows ({name, to}; to None deletes) into dst's params.db, and its GUI state file whole when given, its GUI
    stopped meanwhile and started again; back when its remote answers"""
    sql = []
    for r in rows:
        m = re.fullmatch(r"mode(\d+)\.(\w+)", r["name"])
        if m and m.group(2) in MODE_ROWS:
            if r["to"] is None:
                sql.append(f"delete from mode_params where mode={int(m.group(1))} and name='{m.group(2)}';")
            elif re.fullmatch(r"-?\d+", str(r["to"])):
                sql.append(f"insert or replace into mode_params(mode,name,val) values({int(m.group(1))},"
                           f"'{m.group(2)}',{int(r['to'])});")
            continue
        b = re.fullmatch(r"band(\d+)\.(\w+)", r["name"])
        if b:                                           # a band's memory (band_params: bands_id, name, val)
            if r["to"] is None:
                sql.append(f"delete from band_params where bands_id={int(b.group(1))} and name='{b.group(2)}';")
            elif re.fullmatch(r"-?\d+", str(r["to"])):
                sql.append(f"insert or replace into band_params(bands_id,name,val) values({int(b.group(1))},"
                           f"'{b.group(2)}',{int(r['to'])});")
            continue
        k = r["name"].replace("'", "''")
        if r["to"] is None:
            sql.append(f"delete from params where name='{k}';")
        else:
            v = str(r["to"]).replace("'", "''")
            sql.append(f"insert or replace into params(name,val) values('{k}','{v}');")
    st = ""
    if state is not None:
        body = "".join(f"{k}={v}\n" for k, v in state.items() if re.fullmatch(r"[\w.]+", k) and "'" not in v)
        st = f"printf '%s' '{body}' > /mnt/gui_state.conf; "
    db = f"sqlite3 /mnt/params.db \"{''.join(sql)}\"; " if sql else ""
    if run.radios.get(dst):
        run.radios.pop(dst).close()
    ssh(run, dst, f"/etc/init.d/S95gui stop >/dev/null 2>&1; {db}{st}/etc/init.d/S95gui start >/dev/null 2>&1",
        timeout=120)
    t = time.time()
    while True:                         # its remote back (the GUI's start, then the Sentry's)
        try:
            time.sleep(3)
            run.radio(dst).take_control()
            break
        except (bench.Refused, OSError):
            run.radios.pop(dst, None)
            if time.time() - t > 120:
                raise


def align(names, src, dst, undo=None):
    """src's look-and-behaviour settings (every row core's same-settings lists) written into dst's params.db, dst's
    GUI stopped meanwhile (it writes its own back as it ends) and started again; dst's rows before kept for undo"""
    run = Run(names, ["align"])
    if undo:
        rows = json.load(open(undo))
    else:
        ps, pd = params(run, src), params(run, dst)
        rows = [{"name": k, "was": pd.get(k), "to": ps.get(k)} for k in sorted(set(ps) | set(pd))
                if ps.get(k) != pd.get(k) and not OWN.search(k)]
        ms, md = mode_params(run, src), mode_params(run, dst)
        rows += [{"name": k, "was": md.get(k), "to": ms.get(k)} for k in sorted(set(ms) | set(md))
                 if ms.get(k) != md.get(k)]
        path = os.path.join(run.dir, f"align_{dst}_undo.json")
        json.dump([{"name": r["name"], "was": r["to"], "to": r["was"]} for r in rows], open(path, "w"), indent=1)
        say(f"{dst}'s values before: {path} (suite.py align {src} {dst} --undo {path})")
    sql = []
    for r in rows:
        m = re.fullmatch(r"mode(\d+)\.(\w+)", r["name"])
        if m and m.group(2) in MODE_ROWS:              # a per-mode row (mode_params: mode, name, val)
            if r["to"] is None:
                sql.append(f"delete from mode_params where mode={int(m.group(1))} and name='{m.group(2)}';")
            elif re.fullmatch(r"-?\d+", str(r["to"])):
                sql.append(f"insert or replace into mode_params(mode,name,val) values({int(m.group(1))},"
                           f"'{m.group(2)}',{int(r['to'])});")
            continue
        k = r["name"].replace("'", "''")
        if r["to"] is None:
            sql.append(f"delete from params where name='{k}';")
        else:
            v = str(r["to"]).replace("'", "''")
            sql.append(f"insert or replace into params(name,val) values('{k}','{v}');")
    if not sql:
        say("nothing to align")
        return 0
    script = "".join(sql)
    # the time zone too: the clock's POSIX TZ string, /mnt/tz.conf (clock.c), not a params row
    tz = "" if undo else ssh(run, src, "cat /mnt/tz.conf 2>/dev/null").strip()
    script_tz = f"echo '{tz}' > /mnt/tz.conf; " if tz and re.fullmatch(r"[A-Za-z0-9<>+\-:,./]+", tz) else ""
    ssh(run, dst, f"/etc/init.d/S95gui stop >/dev/null 2>&1; {script_tz}sqlite3 /mnt/params.db \"{script}\"; "
                  f"/etc/init.d/S95gui start >/dev/null 2>&1", timeout=120)
    if script_tz:
        say(f"{dst}: time zone {tz}")
    for r in rows:
        say(f"{dst}: {r['name']} {r['was']} -> {r['to']}")
    return 0


def update(run, only):
    """Each radio's updater asked what is newest (x6100-update check: it reads, changes nothing): the newest release
    for its model, newer than what runs, and whether that one wants a card image"""
    seen = {}
    for n in run.names:
        try:
            j = json.loads(ssh(run, n, "x6100-update check", timeout=180).strip().splitlines()[-1])
        except (bench.Refused, ValueError, IndexError) as e:
            run.result("update", "check", False, f"{n}: {e}", radio=n)
            continue
        seen[n] = j.get("latest")
        run.result("update", "check", bool(j.get("url")),
                   f"{n}: running {j.get('running')}, latest {j.get('latest')} (newer {j.get('newer')}, "
                   f"card image needed {j.get('image_needed')}), {j.get('url') or 'no bundle for it'}", radio=n)
    if len(seen) == 2:
        run.result("update", "same-latest", len(set(seen.values())) == 1, f"the newest each is offered: {seen}")


BATCHES = {
    "core": ("both radios on the network", core),
    "rtl": ("an RTL-SDR on each HOST port, antennas side by side", rtl),
    "cable": ("the radios joined through >= 60 dB (bench.py chain-ok radios), test callsigns", cable),
    "update": ("a release published (the updater's check on each radio; nothing installed)", update),
    # one test per feature, each radio and 1:1 (suite_more.py; the owner, 2026-11-22: "EVERYTHING needs validation")
    "rx": ("both radios on the network (receive only: every receiver setting, D's and E's rule)", suite_more.rx),
    "control": ("both radios on the network (TCI, rigctl, every web page, the remote's protocol and guards)",
                lambda run, only: (suite_more.control(run, only), suite_proto.control(run, only))),
    "system": ("both radios over SSH (read only: logs, settings copy, services, sessions, update state)",
               suite_proto.system),
    "safety": ("the cable (bench.py chain-ok radios): the transmitter's locks, 0.1 W at most", suite_more.safety),
    "paths": ("the cable: tones, the mic on G, CW, AM and FM, both ways at 0.1 W", suite_more.paths),
    "look": ("both radios on the network: every Settings tab photographed side by side, compared", suite_more.look),
}


def report(run):
    ok = sum(1 for x in run.results if x["ok"] is True)
    warn = sum(1 for x in run.results if x["ok"] is None)
    bad = sum(1 for x in run.results if x["ok"] is False)
    with open(os.path.join(run.dir, "report.json"), "w") as f:
        json.dump(run.results, f, indent=1)
    lines = [f"# GoldCorpHF two-radio suite, {time.strftime('%Y-%m-%d %H:%M')}", "",
             f"{ok} pass, {warn} to look at, {bad} fail; radios {', '.join(run.names)}", ""]
    for x in run.results:
        mark = "PASS" if x["ok"] is True else "LOOK" if x["ok"] is None else "FAIL"
        lines.append(f"- **{mark}** {x['batch']}/{x['test']}: {x['says']}")
        if x.get("shot"):
            lines.append(f"  ![{x['test']}]({x['shot']})")
    with open(os.path.join(run.dir, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    suite_more.coverage(run)            # every feature of features.json: covered and passed, failed, or not yet
    say(f"{ok} pass, {warn} to look at, {bad} fail: {os.path.join(run.dir, 'report.md')}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    s = sub.add_parser("run")
    s.add_argument("batches")
    s.add_argument("--only", default="", help="core's tests by name, comma-separated")
    s.add_argument("--radios", default="x6100,x6200")
    sub.add_parser("release")
    sub.add_parser("coverage", help="every feature against the latest result of each test of every run so far")
    s = sub.add_parser("align")
    s.add_argument("src")
    s.add_argument("dst")
    s.add_argument("--undo", default=None)
    a = ap.parse_args()
    if a.cmd == "align":
        return align([a.src, a.dst], a.src, a.dst, a.undo)
    if a.cmd == "coverage":
        suite_more.coverage_all()
        return 0
    if a.cmd == "list":
        for k, (needs, fn) in BATCHES.items():
            print(f"{k:8s} needs {needs}")
        return 0
    batches = a.batches.split(",") if a.cmd == "run" else ["core"] + bench.conf().get("attached", [])
    run = Run((a.radios if a.cmd == "run" else "x6100,x6200").split(","), batches)
    only = [x for x in getattr(a, "only", "").split(",") if x]
    try:
        for b in batches:
            if b not in BATCHES:
                raise bench.Refused(f"no batch {b} (suite.py list)")
            BATCHES[b][1](run, only)
    except bench.Refused as e:
        run.result("suite", "stopped", False, str(e))
    finally:
        for n in list(run.radios):
            try:
                run.radios[n].send("open sentry A")
            except OSError:
                pass
        run.close()
    return 1 if report(run) else 0


if __name__ == "__main__":
    sys.exit(main())
