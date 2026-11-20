# The bench harness

Automated tests through attenuators, run by `bench.py` on the computer beside the radios: the HackRF into a radio
that only listens (profile `hackrf`), or two radios keying into each other (profile `radios`). The radios are worked
through their browser remote (port 50002) with sessions of the harness's own; SSH only to provision those sessions,
set the test callsigns and clean the logs.

Configuration: `~/.config/x6x00-bench/bench.json`, from `bench.example.json`. The tokens, the owner's confirmations
(`chain.json`), the TX log (`tx_log.jsonl`) and the runs' files are kept beside it, never in the repository.

## The rules

Each is enforced in `bench.py` (a gate refuses, and says why, before anything is sent), not only followed.

1. **Nothing is sent without the owner's word on the cable.** `bench.py chain-ok PROFILE "their words"` records it,
   run only after the owner said in the conversation that this profile's cable is in place; it holds 12 hours.
   The pads are in the profile; their total must be at least 70 dB for the HackRF, 60 dB between two radios.
2. **Never into an antenna.** The cable from transmitter to receiver goes through the pads and nowhere else; a
   radio's antenna jack carries the pad chain, not an antenna, for every `radios` test. The owner's word (1) is
   for exactly that.
3. **The HackRF profile: the radio only listens.** It is worked with a session that may not transmit (the radio
   itself refuses PTT, TUNE and the FT8 app's CQ, Auto, reply and call from it, and while that session controls it
   holds itself receive only, the base at 0 W and its keys' ways to the air refused too: the state's `"rxo":1`).
   The harness refuses to start without that hold, stops the HackRF if the hold or the control goes, and refuses
   while
   the radio is on the air or its FT8 app is set to send (CQ, or Auto on a radio with a callsign: without one the
   app sends nothing); a radio that goes on the air while the HackRF feeds it stops the HackRF at once. HackRF: `-a 0` (the amp never), `-p 0`, `-x` 40 at most (the owner's approved
   maximum).
4. **The radios profile: low power, power-rated pads.** At most `limits.radio_w` (the configuration; 5 W
   whatever it says), and the first transmission of a session at 0.1 W whatever is asked. Each radio's own jack
   carries a pad rated for at least twice the test power. The power is set and read back before keying; the ATU
   off (it tunes by transmitting); the frequency inside the amateur bands.
5. **Every transmission watched.** A radio on the air longer than `limits.max_key_s` (16 s at most: one FT8
   transmission), its SWR over 2.0, more power than asked, or a receiver hearing 20 dB more than the chain lets
   through (pads missing) stops everything at once: the HackRF killed, every radio unkeyed, the apps' TX stopped.
   The browser remote's PTT is a lease besides: without its renewal the radio unkeys within 3 s.
6. **Test callsigns only.** `Q<digit>TST` (`bench.py testcall NAME Q1TST`, `restore` after), never a real
   call; FT8 grids FN00. The test QSOs are taken out of the radio's logs after each run (`cleanlog`).
