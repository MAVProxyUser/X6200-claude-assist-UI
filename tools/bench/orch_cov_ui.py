"""The settings, system and screen features no test covered (2026-11-27, the coverage report's system, settings-*,
display, look and feel, main screen and Sentry common/B/H/pool/SCAN CFG areas, each needing nothing but the two
radios, the cable or an RTL-SDR): their orch tests, each judging itself from the radios' logs and files, the remote's
state and settings, or a picture of the screen measured against itself.

Nothing here transmits. A test that restarts a GUI says so on the console first (planned) and runs its stop and start
as one SSH command (never a stop alone). Every test ends with both radios on the Sentry's page A (home_both).

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import os
import re
import time

import bench
from bench import Refused
import orch_cov_apps as oa

H = None                        # the running orch module (orch.py: orch_cov_ui.H = sys.modules[__name__])


def _both(o):
    return [(H.A, o.r[H.A]), (H.B, o.r[H.B])]


def _restart(o, n, why, between="true"):
    """n's GUI stopped, between run (its output returned), started again, all in one SSH command; said on the console
    as planned first; the bench's session connected again, the dial and mode as the other radio's"""
    import orch_cable as oc
    r = o.r[n]
    o.st.ev(f"{n}: its GUI restarts ({why}, planned)", "radio")
    o.planned[n] = f"planned: {why}"
    o.changing += 1
    try:
        out = oa.ssh(r, f"/etc/init.d/S95gui stop >/dev/null 2>&1\n{between}\n/etc/init.d/S95gui start >/dev/null 2>&1",
                     150)
        time.sleep(8)
        oc._reconnect(o, n, why)
        r.take_control()
        other = next(x for m, x in o.r.items() if m != n)
        try:
            bench.ensure(r, {"freq": int(other.state.get("freq", 0)), "mode": int(other.state.get("mode", 0))})
        except Refused as e:                # the lock on (the restart test's own): the dial as the GUI kept it
            o.st.ev(f"{n}: the dial not set as the other's after the restart: {e}", "radio")
        return out
    finally:
        o.changing -= 1
        o.planned.pop(n, None)


# ---- A GUI restart, and what it keeps and what it reads at start (perf-overlay, gpu-off-file,
#      sentry-resume-after-restart, cw-beacon-off-after-restart, sdr-daemon-persistent, network-x6100-wifi-power-held,
#      system-x6200-base-keeper, incoming-log-import, gui-restart-on-audio-loss, radio-core-freq-lock-per-band-mode,
#      sentry-b-state-kept) ----

ADIF = ("<ADIF_VER:5>3.1.4 <EOH>\n<CALL:5>Q2TST <QSO_DATE:8>20261127 <TIME_ON:6>120000 <BAND:3>20m "
        "<FREQ:9>14.074000 <MODE:3>FT8 <RST_SENT:3>-10 <RST_RCVD:3>-12 <EOR>\n")
STOP_PROBE = {
    # between the GUI's stop and its start: the X6200's base kept on by x6200-base-keeper; the X6100's WiFi up (this
    # very SSH command runs over it) and associated as long as before
    "x6200": "sleep 4; pidof x6200-base-keeper >/dev/null && echo KEEPER_ON || echo KEEPER_OFF",
    "x6100": "sleep 4; cat /sys/class/net/wlan0/carrier 2>/dev/null | sed 's/^/CARRIER /'; "
             "cat /sys/class/net/wlan0/carrier_changes 2>/dev/null | sed 's/^/CHANGES /'",
}


def _connected_s(r):
    """The X6100's WiFi link changes so far (its driver tells no connected time): the same after a restart that kept
    the chip's power"""
    out = oa.ssh(r, "cat /sys/class/net/wlan0/carrier_changes 2>/dev/null")
    return int(out.split()[0]) if out.strip() else None


def _rtl_pid(r):
    import orch_gmrs as og
    try:
        return og._daemon_args(r.host, wait=2)[0]
    except Refused:
        return None


