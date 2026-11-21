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
import ast
import hashlib
import struct
import io
import json
import os
import sys
import tarfile
import time

BRIDGE_VERSION = "0.3.12.20"     # the last bundle marked OpenX6X00, the first of GoldCorpHF
UPDATER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "rootfs", "usr", "sbin", "x6100-update")


def updater_allowed(updater=UPDATER):
    """The folders the radio's updater writes (its ALLOWED), read from it: a bundle with a file elsewhere is refused
    whole on the radio. v0.3.12.29 to .31 carried /usr/share/voacapl/README and no updater took them (2026-11-19);
    every updater since the first has the same ALLOWED, so a file outside it is refused here, at the build"""
    for node in ast.parse(open(updater).read()).body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "ALLOWED" for t in node.targets):
            return tuple(ast.literal_eval(node.value))
    sys.exit(f"no ALLOWED in {updater}")


def path_refused(p, allowed):
    """As x6100-update's bundle check: True when the radio's updater would refuse a file at path p"""
    return not p.startswith("/") or ".." in p.split("/") or not p.startswith(allowed) or p.endswith("/")

WEB_FILES = ["/usr/bin/x6100_webserver"]
WEB_DIRS = ["/usr/lib/python3.11/site-packages/x6100_webserver"]
# The FT8 library the GUI is built with and links (the buildroot tree's ft8lib package, not the GUI's CMake): without
# it a radio updated in place keeps its card image's. The X6200's images up to v0.3.8 have ft8_lib 117022b, which
# sends a CQ with a compound call (CQ PA/ON6URE) as free text and refuses CQ OT4ABCD and a CQ over 13 characters; both
# trees are at 73b0db8 now, the X6100's since before its first image. The two have the same headers and exported
# symbols (only message.c's packing differs), so the GUI runs on either: v0.3.8's on this one when its bundle is
# installed over a newer one (which leaves the library), this one's on 117022b when a rollback puts that back
LIB_FILES = ["/usr/lib/libft8.so"]
# The SDR's daemon (tools/rtlspec, buildroot's x6x00-rtlspec package: librtlsdr and libhackrf linked in, so it needs
# only libusb) and the libbladeRF it opens for a bladeRF: a radio updated in place keeps its card image's daemon
# otherwise, RTL-SDR only (v0.3.12.23 and before) or none. The GUI tells a daemon running from another file to end and
# starts this one (src/rtl_spectrum.c); a GUI of before reads this daemon's hello all the same (its first five words)
# and dsd-fme, which the daemon runs on B's narrow FM (buildroot's x6x00-dsd-fme; libsndfile, PulseAudio and ncurses,
# which every image has): without it the daemon decodes DMR alone
SDR_FILES = ["/usr/bin/x6x00-rtlspec", "/usr/lib/libbladeRF.so.2", "/usr/bin/dsd-fme",
             "/usr/bin/x6x00-df",       # the KrakenSDR's direction finder (page I), from the same package
             "/usr/bin/x6x00-modem"]    # one end of the modem link between two radios (tools/modem; libc and libm)
