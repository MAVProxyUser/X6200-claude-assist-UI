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
    python3 orch.py coverage             # features.json against every test's latest result, orch's and the suite's
    python3 facilitate.py                # FACILITATE.md: what still waits on the owner, in sessions

One run at a time, whichever session starts it: `run` takes `~/.config/x6x00-bench/orch_run.lock` (created with
O_EXCL, its pid in it) and waits while another live run holds it; it also waits while a GUI install holds
`gui_install.lock` or a `tools/cross_x6200/install.sh` runs (another session's install replaced both GUIs mid-run,
2026-11-25). An install takes `gui_install.lock` (its pid) after no run is going and drops it after both radios.

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

1. Takes control of both radios through their remotes (`bench.Radio`), logs every command it sends, and turns both
   radios' update check off for the run (`/mnt/update/no-check`, marked as the run's, taken away at its end; an
   owner's own file is left). While that file is there the GUI keeps the update question and the power-on box away:
   not opened, and one already open put away unanswered within a quarter second (an X6200 question fetched 6 minutes
   before a run stayed over its tests 3 h, 2026-11-26). `update-check-off` checks both, with `cfg? upd_state`.
2. Before every test (`Orch.ready`): neither keyed, both transmitters locked (the Sentry's A/B), the two on one
   frequency and mode, the squelch at 0 and the volume at 0 on both; anything else is put right first, or the run stops.
3. Runs the test: its steps, each confirmed; an over goes through `over()`: both set alike (page G, the receiver
   profile, the power), the bench's gates (`chain_gate`, `radio_tx_gate`, `listener_gate`), the sender armed, the
   Watch (`bench.Watch`: too long keyed, too much power over the pad's rating, a listener hearing nothing), then
   locked again. A test whose role transmits (TX, both ways, keyed) is not started while the owner's word on the
   radios cable has lapsed (`sends_gate`: `chain_gate`'s 12 h): it is NOT RUN, with the reason, its last real result
   kept, never a FAIL (2026-11-26: rit-xit's first two "failures" and most of power-steps' were that refusal). Nor
   while a sender is an X6200 on a supply under 12.0 V with its battery under 7.3 V at rest (`bench.battery_tx_gate`):
   the over would sag it to 7.0 V and its GUI would switch the radio off 30 s later, as it did mid js8-groups
   2026-11-26 on the bench's 11.8 V supply (the radio stays off till its POWER key); an X6200 listener on such a
   supply at 7.1 V or under is refused too (`listener_gate`: receive alone drains it to the cut). A test
   that fails while a radio's link stalled under it (its GUI the same process: the X6100's WiFi drops itself for
   ~10 s) runs once more, after both radios answer again; the first try's words are kept with the result (`RETRY` in
   the events). A GUI that restarted is a crash: no retry.
   Every test ends in `bench._finish`: each radio put back as its snapshot found it, read back, then its settings
   database. A radio not put back is recorded (`left_wrong.json`) and every test refuses it till `bench.py restore`.
   The database's read is tried three times 8 s apart (`DB_RETRY_S`), and a radio recorded only because it could
   not be read (its live settings were already back) is let go by the next test that finds it reading as that
   snapshot (`left_wrong_gate`): one stalled link at the end of air olivia refused the 44 tests after it, 2026-11-26.
   A radio's link gone between tests stops the run with that said (`run stopped`), not a traceback.
4. Records the result in `orch_results.json` (kept across runs), the run's events in `runs/<time>-orch/events.jsonl`
   and its results in `results.json`.
5. Ends with both radios on the Sentry's page A, transmitters locked, on the last test's frequency, alike; the END
   line says when it could not.

Guards running all the while: the sync guard; a radio that transmits while its role is RX (both stopped,
photographed); a signal over -100 dBm on a receiver with neither radio saying it sends (`RF_ALARM_DBM`: both stopped);
a radio silent for 3 s (`STALE_S`: its GUI's process checked, a crash fails the test, a WiFi stall is said). A
Watch stops an over at 6 s of silence (`bench.STALE_S`), but a radio that cannot go on the air unseen, off the air at
its last word with its TX locked or the over's listener in an app that keys only when told to (WeFax, NavTex, Time
Signal, Airspace), goes 15 s (`bench.stale_limit`, `STALE_HELD_S`): its stall is said and the over goes on (the
X6200's WiFi held its link 4 s in WeFax's Auto, 2026-11-24).

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
- **Children too**: a bench program a test runs as a child (`suite.run_child`: tones.py --bursts for power-loop,
  bench.py radios-walkie for the paths) publishes its transitions, follow window and declarations to
  `bench.SYNC_DIR/<pid>.json`; the parent's `changing()`, `declared()` and `declarations()` read them, so orch's guard
  sees a child's set-ups as its own. Every bench process owns the directory for its own children.
- **The follow window closes only after the guard saw the two apart in it**: an app asked to open has not tuned yet
  when the two still read alike, and closing the window then let its tune a moment later count as a violation;
  `sync_pair` closes it outright.
- **Teardown is one transition**: a test's own cleanups (apps closed, files put back) and `finish()` run as one
  transition, which ends with the pair alike. `bench.close_apps()` keeps a radio on the dial and mode its app was on
  (closing an app puts it on the Sentry page's own dial, G's channel). air_b's `listen_at()` sets both radios' dial and
  mode together with the listener's receiver, and tests set the receiver they need (the CW tests an 800 Hz filter on
  the key tone): tests leave the receiver as they end, so a probe's wide filter is not inherited. Before each
  radio's receiver is set a radio at a time (`rx_profile`: PRE, ATT, AGC, filter, read back, several seconds), both
  dials go at once (air_b's `both_at()`, air_c's proof): a radio moved first and set up alone sat 9 s on another dial.
- **An app's own state the two must share is read back**: the Olivia app's variant (the remote's `av`) is set alike
  on both before its overs and every Mode step is read back, a tap that did not take tapped again.
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
| air paths if-shift | `ifs` STEPPED: the listener's IF shift stepped through +-2000 Hz, what it measures |

sim_test.py checks the framework itself (every rule, the Watch reading the registry, transitions, sync_pair).

## The console (http://127.0.0.1:8765/)

Read only, but for its Re-run all button and the verdicts by hand. **Radios**: both live (frequency, mode, on the air or not, power, page, app, link), the sync state.
**Now**: the test and its steps as they go, the progress. **This run**: the tests run now, their results and what
each found (pictures of both screens for a failed one). **Not rerun**: the earlier results, the latest first, with
their tallies and GUI versions. **Where the radios differ**: the caps test's map. **Events**: everything the
orchestrator does, the newest first; `/log` the whole run's events, `/status` the state as JSON, `/shot/<file>` a
screen picture.

A test's tally ("3/3 passed") counts from the last change to its own code (`test_code`: its function's source), the
lifetime totals beside it: a test fixed over several runs no longer reads 1/7 for ever. **Between runs** the standing
console watches the radios itself (`idle_watch`): their frequency and mode over receive-only sessions every 2 s,
nothing set and control never taken; a difference over 3 s shows OUT OF SYNC in the sync state and the events, and
goes to `~/.config/x6x00-bench/idle_sync.jsonl` (a dial bumped at the radio between runs, 2026-11-25). The next run's
own check puts the two alike, its guard watching from then on.

