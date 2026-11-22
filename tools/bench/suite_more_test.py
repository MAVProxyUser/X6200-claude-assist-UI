#!/usr/bin/env python3
"""suite_more.py's own logic, no radio: D's and E's rule as the GUI has it (rx_setup_on), the features a web page
serves, and the coverage report's counting (pass, fail, in part, credited to the older suite's tests, not covered).
SPDX-License-Identifier: LGPL-2.1-or-later"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import suite_more  # noqa: E402

results = []


def check(ok, what):
    results.append(ok)
    print(("ok    " if ok else "WRONG ") + what)


r = suite_more.rule_of
check(r(1840000) == {"att": 0, "pre": 0, "agc": 1}, "160 m, a decoder app: PRE off (under 8 MHz), AGC slow")
check(r(14074000) == {"att": 0, "pre": 1, "agc": 1}, "20 m, a decoder app: PRE on (8 MHz and up)")
check(r(7030000, cw_or_prop=True) == {"att": 0, "pre": 0, "agc": 2}, "CW on 40 m: AGC fast, PRE off")
check(r(1000000, listening=True)["pre"] == 0, "AM broadcast: PRE off (strong locals)")
check(r(98000000, listening=True) == {"att": 0, "pre": 0, "agc": 3}, "FM broadcast listened to: PRE off, AGC auto")
check(r(121500000, listening=True)["pre"] == 1, "the airband: PRE on")
check("web-keys-page" in suite_more.features_getting("/keys"), "the /keys page serves web-keys-page")
check(not suite_more.features_getting("/no_such_page"), "no feature for a page nobody gets")


class FakeRun:
    def __init__(self, d):
        self.dir, self.results = d, []


d = tempfile.mkdtemp(prefix="suite-more-test-")
feats = [{"id": "a", "area": "x", "needs": "none", "feature": "A"},
         {"id": "b", "area": "x", "needs": "none", "feature": "B"},
         {"id": "c", "area": "y", "needs": "cable", "feature": "C"},
         {"id": "d", "area": "y", "needs": "none", "feature": "D", "suite_has": "core/screen app101_FT8",
          "coverage": "full"},
         {"id": "e", "area": "y", "needs": "none", "feature": "E"}]
real = suite_more.HERE
try:
    suite_more.HERE = d
    with open(os.path.join(d, "features.json"), "w") as f:
        json.dump(feats, f)
    run = FakeRun(d)
    run.results = [{"batch": "rx", "test": "t1", "ok": True, "covers": ["a"]},
                   {"batch": "rx", "test": "t2", "ok": False, "covers": ["b"]},
                   {"batch": "control", "test": "web /x", "ok": True, "partial": ["c"]},
                   {"batch": "core", "test": "screen app101_FT8", "ok": True}]
    suite_more.coverage(run)
    text = open(os.path.join(d, "coverage.md")).read()
    check("| x | 2 | 1 | 1 | 0 | 0 | 0 |" in text, "area x: one passed, one failed")
    check("| y | 3 | 1 | 0 | 0 | 1 | 1 |" in text, "area y: one credited to the older suite's test, one in part, "
          "one not covered")
    check("`e` (y, needs none): E" in text and "`a`" not in text.split("Not covered yet")[1],
          "the uncovered feature listed, the covered ones not")
finally:
    suite_more.HERE = real
    shutil.rmtree(d, ignore_errors=True)
# suite_proto.py's parsers: the system read, the TCI greeting
import suite_proto  # noqa: E402

kv = suite_proto.sys_parse("now=1795382681\nbat=1795382640,8.6,99\ntrace_has=settings read\ntrace_has=first frame\n"
                           "proc=gpsd\nproc=x6100_gui\nupd={\"pending\": null}\nts=funnel=off x=1\n")
check(kv["now"] == "1795382681" and kv["trace_has"] == ["settings read", "first frame"] and
      kv["proc"] == ["gpsd", "x6100_gui"], "the system read: single keys as values, repeated ones as lists")
check(kv["upd"] == '{"pending": null}' and kv["ts"] == "funnel=off x=1", "a value keeps its own = signs")
g = ["protocol:ExpertSDR3,1.9;", "audio_samplerate:48000;", "drive:0,10;", "ready;"]
check(suite_proto.greeted(g, "audio_samplerate") == "48000" and suite_proto.greeted(g, "drive") == "0,10" and
      suite_proto.greeted(g, "dds") is None, "a TCI greeting's values")
import struct  # noqa: E402
jp = (b"\xff\xd8\xff\xe0" + struct.pack(">H", 16) + b"JFIF\x00" + bytes(9) + b"\xff\xc0" +
      struct.pack(">HBHH", 17, 8, 400, 240) + bytes(12))
check(suite_proto.jpeg_size(jp) == (240, 400) and suite_proto.jpeg_size(b"\xff\xd8\x00") is None,
      "a JPEG's own size from its SOF, past its APP0")
bad = results.count(False)
print(f"suite_more: {len(results) - bad} of {len(results)} as they should be")
sys.exit(1 if bad else 0)
