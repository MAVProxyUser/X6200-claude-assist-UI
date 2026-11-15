#!/usr/bin/env python3
"""The SD card's kernel and U-Boot in an update (rootfs/usr/sbin/x6100-update install --kernel/--uboot, bundle-info,
boot-rollback, boot-check), run on the Mac against a card of its own: a file for the card's raw bytes (its U-Boot area
from 8 KiB to 1 MiB, something else before and after), a folder for its FAT boot partition, a folder for the root's
/lib/modules, sysfs and /proc/cmdline as files. See the Makefile:

    make          (python3 -I -B boot_test.py [-v])

1. Refused before a byte is written: a disk that says MMC (the internal eMMC), a radio not started from the SD card,
   a boot partition that starts inside U-Boot's area, a bundle that wants a file outside the boot partition or
   /lib/modules.
2. Only what is ticked: no flag touches nothing on the card; --uboot alone leaves the kernel and its modules.
3. --kernel --uboot: the kernel set on the boot partition with each old file kept as <name>.prev, the new release's
   modules, U-Boot in its area padded with zeros, the bytes before and after it as they were, the old area saved.
4. U-Boot that does not read back as written: the old one put back, the kernel set undone too, nothing changed.
5. boot-rollback: the card as it was before (U-Boot's area, the kernel set, the modules, the new release's folder gone).
6. A power cut while it was writing (the journal still "applying"): boot-check puts the card back as it was.
7. bundle-info: what differs from the card (both before, neither after); nothing offered off the SD card; what the
   bundle's version needs of them (its card image level), or that a card image is needed all the same.
8. A part the bundle does not carry, or that the card has already, is left and said: a second install of the same
   bundle keeps the .prev files of the kernel before.
9. The card image level: a "<model>-boot" line's parts on the card reach it (boot-level), boot-rollback takes it back;
   without them it is still wanted, with the parts that would reach it.
10. The modules of kernels nothing can start go: kept are the running one's, the card's and the one it replaced.
11. upgrade passes --kernel/--uboot to the install.
"""
import gzip
import hashlib
import io
import json
import os
import runpy
import shutil
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
UPDATER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-update")
MKBUNDLE = os.path.join(HERE, "mkbundle.py")

KIB = 1024
AREA = 1016 * KIB


def read(path):
    with open(path, "rb") as f:
        return f.read()


