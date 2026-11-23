"""The 1:1 suite's "cover_a" batch: settings and the display, features of features.json not reached by the other batches,
each tried on both radios and compared, receive only (see suite_cover.py for the rules and the helpers). Whatever a test
changes beyond per_radio's snapshot (a Display setting, a key setting, a band, a channel, the Bluetooth window) it puts
back itself in a finally block and reads back; every radio is left on the Sentry's page A. A test that turns the
main knob opens the Sentry again after it (sentry_again: the knob's MANUAL undone); one that loses control to someone
at the radio stops (ControlLost), never taking it back; a radio with the update question up (box_up: the owner's to
answer) is not photographed, its screen tests NOT RUN.

    suite.py run cover_a [--only TEST,...]

The tests (the features each covers in brackets):
  btcat           the CAT bridge: btcat holds the real UART (a tty, major 4), a pty bind-mounted over /dev/ttyS2, RFCOMM
                  listening, both logs [btcat-bridge]; the adapter's Powered as the BTCAT_BT switch has it
                  [btcat-adapter-power]
  bt-pairing      the pairing window (btcat-ctl, what Settings' Pairing runs): hidden and not pairable outside it,
                  visible and pairable with 180 s to go in it, hidden again when closed [btcat-pairing-window]
  backlight       Display > BACKLIGHT from the web: the light at its level, dimmed to the idle level after the 5 s
                  timeout, back at a knob turn; Off never dims; a level under the dim level takes it down too
                  [settings-display-backlight]
  screen-off      Screen always off: the panel dark at once (bl_power), the remote's meters and state go on, lit again
                  when off [settings-display-screen-off]
  screensaver     both radios at once (the setting's least is a minute, ~80 s) and with no remote client connected (any
                  client is a viewer, which keeps it away): Matrix rain over the GUI after 1 min; under it TCI's audio
                  at its rate and the GUI's main thread lighter than lit; a viewer connecting ends it at once, the GUI
                  drawn whole [display-screensaver, display-screensaver-radio-keeps-working,
                  display-screensaver-watched]
  themes          a theme from the web applied at once (params.db, the Sentry rebuilt in its colours: Outdoor's white),
                  1bit two colours only (ink and paper), the theme as found back [theme-select-live, theme-1bit]
  themes-alike    every built-in theme on both radios on the Sentry's A, photographed and compared radio to radio
                  [theme-builtins-render-alike]
  zoom-step       Zoom and step per mode: USB and CW keep their own; off, one zoom and step for all, on again each its
                  own [settings-display-zoom-step-per-mode]
  step-zoom       Step per zoom: zoom 1 brings 1000 Hz back, zoom 8 10 Hz; off, a zoom keeps the step
                  [settings-display-step-per-zoom]
  band-plan       the plan chosen on the web, the remote's 'plan': US General has NOT YOUR CLASS, US Extra none,
                  Region 2 phone to 7.3 MHz; as found again [settings-general-band-plan]
  own-band        a band of one's own ("BENCH 60m CW" 5258.5-5264 kHz, PUT /api/bands) gets a CW strip at the next band
                  change; taken out, its rows gone [main-screen-band-plan-own-bands]
  mode-from-plan  the knob down from DIG into a band's CW segment sets CW, up from DIG into phone USB; off, nothing;
                  all in one band (17 m here: 20 m is two bands at the CW edge, whose memories set the mode)
                  [settings-general-mode-from-band-plan]
  lock-per-band   FREQUENCY LOCK per band and mode: locked on 20 m USB, free on USB-D and on the next band, locked
                  again back on 20 m USB, the knob held [settings-keys-lock-per-band]
  channel-snap    Display > Channels: a channel half a step up stops the knob on it, with the marks off the knob steps
                  past [settings-display-channel-marks]
  speaker-off     Speaker always off from the web: the remote's spk 0 and muted at any volume, the volume kept; back
                  [settings-audio-speaker-always-off]
  line-in         'set linein': the state and params.db line_in, then as found [settings-audio-line-levels]
  transverters    both transverters off (no shift saved), no 4 m/2 m/70 cm band in the remote's 'bands', 144.3 MHz
                  neither received nor sent (band_use 0, 'set freq' refused) [transverter-off-by-default]
  tx-shift        a -600 kHz TX shift on 10 m from the web Bands page: on 29.6 MHz NFM the remote's TX frequency is
                  29.0 MHz with split on (nothing sent); shift 0 again, the band as found [settings-general-tx-shift]
  tz-alike        the same /mnt/tz.conf on both, their local clocks the same [lookfeel-time-zone-alike]
  tx-outside      TX outside ham bands off on the web Display page, as by default [tx-outside-ham-bands-switch]

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import gzip
import json
import os
import shlex
import socket
import subprocess
import sys
import threading
import time

import bench
from suite_cover import by_radio, keep, wait_state
from suite_more import F_TEST, per_radio
from suite_proto import http, line_after

try:
    import numpy as np
    from PIL import Image
except ImportError:             # the pictures need Pillow and numpy; those tests say so and look
    np = Image = None

B = "cover_a"


def ssh(host, cmd, timeout=40):
    """suite_cover.ssh over IPv4 (-4): the radios' .local names have no IPv6 answer and each lookup cost ~5 s
    (bench.ssh's note, 2026-11-23), which ran the backlight's 5 s dim out before its first reading"""
    r = subprocess.run(["ssh", "-4", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout

# ---- helpers ----


def ip_of(r):
    return socket.gethostbyname(r.host)


def disp(ip):
    """The web Display page's settings (GET /api/display: the GUI's disp_get), {} when it does not answer"""
    st, body = http(ip, "/api/display")
    try:
        return json.loads(body) if st == 200 else {}
    except ValueError:
        return {}


def disp_set(ip, name, val):
    """One Display setting through the web page (POST /api/display, the GUI's disp_set): the HTTP status"""
    body = {"name": name, "theme": val} if name == "theme" else {"name": name, "val": int(val)}
    return http(ip, "/api/display", method="POST", body=body)[0]


def disp_back(ip, found, names):
    """names put back in that order as found had them (found: disp() before), read back: those not back"""
    for k in names:
        if found.get(k) is not None:
            disp_set(ip, k, found[k])
    time.sleep(0.4)
    now = disp(ip)
    return [k for k in names if found.get(k) is not None and now.get(k) != found[k]]


CFG_PY = ("import socket,sys\n"
          "s=socket.socket(socket.AF_UNIX);s.settimeout(4);s.connect('/tmp/x6100_cfg.sock')\n"
          "s.sendall(sys.argv[1].encode()+b'\\n');b=b''\n"
          "while not b.endswith(b'\\n'):\n"
          "    d=s.recv(65536)\n"
          "    if not d: break\n"
          "    b+=d\n"
          "print(b.decode(errors='replace').strip())\n")


def cfg(h, obj):
    """One request to the GUI's configurator socket on the radio (src/cfg_socket.cpp), over SSH: its answer, {} none"""
    out = ssh(h, "python3 -c " + shlex.quote(CFG_PY) + " " + shlex.quote(json.dumps(obj))).strip()
    try:
        return json.loads(out.splitlines()[-1]) if out else {}
    except ValueError:
        return {}


def sql(h, q):
    """A read (or a cleanup) of the radio's params.db with sqlite3, waiting up to 3 s for the GUI's own writes"""
    return ssh(h, "sqlite3 -cmd '.timeout 3000' -separator '|' /mnt/params.db " + shlex.quote(q)).strip()


class ControlLost(Exception):
    """The bench's session lost control to someone else (the radio's own keys and knobs give it to the operator at
    the radio, who has the last word): the test stops there, control is never taken back while it runs"""


def ctl_check(r, where):
    if getattr(r, "ctl_mine", True) is False:
        raise ControlLost(f"{r.name}: control went to someone else ({where})")


def setk(r, key, val, secs=6.0):
    """'set KEY VAL' and the state read back"""
    ctl_check(r, f"before 'set {key}'")
    r.send(f"set {key} {val}")
    return wait_state(r, lambda s: str(s.get(key)) == str(val), secs)


def zs(r):
    return int(r.state.get("zoom", 0)), int(r.state.get("step", 0))


def set_mode(r, m):
    """The mode, and a moment for its zoom and step (they load with it)"""
    ok = setk(r, "mode", m)
    time.sleep(0.6)
    return ok


def hello(r, secs=4.0):
    """The remote's band list and band plan as the radio has them now: the panel turned off and on, which sends both
    afresh ('bands [...]', 'plan [...]'). (bands, plan), each None when it did not come"""
    t0 = time.time()
    r.send("panel 0")
    time.sleep(0.25)
    r.send("panel 1")
    b = line_after(r, t0, "bands [", secs)
    p = line_after(r, t0, "plan [", secs)
    wait_state(r, lambda s: bool(s), 2.0)
    try:
        return (json.loads(b.split(" ", 1)[1]) if b else None), (json.loads(p.split(" ", 1)[1]) if p else None)
    except ValueError:
        return None, None


UPDATE_BOX = ("cat /mnt/update/announced 2>/dev/null; echo '<|>'; cat /usr/share/x6100/app-version 2>/dev/null; "
              "echo '<|>'; cat /mnt/update/check.json 2>/dev/null")


def box_up(h):
    """The version the update question ("Apply GoldCorpHF V?", src/update_watch.c) is up for, or is about to be (it
    comes ~2 min after a GUI start and stays till answered): a newer release found for the version running, with a
    bundle and no card image wanted, not yet answered (/mnt/update/announced). None when there is none. The box
    covers the screen and takes the MFK: it is the owner's to answer, never the bench's (2026-11-23), so a test that
    needs that radio's screen or MFK is not run there while it is up"""
    try:
        seen, ver, js = (x.strip() for x in ssh(h, UPDATE_BOX).split("<|>", 2))
        chk = json.loads(js) if js else {}
    except ValueError:
        return None
    latest = chk.get("latest") or ""
    if chk.get("newer") and latest and chk.get("running") == ver and not chk.get("image_needed") and \
            str(chk.get("url") or "").startswith("https://") and seen.splitlines()[:1] != [latest]:
        return latest
    return None


def box_says(v):
    return (f"NOT RUN (box up): the update question 'Apply GoldCorpHF {v}?' covers the screen and takes the MFK; it "
            f"is the owner's to answer, so the screen is not photographed under it")


def sentry_again(r):
    """After a test's own turns of the main knob: A out of MANUAL again. The VFO turned with the Sentry open (G, where
    per_radio runs a test, or A) puts A's scans in MANUAL (app_sentry.c x62_by_hand, MAN-OVERRIDE: the title's BROWSE
    became MANUAL on both radios after the backlight test, 2026-11-23) till A's F1 or a band key, which move the dial
    and A's band. The Sentry closed ('close': it is the home screen and comes back at once) and on A again takes A's
    mode from its saved state (state_restore: as SCAN CFG's style has it), the dial and its pages as they were. A
    Sentry found in MANUAL (a hand on the VFO before the test) would not stay so: the runs found BROWSE"""
    if getattr(r, "ctl_mine", True) is False:
        return False                    # control is someone else's: nothing sent (finish says what is left)
    try:
        bench.open_page(r, "A")
        r.send("close")
        wait_state(r, lambda s: s.get("pg") != "A", 3.0)
        time.sleep(0.5)
        bench.open_page(r, "A")
    except bench.Refused as e:
        bench.say(f"{r.name}: the Sentry not opened again on A ({e}): A may be left in MANUAL")
        return False
    return r.state.get("pg") == "A"


def knob_test(fn):
    """fn(r) of a test that turns the main knob: sentry_again after it, whatever it returned (a refusal after a
    detent too)"""
    def call(r):
        try:
            return fn(r)
        finally:
            sentry_again(r)
    return call


def knob_until(r, d, cond, most=10):
    """'knob main d' one detent at a time, slowly (no acceleration, the spot snap's slow turn), until cond(freq)"""
    for _ in range(most):
        ctl_check(r, "before a knob detent")
        f = int(r.state.get("freq", 0))
        r.send(f"knob main {d}")
        wait_state(r, lambda s, f=f: int(s.get("freq", 0)) != f, 1.5)
        time.sleep(0.4)
        if cond(int(r.state.get("freq", 0))):
            return True
    return False


# ---- the screen: the GUI's own dump (SIGUSR1, /tmp/screen.raw, as suite.Run.shot) ----

PANEL = (480, 800)
DUMP = ("rm -f /tmp/screen.raw; kill -USR1 $(pidof x6100_gui); "
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -f /tmp/screen.raw ] && break; sleep 0.2; done; "
        "sleep 0.3; gzip -1 -c /tmp/screen.raw")


