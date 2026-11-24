"""The 1:1 suite's "cover" batch: features of features.json the other batches had not reached, each tried on both
radios and compared, receive only. A test changes nothing it does not put back (per_radio's snapshot and finish; the
SSH reads change nothing), and every radio is left on the Sentry's page A.

    suite.py run cover [--only TEST,...]

The tests (the features each covers in brackets):
  filter-cw       'set filter LO HI' in CW: the passband centred on the key tone, its width HI-LO, never under 0
  filter-wfm      the X6200's WFM filter: 0-9000 taken, a wider one held to the radio's top; the X6100 refuses WFM
  walkie-upright  G held up: the remote's screen pictures come portrait (taller than wide)
  refusals        an unknown key, a position off the globe, a pairing without a name or with a wrong code, unpair
                  and sessions from an unpaired page: each its own error, nothing changed; sessions for the bench's
  rigctl-local    rigctl on 127.0.0.1 (Pat's rules) answers otherwise than rigctl from the network
  sessions-mtime  a session's reconnects do not rewrite /mnt/remote_sessions.conf
  config-files    /mnt/tci.conf and ft8_udp.conf with the keys the manual names
  bootvideo       the last start's boot video decoded by the video engine
  gui-cpu         the GUI's main thread on the Sentry's A as a share of one core, against the manual's table
  tci-rate        TCI's audio at its default (48 kHz float32 stereo) at full rate over the radio's WiFi for 30 s
  web-writes      a PSK macro added and deleted (non-ASCII refused), a test SSH key added, logged in with and
                  removed, a page under another name refused: each file compared with its copy after, put back if not
  tci-switches    TCI listen only (a tune ignored by TCI and by rigctl, which follows it), TCI off (rigctl refused too),
                  everything put back, /mnt/tci.conf as found; TCI's Bluetooth service on channel 22
  ft8-more        FT8's MODE through FT8, FT4, FT2 with each one's 20 m frequency; the main knob moving TX Hz
  channels-web    channels added from the web at 145.5 MHz (both) and 60 MHz (the X6200 only), taken out again
  smartcw-guard   SmartCW's page with no stick says so, a POST from another site refused
  winlink-pat     Pat's mailbox and its rig (rigctl on 127.0.0.1), no P2P listen; a look when Winlink is off

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import json
import os
import socket
import subprocess
import sys
import time

import bench
from remote_ws import Remote
from suite_more import F_TEST, per_radio
from suite_proto import by_radio, expect, fresh, http, jpeg_size, line_after, texts

HERE = os.path.dirname(os.path.abspath(__file__))


def ssh(host, cmd, timeout=40):
    r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{host}", cmd],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout


def wait_state(r, cond, secs=3.0):
    t0 = time.time()
    while time.time() - t0 < secs:
        if cond(r.state):
            return True
        time.sleep(0.05)
    return cond(r.state)


# ---- with the bench's session (per_radio: snapshot, finish, page A) ----

def t_filters(run):
    def cw(r):
        bench.ensure(r, {"freq": 14030000, "mode": 4})
        kt = int(r.state.get("kt", 0))
        out = []
        for lo, hi in ((0, 500), (200, 1200), (0, 3000)):
            r.send(f"set filter {lo} {hi}")
            wait_state(r, lambda s: int(s.get("fhi", 0)) - int(s.get("flo", 0)) in (hi - lo, ), 3.0)
            flo, fhi = int(r.state["flo"]), int(r.state["fhi"])
            out.append((lo, hi, flo, fhi))
        # the width as asked; centred on the key tone unless that would take the low edge under 0
        ok = all(fhi - flo == hi - lo and flo >= 0 and (abs((flo + fhi) / 2 - kt) <= 1 or flo == 0)
                 for lo, hi, flo, fhi in out)
        return ok, f"key tone {kt} Hz: " + "; ".join(f"{lo}-{hi} asked: {flo}-{fhi}" for lo, hi, flo, fhi in out)
    per_radio(run, "cover", "filter-cw", cw, ["panel-filter-cw"])

    def wfm(r):
        r.errs.clear()
        if r.name == "x6100":
            r.send("set freq 99900000")
            r.send("set mode 9")
            time.sleep(1.5)
            freq_no = any("not a frequency this radio tunes" in e for e in r.errs)
            mode_no = any(e.startswith("mode") for e in r.errs)
            return freq_no and mode_no and int(r.state.get("mode", -1)) != 9, \
                f"no FM broadcast on the X6100: 99.9 MHz refused {freq_no}, WFM refused {mode_no} ({r.errs[-2:]})"
        bench.ensure(r, {"freq": 99900000, "mode": 9})
        res = []
        for hi in (5000, 20000):
            r.send(f"set filter 0 {hi}")
            wait_state(r, lambda s: int(s.get("fhi", 0)) == hi, 2.0)
            time.sleep(0.5)
            res.append((hi, int(r.state["flo"]), int(r.state["fhi"])))
        ok = res[0][2] == 5000 and res[1][2] == 8000 and res[1][1] == 0
        return ok, "WFM: " + "; ".join(f"0-{hi} asked: {lo}-{fhi}" for hi, lo, fhi in res) + \
            " (the manual: an edge up to 8 kHz in WFM, 16 kHz whole)"
    per_radio(run, "cover", "filter-wfm", wfm, ["panel-filter-wfm"])


def t_walkie_upright(run):
    def fn(r):
        # the Sentry's G opened here (it had relied on the page an earlier test left; orch starts each on A), and the
        # page found put back after
        page = r.state.get("pg")
        bench.open_page(r, "G")
        c, _ = fresh(r.host, secs=0.3)
        try:
            c.send("screen 1")
            time.sleep(3.0)
            fr = [f for f in list(c.frames) if f[2][8:10] == b"\xff\xd8"]
        finally:
            c.close()
        sizes = [jpeg_size(f[2][8:]) for f in fr[-3:]]
        ok = bool(sizes) and all(s and s[1] > s[0] for s in sizes) and r.state.get("pg") == "G"
        said = f"page {r.state.get('pg')}: {len(fr)} pictures, the last {sizes} (width, height): portrait {ok}"
        if page and page != "G":
            bench.open_page(r, page)
        return ok, said
    per_radio(run, "cover", "walkie-upright", fn, ["screen-walkie-upright"])


def t_refusals(run):
    def fn(r):
        f0 = {k: r.state.get(k) for k in ("freq", "mode", "vol")}
        t0 = time.time()
        r.send("key FOO 1")
        key = line_after(r, t0, "err unknown key")
        t0 = time.time()
        r.send("pos 95 0")
        pos = line_after(r, t0, "err pos")
        c, _ = fresh(r.host, secs=0.5)
        try:
            c.send("pair 123")
            noname = expect(c, "err pair CODE NAME", 3.0)
            c.send("pair 000000 Bench")
            wrong = expect(c, "err pairing refused:", 3.0)
            c.send("unpair")
            unp = expect(c, "err not paired", 3.0)
            c.send("sessions")
            sess_view = expect(c, "err admin only", 3.0)
        finally:
            c.close()
        t0 = time.time()
        r.send("sessions")
        mine = line_after(r, t0, "sessions [") or line_after(r, t0, "err admin only", 1.0)
        listed = None
        if mine and mine.startswith("sessions ["):
            try:
                listed = len(json.loads(mine.split(" ", 1)[1]))
            except ValueError:
                listed = None
        same = all(r.state.get(k) == v for k, v in f0.items())
        ok = bool(key and pos and noname and wrong and "wrong or expired" in wrong and unp and sess_view and mine
                  and same)
        return ok, (f"key FOO: {key!r}; pos 95 0: {pos!r}; pair 123: {noname!r}; a wrong code: {wrong!r}; unpaired "
                    f"unpair: {unp!r}, sessions: {sess_view!r}; the bench's sessions: "
                    f"{(str(listed) + ' sessions listed') if listed is not None else mine!r}; nothing changed {same}")
    per_radio(run, "cover", "refusals", fn, ["screen-keys", "panel-gps-pos", "pair-wrong-code", "sessions-unpair",
                                              "sessions-list"])


# ---- without a session: the network and SSH ----

GET_INFO = "\\get_info\n"


def rig_ask(host, port=4532, text=GET_INFO, secs=3.0):
    s = socket.create_connection((host, port), timeout=5)
    try:
        s.sendall(text.encode())
        s.settimeout(secs)
        got = b""
        t0 = time.time()
        while time.time() - t0 < secs and b"RPRT" not in got and b"\n" not in got[-1:]:
            try:
                d = s.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            got += d
        return got.decode(errors="replace").strip()
    finally:
        s.close()


LOCAL_RIG = ("python3 -c \"import socket;s=socket.create_connection(('127.0.0.1',4532),5);s.sendall(b'\\\\\\\\get_info\\\\n');"
             "s.settimeout(3);print(s.recv(4096).decode(errors='replace').strip())\"")


def t_rigctl_local(run):
    def fn(n, h, ip):
        net = rig_ask(h)
        local = ssh(h, LOCAL_RIG).strip()
        ok = bool(net) and bool(local) and net != local and "GoldCorpHF" in net
        return ok, f"from the network {net[:80]!r}; on the radio (Pat's) {local[:80]!r}: they differ {net != local}"
    by_radio(run, "cover", "rigctl-local", ["rigctl-local-pat-rules"], fn)


def t_sessions_mtime(run):
    def fn(n, h, ip):
        # one connection first: a session not seen for an hour is written at its first connection, by design
        # (remote_server.c sess_touch: another address, or an hour on); the three after it must write nothing
        # (orch, 2026-11-24: the bench's receive-only session, an hour unused, wrote the file inside the check)
        r = bench.Radio(n, run.c["radios"][n], False)
        time.sleep(1.0)
        r.close()
        time.sleep(1.5)
        before = ssh(h, "date -r /mnt/remote_sessions.conf +%s").strip()
        for _ in range(3):
            r = bench.Radio(n, run.c["radios"][n], False)
            time.sleep(1.0)
            r.close()
            time.sleep(0.5)
        after = ssh(h, "date -r /mnt/remote_sessions.conf +%s").strip()
        ok = bool(before) and before == after
        return ok, f"/mnt/remote_sessions.conf written at {before}, after 3 sessions came and went {after}"
    by_radio(run, "cover", "sessions-mtime", ["sessions-last-seen"], fn)


OPTIONAL = {"/mnt/tci.conf": "TCI on its defaults (on, port 50001, operate, Ask): written by the first change"}


def t_config_files(run):
    want = {"/mnt/tci.conf": ("enabled", "port", "trust", "tx_confirm"), "/mnt/ft8_udp.conf": ("enabled", "host")}

    def fn(n, h, ip):
        out, ok = [], True
        for path, keys in want.items():
            text = ssh(h, f"cat {path} 2>/dev/null")
            have = {ln.split("=", 1)[0].strip() for ln in text.splitlines() if "=" in ln and not ln.startswith("#")}
            miss = [k for k in keys if k not in have]
            if not text and path in OPTIONAL:
                out.append(f"{path}: none, {OPTIONAL[path]}")
                continue
            ok &= bool(text) and not miss
            out.append(f"{path}: {'there' if text else 'MISSING'}, keys {sorted(have)}" + (f", lacks {miss}" if miss else ""))
        return ok, "; ".join(out)
    by_radio(run, "cover", "config-files", ["mnt-config-files"], fn)


def t_bootvideo(run):
    def fn(n, h, ip):
        log = ssh(h, "tail -4 /tmp/bootvideo.log 2>/dev/null; cut -d. -f1 /proc/uptime")
        lines = log.strip().splitlines()
        up = lines[-1] if lines else "?"
        ok = any("video engine decoded it" in x for x in lines[:-1])
        return ok, f"/tmp/bootvideo.log (up {up} s): {' | '.join(lines[:-1])[-220:]!r}"
    by_radio(run, "cover", "bootvideo", ["boot-video-hw-decode"], fn)


GUI_CPU = r"""
p=$(pidof x6100_gui); a=$(cut -d' ' -f14,15 /proc/$p/task/$p/stat); sleep 10; b=$(cut -d' ' -f14,15 /proc/$p/task/$p/stat)
echo "$a $b"; grep -m1 -i 'gpu:' /tmp/x6100_log.txt
"""


def gui_share(h):
    """The GUI's main thread over 10 s as a share of one core, and the log's gpu: line"""
    out = ssh(h, GUI_CPU, timeout=60).strip().splitlines()
    u0, s0, u1, s1 = (int(x) for x in out[0].split())
    return (u1 + s1 - u0 - s0) / 10, (out[1] if len(out) > 1 else "")


def t_gui_cpu(run):
    """The GUI's main thread on the Sentry's A against the manual's table, and a Panel connection (no pictures)
    costing it nothing: up to v0.3.12.47 any connection had the screen copied for the picture encoder (a quarter of a
    core: 9 -> 33 % on the X6200)"""
    def fn(n, h, ip):
        c, _ = fresh(h, secs=0.3)
        try:
            c.send("panel 1")
            st = next((json.loads(x[6:]) for x in texts(c, 2.0) if x.startswith("state ")), {})
            with_panel, _ = gui_share(h)
        finally:
            c.close()
        time.sleep(3.0)
        alone, gpu = gui_share(h)
        on_gpu = bool(gpu) and "off" not in gpu.lower() and "no " not in gpu.lower()
        top = 30 if on_gpu else 45                              # the table: A 20-22 % with the GPU, 34-35 % without
        ok = st.get("pg") == "A" and alone <= top and with_panel <= alone + 4
        return ok, (f"page {st.get('pg')}: the main thread {alone:.1f} % of one core over 10 s (at most {top}: "
                    f"{'with' if on_gpu else 'without'} the GPU), {with_panel:.1f} % with a Panel connected (at most "
                    f"4 more); {gpu[:80]!r}")
    by_radio(run, "cover", "gui-cpu", ["gpu-cpu-load", "remote-panel-no-screen-cost"], fn)


def t_tci_rate(run):
    def fn(n, h, ip):
        r = subprocess.run([sys.executable, "-B", os.path.join(HERE, "tci_rate.py"), h, "--seconds", "30",
                            "--format", "wsjtx"], capture_output=True, text=True, timeout=90)
        out = (r.stdout + r.stderr).strip().splitlines()
        last = next((x for x in reversed(out) if x.startswith(("PASS", "FAIL"))), out[-1] if out else "")
        return r.returncode == 0 and last.startswith("PASS"), f"48 kHz float32 stereo, 30 s: {last[:200]}"
    by_radio(run, "cover", "tci-rate", ["tci-wifi-throughput"], fn)


# ---- the web's writes, each put back (the file compared with its copy, restored from it if it differs) ----

KEEP = r"""
f={f}; b=/tmp/bench_keep_$(basename $f)
if [ "$1" = save ]; then if [ -e $f ]; then cp -p $f $b; echo saved; else rm -f $b; echo none; fi
else if [ -e $b ]; then cmp -s $f $b && echo same || {{ cp -p $b $f; echo restored; }}; rm -f $b
     else rm -f $f; echo removed; fi; fi