7. **Everything logged.** Each HackRF and radio transmission and each stop goes to `tx_log.jsonl`.
8. **Stop on demand.** Ctrl-C or `bench.py stop` is the emergency stop (rule 5's actions).
9. **The radios as found.** A test closes the apps it opened (the X6200 goes back to the Sentry) and releases
   control; a callsign set for the tests is put back with `testcall NAME restore`.

## Use

    bench.py provision x6200 rx          # once: a receive-only session (the GUI restarts)
    bench.py chain-ok hackrf "the owner's words"
    bench.py preflight hackrf            # every gate, nothing sent
    bench.py hackrf-ft8 --band 20m --x 30
    bench.py radios-ft8 x6100 x6200      # both radios on the cable, test callsigns set: CQ, then a reply
    bench.py radios-walkie x6200 x6100   # G on both: each band's channel, AM and FM, each sends, the other hears

`radios-walkie` (2026-11-19, the owner: "BOTH radios should be able to TX & RX to each other across like any band
selected ... across the wire + PAD setup"): both radios on G (held there: on any other page A's scans retune the
dial), on each band's channel G offers (160 to 10 m, 10 m and 6 m FM calling; never CB or the other services, outside
the amateur bands) in AM and FM (SSB without a voice sends nothing), each in turn armed with A/B and keyed 4 s on the
air at 0.1 W while the other listens. Heard: the listener's own spectrum within 3 kHz of the dial or its S-meter up
15 dB (the panel's spectrum frames, kept by remote_ws.py: the X6200's S-meter does not follow an AM signal its
spectrum shows 40 dB up). One radio on the air at a time: the sender is armed only when the other has been out of TX
a second, the watch stops everything when two are on the air at once (sim_test.py's case "both"). A tap of A/B or a
key the radio missed over WiFi is tried again, an over during which the listener sent nothing (the X6200's reports
stalled for seconds now and then) is tried once more; afterwards both on the first radio's channel, on G, each its own
power and tuner back. `--senders`, `--keep-armed` (each radio armed through a channel's overs, as an operator keeps
it). First runs: the X6200 heard by the X6100 on every band and mode; the X6100 by the X6200 on every band and mode
while the X6200 only listened. The X6100 put out 0.9 W asked for 0.1 W on 6 m, and the watch stopped it there.

`tones.py TX RX` (2026-11-20, the owner: "step some tones across the audio range and see if they come through the other
side the same way"): tones from 200 to 3100 Hz in USB into one radio's transmitter through its own TCI server
(tci_tone_tx.py, run on that radio: a client from the radio itself is not asked about on its screen), the other radio's
TCI audio recorded here with its AGC off, then the same with it on LSB: each tone's frequency (offset and slope fitted;
a few hundredths of a hertz), level, harmonics, the other sideband, and the listener's own spectrum at each tone's RF.
At 0.1 W, the first transmission of the session; the watch as radios-walkie's (more power than asked ends the over:
the sender killed, since the remote's PTT release does not end a TCI transmission). Faded tones with short gaps
measure the passband. The listener's AGC is off and its squelch open for the run (a shut squelch mutes the audio
whole: the X6200's, at 4, faked a silent transmitter and an image 101-110 dB down on 2026-11-20). The X6100 (gcfix4)
into the X6200, 20 m: 300-2900 Hz within 3.5 dB, the other sideband about 42 dB down (the two radios' images
together), the two radios' oscillators 7.2 Hz apart, the slope 8 ppm.

`tones.py TX RX --bursts 0.05,0.1,0.2,0.4,0.8,1.6,3.2`: the transmitter's power loop on the first sound after a
pause. 1 kHz bursts of 0.5 s with 2 ms starts, each after the pause given; in the tone's band of the listener's audio
each burst's first 80 ms against its steady end (the overshoot), its lowest after that (the dip), how long it takes
to settle within 1 dB, and what came in the pause. Xiegu's X6100 loop: +5 to +18 dB, then 6-14 dB under; with gcfix4
within +1.1 dB (tools/base_update/README.md). A burst too hot for the watch (0.6 W at 0.1 W) ends the over, so
Xiegu's loop is seen on its first bursts only. The sender on the radio is stopped by its pid file (the X6100's
BusyBox has no pkill).

`--w 5` (2026-11-20, the owner: gcfix "needs a run at 5-10 W into a dummy load"): every pass at 0.1 W first (rule 4),
then at the power asked, up to `limits.radio_w` and never over 5 W, a pad of twice that on each radio's jack. The
X6100 at 5 W, 20 m: gcfix4 within +0.3 dB after every pause, 4.7 W; Xiegu's loop (gcfix3) +0.9 dB, and its own
noise in the pauses 9-10 dB higher; 6 m, gcfix4, within +0.6 dB. So the bursts are a low-power problem.

