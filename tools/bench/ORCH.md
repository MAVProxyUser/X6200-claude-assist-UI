# The two-radio orchestrator (orch.py) and its console

`tools/bench/orch.py` is the one way the two test radios (an X6100 and an X6200, joined by a cable through 80 dB of
pads) are tested. It holds both radios, runs every test on them as a pair, and shows the run live on a console at
**http://127.0.0.1:8765/**. The rules it enforces are the owner's (2026-11-23/24):

- **Both radios always on the same frequency and mode.** The only thing that changes from test to test is the role:
  both listen, or one sends and the other listens. A sync guard samples both every 0.2 s (`GUARD_POLL_S`); outside a
  change the orchestrator makes to both, a difference lasting 0.6 s (`GUARD_S`) fails the test it happened in ("OUT
  OF SYNC") and is photographed. A test that needs them apart must declare it (see "Intentional offsets").
- **Every step is confirmed.** A setting is sent to both and the test goes on only once each radio's own state says
  it (`bench.ensure`), then the two are compared (`Orch.both`): never after a fixed wait. Each step is shown on the
  console as running, confirmed or failed, with what it found.
- **Solid pass/fail criteria.** Audio over the air is proved by a stepped tone sequence (600, 1000, 1500, 2100,
  2400 Hz, 0.5 s each, `STEP_HZ`) that the listener must be shown to hear in order, by its spectrum and its TCI audio
  both; data by what was sent against what was decoded (CW, FT8, JS8, RTTY, PSK31, Olivia, WeFax, SSTV, the CW
  Keyboard, digital voice).
- **Nothing passed is run again** unless asked (`--all`, `--soak`); new tests are added and run alone
  (`--only NAME`). A failure is fixed, or worked around in the radio (and the test reshaped to check the radio's real
  behaviour), never left standing: `KNOWN` in orch.py is empty and stays so. Findings are in
  [ORCH_FINDINGS.md](ORCH_FINDINGS.md).
- **Every change gets a test here** (the owner: "ensure that your changes have tests, that in turn get run through
  http://127.0.0.1:8765").

## Running it

    python3 orch.py console --lan        # the console alone, up until stopped (keep it running)
    python3 orch.py list                 # every test, its last result and tally
    python3 orch.py list --md            # the table below
    python3 orch.py run                  # the tests not passed yet (new and failed): the usual run
    python3 orch.py run --only "dv-dmr,tci-ask x6100" [--lan] [--linger 0]
    python3 orch.py run --all            # every test, the passed ones too (only on the owner's word)
    python3 orch.py run --soak 3         # every test three times, each one's tally kept (only on the owner's word)
    python3 orch.py spec RUN/…_spectrum.json   # replay the spectrum judge on an over's saved frames

`run` options: `--port 8765`, `--lan` (the console on the network as well), `--linger S` (how long a run's own console
stays up after it; 0 with the standing console), `--sql N` (the squelch held, default 0), `--squelch-cal`.

**The standing console** is its own process (`orch.py console --lan`): every run writes its state to
`orch_status.json` each second and, finding port 8765 taken, leaves the serving to it. Between runs it shows the last
run. It must stay up: stop runs with `pkill -f 'orch.py run'`, never the console. After its page changes in orch.py
it is restarted (kill it, `nohup python3 -B orch.py console --lan &`).

### Before a run

- **The bench's configuration** in `~/.config/x6x00-bench/bench.json` (the radios' addresses, the pads of the
  "radios" profile, the limits; [bench.example.json](bench.example.json)) and the remote's tokens there (never in the
  repository).
- **The owner's word on the cable** within the last 12 hours: `python3 bench.py chain-ok radios "their words"`. Every
  over checks it (`bench.chain_gate`); over 5 W also `power-ok`. Orch transmits at 0.1 W (`W`), 1 W at most.
- SSH keys on both radios (root), numpy on this computer, and for the AMBE tests the cross compiler and sysroot
  (`tools/cross_x6200/setup.sh`), dsd-fme's sources (`tools/dsdfme/build.sh`), and macOS's `say`/`afconvert`.
- Both radios powered and on the network; the X6200 on a supply or over 35 % of battery. Neither held by its
  operator: a run takes control only when it is free or the bench's own, and waits (up to 15 minutes, said on the
  console) for an operator at the radio to let go. It never takes control from the operator.

## What a run does

