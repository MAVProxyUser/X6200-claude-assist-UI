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
   itself refuses PTT, TUNE and the FT8 app's CQ, Auto, reply and call from it); the harness refuses to start while
   the radio is on the air or its FT8 app is set to send; a radio that goes on the air while the HackRF feeds it
   stops the HackRF at once. HackRF: `-a 0` (the amp never), `-p 0`, `-x` 40 at most (the owner's approved
   maximum).
4. **The radios profile: low power, power-rated pads.** At most `limits.radio_w` (the configuration; 5 W
   whatever it says), and the first transmission of a session at 0.1 W whatever is asked. Each radio's own jack
   carries a pad rated for at least twice the test power. The power is set and read back before keying; the ATU
   off (it tunes by transmitting); the frequency inside the amateur bands.
5. **Every transmission watched.** A radio on the air longer than `limits.max_key_s` (16 s at most: one FT8
   transmission), its SWR over 2.0, more power than asked, or a receiver hearing 20 dB more than the chain lets
   through (pads missing) stops everything at once: the HackRF killed, every radio unkeyed, the apps' TX stopped.
   The browser remote's PTT is a lease besides: without its renewal the radio unkeys within 3 s.
6. **Test callsigns only.** `Q<digit>TST` (`bench.py testcall NAME Q1TST`, `--restore` after), never a real
   call; FT8 grids FN00. The test QSOs are taken out of the radio's logs after each run (`cleanlog`).
7. **Everything logged.** Each HackRF and radio transmission and each stop goes to `tx_log.jsonl`.
8. **Stop on demand.** Ctrl-C or `bench.py stop` is the emergency stop (rule 5's actions).
9. **The radios as found.** A test closes the apps it opened (the X6200 goes back to the Sentry) and releases
   control; a callsign set for the tests is put back with `testcall NAME --restore`.

## Use

    bench.py provision x6200 rx          # once: a receive-only session (the GUI restarts)
    bench.py chain-ok hackrf "the owner's words"
    bench.py preflight hackrf            # every gate, nothing sent
    bench.py hackrf-ft8 --band 20m --x 30
    bench.py radios-ft8 x6100 x6200      # both radios on the cable, test callsigns set: CQ, then a reply

`hackrf-ft8` makes the message with ft8_lib's `gen_ft8` (`gen_ft8` in the configuration), turns it into upper
sideband I/Q at 2.004 MS/s (100 kHz above the HackRF's centre, clear of its DC spike), starts on the next FT8 slot
and passes when the radio's FT8 app shows the text. `radios-ft8` is untried until two radios are on the bench.
