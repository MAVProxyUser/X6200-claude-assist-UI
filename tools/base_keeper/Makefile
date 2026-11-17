# Host test of the X6200's base keeper (rootfs/usr/sbin/x6200-base-keeper): the keeper's own code with the base and
# the GUI faked. See keeper_test.py for the cases.
RUN = python3 -I -B

all: test

test:
	$(RUN) keeper_test.py

.PHONY: all test
