# x6x00-modem: BinModem's fax modems in C, for a link between the two radios

The owner asked for [BinModem](https://github.com/CasualArclamp/BinModem), a dial-up softmodem in Rust, with one
end of the conversation on the X6100 and the other on the X6200, the exchange visualised live on the Mac, and no
Rust on the radios ("port it so we aren't moving rust here"). This directory is that port.

**License.** GPL-3.0-or-later, as BinModem is: these files are a port of its code and carry its license. They are
a separate program (the radio end is its own executable, `x6x00-modem`). The GUI keeps its own license
(LGPL-2.1).

## Why the fax modems

A fax call is half duplex, which suits two transceivers on one link: only one of them transmits at a time. The
fax modems are BinModem's half-duplex ones:

- **V.21 channel 2:** 300 bit/s FSK, used for the negotiation.
- **V.27 ter:** 2400 and 4800 bit/s.
- **V.29:** 7200 and 9600 bit/s.
- **T.30's procedure:** capabilities, a rate chosen, a training check at that rate, then a step down until it
  passes.

BinModem's dial-up modems (V.22bis to V.90) are full duplex. They need both ends transmitting at once, which two
half-duplex radios cannot do.

## What is ported, and how closely

| Part | Source in BinModem | Here |
|---|---|---|
| Butterworth sections, one-pole, oscillator, root-raised-cosine | `dsp/src/{filter,nco,shaping}.rs` | `dsp.c`, same formulas and constants |
| Least squares (normal equations, Cholesky) | `dsp/src/complex.rs` | `dsp.c` |
| V.21 transmitter and receiver, FSK detector | `datapump/src/{v21,bell103}.rs`, `dsp/src/fsk.rs` | `v21.c`, line for line |
| V.27 ter transmitter (turn-on, scrambler, Tables 1 and 2) | `datapump/src/v27ter.rs` | `v27ter.c`, line for line |
| V.29 transmitter (synchronizing signal, conditioning generator, V.32 scrambler, Table 2) | `datapump/src/v29.rs`, `v32.rs` | `v29.c`, line for line |
| Phase and amplitude receiver | `dsp/src/qam/*`, `v27ter/receiver.rs`, `v29.rs` | `qamrx.c`: BinModem's method, rewritten |

The receivers keep BinModem's method:

1. Find the burst's turn-on against the symbols the transmitter is known to send.
2. Solve a 31-tap half-symbol equaliser to it by least squares.
3. Follow the burst by its decisions, turning an error into a timing error by the equaliser output's slope.

They leave out what BinModem needs for VoIP and real fax machines: jitter-buffer slips, finding a turn-on that
started unheard, and short trainings. They add what a radio link has and a telephone line does not:

- The two radios' carriers some hertz apart. The offset is measured as the burst is found, then followed.
- The two radios' audio clocks free-running against each other. A second-order clock loop follows that drift.

## Checked against BinModem

`interop.sh` compares the two implementations at every rate, at 24 kHz (the radios' TCI audio) and 16 kHz.
Results (2026-11-21, BinModem ea61ee0):

| | 2400 | 4800 | 7200 | 9600 |
|---|---|---|---|---|
| C transmitter's samples against BinModem's | within 1.7e-6 | within 1.6e-6 | within 1.9e-6 | within 1.7e-6 |
| C receiver on BinModem's transmitter, 9600 random bits | exact | exact | exact | exact |
| BinModem's receiver on the C transmitter | exact | exact | exact | exact |

The C transmitter takes its pulse from a table, linear between 1024 points a symbol, rather than evaluating the
root-raised cosine every sample. That table is the whole of the 1e-6 difference. On the radio's processor it
saves most of a core's third.

The BinModem side of the check is a small harness, `crates/datapump/examples/faxpump.rs` in a BinModem checkout.
It is not part of either project; it lives only on the Mac:

- `faxpump tx RATE BITS.txt OUT.f64 FS` writes BinModem's transmitter output as raw f64 samples.
- `faxpump rx RATE IN.f64 FS` prints BinModem's receiver's bits as 0/1, with 0.2 s of quiet line before and
  0.5 s after.

Build it with `cargo build --release --example faxpump -p datapump`.

## Through a simulated SSB link

`make test` runs each rate through `channel.c`:

- the radios' audio filters (300 to 2700 Hz on transmit, 250 to 2800 on receive, 4th-order each);
- a carrier offset, applied as a single-sideband shift;
- the two sample clocks apart;
- noise.

Every bit must come back. Results (20,000 random bits per burst; SNR measured over the whole 12 kHz band, with
12 Hz offset and 50 ppm):

| Rate | Clean down to | Equaliser trained to (at 30 dB) |
|---|---|---|
| V.27 ter 2400 | 8 dB | 34 dB |
| V.27 ter 4800 | 13 dB | 31 dB |
| V.29 7200 | 15 dB | 15 dB |
| V.29 9600 | 22 dB | 13 dB |

V.29's training stops near 14 dB however clean the line is, and that is the band. V.29 puts its carrier at
1700 Hz with 2400 symbols a second, so its spectrum reaches 2900 Hz before roll-off. A 2700 Hz filter cuts into
the part the equaliser needs. The X6100's transmit filter ends at 2850 Hz (its Hilbert), so V.29 on the radios
sits at that edge. V.27 ter 4800 (1000 to 2600 Hz) fits inside it comfortably.

Offsets of up to ±60 Hz and clock drift of ±120 ppm decode with no errors. The receiver reports both. With the
simulated link set to -60 Hz and -120 ppm, it read -60.2 Hz and -121 ppm.