"""


def keep(h, path, what):
    """'save' a radio's file aside, or 'back': compared with that copy and put back if it differs"""
    return ssh(h, "set -- " + what + "\n" + KEEP.format(f=path)).strip()


def t_web_writes(run):
    def macros(n, h, ip):
        path = "/mnt/psk_macros.txt"
        k0 = keep(h, path, "save")
        try:
            st, body = http(ip, "/api/macros/psk")
            before = json.loads(body)["macros"] if st == 200 else None
            st1, body1 = http(ip, "/api/macros/psk", method="POST", body={"name": "BENCH", "text": "Q9TST TEST"})
            after = json.loads(body1)["macros"] if st1 == 200 else []
            mine = next((m for m in after if m["name"] == "BENCH" and m["text"] == "Q9TST TEST"), None)
            in_file = "BENCH\tQ9TST TEST" in ssh(h, f"cat {path}")
            bad, _ = http(ip, "/api/macros/psk", method="POST", body={"name": "BENCH2", "text": "caf\u00e9"})
            st2 = None
            if mine:
                st2, _ = http(ip, f"/api/macros/psk/{mine['id']}", method="DELETE")
            st3, body3 = http(ip, "/api/macros/psk")
            back_list = json.loads(body3)["macros"] if st3 == 200 else None
        finally:
            k1 = keep(h, path, "back")
        ok = st1 == 200 and bool(mine) and in_file and bad == 400 and st2 == 200 and back_list == before and \
            k1 in ("same", "removed")
        return ok, (f"{len(before or [])} PSK macros; one added: {st1}, in the file {in_file}; a non-ASCII text "
                    f"refused {bad}; deleted {st2}, the list as before {back_list == before}; the file {k0}, then {k1}")
    by_radio(run, "cover", "web-macros", ["web-macros-page"], macros)

    def ssh_keys(n, h, ip):
        kf = os.path.join(HERE, "..", "..", "build", "bench_ed25519")
        if not os.path.exists(kf):
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "bench@suite", "-f", kf], check=True)
        pub = open(kf + ".pub").read().strip()
        path = "/root/.ssh/authorized_keys"
        k0 = keep(h, path, "save")
        st2 = st3 = None
        login = False
        try:
            st1, body1 = http(ip, "/api/ssh/keys", method="POST", body={"key": pub})
            keys = json.loads(body1).get("keys", []) if st1 == 200 else []
            mine = next((k for k in keys if k.get("comment") == "bench@suite"), None)
            if mine:
                r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o",
                                    "ConnectTimeout=8", "-i", kf, f"root@{h}", "echo in"], capture_output=True,
                                   text=True, timeout=30)
                login = r.stdout.strip() == "in"
                st2, _ = http(ip, "/api/ssh/keys", method="DELETE", body={"fingerprint": mine["fingerprint"]})
                st3, body3 = http(ip, "/api/ssh/keys", method="DELETE", body={"fingerprint": mine["fingerprint"]})
        finally:
            k1 = keep(h, path, "back")
        ok = st1 == 200 and bool(mine) and str(mine.get("fingerprint", "")).startswith("SHA256:") and login and \
            st2 == 200 and st3 == 404 and k1 == "same"
        return ok, (f"a test key added {st1} ({mine.get('fingerprint', '')[:20] if mine else None}...), ssh with it "
                    f"{'in' if login else 'REFUSED'}; removed {st2}, again {st3} (not there); authorized_keys "
                    f"{k0}, then {k1}")
    by_radio(run, "cover", "web-ssh-keys", ["ssh-add-key", "ssh-remove-key"], ssh_keys)

    def own_name(n, h, ip):
        """By design (apps.py refuse_other_sites): a change needs one of the radio's own names, and so do the reads of
        HOST_ONLY_READS (the SSH keys' list among them); other reads answer by any name"""
        st, _ = http(ip, "/api/macros/psk", host=f"{n}.nl", method="POST", body={"name": "BENCH", "text": "X"})
        st_read, _ = http(ip, "/api/macros/psk", host=f"{n}.nl")
        st_keys, _ = http(ip, "/api/ssh", host=f"{n}.nl")
        st_own, _ = http(ip, "/api/ssh", host=f"{n}.local")
        ok = st == 403 and st_read == 200 and st_keys == 403 and st_own == 200
        return ok, (f"Host {n}.nl: a change {st} (refused), a plain read {st_read}, the SSH keys' list {st_keys} "
                    f"(refused); Host {n}.local: the keys' list {st_own}")
    by_radio(run, "cover", "web-own-name", ["own-names-dhcp-domain"], own_name)