`image.py TX RX [--bands ...] [--w 5]` (2026-11-20, the owner: "you MUST prove this via testing"): where the
transmitter's opposite-sideband image and its carrier leak land and how strong they are, band by band. TX sends USB
tones fa (600, 900 Hz), RX listens in USB with its dial OFF Hz below (1500; 20 and 80 m also 1200). In RX's audio the
wanted tone is at OFF + fa, TX's carrier at OFF, and TX's tone mirrored around its dial at OFF - fa. RX's own image
mirrors around RX's dial into RX's other sideband, which its USB demodulator leaves out. The image's measured place
is checked against 2 x carrier - wanted (mirror error in Hz), and two offsets move RX against TX. RX's panadapter is
read for anything else the over brought up. The session's first over is at 0.1 W, the rest at --w; RX's squelch
open, AGC 3, attenuator on (--rx-att, --rx-rfg). If RX's TCI audio stops (its WiFi), a new connection is made.
The X6100 into the X6200 at 5 W, all bands 160-6 m: the image at the dial minus fa to within 0.09 Hz, -42.5 to
-54.5 dBc (15 and 12 m the worst); Xiegu's own V1.1.9 the same (-45.2 to -46.6 dBc on 20, 15 and 6 m). The
carrier: -30 to -48 dBc at 5 W, about -12 dBc at 0.1 W on HF (it does not scale with power). Details:
tools/base_update/README.md.

`txcal.py TX RX [--bands ...] [--w 5]` (2026-11-20, the owner: "please make it so"): the transmitter's calibration
band by band, against the other radio, in image.py's geometry (a 1000 Hz tone, RX 1500 Hz below). It sets TX's band
values through its remote (`set txig/txip`: the I/Q balance, register 52, the X6100's base gcfix5; `set txio/txqo`:
the DC offsets, registers 50/51) and measures the image and the carrier for each. The X6100's base takes them only
between transmissions, so each point is an over of its own (a 1.6 s tone), each over moving one value of each pair
and measuring both. Each pair's residual is a bowl: five points, a parabola on each axis to its bottom, a second pass
at a third of the step, the last point measured; 11 overs a band. What it finds stays in the band's settings (the GUI
saves them) when it is better than before; `--dry` measures only. An over whose tone did not come through to RX is
done again. The session's first over is a short one at 0.1 W.

