# Host test of radio.c's side of the X6200's WR12 (#134): `make` builds it and runs it. The real src/radio.c, compiled
# as the PC build compiles it for the X6200 (BOARD_X6200, HOST_BUILD), with the real hw_stub.c, base_guard.c,
# ham_bands.c and tx_ranges.c, and stand-ins for the settings, the WR12's driver and the rest of the GUI (fakes.c). What
# radio.c sends the base is caught on its way to hw_stub.c (--wrap); the radio's own thread is not started (--wrap of
# pthread_create): no packets, the base keys by itself only when the test says so, its data is taken as flowing (--wrap
# of base_no_data: these cases are the WR12's; tools/base_guard tests the no-data rule, #152), and the base log as
# free (--wrap of base_guard_log_free: its one line a second is base_guard's), the CW key's pin caught too (--wrap of
# hw_pin_set). radio.c is compiled inside radio_thread.c, which hands the test the radio thread's turn at the base's
# open TX (#176), and its readings of the base's supply (radio_tick, the packets the test's: --wrap of hw_flow_read)
# through the real bat_watch.c to the top bar and the battery model (#223). See radio_wr12_test.c for the cases,
# fakes.h for the stand-ins; radio.c's log goes to radio_wr12_test.log here, removed after a run that passed.
# radio.c's headers want what the PC build has (host/README.md): LVGL beside src/ (LVGL), liquid-dsp's headers
# (DEPS, as host/build.sh puts them in build-host/deps), and libnm's (pkg-config).
TOP    := ../..
SRC    := $(TOP)/src
DEPS   ?= $(TOP)/build-host/deps
LVGL   ?= $(TOP)/lvgl
DEFS   := -DBOARD_X6200=1 -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 \
          -DUSE_FBDEV=0 -DBOARD_DATA='"$(CURDIR)/build"' -DBOARD_SHARE='"$(CURDIR)/build"' \
          -DBASE_GUARD_LOG='"$(CURDIR)/build/base_i2c.log"'
UNAME  := $(shell uname -s)
# A Mac (2026-11-19): CI was the only place this test ran, so a call radio.c gained that fakes.c has no stand-in for
# showed first there, three times. On a Mac now too, so tools/host_checks.sh finds it before a push: libnm's header is
# the two names wifi.h takes from it (mac_include), liquid-dsp's headers Homebrew's when DEPS has none
ifeq ($(UNAME),Darwin)
  ifeq ($(wildcard $(DEPS)/include/liquid/liquid.h),)
    DEPS := $(shell brew --prefix 2> /dev/null)
  endif
  NM_INC := -I mac_include $(shell pkg-config --cflags glib-2.0)
else
  NM_INC := $(shell pkg-config --cflags libnm)
endif
INC    := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL) $(NM_INC)
# radio.c as the PC build has it, without its warnings; the test's own files with them (but params/types.h's comment)
GUI    := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC)
CFLAGS := $(GUI) -Wall -Wextra -Wno-comment
# The calls caught (above), by the file that has them: hw_stub.c's, base_guard.c's and the C library's. On Linux GNU
# ld's --wrap=S sends every call of S to __wrap_S (fakes.c) and gives the real one as __real_S. A Mac's linker has no
# --wrap: there each file is compiled with S named __wrap_S, and the file that defines S with it named __real_S, which
# comes to the same (mac_real.c: the C library's __real_)
HW_WRAPPED    := hw_vfo_set_freq hw_set_i hw_set_f hw_pin_set hw_flow_read
GUARD_WRAPPED := base_no_data base_guard_log_free
LIBC_WRAPPED  := pthread_create pthread_detach
WRAPPED       := $(HW_WRAPPED) $(GUARD_WRAPPED) $(LIBC_WRAPPED)
wrap_as        = $(foreach s,$(2),-D$(s)=$(1)$(s))

# A setting or a function radio.c came to use that fakes.c has no stand-in for yet no longer stops the build (it did,
# on CI, four times 2026-11-18..20; Linux's lazy binding of a missing name could not take a setting: "unexpected PLT
# reloc type"): the link is run by tools/host_link/autolink.py, which makes a stand-in for each name the linker says
# it lacks (a setting like the rest, through build/auto_settings.h; a function that answers 0 and says so once when
# called) and links again, saying which it made. A case that needs the real thing still FAILs, by its own check.
# ifeq, not $(if ...): the commas of -Wl split $(if)'s arguments (a bare -Wl, 2026-11-19)
ifeq ($(UNAME),Darwin)
  RENAME   := $(call wrap_as,__wrap_,$(WRAPPED))
  WRAPS    :=
  WRAPPERS := build/hw_stub.o build/base_guard.o build/mac_real.o
else
  RENAME   :=
  WRAPS    := $(foreach s,$(WRAPPED),-Wl,--wrap=$(s))
  WRAPPERS := $(SRC)/hw/stub/hw_stub.c $(SRC)/base_guard.c
endif

all: test

build/radio.o: radio_thread.c $(SRC)/radio.c $(wildcard $(SRC)/*.h $(SRC)/hw/*.h $(SRC)/cfg/*.h)
	mkdir -p build && $(CC) $(GUI) $(RENAME) -c -o $@ radio_thread.c

radio_wr12_test: radio_wr12_test.c fakes.c fakes.h build/radio.o $(WRAPPERS) \
                 $(SRC)/pa_bias.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/tx_ranges.c $(SRC)/bat_watch.c
	python3 $(TOP)/tools/host_link/autolink.py --src $(SRC) --stubs build/auto_stubs.c \
	    --settings build/auto_settings.h -- \
	    $(CC) $(CFLAGS) -I build $(RENAME) -o $@ radio_wr12_test.c fakes.c build/radio.o $(WRAPPERS) \
	    $(SRC)/pa_bias.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/tx_ranges.c $(SRC)/bat_watch.c build/auto_stubs.c \
	    -lm $(WRAPS)

# A Mac's: the files that define what is caught, each with its own names as __real_ and the rest as __wrap_
build/hw_stub.o: $(SRC)/hw/stub/hw_stub.c $(wildcard $(SRC)/hw/*.h)
	mkdir -p build && $(CC) $(GUI) $(call wrap_as,__real_,$(HW_WRAPPED)) \
	    $(call wrap_as,__wrap_,$(GUARD_WRAPPED) $(LIBC_WRAPPED)) -c -o $@ $<

build/base_guard.o: $(SRC)/base_guard.c $(SRC)/base_guard.h
	mkdir -p build && $(CC) $(GUI) $(call wrap_as,__real_,$(GUARD_WRAPPED)) \
	    $(call wrap_as,__wrap_,$(HW_WRAPPED) $(LIBC_WRAPPED)) -c -o $@ $<

build/mac_real.o: mac_real.c
	mkdir -p build && $(CC) $(CFLAGS) -c -o $@ $<

test: radio_wr12_test
	./radio_wr12_test

clean:
	rm -rf radio_wr12_test radio_wr12_test.log build

.PHONY: all test clean