# ---- TCI's own switches: listen only, off (put back as found, the file too), its Bluetooth service ----

def tci_greets(h, port=50001, secs=2.0):
    try:
        c = Remote(h, port, timeout=5)
    except (ConnectionError, OSError):
        return None
    try:
        return texts(c, secs)
    finally:
        c.close()


def t_tci_switches(run):
    def fn(n, h, ip):
        st, body = http(ip, "/api/tci")
        orig = json.loads(body) if st == 200 else None
        if not orig or not orig.get("enabled"):
            return None, f"TCI not on as found ({orig}): left alone"
        k0 = keep(h, "/mnt/tci.conf", "save")
        res = {}
        try:
            conf = {"enabled": True, "port": 50001, "trust": "view", "tx_confirm": bool(orig.get("tx_confirm"))}
            res["view"] = http(ip, "/api/tci", method="POST", body=conf)[0]
            time.sleep(2.5)
            c = Remote(h, 50001, timeout=5)
            try:
                g = texts(c, 1.5)
                f0 = next((x for x in g if x.startswith("vfo:0,0,")), "")
                c.send("vfo:0,0,7100000;")                     # only a tune: no transmit request is ever sent
                ans = [x for x in texts(c, 2.0) if x.startswith("vfo:0,0,")]
            finally:
                c.close()
            res["tci_tune"] = (f0, ans[-1] if ans else None)
            rig_set = rig_ask(h, text="F 7100000\n")
            rig_get = rig_ask(h, text="f\n")
            res["rig"] = (rig_set, rig_get)
            conf["trust"], conf["enabled"] = orig.get("trust", "operate"), False
            res["off"] = http(ip, "/api/tci", method="POST", body=conf)[0]
            time.sleep(2.5)
            try:
                rig_ask(h, text="f\n", secs=1.0)
                res["rig_off"] = "answered"
            except OSError as e:
                res["rig_off"] = f"refused ({type(e).__name__})"
            res["tci_off"] = "refused" if tci_greets(h) is None else "answered"
        finally:
            back = {"enabled": True, "port": orig.get("port", 50001), "trust": orig.get("trust", "operate"),
                    "tx_confirm": bool(orig.get("tx_confirm"))}
            http(ip, "/api/tci", method="POST", body=back)
            k1 = keep(h, "/mnt/tci.conf", "back")
            time.sleep(2.5)
        greet = tci_greets(h)
        again = rig_ask(h, text="f\n")
        f0, f1 = res["tci_tune"]
        held = bool(f0) and f1 == f0
        rig_refused = res["rig"][0].startswith("RPRT -") and res["rig"][1].strip().splitlines()[0] == f0.split(",")[2].rstrip(";")
        ok = res["view"] == 200 and held and rig_refused and res["off"] == 200 and res["rig_off"].startswith("refused") \
            and res["tci_off"] == "refused" and bool(greet) and bool(again) and k1 in ("same", "removed")
        return ok, (f"listen only: TCI's tune answered {f1!r} (was {f0!r}), rigctl's F {res['rig'][0]!r}, f "
                    f"{res['rig'][1].splitlines()[0] if res['rig'][1] else None!r}; TCI off: rigctl {res['rig_off']}, TCI "
                    f"{res['tci_off']}; put back: TCI greets {bool(greet)}, rigctl answers {again[:12]!r}; the file "
                    f"{k0}, then {k1}")
    by_radio(run, "cover", "tci-switches", ["tci-listen-only", "rigctl-follows-tci-switch", "settings-general-tci-clients"], fn)

    def bt(n, h, ip):
        out = ssh(h, "sdptool browse local 2>&1 | grep -i -B3 -A8 'tci' | grep -i -m2 'channel\\|tci'; "
                     "grep -m1 -i 'tci: service registered' /tmp/bt_pair.log 2>/dev/null").strip()
        ok = "22" in out and "registered" in out.lower()
        return ok, f"{out[:220]!r}"
    by_radio(run, "cover", "bt-tci-service", ["bt-tci-service"], bt)


