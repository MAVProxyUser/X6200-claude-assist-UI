#!/usr/bin/env python3
"""The headers the X6200 GUI is built against, from Ubuntu's armhf archive (ports.ubuntu.com): resolves each wanted
-dev package in the suite's Packages index (the -updates pocket first), downloads it into DEBS and prints the files.
Ubuntu 22.04 (jammy) for most: its GLib is the radio's 2.72 and its libjpeg the radio's jpeg8 ABI (libjpeg.so.8);
24.04 (noble) for gpsd (the radio's libgps.so.30), Boost (js8core wants boost/math/ccmath, 1.78 and later) and
libsndfile (the radio's 1.2.0; MP3, SF_FORMAT_MPEG, came in 1.1).
Only headers and pkg-config files are used: the libraries linked are the radio's own (setup.sh).

    fetch_debs.py DEBS_DIR
"""
import lzma, os, sys, urllib.request

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
WANT = {
    "jammy": ["libglib2.0-dev", "libdbus-1-dev", "libdbus-glib-1-dev", "libnm-dev", "libpng-dev", "zlib1g-dev",
              "libudev-dev", "libpulse-dev", "libasound2-dev", "libjpeg-turbo8-dev", "libjpeg8-dev",
              "libfftw3-dev", "libsqlite3-dev", "libxkbcommon-dev", "libdrm-dev"],
    "noble": ["libgps-dev", "libboost1.83-dev", "libsndfile1-dev"],
}


def index(suite):
    """{package: (version, filename)}, the -updates pocket over the release"""
    out = {}
    for pocket in (suite, suite + "-updates"):
        for comp in ("main", "universe"):
            url = f"{MIRROR}/dists/{pocket}/{comp}/binary-armhf/Packages.xz"
            with urllib.request.urlopen(url, timeout=60) as r:
                text = lzma.decompress(r.read()).decode("utf-8", "replace")
            for block in text.split("\n\n"):
                f = dict(l.split(": ", 1) for l in block.splitlines() if ": " in l and not l.startswith(" "))
                if "Package" in f and "Filename" in f:
                    out[f["Package"]] = (f["Version"], f["Filename"])
    return out


def main():
    debs = sys.argv[1]
    os.makedirs(debs, exist_ok=True)
    for suite, pkgs in WANT.items():
        idx = index(suite)
        for p in pkgs:
            if p not in idx:
                sys.exit(f"{p}: not in {suite}")
            ver, fn = idx[p]
            dest = os.path.join(debs, os.path.basename(fn))
            if not os.path.exists(dest):
                urllib.request.urlretrieve(f"{MIRROR}/{fn}", dest + ".part")
                os.replace(dest + ".part", dest)
            print(f"{suite:6s} {p:22s} {ver:28s} {os.path.getsize(dest) // 1024:6d} KB")


if __name__ == "__main__":
    main()