1. Takes control of both radios through their remotes (`bench.Radio`), logs every command it sends.
2. Before every test (`Orch.ready`): neither keyed, both transmitters locked (the Sentry's A/B), the two on one
   frequency and mode, the squelch at 0 and the volume at 0 on both; anything else is put right first, or the run stops.
3. Runs the test: its steps, each confirmed; an over goes through `over()`: both set alike (page G, the receiver
   profile, the power), the bench's gates (`chain_gate`, `radio_tx_gate`, `listener_gate`), the sender armed, the
   Watch (`bench.Watch`: too long keyed, too much power over the pad's rating, a listener hearing nothing), then
   locked again.
4. Records the result in `orch_results.json` (kept across runs), the run's events in `runs/<time>-orch/events.jsonl`
   and its results in `results.json`.
5. Ends with both radios on the Sentry's page A, transmitters locked, on the last test's frequency, alike.

Guards running all the while: the sync guard; a radio that transmits while its role is RX (both stopped,
photographed); a signal over -100 dBm on a receiver with neither radio saying it sends (`RF_ALARM_DBM`: both stopped);
a radio silent for 3 s (`STALE_S`: its GUI's process checked, a crash fails the test, a WiFi stall is said).

### Intentional offsets

The radios are on different frequencies only when a test declares it, for its own overs (the owner: "UNLESS
specifically part of a test that does so INTENTIONALLY"). Nothing else may part them. A test declares the exact
difference, and only that difference passes:

- **The sync guard**: `o.apart = {"freq": 1500}` while the listener is moved, `None` again once both are back on one
  frequency (the image tests). The guard then takes the two dials 1500 Hz apart as in step; any other difference, or
  a mode difference, is still a violation. The declaration is said on the console's events.
- **The over's Watch** (`bench.Watch(..., offsets={"freq": 1500})`): as the sender keys, every receiver setting of the
  other radio is still compared with its own (`bench.PAIR_KEYS`); only the declared key at exactly that amount
  passes. image.py declares its offset the same way.
- The radios are put back on one frequency (and `o.apart` cleared) before the test ends, whatever happens.

## The console (http://127.0.0.1:8765/)

Read only. **Radios**: both live (frequency, mode, on the air or not, power, page, app, link), the sync state.
**Now**: the test and its steps as they go, the progress. **This run**: the tests run now, their results and what
each found (pictures of both screens for a failed one). **Not rerun**: the earlier results, the latest first, with
their tallies and GUI versions. **Where the radios differ**: the caps test's map. **Events**: everything the
orchestrator does, the newest first; `/log` the whole run's events, `/status` the state as JSON, `/shot/<file>` a
screen picture.

## Files

| where | what |
|---|---|
| `~/.config/x6x00-bench/orch_results.json` | each test's last result, passes and runs (what is not rerun) |
| `~/.config/x6x00-bench/orch_status.json` | the run's state for the console, each second |
| `~/.config/x6x00-bench/orch_caps.json` | what each radio takes (the caps test), the tests' guide to their differences |
| `~/.config/x6x00-bench/runs/<time>-orch/` | a run: `events.jsonl`, `results.json`, each over's WAV and spectrum frames, the AMBE and digital voice files |
| `~/.config/x6x00-bench/dv/` | the AMBE tests' speech (this computer's voice) and their built tools |
| `~/.config/x6x00-bench/shots/` | the screen pictures (never committed: they show the operator's locator) |
| `~/.config/x6x00-bench/tx_log.jsonl` | every transmission: who, frequency, power, why |
| on the radios, `/mnt/bench_backups/` | a binary replaced by a hot install, at most one each (never in /root: the root file system has ~90 MB free) |

## The scripts it uses

| file | for |
|---|---|
| [orch.py](orch.py) | the orchestrator, its tests and its console |
| [bench.py](bench.py) | the radios through their remotes (`Radio`), `ensure`, `arm`, the gates, the `Watch`, `tci_send`, `tx_log` |
| [tones.py](tones.py) | `TciRx`: each radio's TCI receiver audio |
| [remote_ws.py](remote_ws.py) | the WebSocket client (the remote's and TCI's) |
| [tci_tone_tx.py](tci_tone_tx.py), [tci_cw_tx.py](tci_cw_tx.py) | run on a radio: tones or CW text into its own TCI (a local client, not asked) |
| [digimodes.py](digimodes.py), [sstv_enc.c](sstv_enc.c), [acoustic.py](acoustic.py) | PSK31/RTTY/Olivia signals, the SSTV picture, G's voiceprint |
| [image.py](image.py) | the image test's analysis (the tone's onset, the audio's floor, the bands); also its own run, the listener 1500 Hz below (declared) |
| [suite.py](suite.py), [suite_more.py](suite_more.py), [suite_proto.py](suite_proto.py), [suite_cover.py](suite_cover.py), [suite_air_b.py](suite_air_b.py) | helpers from the earlier 1:1 suite: pictures compared, rigctl, HTTP, text logs, macros kept and put back |
| [ambe_port.py](ambe_port.py) | run on a radio: its AMBE chip through the SDR daemon's port 127.0.0.1:2460 (`id`, `parity`, `enc`, `dec`) |
| [dv_replay.py](dv_replay.py) | a digital voice call (I/Q) replayed into a test SDR daemon on a radio: the calls it shows, its audio (`--env`, `--log`, `--rate`) |
| [../dsdfme/](../dsdfme) | the calls' generators: `dstar_gen.py`, `p25_gen.cpp` + `p25_mod.py`, `ysf/ysf_gen.py` + `ysf/dmr_to_ambed.c`, `nxdn/nxdn_gen.py`; `mbe_dvsi/mbe_dvsi_test.c` (libmbe-dvsi on the radio: DMR, `p25`) |
| [../rtlspec/dmr_test.c](../rtlspec/dmr_test.c) | `dmr_test gen FRAMES OUT`: the DMR call |

On a radio, the SDR daemon traces its digital voice with `RTLSPEC_DSD_TRACE` in its environment (every dsd-fme line,
the speech it sends to `/tmp/dsd_speech.raw`, the audio path B's listener gets): `dv_replay.py --env
RTLSPEC_DSD_TRACE=1 --log FILE`.

## Adding a test

A test is `Test(name, role, what, fn, est_s)` in `TESTS`; `fn(o)` returns `(ok, says, data)`. Inside, `with
o.step("what is confirmed") as s: ...; s.confirm("what was found")`, `o.both({...})` for settings on both,
`over(o, sender, listener, f, mode, label, body)` for a transmission, `in_parallel(o, fn, wait)` for the same work on
both radios at once, `pair(...)` for a test both ways. A probe that changes a radio's setting puts it back. Run it
alone (`run --only NAME`), then fix what it finds; add a line to ORCH_FINDINGS.md for anything found.

**The earlier 1:1 suite's checks** run through orch without being copied: `suite_checks(module, t_name)` calls the
batch's `t_*` with its `per_radio`/`by_radio` pointed at a collector, so each check (`fn(radio)` or `fn(name, host,
ip)`) is gathered without running; `suite_pair(o, module, t_name, test)` then runs one on both radios at once
(`in_parallel`), takes the bench's control back if a TCI client of the check's had it, and puts both back alike.
`SUITE_PORTED` lists the ones taken (suite_proto's TCI, panel, control, screen, core and web checks); the rest of the
old batches stay outside until each is worth bringing in.

## The tests (132)

Generated by `python3 orch.py list --md`; regenerate it when a test is added.

| # | test | role | about | what passes |
|---|---|---|---|---|
| 1 | `identity` | both | 10 s | the same GUI and apps on both, neither's A running a routine of its own |
| 2 | `services` | both | 15 s | the ports and pages that must answer, and the two that must not |
| 3 | `web-secrets` | both | 5 s | the HTTPS key, the backups and the remote's sessions refused (403) by the Files page |
| 4 | `caps` | both RX | 120 s | every option probed on both at once (frequencies, modes, filters, IF shift, zoom, steps, AGC, NB, NR): what each takes, the differences mapped for the tests |
| 5 | `bands-all` | both RX | 60 s | every band both radios share (same edges), entered on both at once: same frequency and mode |
| 6 | `sync-bands` | both RX | 40 s | both radios through 10 bands/modes together, each confirmed on both |
| 7 | `usb-steps x6100>x6200` | X6100 TX, X6200 RX | 30 s | USB 14.200: a stepped tone sequence from the X6100 heard in order in the X6200's audio |
| 8 | `usb-steps x6200>x6100` | X6200 TX, X6100 RX | 30 s | USB 14.200: a stepped tone sequence from the X6200 heard in order in the X6100's audio |
| 9 | `lsb-steps x6100>x6200` | X6100 TX, X6200 RX | 30 s | LSB 7.200: the stepped sequence heard |
| 10 | `lsb-steps x6200>x6100` | X6200 TX, X6100 RX | 30 s | LSB 7.200: the stepped sequence heard |
| 11 | `am-steps x6100>x6200` | X6100 TX, X6200 RX | 30 s | AM 29.000: the stepped sequence heard |
| 12 | `am-steps x6200>x6100` | X6200 TX, X6100 RX | 30 s | AM 29.000: the stepped sequence heard |
| 13 | `fm-steps x6100>x6200` | X6100 TX, X6200 RX | 30 s | FM 29.600: the stepped sequence heard |
| 14 | `fm-steps x6200>x6100` | X6200 TX, X6100 RX | 30 s | FM 29.600: the stepped sequence heard |
| 15 | `cw-text x6100>x6200` | X6100 TX, X6200 RX | 30 s | CW 14.040: 'VVV TEST DE Q1TST' keyed, the other's decoder must print TEST DE Q1TST |
| 16 | `cw-text x6200>x6100` | X6200 TX, X6100 RX | 30 s | CW 14.040: 'VVV TEST DE Q1TST' keyed, the other's decoder must print TEST DE Q1TST |
| 17 | `power-steps x6100>x6200` | X6100 TX, X6200 RX | 45 s | USB 14.200: a tone at 0.2, 0.5, 1.0 W; the other's level must rise 4.0 and 3.0 dB (2 dB) |
| 18 | `power-steps x6200>x6100` | X6200 TX, X6100 RX | 45 s | USB 14.200: a tone at 0.2, 0.5, 1.0 W; the other's level must rise 4.0 and 3.0 dB (2 dB) |
| 19 | `power-6m x6100>x6200` | X6100 TX, X6200 RX | 45 s | USB 50.200: a tone at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB): no floor under 1 W (the factory X6100's 0.8 W at 0.1 W) |
| 20 | `power-6m x6200>x6100` | X6200 TX, X6100 RX | 45 s | USB 50.200: a tone at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB): no floor under 1 W (the factory X6100's 0.8 W at 0.1 W) |
| 21 | `power-6m-fm x6100>x6200` | X6100 TX, X6200 RX | 45 s | FM 52.525 with no audio: the carrier at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB), or, from the X6100 (its base's ~1 W floor there, FLOOR_6M), stay alike with the GUI's note of it in the log |
| 22 | `power-6m-fm x6200>x6100` | X6200 TX, X6100 RX | 45 s | FM 52.525 with no audio: the carrier at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB), or, from the X6100 (its base's ~1 W floor there, FLOOR_6M), stay alike with the GUI's note of it in the log |
| 23 | `power-6m-am x6100>x6200` | X6100 TX, X6200 RX | 45 s | AM 50.400 with no audio: the carrier at 0.1, 0.3, 1.0 W; as power-6m-fm |
| 24 | `power-6m-am x6200>x6100` | X6200 TX, X6100 RX | 45 s | AM 50.400 with no audio: the carrier at 0.1, 0.3, 1.0 W; as power-6m-fm |
| 25 | `ft8-cq x6100>x6200` | X6100 TX, X6200 RX | 40 s | FT8 14.074: a CQ in the next slot, the other must decode exactly the message sent |
| 26 | `ft8-cq x6200>x6100` | X6200 TX, X6100 RX | 40 s | FT8 14.074: a CQ in the next slot, the other must decode exactly the message sent |
| 27 | `psk31-text x6100>x6200` | X6100 TX, X6200 RX | 45 s | PSK31 on the listener's app dial: 'CQ CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 28 | `psk31-text x6200>x6100` | X6200 TX, X6100 RX | 45 s | PSK31 on the listener's app dial: 'CQ CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 29 | `rtty-text x6100>x6200` | X6100 TX, X6200 RX | 120 s | RTTY made to the listener's own settings: 'RYRY CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 30 | `rtty-text x6200>x6100` | X6200 TX, X6100 RX | 120 s | RTTY made to the listener's own settings: 'RYRY CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 31 | `wefax-chart x6100>x6200` | X6100 TX, X6200 RX | 60 s | WeFax: 12 s of a half-white, half-black chart; the other's saved fax must show the two halves at least 80 grey levels apart |
| 32 | `wefax-chart x6200>x6100` | X6200 TX, X6100 RX | 60 s | WeFax: 12 s of a half-white, half-black chart; the other's saved fax must show the two halves at least 80 grey levels apart |
| 33 | `tune-carrier x6100>x6200` | X6100 TX, X6200 RX | 25 s | TUNE at 0.1 W: on the air, heard 15 dB up, off by itself 2.5-5 s after the lease's last renewal |
| 34 | `tune-carrier x6200>x6100` | X6200 TX, X6100 RX | 25 s | TUNE at 0.1 W: on the air, heard 15 dB up, off by itself 2.5-5 s after the lease's last renewal |
| 35 | `ft8-qso x6100>x6200` | X6100 TX, X6200 RX | 180 s | FT8: a whole QSO by the apps (Auto), logged on both, one ADIF record each (taken out after) |
| 36 | `ft8-qso x6200>x6100` | X6200 TX, X6100 RX | 180 s | FT8: a whole QSO by the apps (Auto), logged on both, one ADIF record each (taken out after) |
| 37 | `ft8-stop x6100>x6200` | X6100 TX, X6200 RX | 35 s | FT8: ft8 stop 2 s into a CQ takes the sender off the air within 1.5 s (its state's word), not again |
| 38 | `ft8-stop x6200>x6100` | X6200 TX, X6100 RX | 35 s | FT8: ft8 stop 2 s into a CQ takes the sender off the air within 1.5 s (its state's word), not again |
| 39 | `tci-control` | both RX | 20 s | TCI vfo and modulation on both radios' own TCI: each confirmed by its own state, the two alike |
| 40 | `rigctl` | both RX | 15 s | rigctl F/f M/m on both: confirmed by each radio's state, alike; its PTT refused |
| 41 | `dial-lock` | both RX | 15 s | the dial lock on both: a set refused with the dial kept; off, the set taken |
| 42 | `remote-security` | both | 15 s | a stranger's token: watch only, its set/PTT/knob/control/TUNE/FT8 each refused |
| 43 | `rx-session-refused` | both | 15 s | the receive-only session's PTT, TUNE and FT8 CQ refused, nothing keyed |
| 44 | `apps-alike` | both RX | 60 s | JS8, Olivia, SSTV, WeFax, NavTex, PSK each opened on both at once: the same dial |
| 45 | `ft8-reply x6100>x6200` | X6100 TX, X6200 RX | 60 s | FT8: a CQ, the other decodes it and replies; the first must decode exactly the reply |
| 46 | `ft8-reply x6200>x6100` | X6200 TX, X6100 RX | 60 s | FT8: a CQ, the other decodes it and replies; the first must decode exactly the reply |
| 47 | `remote-busy x6100` | X6100 TX, X6200 RX | 20 s | while the X6100 is on the air another session cannot take control, the X6200 never keyed |
| 48 | `remote-busy x6200` | X6200 TX, X6100 RX | 20 s | while the X6200 is on the air another session cannot take control, the X6100 never keyed |
| 49 | `band-memory` | both RX | 20 s | each band its own ATT: 20 m on, 15 m off, each back from its memory on both |
| 50 | `cb-channel` | both RX | 15 s | CB 19 as G's channel, then a channel up: 27.205 AM on both |
| 51 | `js8-text x6100>x6200` | X6100 TX, X6200 RX | 90 s | JS8: a directed text, the other's list must show it from the sender with the text exact |
| 52 | `js8-text x6200>x6100` | X6200 TX, X6100 RX | 90 s | JS8: a directed text, the other's list must show it from the sender with the text exact |
| 53 | `sstv-picture x6100>x6200` | X6100 TX, X6200 RX | 60 s | SSTV: a Robot 8 picture (two white quarters, two black) made by the GUI's own encoder; the other's saved picture must show the white quarters 80 grey levels over the black |
| 54 | `sstv-picture x6200>x6100` | X6200 TX, X6100 RX | 60 s | SSTV: a Robot 8 picture (two white quarters, two black) made by the GUI's own encoder; the other's saved picture must show the white quarters 80 grey levels over the black |
| 55 | `olivia-text x6100>x6200` | X6100 TX, X6200 RX | 75 s | Olivia: the sender's app sends a macro 'TEST'; the other's text log must show it in the variant sent |
| 56 | `olivia-text x6200>x6100` | X6200 TX, X6100 RX | 75 s | Olivia: the sender's app sends a macro 'TEST'; the other's text log must show it in the variant sent |
| 57 | `cw-kbd x6100>x6200` | X6100 TX, X6200 RX | 45 s | CW Keyboard 14.040: a stored message 'VVV VVV Q1TST' keyed by the app; the other's decoder must print Q1TST |
| 58 | `cw-kbd x6200>x6100` | X6200 TX, X6100 RX | 45 s | CW Keyboard 14.040: a stored message 'VVV VVV Q1TST' keyed by the app; the other's decoder must print Q1TST |
| 59 | `pages-look` | both RX | 90 s | the Sentry's pages A-H on both, photographed together: the Sentry's colours on each, the two alike |
| 60 | `g-voiceprint x6100>x6200` | X6100 TX (its mic), X6200 RX (its speaker plays) | 45 s | G's voiceprint on the air: the X6200 plays stepped tones on its speaker, the X6100 keyed on G at 0.1 W picks them up on its mic; its voiceprint (two pictures), its own capture and the X6200's receiver must show them |
| 61 | `ext-tune x6100>x6200` | X6100 TX, X6200 RX | 60 s | TUNE for an external tuner from the FT8 app's Ext tune at 0.1 W: a tone at its TX offset in the other's audio, off on the second press, the app and dial kept, off by its time limit |
| 62 | `ext-tune x6200>x6100` | X6200 TX, X6100 RX | 60 s | TUNE for an external tuner from the FT8 app's Ext tune at 0.1 W: a tone at its TX offset in the other's audio, off on the second press, the app and dial kept, off by its time limit |
| 63 | `js8-close-to-g` | both RX | 90 s | JS8 closed back to G on CB 20 AM, five times on both: no GUI dies (the X6200's SIGSEGV of 20:36) |
| 64 | `system-health` | both | 10 s | battery log fresh, clock within 1.5 s, the data partition read-write, on both |
| 65 | `rx-vfo-split` | both RX | 30 s | VFO B and back, split, RIT and XIT at +200/-200/0: each set on both, read back alike |
| 66 | `tci-receiver` | both RX | 30 s | TCI's filter, AGC, RIT, a VFO in capitals and an unknown command on both radios' TCI: each confirmed by the radio's state, the two alike |
| 67 | `rigctl-more` | both RX | 20 s | rigctl's levels, split (its setting refused) and state dump on both, nothing keyed |
| 68 | `remote-apps` | both RX | 20 s | the remote's apps list the same on both (more than ten); WSPR's opening refused on both |
| 69 | `power-cap` | both RX | 15 s | 20 W asked of both: each reads its own top (X6100 10 W, X6200 8 W, 5 W on a battery), nothing keyed, 0.1 W after |
| 70 | `web-pages` | both | 120 s | every page and GET API of the web configurator: answered alike on both, none with a server error |
| 71 | `tci-out-of-band x6100` | both RX | 15 s | a TCI audio key from a program on the X6100 on 15.000 MHz, armed: refused, off the air |
| 72 | `tci-out-of-band x6200` | both RX | 15 s | a TCI audio key from a program on the X6200 on 15.000 MHz, armed: refused, off the air |
| 73 | `tci-ask x6100` | both RX | 15 s | a network TCI client's trx on the X6100, armed: asked on its screen, not keyed; the question gone ~20 s after the client leaves |
| 74 | `tci-ask x6200` | both RX | 15 s | a network TCI client's trx on the X6200, armed: asked on its screen, not keyed; the question gone ~20 s after the client leaves |
| 75 | `tci-cw-usb x6100` | both RX | 15 s | CW text through the X6100's TCI in USB, armed: refused, off the air, nobody asked |
| 76 | `tci-cw-usb x6200` | both RX | 15 s | CW text through the X6200's TCI in USB, armed: refused, off the air, nobody asked |
| 77 | `tci-key-cw x6100` | both RX | 15 s | a TCI audio key on the X6100 in CW, armed: refused, off the air |
| 78 | `tci-key-cw x6200` | both RX | 15 s | a TCI audio key on the X6200 in CW, armed: refused, off the air |
| 79 | `tci-answers` | both | 30 s | the 1:1 suite's tci-answers (tci proto) on both radios at once: dds, start/stop and TCI's fixed answers as the code has them, the unsupported CW commands left unanswered |
| 80 | `tci-cw-speed` | both | 30 s | the 1:1 suite's tci-cw-speed (tci proto) on both radios at once: the KEY speed asked (control kept), set (both names echoed, control to TCI), 99 clamped to the radio's top, put back |
| 81 | `tci-rx-sensors` | both | 30 s | the 1:1 suite's tci-rx-sensors (tci proto) on both radios at once: rx_smeter ten a second always, rx_channel_sensors only while enabled |
| 82 | `tci-audio` | both | 30 s | the 1:1 suite's tci-audio (tci proto) on both radios at once: the greeting's audio defaults, 24k int16 mono 512 set and streamed, none after stop |
| 83 | `tci-drive` | both | 30 s | the 1:1 suite's tci-drive (tci drive) on both radios at once: drive read and set lower: the power follows, nothing keyed |
| 84 | `tci-four` | both | 30 s | the 1:1 suite's tci-four (tci guards) on both radios at once: four TCI clients at most, the fifth refused |
| 85 | `tci-origin` | both | 30 s | the 1:1 suite's tci-origin (tci guards) on both radios at once: a page from another site, a rebound name and Pat's mailbox refused; the radio's own page and a program without Origin taken |
| 86 | `panel-greeting` | both | 30 s | the 1:1 suite's panel-greeting (panel) on both radios at once: the remote's hello, keys list and token |
| 87 | `panel-bands-plan` | both | 30 s | the 1:1 suite's panel-bands-plan (panel) on both radios at once: the bands plan as the remote sends it |
| 88 | `panel-view-only` | both | 30 s | the 1:1 suite's panel-view-only (panel) on both radios at once: a view-only session sends nothing |
| 89 | `panel-ping-clock` | both | 30 s | the 1:1 suite's panel-ping-clock (panel) on both radios at once: the link's ping and the clock's answer |
| 90 | `panel-meter` | both | 30 s | the 1:1 suite's panel-meter (panel) on both radios at once: the S-meter and the battery in the remote's state |
| 91 | `panel-kt-cwrise` | both | 30 s | the 1:1 suite's panel-kt-cwrise (panel) on both radios at once: the CW rise time read |
| 92 | `panel-filter-am` | both | 30 s | the 1:1 suite's panel-filter-am (panel) on both radios at once: AM/FM filters as the panel sets them |
| 93 | `panel-refused-set` | both | 30 s | the 1:1 suite's panel-refused-set (panel) on both radios at once: a refused setting reverted on the panel |
| 94 | `panel-play-guard` | both | 30 s | the 1:1 suite's panel-play-guard (panel) on both radios at once: a file play refused where it must be |
| 95 | `panel-wide` | both | 30 s | the 1:1 suite's panel-wide (panel) on both radios at once: the wide spectrum as the remote sends it |
| 96 | `control-needed` | both | 30 s | the 1:1 suite's control-needed (control owner) on both radios at once: a setting without control refused |
| 97 | `control-lost-free` | both | 30 s | the 1:1 suite's control-lost-free (control owner) on both radios at once: control free again when its holder leaves |
| 98 | `screen-stream` | both | 30 s | the 1:1 suite's screen-stream (screen) on both radios at once: the screen's JPEG stream and its quality |
| 99 | `screen-viewers` | both | 30 s | the 1:1 suite's screen-viewers (screen) on both radios at once: the most screen viewers at once |
| 100 | `remote-origin` | both | 30 s | the 1:1 suite's remote-origin (screen) on both radios at once: the remote's WebSocket refuses other sites, takes its own names |
| 101 | `band-cycle` | both | 30 s | the 1:1 suite's band-cycle (core) on both radios at once: the band keys through every band, 60 m included, the untunable skipped |
| 102 | `broadcast-am` | both | 30 s | the 1:1 suite's broadcast-am (core) on both radios at once: the broadcast bands in AM |
| 103 | `tci-refused-answer` | both | 30 s | the 1:1 suite's tci-refused-answer (core) on both radios at once: a TCI tune the radio refuses (an app's hold) answered with what it kept |
| 104 | `knobs` | both | 30 s | the 1:1 suite's knobs (core) on both radios at once: the knobs through the remote tune the radio |
| 105 | `wide-zoom` | both | 30 s | the 1:1 suite's wide-zoom (core) on both radios at once: the X6200's wide zooms |
| 106 | `web-tci` | both | 30 s | the 1:1 suite's web-tci (web guards) on both radios at once: the web TCI page |
| 107 | `web-guards` | both | 30 s | the 1:1 suite's web-guards (web guards) on both radios at once: the web's guarded pages refuse what they must |
| 108 | `funnel-sim` | both | 30 s | the 1:1 suite's funnel-sim (web guards) on both radios at once: a Funnel (internet) connection's limits |
| 109 | `web-https` | both | 30 s | the 1:1 suite's web-https (web more) on both radios at once: HTTPS served with its own certificate |
| 110 | `wss-proxy` | both | 30 s | the 1:1 suite's wss-proxy (web more) on both radios at once: the WebSocket proxy over HTTPS |
| 111 | `ssh-key-checks` | both | 30 s | the 1:1 suite's ssh-key-checks (web more) on both radios at once: the SSH keys page refuses bad keys |
| 112 | `backup-parts` | both | 30 s | the 1:1 suite's backup-parts (web more) on both radios at once: the backup's parts listed |
| 113 | `textlog-page` | both | 30 s | the 1:1 suite's textlog-page (web more) on both radios at once: the text log page |
| 114 | `system-checks` | both | 60 s | each radio read over SSH, nothing changed: battery log and model, data partition, settings' good copy, password logins refused, Tailscale, logs, start trace, services and ports, CPU, temperature, GPU, sessions file, update state, ADIF header, libft8, clock; the same version on both |
| 115 | `image x6100` | X6100 TX, X6200 RX (1500 Hz below, declared) | 120 s | the X6100's opposite-sideband image and carrier leak on 20, 17, 15, 12 and 10 m at 0.1 W: each image -35 dBc or lower (its 15 m shifts, 11-22); the carrier leak said |
| 116 | `image x6200` | X6200 TX, X6100 RX (1500 Hz below, declared) | 120 s | the X6200's image and carrier leak, as image x6100 |
| 117 | `ambe-vocoder` | both RX | 150 s | each radio's AMBE chip through its SDR daemon: the parity field asked for, the speech through each chip at DMR's and D-STAR's rates and the other's frames, libmbe-dvsi alike with the parity field off, P25 full rate only on a chip with the P25 option |
| 118 | `dv-dmr` | both RX | 70 s | a DMR call of the test speech replayed into each radio's SDR daemon: TG 3100 from 3112345 shown, its voice through the radio's own AMBE chip like the speech (r >= 0.9) |
| 119 | `dv-dmr-noparity` | both RX | 60 s | the DMR call again with both chips' parity field off: the test daemon reads the chip's answers whole through B's daemon's port; the field on again after |
| 120 | `dv-dstar` | both RX | 60 s | a D-STAR call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.4) |
| 121 | `dv-ysf` | both RX | 60 s | a YSF call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.7) |
| 122 | `dv-nxdn` | both RX | 60 s | an NXDN48 call of the test speech: TG 100 from 1234 shown, its voice heard (r >= 0.75) |
| 123 | `dv-p25` | both RX | 60 s | a P25 phase 1 call (1 kHz IMBE frames): TG 4321 from 1234567 shown; the tone heard where the chip has DVSI's P25 option, silence where it has not |
| 124 | `tci-audio-rate` | both RX | 15 s | each radio's TCI receiver audio at 97-103 % of real time |
| 125 | `tci-takes-control` | both RX | 20 s | a TCI client's set takes control (ctl tci) on both; gone, control back |
| 126 | `rx-settings` | both RX | 40 s | PRE, ATT, AGC, NB, NR, squelch, RF gain: each set on both, read back alike |
| 127 | `ptt-lease x6100` | X6100 TX, X6200 RX | 20 s | one PTT never renewed: off the air by itself within 4 s |
| 128 | `ptt-lease x6200` | X6200 TX, X6100 RX | 20 s | one PTT never renewed: off the air by itself within 4 s |
| 129 | `ptt-locked x6100` | both RX | 12 s | the X6100's PTT with its transmitter locked: stays off the air |
| 130 | `ptt-locked x6200` | both RX | 12 s | the X6200's PTT with its transmitter locked: stays off the air |
| 131 | `ptt-out-of-band x6100` | both RX | 12 s | the X6100 armed on 15.000 MHz (WWV, no amateur band): PTT refused |
| 132 | `ptt-out-of-band x6200` | both RX | 12 s | the X6200 armed on 15.000 MHz (WWV, no amateur band): PTT refused |
