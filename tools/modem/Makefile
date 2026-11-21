# x6x00-modem: BinModem's fax modems in C, for a link between two radios (README.md).
# GPL-3.0-or-later, as BinModem.
#   make test        the pumps through the simulated SSB link (on the Mac or any host)
#   make interop     against BinModem's own (needs a BinModem checkout built: BINMODEM=...)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
BUILD   ?= build
CORE    = dsp.c v21.c v27ter.c v29.c qamrx.c pump.c
BINMODEM ?= ../../build/binmodem

all: $(BUILD)/test_pump $(BUILD)/interop

$(BUILD):
	mkdir -p $@

$(BUILD)/test_pump: test_pump.c channel.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_pump.c channel.c $(CORE) -lm

$(BUILD)/interop: interop.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ interop.c $(CORE) -lm

# Each rate through: a clean line; the radios' audio filters with their carriers 37 Hz apart; their clocks
# 120 ppm apart and 60 Hz; and 12 Hz, 50 ppm and noise. Every bit must come back.
test: $(BUILD)/test_pump
	$(BUILD)/test_pump 0 0 300 0
	$(BUILD)/test_pump -37 0 300 1
	$(BUILD)/test_pump -60 -120 300 1
	$(BUILD)/test_pump 12 50 30 1

interop: $(BUILD)/interop
	./interop.sh $(BINMODEM)

clean:
	rm -rf $(BUILD)

.PHONY: all test interop clean