SKIP_PREFIXES = ("/usr/include/",)       # the GUI package installs headers too
# kexec-tools' program (the X6100's image): its restart without the processor's reset (x6x00-reboot); carried when the
# target has it, so a radio gets it with the kernel that can use it
OPTIONAL_FILES = ["/usr/sbin/kexec"]
# The image's own packages that a card of before them lacks, carried whole (the owner, 2026-11-19: "I should not have
# to write this to the card myself"): Mesa (the GPU drawing the screen, the X6100's card v0.3.12.28 had none),
# GStreamer and its plugins (the boot video in hardware), ffmpeg (the Internet radio's AAC, Ogg, Opus, FLAC and HLS).
# Their files from buildroot's packages-file-list.txt (--packages-file-list), the libraries and programs only (the
# folders every updater writes), and every library they load from /usr/lib, followed (DT_NEEDED): the same buildroot
# built every card, so a library a card has already is the same one. The C library's, in /lib, are every card's
CARRY_PKGS = ("mesa3d", "gstreamer1", "gst1-plugins-base", "gst1-plugins-good", "gst1-plugins-bad", "ffmpeg")
CARRY_DIRS = ("/usr/lib/", "/usr/bin/")
CARRY_SKIP = ("/usr/lib/pkgconfig/", "/usr/lib/cmake/", "/usr/lib/gstreamer-1.0/include/")
# Mesa's driver is one file under a name for each display controller it knows (29: rockchip, imx-drm, meson...), hard
# links on the card; in a bundle every name is a copy of it (11.7 MB), and v0.3.12.33's carried 355 MB so. The radios'
# two only: the Mali-400's and the R16 display's (what an X6200 card has, and what the GUI's EGL opens)
CARRY_DRI = ("/usr/lib/dri/lima_dri.so", "/usr/lib/dri/sun4i-drm_dri.so")
CARRY_MAX_MB = 80                        # the carried part at most (about 40 MB: Mesa 26, GStreamer and ffmpeg)
# Which radios get them: the X6100's cards up to v0.3.12.31 have no Mesa nor GStreamer; every X6200 card has both
# (and ffmpeg). And not what every X6100 card has already: an updater of before v0.3.12.36 counts each file of a
# bundle, the same or not, and the system partition is 488 MB (v0.3.12.34/.35's 129 MB did not fit: "no room")
CARRY_MODELS = ("x6100",)
CARRY_PKGS_MODEL = ("mesa3d", "gstreamer1", "gst1-plugins-base", "gst1-plugins-good", "gst1-plugins-bad")
CARRY_HAVE = ("libglib-2.0.so", "libgobject-2.0.so", "libgmodule-2.0.so", "libgio-2.0.so", "libgthread-2.0.so",
              "libffi.so", "libz.so", "libexpat.so", "libdrm.so", "libpcre2-8.so", "libav", "libsw", "libpostproc")
UBOOT = "u-boot-sunxi-with-spl.bin"
UBOOT_OFFSET = 8 * 1024                  # genimage's u-boot partition: offset 8K, size 1016K
UBOOT_AREA = 1016 * 1024


