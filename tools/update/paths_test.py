#!/usr/bin/env python3
"""Host test: every file the image installs is one the radio's updater writes (rootfs/usr/sbin/x6100-update's
ALLOWED), and mkbundle.py refuses to build a bundle that has one it does not. The updater refuses a bundle whole for
one file outside ALLOWED: v0.3.12.29 to .31 carried /usr/share/voacapl/README and no radio could take them
(2026-11-19). `make test` runs it.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import os
import re
import runpy
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import mkbundle  # noqa: E402


class Paths(unittest.TestCase):
    def setUp(self):
        self.allowed = mkbundle.updater_allowed()

    def test_the_updaters_own_list(self):
        """mkbundle reads the same ALLOWED the updater runs with"""
        up = runpy.run_path(mkbundle.UPDATER, run_name="x6100_update")
        self.assertEqual(self.allowed, tuple(up["ALLOWED"]))

    def test_rootfs_files(self):
        """The rootfs overlay (CMake installs it at /): each of its files where the updater writes"""
        refused = []
        root = os.path.join(TOP, "rootfs")
        for d, _, files in os.walk(root):
            for n in files:
                p = "/" + os.path.relpath(os.path.join(d, n), root)
                if mkbundle.path_refused(p, self.allowed):
                    refused.append(p)
        self.assertEqual(refused, [], "files the radio's updater would refuse (move them under an ALLOWED folder)")

    def test_cmake_destinations(self):
        """The GUI's CMake installs (prefix /usr) into folders the updater writes"""
        text = open(os.path.join(TOP, "CMakeLists.txt")).read()
        refused = []
        for d in re.findall(r"DESTINATION\s+([^\s)]+)", text):
            if d.startswith("${") or d == "/":
                continue                # the build's own folder; the rootfs overlay (above)
            p = "/usr/" + d.strip("/") + "/x"
            if mkbundle.path_refused(p, self.allowed):
                refused.append(d)
        self.assertEqual(refused, [])

    def test_mkbundle_refuses(self):
        """A file outside ALLOWED stops the build of the bundle, named"""
        with tempfile.TemporaryDirectory() as t:
            target = os.path.join(t, "target")
            for p in ("usr/share/voacapl/README", "usr/bin/x6100-gui"):
                os.makedirs(os.path.dirname(os.path.join(target, p)), exist_ok=True)
                open(os.path.join(target, p), "w").write("x\n")
            manifest = os.path.join(t, "install_manifest.txt")
            open(manifest, "w").write("/usr/share/voacapl/README\n/usr/bin/x6100-gui\n")
            r = subprocess.run([sys.executable, "-I", "-B", os.path.join(HERE, "mkbundle.py"), "--target", target,
                                "--manifest", manifest, "--version", "0.0.1", "--out", os.path.join(t, "b.tar.gz")],
                               capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("/usr/share/voacapl/README", r.stderr)
            self.assertNotIn("/usr/bin/x6100-gui", r.stderr)
            self.assertFalse(os.path.exists(os.path.join(t, "b.tar.gz")))

    def test_rule(self):
        """The updater's rule: under an ALLOWED folder, absolute, no "..", a file"""
        for p in ("/usr/bin/voacapl", "/usr/share/x6100/voacapl/README", "/etc/init.d/S99x"):
            self.assertFalse(mkbundle.path_refused(p, self.allowed), p)
        for p in ("/usr/share/voacapl/README", "usr/bin/x", "/usr/bin/../../root/x", "/usr/bin/", "/opt/x"):
            self.assertTrue(mkbundle.path_refused(p, self.allowed), p)


def tiny_elf(needed):
    """A 32-bit little-endian ELF (ARM's) with one PT_LOAD over the file and a PT_DYNAMIC naming needed (DT_NEEDED),
    as mkbundle.elf_needed reads a library"""
    strtab = b"\0" + b"".join(n.encode() + b"\0" for n in needed)
    offs, at = [], 1
    for n in needed:
        offs.append(at)
        at += len(n) + 1
    ehdr, phdr = 52, 32
    dyn_off = ehdr + 2 * phdr
    dyn = b"".join(struct.pack("<iI", 1, o) for o in offs)
    str_off = dyn_off + len(dyn) + 16
    dyn += struct.pack("<iI", 5, str_off) + struct.pack("<iI", 0, 0)
    size = str_off + len(strtab)
    head = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9) + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, ehdr, 0, 0, ehdr,
                                                                    phdr, 2, 0, 0, 0)
    ph = struct.pack("<IIIIIIII", 1, 0, 0, 0, size, size, 5, 0x1000)
    ph += struct.pack("<IIIIIIII", 2, dyn_off, dyn_off, dyn_off, len(dyn), len(dyn), 6, 4)
    return head + ph + dyn + strtab