**Tally** (the owner, 2026-11-26): this batch (the run going, or the last) beside all time: the results (pass, fail,
LOOK, each with its share), the sync violations (in runs; between runs from `idle_sync.jsonl`) and the tests that came
good, passing after a FAIL. Counted from every run's `events.jsonl` (`tally`; `/tally` as JSON), each read again only
as it grows. A test came good after a change when a commit landed between the FAIL and the PASS, or its own code
(`results.json`'s `code`) or either radio's GUI binary (`meta.json`'s md5) differed; on a retest alone when none did;
"unclear" for runs before 2026-11-26, which kept neither (a fix installed on the radio before its commit). Under it:
every test by its latest result (`orch_results.json`), the tests that fail most often, all time, and the latest to come
good. LOOK means the test ran but cannot judge by itself: what it found (a picture, a reading) is for a person to look
at. **Re-run all** starts `orch.py run --all` from the standing console when no run goes, none holds the radios
(`orch_run.lock`) and no GUI install is under way. Only from this computer (a POST from 127.0.0.1 with the page's own
token) and after a confirmation; its output goes to `runs/<time>-console-rerun.log`. A run's own console has no working
button: the standing one must be up.

**To look at** (the owner, 2026-11-26: "none of the look items have artifacts for me to look at, nor an approval method
to say they pass manually"): every test whose latest result is LOOK, the latest first. Each one's **show** (on any
LOOK or FAIL row, in this run's table and the kept one too) opens what a person needs to judge it (`/detail?name=`):
what passes, what the test said, its pictures, the run's files written while it ran (pictures shown, audio played in
the page, a `.raw` as 24 kHz audio: `/runfile/<run>/<file>`), its steps and the numbers it returned. **It passes** /
**It fails** (an optional note) record a verdict by hand on that result, **Undo** takes it back: only from this computer,
as Re-run all. The verdict stands in `orch_results.json` (`judged`: who, when, the note, what the test said) until the
test runs again; a LOOK again keeps it beside the new result as `judged_before`. `python3 orch.py judge <test>
PASS|FAIL|UNDO [note] [--by who]` does the same from a shell. Every verdict is kept in `orch_judged.jsonl`, and the
tally counts them.

## Files

| where | what |
|---|---|
| `~/.config/x6x00-bench/orch_results.json` | each test's last result, passes and runs (what is not rerun) |
| `~/.config/x6x00-bench/orch_status.json` | the run's state for the console, each second |
| `~/.config/x6x00-bench/orch_judged.jsonl` | every verdict by hand (the console's It passes / It fails / Undo, `orch.py judge`) |
| `~/.config/x6x00-bench/orch_caps.json` | what each radio takes (the caps test), the tests' guide to their differences |
| `~/.config/x6x00-bench/runs/<time>-orch/` | a run: `events.jsonl`, `results.json` (each test's code fingerprint with its result), `meta.json` (the md5 of each radio's GUI binary at its start), each over's WAV and spectrum frames, the AMBE and digital voice files; the CW tests' listener audio (cw-text's `cw_text_*.wav`, cw-quiet's `cw_quiet_<lo>_<hi>_<radio>.wav` from before the first switch; cw-copy, cw-carrier and tci-cw's `*.raw`, 24 kHz int16; every `cw_over`'s `cw_<label>_<sender>_to_<listener>_<time>.wav`: cw-dec-snr, cw-key-train, cw-his and the rest) for `tools/cw_replay` |
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

**What a radio's base lacks** (`radio_caps`) is greyed in its place, never dropped, so both radios' pages stay alike
(the owner, 2026-11-26: "we want 1:1 unity"): `buttons?` marks such a button `~NAME`, its press only says it is not on
this radio's base, and its `set`/`cfg` is refused. `lacks_check(r, res, cfgs=, sets=, buttons=, cmds=)` wants each
button greyed (not live, not missing) and each setting refused with nothing changed.

**Remote hooks for the tests** besides `set`/`cfg`: read-only `cfg?` names (`radio_caps`, `cwt_target` and
`cwt_state` for the CW Trainer, `upd_state` for the update question, `a_tx_rows`, `beacons_heard_<band>`), and
`cwsend TEXT` (operate, a session that may transmit): the radio's own keyer sends TEXT in CW/CW-R when it is idle, as
TCI's CW macros do, without a TCI client (the CW Trainer's answers).

## Over-the-air coverage (every transmission test, 0.1 W over the 80 dB cable unless said)

orch's own tests and the earlier suite's over-the-air groups, run whole through orch (`air ...`: `suite_group` runs a
batch's `t_*` with a `suite.Run` of its own, its sync_both and put-back through the one framework, a `bench.Watch` on
every over; orch's sync guard stays on, every deviation in them declared; orch takes control back and puts both alike
after; the group passes with no row failed). Every test runs both ways unless it says otherwise.

| area | orch's own | the suite's, run whole (`air ...`) |
|---|---|---|
| voice and tones | usb/lsb/am/fm-steps (the stepped tones heard in order), g-voiceprint (G's mic) | air paths walkie (AM/FM on 20 and 10 m) |
| every band | sync-bands, bands-all (receive) | air paths bands (an over on each band 160-6 m) |
| power | power-steps (20 m), power-6m, power-6m-fm, power-6m-am (the X6100's base floor checked with the GUI's note), power-cap (receive), comp-mic x6100 (Comp through its mic: the X6200's speaker into it, its monitor read) | air tune-power, air power-loop (bursts after pauses), air cw-alc, air comp |
| TUNE | tune-carrier, ext-tune | air tune, air tune-app, air tune-key, air qsy-out |
| PTT and its refusals | ptt-lease, ptt-locked, ptt-out-of-band, rx-session-refused, remote-security, remote-busy | air keys-with-ptt, air armed-ptt-a, air sentry-lock |
| split and TX shift | (image x6100/x6200: the listener 1500 Hz below, declared) | air split, air split-cw |
| CW | cw-text, cw-kbd, cw-timing x6100/x6200/x6100 1w (every mark and gap on the air against the keyer's schedule; its own log tells the keyer's fault from the base's); receive only: cw-quiet and cw-quiet wide (the decoders print nothing in the cable's quiet after USB-D to CW, through 300-1100 and 100-1300 Hz), cw-noise-shape (the noise's shape the decoder judges against, both radios, five CW filters and CW-R), cw-noise-tone (the X6200's resonance at a 600 Hz key tone) | air cw-copy (extended letters, the VFO line), air cw-carrier, air cw-kbd (stop) |
| FT8/FT4/FT2 | ft8-cq, ft8-reply, ft8-qso, ft8-stop, ft8-copies x6200/x6100/x6200 1w/x6200 tci/x6200 tci app (an app's overs, and the same message through TCI, also with the app open: the power 156 Hz either side) | air ft8-tx (level, ALC), air ft8-qso (FT8/FT4/FT2), air ft8-multistream, air ft8-hold, air ft8-late |
| JS8 | js8-text, js8-close-to-g | air js8 (CQ, directed, heartbeat, inbox, stop) |
| PSK, RTTY, Olivia, JTTY | psk31-text, rtty-text, olivia-text | air psk (macros, stop), air rtty, air olivia (Olivia and Contestia modes), air jtty |
| WeFax, SSTV | wefax-chart, sstv-picture | air wefax-cont, air wefax-auto |
| TCI | tci-out-of-band, tci-ask, tci-cw-usb, tci-key-cw (refusals), tci-drive, tci-tail x6200/x6100 (an over's last audio: unkeyed at once, all of it on the air) | air tci-mic, air tci-audio, air tci-cw, air tci-level, air tci-ask (asked, then on the air) |
| receivers while one sends | (every over's listener judged) | air smeter-front-end, air smeter-am, air readings-zero, air band-edges, air g-moni, air codec-clock, air text-log |
| IF shift, images, spurs | image x6100, image x6200, spurs x6200/x6100 (a clean tone: its sidebands and skirt, AGC on and off, 0.3 and 0.9 of full scale), skirt x6200 levels (0.1 to 0.95 up and down) | air paths if-shift (the X6100's) |

Not over the air in orch yet: nothing of the suite's over-the-air batches is left out except what orch has already
(power-steps, ft8-stop, remote-busy, ext-tune, ft8-cq) and the paths parts it repeats (tones, mic-on-g, rtty, psk31);
the paths' CW each way is `air paths cw`.

**Coverage.** `orch.py coverage` writes `runs/<time>-coverage/coverage.md`: each of features.json's 1397 features
pass, fail, look, partial or not covered, by area. Orch's tests name the features they cover (`ORCH_COVERS` for the
native ones; the suite's own rows for the 1:1 ports and the air groups); the suite's older runs count where orch has
not run a check again, and each feature goes by its latest result (2026-11-25: 492 pass, 4 fail, 24 look, 137 in
part, 740 not covered). `facilitate.py` turns what is left and needs internet services, Bluetooth, GPS or the owner's
hands into sessions in [FACILITATE.md](FACILITATE.md), and counts what the bench can still write and run alone.

**The cable's own tests (orch_cable.py, 2026-11-25).** The 112 features FACILITATE.md left to the bench over the cable
got 113 tests at once (the owner: "write ALL the tests first, then we can debug them via the runs"): CW (Dec SNR,
Parse, ZAP, the tuning bars, CW peak, Train, Log QSO, MSG > CW with {CALL}/{HIS}, the beacon, QRSS3, the CW Keyboard's
Live), the test beacon (Bands, Field Ops), Field Ops' CQ, Propagation, the Scanner, FT8 (the grid, the modifier,
Retries, Auto off, compound calls, Hound, the list), JS8 and GhostNet, SSTV's own TX (Robot 8), WeFax's look and tilt,
PSK31, the transverter's limit, the 0 W warning, AGC, CESSB, Moni, the mic, the TX panel, the screensaver, the walkie,
G's lamp, SCAN CFG's priority, MSG > Voice, Winlink (ARDOP, Mercury, XARA), the Panel's PTT in headless Chromium,
PSK Reporter caught on the radio itself, the X6100's TX Cal. Each says what it covers (orch_cable.COVERS). Four wait
on the owner's word (a file in ~/.config/x6x00-bench: owner_ok_qrss_kill, owner_ok_clock, owner_ok_3min,
owner_ok_internet) and RADE on a recording from the hand-mic session; they say so and change nothing. A feature only a
picture shows is LOOK, never PASS.

Three remote commands for the bench's tests came with them (remote_server.c): `cfg NAME VALUE` / `cfg? NAME` set and
read any stored setting by its database name through its own validator (bench.cfg_set / cfg_get: Dec SNR, Parse,
Train, the transverters, Moni, the FT8 and JS8 settings...), `btn NAME [hold]` presses a main-screen button by name
(the CW pages' ZAP, Log QSO...), and every message the radio puts in its message box goes to the panel clients as
`msg "TEXT"` (bench.Radio.msgs): the tests read "QRSS3 stopped: tuned away" or "TX 0 W: level ..." instead of a
picture.

**GMRS (orch_gmrs.py, 2026-11-25).** The owner's GMRS licence: B and G listen to GMRS's 22 channels and the repeater
inputs, and nothing sends there (47 CFR 95E: certified GMRS radios only). Five tests, all receive only, through the
remote's `set bch` / `set gsdr` and its state's `bhz`, `bpre`, `bmode`, `bname`, `bscan`, `ghz`, `garm`, `gno`, `sdr`:
`gmrs-b` (B on GMRS 20, GMRS 8 and GMRS 20 IN on both: the GMRS preset, NFM, the names; A's dial untouched; `set bch`
refused off B), `gmrs-scan` (B's F1 on GMRS: bscan 4, the channels in frequency order, each named; on each radio with an
SDR in turn, else the X6200's daemon replaced by one replaying noise, `x6x00-rtlspec -i`, for the test and its own
started again after) and `gmrs-g` (G on GMRS 20 and its input through each radio's SDR in turn: `gno` GMRS's reason, A/B
arms nothing, 446.000 no GMRS reason, G back on the radio; needs a real SDR on a HOST port) and `gmrs-f` (F SYSTEM's
GMRS panel on both, by its keys: chosen by `fsel`, open `gmp`, its channel `gmhz`, its receiver `gmrx`; F1 tunes on B's
SDR, with no receiver nothing), `gmrs-xvtr` (the radio's own receiver: a transverter taking 462.5-467.8 MHz in on both
radios, the WR12's stand-in, put back after: F's panel says A hears it and tunes A in NFM on both; a radio with an SDR
hears GMRS on B's SDR first, its panel says so, A's dial is tuned straight and G's GMRS group is the SDR's; A/B arms
nothing, the remote's PTT is refused with GMRS's words and nothing keys, A's F1 scans the 22 on each radio, declared
apart while the dials step, G's GMRS group (the radio's, or the SDR's) steps with CH +). Host checks: `tools/gmrs` (the
table, the scan's next place, F's panel rows, the tones' rows; gmrs_tx_x6100/x6200 on the real settings: out of reach as
the radio comes, received through the WR12 and a transverter, refused as GMRS through it whatever the switch and the TX
ranges), `tools/ham_bands` and `tools/tx_ranges` (GMRS's spans written out).

**NIFOG (orch_nifog.py, 2026-11-25).** CISA's guide on F SYSTEM and on the web page, receive only, one test per
feature (features.json's f-nifog-* and web-nifog-*), on both radios at once, each ending with NIFOG closed on page A.
The reader's words come through the remote's `nifog?` (`open`, `page`, `y`, `maxy`, `cap`, `where`, `legend`, `keys`,
as the screen has them), its page and scroll also as the GUI's state's `s.nf` and `s.nfy`; a page's print is counted in
a picture of its window (taken again while a message box covers it). `nifog` walks it all; `nifog-list`,
`nifog-open-close` (F2, the push, page A), `nifog-drawn` (pages 30 and 68 after G: centred, across, the theme's
colours, the GUI log's line), `nifog-caption` (pages 1, 4, 38, 68 against tools/nifog/nifog.idx), `nifog-mfk` (1 and
192 the ends), `nifog-vfo` (24 px a click, over a page's ends, no tuning), `nifog-sections` (F1/F3 from page 40),
`nifog-contents` (F4), `nifog-chapter` (F5, from the last to the cover), `nifog-legend-keys`, `nifog-url` (the radio's
hostname and address), `nifog-reopen`, `nifog-sentry-closed` (AIRSPACE opened over it and closed: F's list again),
`nifog-files` (/usr/share/x6100/nifog: CISA's PDF, the contents, 192 PNGs), `web-nifog-pdf`, `web-nifog-pages` (all
192 as the radio's files; the address looked up once, a .local lookup took 5 s), `web-nifog-viewer` (headless
Chromium) and `web-nifog-menu`. The tap on the page has none: neither radio has a touch device. Host checks:
`tools/nifog` (src/nifog.c's index and nifog_load, render.py, the committed index against the PDF where poppler is,
the packaging, the web routes).

**The apps (orch_cov_apps.py, 2026-11-26).** The apps' features of features.json that no test covered and that
need only the two radios (or the cable, at 0.1 W, one radio sending at a time), one test per app or group of
features, on both radios at once where nothing is sent: Bands, Time Signal, the Memories page's API, WSPR's web
settings, CB and HF Air Band (PTT and a program's TCI audio refused off the bands), Zello and SVXConnect with no
account (Talk via, the links' squelch rule, one link at a time, the PTT held while open), Walkie Talkie mode (no
app inside, VOL, the WLK pages, its channels, its bandscope), FT8 (Time Sync with NTP stopped and the clock 3 s
ahead, the Log Map, a typed dial), PSK31's squelch, RADE's list and Power, WeFax (its stations and WEFAX channels,
View, the browser, Power), SSTV (the RX Pics Browser, Delete, the Gallery, the TX folders), JS8 and GhostNet
(Freq, GhostNet mode, its nets, reminder and RTTY entry, AUTO off at the open, no callsign, INFO, the map), Field
Ops (its tiles, a SOTA activation end to end, the Test beacon's rules, TOTA/WCA/MOTA/LH/LLOTA, Tiles, XOTA and
NOTA, the map from test tiles, WWFF/WWBOTA/HOTA with a CB call so nothing is spotted, a restart mid-activation),
Channels over FT8 (the hand mic's V/M), birdie marks, the CW Keyboard's refusals, Propagation, Flights, Weather,
the CW Trainer (Learn and Calls answered by TCI CW at 0 W, receive-only ranges) and the first-start guide. They
read the GUI through three read-only bench hooks of the remote: `fkeys?` (the five F-key labels as drawn, so a
key is pressed only on a page the test has read), `type TEXT` (the open text window's text and its Enter) and
`hkey VM|UP|DOWN 1|0` (a hand mic key); the screen's own words by tesseract's OCR of a screenshot. References and
positions come from the radio's own lists at run time (refs.tsv), never written in the test.

**The receivers measured (orch_rx.py, 2026-11-26).** What each radio's receiver does to a known signal, in its
own TCI audio, S-meter and spectrum, not its settings read back; each test both ways, the other radio sending a file
of tones at 0.1 W, every over's received audio kept in the run's folder (`rx_*.raw`). A tone's level is taken against
a pilot sent with it all through (the listener's AGC moves both alike), the narrow runs against a wide one (the
sender's own response out), and a file's steps are found by the pilot's start and end in the audio (its rate is not
exactly 24 kHz). A step the sender's audio makes is no clean step on the air (a 30 dB step moved the X6100's RF
8-10 dB, its power loop holding the power; the X6200's first 10 dB moved nothing, saturated): the AGC's step is the
listener's own ATT, NR's noise is sent with its tone, the S-meter's steps start under the X6200's knee.
`rx-filter` (100-3000 against 500-2300 and 1250-1750: flat within 3 dB inside, 15 dB or more down 300 Hz out, the
-6 dB points within 150 Hz of the edges; judged inside the wide filter's flat 300-2800 where the wide run was 25 dB
clear), `rit-xit` (the listener's RIT and the sender's XIT +-200 Hz move the heard pitch 200 Hz the right way),
`rx-pre-att` (two overs: the spectrum moves 6 dB or more each way, the S-meter reads alike by its figures),
`rx-agc` (the listener's ATT put in mid-over, AGC Slow and Fast: 6 dB at most left, Fast within 1.5 s, Slow 1.5
times as long), `rx-dnf` (where the tone lands heard first, the notch centred there: 20 dB against a tone 700 Hz
off; the X6200's auto notch, the X6100's refused), `rx-nr` (a tone over noise sent with it: 3 dB or more further
over it with NR) and `rx-smeter` (the S-meter's steps within 3 dB or 30 % of the spectrum's). No NB test: clicks
through an SSB transmitter are its audio's width, and neither base's blanker took them (-0.6 and -1.1 dB,
2026-11-26); a wideband impulse source (the HackRF) is what it needs. The listener's own changes are declared apart
and put back to the sender's before the declaration ends.

## The tests (496)

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
| 17 | `cw-quiet` | both RX | 120 s | CW 14.040, nothing sent: both from USB-D to CW three times and a stay in CW, 12 s of the cable's quiet each: neither decoder prints a character (their audio kept for tools/cw_replay) |
| 18 | `cw-noise-tone` | both RX | 90 s | the X6200's CW resonance at another key tone (600 Hz, the remote's kt, put back after): the noise peaks there, as the decoder's colour takes it |
| 19 | `classic-wf3d` | both RX | 90 s | the classic screen's waterfall in 3D (upstream's drawing thread) and 2D on both: lit and moving in each, the GUI's CPU said, the GUI still the same process; the screen and keys put back |
| 20 | `cw-quiet wide` | both RX | 120 s | cw-quiet through a wide CW filter, 100-1300 Hz (the X6200's resonance taken out down to 150 Hz, the X6100's noise rising under 300 Hz): neither decoder prints a character |
| 21 | `cw-noise-shape` | both RX | 70 s | CW 14.040, nothing sent: the receive noise's shape through five CW filters and CW-R on both (over the band's 25th percentile, the decoder's floor): measured, said |
| 22 | `power-steps x6100>x6200` | X6100 TX, X6200 RX | 45 s | USB 14.200: a tone at 0.2, 0.5, 1.0 W; the other's level must rise 4.0 and 3.0 dB (2 dB) |
| 23 | `power-steps x6200>x6100` | X6200 TX, X6100 RX | 45 s | USB 14.200: a tone at 0.2, 0.5, 1.0 W; the other's level must rise 4.0 and 3.0 dB (2 dB) |
| 24 | `power-6m x6100>x6200` | X6100 TX, X6200 RX | 45 s | USB 50.200: a tone at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB): no floor under 1 W (the factory X6100's 0.8 W at 0.1 W) |
| 25 | `power-6m x6200>x6100` | X6200 TX, X6100 RX | 45 s | USB 50.200: a tone at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB): no floor under 1 W (the factory X6100's 0.8 W at 0.1 W) |
| 26 | `power-6m-fm x6100>x6200` | X6100 TX, X6200 RX | 45 s | FM 52.525 with no audio: the carrier at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB), or, from the X6100 (its base's ~1 W floor there, FLOOR_6M), stay alike with the GUI's note of it in the log |
| 27 | `power-6m-fm x6200>x6100` | X6200 TX, X6100 RX | 45 s | FM 52.525 with no audio: the carrier at 0.1, 0.3, 1.0 W; the other's level must rise 4.8 and 5.2 dB (2 dB), or, from the X6100 (its base's ~1 W floor there, FLOOR_6M), stay alike with the GUI's note of it in the log |
| 28 | `power-6m-am x6100>x6200` | X6100 TX, X6200 RX | 45 s | AM 50.400 with no audio: the carrier at 0.1, 0.3, 1.0 W; as power-6m-fm |
| 29 | `power-6m-am x6200>x6100` | X6200 TX, X6100 RX | 45 s | AM 50.400 with no audio: the carrier at 0.1, 0.3, 1.0 W; as power-6m-fm |
| 30 | `ft8-cq x6100>x6200` | X6100 TX, X6200 RX | 40 s | FT8 14.074: a CQ in the next slot, the other must decode exactly the message sent |
| 31 | `ft8-cq x6200>x6100` | X6200 TX, X6100 RX | 40 s | FT8 14.074: a CQ in the next slot, the other must decode exactly the message sent |
| 32 | `psk31-text x6100>x6200` | X6100 TX, X6200 RX | 45 s | PSK31 on the listener's app dial: 'CQ CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 33 | `psk31-text x6200>x6100` | X6200 TX, X6100 RX | 45 s | PSK31 on the listener's app dial: 'CQ CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 34 | `rtty-text x6100>x6200` | X6100 TX, X6200 RX | 120 s | RTTY made to the listener's own settings: 'RYRY CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 35 | `rtty-text x6200>x6100` | X6200 TX, X6100 RX | 120 s | RTTY made to the listener's own settings: 'RYRY CQ DE Q1TST Q1TST K' sent, decoded with at most 5 % of the characters wrong |
| 36 | `wefax-chart x6100>x6200` | X6100 TX, X6200 RX | 60 s | WeFax: 12 s of a half-white, half-black chart; the other's saved fax must show the two halves at least 80 grey levels apart |
| 37 | `wefax-chart x6200>x6100` | X6200 TX, X6100 RX | 60 s | WeFax: 12 s of a half-white, half-black chart; the other's saved fax must show the two halves at least 80 grey levels apart |
| 38 | `tune-carrier x6100>x6200` | X6100 TX, X6200 RX | 25 s | TUNE at 0.1 W: on the air, heard 15 dB up, off by itself 2.5-5 s after the lease's last renewal |
| 39 | `tune-carrier x6200>x6100` | X6200 TX, X6100 RX | 25 s | TUNE at 0.1 W: on the air, heard 15 dB up, off by itself 2.5-5 s after the lease's last renewal |
| 40 | `ft8-qso x6100>x6200` | X6100 TX, X6200 RX | 180 s | FT8: a whole QSO by the apps (Auto), logged on both, one ADIF record each (taken out after) |
| 41 | `ft8-qso x6200>x6100` | X6200 TX, X6100 RX | 180 s | FT8: a whole QSO by the apps (Auto), logged on both, one ADIF record each (taken out after) |
| 42 | `ft8-stop x6100>x6200` | X6100 TX, X6200 RX | 35 s | FT8: ft8 stop 2 s into a CQ takes the sender off the air within 1.5 s (its state's word), not again |
| 43 | `ft8-stop x6200>x6100` | X6200 TX, X6100 RX | 35 s | FT8: ft8 stop 2 s into a CQ takes the sender off the air within 1.5 s (its state's word), not again |
| 44 | `tci-control` | both RX | 20 s | TCI vfo and modulation on both radios' own TCI: each confirmed by its own state, the two alike |
| 45 | `rigctl` | both RX | 15 s | rigctl F/f M/m on both: confirmed by each radio's state, alike; its PTT refused |
| 46 | `dial-lock` | both RX | 15 s | the dial lock on both: a set refused with the dial kept; off, the set taken |
| 47 | `remote-security` | both | 15 s | a stranger's token: watch only, its set/PTT/knob/control/TUNE/FT8 each refused |
| 48 | `rx-session-refused` | both | 15 s | the receive-only session's PTT, TUNE and FT8 CQ refused, nothing keyed |
| 49 | `apps-alike` | both RX | 60 s | JS8, Olivia, SSTV, WeFax, NavTex, PSK each opened on both at once: the same dial |
| 50 | `ft8-reply x6100>x6200` | X6100 TX, X6200 RX | 60 s | FT8: a CQ, the other decodes it and replies; the first must decode exactly the reply |
| 51 | `ft8-reply x6200>x6100` | X6200 TX, X6100 RX | 60 s | FT8: a CQ, the other decodes it and replies; the first must decode exactly the reply |
| 52 | `remote-busy x6100` | X6100 TX, X6200 RX | 20 s | while the X6100 is on the air another session cannot take control, the X6200 never keyed |
| 53 | `remote-busy x6200` | X6200 TX, X6100 RX | 20 s | while the X6200 is on the air another session cannot take control, the X6100 never keyed |
| 54 | `band-memory` | both RX | 20 s | each band its own ATT: 20 m on, 15 m off, each back from its memory on both |
| 55 | `cb-channel` | both RX | 15 s | CB 19 as G's channel, then a channel up: 27.205 AM on both |
| 56 | `js8-text x6100>x6200` | X6100 TX, X6200 RX | 90 s | JS8: a directed text, the other's list must show it from the sender with the text exact |
| 57 | `js8-text x6200>x6100` | X6200 TX, X6100 RX | 90 s | JS8: a directed text, the other's list must show it from the sender with the text exact |
| 58 | `sstv-picture x6100>x6200` | X6100 TX, X6200 RX | 60 s | SSTV: a Robot 8 picture (two white quarters, two black) made by the GUI's own encoder; the other's saved picture must show the white quarters 80 grey levels over the black |
| 59 | `sstv-picture x6200>x6100` | X6200 TX, X6100 RX | 60 s | SSTV: a Robot 8 picture (two white quarters, two black) made by the GUI's own encoder; the other's saved picture must show the white quarters 80 grey levels over the black |
| 60 | `olivia-text x6100>x6200` | X6100 TX, X6200 RX | 75 s | Olivia: the sender's app sends a macro 'TEST'; the other's text log must show it in the variant sent |
| 61 | `olivia-text x6200>x6100` | X6200 TX, X6100 RX | 75 s | Olivia: the sender's app sends a macro 'TEST'; the other's text log must show it in the variant sent |
| 62 | `cw-kbd x6100>x6200` | X6100 TX, X6200 RX | 45 s | CW Keyboard 14.040: a stored message 'VVV VVV Q1TST' keyed by the app; the other's decoder must print Q1TST |
| 63 | `cw-kbd x6200>x6100` | X6200 TX, X6100 RX | 45 s | CW Keyboard 14.040: a stored message 'VVV VVV Q1TST' keyed by the app; the other's decoder must print Q1TST |
| 64 | `pages-look` | both RX | 90 s | the Sentry's pages A-H on both, photographed together: the Sentry's colours on each, the two alike |
| 65 | `comp-mic x6100` | X6100 TX (its mic), X6200 RX (its speaker plays) | 90 s | Comp through the X6100's mic: the X6200's speaker (at 60) plays loud and soft steps 20 dB apart every 0.15 s, the X6100 keyed with PTT on G at Comp 1:1 and 4:1, its capture of each over (its monitor) read: 4:1 at least 3 dB narrower. On a base without a compressor (Xiegu's factory one: 11-25 none, 16.7 against 16.0 dB): MFK 2:4 without Comp, 'set comp 4' and 'cfg comp 4' refused. The X6200's capture holds no monitor of its over: its Comp is not measured this way |
| 66 | `g-voiceprint x6100>x6200` | X6100 TX (its mic), X6200 RX (its speaker plays) | 45 s | G's voiceprint on the air: the X6200 plays stepped tones on its speaker, the X6100 keyed on G at 0.1 W picks them up on its mic; its voiceprint (two pictures), its own capture and the X6200's receiver must show them |
| 67 | `ext-tune x6100>x6200` | X6100 TX, X6200 RX | 60 s | TUNE for an external tuner from the FT8 app's Ext tune at 0.1 W: a tone at its TX offset in the other's audio, off on the second press, the app and dial kept, off by its time limit |
| 68 | `ext-tune x6200>x6100` | X6200 TX, X6100 RX | 60 s | TUNE for an external tuner from the FT8 app's Ext tune at 0.1 W: a tone at its TX offset in the other's audio, off on the second press, the app and dial kept, off by its time limit |
| 69 | `js8-close-to-g` | both RX | 90 s | JS8 closed back to G on CB 20 AM, five times on both: no GUI dies (the X6200's SIGSEGV of 20:36) |
| 70 | `system-health` | both | 10 s | battery log fresh, clock within 1.5 s, the data partition read-write, on both |
| 71 | `rx-vfo-split` | both RX | 30 s | VFO B and back, split, RIT and XIT at +200/-200/0: each set on both, read back alike |
| 72 | `tci-receiver` | both RX | 30 s | TCI's filter, AGC, RIT, a VFO in capitals and an unknown command on both radios' TCI: each confirmed by the radio's state, the two alike |
| 73 | `rigctl-more` | both RX | 20 s | rigctl's levels, split (its setting refused) and state dump on both, nothing keyed |
| 74 | `remote-apps` | both RX | 20 s | the remote's apps list the same on both (more than ten); WSPR's opening refused on both |
| 75 | `power-cap` | both RX | 15 s | 20 W asked of both: each reads its own top (X6100 10 W, X6200 8 W, 5 W on a battery), nothing keyed, 0.1 W after |
| 76 | `smc-remote` | both RX | 30 s | TX Cal's S-meter Measure from the remote on both: its figures read, a Measure on the noise refused with nothing changed, PRE put a dB off and back, each read back |
| 77 | `web-pages` | both | 120 s | every page and GET API of the web configurator: answered alike on both, none with a server error |
| 78 | `tci-out-of-band x6100` | both RX | 15 s | a TCI audio key from a program on the X6100 on 15.000 MHz, armed: refused, off the air |
| 79 | `tci-out-of-band x6200` | both RX | 15 s | a TCI audio key from a program on the X6200 on 15.000 MHz, armed: refused, off the air |
| 80 | `tci-ask x6100` | both RX | 15 s | a network TCI client's trx on the X6100, armed: asked on its screen, not keyed; the question gone ~20 s after the client leaves |
| 81 | `tci-ask x6200` | both RX | 15 s | a network TCI client's trx on the X6200, armed: asked on its screen, not keyed; the question gone ~20 s after the client leaves |
| 82 | `tci-cw-usb x6100` | both RX | 15 s | CW text through the X6100's TCI in USB, armed: refused, off the air, nobody asked |
| 83 | `tci-cw-usb x6200` | both RX | 15 s | CW text through the X6200's TCI in USB, armed: refused, off the air, nobody asked |
| 84 | `tci-key-cw x6100` | both RX | 15 s | a TCI audio key on the X6100 in CW, armed: refused, off the air |
| 85 | `tci-key-cw x6200` | both RX | 15 s | a TCI audio key on the X6200 in CW, armed: refused, off the air |
| 86 | `tci-answers` | both | 30 s | the 1:1 suite's tci-answers (tci proto) on both radios at once: dds, start/stop and TCI's fixed answers as the code has them, the unsupported CW commands left unanswered |
| 87 | `tci-cw-speed` | both | 30 s | the 1:1 suite's tci-cw-speed (tci proto) on both radios at once: the KEY speed asked (control kept), set (both names echoed, control to TCI), 99 clamped to the radio's top, put back |
| 88 | `tci-rx-sensors` | both | 30 s | the 1:1 suite's tci-rx-sensors (tci proto) on both radios at once: rx_smeter ten a second always, rx_channel_sensors only while enabled |
| 89 | `tci-audio` | both | 30 s | the 1:1 suite's tci-audio (tci proto) on both radios at once: the greeting's audio defaults, 24k int16 mono 512 set and streamed, none after stop |
| 90 | `tci-drive` | both | 30 s | the 1:1 suite's tci-drive (tci drive) on both radios at once: drive read and set lower: the power follows, nothing keyed |
| 91 | `tci-four` | both | 30 s | the 1:1 suite's tci-four (tci guards) on both radios at once: four TCI clients at most, the fifth refused |
| 92 | `tci-origin` | both | 30 s | the 1:1 suite's tci-origin (tci guards) on both radios at once: a page from another site, a rebound name and Pat's mailbox refused; the radio's own page and a program without Origin taken |
| 93 | `panel-greeting` | both | 30 s | the 1:1 suite's panel-greeting (panel) on both radios at once: the remote's hello, keys list and token |
| 94 | `panel-bands-plan` | both | 30 s | the 1:1 suite's panel-bands-plan (panel) on both radios at once: the bands plan as the remote sends it |
| 95 | `panel-view-only` | both | 30 s | the 1:1 suite's panel-view-only (panel) on both radios at once: a view-only session sends nothing |
| 96 | `panel-ping-clock` | both | 30 s | the 1:1 suite's panel-ping-clock (panel) on both radios at once: the link's ping and the clock's answer |
| 97 | `panel-meter` | both | 30 s | the 1:1 suite's panel-meter (panel) on both radios at once: the S-meter and the battery in the remote's state |
| 98 | `panel-kt-cwrise` | both | 30 s | the 1:1 suite's panel-kt-cwrise (panel) on both radios at once: the CW rise time read |
| 99 | `panel-filter-am` | both | 30 s | the 1:1 suite's panel-filter-am (panel) on both radios at once: AM/FM filters as the panel sets them |
| 100 | `panel-refused-set` | both | 30 s | the 1:1 suite's panel-refused-set (panel) on both radios at once: a refused setting reverted on the panel |
| 101 | `panel-play-guard` | both | 30 s | the 1:1 suite's panel-play-guard (panel) on both radios at once: a file play refused where it must be |
| 102 | `panel-wide` | both | 30 s | the 1:1 suite's panel-wide (panel) on both radios at once: the wide spectrum as the remote sends it |
| 103 | `control-needed` | both | 30 s | the 1:1 suite's control-needed (control owner) on both radios at once: a setting without control refused |
| 104 | `control-lost-free` | both | 30 s | the 1:1 suite's control-lost-free (control owner) on both radios at once: control free again when its holder leaves |
| 105 | `screen-stream` | both | 30 s | the 1:1 suite's screen-stream (screen) on both radios at once: the screen's JPEG stream and its quality |
| 106 | `screen-viewers` | both | 30 s | the 1:1 suite's screen-viewers (screen) on both radios at once: the most screen viewers at once |
| 107 | `remote-origin` | both | 30 s | the 1:1 suite's remote-origin (screen) on both radios at once: the remote's WebSocket refuses other sites, takes its own names |
| 108 | `band-cycle` | both | 30 s | the 1:1 suite's band-cycle (core) on both radios at once: the band keys through every band, 60 m included, the untunable skipped |
| 109 | `broadcast-am` | both | 30 s | the 1:1 suite's broadcast-am (core) on both radios at once: the broadcast bands in AM |
| 110 | `tci-refused-answer` | both | 30 s | the 1:1 suite's tci-refused-answer (core) on both radios at once: a TCI tune the radio refuses (an app's hold) answered with what it kept |
| 111 | `knobs` | both | 30 s | the 1:1 suite's knobs (core) on both radios at once: the knobs through the remote tune the radio |
| 112 | `wide-zoom` | both | 30 s | the 1:1 suite's wide-zoom (core) on both radios at once: the X6200's wide zooms |
| 113 | `web-tci` | both | 30 s | the 1:1 suite's web-tci (web guards) on both radios at once: the web TCI page |
| 114 | `web-guards` | both | 30 s | the 1:1 suite's web-guards (web guards) on both radios at once: the web's guarded pages refuse what they must |
| 115 | `funnel-sim` | both | 30 s | the 1:1 suite's funnel-sim (web guards) on both radios at once: a Funnel (internet) connection's limits |
| 116 | `web-https` | both | 30 s | the 1:1 suite's web-https (web more) on both radios at once: HTTPS served with its own certificate |
| 117 | `wss-proxy` | both | 30 s | the 1:1 suite's wss-proxy (web more) on both radios at once: the WebSocket proxy over HTTPS |
| 118 | `ssh-key-checks` | both | 30 s | the 1:1 suite's ssh-key-checks (web more) on both radios at once: the SSH keys page refuses bad keys |
| 119 | `backup-parts` | both | 30 s | the 1:1 suite's backup-parts (web more) on both radios at once: the backup's parts listed |
| 120 | `textlog-page` | both | 30 s | the 1:1 suite's textlog-page (web more) on both radios at once: the text log page |
| 121 | `btcat-bridge` | both RX | 45 s | the 1:1 suite's cover_a btcat-bridge on both radios at once (it puts back what it changes): the Bluetooth CAT bridge on its serial port, its RFCOMM listening |
| 122 | `btcat-power` | both RX | 45 s | the 1:1 suite's cover_a btcat-power on both radios at once (it puts back what it changes): the Bluetooth adapter's power as set |
| 123 | `bt-pairing` | both RX | 45 s | the 1:1 suite's cover_a bt-pairing on both radios at once (it puts back what it changes): the pairing window opened and closed by itself |
| 124 | `backlight` | both RX | 45 s | the 1:1 suite's cover_a backlight on both radios at once (it puts back what it changes): the backlight setting changes the screen's brightness, put back |
| 125 | `screen-off` | both RX | 45 s | the 1:1 suite's cover_a screen-off on both radios at once (it puts back what it changes): the screen off and on again |
| 126 | `screensaver` | both RX | 150 s | the 1:1 suite's cover_a screensaver on both radios at once (it puts back what it changes): the screensaver after its minute, gone at a touch |
| 127 | `themes` | both RX | 45 s | the 1:1 suite's cover_a themes on both radios at once (it puts back what it changes): each theme taken live, 1bit included, put back |
| 128 | `themes-alike` | both RX | 45 s | the 1:1 suite's cover_a themes-alike on both radios at once (it puts back what it changes): the built-in themes drawn alike on the two radios |
| 129 | `zoom-step` | both RX | 45 s | the 1:1 suite's cover_a zoom-step on both radios at once (it puts back what it changes): the zoom kept per mode |
| 130 | `step-zoom` | both RX | 45 s | the 1:1 suite's cover_a step-zoom on both radios at once (it puts back what it changes): the tuning step kept per zoom |
| 131 | `band-plan` | both RX | 45 s | the 1:1 suite's cover_a band-plan on both radios at once (it puts back what it changes): the band plan's segments as set |
| 132 | `own-band` | both RX | 45 s | the 1:1 suite's cover_a own-band on both radios at once (it puts back what it changes): a band of the operator's own on the main screen's plan |
| 133 | `mode-from-plan` | both RX | 45 s | the 1:1 suite's cover_a mode-from-plan on both radios at once (it puts back what it changes): the mode taken from the band plan as the dial moves |
| 134 | `lock-per-band` | both RX | 45 s | the 1:1 suite's cover_a lock-per-band on both radios at once (it puts back what it changes): the dial lock kept per band |
| 135 | `channel-snap` | both RX | 45 s | the 1:1 suite's cover_a channel-snap on both radios at once (it puts back what it changes): a channel band's dial snapped to its channels |
| 136 | `speaker-off` | both RX | 45 s | the 1:1 suite's cover_a speaker-off on both radios at once (it puts back what it changes): the speaker off keeps the audio elsewhere |
| 137 | `line-in` | both RX | 45 s | the 1:1 suite's cover_a line-in on both radios at once (it puts back what it changes): the line in as the audio source |
| 138 | `transverters` | both RX | 45 s | the 1:1 suite's cover_a transverters on both radios at once (it puts back what it changes): a transverter's offsets and its power limit |
| 139 | `tx-shift` | both RX | 45 s | the 1:1 suite's cover_a tx-shift on both radios at once (it puts back what it changes): a TX shift's frequency, and a shift below 0 Hz refused |
| 140 | `tx-outside` | both RX | 45 s | the 1:1 suite's cover_a tx-outside on both radios at once (it puts back what it changes): transmitting outside the ham bands: the switch refuses it while off |
| 141 | `bootvideo` | both RX | 45 s | the 1:1 suite's cover bootvideo on both radios at once: bootvideo |
| 142 | `channels-web` | both RX | 45 s | the 1:1 suite's cover channels-web on both radios at once: channels web |
| 143 | `config-files` | both RX | 45 s | the 1:1 suite's cover config-files on both radios at once: config files |
| 144 | `filter-cw` | both RX | 45 s | the 1:1 suite's cover filter-cw on both radios at once: filters |
| 145 | `filter-wfm` | both RX | 45 s | the 1:1 suite's cover filter-wfm on both radios at once: filters |
| 146 | `ft8-modes-knob` | both RX | 45 s | the 1:1 suite's cover ft8-modes-knob on both radios at once: ft8 more |
| 147 | `gui-cpu` | both RX | 45 s | the 1:1 suite's cover gui-cpu on both radios at once: The GUI's main thread on the Sentry's A against the manual's table, and a Panel connection (no pictures) costing it nothing: up to v0.3.12.47 any connection had the screen copied for the picture encod |
| 148 | `refusals` | both RX | 45 s | the 1:1 suite's cover refusals on both radios at once: refusals |
| 149 | `rigctl-local` | both RX | 45 s | the 1:1 suite's cover rigctl-local on both radios at once: rigctl local |
| 150 | `sessions-mtime` | both RX | 45 s | the 1:1 suite's cover sessions-mtime on both radios at once: sessions mtime |
| 151 | `smartcw-guard` | both RX | 45 s | the 1:1 suite's cover smartcw-guard on both radios at once: smartcw guard |
| 152 | `tci-switches` | both RX | 45 s | the 1:1 suite's cover tci-switches on both radios at once: tci switches |
| 153 | `bt-tci-service` | both RX | 45 s | the 1:1 suite's cover bt-tci-service on both radios at once: tci switches |
| 154 | `walkie-upright` | both RX | 45 s | the 1:1 suite's cover walkie-upright on both radios at once: walkie upright |
| 155 | `web-macros` | both RX | 45 s | the 1:1 suite's cover web-macros on both radios at once: web writes |
| 156 | `web-ssh-keys` | both RX | 45 s | the 1:1 suite's cover web-ssh-keys on both radios at once: web writes |
| 157 | `web-own-name` | both RX | 45 s | the 1:1 suite's cover web-own-name on both radios at once: web writes |
| 158 | `winlink-pat` | both RX | 45 s | the 1:1 suite's cover winlink-pat on both radios at once: winlink pat |
| 159 | `a-band` | both RX | 45 s | the 1:1 suite's cover_b a-band on both radios at once: a band |
| 160 | `a-scan` | both RX | 45 s | the 1:1 suite's cover_b a-scan on both radios at once: a scan |
| 161 | `a-span` | both RX | 45 s | the 1:1 suite's cover_b a-span on both radios at once: a span |
| 162 | `agc-split` | both RX | 45 s | the 1:1 suite's cover_b agc-split on both radios at once: agc split |
| 163 | `band-across` | both RX | 45 s | the 1:1 suite's cover_b band-across on both radios at once: band across |
| 164 | `c-dwell` | both RX | 45 s | the 1:1 suite's cover_b c-dwell on both radios at once: c dwell |
| 165 | `c-empty` | both RX | 45 s | the 1:1 suite's cover_b c-empty on both radios at once: c empty |
| 166 | `c-listen` | both RX | 45 s | the 1:1 suite's cover_b c-listen on both radios at once: c listen |
| 167 | `close-rtty` | both RX | 45 s | the 1:1 suite's cover_b close-rtty on both radios at once: The remote's close closes the RTTY panel (the one app with no window): opened (E's types, as open_app does), the state's app RTTY, then "close": none within 3 s |
| 168 | `d-e-rule` | both RX | 45 s | the 1:1 suite's cover_b d-e-rule on both radios at once: d e rule |
| 169 | `d-scan-listen` | both RX | 45 s | the 1:1 suite's cover_b d-scan-listen on both radios at once: d scan listen |
| 170 | `d-sweep-mark` | both RX | 45 s | the 1:1 suite's cover_b d-sweep-mark on both radios at once: d sweep mark |
| 171 | `df-keys` | both RX | 45 s | the 1:1 suite's cover_b df-keys on both radios at once: df keys |
| 172 | `df-source` | both RX | 45 s | the 1:1 suite's cover_b df-source on both radios at once: df source |
| 173 | `df-targets` | both RX | 45 s | the 1:1 suite's cover_b df-targets on both radios at once: df targets |
| 174 | `df-tune` | both RX | 45 s | the 1:1 suite's cover_b df-tune on both radios at once: df tune |
| 175 | `e-emergency` | both RX | 45 s | the 1:1 suite's cover_b e-emergency on both radios at once: e emergency |
| 176 | `e-scan-sel` | both RX | 45 s | the 1:1 suite's cover_b e-scan-sel on both radios at once: e scan sel |
| 177 | `f-classic` | both RX | 45 s | the 1:1 suite's cover_b f-classic on both radios at once: f classic |
| 178 | `f-go` | both RX | 45 s | the 1:1 suite's cover_b f-go on both radios at once: f go |
| 179 | `f-open` | both RX | 45 s | the 1:1 suite's cover_b f-open on both radios at once: f open |
| 180 | `f-shell` | both RX | 45 s | the 1:1 suite's cover_b f-shell on both radios at once: f shell |
| 181 | `f-tribal` | both RX | 45 s | the 1:1 suite's cover_b f-tribal on both radios at once: f tribal |
| 182 | `fst` | both RX | 45 s | the 1:1 suite's cover_b fst on both radios at once: fst |
| 183 | `g-3d` | both RX | 45 s | the 1:1 suite's cover_b g-3d on both radios at once: g 3d |
| 184 | `g-list` | both RX | 45 s | the 1:1 suite's cover_b g-list on both radios at once: G's F4 CH LIST on CB: the MFK two down the list and its push tune CB 21 from CB 19 and close the list (the dial moves no more with the MFK: off the list it goes group to group, so a third click would  |
| 185 | `g-own` | both RX | 45 s | the 1:1 suite's cover_b g-own on both radios at once: G opens on its own channel and gives A's dial back as it is left; a channel tuned on G is G's own, a band's dial is not; the remote's wch says it and "set wch" sets the memory alone (a frequency the r |
| 186 | `gen-help` | both RX | 45 s | the 1:1 suite's cover_b gen-help on both radios at once: gen help |
| 187 | `home` | both RX | 45 s | the 1:1 suite's cover_b home on both radios at once: home |
| 188 | `key-actions` | both RX | 45 s | the 1:1 suite's cover_b key-actions on both radios at once: key actions |
| 189 | `keys` | both RX | 45 s | the 1:1 suite's cover_b keys on both radios at once: keys |
| 190 | `knob-bind` | both RX | 45 s | the 1:1 suite's cover_b knob-bind on both radios at once: knob bind |
| 191 | `listen` | both RX | 45 s | the 1:1 suite's cover_b listen on both radios at once: listen |
| 192 | `manual` | both RX | 45 s | the 1:1 suite's cover_b manual on both radios at once: manual |
| 193 | `page-key-hold` | both RX | 45 s | the 1:1 suite's cover_b page-key-hold on both radios at once: page key hold |
| 194 | `pool` | both RX | 45 s | the 1:1 suite's cover_b pool on both radios at once: pool |
| 195 | `scancfg-astep` | both RX | 45 s | the 1:1 suite's cover_b scancfg-astep on both radios at once: scancfg astep |
| 196 | `scancfg-chans` | both RX | 45 s | the 1:1 suite's cover_b scancfg-chans on both radios at once: scancfg chans |
| 197 | `scancfg-open` | both RX | 45 s | the 1:1 suite's cover_b scancfg-open on both radios at once: scancfg open |
| 198 | `scancfg-rec` | both RX | 45 s | the 1:1 suite's cover_b scancfg-rec on both radios at once: scancfg rec |
| 199 | `scancfg-step` | both RX | 45 s | the 1:1 suite's cover_b scancfg-step on both radios at once: scancfg step |
| 200 | `scancfg-style` | both RX | 45 s | the 1:1 suite's cover_b scancfg-style on both radios at once: scancfg style |
| 201 | `scancfg-sweep` | both RX | 45 s | the 1:1 suite's cover_b scancfg-sweep on both radios at once: scancfg sweep |
| 202 | `tune-power-remote` | both RX | 45 s | the 1:1 suite's cover_b tune-power-remote on both radios at once: The remote's tpwr: TUNE's own power read and set, saved in the settings database as Settings does (tenths: 0.2 W is "2"), "set tpwr 0" clamped to 0.1 W; nothing keyed |
| 203 | `user-page` | both RX | 45 s | the 1:1 suite's cover_b user-page on both radios at once: user page |
| 204 | `value-buttons` | both RX | 45 s | the 1:1 suite's cover_b value-buttons on both radios at once: value buttons |
| 205 | `vol-knob` | both RX | 45 s | the 1:1 suite's cover_b vol-knob on both radios at once: vol knob |
| 206 | `walk` | both RX | 45 s | the 1:1 suite's cover_b walk on both radios at once: walk |
| 207 | `x6100-keys` | both RX | 45 s | the 1:1 suite's cover_b x6100-keys on both radios at once: x6100 keys |
| 208 | `air smeter-front-end` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a smeter-front-end over the air, run whole: smeter-front-end |
| 209 | `air readings-zero` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a readings-zero over the air, run whole: readings-zero |
| 210 | `air band-edges` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a band-edges over the air, run whole: band-edges |
| 211 | `air keys-with-ptt` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a keys-with-ptt over the air, run whole: keys-with-ptt |
| 212 | `air armed-ptt-a` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a armed-ptt-a over the air, run whole: armed-ptt-a |
| 213 | `air smeter-am` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a smeter-am over the air, run whole: smeter-am |
| 214 | `air comp` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a comp over the air, run whole: comp |
| 215 | `air split` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a split over the air, run whole: split |
| 216 | `air split-cw` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a split-cw over the air, run whole: split-cw |
| 217 | `air cw-alc` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a cw-alc over the air, run whole: cw-alc |
| 218 | `air qsy-out` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a qsy-out over the air, run whole: qsy-out |
| 219 | `air tune` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune over the air, run whole: tune |
| 220 | `air tune-power` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune-power over the air, run whole: tune-power |
| 221 | `air tune-app` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune-app over the air, run whole: tune-app |
| 222 | `air tune-key` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a tune-key over the air, run whole: tune-key |
| 223 | `air text-log` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a text-log over the air, run whole: text-log |
| 224 | `air codec-clock` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a codec-clock over the air, run whole: codec-clock |
| 225 | `air power-loop` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a power-loop over the air, run whole: path-tx-power-loop-bursts (tx-x6100-gcfix5-pause-hold from an X6100 on gcfix4/5, at 0.1 and 1 W): tones.py --bursts as a child, each way: every burst after its pause within 1 dB of its steady level (overshoot and dip); o |
| 226 | `air ft8-tx` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_a ft8-tx over the air, run whole: ft8-tx |
| 227 | `air cw-copy` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b cw-copy over the air, run whole: cw-copy |
| 228 | `air cw-carrier` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b cw-carrier over the air, run whole: cw-carrier |
| 229 | `air cw-kbd` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b cw-kbd over the air, run whole: cw-kbd |
| 230 | `air rtty` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b rtty over the air, run whole: rtty |
| 231 | `air psk` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b psk over the air, run whole: psk |
| 232 | `air jtty` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b jtty over the air, run whole: jtty |
| 233 | `air olivia` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b olivia over the air, run whole: olivia |
| 234 | `air wefax-cont` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b wefax-cont over the air, run whole: wefax-cont |
| 235 | `air wefax-auto` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b wefax-auto over the air, run whole: wefax-auto |
| 236 | `air ft8-multistream` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-multistream over the air, run whole: ft8-multistream |
| 237 | `air ft8-qso` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-qso over the air, run whole: ft8-qso |
| 238 | `air ft8-hold` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-hold over the air, run whole: ft8-hold |
| 239 | `air ft8-late` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b ft8-late over the air, run whole: ft8-late |
| 240 | `air js8` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_b js8 over the air, run whole: js8 |
| 241 | `air tci-mic` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-mic over the air, run whole: tci-mic |
| 242 | `air tci-audio` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-audio over the air, run whole: tci-audio |
| 243 | `air tci-cw` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-cw over the air, run whole: tci-cw |
| 244 | `air tci-level` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-level over the air, run whole: tci-level |
| 245 | `air tci-ask` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c tci-ask over the air, run whole: tci-ask |
| 246 | `air sentry-lock` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c sentry-lock over the air, run whole: sentry-lock |
| 247 | `air g-moni` | both ways (each direction: one sends 0.1 W, the other listens) | 120 s | the 1:1 suite's air_c g-moni over the air, run whole: g-moni |
| 248 | `air paths bands` | both ways | 600 s | an over on every band both radios share, 160 to 6 m, each way: heard, the receiver's level per band (suite_more band_proofs) |
| 249 | `air paths walkie` | both ways | 400 s | the walkie in AM and FM on 20 m and 10 m FM, each way (bench.py radios-walkie) |
| 250 | `air paths cw` | both ways | 180 s | CW text through TCI each way ('VVV TEST DE <test call>'): the listener's decoder copies the words (suite_more cw_over) |
| 251 | `air paths if-shift` | X6200 TX, X6100 RX | 120 s | the X6100's IF shift at +-2000 Hz: the tone's pitch kept, the base 2.1 kHz the other way (ifshift.py) |
| 252 | `system-checks` | both | 60 s | each radio read over SSH, nothing changed: battery log and model, data partition, settings' good copy, password logins refused, Tailscale, logs, start trace, services and ports, CPU, temperature, GPU, sessions file, update state, ADIF header, libft8, clock; the same version on both |
| 253 | `image x6100` | X6100 TX, X6200 RX (1500 Hz below, declared) | 120 s | the X6100's opposite-sideband image and carrier leak on 20, 17, 15, 12 and 10 m at 0.1 W: each image -35 dBc or lower (its 15 m shifts, 11-22); the carrier leak said |
| 254 | `image x6200` | X6200 TX, X6100 RX (1500 Hz below, declared) | 120 s | the X6200's image and carrier leak, as image x6100 |
| 255 | `spurs x6200` | X6200 TX, X6100 RX | 90 s | what comes with the X6200's clean 1500 Hz tone in the X6100's audio (USB-D, the FT8 dial), the X6100's AGC on and off, the tone at 0.3 and 0.9 of full scale: nothing 156.25 Hz either side over -30 dBc (the X6100's FT8 app decoded the X6200's answers 156 Hz off, 11-23 to 11-25) |
| 256 | `skirt x6200 levels` | X6200 TX, X6100 RX | 200 s | the X6200's skirt at 0.1 W over SKIRT_OVERS alike TCI overs (a tone at 0.3 of full scale; the levels' sweep could not reach the base, its TCI holding the drive): each over's power 100-155 Hz off the tone per 55 Hz at most SKIRT_MAX_DBC (11-25/26: now and then an early over -31 to -35 dBc, the rest -55) |
| 257 | `spurs x6100` | X6100 TX, X6200 RX | 90 s | the same the other way: the X6100's tone in the X6200's audio |
| 258 | `tci-tail x6200` | X6200 TX, X6100 RX | 90 s | a TCI over's end on the air: the X6200's TCI plays 2 s of 1000 Hz and unkeys at once (WSJT-X's way), the X6100 must hear all of it; then with 1.5 s of silence after it (the owner's ask, 11-24: the X6200 dropped 1.2-1.4 s, the X6100 0.3-0.5 s, and the bench pads 0.8 s) |
| 259 | `tci-tail x6100` | X6100 TX, X6200 RX | 90 s | the same the other way: the X6100's TCI over in the X6200's audio |
| 260 | `ft8-copies x6200` | X6200 TX, X6100 RX | 210 s | the X6200's FT8 app's CQs from TX Hz 1325 (no VFO move), 1500 and 2000 at 0.1 W in the X6100's audio, then the same message through its TCI: the power 156.25 Hz either side -40 dBc at most and no more than 6 dB over TCI's (till 11-26 -25 to -31 dBc, TCI -55: the app played one level at any power and the base took it down to its setting) |
| 261 | `ft8-copies x6200 1w` | X6200 TX at 1 W, X6100 RX | 150 s | the X6200's FT8 CQ from 1500 Hz at 1 W, then through its TCI: as ft8-copies x6200 (-44 dBc on 11-25) |
| 262 | `ft8-copies x6200 tci` | X6200 TX, X6100 RX | 90 s | the same FT8 message through the X6200's TCI (made here): whether its skirt is the FT8 waveform through its base or the app's way out (said) |
| 263 | `ft8-copies x6200 tci app` | X6200 TX, X6100 RX | 120 s | ft8-copies x6200 tci with its FT8 app open meanwhile: whether the app's waterfall, decoder and capture are what disturb its audio (said) |
| 264 | `ft8-copies x6100` | X6100 TX at 1 W, X6200 RX | 210 s | the same the other way at 1 W: the X6100's FT8 app's CQs, then through its TCI, in the X6200's audio (at 0.1 W the X6200's floor, -33 dBc through the cable, hides -40: LOOK 11-26) |
| 265 | `cw-timing x6100` | X6100 TX, X6200 RX | 90 s | the X6100's CW keyer as it reaches the air: PARIS x4 at 20 WPM through its TCI, read element by element in the X6200's audio: none merged or lost, each mark and gap within 25 ms of its kind (11-25: its gaps cut short by a signal, its base's key-up late) |
| 266 | `cw-timing x6200` | X6200 TX, X6100 RX | 90 s | the same the other way: the X6200's keyer in the X6100's audio |
| 267 | `cw-timing x6100 1w` | X6100 TX at 1 W, X6200 RX | 90 s | the X6100's keyer at 1 W: whether its base's late key-ups at 0.1 W are its power loop that low |
| 268 | `ambe-vocoder` | both RX | 150 s | each radio's AMBE chip through its SDR daemon: the parity field asked for, the speech through each chip at DMR's and D-STAR's rates and the other's frames, libmbe-dvsi alike with the parity field off, P25 full rate only on a chip with the P25 option |
| 269 | `dv-dmr` | both RX | 70 s | a DMR call of the test speech replayed into each radio's SDR daemon: TG 3100 from 3112345 shown, its voice through the radio's own AMBE chip like the speech (r >= 0.9) |
| 270 | `dv-dmr-noparity` | both RX | 60 s | the DMR call again with both chips' parity field off: the test daemon reads the chip's answers whole through B's daemon's port; the field on again after |
| 271 | `dv-dstar` | both RX | 60 s | a D-STAR call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.4) |
| 272 | `dv-ysf` | both RX | 60 s | a YSF call of the test speech: CQCQCQ from N0CALL shown, its voice heard (r >= 0.7) |
| 273 | `dv-nxdn` | both RX | 60 s | an NXDN48 call of the test speech: TG 100 from 1234 shown, its voice heard (r >= 0.75) |
| 274 | `dv-p25` | both RX | 60 s | a P25 phase 1 call (1 kHz IMBE frames): TG 4321 from 1234567 shown; the tone heard where the chip has DVSI's P25 option, silence where it has not |
| 275 | `tci-audio-rate` | both RX | 15 s | each radio's TCI receiver audio at 97-103 % of real time |
| 276 | `tci-takes-control` | both RX | 20 s | a TCI client's set takes control (ctl tci) on both; gone, control back |
| 277 | `rx-settings` | both RX | 40 s | PRE, ATT, AGC, NB, NR, squelch, RF gain: each set on both, read back alike |
| 278 | `ptt-lease x6100` | X6100 TX, X6200 RX | 20 s | one PTT never renewed: off the air by itself within 4 s |
| 279 | `ptt-lease x6200` | X6200 TX, X6100 RX | 20 s | one PTT never renewed: off the air by itself within 4 s |
| 280 | `ptt-locked x6100` | both RX | 12 s | the X6100's PTT with its transmitter locked: stays off the air |
| 281 | `ptt-locked x6200` | both RX | 12 s | the X6200's PTT with its transmitter locked: stays off the air |
| 282 | `ptt-out-of-band x6100` | both RX | 12 s | the X6100 armed on 15.000 MHz (WWV, no amateur band): PTT refused |
| 283 | `ptt-out-of-band x6200` | both RX | 12 s | the X6200 armed on 15.000 MHz (WWV, no amateur band): PTT refused |
| 284 | `nifog` | both RX | 90 s | F > NIFOG on both: F2 opens it, the MFK, F3 and the VFO move through its pages, F2 closes; /nifog, CISA's PDF and a page's PNG served |
| 285 | `nifog-list` | both RX | 30 s | F's list: NIFOG its row 11 (s.sys, fsel) |
| 286 | `nifog-open-close` | both RX | 60 s | F2 and the push open and close NIFOG, page A closes it |
| 287 | `nifog-drawn` | both RX | 120 s | pages 30 and 68 drawn after G: centred, across, the theme's colours; the log |
| 288 | `nifog-caption` | both RX | 120 s | the caption on pages 1, 4, 38, 68 as the contents say |
| 289 | `nifog-mfk` | both RX | 120 s | the MFK a page on and back, between 1 and 192 |
| 290 | `nifog-vfo` | both RX | 120 s | the VFO 24 px a click, over the page's ends; no tuning |
| 291 | `nifog-sections` | both RX | 90 s | F1 and F3 by section from page 40 |
| 292 | `nifog-contents` | both RX | 90 s | F4 the contents from page 68 |
| 293 | `nifog-chapter` | both RX | 150 s | F5 by chapter, from the last to the cover |
| 294 | `nifog-legend-keys` | both RX | 40 s | NIFOG's legend and F keys while open |
| 295 | `nifog-url` | both RX | 40 s | the line under the page: the radio's name and address |
| 296 | `nifog-reopen` | both RX | 90 s | opened again on the page and scroll left, drawn |
| 297 | `nifog-sentry-closed` | both RX | 60 s | the Sentry closed under NIFOG: F's list again, NIFOG draws |
| 298 | `nifog-files` | both RX | 60 s | /usr/share/x6100/nifog: the PDF, the contents, 192 PNGs |
| 299 | `web-nifog-pdf` | both RX | 120 s | /nifog.pdf inline, saved, a Range |
| 300 | `web-nifog-pages` | both RX | 300 s | every page's PNG as the radio's file; 404 past the ends |
| 301 | `web-nifog-viewer` | both RX | 90 s | the /nifog viewer in headless Chromium |
| 302 | `web-nifog-menu` | both RX | 20 s | NIFOG in the web menu and on General |
| 303 | `cw-dec-snr x6100>x6200` | X6100 TX, X6200 RX | 90 s | Dec SNR 5 copies a weak signal, 30 misses it, 5 again copies |
| 304 | `cw-dec-snr x6200>x6100` | X6200 TX, X6100 RX | 90 s | Dec SNR 5 copies a weak signal, 30 misses it, 5 again copies |
| 305 | `cw-parse x6100>x6200` | X6100 TX, X6200 RX | 60 s | run-together CW printed split with Parse on, run together off |
| 306 | `cw-parse x6200>x6100` | X6200 TX, X6100 RX | 60 s | run-together CW printed split with Parse on, run together off |
| 307 | `cw-zap x6100>x6200` | X6100 TX, X6200 RX | 90 s | 200 Hz off, ZAP tunes onto the station within 10 Hz, in CW and CW-R |
| 308 | `cw-zap x6200>x6100` | X6200 TX, X6100 RX | 90 s | 200 Hz off, ZAP tunes onto the station within 10 Hz, in CW and CW-R |
| 309 | `cw-tuner x6100>x6200` | X6100 TX, X6200 RX | 70 s | the tuning bars 30 Hz off in CW and CW-R (pictures) |
| 310 | `cw-tuner x6200>x6100` | X6200 TX, X6100 RX | 70 s | the tuning bars 30 Hz off in CW and CW-R (pictures) |
| 311 | `cw-key-train x6100>x6200` | X6100 TX, X6200 RX | 60 s | KEY Train: TCI CW at 0 W, nothing heard; Train off: decoded |
| 312 | `cw-key-train x6200>x6100` | X6200 TX, X6100 RX | 60 s | KEY Train: TCI CW at 0 W, nothing heard; Train off: decoded |
| 313 | `cw-own-carrier x6100>x6200` | X6100 TX, X6200 RX | 50 s | the sender's TX meters and ALC; its classic screen on the air (picture) |
| 314 | `cw-own-carrier x6200>x6100` | X6200 TX, X6100 RX | 50 s | the sender's TX meters and ALC; its classic screen on the air (picture) |
| 315 | `cw-log-qso x6100>x6200` | X6100 TX, X6200 RX | 70 s | the decoder's confirmed call logged by Log QSO (CW 1:2) |
| 316 | `cw-log-qso x6200>x6100` | X6200 TX, X6100 RX | 70 s | the decoder's confirmed call logged by Log QSO (CW 1:2) |
| 317 | `cw-msg x6100>x6200` | X6100 TX, X6200 RX | 60 s | MSG > CW 'TEST {CALL} TEST': decoded with the call, one over through the gaps |
| 318 | `cw-msg x6200>x6100` | X6200 TX, X6100 RX | 60 s | MSG > CW 'TEST {CALL} TEST': decoded with the call, one over through the gaps |
| 319 | `cw-his x6100>x6200` | X6100 TX, X6200 RX | 80 s | {HIS}: the call the decoder confirmed sent back |
| 320 | `cw-his x6200>x6100` | X6200 TX, X6100 RX | 80 s | {HIS}: the call the decoder confirmed sent back |
| 321 | `cw-beacon x6100>x6200` | X6100 TX, X6200 RX | 90 s | the beacon: 10 s pause, Until 2 sends, then 'Beacon over: 2 sends' |
| 322 | `cw-beacon x6200>x6100` | X6200 TX, X6100 RX | 90 s | the beacon: 10 s pause, Until 2 sends, then 'Beacon over: 2 sends' |
| 323 | `cw-beacon-stops x6100>x6200` | X6100 TX, X6200 RX | 70 s | a beacon stopped in its pause by rigctl's T 1 / T 0 |
| 324 | `cw-beacon-stops x6200>x6100` | X6200 TX, X6100 RX | 70 s | a beacon stopped in its pause by rigctl's T 1 / T 0 |
| 325 | `qrss3 x6100>x6200` | X6100 TX, X6200 RX | 70 s | QRSS3 on 30 m: a 3 s dot on 10.140050; retuned mid-send: stopped |
| 326 | `qrss3 x6200>x6100` | X6200 TX, X6100 RX | 70 s | QRSS3 on 30 m: a 3 s dot on 10.140050; retuned mid-send: stopped |
| 327 | `cwkbd-live x6100>x6200` | X6100 TX, X6200 RX | 60 s | CW Keyboard's on-screen keyboard with Live: each key keyed at once |
| 328 | `cwkbd-live x6200>x6100` | X6200 TX, X6100 RX | 60 s | CW Keyboard's on-screen keyboard with Live: each key keyed at once |
| 329 | `bands-beacon x6100>x6200` | X6100 TX, X6200 RX | 90 s | the Bands app's Test beacon: two presses, decoded, back; ESC: nothing |
| 330 | `bands-beacon x6200>x6100` | X6200 TX, X6100 RX | 90 s | the Bands app's Test beacon: two presses, decoded, back; ESC: nothing |
| 331 | `field-beacon x6100>x6200` | X6100 TX, X6200 RX | 120 s | Field Ops' Test beacon: decoded; a third press stops it at once |
| 332 | `field-beacon x6200>x6100` | X6200 TX, X6100 RX | 120 s | Field Ops' Test beacon: decoded; a third press stops it at once |
| 333 | `field-cq x6100>x6200` | X6100 TX, X6200 RX | 90 s | Field Ops' CQ in a SOTA activation: decoded, CQ again stops it; SSB's voice CQ |
| 334 | `field-cq x6200>x6100` | X6200 TX, X6100 RX | 90 s | Field Ops' CQ in a SOTA activation: decoded, CQ again stops it; SSB's voice CQ |
| 335 | `prop-beacon x6100>x6200` | X6100 TX, X6200 RX | 75 s | Propagation's Beacons on 20 m score a carrier in a beacon's slot |
| 336 | `prop-beacon x6200>x6100` | X6200 TX, X6100 RX | 75 s | Propagation's Beacons on 20 m score a carrier in a beacon's slot |
| 337 | `scanner x6100>x6200` | X6100 TX, X6200 RX | 120 s | the Scanner stops on a carrier and goes on after Hang; frozen while keyed |
| 338 | `scanner x6200>x6100` | X6200 TX, X6100 RX | 120 s | the Scanner stops on a carrier and goes on after Hang; frozen while keyed |
| 339 | `ft8-cq-grid x6100>x6200` | X6100 TX, X6200 RX | 120 s | FT8 CQ: the 4-character square, none from 2, the POTA modifier |
| 340 | `ft8-cq-grid x6200>x6100` | X6200 TX, X6100 RX | 120 s | FT8 CQ: the 4-character square, none from 2, the POTA modifier |
| 341 | `ft8-retries x6100>x6200` | X6100 TX, X6200 RX | 150 s | an unanswered reply sent Retries (3) times, then dropped |
| 342 | `ft8-retries x6200>x6100` | X6200 TX, X6100 RX | 150 s | an unanswered reply sent Retries (3) times, then dropped |
| 343 | `ft8-auto-off x6100>x6200` | X6100 TX, X6200 RX | 120 s | Auto off: nothing moves on until the press |
| 344 | `ft8-auto-off x6200>x6100` | X6200 TX, X6100 RX | 120 s | Auto off: nothing moves on until the press |
| 345 | `ft8-compound x6100>x6200` | X6100 TX, X6200 RX | 90 s | a compound call's CQ; a call too long refused |
| 346 | `ft8-compound x6200>x6100` | X6200 TX, X6100 RX | 90 s | a compound call's CQ; a call too long refused |
| 347 | `ft8-list-look x6100>x6200` | X6100 TX, X6200 RX | 120 s | the list's distance in km and miles, the For you rows and Show held |
| 348 | `ft8-list-look x6200>x6100` | X6200 TX, X6100 RX | 120 s | the list's distance in km and miles, the For you rows and Show held |
| 349 | `ft8-hound x6100>x6200` | X6100 TX, X6200 RX | 90 s | Hound calls a Fox at 1000 Hz or more |
| 350 | `ft8-hound x6200>x6100` | X6200 TX, X6100 RX | 90 s | Hound calls a Fox at 1000 Hz or more |
| 351 | `js8-speed x6100>x6200` | X6100 TX, X6200 RX | 120 s | Turbo heard with All speeds, not with My speed |
| 352 | `js8-speed x6200>x6100` | X6200 TX, X6100 RX | 120 s | Turbo heard with All speeds, not with My speed |
| 353 | `js8-groups x6100>x6200` | X6100 TX, X6200 RX | 120 s | @X6100 and @GSTFLASH shown, never answered |
| 354 | `js8-groups x6200>x6100` | X6200 TX, X6100 RX | 120 s | @X6100 and @GSTFLASH shown, never answered |
| 355 | `ghostnet x6100>x6200` | X6100 TX, X6200 RX | 150 s | @GHOSTNET SNR? answered (AUTO); @GHOSTNET TEST logged |
| 356 | `ghostnet x6200>x6100` | X6200 TX, X6100 RX | 150 s | @GHOSTNET SNR? answered (AUTO); @GHOSTNET TEST logged |
| 357 | `js8-look x6100>x6200` | X6100 TX, X6200 RX | 180 s | JS8's alert word, a two-way QSO's log popup at 73, Save, the ADIF |
| 358 | `js8-look x6200>x6100` | X6200 TX, X6100 RX | 180 s | JS8's alert word, a two-way QSO's log popup at 73, Save, the ADIF |
| 359 | `sstv-rx x6100>x6200` | X6100 TX, X6200 RX | 90 s | Manual RX Robot 8: picture and _dn with Denoise, one without |
| 360 | `sstv-rx x6200>x6100` | X6200 TX, X6100 RX | 90 s | Manual RX Robot 8: picture and _dn with Denoise, one without |
| 361 | `sstv-image-only x6100>x6200` | X6100 TX, X6200 RX | 90 s | TX IMAGE ONLY then REPEAT LAST TX: two pictures alike |
| 362 | `sstv-image-only x6200>x6100` | X6200 TX, X6100 RX | 90 s | TX IMAGE ONLY then REPEAT LAST TX: two pictures alike |
| 363 | `sstv-halt-hold x6100>x6200` | X6100 TX, X6200 RX | 60 s | knob and BAND_UP held off during TX; Halt TX at once |
| 364 | `sstv-halt-hold x6200>x6100` | X6200 TX, X6100 RX | 60 s | knob and BAND_UP held off during TX; Halt TX at once |
| 365 | `wefax-look x6100>x6200` | X6100 TX, X6200 RX | 60 s | the saved PNG black on white, the screen's fax inverted (Tilt: tools/wefax_plan) |
| 366 | `wefax-look x6200>x6100` | X6200 TX, X6100 RX | 60 s | the saved PNG black on white, the screen's fax inverted (Tilt: tools/wefax_plan) |
| 367 | `psk-look x6100>x6200` | X6100 TX, X6200 RX | 90 s | PSK31: the status's audio and quality, the DX call from 'DE <call>' |
| 368 | `psk-look x6200>x6100` | X6200 TX, X6100 RX | 90 s | PSK31: the status's audio and quality, the DX call from 'DE <call>' |
| 369 | `transverter x6100>x6200` | X6100 TX, X6200 RX | 70 s | the transverter's 0.1 W limit at PWR 1 W; ATU refused there |
| 370 | `transverter x6200>x6100` | X6200 TX, X6100 RX | 70 s | the transverter's 0.1 W limit at PWR 1 W; ATU refused there |
| 371 | `zero-power x6100>x6200` | X6100 TX, X6200 RX | 60 s | Line-in 0 still sends: no false 'TX 0 W' (the warning: tools/txlevel) |
| 372 | `zero-power x6200>x6100` | X6200 TX, X6100 RX | 60 s | Line-in 0 still sends: no false 'TX 0 W' (the warning: tools/txlevel) |
| 373 | `agc-knee x6100>x6200` | X6100 TX, X6200 RX | 120 s | the AGC Knee changes the audio over 0.1/0.3/1 W; where the base ignores them (the X6200's, Xiegu's own X6100 base), Knee and Slope offered nowhere |
| 374 | `agc-knee x6200>x6100` | X6200 TX, X6100 RX | 120 s | the AGC Knee changes the audio over 0.1/0.3/1 W; where the base ignores them (the X6200's, Xiegu's own X6100 base), Knee and Slope offered nowhere |
| 375 | `walkie x6100>x6200` | X6100 TX, X6200 RX | 70 s | Walkie Talkie mode's PTT with the walkie mic at 0.1 W, the mic back after |
| 376 | `walkie x6200>x6100` | X6200 TX, X6100 RX | 70 s | Walkie Talkie mode's PTT with the walkie mic at 0.1 W, the mic back after |
| 377 | `g-lamp x6100>x6200` | X6100 TX, X6200 RX | 45 s | G's lamps on the air and after, judged on their words in the state |
| 378 | `g-lamp x6200>x6100` | X6200 TX, X6100 RX | 45 s | G's lamps on the air and after, judged on their words in the state |
| 379 | `scancfg-priority x6100>x6200` | X6100 TX, X6200 RX | 90 s | SCAN CFG's PRIORITY catches a carrier; F3 SKIP passes it |
| 380 | `scancfg-priority x6200>x6100` | X6200 TX, X6100 RX | 90 s | SCAN CFG's PRIORITY catches a carrier; F3 SKIP passes it |
| 381 | `voice-msg x6100>x6200` | X6100 TX, X6200 RX | 60 s | MSG > Voice on the air on 28.400; refused on 27.185 |
| 382 | `voice-msg x6200>x6100` | X6200 TX, X6100 RX | 60 s | MSG > Voice on the air on 28.400; refused on 27.185 |
| 383 | `winlink-ardop x6100>x6200` | X6100 TX, X6200 RX | 120 s | Pat's ARDOP call keys at 0.1 W on 14.105 USB-D; the dial turned stops it |
| 384 | `winlink-ardop x6200>x6100` | X6200 TX, X6100 RX | 120 s | Pat's ARDOP call keys at 0.1 W on 14.105 USB-D; the dial turned stops it |
| 385 | `winlink-mercury-xara x6100>x6200` | X6100 TX, X6200 RX | 150 s | Mercury and XARA calls keyed, the other silent |
| 386 | `winlink-mercury-xara x6200>x6100` | X6200 TX, X6100 RX | 150 s | Mercury and XARA calls keyed, the other silent |
| 387 | `winlink-busy x6100>x6200` | X6100 TX, X6200 RX | 90 s | Pat holds off a busy frequency; the 500 bandwidth cap |
| 388 | `winlink-busy x6200>x6100` | X6200 TX, X6100 RX | 90 s | Pat holds off a busy frequency; the 500 bandwidth cap |
| 389 | `panel-ptt x6100>x6200` | X6100 TX, X6200 RX | 90 s | the Panel's PTT states, Tap, another client, a stalled link (headless browser) |
| 390 | `panel-ptt x6200>x6100` | X6200 TX, X6100 RX | 90 s | the Panel's PTT states, Tap, another client, a stalled link (headless browser) |
| 391 | `pskreporter x6100>x6200` | X6100 TX, X6200 RX | 450 s | PSK Reporter's report caught on the radio itself, nothing sent out |
| 392 | `pskreporter x6200>x6100` | X6200 TX, X6100 RX | 450 s | PSK Reporter's report caught on the radio itself, nothing sent out |
| 393 | `cw-peak-q x6100` | X6100 RX | 50 s | the X6100's CW peak and its Q in its receive noise, nothing sent; on a base without the filter (Xiegu's own), the GUI assumes none and offers none |
| 394 | `cessb x6200>x6100` | X6200 TX, X6100 RX | 60 s | the X6200's CESSB raises the average power of the same audio; on a base without it (the X6200's stock one), no CESSB card and the remote refuses it |
| 395 | `iq-offsets x6100>x6200` | X6100 TX, X6200 RX | 70 s | the X6100's IQ offsets change its carrier |
| 396 | `patched-rows x6100` | X6100 TX, X6200 RX | 40 s | the patched-base rows hidden on the factory base |
| 397 | `moni x6100` | X6100 TX, X6200 RX | 20 s | Moni 30: the base gets it with the speaker up, 0 at VOL 0 |
| 398 | `moni x6200` | X6200 TX, X6100 RX | 20 s | Moni 30: the base gets it with the speaker up, 0 at VOL 0 |
| 399 | `mic-gain x6100` | X6100 TX, X6200 RX | 20 s | the mic gains read back, Input steps the state's mic |
| 400 | `mic-gain x6200` | X6200 TX, X6100 RX | 20 s | the mic gains read back, Input steps the state's mic |
| 401 | `tx-panel x6100` | X6100 TX, X6200 RX | 40 s | Popups TX on and off while keyed: the panel and the red frames, judged on the pictures |
| 402 | `tx-panel x6200` | X6200 TX, X6100 RX | 40 s | Popups TX on and off while keyed: the panel and the red frames, judged on the pictures |
| 403 | `screensaver-tx x6100` | X6100 TX, X6200 RX | 100 s | a TX ends the screensaver within a second |
| 404 | `screensaver-tx x6200` | X6200 TX, X6100 RX | 100 s | a TX ends the screensaver within a second |
| 405 | `stuck-tx-log` | X6100 TX, X6200 RX | 10 s | every 'TX power set to 0' restored in the base logs |
| 406 | `update-check-off` | X6100 RX | 5 s | both radios' update check off while a run goes |
| 407 | `long-press-busy x6100` | X6100 TX, X6200 RX | 30 s | AB held 1.5 s on the air is a long press |
| 408 | `long-press-busy x6200` | X6200 TX, X6100 RX | 30 s | AB held 1.5 s on the air is a long press |
| 409 | `a-tx-rows x6100` | X6100 TX, X6200 RX | 30 s | A's on-air rows through a 5 s over and none after, counted |
| 410 | `a-tx-rows x6200` | X6200 TX, X6100 RX | 30 s | A's on-air rows through a 5 s over and none after, counted |
| 411 | `qrss3-key-safety` | X6200 TX, X6100 RX | 30 s | QRSS3's key let go on a crash (the owner's word) |
| 412 | `js8-time-sync` | X6100 TX, X6200 RX | 30 s | JS8's Time Sync with a clock 1 s off (the owner's word) |
| 413 | `iradio-tx` | X6100 TX, X6200 RX | 30 s | Internet radio stops on TX (the owner's OK for the internet) |
| 414 | `walkie-atu` | X6100 TX, X6200 RX | 30 s | Walkie Talkie's ATU held tunes (the base's own carrier: the owner's word) |
| 415 | `rade-rx x6100>x6200` | X6100 TX, X6200 RX | 60 s | RADE: a recorded over decoded (the hand-mic session's) |
| 416 | `panel-3min` | X6100 TX, X6200 RX | 200 s | three minutes on the air at most (the owner's word) |
| 417 | `field-update-lists x6100` | X6100 TX, X6200 RX | 300 s | Field Ops' Update list through the web page: the ten lists downloaded, counted, nothing left behind |
| 418 | `field-update-lists x6200` | X6200 TX, X6100 RX | 300 s | Field Ops' Update list through the web page: the ten lists downloaded, counted, nothing left behind |
| 419 | `gmrs-b` | both RX | 40 s | B's channel GMRS 20, GMRS 8 and GMRS 20 IN on both: preset GMRS, NFM, named; A's dial untouched and alike |
| 420 | `gmrs-scan` | both RX | 120 s | B's F1 on GMRS: GMRS's 22 channels in frequency order, each named (each radio's SDR in turn, else the X6200's daemon replaying noise) |
| 421 | `gmrs-g` | both RX | 60 s | G on GMRS 20 and its input through each radio's SDR: refused for GMRS's reason, A/B arms nothing; G back on the radio |
| 422 | `gmrs-f` | both RX | 60 s | F SYSTEM's GMRS panel on both: chosen, opened, the MFK, the tones, F1 tunes on the receiver there (none: nothing), F5 closes; A's dial untouched |
| 423 | `gmrs-xvtr` | both RX | 120 s | a GMRS transverter on both (the WR12's stand-in): F's panel tunes A in NFM, A/B and the PTT refused as GMRS, A's scan of the 22, G's GMRS group on the radio (the SDR's where a radio has one) |
| 424 | `bands-app` | both RX | 60 s | the Bands app opened and closed on both; the MFK and its press tune 40 m's phone frequency in LSB, from CW its CW frequency in CW, from CW-R 20 m's in CW-R |
| 425 | `timesig-f2` | both RX | 40 s | Time Signal's F2 Set clock: Auto, Off, Auto again, as the button reads |
| 426 | `channels-api` | both RX | 60 s | the Memories API: a stale Delete refused, Delete and Undo at its place, a Save of the frequency alone keeps a 40-byte name and an extra key (the files put back) |
| 427 | `wspr-web` | both RX | 20 s | WSPR's Hop and band uses set through the web page, read back there and by cfg? |
| 428 | `cb-refused` | both RX (PTT refused) | 60 s | CB's PTT refused on both (the remote's and a program's TCI audio), each armed in turn; the app sets no power |
| 429 | `hfair` | both RX (PTT refused) | 70 s | HF Air Band: VOLMET first, the MFK tunes a row in USB, PTT refused there |
| 430 | `zello-offline` | both RX | 90 s | Zello with no account: Talk via and No BT mic, SQL 0 refuses the links, one link at a time with SVXConnect both ways |
| 431 | `svx-talk` | both RX | 40 s | SVXConnect not set up: Talk via toggles svx_talk_bt |
| 432 | `walkie-mode` | both RX | 60 s | Walkie Talkie mode: no app inside, VOL the volume, WLK pages, APP held leaves |
| 433 | `walkie-buttons` | both RX | 120 s | the WLK buttons on both, each read back and undone (never ATU) |
| 434 | `walkie-channels` | both RX | 80 s | the walkie channels: Store, the knob in MEM, CH 05 named, Clear held |
| 435 | `walkie-scope` | both RX | 60 s | the walkie bandscope runs with the main spectrum off; the zoom's span |
| 436 | `ft8-time-sync` | both RX | 120 s | FT8's Time Sync: left alone under NTP; 3 s ahead (NTP stopped) moved to the slot, NTP started again |
| 437 | `ft8-log-map` | both RX | 70 s | FT8's Log Map opens and closes, judged on its caption (OCR) |
| 438 | `ft8-custom-freq` | both RX | 90 s | FT8's typed dial 14091 (USB-D, said, on the button), band keys back to the presets, a preset chosen |
| 439 | `psk-squelch` | both RX | 120 s | PSK31's squelch: the knob, psk.conf, off and on; no RX text on with it, noise's text off |
| 440 | `rade-app` | both RX | 90 s | RADE's Freq. list (14.236 USB, 40 m in LSB) and Power steps kept over a reopen |
| 441 | `wefax-freq` | both RX | 70 s | WeFax's Freq.: DDK3 tuned 1.5 kHz under; a WEFAX channel listed by its name |
| 442 | `wefax-view` | both RX | 60 s | WeFax's View: Big, Full Size, Small, Big, each a different screen |
| 443 | `wefax-browser` | both RX | 60 s | WeFax's browser lists and opens a test fax (OCR) |
| 444 | `wefax-power` | both RX | 40 s | WeFax's Power steps 0.1/0.2/0.5/1 W, the button and the state alike |
| 445 | `sstv-browser` | both RX | 70 s | SSTV's RX Pics Browser and Delete picture; the Gallery lists and deletes |
| 446 | `sstv-tx-files` | both RX | 30 s | SSTV's TX folders; a Gallery upload lands in tx_pics |
| 447 | `js8-freq` | both RX (each in a TX session) | 150 s | JS8's Freq: GhostNet mode (7.107, its groups, BAND_UP 14.107), Custom 14080, JS8Call's again; one radio at a time in a TX session |
| 448 | `ghostnet-popup` | both RX (each in a TX session) | 150 s | GhostNet's popup: a net's reminder set and cleared, its RTTY entry (7.077 USB, the stored RTTY settings untouched) |
| 449 | `ghostnet-reminder` | both RX | 80 s | GhostNet's reminder of an own net 4 min ahead said on any screen |
| 450 | `js8-open-off` | both RX | 60 s | JS8 opens with AUTO and HB off; nothing sent in 30 s |
| 451 | `js8-nocall` | both RX (CQ refused, a TX session) | 90 s | JS8 with no callsign: CQ and send refused, off the air |
| 452 | `js8-texts` | both RX (each in a TX session) | 120 s | JS8's INFO typed, in js8_texts.txt, shown again after a reopen |
| 453 | `js8-map` | both RX | 70 s | JS8's Map opens and closes |
| 454 | `field-home` | both RX | 60 s | Field Ops' home: its buttons, the row round in 15 steps, LOG, Bands left of LOG |
| 455 | `field-sota` | both RX | 300 s | a SOTA activation end to end: nearest/MFK/typed, PHONE and QTH, Start, QSOs (SSB, FT8, no mode set on the web), not saved on a read-only folder, the logbook, End, (old) |
| 456 | `field-beacon-rules` | both RX | 120 s | Field Ops' Test beacon: CW first, one press sends nothing, tuned, locked; Use band in CW and USB |
| 457 | `field-programmes` | both RX | 300 s | TOTA, WCA, MOTA, LH, LLOTA: typed refs, counters, MY_SIG/SIG |
| 458 | `field-tiles` | both RX | 120 s | Tiles: the square followed, a T3 QSO's MY_SIG TILES |
| 459 | `field-xota-nota` | both RX | 150 s | XOTA: the copy message, an own place, no spot, MY_SIG XOTA; NOTA's Spot message and a row into its programme |
| 460 | `field-map` | both RX | 150 s | Field Ops' map from test tiles: You are there, km, View, Zoom, No map here |
| 461 | `field-spotting` | both RX | 240 s | WWFF, WWBOTA, HOTA with a CB call: no spot (said), counters, MY_SIG/SIG |
| 462 | `field-persist` | both RX | 180 s | Field Ops over a GUI restart: Start here, the activation as it was |
| 463 | `channels-app-holds` | both RX | 70 s | Channels by the hand mic's V/M over FT8: not tuned, said, kept |
| 464 | `birdie-marks` | both RX | 200 s | birdie marks: five red ticks evenly spaced at the waterfall's top (test list, GUI restarts) |
| 465 | `cwkbd-refusals` | both RX (CW refused) | 80 s | CW Keyboard: NOT CW and 'Set CW mode' in USB, 'No TX' on 27.185 |
| 466 | `prop-conditions` | both RX | 40 s | Propagation shows the kept file's SFI on both |
| 467 | `flights` | both RX | 60 s | Flights' Range round the five; the header Locator, then Phone |
| 468 | `weather` | both RX | 60 s | Weather's header: Locator, then Phone |
| 469 | `cwtrainer-learn` | each in turn keyed at 0 W (train), the other RX | 240 s | CW Trainer Learn: Listen..., K/M groups keyed back right by TCI CW, a prosign <?> wrong, 5 WPM right (one radio at a time, 0 W sidetone) |
| 470 | `cwtrainer-calls` | each in turn keyed at 0 W (train), the other RX | 240 s | CW Trainer Calls: right with a space inside, +1 WPM (Follows), Fixed keeps KEY Speed, back after Calls |
| 471 | `cwtrainer-out-of-band` | each in turn keyed at 0 W (train), the other RX | 150 s | CW Trainer: refused outside South Korea's ranges (7.250): 'No TX on 7.250 MHz', nothing keyed |
| 472 | `first-start` | both RX | 300 s | the first-start guide opens with no callsign (GUI restarts), walked, Done, not again |
| 473 | `zello-ptt` | each TX in turn (0.1 W), the other RX | 90 s | Zello holds the PTT while open; closed, the PTT keys at 0.1 W (cable) |
| 474 | `svx-ptt` | each TX in turn (0.1 W), the other RX | 90 s | SVXConnect holds the PTT while open; closed, the PTT keys at 0.1 W (cable) |
| 475 | `psk-cursor x6100>x6200` | X6100 TX, X6200 RX | 90 s | PSK31 at 0.1 W: the listener's cursor 40 Hz off, a main-knob click snaps it onto the signal, the call decoded there |
| 476 | `psk-cursor x6200>x6100` | X6200 TX, X6100 RX | 90 s | PSK31 at 0.1 W: the listener's cursor 40 Hz off, a main-knob click snaps it onto the signal, the call decoded there |
| 477 | `sstv-reply x6100>x6200` | X6100 TX, X6200 RX | 150 s | SSTV at 0.1 W: Reply and Replay to a typed target as received (Robot 8), TX MODE in Robot 36 (halted at 11 s) |
| 478 | `sstv-reply x6200>x6100` | X6200 TX, X6100 RX | 150 s | SSTV at 0.1 W: Reply and Replay to a typed target as received (Robot 8), TX MODE in Robot 36 (halted at 11 s) |
| 479 | `sstv-restart x6100>x6200` | X6100 TX, X6200 RX | 120 s | SSTV at 0.1 W: a CQ halted and sent again; Auto RX Restart ON saves the second, OFF the first |
| 480 | `sstv-restart x6200>x6100` | X6200 TX, X6100 RX | 120 s | SSTV at 0.1 W: a CQ halted and sent again; Auto RX Restart ON saves the second, OFF the first |
| 481 | `ft8-show x6100>x6200` | X6100 TX, X6200 RX | 150 s | FT8 at 0.1 W: Show CQ/All, the POTA tag, the own row, the Live Map heard and emptied by a band change |
| 482 | `ft8-show x6200>x6100` | X6200 TX, X6100 RX | 150 s | FT8 at 0.1 W: Show CQ/All, the POTA tag, the own row, the Live Map heard and emptied by a band change |
| 483 | `rx-filter x6100>x6200` | X6100 TX, X6200 RX | 110 s | the listener's SSB filter measured: a pilot and a swept tone through 100-3000, 500-2300 and 1250-1750; flat inside, 20 dB down 300 Hz out, the -6 dB points at the edges set |
| 484 | `rx-filter x6200>x6100` | X6200 TX, X6100 RX | 110 s | the listener's SSB filter measured: a pilot and a swept tone through 100-3000, 500-2300 and 1250-1750; flat inside, 20 dB down 300 Hz out, the -6 dB points at the edges set |
| 485 | `rit-xit x6100>x6200` | X6100 TX, X6200 RX | 120 s | the listener's RIT +-200 Hz moves the tone it hears 200 Hz down/up; the sender's XIT +-200 Hz moves it up/down |
| 486 | `rit-xit x6200>x6100` | X6200 TX, X6100 RX | 120 s | the listener's RIT +-200 Hz moves the tone it hears 200 Hz down/up; the sender's XIT +-200 Hz moves it up/down |
| 487 | `rx-pre-att x6100>x6200` | X6100 TX, X6200 RX | 70 s | PRE and ATT move the listener's spectrum the right way; its S-meter reads the signal alike through both (their figures) |
| 488 | `rx-pre-att x6200>x6100` | X6200 TX, X6100 RX | 70 s | PRE and ATT move the listener's spectrum the right way; its S-meter reads the signal alike through both (their figures) |
| 489 | `rx-agc x6100>x6200` | X6100 TX, X6200 RX | 70 s | the listener's ATT put in mid-over: the step levelled by AGC Slow and Fast, Fast settling sooner |
| 490 | `rx-agc x6200>x6100` | X6200 TX, X6100 RX | 70 s | the listener's ATT put in mid-over: the step levelled by AGC Slow and Fast, Fast settling sooner |
| 491 | `rx-dnf x6100>x6200` | X6100 TX, X6200 RX | 70 s | DNF centred where the tone lands notches it 20 dB against one beside it; the auto notch where the base has one, refused where not |
| 492 | `rx-dnf x6200>x6100` | X6200 TX, X6100 RX | 70 s | DNF centred where the tone lands notches it 20 dB against one beside it; the auto notch where the base has one, refused where not |
| 493 | `rx-nr x6100>x6200` | X6100 TX, X6200 RX | 45 s | NR raises a tone over the noise sent with it by 3 dB at least |
| 494 | `rx-nr x6200>x6100` | X6200 TX, X6100 RX | 45 s | NR raises a tone over the noise sent with it by 3 dB at least |
| 495 | `rx-smeter x6100>x6200` | X6100 TX, X6200 RX | 40 s | the S-meter follows 10 dB steps of the signal as the spectrum does |
| 496 | `rx-smeter x6200>x6100` | X6200 TX, X6100 RX | 40 s | the S-meter follows 10 dB steps of the signal as the spectrum does |