def t_restart(o):
    """Each radio's GUI restarted in turn (planned, one SSH command): before it, the Sentry on page D, the frequency
    lock on (per band), /mnt/perf and /mnt/gpu.off made and an ADIF file with one QSO (Q2TST, no OPERATOR) as
    /mnt/incoming_log.adi; while it is stopped the X6200's base keeper runs and the X6100's WiFi stays up and
    associated; after it, page D again, the lock still on, the SDR's daemon the same process, B's channel kept, MSG >
    CW's beacon off, the perf overlay's and the CPU's drawing said in the GUI's log, the QSO imported under the radio's
    callsign and the file renamed .bak. Then started again without perf and gpu.off, and once more by PulseAudio
    stopped: x6100_daemon starts the audio server and the GUI again, the ended run's log in /mnt/app_logs. The QSO
    taken out of the log after, every file put back"""
    res = {}
    done = []
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with oa.file_kept(r, "/mnt/perf", "/mnt/gpu.off", "/mnt/incoming_log.adi", "/mnt/incoming_log.adi.bak"):
                with o.step(f"{n}: before: page D, the lock on, the SDR's daemon, B's channel, the beacon's settings") \
                        as s:
                    bench.open_page(r, "D")
                    r.wait("page D", lambda: r.state.get("pg") == "D", 6)
                    lock_kept = oa.cfg_kept(r, {"lock_per_band": 1})
                    lock_kept.__enter__()
                    done.append((n, lock_kept))
                    r.send("set lock 1")
                    r.wait("the lock on", lambda: bool(r.state.get("ulock")), 4)
                    time.sleep(bench.DB_SETTLE_S)   # the GUI writes what changed every 3 s: the lock's bit saved
                    pid0, bhz0 = _rtl_pid(r), int(r.state.get("bhz") or 0)
                    bcn0 = oa.cfg(r, "cw_bcn_until")
                    conn0 = _connected_s(r) if n == "x6100" else None
                    s.confirm(f"page D, lock on, rtlspec {pid0}, B {bhz0}, beacon until {bcn0}, wlan changes {conn0}")
                with o.step(f"{n}: the GUI stopped and started (perf, gpu.off, an ADIF to import): the stop's probe") \
                        as s:
                    between = ("echo > /mnt/perf; echo > /mnt/gpu.off; "
                               f"printf '%s' '{ADIF}' > /mnt/incoming_log.adi; rm -f /mnt/incoming_log.adi.bak; "
                               + STOP_PROBE[n])
                    probe = _restart(o, n, "the restart test: perf, gpu.off, an ADIF import", between)
                    out["probe"] = " ".join(probe.split())
                    if n == "x6200" and "KEEPER_ON" not in probe:
                        raise Refused("x6200: x6200-base-keeper not running while its GUI was stopped")
                    if n == "x6100":
                        m = re.search(r"CHANGES (\d+)", probe)
                        after = _connected_s(r)
                        if "CARRIER 1" not in probe or (conn0 is not None and (not m or int(m.group(1)) != conn0
                                                                               or after != conn0)):
                            raise Refused(f"x6100: its WiFi over the restart: {out['probe']!r}, link changes "
                                          f"{conn0} before, {after} after")
                    s.confirm(out["probe"])
                with o.step(f"{n}: after: page D, the lock, the daemon, B, the beacon off, perf and the CPU drawing "
                            f"said, the QSO imported") as s:
                    r.wait("page D again", lambda: r.state.get("pg") == "D", 15)
                    if not r.state.get("ulock"):
                        raise Refused(f"{n}: the frequency lock not kept over the restart")
                    pid1, bhz1 = _rtl_pid(r), int(r.state.get("bhz") or 0)
                    if pid0 and pid1 != pid0:
                        raise Refused(f"{n}: the SDR's daemon was {pid0}, now {pid1}: not the same process")
                    if bhz0 and bhz1 != bhz0:
                        raise Refused(f"{n}: B's channel was {bhz0}, now {bhz1}")
                    if str(oa.cfg(r, "msg_cw_beacon")) not in ("0", ""):
                        raise Refused(f"{n}: MSG > CW's beacon reads {oa.cfg(r, 'msg_cw_beacon')!r} after the restart")
                    if str(oa.cfg(r, "cw_bcn_until")) != str(bcn0):
                        raise Refused(f"{n}: the beacon's Until was {bcn0}, now {oa.cfg(r, 'cw_bcn_until')}")
                    time.sleep(12)          # the overlay's first 'perf:' figures (each 10 s)
                    log = oa.ssh(r, "cat /tmp/x6100_log.txt", 20)
                    for want in ("perf: overlay on", "gpu: /mnt/gpu.off: the pictures drawn by the CPU"):
                        if want not in log:
                            raise Refused(f"{n}: no {want!r} in the GUI's log")
                    if not re.search(r"^perf: (?!overlay)", log, re.M):
                        raise Refused(f"{n}: the overlay's figures never came in the GUI's log")
                    own = oa.cfg(r, "callsign")
                    files, rows = "", []
                    for _ in range(10):         # the import runs in a thread of its own at start
                        files = oa.ssh(r, "ls /mnt/incoming_log.adi* 2>/dev/null; true")
                        rows = oa.ssh(r, "sqlite3 /mnt/qso_log.db \"select local_callsign from qso_log where "
                                         "remote_callsign='Q2TST'\"; true").split()
                        if rows or ".failed" in files:
                            break
                        time.sleep(1.0)
                    said = [t for _, t in r.msgs if "mport" in t][-2:]
                    if not rows or (own and rows[0] != own):
                        raise Refused(f"{n}: the imported QSO {'not in the log' if not rows else 'not under the radio callsign'}"
                                      f" (files {files.split()}, said {said})")
                    if "incoming_log.adi.bak" not in files or re.search(r"incoming_log\.adi$", files, re.M):
                        raise Refused(f"{n}: the ADIF file after the import: {files.split()} (said {said})")
                    out["after"] = "page D, lock on, daemon kept, B kept, beacon off, perf and CPU drawing said, " \
                                   "QSO imported (.bak)"
                    s.confirm(out["after"])
                with o.step(f"{n}: the QSO out of the log, the lock off; started again without perf and gpu.off") as s:
                    oa.ssh(r, "sqlite3 /mnt/qso_log.db \"delete from qso_log where remote_callsign='Q2TST'\"; "
                              "rm -f /mnt/incoming_log.adi.failed")
                    r.send("set lock 0")
                    _restart(o, n, "the restart test: perf and gpu.off taken away", "rm -f /mnt/perf /mnt/gpu.off")
                    log = oa.ssh(r, "cat /tmp/x6100_log.txt", 20)
                    if "gpu: /mnt/gpu.off" in log:
                        raise Refused(f"{n}: the CPU's drawing still said after gpu.off went")
                    s.confirm("normal start")
            with o.step(f"{n}: PulseAudio stopped: the audio server and the GUI started again, the run's log kept") as s:
                pid_gui = oa.ssh(r, "pidof x6100_gui").strip()
                logs0 = int(oa.ssh(r, "ls /mnt/app_logs | wc -l").strip() or 0)
                o.st.ev(f"{n}: its GUI restarts (PulseAudio stopped on purpose, planned)", "radio")
                o.planned[n] = "planned: the audio server stopped"
                o.changing += 1
                try:
                    oa.ssh(r, "kill $(pidof pulseaudio)")
                    end = time.time() + 40
                    new = ""
                    while time.time() < end:
                        time.sleep(2)
                        new = oa.ssh(r, "pidof x6100_gui; pidof pulseaudio >/dev/null && echo PA").strip()
                        if "PA" in new and new.split()[0] not in ("", pid_gui, "PA"):
                            break
                    import orch_cable as oc
                    time.sleep(6)
                    oc._reconnect(o, n, "PulseAudio stopped")
                    r.take_control()
                finally:
                    o.changing -= 1
                    o.planned.pop(n, None)
                gui = new.split()[0] if new and new.split()[0] != "PA" else ""
                logs1 = int(oa.ssh(r, "ls /mnt/app_logs | wc -l").strip() or 0)
                if not gui or gui == pid_gui or "PA" not in new:
                    raise Refused(f"{n}: after PulseAudio stopped: GUI {gui or 'none'} (was {pid_gui}), {new!r}")
                if logs1 <= logs0 and logs0 < 30:
                    raise Refused(f"{n}: no new log in /mnt/app_logs ({logs0} -> {logs1})")
                out["audio_loss"] = f"GUI {pid_gui} -> {gui}, app_logs {logs0} -> {logs1}"
                s.confirm(out["audio_loss"])
        return True, f"both GUIs restarted three ways, each kept and read what it should: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        for n, k in done:
            try:
                o.r[n].send("set lock 0")
                k.__exit__(None, None, None)
            except Exception as e:  # noqa: BLE001
                o.st.ev(f"{n}: the lock setting not put back: {e}", "bad")
        for n, r in _both(o):
            try:
                oa.ssh(r, "sqlite3 /mnt/qso_log.db \"delete from qso_log where remote_callsign='Q2TST'\"; "
                          "rm -f /mnt/perf /mnt/gpu.off /mnt/incoming_log.adi.failed")
                log = oa.ssh(r, "cat /tmp/x6100_log.txt", 20)
                if "perf: overlay on" in log or "gpu: /mnt/gpu.off" in log:     # left on by a step that failed
                    _restart(o, n, "the restart test: a clean start after it", "true")
            except (Refused, OSError):
                pass
        oa.home_both(o)


