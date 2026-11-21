#!/usr/bin/env python3
"""A radio's TX calibration as a file tied to that radio (the owner, 2026-11-20: "store the config tied to something
like unique processor ID or serial number"). The calibration (tools/bench/txcal.py) lives in the radio's band settings:
each band's TX I and Q DC offsets (txio, txqo) and I/Q balance (txig, txip). This saves them, with the radio's
fingerprint (x6x00-fingerprint: its processor's chip ID, hashed; no raw ID), to calibrations/<fingerprint>.json, and
puts a saved file back on the same radio, never another (unless --force: a radio whose processor board was changed).
The GUI does the same on its side: a calibration it finds made on another radio is cleared at its start.

    txcal_store.py save RADIO [FILE]     every band's four values read through the radio's remote (it tunes each band
                                         and goes back), into FILE or calibrations/<fingerprint>.json
    txcal_store.py apply RADIO FILE      the file's values set on its bands, the fingerprint checked first
    txcal_store.py show FILE             what a file holds

The remote's harness session (bench.py provision RADIO tx) is used; nothing is transmitted.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
from bench import Radio, Refused, say  # noqa: E402
from image import BANDS  # noqa: E402

FORMAT = "goldcorphf-txcal 1"
KEYS = {"txio": "tx_i_offset", "txqo": "tx_q_offset", "txig": "tx_iq_gain", "txip": "tx_iq_phase"}
DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "calibrations")


def fingerprint(r, host):
    """The radio's fingerprint: its GUI's state (fp), else x6x00-fingerprint over SSH (with its storage witness)"""
    info = {}
    try:
        out = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", f"root@{host}", "x6x00-fingerprint"],
                             capture_output=True, text=True, timeout=20).stdout.strip()
        info = json.loads(out) if out.startswith("{") else {}
    except (subprocess.SubprocessError, ValueError):
        pass
    fp = r.state.get("fp") or info.get("fingerprint")
    if not fp:
        raise Refused("the radio's fingerprint cannot be read (x6x00-fingerprint, or a GUI that says fp)")
    if info.get("fingerprint") and r.state.get("fp") and info["fingerprint"] != r.state["fp"]:
        raise Refused(f"the GUI says {r.state['fp']}, x6x00-fingerprint {info['fingerprint']}: not the same radio?")
    return fp, info.get("storage")


def tune(r, f):
    if int(r.state.get("freq", 0)) != f:
        r.send(f"set freq {f}")
        r.wait(f"{f} Hz", lambda: int(r.state.get("freq", 0)) == f, 8)
    time.sleep(1.0)                 # the band's settings in the state after the band change


def save(a, c):
    r = Radio(a.radio, c["radios"][a.radio], True)
    r.take_control()
    was = int(r.state.get("freq", 0))
    try:
        fp, storage = fingerprint(r, r.host)
        doc = {"format": FORMAT, "fingerprint": fp, "model": fp.split("-")[0], "storage": storage,
               "saved": time.strftime("%Y-%m-%d %H:%M %Z"),
               "units": {"tx_i_offset": "register 50 (x 5000 in the base)", "tx_q_offset": "register 51",
                         "tx_iq_gain": "1e-5 (register 52, low half)", "tx_iq_phase": "1e-5 (register 52, high half)"},
               "bands": {}}
        if a.note:
            doc["note"] = a.note
        for band, f in BANDS.items():
            tune(r, f)
            doc["bands"][band] = {"freq": f, **{name: int(r.state.get(k, 0)) for k, name in KEYS.items()}}
            say(f"{band:>3} m: {doc['bands'][band]}")
    finally:
        if was:
            r.send(f"set freq {was}")
            time.sleep(0.5)
        r.stop(); r.close()
    path = a.file or os.path.join(DIR, f"{fp}.json")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if os.path.exists(path) and not a.file:
        old = json.load(open(path))
        for k in ("note", "results"):       # what was written about it before stays
            if k in old and k not in doc:
                doc[k] = old[k]
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    say("saved", path)


def apply(a, c):
    doc = json.load(open(a.file))
    if doc.get("format") != FORMAT:
        raise Refused(f"{a.file}: not a {FORMAT} file")
    r = Radio(a.radio, c["radios"][a.radio], True)
    r.take_control()
    was = int(r.state.get("freq", 0))
    try:
        fp, _ = fingerprint(r, r.host)
        if fp != doc["fingerprint"]:
            if not a.force:
                raise Refused(f"this radio is {fp}, the file is for {doc['fingerprint']}: another radio's calibration "
                              f"would make its image and carrier worse, not better (--force for a radio whose processor "
                              f"board was changed)")
            say(f"--force: {doc['fingerprint']}'s calibration on {fp}")
        for band, v in doc["bands"].items():
            tune(r, int(v["freq"]))
            for k, name in KEYS.items():
                if name not in v:           # a file from before the warm balance: that pair left as it is
                    continue
                r.send(f"set {k} {int(v[name])}")
                r.wait(f"{k} {v[name]}", lambda k=k, x=int(v[name]): int(r.state.get(k, -99999)) == x, 6)
            say(f"{band:>3} m: set {v}")
    finally:
        if was:
            r.send(f"set freq {was}")
            time.sleep(0.5)
        r.stop(); r.close()


def show(a, c):
    doc = json.load(open(a.file))
    print(f"{doc['fingerprint']} ({doc.get('model')}), saved {doc.get('saved')}")
    if doc.get("note"):
        print(doc["note"])
    for band, v in doc["bands"].items():
        print(f"  {band:>3} m: " + ", ".join(f"{n} {v[n]}" for n in KEYS.values() if n in v))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("save"); s.add_argument("radio"); s.add_argument("file", nargs="?"); s.add_argument("--note")
    s = sub.add_parser("apply"); s.add_argument("radio"); s.add_argument("file"); s.add_argument("--force", action="store_true")
    s = sub.add_parser("show"); s.add_argument("file")
    a = ap.parse_args()
    try:
        {"save": save, "apply": apply, "show": show}[a.cmd](a, None if a.cmd == "show" else bench.conf())
    except Refused as e:
        say(f"REFUSED: {e}")
        sys.exit(2)


if __name__ == "__main__":
    main()
