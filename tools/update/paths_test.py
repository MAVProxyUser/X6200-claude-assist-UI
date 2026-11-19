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


if __name__ == "__main__":
    unittest.main(verbosity=1)
