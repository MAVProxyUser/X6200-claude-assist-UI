# voacapl (built from source, on hfcast-engine)

The program Pat runs for the VOACAP column of its Winlink gateway list (Connect, the gateway list's predicted path
quality; `pat rmslist -q` from a shell), in place of the Fortran `voacapl`, which the radios' build does not compile
(#153, asked 2026-11-02 by km4gkd; done 2026-11-18).

- **Engine**: hfcast-engine by Jonathan McSweet, VOACAP translated into Rust and checked cell by cell against the
  Fortran.
  Source: https://github.com/jonathanmcsweet/hfcast-engine
  Commit: `bf34e8f9f375e190ed43fcac19f25d800992e0e6` (2.14.0, 2026-10-27), cloned by `build.sh`, the commit checked.
  License: Apache-2.0 (its `LICENSE` and `NOTICE` go with the binary to `/usr/share/x6100/licenses/voacapl`).
  Built with its `embedded-coefficients` feature: the ionospheric coefficients and the default antennas compiled in,
  so no `itshfbc` tree on the radio.
- **Ours**: `src/main.rs` (Pat's deck in, hfcast's method-30 listing out with Pat's deck echoed at its top, where
  Pat's parser reads back its hour and frequency), `build.sh`, the host check (`Makefile`, `voacap_test.py`),
  this file. LGPL-2.1-or-later.
- **Test data**: `testdata/`, Pat's own (MIT, Martin Hebnes Pedersen LA5NTA; its README).

## What the data is, and its status

hfcast's notes (`NOTICE`, `docs/licence.md`, `embedded/README.md` in its tree; `hfcast-licence.md` beside the
binary) say it in full. In short, what this binary carries, 544 KB of coefficients and 16 KB of antennas:

- About 195 KB of the coefficients, the antennas and the version file are NTIA/ITS work, a US Government work,
  "not subject to copyright protection in the United States", as VOACAP itself, with its disclaimer.
- About 350 KB is CCIR data (Reports 322 and 340: the atmospheric noise and the foF2 and M(3000)F2 maps). ITU-R
  Study Group 3 publishes the same coefficients in its P.533/P.372 reference software "free from any copyright
  assertions" for implementers of its Recommendations; VOACAP is not one of those, so the position is "the rights
  holder publishes this data freely" rather than a licence written for this use.
- The URSI-88 foF2 map is in no build (hfcast leaves it out).

hfcast's own crate on crates.io ships without the coefficients for that reason, and its repository carries them.
Using them on one's own radio is not redistributing them; an image or update bundle given to others is.

**Decided 2026-11-18 by the owner ("Ship it")**: the image and the update bundle carry voacapl with them, on the
reading above (the ITU's own free publication of the CCIR data, NTIA/ITS's US Government status for the rest), with
hfcast's NOTICE and licence notes beside the binary. If that reading is ever shown wrong, the fallback is hfcast's:
build without `embedded-coefficients` and give voacapl an itshfbc tree the owner supplies.

## How Pat runs it

`x6100-winlink` sets Pat's `prediction.engine` to `voacap` with `/usr/bin/voacapl` and `/usr/share/x6100/voacapl` (a note
only: Pat checks the folder is there) when the program is installed, else `disabled` as before; Pat with
`GOMAXPROCS=2`, so two predictions at a time, each at Pat's nice 10. One prediction (all 24 hours of one frequency)
takes about 50 ms on an X6200; 243 gateways on 40 m about 10 s. The power: Pat asks at a fixed 50 W, voacapl caps it
at `X6100_VOACAP_W` (7 W unless set).
