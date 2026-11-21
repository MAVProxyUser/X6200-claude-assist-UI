# x6x00-modem: BinModem's fax modems in C, for a link between two radios (README.md).
# GPL-3.0-or-later, as BinModem.
#   make test        the pumps through the simulated SSB link (on the Mac or any host)
#   make interop     against BinModem's own (needs a BinModem checkout built: BINMODEM=...)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
BUILD   ?= build
CORE    = dsp.c v21.c v27ter.c v29.c qamrx.c pump.c hdlc.c link.c
BINMODEM ?= ../../build/binmodem

all: $(BUILD)/test_pump $(BUILD)/test_link $(BUILD)/interop $(BUILD)/x6x00-modem $(BUILD)/rxfile $(BUILD)/rxfast

$(BUILD):
	mkdir -p $@

$(BUILD)/test_pump: test_pump.c channel.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_pump.c channel.c $(CORE) -lm

$(BUILD)/test_link: test_link.c channel.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_link.c channel.c $(CORE) -lm

$(BUILD)/x6x00-modem: x6x00-modem.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ x6x00-modem.c $(CORE) -lm

# what V.21 makes of a radio end's recording (x6x00-modem --record)
$(BUILD)/rxfile: rxfile.c v21.c dsp.c hdlc.c $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ rxfile.c v21.c dsp.c hdlc.c -lm

# and what the fast receiver makes of one, its HDLC frames
$(BUILD)/rxfast: rxfast.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ rxfast.c $(CORE) -lm

$(BUILD)/interop: interop.c $(CORE) $(wildcard *.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ interop.c $(CORE) -lm

# Each rate through: a clean line; the radios' audio filters with their carriers 37 Hz apart; their clocks
# 120 ppm apart and 60 Hz; and 12 Hz, 50 ppm and noise. Every bit must come back.
# Then the link, both ends a sample at a time through two such paths: both ways on a clean line at 9600; at 14 dB,
# stepping down to 2400. It must finish, and the two must never be keyed at once.
test: $(BUILD)/test_pump $(BUILD)/test_link
	$(BUILD)/test_pump 0 0 300 0
	$(BUILD)/test_pump -37 0 300 1
	$(BUILD)/test_pump -60 -120 300 1
	$(BUILD)/test_pump 12 50 30 1
	$(BUILD)/test_link 300 7 40 8192 1 > $(BUILD)/link1.txt; s=$$?; tail -6 $(BUILD)/link1.txt; exit $$s
	$(BUILD)/test_link 14 -20 -80 4096 0 > $(BUILD)/link2.txt; s=$$?; tail -6 $(BUILD)/link2.txt; exit $$s

interop: $(BUILD)/interop
	./interop.sh $(BINMODEM)

clean:
	rm -rf $(BUILD)

.PHONY: all test interop clean
