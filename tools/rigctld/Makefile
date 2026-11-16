# Host test of src/rigctld.c, the network's Hamlib NET rigctl (#178): `make` builds it against a pretend radio and runs
# it. The sources are copied to build/ first: a quoted #include looks beside the including file before the -I paths,
# and the shims in stub_include must win over src/ (board.h and radio_types.h come as they are)
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DBOARD_X6200 -I build -I stub_include

all: test

build/.copied: $(SRC)/rigctld.c $(SRC)/rigctld.h $(SRC)/board.h $(SRC)/radio_types.h
	mkdir -p build && cp $^ build/ && touch $@

rigctld_test: rigctld_test.c build/.copied
	$(CC) $(CFLAGS) -o $@ rigctld_test.c build/rigctld.c -lm

test: rigctld_test
	./rigctld_test

clean:
	rm -rf build rigctld_test

.PHONY: all test clean