# ---- Display settings that change the classic screen where they say (settings-display-*, display-*) ----

# (feature, setting, the other value to try, its box on the classic screen (landscape px) that must change)
DISPLAY = [
    ("settings-display-band-plan-strip", "band_plan", lambda v: 0 if str(v) != "0" else 1, (0, 150, 800, 175)),
    ("settings-display-filter-shade", "filter_shade", lambda v: 0 if str(v) != "0" else 4, (0, 140, 800, 330)),
    ("settings-display-spectrum", "spectrum_show", lambda v: 1 if str(v) == "0" else 0, (0, 130, 800, 400)),
    ("settings-display-waterfall-mode", "wf_mode", lambda v: 1 if str(v) == "0" else 0, (0, 150, 800, 400)),
    ("settings-display-broadcast-names", "broadcast_name", lambda v: 0 if str(v) != "0" else 1, (0, 100, 800, 175)),
    ("settings-display-knob-info", "knob_info", lambda v: 0 if str(v) != "0" else 1, (0, 380, 800, 480)),
]


def _diff(a, b, box):
    """The share of box's pixels that differ by more than 40 grey levels between pictures a and b (0-1)"""
    from PIL import Image, ImageChops
    ia, ib = Image.open(a).convert("L").crop(box), Image.open(b).convert("L").crop(box)
    d = ImageChops.difference(ia, ib).point(lambda x: 255 if x > 40 else 0)
    return sum(1 for p in d.getdata() if p) / float(ia.width * ia.height)


