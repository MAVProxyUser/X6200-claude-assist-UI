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
   whatever it says; image.py and txcal.py ask 0.1 and 1 W by default: no test stresses a pad, the owner,
   2026-11-22, after a night with 10 W overs into the 10 W pads), and the first transmission of a session at 0.1 W whatever is asked. Each radio's own jack
   carries a pad rated for at least twice the test power. The power is set and read back before keying; the ATU
   off (it tunes by transmitting); the frequency inside the amateur bands. And a supply that holds the radio:
   before each over the radio's own battery log (`/mnt/battery_log.csv`, a row a minute) is read, and no over goes
   on a radio on its battery at 10 % or less, nor with its DC in sagging under 11.0 V in the last minutes and its
   battery under 30 %, nor over 5 W with any sag (2026-11-20: the X6100's stock adapter, a 12 V charger, sagged to
   0.2-3.9 V while it transmitted, the radio drained its battery all evening and switched off in a 10 W over). An
   unreadable log is said, not a stop.
5. **Every transmission watched.** A radio on the air longer than `limits.max_key_s` (16 s at most: one FT8
   transmission), its SWR over 2.0, its power over the rating of the pad on its jack (the radios profile's `watts`,
   10 W here), or a receiver hearing 20 dB more than the chain lets through (pads missing) stops everything at once:
   the HackRF killed, every radio unkeyed, the apps' TX stopped. More power than asked but under the pad's rating is
   logged, not stopped (the owner, 2026-11-21: "don't kill just log. we have attenuators"): said once an over, its
   highest reading in `tx_log.jsonl` and the run's result (`over_power_w`), and the over goes on. A radio the harness
   hears nothing from for 6 s (its meters come every 100 ms) stops everything too: it cannot be watched. The browser
   remote's PTT is a lease besides: without its renewal the radio unkeys within 3 s.
6. **Test callsigns only.** `Q<digit>TST` (`bench.py testcall NAME Q1TST`, `restore` after), never a real
   call; FT8 grids FN00. The test QSOs are taken out of the radio's logs after each run (`cleanlog`). No real call
   starts with Q, which is why: and WSJT-X takes none for a standard call (chkcall.f90), so a Q call goes as a
   nonstandard one, `CQ Q1TST` with no grid and the reply `<Q1TST> Q2TST`, a hash where the call has been heard;
   radios-ft8 takes either form. Up to v0.3.12.43 the radios' FT8 library sent Q calls as standard and WSJT-X
   discarded every such message (2026-11-21: never `CQ Q1TST EN80` from a radio, while the other radio decoded
   it): a test call WSJT-X is to hear needs ft8lib patch 0002 on the sending radio.
