#!/usr/bin/env python3
"""The two-radio suite's screenshots made fit to publish: what would place the station (its position, its locator,
the networks and stations around it) pixelated, or flattened for a map, in place on both radios alike, then each
screen's two pictures side by side (the X6100 left) into OUT, palette-reduced, for a gallery page
(docs/parity/<date>/README.md). The boxes are the radios' 800x480 landscape pictures' and were checked by eye against
the 2026-11-18 run; look at the output before publishing a run of a newer GUI, whose screens may have moved.

    redact_shots.py RUN_DIR OUT_DIR       RUN_DIR: a suite run (~/.config/x6x00-bench/runs/<time>-core)

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFilter

LOCATOR = (686, 82, 800, 116)                    # the classic header's grid locator, right of NR NB DNF
CLASSIC_HEADER_SKIP = {"app118_Field_Ops", "app124_Channels"}
AIR = [(376, 24, 566, 46),                       # the title's GPS position
       ("flat", 6, 46, 364, 404),                # the scope: aircraft drawn around the station
       (612, 92, 694, 214),                      # the list's NM and BRG columns
       (362, 316, 797, 344)]                     # the target's distance, bearing and position
BOXES = {
    "app103_GPS": [(368, 243, 800, 362)],        # latitude, longitude, QTH grid
    "app104_Settings": [(648, 266, 800, 296)],   # QTH locator
    "app108_WiFi": [(303, 123, 797, 422)],       # the networks seen (names)
    "app126_Propagation": [("flat", 10, 168, 422, 394)],   # the map and its markers
    "app130_Airspace": AIR, "sentry_H": AIR,
    "app200_Weather": [(113, 126, 470, 152), (18, 296, 205, 322)],   # the place's name; sunrise and sunset
    "app203_Flights": [("flat", 18, 172, 238, 385), (656, 188, 794, 338)],   # the map; the distances and directions
    "sentry_B": [(8, 238, 270, 264), (228, 96, 362, 120)],           # the FM station's RDS name and call letters
}
SKIP = {"app114_menu", "app128_RADE"}           # the X6200's menu shot caught the Sentry; RADE did not open

def hide(im, box):
    if box[0] == "flat":                         # a map: its markers' places would still show blurred
        x0, y0, x1, y1 = box[1:]
        r = im.crop((x0, y0, x1, y1))
        im.paste(r.resize((1, 1), Image.BOX).resize(r.size), (x0, y0))
        return
    x0, y0, x1, y1 = box
    r = im.crop(box)
    w, h = r.size
    r = r.resize((max(1, w // 10), max(1, h // 10)), Image.BILINEAR).resize((w, h), Image.NEAREST)
    r = r.filter(ImageFilter.GaussianBlur(4))
    im.paste(r, (x0, y0))

def warm_text(im, box):
    px = im.crop(box).convert("RGB").getdata()
    return sum(1 for r, g, b in px if r > 120 and g > 70) > 20


def main():
    run, out = sys.argv[1], sys.argv[2]
    src = os.path.join(run, "shots")
    os.makedirs(out, exist_ok=True)
    bases = sorted({m.group(1) for f in os.listdir(src) for m in [re.match(r"(.+)_x6[12]00\.png$", f)] if m})
    n = 0
    for base in bases:
        if base in SKIP:
            continue
        ims = []
        for radio in ("x6100", "x6200"):
            im = Image.open(os.path.join(src, f"{base}_{radio}.png")).convert("RGB")
            for b in BOXES.get(base, []):
                hide(im, b)
            if base.startswith("app") and base not in CLASSIC_HEADER_SKIP and warm_text(im, LOCATOR):
                hide(im, LOCATOR)
            ims.append((radio, im))
        pair = Image.new("RGB", (1608, 502), (40, 40, 40))
        d = ImageDraw.Draw(pair)
        for i, (radio, im) in enumerate(ims):
            pair.paste(im, (808 * i, 22))
            d.text((808 * i + 6, 5), radio.upper(), fill=(255, 255, 255))
        pair.quantize(colors=96, method=Image.Quantize.MEDIANCUT).save(os.path.join(out, f"{base}.png"), optimize=True)
        n += 1
    print(f"{n} pairs in {out}: look at each before publishing")


if __name__ == "__main__":
    main()
