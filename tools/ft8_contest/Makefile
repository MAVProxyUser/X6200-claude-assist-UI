# Host test of the FT8/FT4 messages ft8_lib does not unpack (src/ft8/contest_unpack.c): ARRL Field Day (0.3, 0.4),
# the WSPR-style ones (0.6), ARRL RTTY Roundup (i3=3) and the EU VHF contest (i3=5), as WSJT-X 2.7 shows them.
# `make` builds and runs it under ASan and UBSan, `make verbose` prints every case.
#
#   contest_test     each kind packed here as WSJT-X packs it (packjt77.f90, written again from the Fortran), unpacked
#                    and compared with WSJT-X's text: its examples, every section, multiplier, report, power, prefix
#                    and suffix form, serials, grids, the fields WSJT-X does not show, 2 million random messages
#   qso_texts        those texts through the QSO sequence (src/ft8/qso.cpp): none is a call, a grid or a report
#   make wav WAV=f   ft8_lib's own decoder (demo/decode_ft8.c, as it is) with the worker's unpacking
#                    (worker_unpack.c): a file's decodes before (CONTEST_UNPACK=0) and after, and against REF=
#                    (WSJT-X's decodes, one a line) if given. FT4 unless MODE= is empty: WSJT-X's FT4 sample
#                    000000_000002.wav (16 decodes, 7 of them RTTY Roundup exchanges)
#
# FT8LIB: gdyuldin/ft8_lib at the pin (73b0db86), where the x6200 build or host/build.sh (build-host/deps/src) put it.
TOP      := ../..
SRC      := $(TOP)/src
FT8LIB   ?= $(TOP)/build/x6200/src/ft8_lib
MODE     ?= -ft4
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS   := -std=gnu11 -Wall -Wextra -O1 -g $(SAN) -I build/inc -I $(SRC)/ft8
LIBFLAGS := -std=gnu11 -O1 -g $(SAN) -w -I $(FT8LIB)
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-switch -Wno-unused-parameter -Wno-implicit-fallthrough -O1 -g $(SAN) \
            -I $(SRC)/ft8
# ft8_lib's hashtable_init() clears a quarter of its table (security review R-05): ASan's fill would be calls
export ASAN_OPTIONS := malloc_fill_byte=0

UNPACK   := $(SRC)/ft8/contest_unpack.c $(SRC)/ft8/contest_unpack.h
LIB_MSG  := $(FT8LIB)/ft8/message.c $(FT8LIB)/ft8/text.c
LIB_ALL  := $(wildcard $(FT8LIB)/ft8/*.c $(FT8LIB)/common/*.c $(FT8LIB)/fft/*.c)

all: test

build/inc/ft8lib:
	mkdir -p build/inc
	ln -sfn "$(abspath $(FT8LIB))/ft8" $@

contest_test: contest_test.c $(UNPACK) $(LIB_MSG) | build/inc/ft8lib
	mkdir -p build/msg
	$(CC) $(LIBFLAGS) -c -o build/msg/message.o $(FT8LIB)/ft8/message.c
	$(CC) $(LIBFLAGS) -c -o build/msg/text.o $(FT8LIB)/ft8/text.c
	$(CC) $(CFLAGS) -o $@ contest_test.c $(SRC)/ft8/contest_unpack.c build/msg/message.o build/msg/text.o

qso_texts: qso_texts_test.cpp $(SRC)/ft8/qso.cpp $(SRC)/ft8/qso.h $(SRC)/ft8/utils.c $(SRC)/qth/qth.c
	mkdir -p build
	$(CC) -std=gnu11 -O1 -g $(SAN) -c -o build/utils.o $(SRC)/ft8/utils.c
	$(CC) -std=gnu11 -O1 -g $(SAN) -c -o build/qth.o $(SRC)/qth/qth.c
	$(CXX) $(CXXFLAGS) -o $@ qso_texts_test.cpp $(SRC)/ft8/qso.cpp build/utils.o build/qth.o -lm

# ft8_lib's demo decoder, its ftx_message_decode() the worker's (worker_unpack.h); Hound's 0.1 with it, as worker.c
decode_contest: worker_unpack.c worker_unpack.h $(UNPACK) $(SRC)/ft8/hound.c $(LIB_ALL) | build/inc/ft8lib
	mkdir -p build/lib
	for f in $(LIB_ALL); do $(CC) $(LIBFLAGS) -c -o build/lib/$$(basename $$f .c).o $$f || exit 1; done
	$(CC) $(LIBFLAGS) -include worker_unpack.h -I $(FT8LIB)/common -c -o build/demo.o $(FT8LIB)/demo/decode_ft8.c
	$(CC) $(CFLAGS) -I $(FT8LIB) -o $@ worker_unpack.c $(SRC)/ft8/contest_unpack.c $(SRC)/ft8/hound.c \
		$(SRC)/ft8/utils.c $(SRC)/qth/qth.c build/demo.o build/lib/*.o -lm

test: contest_test qso_texts
	./contest_test
	./qso_texts

verbose: contest_test qso_texts
	./contest_test -v
	./qso_texts -v

wav: decode_contest
	@test -n "$(WAV)" || { echo "make wav WAV=file.wav [REF=decodes.txt] [MODE=]"; exit 1; }
	@echo "== before: ft8_lib's unpacking and Hound's"
	@CONTEST_UNPACK=0 ./decode_contest $(MODE) "$(WAV)" 2> /dev/null | sed -n 's/.*~  //p' | tee build/before.txt
	@echo "== after: and contest_unpack()"
	@./decode_contest $(MODE) "$(WAV)" 2> /dev/null | sed -n 's/.*~  //p' | tee build/after.txt
	@if [ -n "$(REF)" ]; then \
		sort build/after.txt > build/after.sorted; sort "$(REF)" > build/ref.sorted; \
		echo "== against $(REF): $$(comm -12 build/after.sorted build/ref.sorted | wc -l | tr -d ' ') of" \
			"$$(wc -l < build/ref.sorted | tr -d ' ') as WSJT-X shows them; not decoded here:"; \
		comm -13 build/after.sorted build/ref.sorted; \
		echo "== shown here, not by WSJT-X:"; comm -23 build/after.sorted build/ref.sorted; \
		test -z "$$(comm -23 build/after.sorted build/ref.sorted)"; \
	fi

clean:
	rm -rf build contest_test qso_texts decode_contest *.dSYM

.PHONY: all test verbose wav clean