`radios-ft8` first ran 2026-11-18 (the X6100 and the X6200 through 40 + 30 dB, 0.1 W, 20 m), both ways: PASS. The
X6200 decoded the X6100 at +11 to +15 dB, the X6100 the X6200 at -2 to -3 dB (the X6200's preamp on, the X6100's
off: a parity run sets them alike). Found by it: the FT8 app opens on its last band and holds the dial (the band keys
move it now, `ft8_to_band`), the log cleaning's script broke through the shells (base64 now), `testcall NAME
restore`, and the radios' power put back after. From 2026-11-19 it says for each transmission the sender's own
power meter (W, its highest on the air) and the listener's S-meter before and at its highest, beside the chain's
expected level: a weak transmitter and a deaf receiver read differently there, while FT8's SNR of a -50 dBm signal
is the decoder's measure of a strong one, not sensitivity (2026-11-19: -10 to +11 dB on either radio for the same
-70 dBm). `--rx-alike` sets the preamp and attenuator off and RF gain to 100 on both, each put back after: below 100
the X6100's base switches in a stepped attenuator, and it had run at 63, its default.

`hackrf-iq FILE --at HZ --offset HZ` sends an I/Q file made elsewhere (int8, 2 MS/s by default, the signal OFFSET
above the HackRF's centre) into the radio tuned to HZ in a mode (7: narrow FM; `--filter LO HI` for the test),
puts the radio's frequency, mode, filter and squelch back after, and runs `--during "CMD"` beside it (for example a
capture on the radio over SSH) and shows its output.

`dv_replay.py HOST FILE.iq` tries B's digital voice with no SDR plugged in: a recorded transmission replayed into a
test SDR daemon beside B's, whose DMR or dsd-fme decodes it with the voice through the USB-3000 B's daemon holds; it passes
when a call is decoded and its voice heard (receive only, nothing on the air).

`hackrf-ft8` makes the message with ft8_lib's `gen_ft8` (`gen_ft8` in the configuration), turns it into upper
sideband I/Q at 2.004 MS/s (100 kHz above the HackRF's centre, clear of its DC spike), starts on the next FT8 slot
and passes when the radio's FT8 app shows the text.

## The two-radio suite (suite.py)

`suite.py` runs batches against both radios at once and writes a report with every screen photographed side by
side (`~/.config/x6x00-bench/runs/<time>-<batches>/report.md`), for a release to break neither radio:

    suite.py list                 # the batches and what each needs plugged in
    suite.py run core             # both radios on the network: versions, services, settings, apps, every screen
    suite.py run rtl,cable        # the hardware batches, when that hardware is on the bench
    suite.py release              # core and the batches bench.json's "attached" names

`core` keys nothing: receive-only sessions, each radio held receive only while the suite has control. It checks
both run the same version, the same services and apps, lists the look-and-behaviour settings that differ, opens
every app and every Sentry page on both through the remote (`apps`, `open`, `close`), photographs them and counts
each picture's colours: cyan, blue or purple (an old theme's accents) is flagged, the Sentry's warm gradient and
green status words are not. On the X6100 it presses the MFK and turns the main dial through the remote and checks
the page moved and the dial did not (the stand-in for the X6200's inner ring). The hardware batches run the
harness's own tests: `rtl` (AIRSPACE on both for 90 s, photographed), `cable` (`radios-ft8 --rx-alike` both ways:
test callsigns set and any ATU off first, all put back after; the cable's `chain-ok` first), `update` (each
radio's `x6100-update check`: the newest release for its model; nothing downloaded or installed).

While the suite or a bench test runs, each radio shows a banner across the top of its screen (the owner,
2026-11-19: "which device is TXing and which is RXing and exactly which test"): the remote's `banner TEXT`
(`banner` alone clears it), its text the test and both radios' parts ("SUITE core: screen app112_SSTV ...",
"BENCH FT8 radio to radio, 20m, 0.1 W through 70 dB: X6100 SENDS CQ, X6200 LISTENS"), and in front of it the
radio's own TX (on red) or RX, from its transmit state. It goes when the suite's connection closes, so a run that
dies leaves none; the suite hides it for the second each screenshot takes.

    suite.py align x6200 x6100           # the X6200's look-and-behaviour settings and time zone onto the X6100
    suite.py align x6200 x6100 --undo FILE

`align` writes the settings core lists as different into the second radio's params.db with its GUI stopped (a GUI
writes its own back as it ends) and copies `/mnt/tz.conf`; the second radio's values before are kept in the run's
folder for `--undo`. Each mode's filter edges (mode_params: `mode<N>.filter_low/high`) are compared and aligned too
(2026-11-19: the X6200's AM 0-5000 against the X6100's 0-4000 was a value kept on one radio, which the general
settings did not show).

Before core's pictures (`pages`, `screens`) the X6100 is made like the X6200 further than `align` goes (photo_sync,
2026-11-19: A and B on different frequencies, the Direction finder's resting state not the same): every params row
but the radio's own hardware and identity (calibrations, PA bias, TX power, ATU, callsign, QTH, WiFi), so the dial,
bands, volumes, the receiver's switches and each app's frequency too, and the GUI state file (`/mnt/gui_state.conf`:
B's frequency, the Sentry's pages, the Direction finder's target and array), its GUI stopped and started again. Its
own come back after the pictures (`photo_sync_x6100_undo.json` in the run's folder).

`redact_shots.py RUN_DIR OUT_DIR` makes a suite run's screenshots fit to publish: what would place the station
pixelated (or a map flattened) on both radios alike, each screen's pair side by side. The first gallery:
[docs/parity/2026-11-18](../../docs/parity/2026-11-18/README.md); the second, after the fixes those pictures led to:
[docs/parity/2026-11-19](../../docs/parity/2026-11-19/README.md) (the X6100's Weather and Flights showed the locator in
their header with no GPS and no helper answer: boxed since). Look at every pair before publishing another run, and
read them with OCR for locators and coordinates too (a picture left unredacted on purpose is the check that it reads).