class Card:
    """A radio's SD card and the updater's functions on it, all in one folder"""

    def __init__(self, root):
        self.root = root
        self.up = runpy.run_path(UPDATER)["apply"].__globals__
        state, work = os.path.join(root, "state"), os.path.join(root, "work")
        for d in (state, work):
            os.makedirs(d)
        self.version = os.path.join(root, "app-version")
        with open(self.version, "w") as f:
            f.write("0.3.12\n")
        # the card: 8 KiB of partition table, U-Boot's 1016 KiB, then 1 MiB of the boot partition's start
        self.dev = os.path.join(root, "mmcblk0")
        self.head, self.old_uboot, self.tail = b"P" * 8 * KIB, (b"OLD-UBOOT" * 120000)[:AREA], b"F" * 1024 * KIB
        with open(self.dev, "wb") as f:
            f.write(self.head + self.old_uboot + self.tail)
        # the boot partition, mounted
        self.boot = os.path.join(root, "boot")
        os.makedirs(self.boot)
        self.old_kernel = {"zImage": b"old kernel", "sun8i-r16-x6200.dtb": b"old dtb", "boot.scr": b"old script"}
        for n, data in self.old_kernel.items():
            with open(os.path.join(self.boot, n), "wb") as f:
                f.write(data)
        self.mounts = 0
        # the root's modules: the running kernel's
        self.rootfs = os.path.join(root, "rootfs")
        self.old_mod = os.path.join(self.rootfs, "lib", "modules", "6.1.82", "kernel", "wifi.ko")
        os.makedirs(os.path.dirname(self.old_mod))
        with open(self.old_mod, "wb") as f:
            f.write(b"old wifi module")
        # sysfs and the command line of a radio started from the SD card
        self.sys = os.path.join(root, "sys", "block")
        self.set_sd("SD", 2048)
        self.cmdline = os.path.join(root, "cmdline")
        self.set_cmdline("console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait panic=10")
        self.restarts = 0
        self.reboots = 0

        def restart():
            self.restarts += 1

        def reboot():
            self.reboots += 1

        def mount(ro=False):
            self.mounts += 1
            return self.boot

        # the card image: level 2, the radio software's own image-required file (what the version installed wants)
        self.stamp = os.path.join(root, "openx6x00-image")
        with open(self.stamp, "w") as f:
            f.write("model=x6200\nlevel=2\n")
        self.required = os.path.join(root, "image-required")
        self.set_required("x6200 2 the WiFi driver fix\n")
        self.up.update(WORK=work, STATE=state, PREV=os.path.join(state, "prev"), JOURNAL=os.path.join(state, "journal.json"),
                       PENDING=os.path.join(state, "pending.json"), LAST=os.path.join(state, "last.json"),
                       LOCK=os.path.join(state, "lock"), PROGRESS=os.path.join(work, ".progress"),
                       VERSION_FILE=self.version, restart_apps=restart, radio_model=lambda: "x6200",
                       TAILSCALE=os.path.join(root, "no-tailscale"), TAILSCALE_STATE=os.path.join(root, "no-state"),
                       BOOT_JOURNAL=os.path.join(state, "boot-journal.json"), BOOT_PREV=os.path.join(state, "boot-prev"),
                       UBOOT_PREV=os.path.join(state, "uboot-prev.bin"), SYS_BLOCK=self.sys, CMDLINE=self.cmdline,
                       SD_DEV=self.dev, MODULES_ROOT=self.rootfs, boot_mount=mount, boot_umount=lambda: None,
                       BOOT_LEVEL=os.path.join(state, "boot-level"), IMAGE_STAMP=self.stamp,
                       IMAGE_REQUIRED=self.required, running_release=lambda: "6.1.82",
                       POWERON=os.path.join(state, "poweron.json"), BOOT_ID=os.path.join(root, "boot_id"),
                       reboot_radio=reboot)
        with open(os.path.join(root, "boot_id"), "w") as f:
            f.write("6a1b2c3d-0000-4000-8000-boot-one\n")

    def set_required(self, text):
        """The installed version's image-required file (apply() would put the bundle's there)"""
        with open(self.required, "w") as f:
            f.write(text)

    def set_sd(self, kind, p1_start):
        d = os.path.join(self.sys, "mmcblk0")
        os.makedirs(os.path.join(d, "device"), exist_ok=True)
        os.makedirs(os.path.join(d, "mmcblk0p1"), exist_ok=True)
        with open(os.path.join(d, "device", "type"), "w") as f:
            f.write(kind + "\n")
        with open(os.path.join(d, "mmcblk0p1", "start"), "w") as f:
            f.write(f"{p1_start}\n")

    def set_cmdline(self, text):
        with open(self.cmdline, "w") as f:
            f.write(text + "\n")

    def raw(self):
        with open(self.dev, "rb") as f:
            return f.read()

    def boot_files(self):
        return {n: read(os.path.join(self.boot, n)) for n in sorted(os.listdir(self.boot))}

    def bundle(self, version="0.3.13", kernel=b"new kernel", uboot=b"NEW-UBOOT" * 50000, release="6.1.90",
               boot=None, image=None):
        """A bundle with no files of the software (they would land on this Mac's own /usr) and the card's parts, its
        "boot" part made by mkbundle.py's own boot_part from an images folder and a target's modules"""
        images, target = os.path.join(self.root, "images"), os.path.join(self.root, "target")
        shutil.rmtree(images, ignore_errors=True)
        shutil.rmtree(target, ignore_errors=True)
        os.makedirs(images)
        for n, data in (("zImage", kernel), ("sun8i-r16-x6200.dtb", b"new dtb"), ("boot.scr", b"new script"),
                        ("u-boot-sunxi-with-spl.bin", uboot)):
            with open(os.path.join(images, n), "wb") as f:
                f.write(data)
        mod = os.path.join(target, "lib", "modules", release, "kernel", "wifi.ko")
        os.makedirs(os.path.dirname(mod))
        with open(mod, "wb") as f:
            f.write(b"new wifi module")
        with open(os.path.join(target, "lib", "modules", release, "modules.dep"), "wb") as f:
            f.write(b"kernel/wifi.ko:\n")
        part, members = runpy.run_path(MKBUNDLE)["boot_part"](images, target, "x6200")
        manifest = {"product": "GoldCorpHF", "model": "x6200", "version": version, "files": [],
                    "boot": boot if boot is not None else part}
        out = os.path.join(self.root, f"bundle-{version}.tar.gz")
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tar:
            data = json.dumps(manifest).encode()
            ti = tarfile.TarInfo("manifest.json")
            ti.size = len(data)
            tar.addfile(ti, io.BytesIO(data))
            if image is not None:       # its image-required, among its files (not in the manifest: not installed here)
                data = image.encode()
                ti = tarfile.TarInfo("files/usr/share/x6100/image-required")
                ti.size = len(data)
                tar.addfile(ti, io.BytesIO(data))
            for member, src in members:
                tar.add(src, member, recursive=False)
        with gzip.open(out, "wb") as f:
            f.write(buf.getvalue())
        return out

    def run(self, cmd, *args):
        return self.up["cmd_" + cmd.replace("-", "_")](list(args))