def dump(run, h, label=None):
    """The screen as it is now (the suite's banner left as it is), landscape, as an RGB picture; saved into the run's
    shots/ as label. None when it could not be had"""
    if Image is None:
        return None
    try:
        r = subprocess.run(["ssh", "-4", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{h}", DUMP],
                           capture_output=True, timeout=40)
        data = gzip.decompress(r.stdout)
    except (subprocess.TimeoutExpired, OSError, EOFError):
        return None
    if len(data) != PANEL[0] * PANEL[1] * 4:
        return None
    im = Image.frombuffer("RGBA", PANEL, data, "raw", "BGRA", 0, 1).convert("RGB").rotate(-90, expand=True)
    if label:
        try:
            im.save(os.path.join(run.dir, "shots", f"{label}.png"))
        except OSError:
            pass
    return im


def shot(run, n, label):
    """suite.Run.shot (the suite's banner hidden for it): (the picture, its path), (None, None) without"""
    p = run.shot(n, label) if Image is not None else None
    return (Image.open(p).convert("RGB"), p) if p else (None, None)


def px(im):
    return np.asarray(im, dtype=np.int16)


def stable(a, b):
    """The lit pixels of a that b has the same: the screen's still parts (frames, labels), not its live ones"""
    pa, pb = px(a), px(b)
    return (np.abs(pa - pb).max(axis=2) < 24) & (pa.max(axis=2) > 60)


def kept(mask, a, c):
    """The share of mask's pixels that c has as a has them"""
    n = int(mask.sum())
    if not n:
        return 0.0
    same = np.abs(px(a) - px(c)).max(axis=2) < 24
    return float((same & mask).sum()) / n


def share_light(im):
    return float((px(im).min(axis=2) > 200).mean())


def share_dark(im):
    return float((px(im).max(axis=2) < 40).mean())


def pic_diff(pa, pb):
    """As suite.picture_diff: the share of two pictures' pixels whose brightness differs by more than 48"""
    a, b = (np.asarray(Image.open(p).convert("L"), dtype=np.int16) for p in (pa, pb))
    if a.shape != b.shape:
        return None
    return float((np.abs(a - b) > 48).mean())


def slug(name):
    return "".join(c if c.isalnum() else "_" for c in name).lower()


def both(a, b):
    """Two verdicts as one: a fail fails, a look (None) looks, else a pass"""
    a, b = (x if x is None else bool(x) for x in (a, b))
    return False if False in (a, b) else None if None in (a, b) else True


def guarded(fn):
    """fn's own errors (an SSH that timed out, an answer not as expected) as its result, never the batch's end; its
    finally blocks have put back what it changed by then"""
    def call(*a):
        r = a[0] if a and hasattr(a[0], "ctl_mine") else None
        try:
            out = fn(*a)
            if r is not None:
                ctl_check(r, "at the test's end")
            return out
        except ControlLost as e:
            return None, (f"STOPPED: {e}: the test stops here and its result is not judged; control is not taken "
                          f"back while it runs")
        except Exception as e:          # noqa: BLE001
            return False, f"error: {type(e).__name__} {e}" + (f" (the radio said: {a[0].errs[-3:]})"
                                                               if a and getattr(a[0], "errs", None) else "")
    return call


def theme_to(r, ip, name, secs=8.0):
    """The theme from the web page (the GUI closes the open window, the Sentry, and builds it again in the new
    colours): (HTTP status, the theme the page reads after). The Sentry's page A again, the dial on the test's"""
    st = disp_set(ip, "theme", name)
    now, t0 = None, time.time()
    while time.time() - t0 < secs:
        now = disp(ip).get("theme")
        if now == name:
            break
        time.sleep(0.3)
    time.sleep(1.0)
    if not wait_state(r, lambda s: s.get("pg") == "A", 5.0):
        bench.open_page(r, "A")
    bench.ensure(r, {"freq": F_TEST, "mode": 2})
    time.sleep(0.8)
    return st, now


def both_radios(run, test, covers, fn):
    """by_radio for a test that waits (the screensaver's minute): fn(name, host, ip) on each radio at once, without a
    remote session; a result per radio and one for the pair"""
    out = {}

    def one(n):
        h = run.host(n)
        try:
            ok, says = fn(n, h, socket.gethostbyname(h))
        except Exception as e:          # noqa: BLE001 (a thread's error: said, never lost)
            ok, says = False, f"error: {type(e).__name__} {e}"
        out[n] = (ok if ok is None else bool(ok), says)

    threads = [threading.Thread(target=one, args=(n,)) for n in run.names]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for n in run.names:
        ok, says = out.get(n, (False, "no result"))
        run.result(B, f"{test} {n}", ok, says, radio=n, covers=covers)
    if len(out) == 2:
        (a, (oa, _)), (b, (ob, _)) = out.items()
        run.result(B, f"{test} 1:1", both(oa, ob), f"{a} {'pass' if oa else 'look' if oa is None else 'FAIL'}, "
                   f"{b} {'pass' if ob else 'look' if ob is None else 'FAIL'}", covers=covers)


# ---- Bluetooth: the CAT bridge, the adapter's power, the pairing window (SSH) ----

BTCAT = r"""
p=$(pidof btcat); echo "pid=$p"
for f in /proc/$p/fd/*; do
  case "$(readlink $f)" in /dev/ttyS2) echo "fd=$(ls -lL $f | awk '{print $5 $6}')";; esac; done
echo "node=$(ls -l /dev/ttyS2 | awk '{print $5 $6}')"
findmnt -n -o SOURCE,FSTYPE /dev/ttyS2 | sed 's/^/mnt=/'
[ -s /tmp/btcat.log ] && echo log=1; [ -s /tmp/btcat-setup.log ] && echo setup=1
echo "rfcomm=$(grep -c 'RFCOMM listening' /tmp/btcat.log 2>/dev/null)"
echo "setup_last=$(tail -1 /tmp/btcat-setup.log 2>/dev/null)"
/usr/sbin/btcat-ctl status | sed 's/^/ctl_/'
bluetoothctl show | grep -E 'Powered:|Discoverable:|Pairable:|DiscoverableTimeout:' | sed 's/^[[:space:]]*/bt_/'
[ -f /tmp/btcat-pairing ] && echo "win_left=$(( $(cat /tmp/btcat-pairing) - $(cut -d. -f1 /proc/uptime) ))"
n=0; for m in $(bluetoothctl devices Paired 2>/dev/null | awk '{print $2}'); do
  bluetoothctl info $m 2>/dev/null | grep -q 'Trusted: yes' || n=$((n+1)); done; echo "untrusted=$n"
"""


def bt_read(h):
    """What BTCAT reads, as {key: value} (bt_Powered: yes, ctl_BTCAT_BT=yes...)"""
    kv = {}
    for ln in ssh(h, BTCAT).splitlines():
        ln = ln.strip()
        if ln.startswith("bt_") and ":" in ln:
            k, _, v = ln.partition(":")
            kv[k.strip()] = v.strip()
        elif "=" in ln:
            k, _, v = ln.partition("=")
            kv[k.strip()] = v.strip()
        elif ln.startswith("ctl_pairing window"):
            kv["ctl_window"] = ln.split(":", 1)[-1].strip()
    return kv


def major(dev):
    try:
        return int(dev.split(",")[0])
    except (ValueError, AttributeError):
        return None


def t_btcat(run):
    def bridge(n, h, ip):
        kv = bt_read(h)
        fd, node = major(kv.get("fd")), major(kv.get("node"))
        ok = bool(kv.get("pid")) and fd == 4 and node == 136 and "devpts" in kv.get("mnt", "") and \
            kv.get("log") == "1" and kv.get("setup") == "1" and kv.get("rfcomm", "0") not in ("", "0") and \
            kv.get("setup_last") == "btcat-setup done"
        return ok, (f"btcat pid {kv.get('pid') or 'NONE'}: its /dev/ttyS2 is {kv.get('fd')} (the UART, a tty: 4,N), "
                    f"/dev/ttyS2 now {kv.get('node')} (a pty: 136,N) mounted from {kv.get('mnt')!r}; /tmp/btcat.log "
                    f"{'there' if kv.get('log') else 'MISSING'} (RFCOMM listening {kv.get('rfcomm')}), "
                    f"/tmp/btcat-setup.log ends {kv.get('setup_last')!r}")
    by_radio(run, B, "btcat-bridge", ["btcat-bridge"], guarded(bridge))

    def power(n, h, ip):
        kv = bt_read(h)
        switch, powered = kv.get("ctl_BTCAT_BT"), kv.get("bt_Powered")
        ok = switch in ("yes", "no") and powered == switch
        return ok, (f"the ADAPTER switch (BTCAT_BT, /etc/default/btcat then /mnt/btcat.conf) {switch!r}, bluetoothctl "
                    f"Powered {powered!r} (read only)")
    by_radio(run, B, "btcat-power", ["btcat-adapter-power"], guarded(power))


def t_bt_pairing(run):
    def fn(n, h, ip):
        kv0 = bt_read(h)
        if kv0.get("ctl_BTCAT_BT") != "yes" or kv0.get("bt_Powered") != "yes":
            return None, f"Bluetooth off as found (BTCAT_BT {kv0.get('ctl_BTCAT_BT')}, Powered {kv0.get('bt_Powered')})"
        if "win_left" in kv0 or kv0.get("bt_Discoverable") == "yes":
            return None, "a pairing window open as found (someone pairing): left alone"
        if kv0.get("untrusted", "0") != "0":
            return None, (f"{kv0.get('untrusted')} paired devices not trusted: closing a window trusts and saves them, "
                          f"so none is opened")
        hidden0 = kv0.get("bt_Discoverable") == "no" and kv0.get("bt_Pairable") == "no"
        dto = kv0.get("bt_DiscoverableTimeout", "0x0")
        kv1, kv2 = {}, {}
        try:
            ssh(h, "/usr/sbin/btcat-ctl discoverable on >/dev/null 2>&1")
            time.sleep(1.5)
            kv1 = bt_read(h)
        finally:
            ssh(h, "/usr/sbin/btcat-ctl discoverable off >/dev/null 2>&1")
            try:
                if int(dto, 16) != 180:     # bluez's own timeout as found (btcat-ctl sets 180)
                    ssh(h, f"bluetoothctl discoverable-timeout {int(dto, 16)} >/dev/null 2>&1")
            except ValueError:
                pass
            time.sleep(1.0)
            kv2 = bt_read(h)
        try:
            left = int(kv1.get("win_left", "-1"))
        except ValueError:
            left = -1
        opened = kv1.get("bt_Discoverable") == "yes" and kv1.get("bt_Pairable") == "yes" and 170 <= left <= 181
        closed = kv2.get("bt_Discoverable") == "no" and kv2.get("bt_Pairable") == "no" and "win_left" not in kv2 \
            and kv2.get("ctl_window") == "closed"
        back = kv2.get("bt_DiscoverableTimeout") == dto
        ok = hidden0 and opened and closed and back
        return ok, (f"as found: discoverable {kv0.get('bt_Discoverable')}, pairable {kv0.get('bt_Pairable')}; "
                    f"btcat-ctl discoverable on: discoverable {kv1.get('bt_Discoverable')}, pairable "
                    f"{kv1.get('bt_Pairable')}, {left} s left of 180; off: discoverable {kv2.get('bt_Discoverable')}, "
                    f"pairable {kv2.get('bt_Pairable')}, window {kv2.get('ctl_window')}; bluez's timeout "
                    f"{kv2.get('bt_DiscoverableTimeout')} (was {dto})")
    by_radio(run, B, "bt-pairing", ["btcat-pairing-window"], guarded(fn))


# ---- the backlight, the screen off (the web Display page, the panel's sysfs) ----

BL = "cat /sys/class/backlight/backlight/brightness /sys/class/backlight/backlight/bl_power"


def light(h):
    """(the backlight's sysfs level, bl_power): these radios count the other way, sysfs = 10 - the GUI's level, and
    bl_power 1 is dark"""
    v = ssh(h, BL).split()
    return (int(v[0]), int(v[1])) if len(v) == 2 else (None, None)


LIGHT_KEYS = ("bright_normal", "bright_idle", "bright_timeout")


def t_backlight(run):
    def fn(r):
        ip, h = ip_of(r), r.host
        d0 = disp(ip)
        if any(d0.get(k) is None for k in LIGHT_KEYS):
            return False, f"the Display page has no backlight settings: {sorted(d0)[:8]}"
        if d0.get("screen_off_always"):
            return None, "Screen always off as found: the panel is dark, nothing to dim"
        res = {}
        try:
            disp_set(ip, "bright_idle", 2)
            disp_set(ip, "bright_normal", 7)
            disp_set(ip, "bright_timeout", 5)      # the minutes again: lit at its level now
            time.sleep(0.8)
            res["lit"] = light(h)
            time.sleep(6.0)
            res["dimmed"] = light(h)
            r.send("knob main 1")                   # a knob: the light back at its level
            time.sleep(0.8)
            res["knob"] = light(h)
            r.send("knob main -1")
            disp_set(ip, "bright_timeout", 0)       # Off: never dims
            time.sleep(6.5)
            res["off"] = light(h)
            disp_set(ip, "bright_normal", 1)        # under the dim level (2): that comes down with it
            time.sleep(0.6)
            d1 = disp(ip)
            res["under"] = (d1.get("bright_normal"), d1.get("bright_idle"), light(h))
        finally:
            not_back = disp_back(ip, d0, ("bright_normal", "bright_idle", "bright_timeout"))
        want = {"lit": 10 - 7, "dimmed": 10 - 2, "knob": 10 - 7, "off": 10 - 7}
        ok = all(res[k] == (v, 0) for k, v in want.items()) and res["under"][:2] == (1, 1) and \
            res["under"][2] == (10 - 1, 0) and not not_back
        return ok, (f"normal 7, dim 2 after 5 s (sysfs, 10 - level): at once {res['lit']}, after 6 s {res['dimmed']}, "
                    f"a knob detent {res['knob']}; timeout Off, 6.5 s on {res['off']} (want "
                    f"{want}); normal 1 under the dim level: normal, dim {res['under'][:2]} (want (1, 1)), light "
                    f"{res['under'][2]}; put back {d0.get('bright_normal')}/{d0.get('bright_idle')}/"
                    f"{d0.get('bright_timeout')} s" + (f", NOT {not_back}" if not_back else ""))
    per_radio(run, B, "backlight", guarded(knob_test(fn)), ["settings-display-backlight"])


def t_screen_off(run):
    def fn(r):
        ip, h = ip_of(r), r.host
        d0 = disp(ip)
        if d0.get("screen_off_always") is None:
            return False, "the Display page has no screen_off_always"
        if d0.get("screen_off_always"):
            return None, "Screen always off as found: left alone"
        lit0 = light(h)
        res = {}
        try:
            disp_set(ip, "screen_off_always", 1)
            time.sleep(1.5)
            res["dark"] = light(h)
            t0 = time.time()
            time.sleep(2.0)
            with r.lock:
                res["meters"] = sum(1 for at, x in list(r.lines) if at >= t0 and x.startswith("m "))
            disp_set(ip, "screen_off_always", 0)
            time.sleep(1.5)
            res["lit"] = light(h)
        finally:
            not_back = disp_back(ip, d0, ("screen_off_always",))
        level = d0.get("bright_normal", 5)
        ok = res["dark"][1] == 1 and res["lit"] == (10 - level, 0) and res["meters"] >= 8 and not not_back
        return ok, (f"as found {lit0} (sysfs level, bl_power), Screen off (min) {d0.get('screen_off_min')} (0 never); "
                    f"always off: {res['dark']} (bl_power 1: dark), the remote's meters on {res['meters']} in 2 s; off "
                    f"again {res['lit']} (want ({10 - level}, 0)); put back {not not_back}. Screen off after N min not "
                    f"waited for (a minute at least: the screensaver test times the same clock)")
    per_radio(run, B, "screen-off", guarded(fn), ["settings-display-screen-off"])


# ---- the screensaver: a minute at the least, both radios at once ----

SAVER_KEYS = ("screen_saver_min", "screen_saver")


def remote_peers(h):
    try:
        return int(ssh(h, "netstat -tn 2>/dev/null | grep ':50002 ' | grep -c ESTABLISHED").strip() or 0)
    except ValueError:
        return 0


TCI_RATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tci_rate.py")


def t_screensaver(run):
    """Every client of the remote counts as a viewer (remote_server.c tick_cb: nclients > 0 -> backlight_viewers), and
    a viewer keeps the screensaver away: so no remote session during the minute, the suite's own closed for it (its
    banner put back after). The radio is only read, over SSH and the web; at the end a viewer connects (one plain
    connection, nothing sent but 'screen 0'), which ends the rain at once"""
    from suite_cover import GUI_CPU
    from suite_proto import fresh

    def gui_share(h):
        """suite_cover.gui_share over IPv4 (its ~5 s lookup put the 10 s under the rain off TCI's 10.5 s)"""
        out = ssh(h, GUI_CPU, timeout=60).strip().splitlines()
        u0, s0, u1, s1 = (int(x) for x in out[0].split())
        return (u1 + s1 - u0 - s0) / 10, (out[1] if len(out) > 1 else "")
    shown = {}
    for n in list(run.radios):
        shown[n] = run.shown.get(n)
        try:
            run.radios.pop(n).close()
        except OSError:
            pass
    time.sleep(1.5)

    def fn(n, h, ip):
        if np is None:
            return None, "no numpy/Pillow on the bench: the screen's pictures cannot be compared"
        v = box_up(h)
        if v:
            return None, box_says(v)
        d0 = disp(ip)
        if d0.get("screen_saver") is None:
            return False, "the Display page has no screen_saver"
        if d0.get("screen_off_always") or d0.get("screen_off_min") == 1:
            return None, "the screen goes dark at the same minute as found (Screen off): left alone"
        peers = remote_peers(h)
        if peers:
            return None, f"{peers} remote connections (a browser, another bench): a client keeps the screensaver away"
        q_rows = "select name from params where name in ('screen_saver','screen_saver_min')"
        rows0 = set(sql(h, q_rows).split())        # no snapshot here (no session): its rows as found, by hand
        st, body = http(ip, "/api/tci")
        try:
            tci_on = st == 200 and bool(json.loads(body).get("enabled"))
        except ValueError:
            tci_on = False
        pre1 = dump(run, h, f"saver_{n}_before")
        time.sleep(1.0)
        pre2 = dump(run, h)
        if pre1 is None or pre2 is None:
            return None, "no screen dump"
        mask = stable(pre1, pre2)
        seen, im, said, ok = None, None, [], True
        try:
            disp_set(ip, "screen_saver_min", 1)
            disp_set(ip, "screen_saver", 1)         # Matrix rain; its minute counted from now
            t_set = time.time()
            time.sleep(12.0)
            cpu_lit, _ = gui_share(h)               # the GUI's main thread over 10 s, lit
            time.sleep(max(0.0, 58.0 - (time.time() - t_set)))
            while time.time() - t_set < 68.0:
                im = dump(run, h)
                if im is not None and kept(mask, pre1, im) < 0.35:
                    seen = (time.time() - t_set, im)
                    break
                time.sleep(0.3)
            if not seen:
                peers = remote_peers(h)
                k = f"{kept(mask, pre1, im):.0%}" if im is not None else "? (no dump)"
                return (None if peers else False), (
                    f"no rain within 68 s of Screensaver 1 min (the GUI's still parts kept {k})"
                    + (f"; {peers} remote connections came meanwhile: a client keeps it away" if peers else ""))
            at, im = seen
            im.save(os.path.join(run.dir, "shots", f"saver_{n}_rain.png"))
            k_rain, dk = kept(mask, pre1, im), share_dark(im)
            # under the rain: the GUI's main thread drawing only the rain, measured as lit was (no client), then TCI's
            # receive audio at its rate (a TCI client is no viewer, its audio no use). One after the other: TCI's
            # client adds its own share to the main thread (~0.5 % on the X6100, 2026-11-23), which lit had not
            cpu_saver, _ = gui_share(h)
            proc = subprocess.Popen([sys.executable, "-B", TCI_RATE, ip, "--seconds", "10.5", "--format", "bench"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) if tci_on else None
            out = ""
            if proc:
                try:
                    out = proc.communicate(timeout=30)[0]
                except subprocess.TimeoutExpired:
                    proc.kill()
                    out = "FAIL: tci_rate.py did not end"
            lines = (out or "").strip().splitlines()
            tci = next((x for x in reversed(lines) if x.startswith(("PASS", "FAIL"))), lines[-1] if lines else "")
            still = dump(run, h, f"saver_{n}_rain_after")
            k_still = kept(mask, pre1, still) if still is not None else 1.0
            # a viewer: the rain gives way at once (not a use), the GUI drawn whole again
            c, _ = fresh(h, secs=1.5)
            try:
                woke = dump(run, h, f"saver_{n}_viewer")
            finally:
                c.close()
            k_woke = kept(mask, pre1, woke) if woke is not None else 0.0
            audio = None if not tci_on else tci.startswith("PASS")
            ok = both(k_rain < 0.35 and k_still < 0.35 and k_woke > 0.7, audio)
            # lighter than lit: the manual's figures (MANUAL.md Screensaver: half of lit) are the main screen's 3D
            # scope's, and TODO.md (2026-11-15) found the rain the same as the 3D it covers; the Sentry, the home
            # screen now, is what is lit here, so a rain not lighter than it is shown to look at, not failed
            light = cpu_saver < cpu_lit
            if ok and not light:
                ok = None
            said.append(f"rain {at:.0f} s after Screensaver 1 min (no client connected): the GUI's still parts kept "
                        f"{k_rain:.0%}, dark {dk:.0%}; under it the GUI's main thread {cpu_saver:.1f} % of a core "
                        f"(the Sentry's A lit {cpu_lit:.1f} %: {'lighter' if light else 'NOT lighter, to look at'}), "
                        f"then TCI's audio "
                        + (f"{tci[:90]!r}" if tci_on else "not tried (TCI off as found)")
                        + f", the rain still there after {k_still:.0%}; a viewer connected: the GUI back {k_woke:.0%}")
        finally:
            not_back = disp_back(ip, d0, SAVER_KEYS)
            time.sleep(bench.DB_SETTLE_S)           # the GUI's write of the values put back
            for k in set(sql(h, q_rows).split()) - rows0:
                sql(h, f"delete from params where name='{k}'")      # a row set back to the default: none, as found
        if not_back:
            ok = False
        return ok, "; ".join(said) + f"; Screensaver {d0.get('screen_saver')}, {d0.get('screen_saver_min')} min " \
            f"put back {not not_back}"
    try:
        both_radios(run, "screensaver", ["display-screensaver", "display-screensaver-radio-keeps-working",
                                         "display-screensaver-watched"], fn)
    finally:
        for n, text in shown.items():           # the suite's own session again, and its banner
            if text:
                run.banner(text, names=[n])


# ---- themes: from the web, on the Sentry's A, photographed ----

INK_MAX, PAPER_MIN = 40, 200


def t_themes(run):
    def fn(r):
        ip, h, n = ip_of(r), r.host, r.name
        if np is None:
            return None, "no numpy/Pillow on the bench: the screen's pictures cannot be compared"
        v = box_up(h)
        if v:
            return None, box_says(v)
        d0 = disp(ip)
        orig, themes = d0.get("theme"), d0.get("themes") or []
        if not orig or "Outdoor" not in themes or "1bit" not in themes:
            return False, f"the theme {orig!r} of {themes}: no Outdoor or 1bit"
        if orig in ("Outdoor", "Outdoor-Negative", "1bit"):
            return None, f"a light theme as found ({orig}): Outdoor's white would not stand out, left alone"
        bench.open_page(r, "A")
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        a, _ = shot(run, n, f"theme_{n}_{slug(orig)}")
        time.sleep(1.0)
        b, _ = shot(run, n, f"theme_{n}_{slug(orig)}_2")
        if a is None or b is None:
            return None, "no screen picture"
        mask = stable(a, b)
        said, ok = [], True
        try:
            st, now = theme_to(r, ip, "Outdoor")
            row = sql(h, "select val from params where name='theme_name'")
            im, _ = shot(run, n, f"theme_{n}_outdoor")
            lo, ko = (share_light(im), kept(mask, a, im)) if im is not None else (0.0, 1.0)
            out_ok = st == 200 and now == "Outdoor" and row == "Outdoor" and lo > 0.4 and ko < 0.5 \
                and share_light(a) < 0.15
            ok = both(ok, out_ok)
            said.append(f"Outdoor from the web: {st}, the page reads {now!r}, params.db {row!r}, the Sentry white "
                        f"{lo:.0%} (was {share_light(a):.0%}), its still parts kept {ko:.0%}")
            st, now = theme_to(r, ip, "1bit")
            im, _ = shot(run, n, f"theme_{n}_1bit")
            if im is None or now != "1bit":
                verdict, one = False, "not applied, or no picture"
            else:
                cols, counts = np.unique(np.asarray(im).reshape(-1, 3), axis=0, return_counts=True)
                order = counts.argsort()[::-1]
                total = int(counts.sum())
                top = [(tuple(int(v) for v in cols[i]), int(counts[i])) for i in order[:2]]
                inks = [c for c, k in top if max(c) < INK_MAX]
                papers = [c for c, k in top if min(c) > PAPER_MIN]
                other = total - sum(k for _, k in top)
                two = len(top) == 2 and bool(inks) and bool(papers) and min(k for _, k in top) > total * 0.005
                # every pixel ink or paper (tools/screenshots/check_1bit.py); a few others: shown to look at
                verdict = False if not two else True if other == 0 else None if other < total * 0.002 else False
                one = (f"{'#%02x%02x%02x' % inks[0] if inks else 'no ink'} on "
                       f"{'#%02x%02x%02x' % papers[0] if papers else 'no paper'}, {other} pixels of another colour "
                       f"({other / total:.2%})")
            ok = both(ok, verdict)
            said.append(f"1bit: {st}, {now!r}: {one}")
        finally:
            st, now = theme_to(r, ip, orig)
        c, _ = shot(run, n, f"theme_{n}_{slug(orig)}_back")
        kb = kept(mask, a, c) if c is not None else 0.0
        back = now == orig and kb > 0.7
        said.append(f"{orig} again: {now!r}, its still parts kept {kb:.0%}")
        return both(ok, back), "; ".join(said)
    per_radio(run, B, "themes", guarded(fn), ["theme-select-live", "theme-1bit"])


def t_themes_alike(run):
    pics, orig, boxed = {}, {}, {}

    def fn(r):
        ip, n = ip_of(r), r.name
        if np is None:
            return None, "no numpy/Pillow on the bench: the screen's pictures cannot be compared"
        v = box_up(r.host)
        if v:
            boxed[n] = v
            return None, box_says(v)
        d0 = disp(ip)
        orig[n], themes = d0.get("theme"), list(d0.get("themes") or [])
        if not orig[n] or not themes:
            return False, f"no themes on the Display page: {d0.get('theme')!r} {themes}"
        bench.open_page(r, "A")
        got, bad = {}, []
        t0 = time.time()
        try:
            for name in themes:
                st, now = theme_to(r, ip, name)
                _, p = shot(run, n, f"alike_{slug(name)}_{n}")
                if st != 200 or now != name or not p:
                    bad.append(name)
                else:
                    got[name] = p
        finally:
            st, now = theme_to(r, ip, orig[n])
        pics[n] = got
        return not bad and now == orig[n], (f"{len(got)} of {len(themes)} themes applied from the web and photographed "
                                            f"on the Sentry's A in {time.time() - t0:.0f} s"
                                            + (f", not: {bad}" if bad else "") + f"; {orig[n]} again: {now == orig[n]}")
    per_radio(run, B, "themes-alike", guarded(fn), ["theme-builtins-render-alike"])
    if boxed:
        run.result(B, "themes-alike compared", None, "NOT RUN (box up on " + ", ".join(boxed) + "): the themes are "
                   "compared radio to radio only with both screens clear", covers=["theme-builtins-render-alike"])
    if len(pics) != 2 or np is None:
        return
    a, b = run.names
    names = [t for t in pics[a] if t in pics[b]]
    diffs = {t: pic_diff(pics[a][t], pics[b][t]) for t in names}
    base = diffs.get(orig.get(a))
    top = max(0.15, (base or 0.0) + 0.05)       # the radios' own (model, dial, live spectra) and a margin
    for t in names:
        d = diffs[t]
        run.result(B, f"themes-alike {slug(t)}", None if d is None else (True if d <= top else None),
                   f"{t}: {'?' if d is None else f'{d:.1%}'} of the screen differs between the radios' pictures "
                   f"(the theme as found: {'?' if base is None else f'{base:.1%}'}; looked at over {top:.0%})",
                   covers=["theme-builtins-render-alike"],
                   shot=os.path.relpath(pics[a][t], run.dir))


# ---- zoom and step: per mode, per zoom (the web Display page, the remote's state) ----

def mode_rows(h, names):
    """mode_params rows {(mode group, name): value} of these names (the group's own; 100: every mode's)"""
    q = "select mode,name,val from mode_params where name in (" + ",".join(f"'{x}'" for x in names) + ")"
    rows = {}
    for ln in sql(h, q).splitlines():
        p = ln.split("|")
        if len(p) == 3:
            rows[(p[0], p[1])] = p[2]
    return rows


def rows_back(h, before, names, drop):
    """The rows a test left that were not there before, in drop(mode, name), taken out (the GUI's queued writes given
    3 s first); what differs from before then: (added, changed)"""
    time.sleep(3.0)
    now = mode_rows(h, names)
    gone = [k for k in now if k not in before and drop(*k)]
    for m, nm in gone:
        sql(h, f"delete from mode_params where mode={int(m)} and name='{nm}'")
    if gone:
        time.sleep(1.0)
        now = mode_rows(h, names)
    added = sorted(k for k in now if k not in before)
    changed = sorted(k for k in before if now.get(k) != before[k])
    return added, changed


ZS_ROWS = ("freq_step", "spectrum_factor", "step_z1", "step_z2", "step_z4", "step_z8")


def t_zoom_step(run):
    def per_mode(r):
        ip, h = ip_of(r), r.host
        d0 = disp(ip)
        if d0.get("zoom_step_per_mode") != 1 or d0.get("step_per_zoom") != 0:
            return None, (f"Zoom and step per mode {d0.get('zoom_step_per_mode')}, step per zoom "
                          f"{d0.get('step_per_zoom')} as found (want the defaults 1 and 0): left alone")
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        rows0 = mode_rows(h, ZS_ROWS)
        usb = zs(r)
        set_mode(r, 4)
        cw = zs(r)
        set_mode(r, 2)
        z1 = next(z for z in (2, 4, 8, 1) if z not in (usb[0], cw[0]))
        s1 = next(s for s in (100, 1000, 10, 5000) if s not in (usb[1], cw[1]))
        res = {}
        try:
            setk(r, "zoom", z1)
            setk(r, "step", s1)
            set_mode(r, 4)
            res["on_cw"] = zs(r)
            set_mode(r, 2)
            res["on_usb"] = zs(r)
            disp_set(ip, "zoom_step_per_mode", 0)   # one for all: USB's become every mode's
            time.sleep(0.6)
            set_mode(r, 4)
            res["off_cw"] = zs(r)
            set_mode(r, 2)
            disp_set(ip, "zoom_step_per_mode", 1)   # each mode's own again
            time.sleep(0.6)
            set_mode(r, 4)
            res["again_cw"] = zs(r)
            set_mode(r, 2)
            res["again_usb"] = zs(r)
        finally:
            not_back = disp_back(ip, d0, ("zoom_step_per_mode",))
            if int(r.state.get("mode", -1)) != 2:
                set_mode(r, 2)
            setk(r, "zoom", usb[0])
            setk(r, "step", usb[1])
            back = zs(r)
            added, changed = rows_back(h, rows0, ZS_ROWS, lambda m, nm: m == "100")
        ok = res.get("on_cw") == cw and res.get("on_usb") == (z1, s1) and res.get("off_cw") == (z1, s1) and \
            res.get("again_cw") == cw and res.get("again_usb") == (z1, s1) and back == usb and not not_back \
            and not changed
        return ok, (f"(zoom, step) USB {usb}, CW {cw}; USB set to {(z1, s1)}: in CW {res.get('on_cw')}, USB again "
                    f"{res.get('on_usb')}; per mode off: CW {res.get('off_cw')} (USB's); on again: CW "
                    f"{res.get('again_cw')}, USB {res.get('again_usb')}; USB put back {back}; params.db rows changed "
                    f"{changed or 'none'}" + (f", added {added}" if added else ""))
    per_radio(run, B, "zoom-step", guarded(per_mode), ["settings-display-zoom-step-per-mode"])


def t_step_zoom(run):
    def per_zoom(r):
        ip, h = ip_of(r), r.host
        d0 = disp(ip)
        if d0.get("step_per_zoom") != 0:
            return None, f"Step per zoom {d0.get('step_per_zoom')} as found (want the default 0): left alone"
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        rows0 = mode_rows(h, ZS_ROWS)
        z0, s0 = zs(r)
        res = {}
        try:
            disp_set(ip, "step_per_zoom", 1)
            time.sleep(0.4)
            setk(r, "zoom", 1)
            setk(r, "step", 1000)
            setk(r, "zoom", 8)
            time.sleep(0.4)
            setk(r, "step", 10)
            setk(r, "zoom", 1)
            time.sleep(0.6)
            res["z1"] = zs(r)
            setk(r, "zoom", 8)
            time.sleep(0.6)
            res["z8"] = zs(r)
            disp_set(ip, "step_per_zoom", 0)        # off: a zoom keeps the step in use
            time.sleep(0.4)
            setk(r, "zoom", 1)
            time.sleep(0.6)
            res["off"] = zs(r)
        finally:
            not_back = disp_back(ip, d0, ("step_per_zoom",))
            setk(r, "zoom", z0)
            setk(r, "step", s0)
            back = zs(r)
            added, changed = rows_back(h, rows0, ZS_ROWS, lambda m, nm: nm.startswith("step_z"))
        ok = res.get("z1") == (1, 1000) and res.get("z8") == (8, 10) and res.get("off") == (1, 10) and \
            back == (z0, s0) and not not_back and not changed
        return ok, (f"step per zoom on, 1000 Hz chosen at zoom 1 and 10 Hz at zoom 8: zoom 1 brings {res.get('z1')}, "
                    f"zoom 8 {res.get('z8')} (zoom, step); off, zoom 1 keeps {res.get('off')}; put back {back} (was "
                    f"{(z0, s0)}); params.db rows changed {changed or 'none'}" + (f", added {added}" if added else ""))
    per_radio(run, B, "step-zoom", guarded(per_zoom), ["settings-display-step-per-zoom"])


# ---- the band plan: the plan chosen, a band of one's own, the mode from the plan ----

def segs_in(plan, lo, hi):
    return sorted([s for s in (plan or []) if s[0] < hi and s[1] > lo])


def t_band_plan(run):
    def fn(r):
        ip = ip_of(r)
        d0 = disp(ip)
        reg0 = d0.get("band_region")
        if reg0 is None:
            return False, "the Display page has no band_region"
        if d0.get("band_region_now") != 2:
            return None, (f"the plan in use is {d0.get('band_region_now')} (not Region 2): a US plan or Region 2 would "
                          f"move the band list's edges (cfg_bands_follow_region), left alone")
        _, p0 = hello(r)
        res = {}
        try:
            for v in (6, 4, 2):                     # US General, US Extra, Region 2: all Region 2's edges
                disp_set(ip, "band_region", v)
                time.sleep(1.2)
                res[v] = hello(r)[1]
        finally:
            not_back = disp_back(ip, d0, ("band_region",))
            time.sleep(1.0)
        _, p1 = hello(r)
        out6 = sum(1 for s in res.get(6) or [] if s[2] == 4)
        out4 = sum(1 for s in res.get(4) or [] if s[2] == 4)
        phone40 = [s for s in segs_in(res.get(2), 7000000, 7300000) if s[2] == 2]
        r2_ok = any(s[1] == 7300000 for s in phone40)
        ok = bool(p0) and out6 > 0 and out4 == 0 and r2_ok and res.get(4) != res.get(2) and p1 == p0 and not not_back
        return ok, (f"the remote's plan: US General {out6} NOT YOUR CLASS segments, US Extra {out4}; Region 2's 40 m "
                    f"phone {[s[:2] for s in phone40]} (to 7.3 MHz {r2_ok}); as found ({reg0}) again: the same plan "
                    f"{p1 == p0}" + (f", NOT put back {not_back}" if not_back else ""))
    per_radio(run, B, "band-plan", guarded(fn), ["settings-general-band-plan"])


OWN = {"name": "BENCH 60m CW", "start_freq": 5258500, "stop_freq": 5264000, "type": 1}


def own_bands(ip):
    st, body = http(ip, "/api/bands")
    try:
        return [b for b in json.loads(body) if b.get("name") == OWN["name"]] if st == 200 else []
    except ValueError:
        return []


def t_own_band(run):
    def fn(r):
        ip, h = ip_of(r), r.host
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        for b in own_bands(ip):                     # a run stopped before its finally: ours, by its name
            http(ip, f"/api/bands/{b['id']}", method="DELETE")
        _, p0 = hello(r)
        st, body = http(ip, "/api/bands", method="PUT", body=OWN)
        mine = own_bands(ip)
        bid = mine[0]["id"] if mine else None
        res = {}
        try:
            if bid is None:
                return False, f"PUT /api/bands {st} {body[:100]!r}: no band added"
            setk(r, "freq", 5260000)
            time.sleep(1.0)
            _, p = hello(r)
            res["in"] = segs_in(p, OWN["start_freq"], OWN["stop_freq"])
            res["mode"] = int(r.state.get("mode", -1))
            setk(r, "freq", F_TEST)
            time.sleep(3.0)                         # the band's own memories written as the radio left it
        finally:
            if bid is not None:
                http(ip, f"/api/bands/{bid}", method="DELETE")
                time.sleep(2.0)
                http(ip, f"/api/bands/{bid}", method="DELETE")     # a late write of the GUI's for it, taken too
                rows = sql(h, f"select (select count(*) from bands where id={int(bid)}),"
                              f"(select count(*) from band_params where bands_id={int(bid)})")
                res["rows"] = rows
        setk(r, "freq", 7100000)                    # a band change: the plan read again without it
        time.sleep(0.8)
        setk(r, "freq", F_TEST)
        time.sleep(1.0)
        _, p2 = hello(r)
        res["after"] = segs_in(p2, OWN["start_freq"], OWN["stop_freq"])
        res["before"] = segs_in(p0, OWN["start_freq"], OWN["stop_freq"])
        want = [[OWN["start_freq"], OWN["stop_freq"], 0]]
        ok = st == 201 and res["before"] == [] and res.get("in") == want and res["after"] == [] and \
            res.get("rows") == "0|0"
        return ok, (f"'{OWN['name']}' {OWN['start_freq']}-{OWN['stop_freq']} Hz added: {st} (id {bid}); the plan "
                    f"there before {res['before']}, tuned in {res.get('in')} (want {want}: CW), mode "
                    f"{res.get('mode')}; "
                    f"taken out: the band's rows and its params {res.get('rows')} (want 0|0), the plan there after a "
                    f"band change {res['after']}")
    per_radio(run, B, "own-band", guarded(fn), ["main-screen-band-plan-own-bands"])


PLAN_MARGIN = 5000              # Hz: a plan edge this far inside its band at least (the knob's steps over it)


def plan_band(bands, plan):
    """The first band of the list (10 MHz and up: USB its voice) holding a CW segment that DIG follows and, further
    up, a phone segment DIG comes before, both edges inside it: (band, the CW segment's end e, the phone's start a), or
    None. One band, because a band change loads the band's own mode (its memory), which is not the band plan's: 20 m
    is two bands on these radios, '20m CW' to 14.070 and '20m SSB' from it, at the plan's CW edge (2026-11-23: the
    knob down to 14.0695 set CW with Mode from band plan off, from 20m CW's memory)"""
    for b in sorted(bands or [], key=lambda x: x["a"]):
        if b["a"] < 10000000:
            continue
        segs = segs_in(plan, b["a"], b["b"])
        for cw in (x for x in segs if x[2] == 0):
            e = cw[1]
            nxt = next((x for x in segs if x[0] == e), None)
            phone = next((x for x in segs if x[0] > e and x[2] == 2), None)
            below = next((x for x in segs if phone and x[1] == phone[0]), None)
            if nxt and nxt[2] in (1, 3) and phone and below and below[2] in (1, 3) and \
                    b["a"] + PLAN_MARGIN <= e and phone[0] + PLAN_MARGIN <= b["b"]:
                return b, e, phone[0]
    return None


def t_mode_from_plan(run):
    def fn(r):
        ip = ip_of(r)
        d0 = disp(ip)
        if d0.get("mode_from_plan") is None:
            return False, "the Display page has no mode_from_plan"
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        bands, plan = hello(r)
        pick = plan_band(bands, plan)
        if not pick:
            return None, (f"no band of the list holds a CW segment, then DIG, ..., DIG, then phone, all inside it: "
                          f"{[(b['n'], b['a'], b['b']) for b in bands or []]}")
        band, e, a = pick
        res = {}
        try:
            if d0["mode_from_plan"] != 1:
                disp_set(ip, "mode_from_plan", 1)
            setk(r, "freq", (e + a) // 2)           # into the band, between the edges (its own mode comes with it)
            set_mode(r, 2)                          # USB there
            step = int(r.state.get("step", 500))
            setk(r, "freq", e + 2 * step)           # two steps over the CW segment
            knob_until(r, -1, lambda f: f < e)
            wait_state(r, lambda s: int(s.get("mode", -1)) == 4, 2.0)
            res["down"] = (int(r.state["freq"]), int(r.state["mode"]))
            cw_step = int(r.state.get("step", 100))
            setk(r, "freq", a - 2 * cw_step)        # DIG under the phone segment, the same band: the mode stays
            time.sleep(0.4)
            res["dig"] = int(r.state["mode"])
            knob_until(r, 1, lambda f: f >= a)
            wait_state(r, lambda s: int(s.get("mode", -1)) == 2, 2.0)
            res["up"] = (int(r.state["freq"]), int(r.state["mode"]))
            res["off_set"] = disp_set(ip, "mode_from_plan", 0)      # off: the knob changes no mode
            time.sleep(0.4)
            setk(r, "freq", e + 2 * step)
            knob_until(r, -1, lambda f: f < e)
            time.sleep(0.8)
            res["off"] = (int(r.state["freq"]), int(r.state["mode"]))
        finally:
            not_back = disp_back(ip, d0, ("mode_from_plan",))
        down, up, off = res.get("down", (0, -1)), res.get("up", (0, -1)), res.get("off", (0, -1))
        ok = down[0] < e and down[1] == 4 and res.get("dig") == 4 and up[0] >= a and up[1] == 2 and off[0] < e and \
            off[1] == 2 and res.get("off_set") == 200 and not not_back
        return ok, (f"{band['n']} ({band['a']}-{band['b']}, one band): CW to {e}, phone from {a}; in USB the knob "
                    f"down from DIG to {down[0]}: mode {down[1]} (want 4, CW); "
                    f"tuned into DIG {res.get('dig')} (stays); the knob up to {up[0]}: mode {up[1]} (want 2, USB); "
                    f"Mode from band plan off ({res.get('off_set')}): down to {off[0]} mode {off[1]} (stays 2); put "
                    f"back {d0['mode_from_plan']} {not not_back}")
    per_radio(run, B, "mode-from-plan", guarded(knob_test(fn)), ["settings-general-mode-from-band-plan"])


# ---- keys: the lock per band and mode ----

def band_lock(h, f):
    """(band id, its lock_modes) of the active band f is in"""
    out = sql(h, f"select b.id, coalesce((select val from band_params where bands_id=b.id and name='lock_modes'),'') "
                 f"from bands b where b.type=1 and b.start_freq<={int(f)} and b.stop_freq>{int(f)} limit 1")
    p = out.split("|") if out else []
    return (p[0], p[1]) if len(p) == 2 else (None, None)


def keys_get(ip):
    st, body = http(ip, "/api/keys")
    try:
        return json.loads(body) if st == 200 else {}
    except ValueError:
        return {}


def t_lock_per_band(run):
    def fn(r):
        ip, h = ip_of(r), r.host
        lp0 = keys_get(ip).get("lock_per_band")
        if lp0 is None:
            return False, "GET /api/keys has no lock_per_band"
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        if int(r.state.get("ulock", 0)):
            return None, "the dial locked as found: left alone"
        bid, lm0 = band_lock(h, F_TEST)
        if lm0 and int(lm0) & (1 << 2):
            return None, f"20 m USB's own lock kept as found ({lm0}): turning the option on would change it"
        res = {}
        try:
            http(ip, "/api/keys", method="POST", body={"key": "lock_per_band", "val": 1})
            time.sleep(0.5)
            r.send("set lock 1")
            res["locked"] = wait_state(r, lambda s: int(s.get("ulock", 0)) == 1, 4.0)
            f = int(r.state["freq"])
            r.send("knob main 1")
            time.sleep(1.0)
            res["held"] = int(r.state["freq"]) == f
            set_mode(r, 3)
            res["usbd"] = int(r.state.get("ulock", -1))
            set_mode(r, 2)
            res["usb"] = int(r.state.get("ulock", -1))
            r.send("set band 1")                    # BAND UP: the next band's own lock
            wait_state(r, lambda s: not (14000000 <= int(s.get("freq", 0)) <= 14350000), 5.0)
            time.sleep(0.8)
            f_next, mode_n = int(r.state["freq"]), int(r.state.get("mode", -1))
            res["moved"] = not (14000000 <= f_next <= 14350000)
            _, nlm = band_lock(h, f_next)
            res["next_want"] = (int(nlm or 0) >> mode_n) & 1
            res["next"] = int(r.state.get("ulock", -1))
            r.send("set band -1")                   # BAND DOWN: 20 m USB, locked
            wait_state(r, lambda s: 14000000 <= int(s.get("freq", 0)) <= 14350000, 5.0)
            time.sleep(0.8)
            res["back"] = (int(r.state["freq"]), int(r.state.get("mode", -1)), int(r.state.get("ulock", -1)))
        finally:
            r.send("set lock 0")
            wait_state(r, lambda s: int(s.get("ulock", 0)) == 0, 4.0)
            http(ip, "/api/keys", method="POST", body={"key": "lock_per_band", "val": int(lp0)})
            t0 = time.time()
            lm1 = None
            while time.time() - t0 < 6.0:
                _, lm1 = band_lock(h, F_TEST)
                if (lm1 or "0") == (lm0 or "0"):
                    break
                time.sleep(0.5)
            lp1 = keys_get(ip).get("lock_per_band")
        ok = res.get("locked") and res.get("held") and res.get("usbd") == 0 and res.get("usb") == 1 and \
            res.get("moved") and res.get("next") == res.get("next_want") and res.get("back", (0, 0, 0))[2] == 1 and \
            int(r.state.get("ulock", 1)) == 0 and lp1 == lp0 and (lm1 or "0") == (lm0 or "0")
        return ok, (f"per band and mode on; LOCK on 20 m USB: locked {res.get('locked')}, a knob detent held the dial "
                    f"{res.get('held')}; USB-D {res.get('usbd')} (free), USB {res.get('usb')} (locked); BAND UP "
                    f"{res.get('next')} (its own: {res.get('next_want')}); BAND DOWN (freq, mode, lock) "
                    f"{res.get('back')}; unlocked, the option {lp1} (was {lp0}), 20 m's lock_modes {lm1!r} (was "
                    f"{lm0!r})")
    per_radio(run, B, "lock-per-band", guarded(knob_test(fn)), ["settings-keys-lock-per-band"])


# ---- Display > Channels: the knob stops on a channel ----

def t_channel_snap(run):
    def fn(r):
        ip, h = ip_of(r), r.host
        d0 = disp(ip)
        if d0.get("channel_marks") is None:
            return False, "the Display page has no channel_marks"
        if d0.get("channel_marks"):
            return None, "Channels on as found: left alone"
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        step = int(r.state.get("step", 0))
        if step < 20:
            return None, f"a step of {step} Hz: no room for a channel between two detents"
        setk(r, "freq", F_TEST)
        time.sleep(0.5)
        knob_until(r, 1, lambda f: True, most=1)
        plain = int(r.state["freq"]) - F_TEST       # the marks off: one step (a cluster spot would stop it)
        if plain != step:
            return None, f"with the marks off one detent moved {plain} Hz, not the step {step}: a spot in the way"
        ch_hz = F_TEST + step // 2
        k0 = keep(h, "/mnt/channels.json", "save")
        res = {}
        try:
            st, _ = http(ip, "/api/channels", method="POST",
                         body={"name": "BENCH SNAP", "frequency": ch_hz, "mode": "USB", "agc": "SLOW", "pre": False,
                               "att": False})
            res["add"] = st
            time.sleep(1.5)                         # the GUI reads the file again within a second
            disp_set(ip, "channel_marks", 1)
            time.sleep(0.8)
            setk(r, "freq", F_TEST)
            time.sleep(0.6)
            knob_until(r, 1, lambda f: True, most=1)
            res["snap"] = int(r.state["freq"])
        finally:
            not_back = disp_back(ip, d0, ("channel_marks",))
            st2, body2 = http(ip, "/api/channels")
            try:
                now = json.loads(body2).get("channels", []) if st2 == 200 else []
            except ValueError:
                now = []
            for i in sorted((i for i, c in enumerate(now) if c.get("name") == "BENCH SNAP"), reverse=True):
                http(ip, f"/api/channels/{i}", method="DELETE", body={"was": now[i]})
            k1 = keep(h, "/mnt/channels.json", "back")
        ok = res.get("add") == 200 and res.get("snap") == ch_hz and not not_back and k1 in ("same", "removed",
                                                                                             "restored")
        return ok, (f"marks off: a detent {plain} Hz (the step); a channel at {ch_hz} ({res.get('add')}), marks on: a "
                    f"detent from {F_TEST} stops at {res.get('snap')} (want {ch_hz}); put back: Channels "
                    f"{d0.get('channel_marks')} {not not_back}, the channel taken out, channels.json {k0}, then {k1}")
    per_radio(run, B, "channel-snap", guarded(knob_test(fn)), ["settings-display-channel-marks"])


# ---- audio: Speaker always off, Line-in ----

def t_speaker_off(run):
    def fn(r):
        ip = ip_of(r)
        d0 = disp(ip)
        if d0.get("speaker_off") is None:
            return False, "the Display page has no speaker_off"
        if d0.get("speaker_off"):
            return None, "Speaker always off as found: left alone"
        if int(r.state.get("vol", 0)) == 0:
            setk(r, "vol", 1)                       # a volume the speaker would play at (finish puts it back)
        time.sleep(0.5)
        vol = int(r.state.get("vol", 0))
        s0 = (int(r.state.get("spk", -1)), int(r.state.get("muted", -1)))
        if s0 != (vol, 0):
            return None, f"the speaker muted otherwise as found (spk, muted) {s0} at volume {vol}: TCI, BT headphones"
        res = {}
        try:
            disp_set(ip, "speaker_off", 1)
            wait_state(r, lambda s: int(s.get("muted", 0)) == 1, 3.0)
            time.sleep(0.3)
            res["off"] = (int(r.state.get("spk", -1)), int(r.state.get("muted", -1)), int(r.state.get("vol", -1)))
            disp_set(ip, "speaker_off", 0)
            wait_state(r, lambda s: int(s.get("muted", 1)) == 0, 3.0)
            time.sleep(0.3)
            res["on"] = (int(r.state.get("spk", -1)), int(r.state.get("muted", -1)), int(r.state.get("vol", -1)))
        finally:
            not_back = disp_back(ip, d0, ("speaker_off",))
        ok = res.get("off") == (0, 1, vol) and res.get("on") == (vol, 0, vol) and not not_back
        return ok, (f"volume {vol}: (spk, muted, vol) {s0 + (vol,)}; Speaker always off {res.get('off')} (want "
                    f"{(0, 1, vol)}); off again {res.get('on')}; put back {not not_back}")
    per_radio(run, B, "speaker-off", guarded(fn), ["settings-audio-speaker-always-off"])


def t_line_in(run):
    def fn(r):
        h = r.host
        li0 = int(r.state.get("linein", -1))
        if li0 < 0:
            return False, "the state has no linein"
        row0 = sql(h, "select val from params where name='line_in'")
        want = 12 if li0 != 12 else 14
        res = {}
        try:
            res["set"] = setk(r, "linein", want)
            t0 = time.time()
            while time.time() - t0 < 8.0:
                res["row"] = sql(h, "select val from params where name='line_in'")
                if res["row"] == str(want):
                    break
                time.sleep(0.5)
        finally:
            setk(r, "linein", li0)
            t0 = time.time()
            while time.time() - t0 < 8.0:
                res["row_back"] = sql(h, "select val from params where name='line_in'")
                if res["row_back"] in (str(li0), row0):
                    break
                time.sleep(0.5)
        ok = res.get("set") and res.get("row") == str(want) and int(r.state.get("linein", -1)) == li0 and \
            res.get("row_back") in (str(li0), row0)
        return ok, (f"line-in {li0} (params.db {row0 or 'default'}): 'set linein {want}' read back "
                    f"{res.get('set')}, params.db {res.get('row')!r}; put back {r.state.get('linein')}, params.db "
                    f"{res.get('row_back')!r}. Play/Rec gain leaving it alone not tried (Settings only)")
    per_radio(run, B, "line-in", guarded(fn), ["settings-audio-line-levels"])


# ---- transverters off: no band, no frequency, no transmit there ----

XVTR_BANDS = ((70000000, 72000000), (144000000, 150000000), (432000000, 438000000))


def t_transverters(run):
    def fn(r):
        ip, h, n = ip_of(r), r.host, r.name
        rows = sql(h, "select id,name,val from transverter")
        shifts = [ln for ln in rows.splitlines() if ln.split("|")[1:2] == ["shift"] and ln.split("|")[2] != "0"]
        bands, _ = hello(r)
        on_x = [(b["n"], b["a"], b["b"]) for b in bands or [] if any(b["a"] < hi and b["b"] > lo for lo, hi in
                                                                      XVTR_BANDS)]
        wr12 = bool(disp(ip).get("wr12"))
        use = {f: cfg(h, {"cmd": "band_use", "lo": f, "hi": f}).get("use") for f in (144300000, 432100000)}
        want_use = 1 if wr12 else 0                 # the X6200's WR12 receives there; nothing ever sends
        f0 = int(r.state.get("freq", 0))
        r.errs.clear()
        t0 = time.time()
        r.send("set freq 144300000")
        err = line_after(r, t0, "err freq", 3.0)
        time.sleep(0.5)
        f1 = int(r.state.get("freq", 0))
        refused = (err is not None and "not a frequency this radio tunes" in err and f1 == f0) if not wr12 else True
        ok = not shifts and bands is not None and not on_x and all(u == want_use for u in use.values()) and refused
        return ok, (f"transverter rows {rows.splitlines() or 'none (the defaults: shift 0)'}, shifts on "
                    f"{shifts or 'none'}; "
                    f"the remote's bands in 4 m/2 m/70 cm {on_x or 'none'} (of {len(bands or [])}); band_use 144.3 and "
                    f"432.1 MHz {list(use.values())} (want {want_use}: never 2, transmit); 'set freq 144300000' "
                    f"{err!r}, the dial {f0} -> {f1}" + (" (WR12 on: received)" if wr12 else ""))
    per_radio(run, B, "transverters", guarded(fn), ["transverter-off-by-default"])


# ---- a band's TX shift (the web Bands page): where the radio would send, never sent ----

def band_of(ip, f):
    st, body = http(ip, "/api/bands")
    try:
        bands = json.loads(body) if st == 200 else []
    except ValueError:
        bands = []
    return next((b for b in bands if b.get("type") == 1 and b["start_freq"] <= f < b["stop_freq"]), None)


def t_tx_shift(run):
    def fn(r):
        ip = ip_of(r)
        f_fm, shift = 29600000, -600000
        b0 = band_of(ip, f_fm)
        if not b0:
            return None, "no active band at 29.6 MHz"
        p0 = b0.get("params", {})
        if p0.get("tx_shift") or p0.get("split"):
            return None, f"10 m with a TX shift {p0.get('tx_shift')} or split {p0.get('split')} as found: left alone"
        edges = {k: b0[k] for k in ("name", "start_freq", "stop_freq", "type")}
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        res = {}
        try:
            res["set"] = http(ip, f"/api/bands/{b0['id']}", method="POST",
                              body=dict(edges, params={"tx_shift": shift}))[0]
            bench.ensure(r, {"freq": f_fm, "mode": 7})      # into the band: its shift loaded with it (no TX)
            time.sleep(0.8)
            res["on"] = (int(r.state.get("freq", 0)), int(r.state.get("txf", 0)), int(r.state.get("split", -1)))
            res["vfo"] = int(r.state.get("vfo", 0))
            bench.ensure(r, {"freq": F_TEST, "mode": 2})
            time.sleep(3.0)                         # the band's own settings written as the radio left it
        finally:
            res["back"] = http(ip, f"/api/bands/{b0['id']}", method="POST", body=dict(edges, params={"tx_shift": 0}))[0]
            b1 = band_of(ip, f_fm) or {}
            p1 = b1.get("params", {})
        bench.ensure(r, {"freq": f_fm, "mode": 7})      # the band again, its shift gone
        time.sleep(0.8)
        res["off"] = (int(r.state.get("freq", 0)), int(r.state.get("txf", 0)), int(r.state.get("split", -1)))
        f, txf, split = res.get("on", (0, 0, 0))
        want_tx = f + shift if res.get("vfo", 0) == 0 else f - shift      # radio_tx_freq: VFO B the other way
        off_f, off_tx = res["off"][0], res["off"][1]
        ok = res.get("set") == 200 and txf == want_tx and split == 1 and res.get("back") == 200 and \
            int(p1.get("tx_shift", 0)) == 0 and int(p1.get("split", 0)) == 0 and off_tx in (0, off_f)
        return ok, (f"{b0['name']}: TX shift {shift // 1000} kHz from the web {res.get('set')}; on {f_fm} NFM "
                    f"(freq, TX freq, split) {res.get('on')} (want TX {want_tx}); shift 0 again {res.get('back')}, the band's "
                    f"tx_shift {p1.get('tx_shift')} split {p1.get('split')}; on the band again {res['off']}. Nothing "
                    f"sent; the status bar's '-600k' not read (no OCR)")
    per_radio(run, B, "tx-shift", guarded(fn), ["settings-general-tx-shift"])


# ---- the time zone alike; TX outside ham bands off ----

TZ = ("[ -s /mnt/tz.conf ] && echo have=1; echo sum=$(head -1 /mnt/tz.conf 2>/dev/null | md5sum | cut -c1-12); "
      "TZ=\"$(head -1 /mnt/tz.conf 2>/dev/null)\" date '+local=%s %z'")


def t_tz(run):
    got = {}
    for n in run.names:
        h = run.host(n)
        try:
            t_mac = time.time()
            out = ssh(h, TZ)
            t_mac = (t_mac + time.time()) / 2
        except (OSError, subprocess.SubprocessError) as e:
            run.result(B, f"tz-alike {n}", False, f"no answer: {e}", radio=n, covers=["lookfeel-time-zone-alike"])
            continue
        kv = dict(ln.split("=", 1) for ln in out.split() if "=" in ln)
        try:
            epoch, off = out.split("local=", 1)[1].split()[:2]
            got[n] = {"have": kv.get("have") == "1", "sum": kv.get("sum"), "skew": int(epoch) - t_mac, "off": off}
        except (IndexError, ValueError):
            got[n] = {"have": False, "sum": None, "skew": None, "off": None}
        g = got[n]
        # no tz.conf: the GUI's clock in UTC (clock.c), not wrong in itself, shown to look at
        run.result(B, f"tz-alike {n}", False if g["skew"] is None else True if g["have"] else None,
                   f"/mnt/tz.conf {'there' if g['have'] else 'MISSING'} (its POSIX TZ not printed), the clock "
                   f"{g['skew'] if g['skew'] is None else round(g['skew'])} s from the bench's", radio=n,
                   covers=["lookfeel-time-zone-alike"])
    if len(got) == 2:
        a, b = (got[n] for n in run.names)
        same = a["sum"] is not None and a["sum"] == b["sum"] and a["off"] == b["off"]
        skew = abs(a["skew"] - b["skew"]) if a["skew"] is not None and b["skew"] is not None else None
        run.result(B, "tz-alike 1:1", same and skew is not None and skew <= 60,
                   f"the same tz.conf {a['sum'] == b['sum']}, the same UTC offset now {a['off'] == b['off']}, the "
                   f"local clocks {skew if skew is None else round(skew)} s apart (at most 60). The title clocks "
                   f"themselves not read (no OCR)", covers=["lookfeel-time-zone-alike"])


def t_tx_outside(run):
    def fn(n, h, ip):
        d = disp(ip)
        if "tx_outside_bands" not in d:
            return False, f"the Display page has no tx_outside_bands: {sorted(d)[:6]}"
        ok = d["tx_outside_bands"] == 0
        return ok, (f"the web Display page's Transmitter card: TX outside ham bands {d['tx_outside_bands']} (off, the "
                    f"default), TX ranges {d.get('tx_ranges')} (0 Ham bands); read only, never switched on (a TX "
                    f"permission: its effect is tx-out-of-band-ptt-refused's)")
    by_radio(run, B, "tx-outside", ["tx-outside-ham-bands-switch"], guarded(fn))


TESTS = (("btcat", t_btcat), ("bt-pairing", t_bt_pairing), ("backlight", t_backlight), ("screen-off", t_screen_off),
         ("screensaver", t_screensaver), ("themes", t_themes), ("themes-alike", t_themes_alike),
         ("zoom-step", t_zoom_step), ("step-zoom", t_step_zoom), ("band-plan", t_band_plan), ("own-band", t_own_band),
         ("mode-from-plan", t_mode_from_plan), ("lock-per-band", t_lock_per_band), ("channel-snap", t_channel_snap),
         ("speaker-off", t_speaker_off), ("line-in", t_line_in), ("transverters", t_transverters),
         ("tx-shift", t_tx_shift), ("tz-alike", t_tz), ("tx-outside", t_tx_outside))


def batch(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE cover_a: {name} (receive only, nothing sent)")
            fn(run)
