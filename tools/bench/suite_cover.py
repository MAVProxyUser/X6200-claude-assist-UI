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
        c, _ = fresh(r.host, secs=0.3)
        try:
            c.send("screen 1")
            time.sleep(3.0)
            fr = [f for f in list(c.frames) if f[2][8:10] == b"\xff\xd8"]
        finally:
            c.close()
        sizes = [jpeg_size(f[2][8:]) for f in fr[-3:]]
        ok = bool(sizes) and all(s and s[1] > s[0] for s in sizes) and r.state.get("pg") == "G"
        return ok, f"page {r.state.get('pg')}: {len(fr)} pictures, the last {sizes} (width, height): portrait {ok}"
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


TESTS = (("filters", t_filters), ("walkie-upright", t_walkie_upright), ("refusals", t_refusals),
         ("rigctl-local", t_rigctl_local), ("sessions-mtime", t_sessions_mtime), ("config-files", t_config_files),
         ("bootvideo", t_bootvideo), ("gui-cpu", t_gui_cpu), ("tci-rate", t_tci_rate), ("web-writes", t_web_writes))


def cover(run, only):
    for name, fn in TESTS:
        if not only or name in only:
            run.banner(f"SUITE cover: {name} (receive only, nothing sent)")
            fn(run)