class BootInstall(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.c = Card(self.tmp)
        self.Fail = self.c.up["Fail"]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def assertCardAsBefore(self):
        self.assertEqual(self.c.raw(), self.c.head + self.c.old_uboot + self.c.tail)
        self.assertEqual(self.c.boot_files(), self.c.old_kernel)
        self.assertEqual(read(self.c.old_mod), b"old wifi module")
        self.assertFalse(os.path.exists(os.path.join(self.c.rootfs, "lib", "modules", "6.1.90")))

    # 0. The line in the sand: GoldCorpHF's bundles, and of the OpenX6X00-marked ones only v0.3.12.20's (the first
    #    under the new name, marked as before so that the older updaters take it)

    def test_product_line_in_the_sand(self):
        ok = self.c.up["check_manifest"]
        for product, version in (("GoldCorpHF", "0.3.12.21"), ("OpenX6X00", "0.3.12.20")):
            ok(json.dumps({"product": product, "model": "x6200", "version": version, "files": []}))
        for product, version in (("OpenX6X00", "0.3.12.19"), ("OpenX6X00", "0.3.12.21"), ("OpenX6X00", "0.4.0"),
                                 ("Other", "0.3.12.20")):
            with self.assertRaises(self.Fail):
                ok(json.dumps({"product": product, "model": "x6200", "version": version, "files": []}))

    # 1. Refused before a byte is written

    def test_refuses_emmc(self):
        b = self.c.bundle()
        self.c.set_sd("MMC", 2048)
        with self.assertRaisesRegex(self.Fail, "not an SD card"):
            self.c.run("install", b, "--kernel", "--uboot")
        self.assertCardAsBefore()
        self.assertFalse(os.path.exists(self.c.up["BOOT_JOURNAL"]))

    def test_refuses_radio_not_started_from_the_card(self):
        b = self.c.bundle()
        self.c.set_cmdline("console=ttyS0,115200 root=/dev/mmcblk2p2 rootwait")
        with self.assertRaisesRegex(self.Fail, "did not start from the SD card"):
            self.c.run("install", b, "--uboot")
        self.assertCardAsBefore()

    def test_refuses_boot_partition_inside_uboot_area(self):
        b = self.c.bundle()
        self.c.set_sd("SD", 1024)       # p1 at 512 KiB
        with self.assertRaisesRegex(self.Fail, "inside U-Boot's area"):
            self.c.run("install", b, "--uboot")
        self.assertCardAsBefore()

    def test_refuses_files_outside_the_boot_partition_and_modules(self):
        for boot in ({"kernel": {"files": [{"name": "../zImage", "sha256": "0", "size": 1}]}},
                     {"kernel": {"files": [{"name": "zImage", "sha256": "0", "size": 1}],
                                 "modules": [{"path": "/etc/passwd", "sha256": "0", "size": 1}]}},
                     {"uboot": {"name": "u", "sha256": "0", "size": AREA + 1, "offset": 8 * KIB}},
                     {"uboot": {"name": "u", "sha256": "0", "size": 10, "offset": 0}}):
            b = self.c.bundle(boot=boot)
            with self.assertRaisesRegex(self.Fail, "refused"):
                self.c.run("install", b, "--kernel", "--uboot")
            self.assertCardAsBefore()


    # 2. Only what is ticked

    def test_no_flag_touches_nothing_on_the_card(self):
        out = self.c.run("install", self.c.bundle())
        self.assertEqual(out, "ok 0.3.13")
        self.assertCardAsBefore()
        self.assertEqual(self.c.mounts, 0)

    def test_uboot_alone_leaves_the_kernel(self):
        out = self.c.run("install", self.c.bundle(), "--uboot")
        self.assertIn("U-Boot on the SD card: from the next power-on", out)
        self.assertNotIn("kernel", out)
        self.assertEqual(self.c.boot_files(), self.c.old_kernel)
        self.assertFalse(os.path.exists(os.path.join(self.c.rootfs, "lib", "modules", "6.1.90")))
        self.assertEqual(self.c.raw()[8 * KIB:8 * KIB + 9], b"NEW-UBOOT")

    # 3. Both

    def test_kernel_with_reboot(self):
        """--reboot: the radio restarted instead of the apps (x6x00-reboot: the X6200's base kept on), the kernel
        starting with it, so no power-on is asked for after"""
        out = self.c.run("install", self.c.bundle(), "--kernel", "--reboot")
        self.assertTrue(out.endswith("(the radio restarts)"), out)
        self.assertEqual((self.c.reboots, self.c.restarts), (1, 0))
        self.assertFalse(os.path.exists(self.c.up["POWERON"]))
        with self.assertRaisesRegex(self.Fail, "install takes --kernel, --uboot and --reboot"):
            self.c.run("install", self.c.bundle(), "--now")

    def test_kernel_and_uboot(self):
        new_uboot = b"NEW-UBOOT" * 50000
        out = self.c.run("install", self.c.bundle(uboot=new_uboot), "--kernel", "--uboot")
        self.assertEqual(out, "ok 0.3.13 (the kernel and U-Boot on the SD card: from the next power-on)")
        raw = self.c.raw()
        self.assertEqual(raw[:8 * KIB], self.c.head)                              # the partition table: untouched
        self.assertEqual(raw[8 * KIB:8 * KIB + AREA], new_uboot + bytes(AREA - len(new_uboot)))
        self.assertEqual(raw[8 * KIB + AREA:], self.c.tail)                       # the boot partition: untouched
        with open(self.c.up["UBOOT_PREV"], "rb") as f:
            self.assertEqual(f.read(), self.c.old_uboot)
        files = self.c.boot_files()
        self.assertEqual(files["zImage"], b"new kernel")
        self.assertEqual(files["zImage.prev"], b"old kernel")
        self.assertEqual(files["boot.scr.prev"], b"old script")
        self.assertFalse(any(n.endswith(".x6new") for n in files))
        new_mod = os.path.join(self.c.rootfs, "lib", "modules", "6.1.90", "kernel", "wifi.ko")
        self.assertEqual(read(new_mod), b"new wifi module")
        self.assertEqual(read(self.c.old_mod), b"old wifi module")   # the running kernel's: kept
        self.assertEqual(self.c.up["load"](self.c.up["BOOT_JOURNAL"])["state"], "done")
        self.assertEqual(self.c.restarts, 1)
        # A power-on asked for, in this boot: the GUI asks for it till the boot differs (update_watch.c)
        pon = self.c.up["load"](self.c.up["POWERON"])
        self.assertEqual(pon["boot_id"], "6a1b2c3d-0000-4000-8000-boot-one")
        self.assertEqual(pon["what"], "the kernel and U-Boot")

    # 4. Not read back as written

    def test_uboot_not_read_back_puts_everything_back(self):
        read, write = self.c.up["uboot_read"], self.c.up["uboot_write"]
        writes = []

        def bad_read(n=AREA):
            return read(n) if len(writes) != 1 else b"\0" * n      # the read-back after the first write: wrong

        def counted_write(area):
            write(area)
            writes.append(1)
        self.c.up.update(uboot_read=bad_read, uboot_write=counted_write)
        with self.assertRaisesRegex(self.Fail, "did not read back"):
            self.c.run("install", self.c.bundle(), "--kernel", "--uboot")
        self.assertCardAsBefore()
        self.assertEqual(self.c.up["load"](self.c.up["BOOT_JOURNAL"])["state"], "rolled back")
        self.assertFalse(os.path.exists(self.c.up["PENDING"]))                  # the software not applied either
        self.assertEqual(self.c.restarts, 0)

    # 5. boot-rollback

    def test_boot_rollback(self):
        self.c.run("install", self.c.bundle(), "--kernel", "--uboot")
        os.remove(self.c.up["POWERON"])
        out = self.c.run("boot-rollback")
        self.assertIn("back on the SD card: from the next power-on", out)
        self.assertEqual(self.c.up["load"](self.c.up["POWERON"])["what"], "the kernel and U-Boot from before")
        self.assertCardAsBefore()
        with self.assertRaisesRegex(self.Fail, "no kernel or U-Boot install"):
            self.c.run("boot-rollback")

    def test_boot_rollback_with_the_same_release_puts_its_modules_back(self):
        self.c.run("install", self.c.bundle(release="6.1.82"), "--kernel")
        self.assertEqual(read(self.c.old_mod), b"new wifi module")
        self.c.run("boot-rollback")
        self.assertEqual(read(self.c.old_mod), b"old wifi module")
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(os.path.dirname(self.c.old_mod)), "modules.dep")))

    # 6. A power cut while it was writing

    def test_boot_check_after_a_power_cut(self):
        self.c.run("install", self.c.bundle(), "--kernel", "--uboot")
        j = self.c.up["load"](self.c.up["BOOT_JOURNAL"])
        j["state"] = "applying"
        self.c.up["save"](self.c.up["BOOT_JOURNAL"], j)
        self.c.run("boot-check")
        self.assertCardAsBefore()
        self.assertEqual(self.c.up["load"](self.c.up["BOOT_JOURNAL"])["state"], "rolled back")

    # 7. bundle-info

    def test_bundle_info(self):
        b = self.c.bundle()
        self.assertEqual(self.c.run("bundle-info", b),
                         {"version": "0.3.13", "sd": True, "kernel": True, "uboot": True, "kernel_release": "6.1.90",
                          "need_kernel": False, "need_uboot": False, "image_needed": False})
        self.c.run("install", b, "--kernel", "--uboot")
        info = self.c.run("bundle-info", b)
        self.assertEqual((info["kernel"], info["uboot"]), (False, False))
        self.c.set_sd("MMC", 2048)
        info = self.c.run("bundle-info", b)
        self.assertEqual((info["sd"], info["kernel"], info["uboot"]), (False, False, False))

    def test_bundle_info_of_a_bundle_without_boot_parts(self):
        info = self.c.run("bundle-info", self.c.bundle(boot={}))
        self.assertEqual((info["kernel"], info["uboot"]), (False, False))

    def test_bundle_info_says_what_its_version_needs(self):
        info = self.c.run("bundle-info", self.c.bundle(image="x6200 3 the new kernel\nx6200-boot 3 kernel\n"))
        self.assertEqual((info["need_kernel"], info["need_uboot"], info["image_needed"]), (True, False, False))
        info = self.c.run("bundle-info", self.c.bundle(image="x6200 3 a new root filesystem\n"))
        self.assertEqual((info["need_kernel"], info["need_uboot"], info["image_needed"]), (False, False, True))
        info = self.c.run("bundle-info", self.c.bundle(boot={}, image="x6200 3 the new kernel\nx6200-boot 3 kernel\n"))
        self.assertEqual((info["need_kernel"], info["image_needed"]), (False, True))      # it does not carry it
        info = self.c.run("bundle-info", self.c.bundle(image="x6200 2 the WiFi driver fix\n"))
        self.assertEqual((info["need_kernel"], info["need_uboot"], info["image_needed"]), (False, False, False))

    # 8. Left when not carried, or on the card already

    def test_a_part_the_bundle_does_not_carry_is_left(self):
        out = self.c.run("install", self.c.bundle(boot={}), "--kernel")
        self.assertEqual(out, "ok 0.3.13 (the bundle carries no kernel)")
        self.assertCardAsBefore()
        self.assertFalse(os.path.exists(self.c.up["BOOT_JOURNAL"]))

    def test_a_part_the_card_has_already_is_left(self):
        b = self.c.bundle()
        self.c.run("install", b, "--kernel", "--uboot")
        out = self.c.run("install", b, "--kernel", "--uboot")
        self.assertEqual(out, "ok 0.3.13 (the kernel on the SD card is the bundle's already; U-Boot on the SD card is "
                              "the bundle's already)")
        files = self.c.boot_files()
        self.assertEqual(files["zImage.prev"], b"old kernel")         # the one before: still there to go back to
        with open(self.c.up["UBOOT_PREV"], "rb") as f:
            self.assertEqual(f.read(), self.c.old_uboot)
        self.c.run("boot-rollback")
        self.assertCardAsBefore()

    # 9. The card image level

    def test_image_level_reached_by_the_kernel(self):
        up = self.c.up
        self.c.set_required("x6200 3 the new kernel\nx6200-boot 3 kernel\n")
        self.assertEqual(up["image_state"](), {"level": 2, "required": 3, "needed": True, "note": "the new kernel",
                                               "boot": ["kernel"]})
        self.c.run("install", self.c.bundle(), "--kernel")
        self.assertEqual(up["image_level"](), 3)
        self.assertFalse(up["image_state"]()["needed"])
        self.c.run("boot-rollback")
        self.assertEqual(up["image_level"](), 2)
        self.assertTrue(up["image_state"]()["needed"])

    def test_image_level_not_reached_without_its_parts(self):
        up = self.c.up
        self.c.set_required("x6200 3 the new kernel and U-Boot\nx6200-boot 3 kernel uboot\n")
        self.c.run("install", self.c.bundle(), "--kernel")             # U-Boot left out
        self.assertEqual(up["image_level"](), 2)
        self.c.run("install", self.c.bundle(), "--uboot")              # the kernel on the card already
        self.assertEqual(up["image_level"](), 3)

    def test_image_routes(self):
        route = self.c.up["boot_route"]
        text = "x6200 4 x\nx6200-boot 3 kernel\nx6200-boot 4 uboot\nx6100-boot 2 kernel\n"
        self.assertEqual(route(text, 2, 4, "x6200"), {"kernel", "uboot"})
        self.assertEqual(route(text, 3, 4, "x6200"), {"uboot"})
        self.assertIsNone(route(text, 1, 4, "x6200"))                # level 2 has no route: a card image
        self.assertEqual(route(text, 4, 4, "x6200"), set())
        self.assertIsNone(route(text, 1, 2, "x6200"))

    # 10. Stale modules

    def test_stale_modules_go(self):
        mods = os.path.join(self.c.rootfs, "lib", "modules")
        for d, n in (("6.1.70", "modules.dep"), ("not-a-kernel", "readme")):
            os.makedirs(os.path.join(mods, d))
            with open(os.path.join(mods, d, n), "w") as f:
                f.write("x")
        self.c.run("install", self.c.bundle(release="6.1.90"), "--kernel")
        self.assertEqual(sorted(os.listdir(mods)), ["6.1.82", "6.1.90", "not-a-kernel"])
        # still running 6.1.82, the card's 6.1.90 replaced by 6.1.95: all three kept (boot-rollback brings 6.1.90 back)
        self.c.run("install", self.c.bundle(kernel=b"newer kernel", release="6.1.95"), "--kernel")
        self.assertEqual(sorted(os.listdir(mods)), ["6.1.82", "6.1.90", "6.1.95", "not-a-kernel"])
        self.c.run("boot-rollback")
        self.assertEqual(self.c.boot_files()["zImage"], b"new kernel")
        self.assertEqual(sorted(os.listdir(mods)), ["6.1.82", "6.1.90", "not-a-kernel"])
        # started on 6.1.90 now: a kernel 6.1.99 makes 6.1.82 go
        self.c.up["running_release"] = lambda: "6.1.90"
        self.c.run("install", self.c.bundle(kernel=b"newest kernel", release="6.1.99"), "--kernel")
        self.assertEqual(sorted(os.listdir(mods)), ["6.1.90", "6.1.99", "not-a-kernel"])

    def old_build_files(self):
        """The running release's folder as an older build of it left it: modules and an index the new set does not
        ship, and a symlink (left as it is)"""
        top = os.path.join(self.c.rootfs, "lib", "modules", "6.1.82")
        old = {"kernel/drivers/media/dvb/tuner.ko": b"old tuner", "kernel/drivers/media/dvb/demod.ko": b"old demod",
               "kernel/sound/codec.ko": b"old codec", "modules.alias.bin": b"old index"}
        for rel, data in old.items():
            os.makedirs(os.path.dirname(os.path.join(top, rel)), exist_ok=True)
            with open(os.path.join(top, rel), "wb") as f:
                f.write(data)
        os.symlink("/nowhere", os.path.join(top, "build"))
        return top, old

    def assertOldBuildBack(self, top, old):
        for rel, data in old.items():
            self.assertEqual(read(os.path.join(top, rel)), data)
        self.assertEqual(read(self.c.old_mod), b"old wifi module")
        self.assertFalse(os.path.exists(os.path.join(top, "modules.dep")))     # new in the bundle: gone again
        self.assertTrue(os.path.islink(os.path.join(top, "build")))

    def test_files_the_same_release_no_longer_ships_go(self):
        top, old = self.old_build_files()
        out = self.c.run("install", self.c.bundle(release="6.1.82"), "--kernel")
        self.assertEqual(out, "ok 0.3.13 (the kernel on the SD card: from the next power-on)")
        found = sorted(os.path.relpath(os.path.join(r, n), top) for r, _, fs in os.walk(top) for n in fs)
        self.assertEqual(found, ["build", "kernel/wifi.ko", "modules.dep"])
        self.assertEqual(sorted(os.listdir(os.path.join(top, "kernel"))), ["wifi.ko"])    # emptied folders gone
        j = self.c.up["load"](self.c.up["BOOT_JOURNAL"])["kernel"]
        self.assertEqual(j["stale"], sorted("/lib/modules/6.1.82/" + rel for rel in old))
        self.assertEqual(j["stale_state"], "moved")
        # boot-rollback brings them back, with the module the bundle replaced
        self.c.run("boot-rollback")
        self.assertOldBuildBack(top, old)
        # another release's kernel leaves this folder alone
        self.c.run("install", self.c.bundle(release="6.1.90"), "--kernel")
        self.assertOldBuildBack(top, old)

    def test_files_the_same_release_no_longer_ships_back_after_a_power_cut(self):
        top, old = self.old_build_files()
        self.c.run("install", self.c.bundle(release="6.1.82"), "--kernel")
        j = self.c.up["load"](self.c.up["BOOT_JOURNAL"])
        j["state"], j["kernel"]["stale_state"] = "applying", "moving"
        self.c.up["save"](self.c.up["BOOT_JOURNAL"], j)
        os.makedirs(os.path.join(top, "kernel", "sound"))
        os.replace(os.path.join(self.c.up["BOOT_PREV"], "lib/modules/6.1.82/kernel/sound/codec.ko"),
                   os.path.join(top, "kernel", "sound", "codec.ko"))           # cut before that one moved
        self.c.run("boot-check")
        self.assertOldBuildBack(top, old)
        self.assertEqual(self.c.boot_files(), self.c.old_kernel)

    # 11. upgrade

    def test_upgrade_passes_the_flags(self):
        b = self.c.bundle()
        self.c.up["cmd_check"] = lambda args: {"newer": True, "latest": "0.3.13", "url": "https://x/b.tar.gz"}
        self.c.up["cmd_download"] = lambda args: b
        out = self.c.run("upgrade", "--kernel")
        self.assertEqual(out, "ok 0.3.13 (the kernel on the SD card: from the next power-on)")
        with self.assertRaisesRegex(self.Fail, "upgrade takes"):
            self.c.run("upgrade", "--all")


if __name__ == "__main__":
    unittest.main()