def t_display_settings(o):
    """On each radio's classic screen, each display setting set the other way (its cfg, read back): its part of the
    screen changes more than the screen moves by itself between two pictures (the waterfall's own scroll measured
    first), and set back it comes back near where it was; the settings put back as found"""
    import orch_cable as oc
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: the classic screen") as s:
                oc.classic(r)
                time.sleep(1.5)
                s.confirm("classic")
            for feat, name, other, box in DISPLAY:
                with o.step(f"{n}: {name} the other way: {feat}'s part of the screen changes, and back") as s:
                    v0 = oa.cfg(r, name)
                    a1 = oa.shot(o, r, f"{name} as found a")
                    time.sleep(0.6)
                    a2 = oa.shot(o, r, f"{name} as found b")
                    alone = _diff(a1, a2, box)
                    extra = {"smeter_needle": 1} if name == "smeter_lamp" else {}     # the lamp lights the needle
                    if extra:
                        needle = oa.cfg_kept(r, extra)
                        needle.__enter__()
                        time.sleep(1.0)
                        a1 = oa.shot(o, r, f"{name} as found a (needle)")
                        time.sleep(0.6)
                        a2 = oa.shot(o, r, f"{name} as found b (needle)")
                        alone = _diff(a1, a2, box)
                    with oa.cfg_kept(r, {name: other(v0)}):
                        time.sleep(1.5)
                        b = oa.shot(o, r, f"{name} {other(v0)}")
                        moved = _diff(a2, b, box)
                    time.sleep(1.5)
                    c = oa.shot(o, r, f"{name} back")
                    back = _diff(a2, c, box)
                    if extra:
                        needle.__exit__(None, None, None)
                    out[name] = (round(alone, 3), round(moved, 3), round(back, 3))
                    if moved < max(0.01, 2.5 * alone):
                        raise Refused(f"{n}: {name} {v0} -> {other(v0)} changed its part of the screen {moved:.1%}, "
                                      f"the screen by itself {alone:.1%}")
                    if back > max(0.6 * moved, 2.5 * alone + 0.01):
                        raise Refused(f"{n}: {name} back to {v0}: its part {back:.1%} off the first picture "
                                      f"(the change was {moved:.1%})")
                    s.confirm(f"by itself {alone:.1%}, set {moved:.1%}, back {back:.1%}")
        return True, f"each display setting changed its part of the classic screen and came back: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- The radios' system tools read only (system-base-fw-flash-check, system-update-integrity) ----

