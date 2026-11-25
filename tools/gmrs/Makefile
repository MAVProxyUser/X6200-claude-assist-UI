# Host test of src/cfg/gmrs.c, GMRS's channels (the owner's licence, 2026-11-25): `make` builds it and runs it.
# - gmrs_test: the 22 channels against the rules written out (frequencies, bandwidths, power, the repeater inputs of
#   15-22), their names, the scan's order, the lookups, the CTCSS privacy codes (each one of ctcss.c's tones) and the
#   DCS codes; and receive only: every channel and input refused by ham_tx_check (a transverter, the switch on),
#   tx_ranges_check and tx_ranges_sdr_check with GMRS's own reason.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -pthread -I $(SRC)
DEPS   := $(SRC)/cfg/gmrs.c $(SRC)/cfg/gmrs.h $(SRC)/cfg/ham_bands.c $(SRC)/cfg/ham_bands.h $(SRC)/cfg/tx_ranges.c \
          $(SRC)/cfg/tx_ranges.h $(SRC)/ctcss.c $(SRC)/ctcss.h

all: test

gmrs_test: gmrs_test.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ gmrs_test.c $(SRC)/cfg/gmrs.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/tx_ranges.c $(SRC)/ctcss.c -lm

test: gmrs_test
	./gmrs_test

clean:
	rm -f gmrs_test

.PHONY: all test clean