# ---- the apps: FT8's modes and knob, the channels from the web, SmartCW's page, Pat's mailbox ----

# each mode's frequency on two bands the app opens on (it opens on its own last band, not the dial's)
FT8_FREQS = {"80m": {"FT8": 3573000, "FT4": 3575000, "FT2": 3568000},
             "20m": {"FT8": 14074000, "FT4": 14080000, "FT2": 14084000}}


def ft8_band(f):
    return "80m" if 3500000 <= f <= 4000000 else "20m" if 14000000 <= f <= 14350000 else None


def t_ft8_more(run):
    from suite_proto import FT8_APPS

    def fn(r):
        bench.ensure(r, {"freq": F_TEST, "mode": 2})
        said, ok = [], True
        r.send("ft8 open")
        try:
            r.wait("FT8 open", lambda: r.state.get("app") in FT8_APPS and r.ft8.get("open") == 1, 10)
            p0, tx0 = r.ft8.get("proto"), r.ft8.get("txf")
            seen = [(r.state.get("app"), int(r.state.get("freq", 0)))]
            try:
                for _ in range(3):
                    p, f = r.ft8.get("proto"), int(r.state.get("freq", 0))
                    r.send("ft8 mode")
                    r.wait("FT8's mode stepped", lambda: r.ft8.get("proto") != p, 5)
                    r.wait("its frequency", lambda: int(r.state.get("freq", 0)) != f, 4)
                    time.sleep(0.5)
                    seen.append((r.state.get("app"), int(r.state.get("freq", 0))))
                table = FT8_FREQS.get(ft8_band(seen[0][1]))
                right = all(f == table.get(a) for a, f in seen) if table else len({f for _, f in seen[:3]}) == 3
                ok &= right and seen[-1] == seen[0] and len({a for a, _ in seen}) == 3
                said.append("MODE: " + " -> ".join(f"{a} {f / 1e6:.3f}" for a, f in seen))
                t1 = r.ft8.get("txf")
                r.send("knob main 1")
                r.wait("TX Hz +1", lambda: r.ft8.get("txf") != t1, 3)
                one = r.ft8.get("txf") - t1
                t2 = r.ft8.get("txf")
                r.send("knob main 10")
                r.wait("TX Hz on", lambda: r.ft8.get("txf") != t2, 3)
                time.sleep(0.5)
                ten = r.ft8.get("txf") - t2
                ok &= one == 1 and 10 <= ten <= 100
                said.append(f"the main knob: 1 click TX Hz {one:+d}, a burst of 10 {ten:+d}")
            finally:
                # the protocol is a setting the app keeps (ft8_protocol): stepped back to the one found whatever
                # happened above (2026-11-23: a run stopped after one step left both radios on another)
                n = 0
                while r.ft8.get("proto") != p0 and n < 4:
                    p = r.ft8.get("proto")
                    r.send("ft8 mode")
                    try:
                        r.wait("FT8's mode back", lambda: r.ft8.get("proto") != p, 5)
                    except bench.Refused:
                        pass
                    n += 1
                if tx0 is not None and r.ft8.get("txf") != tx0:
                    r.send(f"ft8 txfreq {tx0}")
                    r.wait("FT8's TX Hz back", lambda: r.ft8.get("txf") == tx0, 5)
                ok &= r.ft8.get("proto") == p0
                said.append(f"put back: {r.state.get('app')} (as found {r.ft8.get('proto') == p0}), TX Hz {r.ft8.get('txf')}")
        finally:
            r.send("ft8 close")
            r.wait("FT8 closed", lambda: r.state.get("app") not in FT8_APPS, 10)
        return ok, "; ".join(said)
    per_radio(run, "cover", "ft8-modes-knob", fn, ["ft8-mode-cycle", "ft8-knob-tx-offset"])


