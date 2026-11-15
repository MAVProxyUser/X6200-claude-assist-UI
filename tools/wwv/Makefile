# Host test of src/wwv.c (the WWV/WWVH time code): make, then make check
SRC := ../../src
CFLAGS := -std=gnu11 -O2 -Wall -Wextra -I $(SRC)

wwv_test: wwv_test.c $(SRC)/wwv.c $(SRC)/wwv.h
	$(CC) $(CFLAGS) -o $@ wwv_test.c $(SRC)/wwv.c -lm

check: wwv_test
	python3 make_wwv.py synth /tmp/wwv_synth.f32 2026 319 21:30 3 && ./wwv_test /tmp/wwv_synth.f32 2026 319 21:30 3
	python3 make_wwv.py capture /tmp/wwv_real.f32 && ./wwv_test /tmp/wwv_real.f32 2026 312 04:20 2

clean:
	rm -f wwv_test
