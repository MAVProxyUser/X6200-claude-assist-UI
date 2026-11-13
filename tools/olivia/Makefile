# Host test of the Olivia and Contestia modem (src/olivia*.c): `make` builds and runs it. Golden hashes of what Olivia
# sends, then text through the modulator, noise and a tuning error into the receiver. No radio, no LVGL.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -g -I $(SRC) -I .
# The modulator alone without fused multiply-add: the golden audio hashes were taken without it (x86). FMA (arm64,
# and the radio's Cortex-A7 with VFPv4) changes the audio in the last bit, nothing on the air, but enough to flip a
# few samples rounded to 1/4096 and so every audio hash. The receiver keeps the compiler's FMA, as on the radio:
# the clean Contestia 16-tone failure fixed in 147fb59 showed only with it.
MOD_CFLAGS := -ffp-contract=off
SRCS   := olivia_test.c $(SRC)/olivia.c $(SRC)/olivia_demod.c $(SRC)/olivia_rx.c
DEPS   := $(SRCS) olivia_mod.o olivia_golden.inc $(wildcard $(SRC)/olivia*.h)

all: test

olivia_mod.o: $(SRC)/olivia_mod.c $(wildcard $(SRC)/olivia*.h)
	$(CC) $(CFLAGS) $(MOD_CFLAGS) -c -o $@ $<

olivia_test: $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) olivia_mod.o -lm

test: olivia_test
	./olivia_test

clean:
	rm -f olivia_test olivia_mod.o

.PHONY: all test clean
