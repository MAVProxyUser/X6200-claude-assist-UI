#!/usr/bin/env python3
"""Every action Settings > Keys lets the operator give a key (src/key_settings.cpp's lists) has a name on the USER page
(src/button_bar.cpp user_label_of): up to v0.3.12.48 upstream's apps (Weather, Internet radio, Zello, SVXConnect,
Flights, HF Air Band, CB) showed "?" there. Read from the sources: a new action added to the lists without a label
fails here. SPDX-License-Identifier: LGPL-2.1-or-later"""
import os
import re
import sys

TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def main():
    keys = open(os.path.join(TOP, "src", "key_settings.cpp")).read()
    bar = open(os.path.join(TOP, "src", "button_bar.cpp")).read()
    assignable = sorted(set(re.findall(r'\{"[^"]*",\s*(ACTION_\w+)\}', keys)))
    i = bar.index("static const char *user_label_of(int32_t a)")
    j = bar.index("default:", i)
    labelled = set(re.findall(r"case (ACTION_\w+):", bar[i:j]))
    missing = [a for a in assignable if a not in labelled]
    if len(assignable) < 40:
        print(f"FAIL: only {len(assignable)} actions read from key_settings.cpp: its lists' form changed")
        return 1
    if missing:
        print(f"FAIL: {len(missing)} of the {len(assignable)} actions a key can be given have no USER page label "
              f"(button_bar.cpp user_label_of): {', '.join(missing)}")
        return 1
    print(f"user_keys: all {len(assignable)} actions a key can be given have a USER page label")
    return 0


if __name__ == "__main__":
    sys.exit(main())
