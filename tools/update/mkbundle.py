#!/usr/bin/env python3
"""Makes the update bundle the radio installs by itself (rootfs/usr/sbin/x6100-update): the files
the GUI and the web server put in the buildroot target, and the FT8 library the GUI links, and a
manifest with a SHA-256 per file, which the radio checks every file against. No signature: this
fork's radios ask for none, so there is no key to keep (x6100-update).

  python3 tools/update/mkbundle.py --target <buildroot target dir> \\
      --manifest <build/x6100-gui-*/install_manifest.txt> --version 0.2.0 \\
      --out GoldCorpHF-x6200-v0.3.12.20-update.tar.gz [--notes "..."] [--boot-dir <buildroot images dir>]

--boot-dir also carries what the SD card has outside the root filesystem, for the radio to install only when asked
(x6100-update install --kernel / --uboot, the GUI's checkboxes): the kernel set of the card's boot partition (zImage,
sun8i-r16-<model>.dtb, boot.scr, as genimage puts them there) with the kernel's modules (the target's
/lib/modules/<release>/), and U-Boot (u-boot-sunxi-with-spl.bin, raw at the card's byte 8 KiB, its area 1016 KiB).
They go in the manifest's "boot" part, not in "files": nothing installs them unless asked.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import hashlib
import io
import json
import os
import sys
import tarfile
import time

BRIDGE_VERSION = "0.3.12.20"     # the last bundle marked OpenX6X00, the first of GoldCorpHF

WEB_FILES = ["/usr/bin/x6100_webserver"]
WEB_DIRS = ["/usr/lib/python3.11/site-packages/x6100_webserver"]
# The FT8 library the GUI is built with and links (the buildroot tree's ft8lib package, not the GUI's CMake): without
# it a radio updated in place keeps its card image's. The X6200's images up to v0.3.8 have ft8_lib 117022b, which
# sends a CQ with a compound call (CQ PA/ON6URE) as free text and refuses CQ OT4ABCD and a CQ over 13 characters; both
# trees are at 73b0db8 now, the X6100's since before its first image. The two have the same headers and exported
# symbols (only message.c's packing differs), so the GUI runs on either: v0.3.8's on this one when its bundle is
# installed over a newer one (which leaves the library), this one's on 117022b when a rollback puts that back
LIB_FILES = ["/usr/lib/libft8.so"]
SKIP_PREFIXES = ("/usr/include/",)       # the GUI package installs headers too
UBOOT = "u-boot-sunxi-with-spl.bin"
UBOOT_OFFSET = 8 * 1024                  # genimage's u-boot partition: offset 8K, size 1016K
UBOOT_AREA = 1016 * 1024


def sha_size(path):
    with open(path, "rb") as f:
        data = f.read()
    return hashlib.sha256(data).hexdigest(), len(data)


def boot_part(images, target, model):
    """The manifest's "boot" part and the tar members it names ([(member, source)]): the kernel set and U-Boot from
    buildroot's images dir, the modules from the target's /lib/modules/<release>/ (one release there, or none)"""
    members, kfiles, mods = [], [], []
    for name in ("zImage", f"sun8i-r16-{model}.dtb", "boot.scr"):
        src = os.path.join(images, name)
        if not os.path.isfile(src):
            sys.exit(f"missing in {images}: {name}")
        sha, size = sha_size(src)
        kfiles.append({"name": name, "sha256": sha, "size": size})
        members.append(("boot/" + name, src))
    mroot = os.path.join(target, "lib", "modules")
    releases = sorted(d for d in os.listdir(mroot) if os.path.isdir(os.path.join(mroot, d))) \
        if os.path.isdir(mroot) else []
    if len(releases) > 1:
        sys.exit(f"more than one kernel's modules in {mroot}: {', '.join(releases)}")
    release = releases[0] if releases else ""
    if release:
        for root, dirs, files in os.walk(os.path.join(mroot, release)):
            dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(root, d))]   # build/, source/
            for n in sorted(files):
                src = os.path.join(root, n)
                if os.path.islink(src) or not os.path.isfile(src):
                    continue
                path = "/" + os.path.relpath(src, target)
                sha, size = sha_size(src)
                mods.append({"path": path, "sha256": sha, "size": size, "mode": "%o" % (os.stat(src).st_mode & 0o777)})
                members.append(("files" + path, src))
    src = os.path.join(images, UBOOT)
    if not os.path.isfile(src):
        sys.exit(f"missing in {images}: {UBOOT}")
    sha, size = sha_size(src)
    if size > UBOOT_AREA:
        sys.exit(f"{UBOOT} is {size} bytes: more than its {UBOOT_AREA} on the card")
    members.append(("boot/" + UBOOT, src))
    boot = {"kernel": {"files": kfiles, "modules": mods, "release": release},
            "uboot": {"name": UBOOT, "sha256": sha, "size": size, "offset": UBOOT_OFFSET, "area": UBOOT_AREA}}
    return boot, members


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--manifest", required=True, help="the GUI's CMake install_manifest.txt")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--notes", default="")
    ap.add_argument("--model", default="x6100", choices=["x6100", "x6200"], help="the radio the bundle is built for")
    ap.add_argument("--boot-dir", help="buildroot's images dir: the SD card's kernel set and U-Boot, and the target's "
                    "kernel modules, carried for the radio to install only when asked")
    a = ap.parse_args()

    paths = set()
    with open(a.manifest) as f:
        for line in f:
            p = line.strip()
            p = "/" + os.path.normpath(p).lstrip("/")   # CMake writes "//etc/..." for the rootfs files
            if p != "/" and not p.startswith(SKIP_PREFIXES):
                paths.add(p)
    paths.update(WEB_FILES)
    paths.update(LIB_FILES)
    for d in WEB_DIRS:
        for root, _, files in os.walk(os.path.join(a.target, d.lstrip("/"))):
            for n in files:
                paths.add("/" + os.path.relpath(os.path.join(root, n), a.target))

    entries = []
    for p in sorted(paths):
        src = os.path.join(a.target, p.lstrip("/"))
        if not os.path.isfile(src):
            sys.exit(f"missing in target: {p}")
        with open(src, "rb") as f:
            data = f.read()
        entries.append({"path": p, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
                        "mode": "%o" % (os.stat(src).st_mode & 0o777)})
    # GoldCorpHF's bundles, but v0.3.12.20's, the line in the sand: marked OpenX6X00 as every one before it, so the
    # updaters of v0.3.12.19 and before (which take no other) install it and with it the updater that takes
    # GoldCorpHF's (x6100-update's PRODUCT and BRIDGE)
    product = "OpenX6X00" if a.version.lstrip("v") == BRIDGE_VERSION else "GoldCorpHF"
    manifest = {"product": product, "model": a.model, "version": a.version.lstrip("v"),
                "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "notes": a.notes, "files": entries}
    boot_members = []
    if a.boot_dir:
        manifest["boot"], boot_members = boot_part(a.boot_dir, a.target, a.model)
    mjson = json.dumps(manifest, indent=1, sort_keys=True).encode()

    def add_bytes(tar, name, data, mode=0o644):
        ti = tarfile.TarInfo(name)
        ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
        tar.addfile(ti, io.BytesIO(data))

    with tarfile.open(a.out, "w:gz") as tar:
        add_bytes(tar, "manifest.json", mjson)
        for e in entries:
            tar.add(os.path.join(a.target, e["path"].lstrip("/")), "files" + e["path"], recursive=False)
        for member, src in boot_members:
            tar.add(src, member, recursive=False)
    total = sum(e["size"] for e in entries)
    print(f"{a.out}: {len(entries)} files, {total / 1e6:.1f} MB")
    if boot_members:
        b = manifest["boot"]
        print(f"   for the SD card, when asked: kernel {b['kernel']['release'] or '(no modules)'} "
              f"({len(b['kernel']['modules'])} module files), U-Boot {b['uboot']['size']} bytes")


if __name__ == "__main__":
    main()