def elf_needed(path):
    """The libraries an ELF file loads (its DT_NEEDED names), [] for a file that is not one"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if data[:4] != b"\x7fELF":
        return []
    is64, le = data[4] == 2, data[5] == 1
    e = "<" if le else ">"
    if is64:
        phoff, phentsize, phnum = struct.unpack_from(e + "Q", data, 32)[0], *struct.unpack_from(e + "HH", data, 54)
    else:
        phoff, phentsize, phnum = struct.unpack_from(e + "I", data, 28)[0], *struct.unpack_from(e + "HH", data, 42)
    loads, dyn = [], None
    for i in range(phnum):
        o = phoff + i * phentsize
        if is64:
            ptype, _, poff, pvaddr, _, pfilesz = struct.unpack_from(e + "IIQQQQ", data, o)
        else:
            ptype, poff, pvaddr, _, pfilesz = struct.unpack_from(e + "IIIII", data, o)
        if ptype == 1:
            loads.append((pvaddr, poff, pfilesz))
        elif ptype == 2:
            dyn = (poff, pfilesz)
    if not dyn:
        return []
    ent, fmt = (16, e + "qQ") if is64 else (8, e + "iI")
    needed, strtab = [], None
    for o in range(dyn[0], dyn[0] + dyn[1] - ent + 1, ent):
        tag, val = struct.unpack_from(fmt, data, o)
        if tag == 0:
            break
        if tag == 1:
            needed.append(val)
        elif tag == 5:
            strtab = val
    off = next((poff + strtab - va for va, poff, sz in loads if strtab is not None and va <= strtab < va + sz), None)
    if off is None:
        return []
    return [data[off + n:data.index(b"\0", off + n)].decode(errors="replace") for n in needed]


def carried(target, file_list, pkgs=CARRY_PKGS, have=()):
    """The paths of pkgs' libraries and programs in target (packages-file-list.txt: "pkg,./path" a line), with the
    libraries of /usr/lib they load, followed; not those named in have (a name's start: what the cards have)"""
    out = set()
    with open(file_list) as f:
        for line in f:
            pkg, _, rel = line.strip().partition(",")
            p = "/" + os.path.normpath(rel).lstrip("./")
            if pkg in pkgs and p.startswith(CARRY_DIRS) and not p.startswith(CARRY_SKIP) \
                    and not p.endswith((".a", ".la", ".h")) and os.path.isfile(os.path.join(target, p.lstrip("/"))) \
                    and (not p.startswith("/usr/lib/dri/") or p in CARRY_DRI):
                out.add(p)
    todo = list(out)
    while todo:
        for name in elf_needed(os.path.join(target, todo.pop().lstrip("/"))):
            p = "/usr/lib/" + name
            if name.startswith(have):
                continue
            if p not in out and os.path.isfile(os.path.join(target, p.lstrip("/"))):
                out.add(p)
                todo.append(p)
    return out


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
    ap.add_argument("--packages-file-list", help="buildroot's packages-file-list.txt: Mesa, GStreamer and ffmpeg "
                    "carried (CARRY_PKGS) with the libraries they load")
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
    paths.update(SDR_FILES)
    paths.update(p for p in OPTIONAL_FILES if os.path.isfile(os.path.join(a.target, p.lstrip("/"))))
    extra = set()
    if a.packages_file_list and a.model in CARRY_MODELS:
        extra = carried(a.target, a.packages_file_list, CARRY_PKGS_MODEL, CARRY_HAVE) - paths
        paths.update(extra)
    for d in WEB_DIRS:
        for root, _, files in os.walk(os.path.join(a.target, d.lstrip("/"))):
            for n in files:
                paths.add("/" + os.path.relpath(os.path.join(root, n), a.target))

    allowed = updater_allowed()
    refused = sorted(p for p in paths if path_refused(p, allowed))
    if refused:
        sys.exit("the radio's updater would refuse this bundle whole (x6100-update ALLOWED: "
                 + " ".join(allowed) + "), it has:\n  " + "\n  ".join(refused))

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
        for e in entries:      # each a plain file, a link's (a library's soname) its target's bytes: the updater
            src = os.path.realpath(os.path.join(a.target, e["path"].lstrip("/")))     # reads each member's content
            ti = tar.gettarinfo(src, "files" + e["path"])
            ti.type, ti.linkname, ti.size = tarfile.REGTYPE, "", os.path.getsize(src)   # a hard link's too (Mesa's
            with open(src, "rb") as f:                                                  # two drivers are one file)
                tar.addfile(ti, f)
        for member, src in boot_members:
            tar.add(src, member, recursive=False)
    total = sum(e["size"] for e in entries)
    print(f"{a.out}: {len(entries)} files, {total / 1e6:.1f} MB")
    carried_mb = sum(e["size"] for e in entries if e["path"] in extra) / 1e6
    if carried_mb > CARRY_MAX_MB:
        os.remove(a.out)
        sys.exit(f"the image's packages carried come to {carried_mb:.0f} MB, over {CARRY_MAX_MB}: an X6100's root "
                 f"filesystem has about 190 MB free (v0.3.12.33's 355 MB were Mesa's driver copied 29 times)")
    if extra:
        print(f"   carried from the image's packages ({', '.join(CARRY_PKGS_MODEL)}) and what they load: {len(extra)} files, "
              f"{sum(e['size'] for e in entries if e['path'] in extra) / 1e6:.1f} MB")
    if boot_members:
        b = manifest["boot"]
        print(f"   for the SD card, when asked: kernel {b['kernel']['release'] or '(no modules)'} "
              f"({len(b['kernel']['modules'])} module files), U-Boot {b['uboot']['size']} bytes")


if __name__ == "__main__":
    main()
