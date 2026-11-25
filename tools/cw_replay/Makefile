# Offline CW decoder replay (cw_replay.cpp, replay.py): the GUI's receiver (src/cw_decoder) built for this computer on
# the host build's liquid-dsp 1.4.0 (build-host/deps, as host/build.sh leaves it; Homebrew's 1.8 differs; CI's DEPS
# /usr/local). `make` builds it and runs test.sh, the host check: a recording of the X6100's audio of the X6200's CW
# over the bench cable must copy with the CW filter from 0 Hz (TONE_MIN_HZ). By hand: `python3 replay.py
# RECORDING.wav [LO HI [FLOOR_DB]]`. Found the CW filter from 0 Hz that locked it on 20 Hz (2026-11-23).
# And a text after a quiet: the lock taken back by the station held before (2026-11-25).
TOP  := ../..
S    := $(TOP)/src/cw_decoder
DEPS ?= $(TOP)/build-host/deps
SRCS := $(S)/morse_text.cpp $(S)/tone_finder.cpp $(S)/tone_lock.cpp $(S)/key_detector.cpp $(S)/element_timer.cpp \
        $(S)/receiver.cpp

all: test

cw_replay: cw_replay.cpp $(SRCS) $(S)/morse_code.c $(wildcard $(S)/*.h)
	$(CC) -c -O1 -I $(S) $(S)/morse_code.c -o morse_code.o
	$(CXX) -std=c++17 -O1 -I $(S) -I $(DEPS)/include -w cw_replay.cpp $(SRCS) morse_code.o \
	    -L $(DEPS)/lib -lliquid -Wl,-rpath,$(abspath $(DEPS)/lib) -o $@

test: cw_replay
	./test.sh

clean:
	rm -f cw_replay morse_code.o

.PHONY: all test clean