def t_system_readonly(o):
    """On each radio, read only: x6x00-base-flash --check takes its own base firmware file (Xiegu's, named, this radio
    said) and refuses the other radio's, by its name and renamed (identified by its contents), 'nothing was sent to
    the base'; x6100-update bundle-info refuses a bundle made for the other radio and one that would write outside its
    allowed folders (only the manifest is read: nothing staged, nothing changed)"""
    import json
    import subprocess
    import tarfile
    import tempfile
    res = {}
    own = {"x6100": "/mnt/base_update/X6100_BBFW_V1.1.9_251127001.xgf",
           "x6200": "/mnt/base_update/x6200_mcu_20260820141208.xgf"}
    tmp = tempfile.mkdtemp()
    try:
        with o.step("base firmware: each radio's own file checked, the other's refused by its name and renamed") as s:
            for n, r in _both(o):
                other_n = next(m for m in own if m != n)
                ok = oa.ssh(r, f"x6x00-base-flash --check {own[n]} 2>&1; true")
                if f"this radio is an {n.upper()}" not in ok or "refused" in ok:
                    raise Refused(f"{n}: --check of its own file said {' '.join(ok.split())[:160]!r}")
                local = os.path.join(tmp, os.path.basename(own[other_n]))
                subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes",
                                f"root@{o.r[other_n].host}:{own[other_n]}", local], check=True, timeout=60)
                for dest in (f"/tmp/{os.path.basename(own[other_n])}", "/tmp/bench_unknown_name.xgf"):
                    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", local, f"root@{r.host}:{dest}"],
                                   check=True, timeout=60)
                    try:
                        out = oa.ssh(r, f"x6x00-base-flash --check {dest} 2>&1; echo RC=$?")
                    finally:
                        oa.ssh(r, f"rm -f {dest}")
                    if "refused" not in out or "nothing was sent to the base" not in out:
                        raise Refused(f"{n}: --check of the {other_n}'s file as {os.path.basename(dest)} said "
                                      f"{' '.join(out.split())[:160]!r}")
                res[f"{n} fw"] = "own checked, the other's refused (named and renamed)"
            s.confirm(str(res))
        with o.step("update bundles: another radio's refused, one writing outside the allowed folders refused") as s:
            for n, r in _both(o):
                other_n = next(m for m in own if m != n)
                for label, man in (("other radio", {"product": "GoldCorpHF", "model": other_n, "version": "0.0.0-test",
                                                    "files": []}),
                                   ("outside", {"product": "GoldCorpHF", "model": n, "version": "0.0.0-test",
                                                "files": [{"path": "/root/bench_test", "sha256": "0" * 64}]})):
                    mpath = os.path.join(tmp, "manifest.json")
                    with open(mpath, "w") as f:
                        json.dump(man, f)
                    bpath = os.path.join(tmp, f"bench_{label.replace(' ', '_')}.tar.gz")
                    with tarfile.open(bpath, "w:gz") as tf:
                        tf.add(mpath, arcname="manifest.json")
                    subprocess.run(["scp", "-4", "-q", "-o", "BatchMode=yes", bpath, f"root@{r.host}:/tmp/bench_b.tgz"],
                                   check=True, timeout=60)
                    try:
                        out = oa.ssh(r, "x6100-update bundle-info /tmp/bench_b.tgz 2>&1; echo RC=$?")
                    finally:
                        oa.ssh(r, "rm -f /tmp/bench_b.tgz")
                    want = (f"the bundle is for the {other_n.upper()}" if label == "other radio"
                            else "the bundle wants to write /root/bench_test: refused")
                    if want not in out or "RC=0" in out:
                        raise Refused(f"{n}: bundle-info of a bundle {label} said {' '.join(out.split())[:160]!r}")
                    res[f"{n} {label}"] = want
            s.confirm(str(res))
        return True, f"both radios' tools refuse the other radio's firmware and bundles, read only: {res}", res
    except (Refused, subprocess.SubprocessError, OSError) as e:
        return False, str(e), res
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)



# ---- The classic screen's status line (main-screen-status-bar-state, main-screen-battery-indicator,
#      main-screen-status-wifi-icon) ----

def _green_px(path, box):
    """Pixels in box that are plainly green (the WiFi icon connected)"""
    from PIL import Image
    im = Image.open(path).convert("RGB").crop(box)
    return sum(1 for (rr, g, b) in im.getdata() if g > 120 and g > rr + 40 and g > b + 40)


