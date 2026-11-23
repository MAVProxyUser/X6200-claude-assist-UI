# Host test of the multiboot card's warning (Xiegu Multi-Boot: a new card image goes through the app, written onto the
# card it would erase the boot menu and the other systems): rootfs/etc/init.d/S01multiboot's detection at the start
# and src/image_level.c's card_is_multiboot() with the word at the start (LVGL's headers only: its timer and the
# message box are stood in for). `make` runs the two. See s01multiboot_test.sh and image_level_test.c for the cases.
# GoldCorpHF: upstream's update_test.py (what its x6100-update tells about a multiboot card) is left out with its
# updater: this fork keeps its own (the owner, 2026-11-18: "we do NOT want to use upstreams updater either").
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
T      := test.d
DEFS   := -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DTEST_DIR='"$(T)"' -DBOARD_SHARE='"$(T)"' \
          -DBOARD_DATA='"$(T)/data"' -DIMAGE_STAMP='"$(T)/stamp"' -DMULTIBOOT='"$(T)/multiboot"' -DCMDLINE='"$(T)/cmdline"'
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g $(DEFS) -I $(TOP) -I $(SRC) -isystem $(LVGL)

all: test

image_level_test: image_level_test.c $(SRC)/image_level.c $(SRC)/image_level.h
	$(CC) $(CFLAGS) -o $@ image_level_test.c $(SRC)/image_level.c

test: image_level_test
	sh s01multiboot_test.sh $(TOP)/rootfs/etc/init.d/S01multiboot
	./image_level_test

clean:
	rm -rf image_level_test build $(T)

.PHONY: all test clean
