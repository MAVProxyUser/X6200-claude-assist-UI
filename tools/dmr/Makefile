# Host test of the DMR app (APP > DMR): `make` builds and runs them; nothing contacts a DMR network.
# - (upstream's dmr_ambe_test, src/dmr_ambe.c over mbelib, is not in this fork: no mbelib here, the app's voice goes
#   through the DVSI USB-3000 (tools/dsdfme/mbe_dvsi, whose own tests are tools/dsdfme's); the owner: "hard no on the
#   mbelib, USB-3000 is hard requirement")
# - dmr_proto_test: src/dmr_proto.c, what the app reads of its helper (the records, the state lines, who talks, the
#   last heard), with ASan and UBSan.
# - dmr_test.py: rootfs/usr/sbin/x6100-dmr, the helper, against a fake master on 127.0.0.1 (fake_master.py) that
#   answers as the MMDVM hotspot protocol does: the login, the options, the keepalive, the logout, the voice frames
#   it passes on and the ones it drops, the refusals, the secrets kept out of everything it writes, and that it
#   sends nothing but those packets (no voice, no data). Any other address fails the test. The listening log it
#   keeps (heard.tsv): a line a call, the short ones left out, the full log set aside.
# - dmr_web_test.py: the web DMR page's Listening log over such a log: the list, the search, the ADIF of listening
#   reports and the CSV. BOTTLE: a folder with bottle.py, by default the radio's own from a buildroot tree built next
#   to this repository, as in tools/flights; without it this one is skipped.
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)
SAN    := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

all: test

dmr_proto_test: dmr_proto_test.c $(SRC)/dmr_proto.c $(SRC)/dmr_proto.h
	$(CC) $(CFLAGS) $(SAN) -o $@ dmr_proto_test.c $(SRC)/dmr_proto.c

dmr_proto_dump: dmr_proto_dump.c $(SRC)/dmr_proto.c $(SRC)/dmr_proto.h
	$(CC) $(CFLAGS) -o $@ dmr_proto_dump.c $(SRC)/dmr_proto.c

test: dmr_proto_test dmr_proto_dump
	./dmr_proto_test
	python3 -I -B dmr_test.py
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B dmr_web_test.py

clean:
	rm -f dmr_proto_test dmr_proto_dump *.o

.PHONY: all test clean
