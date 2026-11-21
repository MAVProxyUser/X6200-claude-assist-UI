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

## On the radios

Each radio runs one end, `x6x00-modem`. It works through the GUI's own TCI server on that radio:

- 24 kHz int16 mono audio;
- keying with `trx:0,true,tci`;
- an audio frame sent for every TX_CHRONO.

The Mac drives both ends with `tools/bench/bench.py radios-modem A B`, which:

- puts both radios on the band's frequency in USB-D at 0.1 W, through every bench gate;
- arms TX with the Sentry's A/B on its page A;
- starts both ends and holds the interlock, sending each end "may 1" only while the other radio's own GUI state
  says it is off the air (and the bench's Watch stops everything on two radios on the air, an over too long, a
  bad SWR or more power than set);
- serves the live view at http://127.0.0.1:8765/ (`tools/bench/modem_view.html`);
- puts both radios back afterwards (`~/.config/x6x00-bench/modem_was.json` survives an interrupt).

An end with no Mac connected, or a "may" more than 1.5 s old, does not transmit, and a "may 0" mid-transmission
unkeys it and stops it. A second controller connecting is turned away.

`tools/modem/build.sh install` puts a test build in `/tmp` on both radios. The image's own comes from buildroot's
`x6x00-modem` package, and the update bundle carries it.

### What the radios taught it (2026-11-21, X6100 and X6200 through 80 dB)

- **TCI latency.** TCI audio reaches the air about 0.5 s after the key. At the unkey, the last 0.3-0.5 s of audio
  is still in the X6100's transmitter path, and 1.2-1.4 s in the X6200's. Each end therefore keys 0.8 s of
  silence before its audio and holds its own tail after it: 0.6 s on the X6100, 1.8 s on the X6200.
- **V.21 channel.** The X6100's receiver takes steady tones from about 1.5 kHz up out of its audio within 0.3-0.9
  s, while its S-meter holds. Measured with 3 s tones from the X6200:

  | Tone | Heard at the X6100 |
  |---|---|
  | 1000 Hz | 3 s |
  | 1250 Hz | 3 s |
  | 1500 Hz | 0.56 s |
  | 1650 Hz | 0.37-0.51 s |
  | 1850 Hz | 0.93 s |
  | 2200 Hz | none |

  The X6200 hears all of them for the whole 3 s from the X6100, so the cause is on the X6100's side. It does this
  in USB as in USB-D, with the GUI's DNF, auto-notch, NR and NB all off and no PulseAudio processing, so it is in
  its base or codec. T.30's V.21 channel 2 (1650/1850 Hz) lost its preamble to it, so the radio link runs its
  frames on V.21 channel 1 (980/1180 Hz): `--v21 1`, the default.
- **X6200 TCI power.** The base's ALC does not hold the power setting, and TCI audio skips the GUI's drive loop
  (`tx_level.c`). -20 dBFS gave 1.1 W at a 0.1 W setting, and the Watch stopped it. The X6200 end sends at
  -32 dBFS.
- **X6100 TCI level.** The GUI levels TCI audio to -20 dBFS, where the ALC reads 10 at 0.1 W, compressed. The
  X6100 end sends at -44 dBFS, which the levelling takes to -32.
- **AGC.** The receiving radio's AGC holds its noise within about 8 dB of a burst. A burst can't be found by its
  level rising, so a training check is looked for where the DCS before it says it will be, and the line counts
  as busy from 6 dB over its noise.
- **Processor time.** On the R16 a 0.45 s search for a training check takes about 0.3 s, and an ordinary burst's
  find and train about 90 ms.

### Where it stands (2026-11-21 00:55 local)

**The first transfer over the radios.** 2048 bytes went from the X6100 to the X6200 at 0.1 W through 80 dB.
Every byte was checked against the sender's pattern at the far end. The run:

- CAP both ways on V.21 channel 1, then DCS and the training check;
- FTT at 9600, 7200 and 4800, then CFR at 2400, the check clean (0 of 2880 bits wrong);
- the block of 8 frames: frame 0 was lost (the equaliser just out of training) and sent again after a PPR;
- a lost MCF covered by the sender's retry, then EOP and MCF.

It took 91.3 s, 179 bit/s overall. The data itself was on the air for 10.1 s, at 2400 bit/s; the rest was the
step-down from 9600, V.21 control (4.2 s) and the turnarounds.

At 2400 the equaliser trains to 10-12 dB and the received symbols run at 11-13 dB SNR. The carriers are 7.5 Hz
apart and the clocks 11-20 ppm. 4800 comes through at about 9 % bit errors, and 7200 and 9600 do not train (4 dB).
Through filters like the radios' the simulation gets 30 dB, so something on the radio path costs about 20 dB. The
suspects are the X6100's ALC and the receiving radio's AGC and noise.

`rxfast FILE RATE` runs the fast receiver over an end's recording and prints the HDLC frames it decodes.
`rxfile FILE` does the same for V.21.