7. **Everything logged.** Each HackRF and radio transmission and each stop goes to `tx_log.jsonl`.
8. **Stop on demand.** Ctrl-C or `bench.py stop` is the emergency stop (rule 5's actions); then the test's own end
   still puts the radios back (rule 12). A second Ctrl-C leaves at once.
9. **The radios put back, on page A: NEVER left otherwise.** A test closes the apps it opened and releases
   control; a callsign set for the tests is put back with `testcall NAME restore`. Everything it changed is put back
   and read back (`bench.finish`): the settings, each band it tuned through, a mode's own filter. Both radios end on
   the Sentry's page A, whatever page they were on (the owner, 2026-11-22: "when tests finish they should always leave
   on screen A"). Up to three attempts, each renewing a stalled or dropped link, stopping the bench's own programs on
   the radio (a TCI client holds control while connected), taking control again and closing apps; a radio still not
   as found is recorded (`left_wrong.json`), its test fails, and every later test refuses it until `bench.py restore`
   puts it back from the snapshot (the owner, 2026-11-22: "The X6200 was left on the wrong frequency ... fix the
   harness. this should NEVER happen").
10. **Every radio said whole, never taken as found.** (The owner, 2026-11-22: "very often during testing you do
   things like forget the preamp, forget to sync channels, forget you muted an audio source, or had attenuation on
   ... ensure this stops happening".) A test names the whole state each radio needs (`bench.rx_profile`,
   `bench.tx_profile`) and `bench.ensure` sets it in order, the dial and mode first (a band change loads that band's
   own ATT, PRE and AGC, so they are set after it), and reads every key back: one not taken is a refusal naming it.
   A listener: its dial, mode, PRE, ATT, RF gain, AGC and squelch as the test says (PRE and ATT have no default),
   no split, RIT, NB or NR, its own receiver on the speaker (LISTEN 0: an SDR heard there puts the base in play mode,
   where the processor gets no receiver audio at all, and tones.py, the decoders and TCI get nothing), no TCI over
   muting it. A sender: on its dial in its mode at its power, the frequency it says it transmits on (`txf`: split, XIT,
   a TX shift) the dial, ATU and TUNE off; for a mic over, out of play mode (both mics at 0 there). Arming on A or G
   puts the data modes into the voice mode: `bench.arm(r, True, mode=...)` sets the test's again. The radios' GUI
   reports all of it (the remote's state: txf, split, rit, xit, play, heard, spk, moni, muted, tcim, mic); a GUI
   older than that is refused. `bench.py status` shows it all, and what in it would spoil a test.
11. **The path proved before anything is measured.** `bench.path_proof`: the sender sends 1000 Hz over its TCI at
   0.1 W for 2 s, and the listener's spectrum at the tone must rise 20 dB over its quiet (0.1 W through 70-80 dB:
   40-59 dB). Under it the test is refused, with the listener's state; 15 dB under the last proof of the same pair
   is said. Then each over is watched for it (rule 5's watch): an over whose listener's spectrum around the sender's
   TX frequency never rose 15 dB while the sender's meter showed power is unheard, and the test fails (radios-ft8,
   tones, image, txcal) or stops after 3 in a row (radios-walkie). 2026-11-22: tests ran for hours on a listener
   that heard nothing (its battery nearly flat), each result taken for the receiver's.
12. **A listener's supply.** `bench.listener_gate`: a listener at under 25 % on a supply under 12 V is refused (its
   receiver went deaf like that, 2026-11-22, and it switches itself off at 7.0 V).
13. **No test by hand.** An ad-hoc script uses the same pieces (`snapshot`, `ensure`, `path_proof`, `Watch`,
   `finish`), never `set` and `key` on their own: the audit of 2026-11-22 found every hand-made one forgetting
   something (a knob turned whose job it could not see, a mode never set, a PTT not released on a failure).

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
At 0.1 W, the first transmission of the session; the watch as radios-walkie's (more power than asked is logged, the
over goes on; a stop kills the sender, since the remote's PTT release does not end a TCI transmission). Faded tones with short gaps
measure the passband. The listener's AGC is off and its squelch open for the run (a shut squelch mutes the audio
whole: the X6200's, at 4, faked a silent transmitter and an image 101-110 dB down on 2026-11-20). The X6100 (gcfix4)
into the X6200, 20 m: 300-2900 Hz within 3.5 dB, the other sideband about 42 dB down (the two radios' images
together), the two radios' oscillators 7.2 Hz apart, the slope 8 ppm.

`tones.py TX RX --bursts 0.05,0.1,0.2,0.4,0.8,1.6,3.2`: the transmitter's power loop on the first sound after a
pause. 1 kHz bursts of 0.5 s with 2 ms starts, each after the pause given; in the tone's band of the listener's audio
each burst's first 80 ms against its steady end (the overshoot), its lowest after that (the dip), how long it takes
to settle within 1 dB, and what came in the pause. Xiegu's X6100 loop: +5 to +18 dB, then 6-14 dB under; with gcfix4
within +1.1 dB (tools/base_update/README.md). Up to 2026-11-21 a burst too hot for the watch (0.6 W at
0.1 W) ended the over, so Xiegu's loop was seen on its first bursts only; it is now logged and every burst is
measured. The sender on the radio is stopped by its pid file (the X6100's
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
done again. The session's first over is a short one at 0.1 W. After a run it saves the radio's calibration.

`txcal_store.py save RADIO [FILE]` / `apply RADIO FILE` / `show FILE` (2026-11-20, the owner: "store the config tied to
something like unique processor ID or serial number"): a radio's TX calibration (every band's txio, txqo, txig, txip)
as calibrations/<fingerprint>.json. The fingerprint is `x6x00-fingerprint`'s (rootfs/usr/sbin): the processor's chip
ID (the R16's SID) with a salt through SHA-256, "x6100-" or "x6200-" and 16 hex digits, with the eMMC's CID hashed the
same way beside it; no raw ID is written anywhere. `apply` refuses a file made on another radio (`--force` for a radio
whose processor board was changed). The GUI keeps the fingerprint the calibration was made on (tx_cal_radio) and clears
the four settings on every band when it starts on another radio. Tested on the X6100 2026-11-20: saved, the record
pointed at another radio, the GUI restarted and cleared every band ("the TX calibration was radio 0003039's, this is
x6100-7e18f3a4ddc28bbd: cleared on every band"), applied back, read back equal; the X6200 refused the X6100's file.
calibrations/x6100-7e18f3a4ddc28bbd.json is the owner's X6100's (the example).

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

`tci_rate.py HOST [--format wsjtx|bench] [--seconds 120]` checks a radio's TCI receive audio over the real network,
as a TCI program gets it (the tests under tools/tci run on the radio over 127.0.0.1 and never see its WiFi): it asks
for a format, answers the server's PINGs (a client that does not is dropped at 60 s), and passes when every 5 s
window after the first holds 95 % of real time. `wsjtx` is TCI's default (48 kHz float32 stereo, 3 Mbit/s), what
WSJT-X takes; `bench` is the recorder's (24 kHz int16 mono). Receive only. 2026-11-21: the X6100 PASS with wsjtx
(100 %), the X6200 FAIL (mean 31 %: its WiFi sends about 1 Mbit/s, TODO.md).

## The two-radio suite (suite.py)

`suite.py` runs batches against both radios at once and writes a report with every screen photographed side by
side (`~/.config/x6x00-bench/runs/<time>-<batches>/report.md`), for a release to break neither radio:

    suite.py list                 # the batches and what each needs plugged in
    suite.py run core             # both radios on the network: versions, services, settings, apps, every screen
    suite.py run rtl,cable        # the hardware batches, when that hardware is on the bench
    suite.py run rx,control       # no RF: every receiver setting, D's and E's rule, TCI, rigctl, every web page
    suite.py run safety,paths     # the cable at 0.1 W: the transmitter's locks; every band, tones, the mic on G,
                                  # RTTY, PSK31, CW, AM/FM, each both ways and confirmed by the other radio
    suite.py run look             # every Settings tab on both, side by side, compared pixel by pixel
    suite.py coverage             # every feature against the latest result of each test of every run so far
    suite.py release              # core and the batches bench.json's "attached" names

**One test per feature, both radios, 1:1** (the owner, 2026-11-22: "a comprehensive set of unit tests to cover
EVERYTHING 1:1 on both radios"). `features.json` lists every feature the manual and the code have (1386: the id,
area, radios, what it needs plugged in, how a test confirms it, the manual's heading). Each result names the features
it covers, and each run writes `coverage.md` beside its report: by area, the features that passed, failed, or are not
covered yet, every uncovered one listed. The batches of `suite_more.py` (`rx`, `control`, `look`, `safety`, `paths`)
run each test on each radio through the harness's state layer (rules 9-13) and add a 1:1 result for the pair.
Reception is confirmed by the other radio every time: its spectrum (the path proof on each band), its audio (tones),
its decoders (CW through the remote's `cw` lines; RTTY and PSK31 made by `digimodes.py` from the radios' own encoders'
rules, sent by the sender's TCI, written by the listener's app into its text log), or, for the X6200's mic, the
X6100's spectrum. The X6200's remote spectrum does not resolve tones inside its passband (a single tone fills
0-2500 Hz), so where it listens the steps are read from the sender's own capture.

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
