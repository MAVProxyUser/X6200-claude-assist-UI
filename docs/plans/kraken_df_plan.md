# Direction finding (DF) with a KrakenSDR: plan for approval

Status: **approved 2026-11-17 (D1); Phase 0 under way** (results in 5.1). The engine's device-free core is in
`tools/rtlspec/df*.c`; nothing on the radio has changed.
Read for it: Heimdall DAQ (`krakenrf/heimdall_daq_fw`, branch development, 58c4068), the DoA software
(`krakenrf/krakensdr_doa`, 2e1c4e6), KrakenRF's Antenna Array Size Calculator (xlsx), and ours (`tools/rtlspec/`,
`src/sdr_pool.c`, `src/rtl_spectrum.c`, `src/app_sentry.c`, `src/adsb_rx.c`, `docs/MANUAL.md`).
File:line references to Heimdall are `H:file:line`, to the DoA software `K:file:line`.

## In short

- **Build our own, in C, in one process**: a daemon `x6x00-df` that opens the Kraken's five RTLs, keeps them aligned,
  calibrates them with the Kraken's noise source, picks out one signal and works out its bearing. About 2,500 lines of
  C beside `x6x00-rtlspec`, plus page **I** (`src/app_df.c`) and a new owner in the SDR pool.
- **Kraken's stack cannot run here as it is**: five processes (C + Python), ZeroMQ, shared memory, numba/scipy/
  scikit-rf, Dash, node.js; 40 MiB per data frame, about 300 MiB of buffers before Python itself; the radio has 1 GB
  and 87 MB free on its root file system. What it really needs fits in ten jobs (section 1.4).
- **Plain RTL-SDRs cannot do phase DF** without a shared clock (a soldering job on every stick) and a shared noise
  source. "Enough RTL-SDRs" on their own is not enough. The Kraken first.
- **The engine**: MUSIC for the bearing, Bartlett for a 0-100 % "fit" (confidence) and as the fallback. The maths per
  bearing is tiny (a 5x5 matrix, 1.4 ms on the radio); the cost is picking the signal out of five streams:
  **measured 45 % of one core at 1.024 MS/s** (23 % at 0.25, 89 % at 2.4 MS/s).
- **The first test decides the sample rate**: how often a sample is lost on the radio's USB with five streams. One
  lost sample on one channel ruins every bearing until the next calibration.
- **Decisions for the user** are in section 6.2.

---

## 1. The minimal DAQ

### 1.1 What the Kraken gives, and what software has to fix

The KrakenSDR is five RTL2832U + R820T2 receivers behind one internal USB hub (an SMSC USB2517, self-powered), all
clocked from one reference, with a noise source and switches in front of the five inputs.

The shared clock makes the five LOs and the five ADCs run at exactly the same frequency. It does **not** make the
channels line up. Every time the streams start, and every time the tuners retune, four things are new:

| Offset | Cause | Size | Changes when |
|---|---|---|---|
| Whole-sample delay | each stick's USB stream starts at its own moment | up to thousands of samples | each start; each lost sample |
| Fractional delay | each RTL2832U's resampler phase at start | 0-1 sample | each start |
| Phase | each R820T2 PLL locks at its own phase; the RTL's own NCO | any angle | each retune; slow drift with temperature |
| Amplitude | gain spread between tuners | a few dB | gain change; slowly |

All four are measured at once with the **noise source**: one noise signal fed into all five channels at once is the
same signal everywhere, so any delay, phase or level difference between the channels is the receivers' own.

- Noise source switch: **GPIO 0 of CH0** (the RTL2832U with serial 1000), set with
  `rtlsdr_set_bias_tee_gpio(dev, 0, on)` (H:rtl_daq.c:817; the control channel is found by serial,
  `ctr_channel_serial_no = 1000`, H:rtl_daq.c:474-504).
- The five antenna bias-Ts are **GPIO 1-5 of the same CH0 chip** (`rtlsdr_set_bias_tee_gpio(ctr, m+1, on)`,
  H:rtl_daq.c:580).
- While the noise is on, KrakenRF's switches disconnect the antennas (KrakenRF's documentation; Heimdall only flips
  GPIO 0). Phase 1 measures the isolation.

### 1.2 What Heimdall actually does

Started by `daq_start_sm.sh` as `rtl_daq | rebuffer`, `decimate`, `delay_sync.py`, `hw_controller.py`, all at
`chrt -f 99`, after `sysctl kernel.sched_rt_runtime_us=-1`, `usbfs_memory_mb=0` and `drop_caches`.

| Module | Language | Its job | Verdict for us |
|---|---|---|---|
| `rtl_daq.c` (899 lines) | C | Opens the five RTLs by serial 1000+i, dithering off (`rtlsdr_set_dithering`, keenerd/Kraken librtlsdr only, :355), manual gain, one reader thread each with a barrier before `rtlsdr_read_async` (:418-419; 12 x 512 KiB buffers), waits until all five have filled the same block number, flags ADC overload (any byte 255), writes header + five blocks to stdout. Tuning, gain, AGC, noise and "sample-rate ppm" commands over ZeroMQ. | **Keep the ideas**, in our daemon: sequential opens, barrier start, block alignment, overload flags, GPIO 0 noise switch. Drop ZeroMQ, the stdout pipe, pigpio, the deprecated reconfigure path. |
| `rebuffer.c` | C | Re-chunks blocks into CPIs (default 2^20 samples, 0.44 s at 2.4 MS/s). | **Drop**: we work on the stream. |
| `fir_decimate.c` | C (Ne10/KFR) | u8 to float (`(x-127.5)/127.5`), optional FIR decimation (default ratio 1 = none). | **Replace** with a per-target channel filter in the engine. |
| `delay_sync.py` (822) | Python, numba | The calibration state machine (below). | **Keep the measurements, rewrite in C, drop the sample-rate slewing.** |
| `hw_controller.py` (706) | Python | Schedules noise bursts, sets a calibration gain by frequency, steps gain down on overload, control server on TCP 5001. | **Keep** burst scheduling, cal gain, overload step-down. Drop TCP. |
| `iq_server.c`, shared memory, 1024-byte IQ header | C, Python | Hands frames to the DoA side (TCP 5000 or shm A/B buffers with FIFOs). | **Drop**: one process, our own socket to the GUI. |
| `fir_filter_designer.py`, `ini_checker.py`, configs, unit tests | Python | Support. | Drop; our own tests on the Mac. |

**`delay_sync.py`, the heart of it** (state machine at H:delay_sync.py:501-760):

1. **Whole-sample delay** (`STATE_SAMPLE_CAL`, :527-583): FFT cross-correlation of 65,536 noise samples
   (`corr_size`) of each channel against CH0; the peak gives the delay; the peak must stand 20 dB above the value 100
   samples away (`min_corr_peak_dyn_range`, :89, :549-554) or the result is thrown away.
