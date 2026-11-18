#!/usr/bin/env python3
"""Host check of tools/voacap's voacapl (hfcast-engine behind Pat's voacapl interface):

- Pat's nine listings from the Fortran voacapl (testdata/LA5NTA_to_*.out): their decks, echoed at their tops, run
  through voacapl; at the hour each deck asks, the MUF column (the hour, MUF, FOT; MODE, TANGLE, DELAY, V HITE,
  MUFday) as the Fortran printed it. That column is the path's own; the operating frequency's depends on the
  antennas, which those decks name (samples/sample.23) and voacapl takes as the isotrope, as Pat's decks are now.
- Pat's sample deck (testdata/input.dat): the listing echoes Pat's deck (its parser reads the hour and frequency
  back there), has the hour's block, and the power capped to X6100_VOACAP_W (7 W unless set); -v answers.

    voacap_test.py BINARY

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "testdata")
ROWS = ("MODE", "TANGLE", "DELAY", "V HITE", "MUFday")


def echoed_deck(text):
    out, on = [], False
    for line in text.splitlines():
        if line.strip().startswith("123456789"):
            on = True
            continue
        if on:
            if "Coefficients" in line:
                break
            if line.strip():
                out.append(line[1:])
    return "\n".join(out) + "\n"


def muf_column(text, hour):
    """The hour's block: (hour, MUF, FOT) of its FREQ line and each row's first value"""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"\s+(\d+\.\d)\s.*FREQ\s*$", line)
        if m and int(float(m.group(1))) % 24 == hour % 24:
            col = {"FREQ": tuple(line.split()[:3])}
            for row in lines[i + 1:i + 23]:
                for name in ROWS:
                    if row.rstrip().endswith(name):
                        col[name] = row.split()[0]
            return col
    return None


def run(binary, deck, env=None):
    d = tempfile.mkdtemp(prefix="voacapl-test.")
    try:
        with open(os.path.join(d, "input.dat"), "w") as f:
            f.write(deck)
        r = subprocess.run([binary, "--run-dir=" + d, "/no/itshfbc", "input.dat", "output.out"], capture_output=True,
                           text=True, timeout=120, env=dict(os.environ, **(env or {})))
        if r.returncode:
            return None, r.stderr.strip()
        with open(os.path.join(d, "output.out")) as f:
            return f.read(), ""
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    binary = os.path.abspath(sys.argv[1])
    results = []

    def check(ok, what):
        results.append(ok)
        print(("ok    " if ok else "WRONG ") + what, flush=True)

    v = subprocess.run([binary, "-v"], capture_output=True, text=True).stdout.strip()
    check(v.startswith("voacapl (GoldCorpHF") and "hfcast-engine" in v, "-v: " + v)
    for name in sorted(f for f in os.listdir(DATA) if f.startswith("LA5NTA") and f.endswith(".out")):
        with open(os.path.join(DATA, name)) as f:
            ref = f.read()
        deck = echoed_deck(ref)
        hour = int(re.search(r"TIME\s+(\d+)", deck).group(1))
        mine, err = run(binary, deck, {"X6100_VOACAP_W": "100000"})
        if mine is None:
            check(False, "%s: %s" % (name, err))
            continue
        a, b = muf_column(ref, hour), muf_column(mine, hour)
        check(a is not None and a == b, "%s hour %d: the MUF column as the Fortran's%s" %
              (name, hour, "" if a == b else " (Fortran %s, voacapl %s)" % (a, b)))
    with open(os.path.join(DATA, "input.dat")) as f:
        deck = f.read()
    out, err = run(binary, deck)
    check(out is not None, "Pat's sample deck runs" + (": " + err if err else ""))
    if out:
        check(echoed_deck(out).split() == deck.split(), "Pat's deck echoed as it was written")
        check(muf_column(out, 20) is not None, "the block of the hour Pat asked (20)")
        check("0.007kW" in out, "the power capped to 7 W (Pat asked 100 W)")
    nov = re.sub(r"(?m)^MONTH .*$", "MONTH      202611.00", deck)     # from October on the fields run together
    out2, err = run(binary, nov)
    check(out2 is not None and "  Nov    2026" in out2, "November's MONTH card (202611.00)" + (": " + err if err else ""))
    out, _ = run(binary, deck, {"X6100_VOACAP_W": "5"})
    check(out is not None and "0.005kW" in out, "X6100_VOACAP_W=5: 5 W")
    bad = results.count(False)
    print("voacapl: %d of %d cases as they should be" % (len(results) - bad, len(results)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