class Carry(unittest.TestCase):
    """mkbundle's carried packages (Mesa, GStreamer, ffmpeg) with the libraries they load"""

    def test_elf_needed(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "libEGL.so.1")
            open(p, "wb").write(tiny_elf(["libgbm.so.1", "libc.so.6"]))
            self.assertEqual(mkbundle.elf_needed(p), ["libgbm.so.1", "libc.so.6"])
            open(p, "w").write("not an ELF")
            self.assertEqual(mkbundle.elf_needed(p), [])

    def test_carried_size(self):
        """A bundle's carried part stays small: what it costs on a radio's root filesystem (the X6100's has 191 MB
        free), each name a copy"""
        self.assertLessEqual(len(mkbundle.CARRY_DRI), 2)

    def test_carried(self):
        """The packages' libraries and programs, what they load from /usr/lib followed, nothing else: not the C
        library's (/lib), not headers, pkgconfig or /usr/share, not another package's files"""
        with tempfile.TemporaryDirectory() as t:
            files = {"usr/lib/libEGL.so.1": ["libgbm.so.1", "libc.so.6"], "usr/lib/libgbm.so.1": ["libdrm.so.2"],
                     "usr/lib/libdrm.so.2": [], "lib/libc.so.6": [], "usr/lib/dri/lima_dri.so": ["libexpat.so.1"],
                     "usr/lib/libexpat.so.1": [], "usr/bin/gst-launch-1.0": [], "usr/lib/libunrelated.so": [],
                     "usr/share/drirc.d/00-mesa-defaults.conf": None, "usr/lib/pkgconfig/egl.pc": None,
                     "usr/lib/dri/sun4i-drm_dri.so": [], "usr/lib/dri/rockchip_dri.so": [], "usr/lib/dri/meson_dri.so": []}
            for rel, need in files.items():
                os.makedirs(os.path.dirname(os.path.join(t, rel)), exist_ok=True)
                open(os.path.join(t, rel), "wb").write(tiny_elf(need) if need is not None else b"x\n")
            pfl = os.path.join(t, "packages-file-list.txt")
            open(pfl, "w").write("mesa3d,./usr/lib/libEGL.so.1\nmesa3d,./usr/lib/dri/lima_dri.so\n"
                                 "mesa3d,./usr/lib/dri/sun4i-drm_dri.so\nmesa3d,./usr/lib/dri/rockchip_dri.so\n"
                                 "mesa3d,./usr/lib/dri/meson_dri.so\n"
                                 "mesa3d,./usr/share/drirc.d/00-mesa-defaults.conf\nmesa3d,./usr/lib/pkgconfig/egl.pc\n"
                                 "gstreamer1,./usr/bin/gst-launch-1.0\nother,./usr/lib/libunrelated.so\n")
            got = mkbundle.carried(t, pfl)
            # Mesa's driver names but the radios' two left out (v0.3.12.33 carried 29 copies of it: 355 MB)
            self.assertEqual(got, {"/usr/lib/libEGL.so.1", "/usr/lib/libgbm.so.1", "/usr/lib/libdrm.so.2",
                                   "/usr/lib/dri/lima_dri.so", "/usr/lib/dri/sun4i-drm_dri.so", "/usr/lib/libexpat.so.1",
                                   "/usr/bin/gst-launch-1.0"})
            allowed = mkbundle.updater_allowed()
            self.assertEqual([p for p in got if mkbundle.path_refused(p, allowed)], [])


if __name__ == "__main__":
    unittest.main(verbosity=1)