def t_classic_status(o):
    """Each radio's classic screen, its top lines read off the picture: VFO-A, the mode, AGC and its setting, PRE/ATT;
    the charge at the top right ('NN%'); the WiFi icon green (the radio is on its network: the bench talks to it)"""
    import orch_cable as oc
    res = {}
    try:
        for n, r in _both(o):
            with o.step(f"{n}: the classic screen's status line, charge and WiFi icon") as s:
                oc.classic(r)
                time.sleep(1.5)
                p = oa.shot(o, r, "classic status")
                txt = oa.ocr(p, box=(0, 0, 800, 60), psm=6, scale=3) + "\n" + oa.ocr(p, box=(0, 0, 800, 60), psm=11)
                up = txt.upper()
                miss = [w for w, rx in (("VFO-A", r"VFO[- ]?A"), ("the mode", r"\b(USB|LSB|CW|AM|NFM)"),
                                        ("AGC", r"AGC|FAST|SLOW|AUTO|\bOFF\b"), ("the charge", r"\d{1,3}\s?%"))
                        if not re.search(rx, up)]
                green = _green_px(p, (560, 0, 800, 40))
                res[n] = {"text": " ".join(up.split())[:100], "green": green}
                if miss:
                    raise Refused(f"{n}: the status line reads no {miss} ({res[n]['text']!r})")
                if green < 6:
                    raise Refused(f"{n}: no green WiFi icon at the top right ({green} green pixels)")
                s.confirm(f"{res[n]['text'][:60]}; WiFi icon green ({green} px)")
        return True, f"both classic screens show VFO-A, the mode, AGC, the charge and a green WiFi icon: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- Settings taken and kept (settings-general-freq-acceleration, text-log-settings, settings-walkie-microphone,
#      settings-audio-microphone, settings-audio-moni-level, settings-txcal-tx-ranges, tx-ranges-choice,
#      settings-general-transverters) ----

# (feature, setting, the other value): each set through its own validator (the remote's cfg), read back, put back
KEPT = [
    ("settings-general-freq-acceleration", "freq_accel_onset", lambda v: 4 if str(v) != "4" else 2),
    ("text-log-settings", "textlog_mb", lambda v: 5 if str(v) != "5" else 10),
    ("settings-walkie-microphone", "walkie_mic", lambda v: 1 if str(v) != "1" else 0),
    ("settings-audio-microphone", "hmic", lambda v: 25 if str(v) != "25" else 20),
    ("settings-audio-moni-level", "moni", lambda v: 5 if str(v) != "5" else 0),
    ("settings-txcal-tx-ranges", "tx_ranges", lambda v: 1 if str(v) != "1" else 0),
    ("settings-general-transverters", "transverter_1_shift", lambda v: 116000000 if str(v) != "116000000" else 0),
]


def t_settings_kept(o):
    """On each radio a set of settings each set the other way through its own validator (the remote's cfg), read back
    as set, read again a second later (kept), and put back as found: the frequency acceleration's onset, the text
    log's size limit, the walkie's microphone, the hand mic's gain, the Moni level, the TX ranges, a transverter's
    shift; an out-of-range value refused (the onset 9: still as it was)"""
    res = {}
    try:
        for n, r in _both(o):
            out = res[n] = {}
            with o.step(f"{n}: each setting set the other way, kept, put back; the onset 9 refused") as s:
                for feat, name, other in KEPT:
                    v0 = oa.cfg(r, name)
                    with oa.cfg_kept(r, {name: other(v0)}):
                        time.sleep(1.0)
                        again = oa.cfg(r, name)
                        if str(again) != str(other(v0)) and not oa._same_num(again, other(v0)):
                            raise Refused(f"{n}: {name} read {again!r} a second after it was set to {other(v0)!r}")
                    out[name] = f"{v0} -> {other(v0)} -> {oa.cfg(r, name)}"
                v0 = oa.cfg(r, "freq_accel_onset")
                bench.cfg_set(r, "freq_accel_onset", 9)
                got = oa.cfg(r, "freq_accel_onset")
                bench.cfg_set(r, "freq_accel_onset", v0)
                if not str(got).isdigit() or not 1 <= int(got) <= 5:
                    raise Refused(f"{n}: the onset 9 (outside 1-5) read back {got!r}")
                out["onset 9"] = f"held in 1-5 ({got})"
                s.confirm(str(out))
        return True, f"each setting taken, kept and put back on both; out of range refused: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- The Sentry's NOW readings (scancfg-now-box, sentry-now-sun) ----