def t_channels_web(run):
    def fn(n, h, ip):
        k0 = keep(h, "/mnt/channels.json", "save")
        said, ok = [], True
        try:
            st, body = http(ip, "/api/channels")
            before = json.loads(body).get("channels") if st == 200 else None
            base = {"mode": "NFM", "agc": "FAST", "pre": False, "att": False}
            got = {}
            for name, f in (("BENCH 145", 145500000), ("BENCH 60", 60000000)):
                stc, bodyc = http(ip, "/api/channels", method="POST", body=dict(base, name=name, frequency=f))
                got[name] = (stc, bodyc[:90])
            st2, body2 = http(ip, "/api/channels")
            now = json.loads(body2).get("channels") if st2 == 200 else []
            mine = [i for i, c in enumerate(now) if str(c.get("name", "")).startswith("BENCH ")]
            for i in sorted(mine, reverse=True):
                http(ip, f"/api/channels/{i}", method="DELETE", body={"was": now[i]})
            st3, body3 = http(ip, "/api/channels")
            after = json.loads(body3).get("channels") if st3 == 200 else None
            want60 = 200 if n == "x6200" else 400
            ok = got["BENCH 145"][0] == 200 and got["BENCH 60"][0] == want60 and after == before
            said.append(f"{len(before or [])} channels; 145.5 MHz added {got['BENCH 145'][0]}; 60 MHz "
                        f"{got['BENCH 60'][0]} (want {want60}: {got['BENCH 60'][1] if got['BENCH 60'][0] != 200 else 'taken'}); "
                        f"taken out again, the list as before {after == before}")
        finally:
            k1 = keep(h, "/mnt/channels.json", "back")
        ok &= k1 in ("same", "removed", "restored")     # the API writes the file in its own layout: the bytes put back
        return ok, "; ".join(said) + f"; the file {k0}, then {k1}"
    by_radio(run, "cover", "channels-web", ["channels-frequency-ranges", "channels-web-sync"], fn)


