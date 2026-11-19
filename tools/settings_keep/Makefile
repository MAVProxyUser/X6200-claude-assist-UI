# Host test of the settings' last good copy (src/params/db_keep.c) and of the GUI's start with it (src/params/db.c):
# `make` builds it and runs it. The real database_init(), its migrations and the copy, on params.db files made from
# sql/params.sql in build/data, the copy in build/sys (the system partition's /var/lib/x6100 on a radio); stand-ins
# for the new settings' tables, the start trace and LVGL's log (stubs.c). A damaged params.db replaced with the copy
# and the note it gives, the defaults without a good copy, a missing params.db never given the copy, no copy made of
# a start on the defaults or of a damaged file, and the checks' time on a large params.db. See settings_keep_test.c.
# db.c's headers want LVGL beside src/ (LVGL, its log) and liquid-dsp's (DEPS, as host/build.sh puts them in
# build-host/deps).
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
BUILD    := $(CURDIR)/build
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DBUILD='"$(BUILD)"' -DBOARD_DATA='"$(BUILD)/data"' -DBOARD_SHARE='"$(BUILD)/share"' \
            -DPARAMS_KEEP='"$(BUILD)/sys/params-good.db"' -DSPARE_DB='"$(BUILD)/tmp/x6100-params.db"'
# SQLite as the radios have it (buildroot's, upstream): on a Mac Homebrew's when there. macOS's own will not open a WAL
# database read-only while another connection has it (SQLITE_CANTOPEN in the copy's backup step, 2026-11-19), which
# the radios' does: the copy failed on a Mac only
comma    := ,
SQLITE   := $(if $(and $(filter Darwin,$(shell uname -s)),$(wildcard /opt/homebrew/opt/sqlite/lib/libsqlite3.dylib)),/opt/homebrew/opt/sqlite)
CFLAGS   := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g -pthread $(DEFS) $(if $(SQLITE),-I $(SQLITE)/include) \
            -I $(TOP) -I $(SRC) -I $(DEPS)/include
SQLITE_L := $(if $(SQLITE),-L $(SQLITE)/lib -Wl$(comma)-rpath$(comma)$(SQLITE)/lib) -lsqlite3
PARAMS   := $(SRC)/params/db.c $(SRC)/params/db_keep.c
HEADERS  := $(SRC)/params/db.h $(SRC)/params/db_keep.h $(SRC)/params/migrations.h $(SRC)/board.h

all: test

# The migrations as they are, their old printf formats not warned of (as in tools/screen_idle)
build/migrations.o: $(SRC)/params/migrations.c $(HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) -w -c -o $@ $<

settings_keep_test: settings_keep_test.c stubs.c $(PARAMS) build/migrations.o $(HEADERS)
	$(CC) $(CFLAGS) -o $@ settings_keep_test.c stubs.c $(PARAMS) build/migrations.o $(SQLITE_L)

test: settings_keep_test
	./settings_keep_test $(TOP)/sql/params.sql

clean:
	rm -rf settings_keep_test build

.PHONY: all test clean