def t_sentry_now(o):
    """Each radio's SCAN CFG (from page A) shows its NOW box naming what each radio's scan does ('A X6100/X6200 ...');
    page D's rows say NOW as OPEN, CLOSED, LOCAL, OPENING, CLOSING or 'n SIG' (from the sun at the fix, the phone's
    position or the QTH locator)"""
    import suite_cover_b as cb
    res = {}
    try:
        for n, r in _both(o):
            with o.step(f"{n}: SCAN CFG's NOW box names the X6x00's scan") as s:
                bench.open_page(r, "A")
                cb.cfg_open(r, "A")
                time.sleep(1.2)
                p = oa.shot(o, r, "scan cfg now")
                txt = (oa.ocr(p, psm=11) + "\n" + oa.ocr(p, psm=6, stretch=True)).upper()
                cb.cfg_close(r)
                if not re.search(r"\bA\s+X\s?6\s?[12I]\s?[0O]\s?[0O]\s+[A-Z]", txt) or "LAST" not in txt:
                    raise Refused(f"{n}: SCAN CFG's NOW box names no 'A {n.upper()} <doing>' and LAST locks "
                                  f"({' '.join(txt.split())[:140]!r})")
                res[f"{n} scan cfg"] = "NOW: A " + n.upper()
                s.confirm(res[f"{n} scan cfg"])
        with o.step("both on page D: its rows' NOW from the sun or live signals") as s:
            oa.page_both(o, "D")
            time.sleep(1.5)
            for n, r in _both(o):
                p = oa.shot(o, r, "page D now")
                txt = (oa.ocr(p, psm=11) + "\n" + oa.ocr(p, psm=6, stretch=True)).upper()
                words = re.findall(r"\b(OPEN|CLOSED|LOCAL|OPENING|CLOSING|\d+\s?SIG)\b", txt)
                res[f"{n} D"] = sorted(set(words))
                if len(words) < 3:
                    raise Refused(f"{n}: page D's NOW column reads {words}")
            s.confirm(str({k: v for k, v in res.items() if k.endswith(" D")}))
        return True, f"SCAN CFG's NOW box and page D's NOW read on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)



# ---- Windows that look alike on both radios (lookfeel-screen-first-start, lookfeel-screen-swr-scan,
#      lookfeel-screen-wspr) ----

LOOK = [("lookfeel-screen-first-start", 131, "First start", r"FIRST\s*START|WELCOME|STEP"),
        ("lookfeel-screen-swr-scan", 102, "SWR Scan", r"SWR"),
        ("lookfeel-screen-wspr", 116, "WSPR", r"WSPR")]


def t_lookalike(o):
    """First start, SWR Scan and WSPR opened on both radios (the remote's open; none of them sends anything by
    opening): each window named on both pictures, and the two radios' pictures alike (gold on black, the same layout:
    under a third of their pixels differ); closed after"""
    res = {}
    try:
        for feat, action, name, rx in LOOK:
            with o.step(f"both: {name} opened, named on each picture, the two alike") as s:
                pics = {}
                for n, r in _both(o):
                    r.take_control()
                    r.send(f"open {action}")
                for n, r in _both(o):
                    r.wait(f"{name} open", lambda r=r: oa.win(r).lower().startswith(name.lower().split()[0]), 8)
                time.sleep(1.5)
                for n, r in _both(o):
                    pics[n] = oa.shot(o, r, f"look {name}")
                    txt = (oa.ocr(pics[n], psm=11) + oa.ocr(pics[n], psm=6, stretch=True)).upper()
                    if not re.search(rx, txt):
                        raise Refused(f"{n}: {name}'s picture reads no {rx!r} ({' '.join(txt.split())[:100]!r})")
                apart = _diff(pics[H.A], pics[H.B], (0, 0, 800, 480))
                res[name] = round(apart, 3)
                for n, r in _both(o):
                    r.send("close")
                if apart > 0.33:
                    raise Refused(f"{name}: the two radios' pictures differ in {apart:.0%} of their pixels")
                s.confirm(f"named on both, {apart:.0%} of pixels apart")
        return True, f"First start, SWR Scan and WSPR open alike on both radios: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


# ---- Settings > Info's cards (settings-info-firmware, settings-info-power-box) ----