def t_smartcw_guard(run):
    def fn(n, h, ip):
        import urllib.error
        import urllib.request
        out = {}
        for method, origin in (("GET", None), ("POST", "http://example.invalid")):
            hd = {"Host": f"{n}.local:8073"}
            if origin:
                hd["Origin"] = origin
            req = urllib.request.Request(f"http://{ip}:8073/", data=b"{}" if method == "POST" else None,
                                         headers=hd, method=method)
            try:
                with urllib.request.urlopen(req, timeout=6) as resp:
                    out[method] = (resp.status, resp.read(400).decode(errors="replace"))
            except urllib.error.HTTPError as e:
                out[method] = (e.code, e.read(400).decode(errors="replace"))
            except OSError as e:
                out[method] = (None, str(e)[:60])
        stick = ssh(h, "ls /dev/serial/by-id/ 2>/dev/null | grep -ci smartcw").strip()
        page_says = "not plugged in" in out["GET"][1].lower()
        refused = out["POST"][0] in (403, 400)
        ok = (stick == "0" and page_says and refused)
        return ok, (f"SmartCW sticks {stick}; GET :8073 {out['GET'][0]} says 'not plugged in' {page_says}; a POST from "
                    f"another site {out['POST'][0]} {out['POST'][1][:80]!r}")
    by_radio(run, "cover", "smartcw-guard", ["smartcw-page-guard-errors"], fn)