2. **It removes the delay in hardware**: it tells `rtl_daq` to run that stick's sample rate a few ppm fast or slow
   (`rtlsdr_set_sample_freq_correction_f`, a Kraken librtlsdr call) until the samples have slid into line, then sets
   it back (:559-577; gain table `INT_FS_TUNE_GAIN`, :100-104). This is the method of Mikko Laakso's 2019 thesis.
3. **Fractional delay** (`STATE_FRAC_SAMPLE_CAL`, `estimate_frac_delays`, :353-424): 1024-point FFT blocks, the
   phase of the cross-spectrum against CH0, a straight line fitted over +-0.4 of the band; the slope is the delay.
   Also removed by ppm slewing, until under 0.03 sample.
4. **Phase and amplitude** (`STATE_IQ_CAL`, `calc_iq_sync`, :286-351): the 5x5 correlation matrix of the noise, its
   strongest eigenvector; the correction is `1/v` normalised to CH0, amplitude from the channels' powers
   (`amplitude_cal_mode = channel_power`). Applied to every frame as one complex multiply per channel, after removing
   each channel's mean (DC) (`correct_iq`, :803-806). Re-done until the residual is under the tolerances (ini:
   phase 1 degree, amplitude ratio 2).
5. **Track** (`STATE_TRACK`, :693-747): in `cal_track_mode = 2` (Kraken default) the checks run **only on noise
   frames**. 10 failed checks (`maximum_sync_fails`; a good check takes one off the count) or any retune sends it
   back to step 1.

**When the noise is on** (`hw_controller.py`, :511-550): at start until locked, after every retune, and in track a
burst of `cal_frame_burst_size` = 10 CPIs every `cal_frame_interval` = 687 CPIs. At the Kraken defaults
(2^20 samples at 2.4 MS/s = 0.44 s per CPI) that is **4.4 s of noise every 5 minutes**; the DoA UI shows it as a
"recalibration interval" in minutes (K:callbacks/main.py:757). For the burst the gain is set from a frequency table
(gain 8.7 dB at 100 MHz up to 49.6 dB from 700 MHz up, :59) and restored afterwards. After every command `rtl_daq`
sends 5 dummy blocks (0.55 s).

**Weak points we should not copy**

- Between bursts nothing notices a lost sample: a slipped channel gives wrong bearings for up to 5 minutes.
- The DoA side only checks `frame_type == DATA` (K:kraken_sdr_signal_processor.py:375-377); the sync flags are
  never checked before a bearing is computed.
- Slewing the sample rate takes several frames per step; a full calibration takes seconds.
- Memory: one float frame is 5 x 2^20 x 8 B = 40 MiB, double-buffered in three shared-memory stages.

### 1.3 What the DoA side actually does (krakensdr_doa)

- **Algorithms** (K:SP:930-1038, 1 degree grid 0-359): Bartlett, Capon, MEM (from pyARGUS, installed with pip, not
  vendored), MUSIC (local: `P = 1/|a^H En En^H a|`, noise subspace = the M-1 smallest eigenvectors for one source),
  ROOT-MUSIC, TNA. Default **MUSIC, one source**. `R = X X^H / N` from the channel-filtered samples; no diagonal
  loading; decorrelation (forward-backward, spatial smoothing) off by default.
