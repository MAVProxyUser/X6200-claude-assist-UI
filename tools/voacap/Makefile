# Host check of tools/voacap's voacapl (Pat's VOACAP column on hfcast-engine): `make` builds it for this computer with
# its own cargo, hfcast cloned at build.sh's pinned commit into build/voacap/hfcast-engine (a clone at another commit
# is fetched and checked out, never removed), and runs voacap_test.py (Pat's nine Fortran listings and its sample
# deck, testdata/). The radios' binary is build.sh's.
TOP    := ../..
HF     := $(TOP)/build/voacap/hfcast-engine
COMMIT := $(shell sed -n 's/^HFCAST_COMMIT=\([0-9a-f]*\).*/\1/p' build.sh)
URL    := $(shell sed -n 's/^HFCAST_URL=\(.*\)/\1/p' build.sh)

all: test

test:
	@command -v cargo > /dev/null || { echo "cargo: command not found (needs Rust's cargo)"; exit 1; }
	@[ -d $(HF)/.git ] || git clone --quiet $(URL) $(HF)
	@[ "$$(git -C $(HF) rev-parse HEAD)" = "$(COMMIT)" ] || { git -C $(HF) fetch --quiet origin && \
	    git -C $(HF) -c advice.detachedHead=false checkout --quiet $(COMMIT); }
	CARGO_TARGET_DIR=$(TOP)/build/voacap/target cargo build --release --locked --quiet
	python3 -B voacap_test.py $(TOP)/build/voacap/target/release/voacapl

.PHONY: all test
