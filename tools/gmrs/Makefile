# Host test of src/cfg/gmrs.c, GMRS's channels (the owner's licence, 2026-11-25): `make` builds both parts and runs them.
# - gmrs_test: the 22 channels against the rules written out (frequencies, bandwidths, power, the repeater inputs of
#   15-22), their names, the scan's order and next place, F's GMRS panel's rows, G's channel names, the tones' rows,
#   the lookups, the CTCSS privacy codes (each one of ctcss.c's tones) and the DCS codes; and receive only: every
#   channel and input refused by ham_tx_check (a transverter, the switch on), tx_ranges_check and tx_ranges_sdr_check
#   with GMRS's own reason.
# - gmrs_tx_x6100 / gmrs_tx_x6200: the radio's own receiver and transmitter on the real settings (src/cfg on a database
#   made in memory from sql/params.sql, as tools/aerocb does): GMRS out of reach as the radio comes, received through
#   the X6200's WR12 and a transverter, refused as GMRS through the transverter whatever the switch and the TX ranges
#   (gmrs_tx_test.cpp). src/cfg's headers want LVGL beside src/ and ft8_lib's headers (DEPS), as tools/aerocb's do.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DBOARD_DATA='"$(CURDIR)/build"'
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
CFLAGS   := -std=gnu11 -Wall -Wextra -Werror -O1 -g -pthread -I $(SRC)
CFG_CF   := -std=gnu11 -O1 -g $(DEFS) $(INC)
CXXFLAGS := -std=gnu++17 -Wall -O1 -g $(DEFS) $(INC)
CFG_CPP  := $(wildcard $(SRC)/cfg/*.cpp)
CFG_C    := $(wildcard $(SRC)/cfg/*.c)
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h
TX_TEST  := gmrs_tx_test.cpp stubs.cpp
DEPS_C   := $(SRC)/cfg/gmrs.c $(SRC)/cfg/gmrs.h $(SRC)/cfg/ham_bands.c $(SRC)/cfg/ham_bands.h $(SRC)/cfg/tx_ranges.c \
            $(SRC)/cfg/tx_ranges.h $(SRC)/ctcss.c $(SRC)/ctcss.h

all: test

gmrs_test: gmrs_test.c $(DEPS_C)
	$(CC) $(CFLAGS) -o $@ gmrs_test.c $(SRC)/cfg/gmrs.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/tx_ranges.c $(SRC)/ctcss.c -lm

build/x6100/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6100 && $(CC) $(CFG_CF) -c -o $@ $<

build/x6200/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6200 && $(CC) $(CFG_CF) -DBOARD_X6200=1 -c -o $@ $<

gmrs_tx_x6100: $(TX_TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6100/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -o $@ $(TX_TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

gmrs_tx_x6200: $(TX_TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6200/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -DBOARD_X6200=1 -o $@ $(TX_TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: gmrs_test gmrs_tx_x6100 gmrs_tx_x6200
	./gmrs_test
	./gmrs_tx_x6100 $(TOP)/sql/params.sql
	./gmrs_tx_x6200 $(TOP)/sql/params.sql

clean:
	rm -rf gmrs_test gmrs_tx_x6100 gmrs_tx_x6200 build

.PHONY: all test clean
