#!/usr/bin/env python3
"""A host check's link that a new call in the GUI's sources does not break (the owner, 2026-11-20: "I'm soooo sick of
host checks ALWAYS failing... it needs fixed indefinitely").

A host check that links a real GUI file (tools/radio_wr12: src/radio.c) stands in for the rest of the GUI by hand
(fakes.c). Each time that file came to use a setting or a function the stand-ins lacked, the link failed on CI and the
whole run with it: four of six failed runs 2026-11-18..20 were that and nothing else (dsp_zoom_refresh, then
cfg_band_tx_iq_gain, cfg_tx_cal_radio, ws_sha256...). This runs the link; when the linker names what it lacks, it
makes a stand-in for each and links again:
  - a setting (cfg_api.h: extern ParamInt *cfg_x;): a line X(ParamInt, cfg_x) in --settings FILE, which the check's
    fakes include beside their own list, so the new setting is one like the rest (0, observers, set and get)
  - a function: one that answers 0 (its integer and floating-point return registers) and says once, on stderr, that
    it was called: a case that needs what it really does still fails, by its own check, and names it
  - anything else (a table): zeroed memory
The names go into FILE.c (--stubs), which the command must list; it is written empty first. What was made is said in
one line, so the stand-ins to write by hand are known; a link that still fails shows the linker's own words.

    autolink.py --src SRC --stubs build/auto_stubs.c [--settings build/auto_settings.h] -- CC ARGS...

GNU ld's "undefined reference to `x'" and a Mac's "_x", referenced from:" are read.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, os, re, subprocess, sys

LINUX = re.compile(r"undefined reference to [`']([A-Za-z_]\w*)'")
MAC = re.compile(r'^\s+"_([A-Za-z_]\w*)", referenced from:', re.M)

HEAD = """/* Made by tools/host_link/autolink.py at each link: stand-ins for what the GUI's sources call and this check's
 * fakes do not have yet. Answer 0, say so once. Not to be edited: write a real stand-in in the fakes instead. */
#include <stdio.h>

__attribute__((unused)) static void autolink_said(const char *name, int *said) {
    if (!*said) {
        *said = 1;
        fprintf(stderr, "autolink: %s called: no stand-in for it in this check's fakes, it answered 0\\n", name);
    }
}

#if defined(__x86_64__)
#define AUTOLINK_ZERO_FP() __asm__ volatile("xorps %%xmm0, %%xmm0" ::: "xmm0")
#elif defined(__aarch64__)
#define AUTOLINK_ZERO_FP() __asm__ volatile("movi d0, #0" ::: "v0")
#else
#define AUTOLINK_ZERO_FP() ((void)0)
#endif
"""


def headers(src):
    text = []
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for f in files:
            if f.endswith(".h"):
                try:
                    with open(os.path.join(root, f), errors="replace") as fh:
                        text.append(fh.read())
                except OSError:
                    pass
    return "\n".join(text)


def kind(name, h):
    """('setting', type) | ('data', None) | ('function', None), from the GUI's headers"""
    m = re.search(r"extern\s+((?:Param|Subject)\w*)\s*\*\s*" + re.escape(name) + r"\s*;", h)
    if m:
        return "setting", m.group(1)
    if re.search(r"\b" + re.escape(name) + r"\s*\(", h):
        return "function", None
    if re.search(r"\bextern\b[^;(]*\b" + re.escape(name) + r"\b", h):
        return "data", None
    return "function", None


def write(stubs, settings, made):
    with open(stubs, "w") as f:
        f.write(HEAD)
        for name, (k, _) in sorted(made.items()):
            if k == "function":
                f.write(f"\nlong {name}(void);\nlong {name}(void) {{\n    static int said;\n\n"
                        f"    autolink_said(\"{name}\", &said);\n    AUTOLINK_ZERO_FP();\n    return 0;\n}}\n")
            elif k == "setting" and not settings:
                # a pointer to a zeroed object (these fakes have no subjects of their own)
                f.write(f"\n__attribute__((aligned(64))) static char {name}_obj[4096];\nvoid *{name} = {name}_obj;\n")
            elif k == "data":
                f.write(f"\n__attribute__((aligned(64))) char {name}[65536];\n")
    if settings:
        with open(settings, "w") as f:
            f.write("/* Made by tools/host_link/autolink.py: settings the GUI's sources use that the fakes' own list "
                    "lacks */\n#define AUTO_SETTINGS")
            for name, (k, t) in sorted(made.items()):
                if k == "setting":
                    f.write(f" \\\n    X({t}, {name})")
            f.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--stubs", required=True)
    ap.add_argument("--settings")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    os.makedirs(os.path.dirname(os.path.abspath(a.stubs)), exist_ok=True)
    made, h = {}, None
    write(a.stubs, a.settings, made)
    for attempt in range(4):
        p = subprocess.run(cmd, capture_output=True, text=True)
        out = p.stdout + p.stderr
        if p.returncode == 0:
            sys.stdout.write(p.stdout)
            if made:
                print(f"autolink: stand-ins made for {len(made)} name(s) this check's fakes lack: "
                      + ", ".join(f"{n} ({k})" for n, (k, _) in sorted(made.items())))
            return 0
        lacking = sorted(set(LINUX.findall(out)) | set(MAC.findall(out)))
        new = [n for n in lacking if n not in made]
        if not new:
            sys.stdout.write(out)
            return p.returncode
        h = h if h is not None else headers(a.src)
        for n in new:
            made[n] = kind(n, h)
        write(a.stubs, a.settings, made)
    sys.stdout.write(out)
    return 1


if __name__ == "__main__":
    sys.exit(main())
