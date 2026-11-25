# Host test of CISA's NIFOG on the radio: the index the GUI reads (src/nifog.c, through build/nifog_idx) against the
# web page's reading of it (webserver nifog_web.py), the committed index (nifog.idx) itself, render.py's pages (a
# made-up page, no pdftoppm needed) and the web configurator's /nifog, /nifog.pdf and /nifog/page/N through bottle's
# WSGI app. See nifog_test.py. `make` runs it; its files go in build/.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository, as in tools/apps_web; without one the web cases are skipped.
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

test:
	rm -rf build && mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O1 -I $(TOP)/src -o build/nifog_idx nifog_idx.c $(TOP)/src/nifog.c
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B nifog_test.py

clean:
	rm -rf build

.PHONY: all test clean
