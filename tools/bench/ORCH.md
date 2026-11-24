# The two-radio orchestrator (orch.py) and its console

`tools/bench/orch.py` is the one way the two test radios (an X6100 and an X6200, joined by a cable through 80 dB of
pads) are tested. It holds both radios, runs every test on them as a pair, and shows the run live on a console at
**http://127.0.0.1:8765/**. The rules it enforces are the owner's (2026-11-23/24):

- **Both radios always on the same frequency and mode.** The only thing that changes from test to test is the role:
  both listen, or one sends and the other listens. A sync guard samples both every 0.2 s (`GUARD_POLL_S`); outside a
  transition, a difference lasting 0.6 s (`GUARD_S`) that no test declared fails the test it happened in ("OUT OF
  SYNC") and is photographed (see "The one sync framework").
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

### The one sync framework: no deviation that is not declared

The owner, 2026-11-24: "EVERY test should use the same unified framework to sync the radios or make exceptions for
tests so that there is NO deviation". Every test, orch's own and the earlier suite's run through orch, goes through
one framework in `bench.py`:

- **One way to set the two alike: `bench.sync_pair(radios, want)`**: both radios set at once (each read back by
  `ensure`), then compared; a difference that is not declared is refused. orch's `both()` and `ready()`, the suite's
  `sync_both`/`unsync_both` (both radios taken, page A on both, the set-up and the put-back together) and
  `pair_align` (the sender given the listener's set-up) all call it.
- **One registry of declared differences: `with bench.apart(spec, reason):`**. A test that sets the radios apart on
  purpose says what and why, and only that passes. A rule is an exact distance (`{"freq": 15000}`: the two exactly
  15 kHz apart, either way), an exact pair of values (`{"split": (1, 0)}`), a list of pairs, one for each phase of
  the test (`{"mode": [(6, 2), (6, 8)]}`), or `bench.STEPPED`, a setting the test sets or steps itself as what it
  measures (its reason says how; `"filter"` covers the filter's edges). Every declaration in force is shown on the
  console's sync line.
- **One reader of it**: `bench.Watch` (as a radio keys, every receiver setting of the other compared: a difference
  not declared stops the over, and a setting a test keeps apart without declaring it is named) and orch's sync guard
  (the dial and the mode, every 0.2 s, never paused) both call `bench.declared()`.
- **A test ends with the two alike**: tests leave the dial where they ended (the owner, 11-23), so `finish()` ends a
  two-radio test with the idle radio following: the second set to the first's dial and mode by `sync_pair`.
- **Transitions are not differences**: `ensure()`, `open_page()`, `arm()` and `finish()` (the put-back of both radios
  at a test's end) are transitions while they run, and a dial set on one radio leaves the other `DIAL_FOLLOW_S` (8 s,
  one test step) to follow; once the two are alike the window closes. A command sent through `Radio.send` that may
  move a dial outside `ensure()` (`bench.DIAL_CMDS`: an app opening on its own memory, an app closing, a raw
  `set freq`) opens the same window; keys do not. `bench.open_page_both()` opens a page on both radios as one
  transition and sets them alike again (G tunes each radio to its own last channel). A test that tunes one radio
  alone for longer (text-log: the listener's PSK app opened to see where it listens) holds a `bench.transition()`,
  waits for its dial to settle (`bench.settled`) and sets both with `sync_pair`. orch's own changes to both (`both()`, `in_parallel`: the same steps on both radios at once) count the
  same.

The declarations in the tests today (each with its reason in the code):

| test | declared |
|---|---|
| image x6100/x6200 | `freq` 1500: the listener 1500 Hz below, the image's place in its USB audio |
| air split | `freq` 15000, `split` (1, 0), `vfo` (1, 0), `mode` [(6, 3)], `filter` STEPPED: TX on B, the listener between |
| air split-cw | `freq` 10000, `split` (1, 0), `vfo` (1, 0): keyed on the listen VFO A |
| air qsy-out | `freq` 20000: the listener midway between 14.320 and the 14.360 the Panel tunes to under PTT |
| air smeter-am | `freq` 1000, `mode` [(6, 2), (6, 8)], `filter` STEPPED: the carrier read in USB, then AM and SAM |
| air codec-clock | `freq` exactly the tone's place across the listener's span |
| air TUNE (every tune over) | `mode` [(3, the listener's)], `filter` STEPPED: TUNE sends as USB-D |
| air smeter-front-end | `pre`, `att`, `rfg` STEPPED: the listener's own Measure steps them, then the test does |
| air keys-with-ptt | `nr`, `nb` STEPPED: DFN turns them under PTT |
| air sentry-lock | `freq`, `mode`, `filter`, `agc`, `zoom` STEPPED: A's scan moves the listener (the subject) |

sim_test.py checks the framework itself (every rule, the Watch reading the registry, transitions, sync_pair).

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
`SUITE_PORTED` lists the ones taken from suite_proto (TCI, panel, control, screen, core and web checks) and
`SUITE_PORTED_COVER_A` those from suite_cover_a (Bluetooth, the backlight and screen, the screensaver, themes, zoom and
step, the band plan, own bands, mode from the plan, the lock per band, channel snap, speaker, line in, transverters,
TX shift, TX outside the bands). A check that needs no remote client on the radios (the screensaver: a client keeps
it away) runs with `alone=True`: orch closes its own sessions for it, their silence planned, then connects again.
The rest of the old batches stay outside until each is worth bringing in.

## Over-the-air coverage (every transmission test, 0.1 W over the 80 dB cable unless said)

orch's own tests and the earlier suite's over-the-air groups, run whole through orch (`air ...`: `suite_group` runs a
batch's `t_*` with a `suite.Run` of its own, its sync_both and put-back through the one framework, a `bench.Watch` on
every over; orch's sync guard stays on, every deviation in them declared; orch takes control back and puts both alike
after; the group passes with no row failed). Every test runs both ways unless it says otherwise.

| area | orch's own | the suite's, run whole (`air ...`) |
|---|---|---|
| voice and tones | usb/lsb/am/fm-steps (the stepped tones heard in order), g-voiceprint (G's mic) | air paths walkie (AM/FM on 20 and 10 m) |
| every band | sync-bands, bands-all (receive) | air paths bands (an over on each band 160-6 m) |
| power | power-steps (20 m), power-6m, power-6m-fm, power-6m-am (the X6100's base floor checked with the GUI's note), power-cap (receive) | air tune-power, air power-loop (bursts after pauses), air cw-alc, air comp |
| TUNE | tune-carrier, ext-tune | air tune, air tune-app, air tune-key, air qsy-out |
| PTT and its refusals | ptt-lease, ptt-locked, ptt-out-of-band, rx-session-refused, remote-security, remote-busy | air keys-with-ptt, air armed-ptt-a, air sentry-lock |
| split and TX shift | (image x6100/x6200: the listener 1500 Hz below, declared) | air split, air split-cw |
| CW | cw-text, cw-kbd | air cw-copy (extended letters, the VFO line), air cw-carrier, air cw-kbd (stop) |
| FT8/FT4/FT2 | ft8-cq, ft8-reply, ft8-qso, ft8-stop | air ft8-tx (level, ALC), air ft8-qso (FT8/FT4/FT2), air ft8-multistream, air ft8-hold, air ft8-late |
| JS8 | js8-text, js8-close-to-g | air js8 (CQ, directed, heartbeat, inbox, stop) |
| PSK, RTTY, Olivia, JTTY | psk31-text, rtty-text, olivia-text | air psk (macros, stop), air rtty, air olivia (Olivia and Contestia modes), air jtty |
| WeFax, SSTV | wefax-chart, sstv-picture | air wefax-cont, air wefax-auto |
| TCI | tci-out-of-band, tci-ask, tci-cw-usb, tci-key-cw (refusals), tci-drive | air tci-mic, air tci-audio, air tci-cw, air tci-level, air tci-ask (asked, then on the air) |
| receivers while one sends | (every over's listener judged) | air smeter-front-end, air smeter-am, air readings-zero, air band-edges, air g-moni, air codec-clock, air text-log |
| IF shift, images | image x6100, image x6200 | air paths if-shift (the X6100's) |

Not over the air in orch yet: nothing of the suite's over-the-air batches is left out except what orch has already
(power-steps, ft8-stop, remote-busy, ext-tune, ft8-cq) and the paths parts it repeats (tones, mic-on-g, cw, rtty,
psk31). What still has no test at all is in features.json's coverage (about 1000 of 1397 features, most of them the
apps' and the screens' details).

## The tests (263)

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
| 70 | `smc-remote` | both RX | 30 s | TX Cal's S-meter Measure from the remote on both: its figures read, a Measure on the noise refused with nothing changed, PRE put a dB off and back, each read back |
| 71 | `web-pages` | both | 120 s | every page and GET API of the web configurator: answered alike on both, none with a server error |
| 72 | `tci-out-of-band x6100` | both RX | 15 s | a TCI audio key from a program on the X6100 on 15.000 MHz, armed: refused, off the air |
| 73 | `tci-out-of-band x6200` | both RX | 15 s | a TCI audio key from a program on the X6200 on 15.000 MHz, armed: refused, off the air |
| 74 | `tci-ask x6100` | both RX | 15 s | a network TCI client's trx on the X6100, armed: asked on its screen, not keyed; the question gone ~20 s after the client leaves |
| 75 | `tci-ask x6200` | both RX | 15 s | a network TCI client's trx on the X6200, armed: asked on its screen, not keyed; the question gone ~20 s after the client leaves |
| 76 | `tci-cw-usb x6100` | both RX | 15 s | CW text through the X6100's TCI in USB, armed: refused, off the air, nobody asked |
| 77 | `tci-cw-usb x6200` | both RX | 15 s | CW text through the X6200's TCI in USB, armed: refused, off the air, nobody asked |
| 78 | `tci-key-cw x6100` | both RX | 15 s | a TCI audio key on the X6100 in CW, armed: refused, off the air |
| 79 | `tci-key-cw x6200` | both RX | 15 s | a TCI audio key on the X6200 in CW, armed: refused, off the air |
| 80 | `tci-answers` | both | 30 s | the 1:1 suite's tci-answers (tci proto) on both radios at once: dds, start/stop and TCI's fixed answers as the code has them, the unsupported CW commands left unanswered |
| 81 | `tci-cw-speed` | both | 30 s | the 1:1 suite's tci-cw-speed (tci proto) on both radios at once: the KEY speed asked (control kept), set (both names echoed, control to TCI), 99 clamped to the radio's top, put back |
| 82 | `tci-rx-sensors` | both | 30 s | the 1:1 suite's tci-rx-sensors (tci proto) on both radios at once: rx_smeter ten a second always, rx_channel_sensors only while enabled |
| 83 | `tci-audio` | both | 30 s | the 1:1 suite's tci-audio (tci proto) on both radios at once: the greeting's audio defaults, 24k int16 mono 512 set and streamed, none after stop |
| 84 | `tci-drive` | both | 30 s | the 1:1 suite's tci-drive (tci drive) on both radios at once: drive read and set lower: the power follows, nothing keyed |
| 85 | `tci-four` | both | 30 s | the 1:1 suite's tci-four (tci guards) on both radios at once: four TCI clients at most, the fifth refused |
| 86 | `tci-origin` | both | 30 s | the 1:1 suite's tci-origin (tci guards) on both radios at once: a page from another site, a rebound name and Pat's mailbox refused; the radio's own page and a program without Origin taken |
| 87 | `panel-greeting` | both | 30 s | the 1:1 suite's panel-greeting (panel) on both radios at once: the remote's hello, keys list and token |
| 88 | `panel-bands-plan` | both | 30 s | the 1:1 suite's panel-bands-plan (panel) on both radios at once: the bands plan as the remote sends it |
| 89 | `panel-view-only` | both | 30 s | the 1:1 suite's panel-view-only (panel) on both radios at once: a view-only session sends nothing |
| 90 | `panel-ping-clock` | both | 30 s | the 1:1 suite's panel-ping-clock (panel) on both radios at once: the link's ping and the clock's answer |
| 91 | `panel-meter` | both | 30 s | the 1:1 suite's panel-meter (panel) on both radios at once: the S-meter and the battery in the remote's state |
| 92 | `panel-kt-cwrise` | both | 30 s | the 1:1 suite's panel-kt-cwrise (panel) on both radios at once: the CW rise time read |
| 93 | `panel-filter-am` | both | 30 s | the 1:1 suite's panel-filter-am (panel) on both radios at once: AM/FM filters as the panel sets them |
| 94 | `panel-refused-set` | both | 30 s | the 1:1 suite's panel-refused-set (panel) on both radios at once: a refused setting reverted on the panel |
| 95 | `panel-play-guard` | both | 30 s | the 1:1 suite's panel-play-guard (panel) on both radios at once: a file play refused where it must be |
| 96 | `panel-wide` | both | 30 s | the 1:1 suite's panel-wide (panel) on both radios at once: the wide spectrum as the remote sends it |
| 97 | `control-needed` | both | 30 s | the 1:1 suite's control-needed (control owner) on both radios at once: a setting without control refused |
| 98 | `control-lost-free` | both | 30 s | the 1:1 suite's control-lost-free (control owner) on both radios at once: control free again when its holder leaves |
| 99 | `screen-stream` | both | 30 s | the 1:1 suite's screen-stream (screen) on both radios at once: the screen's JPEG stream and its quality |
| 100 | `screen-viewers` | both | 30 s | the 1:1 suite's screen-viewers (screen) on both radios at once: the most screen viewers at once |
| 101 | `remote-origin` | both | 30 s | the 1:1 suite's remote-origin (screen) on both radios at once: the remote's WebSocket refuses other sites, takes its own names |
| 102 | `band-cycle` | both | 30 s | the 1:1 suite's band-cycle (core) on both radios at once: the band keys through every band, 60 m included, the untunable skipped |
| 103 | `broadcast-am` | both | 30 s | the 1:1 suite's broadcast-am (core) on both radios at once: the broadcast bands in AM |
| 104 | `tci-refused-answer` | both | 30 s | the 1:1 suite's tci-refused-answer (core) on both radios at once: a TCI tune the radio refuses (an app's hold) answered with what it kept |
| 105 | `knobs` | both | 30 s | the 1:1 suite's knobs (core) on both radios at once: the knobs through the remote tune the radio |
| 106 | `wide-zoom` | both | 30 s | the 1:1 suite's wide-zoom (core) on both radios at once: the X6200's wide zooms |
| 107 | `web-tci` | both | 30 s | the 1:1 suite's web-tci (web guards) on both radios at once: the web TCI page |
| 108 | `web-guards` | both | 30 s | the 1:1 suite's web-guards (web guards) on both radios at once: the web's guarded pages refuse what they must |
| 109 | `funnel-sim` | both | 30 s | the 1:1 suite's funnel-sim (web guards) on both radios at once: a Funnel (internet) connection's limits |
| 110 | `web-https` | both | 30 s | the 1:1 suite's web-https (web more) on both radios at once: HTTPS served with its own certificate |
| 111 | `wss-proxy` | both | 30 s | the 1:1 suite's wss-proxy (web more) on both radios at once: the WebSocket proxy over HTTPS |
| 112 | `ssh-key-checks` | both | 30 s | the 1:1 suite's ssh-key-checks (web more) on both radios at once: the SSH keys page refuses bad keys |
| 113 | `backup-parts` | both | 30 s | the 1:1 suite's backup-parts (web more) on both radios at once: the backup's parts listed |
| 114 | `textlog-page` | both | 30 s | the 1:1 suite's textlog-page (web more) on both radios at once: the text log page |
| 115 | `btcat-bridge` | both RX | 45 s | the 1:1 suite's cover_a btcat-bridge on both radios at once (it puts back what it changes): the Bluetooth CAT bridge on its serial port, its RFCOMM listening |
| 116 | `btcat-power` | both RX | 45 s | the 1:1 suite's cover_a btcat-power on both radios at once (it puts back what it changes): the Bluetooth adapter's power as set |
| 117 | `bt-pairing` | both RX | 45 s | the 1:1 suite's cover_a bt-pairing on both radios at once (it puts back what it changes): the pairing window opened and closed by itself |
| 118 | `backlight` | both RX | 45 s | the 1:1 suite's cover_a backlight on both radios at once (it puts back what it changes): the backlight setting changes the screen's brightness, put back |
| 119 | `screen-off` | both RX | 45 s | the 1:1 suite's cover_a screen-off on both radios at once (it puts back what it changes): the screen off and on again |
| 120 | `screensaver` | both RX | 150 s | the 1:1 suite's cover_a screensaver on both radios at once (it puts back what it changes): the screensaver after its minute, gone at a touch |
| 121 | `themes` | both RX | 45 s | the 1:1 suite's cover_a themes on both radios at once (it puts back what it changes): each theme taken live, 1bit included, put back |
| 122 | `themes-alike` | both RX | 45 s | the 1:1 suite's cover_a themes-alike on both radios at once (it puts back what it changes): the built-in themes drawn alike on the two radios |
| 123 | `zoom-step` | both RX | 45 s | the 1:1 suite's cover_a zoom-step on both radios at once (it puts back what it changes): the zoom kept per mode |
| 124 | `step-zoom` | both RX | 45 s | the 1:1 suite's cover_a step-zoom on both radios at once (it puts back what it changes): the tuning step kept per zoom |
| 125 | `band-plan` | both RX | 45 s | the 1:1 suite's cover_a band-plan on both radios at once (it puts back what it changes): the band plan's segments as set |
| 126 | `own-band` | both RX | 45 s | the 1:1 suite's cover_a own-band on both radios at once (it puts back what it changes): a band of the operator's own on the main screen's plan |
| 127 | `mode-from-plan` | both RX | 45 s | the 1:1 suite's cover_a mode-from-plan on both radios at once (it puts back what it changes): the mode taken from the band plan as the dial moves |
| 128 | `lock-per-band` | both RX | 45 s | the 1:1 suite's cover_a lock-per-band on both radios at once (it puts back what it changes): the dial lock kept per band |
| 129 | `channel-snap` | both RX | 45 s | the 1:1 suite's cover_a channel-snap on both radios at once (it puts back what it changes): a channel band's dial snapped to its channels |
| 130 | `speaker-off` | both RX | 45 s | the 1:1 suite's cover_a speaker-off on both radios at once (it puts back what it changes): the speaker off keeps the audio elsewhere |
| 131 | `line-in` | both RX | 45 s | the 1:1 suite's cover_a line-in on both radios at once (it puts back what it changes): the line in as the audio source |
| 132 | `transverters` | both RX | 45 s | the 1:1 suite's cover_a transverters on both radios at once (it puts back what it changes): a transverter's offsets and its power limit |
| 133 | `tx-shift` | both RX | 45 s | the 1:1 suite's cover_a tx-shift on both radios at once (it puts back what it changes): a TX shift's frequency, and a shift below 0 Hz refused |
| 134 | `tx-outside` | both RX | 45 s | the 1:1 suite's cover_a tx-outside on both radios at once (it puts back what it changes): transmitting outside the ham bands: the switch refuses it while off |
| 135 | `bootvideo` | both RX | 45 s | the 1:1 suite's cover bootvideo on both radios at once: bootvideo |
| 136 | `channels-web` | both RX | 45 s | the 1:1 suite's cover channels-web on both radios at once: channels web |
| 137 | `config-files` | both RX | 45 s | the 1:1 suite's cover config-files on both radios at once: config files |
| 138 | `filter-cw` | both RX | 45 s | the 1:1 suite's cover filter-cw on both radios at once: filters |
| 139 | `filter-wfm` | both RX | 45 s | the 1:1 suite's cover filter-wfm on both radios at once: filters |
| 140 | `ft8-modes-knob` | both RX | 45 s | the 1:1 suite's cover ft8-modes-knob on both radios at once: ft8 more |
| 141 | `gui-cpu` | both RX | 45 s | the 1:1 suite's cover gui-cpu on both radios at once: The GUI's main thread on the Sentry's A against the manual's table, and a Panel connection (no pictures) costing it nothing: up to v0.3.12.47 any connection had the screen copied for the picture encod |
| 142 | `refusals` | both RX | 45 s | the 1:1 suite's cover refusals on both radios at once: refusals |
| 143 | `rigctl-local` | both RX | 45 s | the 1:1 suite's cover rigctl-local on both radios at once: rigctl local |
| 144 | `sessions-mtime` | both RX | 45 s | the 1:1 suite's cover sessions-mtime on both radios at once: sessions mtime |
| 145 | `smartcw-guard` | both RX | 45 s | the 1:1 suite's cover smartcw-guard on both radios at once: smartcw guard |
| 146 | `tci-switches` | both RX | 45 s | the 1:1 suite's cover tci-switches on both radios at once: tci switches |
| 147 | `bt-tci-service` | both RX | 45 s | the 1:1 suite's cover bt-tci-service on both radios at once: tci switches |
| 148 | `walkie-upright` | both RX | 45 s | the 1:1 suite's cover walkie-upright on both radios at once: walkie upright |
| 149 | `web-macros` | both RX | 45 s | the 1:1 suite's cover web-macros on both radios at once: web writes |
| 150 | `web-ssh-keys` | both RX | 45 s | the 1:1 suite's cover web-ssh-keys on both radios at once: web writes |
| 151 | `web-own-name` | both RX | 45 s | the 1:1 suite's cover web-own-name on both radios at once: web writes |
| 152 | `winlink-pat` | both RX | 45 s | the 1:1 suite's cover winlink-pat on both radios at once: winlink pat |
| 153 | `a-band` | both RX | 45 s | the 1:1 suite's cover_b a-band on both radios at once: a band |
| 154 | `a-scan` | both RX | 45 s | the 1:1 suite's cover_b a-scan on both radios at once: a scan |
| 155 | `a-span` | both RX | 45 s | the 1:1 suite's cover_b a-span on both radios at once: a span |
| 156 | `agc-split` | both RX | 45 s | the 1:1 suite's cover_b agc-split on both radios at once: agc split |
| 157 | `band-across` | both RX | 45 s | the 1:1 suite's cover_b band-across on both radios at once: band across |
| 158 | `c-dwell` | both RX | 45 s | the 1:1 suite's cover_b c-dwell on both radios at once: c dwell |
| 159 | `c-empty` | both RX | 45 s | the 1:1 suite's cover_b c-empty on both radios at once: c empty |
| 160 | `c-listen` | both RX | 45 s | the 1:1 suite's cover_b c-listen on both radios at once: c listen |
| 161 | `close-rtty` | both RX | 45 s | the 1:1 suite's cover_b close-rtty on both radios at once: The remote's close closes the RTTY panel (the one app with no window): opened (E's types, as open_app does), the state's app RTTY, then "close": none within 3 s |
| 162 | `d-e-rule` | both RX | 45 s | the 1:1 suite's cover_b d-e-rule on both radios at once: d e rule |
| 163 | `d-scan-listen` | both RX | 45 s | the 1:1 suite's cover_b d-scan-listen on both radios at once: d scan listen |
| 164 | `d-sweep-mark` | both RX | 45 s | the 1:1 suite's cover_b d-sweep-mark on both radios at once: d sweep mark |
| 165 | `df-keys` | both RX | 45 s | the 1:1 suite's cover_b df-keys on both radios at once: df keys |
| 166 | `df-source` | both RX | 45 s | the 1:1 suite's cover_b df-source on both radios at once: df source |
| 167 | `df-targets` | both RX | 45 s | the 1:1 suite's cover_b df-targets on both radios at once: df targets |
| 168 | `df-tune` | both RX | 45 s | the 1:1 suite's cover_b df-tune on both radios at once: df tune |
| 169 | `e-emergency` | both RX | 45 s | the 1:1 suite's cover_b e-emergency on both radios at once: e emergency |
| 170 | `e-scan-sel` | both RX | 45 s | the 1:1 suite's cover_b e-scan-sel on both radios at once: e scan sel |
| 171 | `f-classic` | both RX | 45 s | the 1:1 suite's cover_b f-classic on both radios at once: f classic |
| 172 | `f-go` | both RX | 45 s | the 1:1 suite's cover_b f-go on both radios at once: f go |
| 173 | `f-open` | both RX | 45 s | the 1:1 suite's cover_b f-open on both radios at once: f open |
| 174 | `f-shell` | both RX | 45 s | the 1:1 suite's cover_b f-shell on both radios at once: f shell |
| 175 | `f-tribal` | both RX | 45 s | the 1:1 suite's cover_b f-tribal on both radios at once: f tribal |
| 176 | `fst` | both RX | 45 s | the 1:1 suite's cover_b fst on both radios at once: fst |
| 177 | `g-3d` | both RX | 45 s | the 1:1 suite's cover_b g-3d on both radios at once: g 3d |
| 178 | `g-list` | both RX | 45 s | the 1:1 suite's cover_b g-list on both radios at once: G's F4 CH LIST on CB: the MFK two down the list and its push tune CB 21 from CB 19 and close the list (the dial moves no more with the MFK: off the list it goes group to group, so a third click would  |
| 179 | `g-own` | both RX | 45 s | the 1:1 suite's cover_b g-own on both radios at once: G opens on its own channel and gives A's dial back as it is left; a channel tuned on G is G's own, a band's dial is not; the remote's wch says it and "set wch" sets the memory alone (a frequency the r |
| 180 | `gen-help` | both RX | 45 s | the 1:1 suite's cover_b gen-help on both radios at once: gen help |
| 181 | `home` | both RX | 45 s | the 1:1 suite's cover_b home on both radios at once: home |
| 182 | `key-actions` | both RX | 45 s | the 1:1 suite's cover_b key-actions on both radios at once: key actions |
| 183 | `keys` | both RX | 45 s | the 1:1 suite's cover_b keys on both radios at once: keys |
| 184 | `knob-bind` | both RX | 45 s | the 1:1 suite's cover_b knob-bind on both radios at once: knob bind |
| 185 | `listen` | both RX | 45 s | the 1:1 suite's cover_b listen on both radios at once: listen |
| 186 | `manual` | both RX | 45 s | the 1:1 suite's cover_b manual on both radios at once: manual |
| 187 | `page-key-hold` | both RX | 45 s | the 1:1 suite's cover_b page-key-hold on both radios at once: page key hold |
| 188 | `pool` | both RX | 45 s | the 1:1 suite's cover_b pool on both radios at once: pool |
| 189 | `scancfg-astep` | both RX | 45 s | the 1:1 suite's cover_b scancfg-astep on both radios at once: scancfg astep |
| 190 | `scancfg-chans` | both RX | 45 s | the 1:1 suite's cover_b scancfg-chans on both radios at once: scancfg chans |
| 191 | `scancfg-open` | both RX | 45 s | the 1:1 suite's cover_b scancfg-open on both radios at once: scancfg open |
| 192 | `scancfg-rec` | both RX | 45 s | the 1:1 suite's cover_b scancfg-rec on both radios at once: scancfg rec |
| 193 | `scancfg-step` | both RX | 45 s | the 1:1 suite's cover_b scancfg-step on both radios at once: scancfg step |
| 194 | `scancfg-style` | both RX | 45 s | the 1:1 suite's cover_b scancfg-style on both radios at once: scancfg style |
| 195 | `scancfg-sweep` | both RX | 45 s | the 1:1 suite's cover_b scancfg-sweep on both radios at once: scancfg sweep |
| 196 | `tune-power-remote` | both RX | 45 s | the 1:1 suite's cover_b tune-power-remote on both radios at once: The remote's tpwr: TUNE's own power read and set, saved in the settings database as Settings does (tenths: 0.2 W is "2"), "set tpwr 0" clamped to 0.1 W; nothing keyed |
| 197 | `user-page` | both RX | 45 s | the 1:1 suite's cover_b user-page on both radios at once: user page |
| 198 | `value-buttons` | both RX | 45 s | the 1:1 suite's cover_b value-buttons on both radios at once: value buttons |
| 199 | `vol-knob` | both RX | 45 s | the 1:1 suite's cover_b vol-knob on both radios at once: vol knob |
| 200 | `walk` | both RX | 45 s | the 1:1 suite's cover_b walk on both radios at once: walk |
| 201 | `x6100-keys` | both RX | 45 s | the 1:1 suite's cover_b x6100-keys on both radios at once: x6100 keys |
| 202 | `air smeter-front-end` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a smeter-front-end over the air, run whole: smeter-front-end |
| 203 | `air readings-zero` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a readings-zero over the air, run whole: readings-zero |
| 204 | `air band-edges` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a band-edges over the air, run whole: band-edges |
| 205 | `air keys-with-ptt` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a keys-with-ptt over the air, run whole: keys-with-ptt |
| 206 | `air armed-ptt-a` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a armed-ptt-a over the air, run whole: armed-ptt-a |
| 207 | `air smeter-am` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a smeter-am over the air, run whole: smeter-am |
| 208 | `air comp` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a comp over the air, run whole: comp |
| 209 | `air split` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a split over the air, run whole: split |
| 210 | `air split-cw` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a split-cw over the air, run whole: split-cw |
| 211 | `air cw-alc` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a cw-alc over the air, run whole: cw-alc |
| 212 | `air qsy-out` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a qsy-out over the air, run whole: qsy-out |
| 213 | `air tune` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune over the air, run whole: tune |
| 214 | `air tune-power` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune-power over the air, run whole: tune-power |
| 215 | `air tune-app` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune-app over the air, run whole: tune-app |
| 216 | `air tune-key` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune-key over the air, run whole: tune-key |
| 217 | `air text-log` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a text-log over the air, run whole: text-log |
| 218 | `air codec-clock` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a codec-clock over the air, run whole: codec-clock |
| 219 | `air power-loop` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a power-loop over the air, run whole: path-tx-power-loop-bursts (tx-x6100-gcfix5-pause-hold from an X6100 on gcfix4/5, at 0.1 and 1 W): tones.py --bursts as a child, each way: every burst after its pause within 1 dB of its steady level (overshoot and dip); o |
| 220 | `air ft8-tx` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a ft8-tx over the air, run whole: ft8-tx |
| 221 | `air cw-copy` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b cw-copy over the air, run whole: cw-copy |
| 222 | `air cw-carrier` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b cw-carrier over the air, run whole: cw-carrier |
| 223 | `air cw-kbd` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b cw-kbd over the air, run whole: cw-kbd |
| 224 | `air rtty` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b rtty over the air, run whole: rtty |
| 225 | `air psk` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b psk over the air, run whole: psk |
| 226 | `air jtty` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b jtty over the air, run whole: jtty |
| 227 | `air olivia` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b olivia over the air, run whole: olivia |
| 228 | `air wefax-cont` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b wefax-cont over the air, run whole: wefax-cont |
| 229 | `air wefax-auto` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b wefax-auto over the air, run whole: wefax-auto |
| 230 | `air ft8-multistream` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-multistream over the air, run whole: ft8-multistream |
| 231 | `air ft8-qso` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-qso over the air, run whole: ft8-qso |
| 232 | `air ft8-hold` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-hold over the air, run whole: ft8-hold |
| 233 | `air ft8-late` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-late over the air, run whole: ft8-late |
| 234 | `air js8` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b js8 over the air, run whole: js8 |
| 235 | `air tci-mic` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-mic over the air, run whole: tci-mic |
| 236 | `air tci-audio` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-audio over the air, run whole: tci-audio |
| 237 | `air tci-cw` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-cw over the air, run whole: tci-cw |
| 238 | `air tci-level` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-level over the air, run whole: tci-level |
| 239 | `air tci-ask` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-ask over the air, run whole: tci-ask |
| 240 | `air sentry-lock` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c sentry-lock over the air, run whole: sentry-lock |
| 241 | `air g-moni` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c g-moni over the air, run whole: g-moni |
| 242 | `air paths bands` | both ways | 600 s | an over on every band both radios share, 160 to 6 m, each way: heard, the receiver's level per band (suite_more band_proofs) |
| 243 | `air paths walkie` | both ways | 400 s | the walkie in AM and FM on 20 m and 10 m FM, each way (bench.py radios-walkie) |
| 244 | `air paths if-shift` | X6200 TX, X6100 RX | 120 s | the X6100's IF shift at +-2000 Hz: the tone's pitch kept, the base 2.1 kHz the other way (ifshift.py) |
| 245 | `system-checks` | both | 60 s | each radio read over SSH, nothing changed: battery log and model, data partition, settings' good copy, password logins refused, Tailscale, logs, start trace, services and ports, CPU, temperature, GPU, sessions file, update state, ADIF header, libft8, clock; the same version on both |
| 246 | `image x6100` | X6100 TX, X6200 RX (1500 Hz below, declared) | 120 s | the X6100's opposite-sideband image and carrier leak on 20, 17, 15, 12 and 10 m at 0.1 W: each image -35 dBc or lower (its 15 m shifts, 11-22); the carrier leak said |
| 247 | `image x6200` | X6200 TX, X6100 RX (1500 Hz below, declared) | 120 s | the X6200's image and carrier leak, as image x6100 |
| 248 | `ambe-vocoder` | both RX | 150 s | each radio's AMBE chip through its SDR daemon: the parity field asked for, the speech through each chip at DMR's and D-STAR's rates and the other's frames, libmbe-dvsi alike with the parity field off, P25 full rate only on a chip with the P25 option |
| 249 | `dv-dmr` | both RX | 70 s | a DMR call of the test speech replayed into each radio's SDR daemon: TG 3100 from 3112345 shown, its voice through the radio's own AMBE chip like the speech (r >= 0.9) |
| 250 | `dv-dmr-noparity` | both RX | 60 s | the DMR call again with both chips' parity field off: the test daemon reads the chip's answers whole through B's daemon's port; the field on again after |
| 251 | `dv-dstar` | both RX | 60 s | a D-STAR call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.4) |
| 252 | `dv-ysf` | both RX | 60 s | a YSF call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.7) |
| 253 | `dv-nxdn` | both RX | 60 s | an NXDN48 call of the test speech: TG 100 from 1234 shown, its voice heard (r >= 0.75) |
| 254 | `dv-p25` | both RX | 60 s | a P25 phase 1 call (1 kHz IMBE frames): TG 4321 from 1234567 shown; the tone heard where the chip has DVSI's P25 option, silence where it has not |
| 255 | `tci-audio-rate` | both RX | 15 s | each radio's TCI receiver audio at 97-103 % of real time |
| 256 | `tci-takes-control` | both RX | 20 s | a TCI client's set takes control (ctl tci) on both; gone, control back |
| 257 | `rx-settings` | both RX | 40 s | PRE, ATT, AGC, NB, NR, squelch, RF gain: each set on both, read back alike |
| 258 | `ptt-lease x6100` | X6100 TX, X6200 RX | 20 s | one PTT never renewed: off the air by itself within 4 s |
| 259 | `ptt-lease x6200` | X6200 TX, X6100 RX | 20 s | one PTT never renewed: off the air by itself within 4 s |
| 260 | `ptt-locked x6100` | both RX | 12 s | the X6100's PTT with its transmitter locked: stays off the air |
| 261 | `ptt-locked x6200` | both RX | 12 s | the X6200's PTT with its transmitter locked: stays off the air |
| 262 | `ptt-out-of-band x6100` | both RX | 12 s | the X6100 armed on 15.000 MHz (WWV, no amateur band): PTT refused |
| 263 | `ptt-out-of-band x6200` | both RX | 12 s | the X6200 armed on 15.000 MHz (WWV, no amateur band): PTT refused |
