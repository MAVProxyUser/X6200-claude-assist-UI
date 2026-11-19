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

`radios-ft8` first ran 2026-11-18 (the X6100 and the X6200 through 40 + 30 dB, 0.1 W, 20 m), both ways: PASS. The
X6200 decoded the X6100 at +11 to +15 dB, the X6100 the X6200 at -2 to -3 dB (the X6200's preamp on, the X6100's
off: a parity run sets them alike). Found by it: the FT8 app opens on its last band and holds the dial (the band keys
move it now, `ft8_to_band`), the log cleaning's script broke through the shells (base64 now), `testcall NAME
restore`, and the radios' power put back after.

`hackrf-iq FILE --at HZ --offset HZ` sends an I/Q file made elsewhere (int8, 2 MS/s by default, the signal OFFSET
above the HackRF's centre) into the radio tuned to HZ in a mode (7: narrow FM; `--filter LO HI` for the test),
puts the radio's frequency, mode, filter and squelch back after, and runs `--during "CMD"` beside it (for example a
capture on the radio over SSH) and shows its output.

`dv_replay.py HOST FILE.iq` tries B's digital voice with no SDR plugged in: a recorded transmission replayed into a
test SDR daemon beside B's, whose DMR or dsd-fme decodes it with the voice through the USB-3000 B's daemon holds; it passes
when a call is decoded and its voice heard (receive only, nothing on the air).

`hackrf-ft8` makes the message with ft8_lib's `gen_ft8` (`gen_ft8` in the configuration), turns it into upper
sideband I/Q at 2.004 MS/s (100 kHz above the HackRF's centre, clear of its DC spike), starts on the next FT8 slot
and passes when the radio's FT8 app shows the text. `radios-ft8` is untried until two radios are on the bench.

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

    suite.py align x6200 x6100           # the X6200's look-and-behaviour settings and time zone onto the X6100
    suite.py align x6200 x6100 --undo FILE

`align` writes the settings core lists as different into the second radio's params.db with its GUI stopped (a GUI
writes its own back as it ends) and copies `/mnt/tz.conf`; the second radio's values before are kept in the run's
folder for `--undo`.