def t_settings_info(o):
    """Settings opened on both, its Info tab: the FIRMWARE card names the firmware's version (the remote's own) and
    the base's; the X6200's its PA bias; the POWER box says Battery, External and Time left; closed after"""
    res = {}
    names = ("Settings",)
    try:
        for n, r in _both(o):
            with o.step(f"{n}: Settings > Info: the FIRMWARE card and the POWER box") as s:
                r.take_control()
                r.send("open 104")
                r.wait("Settings open", lambda: oa.win(r).lower().startswith("settings"), 8)
                oa.press_label(r, r"^Info", names, rest=1.2)
                time.sleep(1.0)
                p = oa.shot(o, r, "settings info")
                txt = (oa.ocr(p, psm=11) + "\n" + oa.ocr(p, psm=6, stretch=True)).upper()
                want = ["FIRMWARE", "BATTERY", "EXTERNAL", "TIME LEFT"] + (["BIAS"] if n == "x6200" else [])
                miss = [w for w in want if not oa.ocr_has(txt, w)]
                if not re.search(r"0\.3\.1\d", txt):
                    miss.append("the firmware's version")
                res[n] = "ok" if not miss else miss
                r.send("close")
                if miss:
                    raise Refused(f"{n}: Settings > Info reads no {miss} ({' '.join(txt.split())[:140]!r})")
                s.confirm("FIRMWARE with its version, POWER box" + (", PA bias" if n == "x6200" else ""))
        return True, f"Settings > Info's FIRMWARE card and POWER box read on both: {res}", res
    except Refused as e:
        return False, str(e), res
    finally:
        oa.home_both(o)


COVERS = {
    "ui-restart": (["perf-overlay", "gpu-off-file", "sentry-resume-after-restart", "cw-beacon-off-after-restart",
                    "sdr-daemon-persistent", "network-x6100-wifi-power-held", "system-x6200-base-keeper",
                    "incoming-log-import", "gui-restart-on-audio-loss"],
                   ["radio-core-freq-lock-per-band-mode", "sentry-b-state-kept"]),
    "ui-display-settings": ([f for f, _, _, _ in DISPLAY] + ["display-knob-info-lines"], []),
    "ui-classic-status": (["main-screen-status-bar-state", "main-screen-battery-indicator",
                           "main-screen-status-wifi-icon"], []),
    "ui-settings-kept": ([], [f for f, _, _ in KEPT] + ["tx-ranges-choice"]),
    "ui-sentry-now": (["scancfg-now-box", "sentry-now-sun"], []),
    "ui-lookalike": ([f for f, _, _, _ in LOOK], []),
    "ui-settings-info": (["settings-info-firmware", "settings-info-power-box"], []),
    "ui-system-readonly": (["system-base-fw-flash-check"], ["system-update-integrity", "system-update-sd-card-guard"]),
}


def tests(Test, A, B):
    """orch's Test rows of this module"""
    return [
        Test("ui-restart", "both, one at a time", "each GUI restarted (planned): page, lock, SDR daemon, B, beacon "
             "kept or off; perf, gpu.off and an ADIF import read at start; the X6200's base keeper and the X6100's "
             "WiFi while stopped; again by PulseAudio stopped", t_restart, 300),
        Test("ui-display-settings", "both", "on the classic screen each display setting (band plan, filter shade, "
             "needle lamp, spectrum, waterfall mode, broadcast names, knob info) changes its part of the screen and "
             "comes back", t_display_settings, 180),
        Test("ui-classic-status", "both", "the classic screen's status line (VFO-A, mode, AGC), the charge and a green "
             "WiFi icon", t_classic_status, 30),
        Test("ui-settings-kept", "both", "seven settings set through their validators, kept, put back; an onset "
             "outside 1-5 refused", t_settings_kept, 40),
        Test("ui-sentry-now", "both", "SCAN CFG's NOW box names A's radio; page D's rows say NOW (OPEN, CLOSED, ... or "
             "n SIG)", t_sentry_now, 40),
        Test("ui-lookalike", "both", "First start, SWR Scan and WSPR open alike on both radios (named, under a third "
             "of their pixels apart)", t_lookalike, 50),
        Test("ui-settings-info", "both", "Settings > Info: the FIRMWARE card with the version (the X6200's PA bias), "
             "the POWER box", t_settings_info, 40),
        Test("ui-system-readonly", "both", "read only: base-flash --check takes the radio's own firmware and refuses "
             "the other's (named and renamed); bundle-info refuses another radio's bundle and one writing outside",
             t_system_readonly, 60),
    ]
