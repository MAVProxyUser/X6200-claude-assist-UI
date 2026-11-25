# Host test of the CI-V CAT's modes (src/cat.cpp, #-less: a -Wall build's find, 2026-11-25): `make` builds it and runs
# it. The real src/cat.cpp (included by the test, its process_req called as the UART's frames would be) and the real
# settings (src/cfg) on a database made in memory from sql/params.sql; stand-ins for LVGL's log and async calls, the
# scheduler and the radio's side (stubs.cpp). src/cfg's headers want LVGL beside src/ and liquid-dsp's and ft8_lib's
# (DEPS, as host/build.sh puts them in build-host/deps). See cat_modes_test.cpp for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DBOARD_DATA='"$(CURDIR)/build"' -DBOARD_X6200=1
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
CFG_CFLAGS := -std=gnu11 -O1 -g $(DEFS) $(INC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC)
CFG_CPP  := $(wildcard $(SRC)/cfg/*.cpp)
CFG_C    := $(wildcard $(SRC)/cfg/*.c)
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h

all: test

build/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build && $(CC) $(CFG_CFLAGS) -c -o $@ $<

cat_modes_test: cat_modes_test.cpp stubs.cpp $(SRC)/cat.cpp $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -o $@ cat_modes_test.cpp stubs.cpp $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: cat_modes_test
	./cat_modes_test $(TOP)/sql/params.sql

clean:
	rm -rf cat_modes_test build

.PHONY: all test clean
