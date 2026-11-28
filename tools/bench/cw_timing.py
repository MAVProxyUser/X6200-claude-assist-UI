"""CW as it reaches the air, against what was keyed (orch's cw-timing, air_b's cw-copy).

cw_elements(): a CW over's marks and gaps read from received audio (the key tone's envelope, zero-phase, 3 dB of
hysteresis, runs under 8 ms folded in). cw_ideal(): a text's marks and gaps in dot units. keyer_said(): the sender's
keyer's own account of its last send, from its GUI's log (src/cw_encoder.c: each key edge against its deadline).
judge(): the three set against each other, telling the keyer's fault (an edge late on its deadline: the GUI's) from
the base's (the keyer on time, the carrier not: 2026-11-25, the X6100's factory base let the carrier run on 30-80 ms
after the key went up now and then, enough at 0.1 W to make two elements one) and from a clean over.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import re

TIMING_MS = 25              # a mark or gap this far off its kind's median is an edge out of time
GUI_LOG = "/var/log/x6100_log.txt"


def _env_db(x, fs, pitch):
    """The key tone's envelope in dB, a step a millisecond (zero-phase, 150 Hz wide: both edges delayed alike), and the
    marks' level and the gaps' floor"""
    import numpy as np
    from scipy.signal import butter, filtfilt
    t = np.arange(len(x)) / fs
    z = x * np.exp(-2j * np.pi * pitch * t)
    b, a = butter(3, 150 / (fs / 2))
    e = np.abs(filtfilt(b, a, z.real) + 1j * filtfilt(b, a, z.imag))
    db = 20 * np.log10(e[::int(fs / 1000)] + 1e-9)
    return db, np.percentile(db, 95), np.percentile(db, 20)


def _keyed(db, on_db, off_db):
    """Each step on (a mark) from over on_db until under off_db"""
    import numpy as np
    on, cur = np.zeros(len(db), bool), False
    for i, v in enumerate(db):
        cur = v > off_db if cur else v > on_db
        on[i] = cur
    return on


def cw_elements(x, fs, pitch):
    """A CW over in received audio x: its marks and gaps in ms, in order [(is_mark, ms)], from the key tone's envelope
    (_env_db), on and off halfway in dB between the marks' level and the gaps' floor with 3 dB either way (an edge's
    ripple is not two edges); the first mark from the over's start, the last gap dropped. (runs, depth in dB). Low on
    the edges on a shallow over (13 dB under the marks at 26 dB deep): a slow fade counts into the mark there, not its
    timing (half_level)"""
    db, hi, lo = _env_db(x, fs, pitch)
    mid = (hi + lo) / 2
    return _runs(_keyed(db, mid + 3, mid - 3)), round(float(hi - lo), 1)


def half_level(x, fs, pitch, ideal, unit_ms):
    """The over's marks and gaps at the marks' half amplitude (6 dB under their level, 1.5 dB either way), where CW's
    timing is measured and a decoder ends a mark (src/cw_decoder/key_detector.cpp's off level): judge()'s result there
    (its "medians", "weight_ms"), None when the elements there are not as keyed. cw_elements' threshold, halfway in dB,
    counts a slow fade into the mark: 2026-11-28, the X6100's factory base's dots read 77-78 ms there at 20 WPM, 17-18
    over the dot, and 64-65 at half amplitude, as the X6200's (an 18 ms shorter mark keyed for it went out 48 ms)"""
    db, hi, _ = _env_db(x, fs, pitch)
    j = judge(_runs(_keyed(db, hi - 4.5, hi - 7.5)), ideal, unit_ms)
    return j if j["as_keyed"] else None


def _runs(on):
    """The steps on and off as runs [(is_mark, ms)]: a run under 8 ms into the one before it; from the first mark to the
    last"""
    runs, k = [], 0
    while k < len(on):
        j = k
        while j < len(on) and on[j] == on[k]:
            j += 1
        runs.append([bool(on[k]), j - k])
        k = j
    merged = []                             # a run under 8 ms is no element: into the one before it
    for m, ms in runs:
        if merged and (ms < 8 or merged[-1][0] == m):
            merged[-1][1] += ms
            if len(merged) > 1 and merged[-1][0] == merged[-2][0]:
                merged[-2][1] += merged.pop()[1]
        else:
            merged.append([m, ms])
    runs = [(m, ms) for m, ms in merged]
    while runs and not runs[0][0]:
        runs.pop(0)
    while runs and not runs[-1][0]:
        runs.pop()
    return runs


def cw_ideal(text):
    """The text's marks and gaps in dot units, in order [(is_mark, units)] (TCI's ">" and "<" speed steps left out;
    characters without a code skipped, as the keyer skips them)"""
    from suite_air_b import MORSE
    out = []
    words = [w for w in text.replace(">", "").replace("<", "").upper().split() if w]
    for wi, word in enumerate(words):
        if wi:
            out.append((False, 7))
        codes = [MORSE[c] for c in word if c in MORSE]
        for ci, code in enumerate(codes):
            if ci:
                out.append((False, 3))
            for ei, el in enumerate(code):
                if ei:
                    out.append((False, 1))
                out.append((True, 1 if el == "." else 3))
    return out


def keyer_lines(ssh, host):
    """The sender's uptime now (s): keyer_said's since=, its lines stamped later are the over's. Their own stamp, not a
    count: a GUI restart between starts a new log (2026-11-26: the line count taken before it found nothing after)"""
    try:
        return float((ssh(host, "cut -d' ' -f1 /proc/uptime") or "").strip())
    except Exception:                       # noqa: BLE001
        return None


def keyer_said(ssh, host, last=1, since=None):
    """The sender's keyer on its last `last` sends, from its GUI's log: (edges, late, the latest ms) summed over them,
    None when unsaid (a GUI before the line, 2026-11-25)"""
    try:
        out = ssh(host, f"grep 'cw_encoder: .* key edges' {GUI_LOG}" + ("" if since is not None else f" | tail -{int(last)}"))
        if since is not None:               # the lines stamped (their uptime, '(128906.980, +11767)') since the mark
            out = "\n".join(ln for ln in (out or "").splitlines()
                            if (m := re.search(r"\((\d+\.\d+),", ln)) and float(m.group(1)) >= since)
    except Exception:                       # noqa: BLE001 (unread: unsaid)
        return None
    got = re.findall(r"(\d+) key edges, (\d+) more than \d+ ms late on their time, the latest (-?\d+) ms", out or "")
    if not got:
        return None
    return (sum(int(a) for a, _, _ in got), sum(int(b) for _, b, _ in got), max(int(c) for _, _, c in got))


PAIR_MS = 6     # two neighbouring errors adding to within this are one edge moved (judge's "shifted")


def judge(runs, ideal, unit_ms):
    """runs (cw_elements) against ideal (cw_ideal): {"as_keyed": every element there, none merged or split; "medians":
    each kind's median ms; "worst_ms"; "out": the errors over TIMING_MS; "marks", "want"; "longest"}"""
    import numpy as np
    res = {"marks": sum(1 for m, _ in runs if m), "want": sum(1 for m, _ in ideal if m),
           "longest": max((ms for m, ms in runs if m), default=0), "unit_ms": unit_ms}
    res["as_keyed"] = len(runs) == len(ideal) and all(m == mi for (m, _), (mi, _) in zip(runs, ideal))
    if not res["as_keyed"]:
        return res
    kinds = {}
    for (m, ms), k in zip(runs, ideal):
        kinds.setdefault(k, []).append(ms)
    med = {k: float(np.median(v)) for k, v in kinds.items()}
    errs = [ms - med[k] for (m, ms), k in zip(runs, ideal)]
    names = {(True, 1): "dot", (True, 3): "dash", (False, 1): "gap", (False, 3): "letter gap", (False, 7): "word gap"}
    res.update(medians={names.get(k, str(k)): round(v) for k, v in med.items()},
               worst_ms=round(max(abs(e) for e in errs)), out=[round(e) for e in errs if abs(e) > TIMING_MS])
    # An edge that moved: an element out of time whose next one is out the other way by the same (their errors adding
    # to within PAIR_MS) and by at most a dot, is one shifted edge and no time gained or lost (a gap long and the mark
    # after it short: a late key-down; a mark long and the gap after it short: a late key-up). Any other outlier is
    # unpaired: time gained or lost. weight_ms: the dots' median over the dot's length
    shifted, used = [], set()
    for i, e in enumerate(errs[:-1]):
        if abs(e) <= TIMING_MS or i in used:
            continue
        f = errs[i + 1]
        if abs(e + f) <= PAIR_MS and abs(e) <= unit_ms:
            # the edge between element i and the next: after a gap a key-down, after a mark a key-up; late when
            # element i ran long
            shifted.append({"at": i, "ms": round(abs(e)), "edge": "key-up" if runs[i][0] else "key-down",
                            "late": e > 0})
            used.update((i, i + 1))
    res["shifted"] = shifted
    res["unpaired"] = [round(e) for i, e in enumerate(errs) if abs(e) > TIMING_MS and i not in used]
    res["weight_ms"] = round(med.get((True, 1), unit_ms) - unit_ms) if (True, 1) in med else None
    return res


def words(res):
    """judge()'s result in words"""
    if not res["as_keyed"]:
        return (f"{res['marks']} marks where {res['want']} were keyed: "
                f"{'merged' if res['marks'] < res['want'] else 'split'} (the longest mark {res['longest']} ms, a dash "
                f"{3 * res['unit_ms']:.0f})")
    med = ", ".join(f"{k} {v}" for k, v in res["medians"].items())
    return (f"every element as keyed; the medians (ms): {med}; the worst {res['worst_ms']} ms off its kind's median, "
            f"{len(res['out'])} over {TIMING_MS}" + (f" ({res['out']})" if res["out"] else ""))


def clean(res):
    """The over reached the air as keyed, every edge in time"""
    return res["as_keyed"] and not res["out"]