- **Steering vector** (K:SP:1621-1641): `a_m(theta) = exp(j2pi(x_m cos theta + y_m sin theta))`, x and y in
  wavelengths. UCA: `x_m = r cos(2pi m/M)`, `y_m = -r sin(2pi m/M)` (the calculator's layout). ULA: elements along y.
  Custom: x/y in metres divided by lambda.
- **Signal selection** (K:SP:151-181, 509-593): up to 16 "VFOs" (frequency + bandwidth, default 12.5 kHz) inside the
  2.4 MHz; a Welch spectrum of **channel 1 only** (4096 points) gives the squelch: DoA runs only if the strongest bin in
  the VFO is over "mean of the spectrum + 5 dB". Channel filter: FIR shifted to the VFO, decimate by `fs // bw`.
- **Bearing** = argmax of the spectrum, whole degrees. **Confidence** = peak-to-average of the spectrum in dB
  (`calculate_doa_papr`, K:SP:1703-1705).
- **ULA ambiguity**: scans 0-359 and blanks the back half on request ("Forward"/"Backward", K:SP:1023-1033).
- **No averaging over time**: each CPI (0.44 s) stands alone.
- **Cruft**: the Dash/Plotly web UI (about 5,600 lines of Python, port 8080), node.js middleware (8042/8021, relays to
  map.krakenrf.com, 801 vendored files), a php/miniserve file server, six output formats (Kerberos, Kraken App/Pro,
  RDF Mapper, "Full POST" which also asks ip.seeip.org for the public IP), `status.json` and `doa.xml` written every
  CPI, gpsd, TAK, recording. The core (about 2,650 lines) imports the UI's `variables.py`, which imports plotly.
- **Bugs seen** (not to repeat): the UCA radius uses a channel count of 4 until the first frame arrives
  (K:SP:144); `elif "ULA":` is always true (K:SP:1631); the phase-mode offset adds degrees to radians (K:SP:1614).

### 1.4 The bare minimum, and what `x6x00-df` does instead

What it takes to get a bearing out of a Kraken, and nothing else:

1. Find the Kraken: five RTLs with serials 1000-1004 behind one hub (**never by RTL number**: a plain stick with
   serial 00000001 sorts first, `sdr_rtl.c:56-79`, so with both plugged in the Kraken is RTL1-RTL5).
2. Open them **one at a time** (four `rtl_fm` opening at once left one with no samples, 2026-11-16), equal manual
   gain, RTL AGC off, tuner AGC off, **PLL dithering off**, all bias-Ts off, noise off.
3. Start all five streams behind a barrier; one reader thread per stick (librtlsdr's async read, 12 x 32 KiB as
   `sdr_rtl.c` already does: 1.9 MiB in all, inside the radio's 16 MB `usbfs_memory_mb`, so no need for Heimdall's 0).
4. Count bytes per stick; keep each stick's samples in a ring (0.25 s).
5. **Calibrate** (1.5): noise on, read one block, noise off.
6. **Align in software**: skip the whole-sample delay at the ring's read pointer. No sample-rate slewing, so no
   `rtlsdr_set_sample_freq_correction_f`, no multi-frame loop.
7. **Correct per signal, not per stream**: the fractional delay, phase and amplitude become one complex number per
   channel at the target's frequency (1.5), applied after the channel filter at the low rate.
8. Pick out the target (mix + decimating filter per channel), 5x5 correlation matrix, DoA (section 2).
9. Watch for lost samples (1.7) and re-calibrate on a slip, a retune, and on a timer.
10. Talk to the GUI on one socket (1.9).

### 1.5 Calibration, step by step (one noise block, about 0.15 s)

At 1.024 MS/s: noise on (GPIO 0), throw away 50 ms (switch, filters), keep 65,536 samples per channel (64 ms),
noise off, throw away 30 ms. The DF display holds its last bearing during the burst.

1. **Level check**: no channel clipped (bytes at 0 or 255 under 0.01 %) and noise at least 15 dB over the floor; else
   step the gain (all five together) and repeat. Start from Heimdall's frequency table.
2. **Whole-sample delay** `k_m` (only on the first burst after a start, or when the fit with the known delays fails):
   FFT cross-correlation against CH0 (the first 16,384 samples, all 65,536 if a peak is too low; the same 20 dB peak
   test as Heimdall). CPU on the radio: 145 ms for a calibration with the search, 58 ms without (Phase 0).
3. **Cross-spectrum** of each channel against CH0, averaged over 64 blocks of 1024 points (after removing `k_m` and the
   mean): `S_m(f) = mean(X_m(f) X_0*(f))`.
4. **Fit** `arg S_m(f) = phi_m + 2 pi f tau_m` over +-0.4 of the band: the slope is the fractional delay `tau_m`, the
   intercept the phase `phi_m`. A fit residual over 2 degrees rms means a bad block: try again. A slope over 1 sample
   means `k_m` is wrong: back to 2.
5. **Amplitude** `g_m = sqrt(P_0 / P_m)` from the channels' noise powers.
6. **Store** a per-channel table `c_m(f) = g_m exp(-j(phi_m + 2 pi f tau_m))`, and also the measured
   `S_m(f)/|S_m(f)|` per 1024-point bin, so that filter ripple between sticks can be corrected bin by bin later if the
   bench shows it matters.
7. **Report**: delays, phases, gains, peak margin, how far each value moved since the last burst (that is the drift).

Why the per-signal correction is enough: after the whole-sample shift, the leftover delay is under one sample. For a
signal at offset `f` in the band it is a phase `2 pi f tau_m`, which `c_m(f)` removes. Across a 25 kHz channel at
1.024 MS/s, half a sample is 4.4 degrees edge to edge, +-2.2 degrees about the centre, which the centre correction
cancels; a 4-tap fractional-delay filter at the low rate can remove even that if tests ask for it.

### 1.6 When to calibrate

| Trigger | Why |
|---|---|
| Start; every retune | new delays (start), new PLL phases (retune). Retune + calibrate under 0.5 s. |
| Slip suspected (1.7) | the channel is off by whole samples. |
| Gain change | if the bench shows gain steps move the phases differently per channel (Phase 1, test 1f). |
| Timer, default 2 minutes, set on the radio | slow phase drift. **Waits for the squelch to close** (up to 30 s past due), so a bearing is not cut by a burst. |
| By hand (F4 on page I) | |

Heimdall: 5.5 s blind every 5 minutes. Ours: 0.15 s every 2 minutes, in a gap between transmissions. The default
interval comes from the drift measured in Phase 1 (test 1d).

### 1.7 Lost samples (slips)

A lost sample is silent: the RTL2832U's buffer overflows inside the stick and the stream just goes on, one or more
samples short. The 2026-11-16 measurement (B's daemon + four `rtl_test` through the Kraken's hub, about 24 MB/s) lost
**0-3 samples per million**; for DF each loss event on any channel is a broken array until the next calibration.

Our checks, cheapest first:

1. **Byte counts**: the five sticks must deliver the same number of bytes; a short or failed USB transfer is a slip.
2. **One channel out of line** (4 elements or more): with the target 12 dB up, each channel is left out in turn and
   the rest fitted (Bartlett, any bearing). If leaving one out brings the fit to 0.9 or more, and 0.1 over the fit of
   all of them, that channel has slipped (or its cable or antenna is bad): **the gate shuts at once**, and after 0.3 s
   of it a calibration is asked for (at most once in 10 s). Page I names the channel.
3. **Fit collapse**: target 15 dB up and the fit under 0.5: the same.
4. **Timer** (1.6): bounds the damage when there is no signal to check against.
5. Every burst re-checks `k_m`: a calibration with the known delays (fast, 1.5) fails on a slipped channel, and the
   full search runs.

Found in Phase 0 (test 0b): the fit alone is not enough. 256 samples lost at 150 kHz off centre turn that channel by
exactly 180 degrees, and the five still fit a plane wave from the other side (fit 0.68): the plan's first detector
showed a bearing 178 degrees off. Leaving one out catches it in 0.25 s and names the channel; it also catches a slip of
a single sample (53 degrees on one channel, fit still 0.85). With 3 elements leaving one out proves nothing (any two
fit some bearing), so a UCA-3 or ULA-3 relies on 3. and the timer.

The first bench test (Phase 0, 0a) counts slip events per hour at each sample rate, which sets the default rate.

### 1.8 Gain, overload, the stick's own settings

- One gain for all five (manual). No AGC anywhere: independent AGCs would move gains and phases independently.
- Overload: any channel with bytes at 0/255 over 0.01 % of a block: flag OVL on page I, all five down one step,
  calibrate. Up again only by hand (or after 10 s clean, if the user wants auto).
- DC: remove each channel's mean before use (as Heimdall does).
- **librtlsdr**: our build is the RTL-SDR Blog fork (`buildroot/br2_external/package/rtlsdr-blog`). It has
  `rtlsdr_set_bias_tee_gpio()` (the noise switch), so no GPIO patch. It has **no dithering switch**: its R820T code
  turns the PLL's sigma-delta modulator on whenever the frequency has a fractional part (`tuner_r82xx.c:516-522`).
  Kraken calls `rtlsdr_set_dithering(dev, 0)` from keenerd's/Kraken's librtlsdr. Plan: a small patch to our package
  adding that call (GPL-2.0-or-later like librtlsdr). Reading Kraken's `krakenrf/librtlsdr` for the exact register is
  a download the user has to OK (6.2, D9). Phase 1 test 1g measures whether it matters.
- **GPIO hazard**: on Kraken CH0, GPIO 0 is the noise switch. The usual "bias-T on" call
  (`rtlsdr_set_bias_tee(dev, 1)` = GPIO 0) would switch the noise on and disconnect **all five** antennas. Today
  `sdr_rtl.c:162` forces bias-T off on every open (safe: it also forces the noise off). Any future bias-T option for B
  must skip Kraken CH0 and use GPIO 1-5 instead.

### 1.9 Process and interface

- New binary **`x6x00-df`**, sources in `tools/rtlspec/` (`df.c` main/capture/socket, `df_cal.c`, `df_chan.c`,
  `df_doa.c`, `df_test.c`), sharing `sdr_rtl.c`'s USB walk and order, built by the same buildroot package. Not a mode
  of `x6x00-rtlspec`: that daemon holds one device by design (`sdr.h:8`) and carries B's DMR/vocoder/dsd-fme.
- Same style as `x6x00-rtlspec`: AF_UNIX abstract SOCK_SEQPACKET `@x6x00-df`, one client, a second copy exits,
  double-fork, started by the GUI when page I opens.
- Daemon to GUI: `DFST` (state line: NODEV/OPEN/CAL/LISTEN/TRACK/HOLD/SLIP, serials, centre, rate, gain, cal age,
  slips, OVL), `RSPC` (CH0 spectrum rows, the existing format, so B's spectrum/waterfall code is reused),
  `DFDO` (per estimate: time, target, level over floor, SNR, fit, eigenvalue ratio, bearing, uncertainty, mirror flag,
  360 x u8 spectrum), `DFCL` (calibration results per channel).
- GUI to daemon: one line each: `f` centre, `s` rate, `g` gain, `t` target offset + bandwidth + mode, `a` array
  (kind, N, size in mm, per-element x/y, channel map), `m` method, `q` squelch, `c` calibrate, `i` interval, `Q` quit.
- Threads: five readers (librtlsdr callbacks), one worker (alignment, filters, DoA), the socket. Reader threads at a
  modest `SCHED_FIFO` priority only if Phase 0 shows slips from scheduling; never Heimdall's `sched_rt_runtime_us=-1`.

### 1.10 Plain RTL-SDRs: can they DF?

**Not on their own.** Each stick has its own crystal, about 1 ppm apart:

- At 441 MHz, 1 ppm is 441 Hz: the phase between two sticks spins 441 turns a second. Phase DF needs it constant.
- The sample clocks drift too: at 1.024 MS/s, 1 ppm is a sample every second.
- Independent PLL phase noise on each stick adds on top.

What "enough RTL-SDRs" would need:

1. **One clock for all**: sticks with a clock input (some RTL-SDR Blog models have clock pads; it means soldering
   and taking each stick's own TCXO out of the circuit), fed from one reference through a buffer.
2. **One noise source** through a splitter into every channel, with RF switches to take the antennas off (or the
   user swaps cables by hand for every calibration, and every retune needs one).
3. **Equal cables**, same tuner type.
4. **At least 3** for a full circle (a UCA of 3); 2 give a bearing with a mirror ambiguity.

That is a home-made Kraken. The daemon could take such a group (the user names the serials; a 10-second check on a
common signal confirms the phase stays put), but it is not worth building before the Kraken works. Honest options
for plain sticks that need no mod:

- **Amplitude DF**: directional antennas on separate sticks, bearing from the level differences. Roughly +-15-30
  degrees, needs a level calibration. Possible later as a mode of page I.
- **One stick, turned by hand**: a directional antenna and the level on B. Works today.
- A **KerberosSDR** (4 channels, same noise-source GPIO) should work like the Kraken with N = 4 (not tested; we have
  none). A **bladeRF 2.0** has two receive ports on one LO: a coherent pair, a bearing with a mirror (only if the
  user's bladeRF is that model; `sdr_bladerf.c` uses RX(0) only today).

---

## 2. The DF engine

### 2.1 Per estimate (default every 100 ms)

1. Five aligned u8 streams to float, DC removed.
2. **Channel filter per channel**: mix the target to 0 Hz (one oscillator shared by all five, so it adds no phase
   between channels), two-stage decimating FIR (1.024 MS/s to 128 kS/s to 32 kS/s for signals up to 25 kHz; 256 kS/s
   for broadcast FM). The same filter on all five channels.
3. Apply `c_m(f_target)` (1.5).
4. **Correlation matrix** `R = (1/K) sum x x^H`, 5x5 Hermitian, K = 3,200 snapshots per 100 ms. Optionally averaged
   over the last 0.5 s for the display.
5. **Gate** ("enough signal"): the target's strongest 1 kHz bin at least 8 dB over the floor (the floor = median of
   the CH0 spectrum; the squelch is set on page I), strongest eigenvalue at least 6 dB over the next (one dominant
   source across the array), **fit at least 0.5** (a plane wave explains it), no slip suspected (1.7), no OVL,
   calibration valid. Three blocks in a row open it.
6. **DoA**: MUSIC + Bartlett on a 1 degree grid, the peak refined by a parabola through the three best points.
7. Out: bearing, uncertainty, fit, secondary peaks, the 360-point spectrum.

### 2.2 Which algorithm, and why

| Method | P(theta) | For | Against |
|---|---|---|---|
| **Bartlett** (delay-and-sum) | `a^H R a` | robust to calibration error and to a wrong source count; its peak height is the fit | wide peaks (about the calculator's beamwidth) |
| Capon (MVDR) | `1/(a^H R^-1 a)` | sharper | needs a clean inverse (diagonal loading) |
| **MUSIC** | `1/(a^H En En^H a)` | sharpest peak, the Kraken default, one source assumed | wrong if two strong sources or a bad calibration |
| MEM | `1/|a^H R^-1 e_0|^2` | sharp | spurious peaks |

- **Default MUSIC for the bearing, Bartlett beside it** (the fit and, with the user's choice, the fallback). With
  only 5x5 all four cost next to nothing, so page I can switch between them freely (F3).
- The fit `F = a(theta)^H R a(theta) / (M trace R)` is 0-100 %: how much of what the array receives is one plane wave
  from that bearing. About `SNR/(1+SNR)` for a clean signal; low for noise, multipath or a broken calibration. It is
  both the confidence on page I and the slip detector (1.7). Kraken's peak-to-average (dB) is shown as well, for
  comparison with their numbers.
- **Uncertainty wedge**: from the width of the Bartlett peak and the SNR (the Cramer-Rao bound for a UCA scales as
  `beamwidth / sqrt(2 K SNR)`), shown as +- degrees; widened to the spread of the last second of estimates when that
  is bigger.
- Secondary peaks over half the main one are drawn too (multipath shows as a second lobe).
- Not at first: ROOT-MUSIC and the UCA phase-mode transform (Kraken's are buggy, and needed only for two coherent
  sources); spatial smoothing; more than one source. Forward-backward averaging only for a ULA (a 5-element UCA is not
  centre-symmetric).
- Diagonal loading of `1e-3 trace(R)/M` for Capon and MEM.

### 2.3 Cost on the radio (Allwinner R16, 4 x Cortex-A7 at 1.008 GHz, NEON)

**Measured on the radio** (2026-11-17, `tools/rtlspec/df_bench.c` cross-built, one thread at nice 19 beside B's
daemon and the GUI; five channels, a 12.5 kHz target, 0.1 s blocks); "core" means one of the four.

| Job | 0.25 MS/s | 1.024 MS/s | 2.4 MS/s |
|---|---|---|---|
| Oscillator (shared by the five) | 0.9 % | 2.9 % | 6.7 % |
| Bytes to floats + mix, 5 channels | 3.5 % | 12.7 % | 28.9 % |
| FIR stage 1 (24 / 52 / 112 taps, by 3 / 8 / 18) | 7.4 % | 17.4 % | 37.2 % |
| FIR stage 2 (20 / 40 / 40 taps, by 2 / 4 / 4) | 3.5 % | 4.3 % | 5.4 % |
| **Channel filter** | **15.7 %** | **38.7 %** | **81.1 %** |
| **Whole engine** (+ CH0 level spectrum, R, DoA, leave-one-out) | **21-23 %** | **45-50 %** | **89-96 %** |
| DoA + leave-one-out, per estimate | 1.4 ms | 1.4 ms | 1.4 ms |
| Calibration (5 x 65,536 samples): first / with the delays known | 145 / 58 ms | | |

- The DoA maths is not where the time goes (1.4 % of a core at 10 estimates a second); the channel filter is.
- The first plain-C filter took 96 % of a core at 1.024 MS/s: GCC does not vectorise float loops for 32-bit NEON, and
  the A7 stalls on every move from a core register to NEON. The NEON version (four channels sharing each tap load,
  512-sample chunks that stay in the 32 KB L1) brought it to 39 %.
- The next step (Phase 1): 16-bit integer NEON (`vmlal.s16`, twice the multiply-accumulates of float on the A7) for
  the mix and stage 1, and a channel-interleaved layout; expected about 20 % at 1.024 MS/s. Or filter only part of
  each 100 ms (say 50 %): as accurate for a steady carrier, slightly noisier for short bursts.
- For scale: `x6x00-rtlspec` with one stick at 2.1 MS/s and B's demodulators uses about 54 % of a core today.
- 2.4 MS/s needs the filter split over two threads.

### 2.4 Arrays: what the calculator says

The calculator has two sheets, Circular (UCA) and Linear (ULA), each for **3, 4 or 5 elements** (a drop-down). Its
formulas (lambda in metres = 300 / f in MHz; `s` = the "spacing multiplier", element spacing in wavelengths, rows
0.5 down to 0.1):

| | UCA (circular) | ULA (linear) |
|---|---|---|
| Element spacing | `d = s lambda` | `d = s lambda` |
| Size | radius `r = d / (2 sin(pi/N))` (the sheet writes `s / sqrt(2(1 - cos(360/N)))`) | length `L = (N-1) d` |
| Est. resolution (deg) | `degrees(1.22 / (2 r/lambda)) / 10` | `degrees(1.22 / (L/lambda)) / 10` |
| Fixed size to frequency | `f = 300 s / d` with `d = 2 r sin(pi/N)` | `f = 300 s (N-1) / L` |
| Element positions | `x_k = r cos(2 pi k/N)`, `y_k = -r sin(2 pi k/N)`, "ANT 0 points to the forward direction" (top-down view, numbered clockwise) | `x_k = k d`, `y_k = 0` |

The limits it states: **`s` at or under 0.5** to avoid ambiguity (a larger spacing lets one wave look like another
direction); as close to 0.5 as the space allows; **0-25 degrees** of "resolution" acceptable (its colouring turns at
20). Its "resolution" is the Rayleigh beamwidth divided by 10 (a "super-resolution" rule of thumb), not an accuracy.

What it does not say: a ULA cannot tell front from back (each bearing has a mirror across the line), and its
resolution is for broadside only, getting worse toward the ends; a UCA covers the full circle.

Numbers (from the formulas):

| Band | MHz | lambda (cm) | UCA-5 radius at s 0.5 | UCA-4 | UCA-3 | spacing (UCA or ULA) | ULA-5 length |
|---|---|---|---|---|---|---|---|
| Air ELT | 121.5 | 247 | 105 cm | 87 | 71 | 123 | 494 |
| 2 m | 146.52 | 205 | 87 | 72 | 59 | 102 | 410 |
| NOAA WX | 162.55 | 185 | 79 | 65 | 53 | 92 | 369 |
| 70 cm | 441.0 | 68 | **29** | 24 | 20 | 34 | 136 |
| ISM | 433.92 | 69 | 29 | 24 | 20 | 35 | 138 |
| 33 cm | 915 | 33 | 14 | 12 | 10 | 16 | 66 |

Resolution by `s` (independent of frequency): UCA-5 8 deg at 0.5, 10 at 0.4, 14 at 0.3, 21 at 0.2, 27 at 0.15;
UCA-4 10/12/17/25/33; UCA-3 12/15/20/30/40; ULA-5 3.5/4.4/5.8/8.7/11.7.
A fixed UCA-5 of 25 cm radius (the sheet's example): s = 0.5 at 510 MHz, 0.4 at 408, 0.3 at 306, 0.2 at 204,
0.15 at 153 MHz. So one car-roof array of 25-30 cm serves 70 cm well and 2 m poorly (`s` 0.14-0.17 at 146 MHz,
about 24-29 degrees).

### 2.5 Choosing the arrangement on the radio

On page I, F2 ARRAY:

- **Kind**: UCA-5, UCA-4, UCA-3, ULA-5, ULA-4, ULA-3, CUSTOM. With N under 5, CH0 to CH(N-1) are used.
- **Size**: the radius (UCA) or spacing (ULA) in cm, 0.5 cm steps on the knob. Or **SIZE FOR FREQ**: sets the size
  for `s` = 0.5 (or the user's target `s`) at the current frequency, the calculator's first table.
- **Live check** at the current frequency, the calculator's numbers: `s`, est. resolution, and a word: OK (`s` 0.2 to
  0.5), AMBIGUOUS in red (`s` over 0.5), COARSE in amber (resolution over 20 degrees). Retuning updates it.
- **Channel map**: which Kraken channel is on which antenna (default CHk = ANTk), **cable delays** in ns per channel
  (default 0: equal cables; Heimdall has the same as `iq_adjust_time_delay_ns`).
- **Heading**: the bearing is relative to ANT 0's direction ("forward"); a heading offset in degrees turns it into a
  compass bearing (the radio has no compass or GPS).
- **ULA**: bearing 0 is broadside on the side marked forward, +-90 along the line; front/back both shown (the
  mirror dashed), or FRONT only.
- **Bias-T** per channel for active antennas (GPIO 1-5 of CH0), off by default.
- **CUSTOM**: x/y per element in cm, entered on the radio (or in the settings file).
- Saved with `gui_state_set`, per array, so a 2 m array and a 70 cm array can both be kept.

---

## 3. Page I

### 3.1 When it is there

- On the ring **only while a coherent array is present**: a Kraken detected (serials 1000-1004 behind one hub) or a
  group the user has declared coherent (1.10). Found by `sdr_pool.c`'s existing 1 s scan.
- Order **A B C D E F G H I**, then A again: H's "next" goes to I when I is there (else A, as now,
  `app_adsb.c:1233-1234`); A's "previous" goes to I when it is there (else H).
- Unplugged while I is open: I says ARRAY GONE and stays until left; then it drops from the ring.
- Like H, its own app (`src/app_df.c`), not more of `app_sentry.c` (8,600 lines). Pictures through `gpu.c`'s
  `gpu_radar`/`gpu_line`/`gpu_tri`, as AIRSPACE draws its radar; the CPU fallback as in `app_sentry.c:3596`.

### 3.2 What it shows (landscape)

```
+------------------------------------------+---------------------------------------------+
|                 0 (ANT0)                 | 441.0000  NFM 12.5k   TRACK                 |
|            .     ^     .                 | +24 dB over floor   OVL -                   |
|        .        /|\        .             | BEARING 037   +-3   FIT 91 %   MUSIC        |
|      .      4   |   1      .             | UCA-5  r 29.0 cm  s 0.50  res 8  OK         |
|     .           o           .            | CAL 41 s ago  slips 0  1.024 MS/s  gain 28  |
|      .       3     2       .             +---------------------------------------------+
|        .                 .               | CH0 spectrum, 1 MHz, target band marked     |
|            .    .    .                   +---------------------------------------------+
|  polar P(theta), needle, wedge, history  | bearing waterfall: 0-359 across, time down  |
+------------------------------------------+---------------------------------------------+
  F1 TARGET    F2 ARRAY    F3 METHOD    F4 CAL    F5 SET
```

- **Compass/polar** (left, the big part): the array drawn to scale in the middle, elements numbered as wired, ANT 0
  marked forward; the DoA spectrum as a filled polar curve; the bearing needle with its +- wedge, coloured by fit
  (green 80 % up, amber 50-80, red under); secondary peaks as short ticks; the last 60 bearings as fading dots on the
  rim; ULA mirror dashed; north up and "N" shown when a heading is set, else ANT 0 up.
- **Target** (right top): frequency and bandwidth, level over the floor, state (CAL / LISTEN / TRACK / HOLD / SLIP /
  OVL), bearing, uncertainty, fit, method; array, size, `s`, resolution, the word; calibration age, slips, rate, gain.
- **CH0 spectrum** (right middle): the Kraken's passband with the target band marked; the VFO knob moves the target.
- **Bearing waterfall** (right bottom): one row per estimate, 0-359 across, colour = P(theta): short transmissions
  and moving sources show up as tracks.
- **HOLD**: when the signal stops, the last bearing stays, fading over 30 s, with its time.

### 3.3 Keys (G's conventions: MFK for the ring, VFO for fine moves, arrows step)

- F1 **TARGET**: MANUAL (frequency + bandwidth 2.5/6/12.5/25/200 kHz), AUTO (strongest peak in the passband, with
  hysteresis), **B** (B's frequency, when B is on another SDR).
- F2 **ARRAY** (2.5). F3 **METHOD** (MUSIC/Bartlett/Capon/MEM). F4 **CAL** (now; held: the interval).
  F5 **SET** (rate, gain, squelch, heading, history length).
- VFO knob: target within the passband; arrows: retune the Kraken by half a passband.

### 3.4 With the SDR pool and the other screens

- New pool owner `SDR_OWN_DF` (`sdr_pool.h:33-36`) that claims the **whole array or nothing**, and a pass in
  `resolve()` (`sdr_pool.c:150-196`): B first (unchanged), then **DF while I wants it**, then watch channels, then
  AIRSPACE spares.
- Why all five: CH0 is the noise switch, and a burst disconnects every antenna, so another user on a Kraken channel
  would hear noise during calibration.
- **B on a Kraken channel**: B moves to another SDR if one is free (the plain stick, the HackRF, the bladeRF);
  if there is none, B is lent to I and says so, as AIRSPACE borrows B today. (Decision D5.)
- Watch channels and AIRSPACE spares on Kraken sticks stop while I holds them (AIRSPACE's dump1090 gets SIGTERM and
  up to 8 s, `adsb_rx.c:477-488`; I shows "waiting for RTL3") and come back when I lets go.
- From B: a key "DF THIS" sends B's frequency to I (B must not be on the Kraken).
- Files: `src/app_df.c` (new), `src/df_rx.c` (new client, like `rtl_spectrum.c`), `src/sdr_pool.c/.h`,
  `src/app_adsb.c` (next), `src/app_sentry.c` (previous from A, legend), `docs/MANUAL.md`, the buildroot package
  (one more binary), the librtlsdr patch.

---

## 4. Budgets on this radio

Measured 2026-11-17 (read only): 4 cores at 1.008 GHz, 1002 MB RAM with 811 MB available, root file system
488 MB with 87 MB free, `usbfs_memory_mb` 16, SoC at 61 C, total CPU 71 % idle (B's daemon about 44 % of a core,
the GUI about 28 %). The MUSB host takes **10,300 interrupts a second** for B's stick (2.1 MS/s, 4.2 MB/s) and the
USB-3000: about one per 512-byte packet, all on CPU0.

### USB

| Rate per channel | Kraken (5 ch) | + B on its own stick (2.1168 MS/s) | MUSB interrupts (est.) |
|---|---|---|---|
| 0.25 MS/s | 2.5 MB/s | 6.7 MB/s | +5,000/s |
| **1.024 MS/s (proposed default)** | **10.2 MB/s** | **14.5 MB/s** | +20,000/s |
| 2.4 MS/s | 24.0 MB/s | 28.2 MB/s | +47,000/s |

Proven so far: about 24 MB/s with 0-3 samples per million lost (2026-11-16). 1.024 MS/s keeps DF plus B at 60 % of
that, sees 1 MHz around the target (a repeater pair, neighbouring channels), and is plenty for any voice signal.
0.25 MS/s is the fallback if slips are frequent. The final default comes from Phase 0 test 0a.

### CPU

`x6x00-df` measured 45-50 % of a core at 1.024 MS/s, 21-23 % at 0.25 MS/s (2.3); the GUI on page I about what the
Sentry's screens take now (16-22 % of a core, measured 2026-11-17); USB interrupts extra on CPU0. Fits in the 2.8
idle cores. At 2.4 MS/s (89-96 %) it needs two filter threads.

### Memory

| | x6x00-df | Heimdall + DoA at Kraken defaults |
|---|---|---|
| USB buffers | 12 x 32 KiB x 5 = 1.9 MiB | 12 x 512 KiB x 5 = 30 MiB (needs `usbfs_memory_mb=0`) + 8 x 512 KiB x 5 alignment = 20 MiB |
| Streams | rings 0.25 s x 5 = 2.5 MiB | rebuffer u8 CPIs 2 x 10 MiB |
| Frames | none (streaming) | 40 MiB per float CPI, A/B in three shared-memory stages = 240 MiB |
| Calibration | 65,536 x 5 complex + FFT space = 8 MiB, briefly | Python copies of the frames |
| History | 1 h at 1/s + 60 s at 10/s with spectra = 1.5 MiB | none |
| **Total** | **about 15 MiB** | **over 300 MiB**, plus Python, numba, scipy, Dash, node |

### Disk and the rest

- `x6x00-df`: a few hundred KB (librtlsdr static, as `x6x00-rtlspec`). Kraken's stack needs numba (LLVM), scipy,
  scikit-rf, pyzmq, Dash, node.js; numba on 32-bit ARM is the blocker; none fit in 87 MB.
- **Power**: the Kraken needs its own supply (the radio's HOST power cannot run it).
- **Heat**: one more core at 30-40 % on a SoC at 61 C; page I shows the temperature F shows.

---

## 5. Phases, each with a bench test

**Rules for every test**: the X6200 stays receive-only (the Sentry open, `radio_rx_only`; its antenna port is never in
a test path). The HackRF goes only into the Kraken, through the 40 dB (10 W) and 30 dB attenuators, amp off (`-a 0`),
antenna power off (`-p 0`), the lowest `-x` that gives 20-30 dB over the floor; any level past what the user approved
is asked first. Only amateur frequencies for the HackRF (441.000 by default). Nothing on the radio changes without the
user's OK for that step; no GUI or daemon restarts while another session is testing.

### Phase 0: the facts that decide the design (no new code on the radio)

- **0a. Slip rate**: the Kraken on its own power on the HOST port; five `rtl_test` in test mode (a counter instead of
  samples: every gap is a lost sample), 30 minutes each at 0.25, 1.024 and 2.4 MS/s, with B's daemon running and
  without. Count **events per hour per channel**, not samples per million. Pass: 1.024 MS/s under one event per 10
  minutes across all five. Needs the user's OK to hold watch channels and AIRSPACE off the Kraken meanwhile.
- **0b. Synthetic tests on the Mac** (`tools/rtlspec/df_test.c`, part of the Mac-runnable tests): five channels
  made up with a plane wave from a known bearing for each array kind, noise, random per-channel delays (whole and
  fractional), phases and gains, a noise-source burst, injected slips, a second weaker source. Pass: calibration
  recovers phases under 0.5 degree and delays under 0.02 sample; bearing within 1 degree at 20 dB SNR (UCA-5,
  s 0.5); slip caught within 1 s; mirror and sign conventions match the calculator's drawing.
- **0c. CPU**: the engine's loops on the radio from a recorded file (`sdr_file.c` backend, no stick): CPU per stage at
  the three rates. A test binary in /tmp, with the user's OK.

### 5.1 Phase 0 results (2026-11-17)

- **0b. Synthetic tests: pass**, 40 checks (`tools/rtlspec/df_test.c`, in `tools/host_checks.sh` as `df`, about 9 s;
  the NEON code and the plain loops (`-DDF_NO_NEON`) both pass; the radio's 32-bit build compiles clean with -Werror):
  - the calculator's own numbers (UCA 416 MHz s 0.5: 8.2173 degrees; ULA 440 MHz: 3.4950) and its ANT layout;
  - calibration over 10 random sets of delays (up to 3,000 samples), phases and gains: delays within 0.0006 sample,
    phases within 0.06 degree, gains within 1.7 %; with the delays known, the same answer;
  - bearings at 20 dB SNR, NFM 150 kHz off centre, each channel with its own random delays, phase and gain, 8-bit:
    UCA-5 (s 0.5 and 0.25), UCA-4, UCA-3, ULA-5 (with its mirror) at 0-300 degrees: every one within 0.05 degree, fit
    0.97-0.99. Bartlett, Capon, MEM within 0.02. A second transmitter 10 dB down at 120 degrees away: 0.1 off;
  - no bearing claimed for noise alone, nor for equal phases on a circle (fit 0.02); the timer asks in a gap;
  - slips: 256 samples on one channel caught in 0.25 s, the channel named, no wrong bearing shown, the stale delays
    refused and the new search finding the 256; one sample also caught. This changed the slip detector (1.7).
- **0c. CPU: measured** (2.3): 45-50 % of a core at 1.024 MS/s, 21-23 % at 0.25, 89-96 % at 2.4; a calibration 145 ms
  of CPU (58 ms with the delays known). The bench binary ran in /tmp at nice 19 and was removed.
- **0a. Slip rate: the Kraken plugged in 2026-11-17, sticks not yet free.** Ready:
  `tools/rtlspec/df_slip_test.sh -h root@<radio> -s 1024000 -m 30`, then `-s 250000` and `-s 2400000`. It refuses to
  start while a watch channel or AIRSPACE holds a Kraken stick.
  - First look (read only, before the test): with the Kraken alone on HOST, the SDR pool gave AIRSPACE three spares
    (dump1090 on 1000, 1001, 1003 at 2.4 MS/s, adaptive gain) and B took 1004 at 2.1168 MS/s: 9.3 MS/s, about 18.6
    MB/s. Within 2 minutes the MUSB logged 32 "three-strikes" errors on every RX endpoint, the Kraken's hub dropped off
    the bus once and came back, then `usb_submit_urb returned -121` on one stick and B's stick re-enumerated; two
    dump1090s did not come back. That is less than the 24 MB/s streamed cleanly on 2026-11-16, so the bytes alone are
    not the limit: dump1090 opening sticks and writing gain registers while the others stream is the suspect. To be
    told apart by the test (five steady streams, no register writes).

### Phase 1: the DAQ alone (x6x00-df, command-line output, no page I)

Most of these need **no outside RF**: the noise source is inside the Kraken.

- **1a. Noise switch**: noise on raises all five by the same 20-40 dB; off returns to the floor; B's stick unaffected.
- **1b. Start**: 20 starts; whole-sample delays found every time with over 20 dB of peak margin.
- **1c. Repeatability**: calibrate, then a second noise block 1 s later: residual under 1 degree, 0.2 dB.
- **1d. Drift**: one hour, a burst every minute: phase moves per channel per minute. Sets the default interval.
- **1e. Retune**: 20 retunes from 100 to 1000 MHz: valid again in under 0.5 s each time.
- **1f. Gain**: noise on, all gains stepped together: phase change per step and channel. Over 2 degrees: calibrate
  after every gain change at the operating gain.
- **1g. Dithering**: 1c and 1d with dithering on (our librtlsdr today) and off (patched). Shows whether the patch is
  needed.
- **1h. Switch isolation**: HackRF CW at 441.000 through 70 dB of attenuators into CH0, noise on and off: the tone's
  drop is the isolation (expect over 40 dB).
- **1i. Slips in real use**: one hour streaming at the chosen rate with the detectors on; plus a forced slip (samples
  dropped in software on one channel) caught within 1 s.

### Phase 2: the engine, cabled

Needs a splitter (1:4/1:5, or 1:2s) and equal SMA cables, BNC-SMA adapters for the attenuators. Without a splitter,
skip to Phase 3.

- **2a. All in phase**: HackRF to 70 dB of attenuators to the splitter to CH0-CH4. Declared ULA-5: bearing 0
  (broadside) within 1 degree, fit over 95 %. Declared UCA-5: equal phases are no direction in the plane: the fit must
  read low and no bearing be claimed (tests the gate).
- **2b. A known bearing from a cable**: two channels, one extra cable of known electrical length (velocity factor
  measured or from the data sheet), declared 2-element ULA at lambda/2: bearing = `asin(delta phase / pi)` within 2
  degrees. Swapping the cable to the other channel mirrors the bearing (sign check).
- **2c. Weak signals**: step the HackRF down until the error passes 2 degrees: the SNR needed, and the gate set just
  above it.
- **2d. Budgets**: CPU and USB with B running at the chosen rate, against section 4.

### Phase 3: page I, the pool, off the air (receive only)

- **3a. Ring**: plug the Kraken in: I appears within 2 s; H to I to A; A back to I; unplug: ARRAY GONE, then gone.
- **3b. Pool**: open I with watch channels and AIRSPACE on Kraken sticks: they let go, I runs, they come back on
  leaving I; B on the plain stick untouched; B on a Kraken stick handled per D5.
- **3c. Known transmitters**: a UCA at the calculator's radius for the band, outdoors, clear of metal; DF transmitters
  whose sites are published (a 70 cm or 2 m repeater, NOAA weather radio, an FM broadcast tower with a big array),
  bearings worked out from their coordinates. Pass: within 10 degrees with fit over 70 %; record how multipath looks.
- **3d. Turn the array** 90 degrees by hand: the bearing turns 90 degrees the other way. Catches sign and mirror
  errors that cables cannot.
- **3e. Manual**: the I section in `docs/MANUAL.md`.

### Phase 4: later, each only if the user wants it

DF in the background with bearings in C's log; DF of every signal over the squelch at once (FFT per channel);
scanning DF through a frequency list (0.5 s per stop); declared plain-RTL groups and amplitude DF (1.10); a bladeRF
2.0 pair; GPS course as heading when moving.

---

## 6. Risks and questions

### 6.1 Risks

1. **Slips on the MUSB host** (the radio's USB): the whole design rests on test 0a. If slips are frequent at every
   rate, DF still works at 0.25 MS/s with calibration every 30-60 s, at the cost of seeing only 250 kHz.
2. **MUSB under five streams for hours**: the 9101/9102 kernel fixes were for a crash on disconnect; a long
   five-stream run has not been done. A crash hangs the whole radio.
3. **Phase versus gain, and dithering** (1f, 1g): either can force more calibration or the librtlsdr patch.
4. **Real-world accuracy**: multipath, metal nearby, unequal cables, antennas closer than the far field, mutual
   coupling. Bench numbers will be better than the field; test 3c tells the truth.
5. **Sharing the Kraken**: while I runs, watch channels and AIRSPACE lose any Kraken sticks.
6. **GPIO 0** (1.8): a bias-T switched on for B on Kraken CH0 would disconnect all five antennas. Guard it in code.
7. **RTL numbering**: a plain stick plugged in beside the Kraken renumbers the Kraken to RTL1-RTL5; DF finds it by
   serial, but the user's RTL0-RTL4 habits change.
8. **Heat and CPU** at 2.4 MS/s.

### 6.2 Decisions for the user

- **D1. Approve the approach**: our own C daemon `x6x00-df` + page I + pool owner, Kraken first; plain RTLs only as
  a later, declared group. **Approved 2026-11-17.**
- **D2. Licences**: Heimdall and krakensdr_doa are **GPL-3.0-or-later**; our repository is **LGPL-2.1**; librtlsdr is
  GPL-2.0-or-later (already static in `x6x00-rtlspec`). **Recommended: reimplement** from the published maths (MUSIC,
  Capon, Bartlett, Burg) and the measurements described above, copying no code, so our new files stay LGPL-2.1.
  The other way, copying code into the daemon, makes `x6x00-df` GPL-3 (allowed, since it is a separate program and the
  source is public, but their notices must be kept). Not legal advice.
- **D3. Default sample rate**: 1.024 MS/s (proposed), 0.25 or 2.4, after test 0a. CPU measured: 45-50 % / 21-23 % /
  89-96 % of a core.
- **D4. Calibration interval**: 2 minutes (proposed), waiting for the squelch to close; final value after test 1d.
- **D5. B on a Kraken stick when I opens**: move B to another SDR if one is free, else lend B to I (proposed); or I
  waits until B is moved by hand.
- **D6. DF only while I is open** (proposed: the Kraken goes back to AIRSPACE and watch channels on leaving), or in
  the background with bearings logged.
- **D7. Page I on the ring only when an array is present** (proposed, as asked), or always there with "NO ARRAY".
- **D8. Hardware on hand**: a splitter (1:4/1:5 or 1:2s) and five equal SMA cables for Phase 2? Antennas and a mount
  for an array (five identical whips on a ground plane, radius from 2.4)? Which band first (70 cm fits a 29 cm
  radius; 2 m needs 87 cm)?
- **D9. Downloads**: OK to read `krakenrf/librtlsdr` (or keenerd's) for the exact dithering register? Anything else
  beyond the three links given is asked first.
- **D10. Plain RTL-SDRs**: drop phase DF for them (proposed), or plan the clock-mod group / amplitude DF as Phase 4.

---

## Appendix: sources

- Heimdall DAQ, development 58c4068 (2024-04-09): `Firmware/_daq_core/rtl_daq.c`, `delay_sync.py`,
  `hw_controller.py`, `fir_decimate.c`, `rebuffer.c`, `iq_header.h`, `Firmware/daq_chain_config.ini`,
  `Firmware/daq_start_sm.sh`. Licence GPL-3.0-or-later.
- krakensdr_doa 2e1c4e6 (2025-12-13): `_sdr/_signal_processing/kraken_sdr_signal_processor.py` (SP),
  `_sdr/_receiver/kraken_sdr_receiver.py`, `_ui/_web_interface/`. Licence GPL-3.0-or-later; pyARGUS not vendored.
- KrakenRF, Antenna_Array_Size_Calculator.xlsx (krakensdr_docs, antenna_array/): two sheets, formulas above, read as
  XML (no macros in an .xlsx).
- M. Laakso, "Multichannel coherent receiver on the RTL-SDR", 2019 (cited in `delay_sync.py` for the sample-rate
  slewing we do not copy).
- R. Schmidt, "Multiple emitter location and signal parameter estimation", 1986 (MUSIC); J. Capon, 1969 (MVDR).
