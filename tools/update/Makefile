# Host test of what the radio says after a version was taken back (rootfs/usr/sbin/x6100-update): `make` runs it, the
# updater's own code on a folder of its own under build/tmp for each case. The start loop's step back, the one of a
# version that could not use the settings file, the one by hand and the one of an install cut short: the reason and
# the sentence in last.json, "notice" giving it once, "status" giving it, and last.json as earlier updaters wrote it.
# See rollback_test.py for the cases. boot_test.py: the SD card's kernel and U-Boot (install --kernel/--uboot,
# bundle-info, boot-rollback, boot-check) against a card of its own (a file, folders), with mkbundle.py's boot part.
# paths_test.py: every file the image installs where the updater writes, and mkbundle.py refusing one it would not.
RUN = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp python3 -I -B

all: test

test:
	$(RUN) rollback_test.py
	$(RUN) boot_test.py
	$(RUN) paths_test.py

clean:
	rm -rf build

.PHONY: all test clean