def t_winlink_pat(run):
    def fn(n, h, ip):
        on = ssh(h, "cat /mnt/winlink.conf 2>/dev/null | grep -c '^enabled=1'").strip()
        if on != "1":
            return None, "Winlink is off on this radio (/mnt/winlink.conf): Pat's mailbox not running, as it should be"
        st, _ = http(ip.split(":")[0] + ":8080", "/", host=f"{n}.local:8080")
        cfg = ssh(h, "cat /mnt/winlink/config.json /root/.config/pat/config.json 2>/dev/null | head -200")
        rig = '"address": "127.0.0.1:4532"' in cfg or "127.0.0.1:4532" in cfg
        listen = '"listen": []' in cfg or '"listen":[]' in cfg
        return st == 200 and rig and listen, f"Pat's mailbox {st}; rig at 127.0.0.1:4532 {rig}; no P2P listen {listen}"
    by_radio(run, "cover", "winlink-pat", ["winlink-mailbox-pat"], fn)


TESTS = (("filters", t_filters), ("walkie-upright", t_walkie_upright), ("refusals", t_refusals),
         ("rigctl-local", t_rigctl_local), ("sessions-mtime", t_sessions_mtime), ("config-files", t_config_files),
         ("bootvideo", t_bootvideo), ("gui-cpu", t_gui_cpu), ("tci-rate", t_tci_rate), ("web-writes", t_web_writes), ("tci-switches", t_tci_switches),
         ("ft8-more", t_ft8_more), ("channels-web", t_channels_web), ("smartcw-guard", t_smartcw_guard),
         ("winlink-pat", t_winlink_pat))


def cover(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE cover: {name} (receive only, nothing sent)")
            fn(run)
