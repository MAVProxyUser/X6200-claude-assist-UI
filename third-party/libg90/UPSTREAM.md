# libg90 (vendored)

The Xiegu G90 body link (the DE-9 cable between the G90's head and its body): the frame codec and the serial
link, by Chris Lawson, for the GSOC head driving a G90 (`HW_BACKEND=g90`, src/hw/g90).

Source: https://github.com/cjlawson02/libg90
Commit: `4eefd6e9fa43c554c1de502c2b3d2ed6b08d6f39` (2026-11-14, "Pick the head-frame profile from the body's
reported firmware"), the one cjlawson02's gsoc/g90-probe pins.
License: MIT (see `LICENSE`)

Copied from that commit: `CMakeLists.txt`, `cmake/`, `lib/`, `LICENSE`, `README.md`. Left out: `cli/` (g90ctl),
`tests/`, `docs/`, `scripts/` and the repository's tooling; their CMake options are off here
(LIBG90_BUILD_CLI, LIBG90_BUILD_TESTS in src/CMakeLists.txt). This file is ours. No local patches.
