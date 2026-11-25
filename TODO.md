# To do

## The owner's backlog (from the conversations, audited 2026-11-16, 2026-11-21 and 2026-11-24)

What the owner asked for in the working sessions, checked against git; GitHub issues are the sections below. The
radios transmit only into the bench cable (80 dB of pads, the owner's word on it), at 1 W or less. On 2026-11-17 the
side sessions (the Meteor/SCAN CFG one, the Kraken DF one and its kernel one) were folded into one backlog, in the
order the owner approved that day.

**2026-11-21 to 11-24: the owner's asks, audited 2026-11-24 07:45 UTC** (172 messages, direct and queued, since the
11-21 audit, checked against git's 177 commits and the orchestrator's results; the older blocks below are kept as
they were, each item's state brought here when it changed).

Open, mine to do:
- **Every test through the orchestrator** (the owner, 11-24: "all tests should ultimately use the same orchestration
  framework", "whittle down the tests to essential testable things, not slop"; and "ensure that your changes have
  tests, that in turn get run through http://127.0.0.1:8765"): tools/bench/orch.py has 152 tests (2026-11-24 15:40:
  all pass, none KNOWN; documented in tools/bench/ORCH.md); the 1:1 suite's batches (suite.py, suite_more.py, suite_cover*.py, air_*.py; 1386
  features listed, ~1040 not covered on 11-23) still run outside it. Left: pick the essential ones of features.json
  and move them in (surveyed 11-24: receive settings beyond rx-settings (VFO, split, RIT, XIT, filters, zoom, IF
  shift), the band plan (mode from plan, lock per band, transverters, TX shift), the safety refusals not in orch
  (TCI out of band, TCI CW, A/B arming), TCI and rigctl answers, the web pages and guards, the system checks).
- **The X6200's dsd-fme in the image**: the bundle's copy needed librtlsdr.so.0, which the X6200's card lacks: B had
  no P25, D-STAR, YSF or NXDN there from 11-19 to 11-24. The package builds it without RTL-SDR now; the X6200 runs the
  Mac build (hot install) till the next image and bundle (a tag, the owner's word).
- The X6200 taking no LSB-D (mode 1): NOT REPRODUCED 2026-11-24 (the later caps runs read it on both; both took it
  from LSB on 7.074). Closed.
- **A TCI over unkeys before its audio is out**: the sender drops what is still in its transmit path (0.3-0.5 s on
  the X6100, 1.2-1.4 s on the X6200): the step tests' last step and SSTV's last rows were lost until the bench padded
  0.8 s of silence (STEP_TAIL_S). Holding the key until the path is empty would fix it for every TCI program.
- **The web server's fix in an image**: the HTTPS key and the remote's sessions were downloadable without a login
  (3906e452, 7b01807b); both radios run the fixed server by hot install only. The next tag carries it (the owner's
  word to tag).
- **The upstream merge's look on the radios** (11-23): CHECKED 2026-11-24 (receive only): WeFax on both alike in the
  Sentry's amber, its All Charts tiles and page 4's "Night Auto (on)" at night; the WiFi held through both GUIs'
  restarts; Speech off by default (no voice_mode stored on either). Left: the classic screen's 3D waterfall on
  upstream's thread; the USER page has no labels for upstream's 200+ apps.
- Both radios' CW decoders lose the dits of the first characters ('VVV' prints 'TTT'): a decoder that locks sooner
  would copy short overs whole (noted; the tests send two V groups first).

Done 2026-11-25 night (the same asks, continued):
- air_b's WeFax, FT8 and JS8 groups pass both ways with no sync gap: wefax-cont, wefax-auto, ft8-multistream, ft8-qso,
  ft8-hold, ft8-late, js8; olivia passes again. Fixed on the way: the radios set up one at a time sat on two dials
  (both_at(): JS8, FT8, air_c's proof); WeFax's sender left in USB-D; the Olivia app now says its variant (the remote's
  `av`) and the test reads every Mode step back; a WiFi stall of a listener that cannot key no longer stops an over
  (bench.stale_limit).
- A real fault ft8-hold found, fixed in the GUI (both radios by hot install, in the next tag): the X6100's FT8 app
  listed the X6200's overs 156 Hz off and dropped the real decode (src/ft8/worker.c kept ft8_lib's candidate delete,
  which put the list out of its order); a bench recording in radio_captures/bench is its regression test
  (tools/ft8_sync decoder_test).
- New orch tests: spurs x6200/x6100 (what comes with a clean tone), ft8-copies x6200/x6100 and x6200 1w (an FT8 app's
  overs, the power 156 Hz either side). comp reshaped: loud and soft every 0.15 s, judged in the audio (to run).
- To look at: the X6200 at 0.1 W transmits with a noise skirt (-25 dBc per 55 Hz out to 170 Hz; -44 at 1 W): try a
  higher audio level into its base for the apps at low power (its TCI tone at 0.9 of full scale was clean). The X6100's
  tone in the X6200's audio has a component 61.4 Hz either side at -30 dBc (whose: its TX or the X6200's RX).
- air_c's seven groups and the paths (bands, walkie, if-shift) pass both ways with no sync gap; fixed on the way: a
  TCI program that keyed and quit at once keyed the X6100 for an instant (src/tci_server.c: refused when its close
  came with its key); tci-level's X6100 meter floor at 0.1 W (judged by the listener); if-shift's IF shift declared;
  an over a link stall stopped runs once more (bench.once_more_after_stall).
- comp: through TCI it cannot see Comp (the X6100's factory base levels even 0.15 s steps; the X6200's Comp leaves
  TCI's modem audio alone). NEXT: judge Comp through the mic, the other radio's speaker into it (acoustic.py's path).
- cw-copy X6100 -> X6200 taken apart (the owner chose it first, 11-25 night): the GUI's keyer lost the rest of a sleep a
  signal cut short (fixed: deadlines, tools/beacon's test); the X6100's factory base moves key edges 30-50 ms now and
  then and at times runs two elements together (said, not judged; cw-timing x6100 / x6200 / x6100 1w measure it, the
  keyer's own log tells whose a fault is). The X6200 keys within 7 ms.
- The X6200's 0.1 W skirt is the apps' way out, not its base: the same FT8 through TCI is clean, the app's overs carry
  10-13 % AM noise and 5-7 Hz FM jitter (orch: ft8-copies x6200 tci / tci app, skirt x6200 levels). Ruled out, each
  measured: the waveform, the level, the play stream (a 300 ms TX stream of its own: no change, reverted), the app's
  load, the VFO move. NEXT: diff what tx_player does around the over against TCI's tx_hold on the X6200 (the meters'
  reads, the codec's level writes, the keepalive's register block with the moved VFO), one at a time.
- comp through the mic (orch comp-mic x6100): the X6100's factory base does not compress at 4:1 (MANUAL). The X6200's
  capture holds no monitor of its over: its Comp needs a third sound source (this computer's speaker by its mic, with
  the X6100 receiving) or a third receiver. power-loop's X6200 figure: measured (Xiegu's loop, said).
- CW decoder: a text after a quiet copies whole now (the lock takes back the station it held; tools/cw_replay's check).
  Left: the X6200's decoder keys on its noisier audio in a quiet and its speed drifts (the first letters of the next
  text read slow); its first dits from a cold start (the tone finder locks during them: keep the last second and run
  it again once locked).
- Left: the apps' TX stream on the X6200 (the skirt); the X6200's Comp (a third source); the X6100's +61.4 Hz line (a
  third receiver).

Done 2026-11-24 evening (the owner: "get the over the air test coverage hammered out in full", "EVERY test should use
the same unified framework to sync the radios or make exceptions for tests so that there is NO deviation"):
- The one sync framework (bench.sync_pair, bench.apart, transitions; tools/bench/ORCH.md) in every test, and the 11
  air_a groups that failed under it all pass: finish() is one transition and ends a pair with the idle radio following;
  open_page_both() (G tunes each radio to its own channel); app commands open the follow window (bench.DIAL_CMDS);
  text-log's app peek; split's zoom after its mode change; qsy-out's listener zoom; orch's guard says the 20 s before
  a violation.
- Real faults the air groups found, fixed in the GUI (on both radios by hot install, in the next tag): the X6200's
  AM/SAM/WFM S-meter read a carrier up to 15 dB over its USB reading (it learned its offset in the noise, on the air,
  from a signal's rise, and summed the spectrum as stretched to the zoom); TX Cal's S-meter Measure from the remote
  ("smc", the smeter-front-end test runs each radio's own Measure: neither radio had ever been measured, PRE read
  3-4 dB low and RF gain 40 4-6 dB high on the defaults). The bench's TCI CW keyer left at the first break-in drop.
- Then the rest of air_a: tune, tune-power, tune-key, tune-app, cw-alc and ft8-tx pass both ways; power-loop measures
  both bases' loops (Xiegu's, said); fixed on the way: the Watch stopped an over on one bad 18.5 W meter frame (now
  two in a row), the follow window closed before an app had tuned, a child bench program's set-ups were invisible to
  the guard (bench.SYNC_DIR), tune-app judged by the listener's audio (the references cancel).
- Left in air_a: comp (both radios' loops level 2 s stretches: reshape with shorter ones, judged in the audio);
  power-loop's X6200 figure (the X6100 listener's audio clips at 0.1 W).
- air_b CW and text (cw-copy, cw-carrier, cw-kbd, rtty, psk, jtty, olivia): every row copies both ways; fixed on the
  way: apps closed mid-test (close_apps keeps the app's dial), the teardown one transition, lock_again's G, listen_at
  (both radios with the listener), the CW filter the tests need (800 Hz: a probe had left 400-3000 and the X6200's
  decoder printed the noise), the CW Keyboard from page A, cw-carrier judged on its steady middle (and its dip
  measured), cw-copy at 20 WPM (a dot lost at 25 X6100 -> X6200), cw-vfo-line to the X6200 picture's 750 Hz bins.
- The console showed a finished run ("no test running") while the next keyed (the owner): fixed 40982314.
- Left: air_b WeFax, FT8, JS8 (7 groups), air_c (7), air paths bands/walkie/if-shift; comp; power-loop's X6200 figure.

Done 2026-11-24 afternoon (the owner: "I just connected a DVMega to the 6200 and a p25 USB3000 to the other... do some
more AMBE tests", "The 6 m output floor... maybe time to revisit now? same with the image shifts?"):
- AMBE: the DVMega stick (`AMBE3000F`, X6200) and the USB-3000 (`AMBE3000P25F`, X6100) decode DMR and D-STAR alike (the
  speech r 0.97/0.95 through each, and through the other's frames). Fixed: the stick sends no parity field until
  asked and three parsers cut frames ending in 0x2F (ambe.c, libmbe-dvsi); P25 full rate only on a chip with the P25
  option (the stick decoded it to 782 Hz); libmbe-dvsi's decoder reset before its rate; and B's D-STAR dropouts and
  the X6100's P25 400 Hz (the owner: "fix that"): the NXDN dsd-fme's false frames set the shared chip's rate between
  the P25/D-STAR one's; the daemon's port now sets each client's own rate before its frame. Tests in orch, all pass:
  ambe-vocoder, dv-dmr, dv-dmr-noparity, dv-dstar, dv-ysf, dv-nxdn, dv-p25.
- Ported from the 1:1 suite into orch (all pass both radios): rx-vfo-split, tci-receiver, rigctl-more, remote-apps,
  power-cap, web-pages (77 routes alike), and TCI's refusals on each radio (tci-out-of-band, tci-ask, tci-cw-usb,
  tci-key-cw); system-checks (the system batch); and 35 of suite_proto's checks run as they are through an adapter
  (suite_checks/suite_pair: TCI's answers, CW speed, sensors, audio, drive, four clients, origins; the panel; control;
  the screen; the band keys, broadcast AM, knobs, wide zoom; the web's guards, HTTPS, WSS, SSH keys, backup, text log).
  Then suite_cover_a's 20 (Bluetooth, backlight, screen off, screensaver with orch's sessions closed for it, themes,
  zoom/step, band plan, own band, mode from plan, lock per band, channel snap, speaker, line in, transverters, TX
  shift, TX outside): all pass. Left of the old batches: cover_b/c/d's app and system checks, air_a/b/c's
  over-the-air ones orch does not have yet, panel-log, if-shift, tz-alike.
- Deep backlog (the owner, 2026-11-24: "those feel like things to put deep in the backlog"): nulling the X6100's
  carrier leak with a calibration that keys it and writes its per-band DC offsets (txcal.py); whether the patched
  gcfix5 base goes back on the X6100 (it lowers the 6 m floor); the screensaver's rain drawn by the GPU (the owner:
  "put GPU screen saver deep in backlog"): it is LVGL's software blend cell by cell, 15 frames a second
  (src/screen_saver.c), 15-16 % of a core against the lit page A's 10 %; the Mali400 path is in src/gpu.c.
- 6 m: power-6m (USB, a TCI tone at 0.1/0.3/1.0 W) passes both ways: the X6100 heard +6.0/+5.0 dB apart, its meter
  0.1/0.3/1.1 W, so its 6 m floor is gone wherever the GUI sets the drive (TCI, the digital apps). power-6m-fm (the
  base's own carrier): the X6200 follows; the factory X6100 stays at about 1 W at any setting under 1 W (its base's
  ALC-only control: the GUI now says so, below; the base fix gcfix5 waits on the owner's go).
- The image shift (X6100 15/12 m): measured in orch with the listener 1500 Hz below, declared to the sync guard and
  the Watch (the owner: apart only when a test does it "INTENTIONALLY"): image x6100/x6200 pass, the X6100's 15 m
  -43 dBc in two runs (no shift today). Its carrier leak at 0.1 W, -12.5 to -27 dBc (no DC offsets on the factory
  base), is said with each run: nulling it (txcal.py keys it and writes the offsets) is the owner's call.
- The X6100's 6 m AM/FM floor (about 1 W at any setting under 1 W, its factory base): worked around (the owner: no
  known flaws without one): the GUI says so at a keying there and logs it; power-6m-fm/-am check the floor and the
  note. In USB through TCI and the apps the power follows the setting (power-6m).
- The console (http://127.0.0.1:8765) lists the earlier results the latest first (the owner).
- Closed by the owner: new HTTPS keys on the radios ("nope, we don't care").

Open, the owner's call or hands:
- **CW from the X6100 misread by the X6200's decoder** (orch cw-text x6100>x6200): FIXED 2026-11-24, no longer a
  known fault (5 of 5). Two causes: the word spaces (the decoder keyed on the X6100's carrier, 62 dB under its marks,
  as the X6200's AGC lifted it through each word gap; a key down must now be a rise, key_detector.cpp), and the first
  dits read as dahs (the bench's caps test had left both radios' CW filter 6 kHz wide, and the band's noise set the
  decoder's speed; filters back to 800 Hz, the caps test puts them back now). AM and NFM were left at 6 kHz too
  (kept: plausible as they were).
- The squelch mutes the received audio (speaker, decoders, TCI), not the spectrum: ANSWERED 2026-11-24 (the owner:
  "I don't want wrong squelch to prevent reception"): the tests hold it open (0).
- The X6100's 15 m/12 m image shifts (below, "15 m and 12 m"): left to try the battery alone, 15 m alone, the case
  temperature. The 10 W figures want a 5 A supply (the 3 A one current-limits).
- The X6100's 20 m "rake": most likely its stock charger; to confirm with the charger back on for a minute.
- G's voiceprint of the microphone while keyed: DONE 2026-11-24 (orch g-voiceprint x6100>x6200, the owner: "with the
  output from the other radio stepping tones"): the X6200's speaker played 2000-500 Hz steps, the X6100 keyed on G at
  0.1 W; its voiceprint drew all six in order (two pictures, read by their lines), its capture held them, the X6200
  heard the over. The X6200 as the mic radio: its G does not raise its monitor (issue #15), so not tested that way.
- The X6200 heard nothing of the X6100 at 7.0-7.6 V of battery: NOT REPRODUCED 2026-11-24 at 7.5 V, 9 % (all the
  steps heard). It switched itself off sending 0.2 W at 7.1 V on its stock charger.
- The factory X6100: 6 m puts out 0.8-1.2 W at 0.1 W (its base's floor, not the GUI); Comp; keyboard/mouse, the
  sideswiper and KEY Train with a paddle untried on it (the 6 m floor: ignored for now, the owner 2026-11-24).
- Later (the owner, 2026-11-24): the image shift, the 5 A supply, the rake, the Kraken. From before: the USB-3000's
  voice on B with a powered hub and an SDR; screenshots of every menu (deferred).

Done since 11-21 (details in git and tools/bench/ORCH_FINDINGS.md):
- **The X6200's TCI power below 1 W** (orch power-steps x6200>x6100): its meter reads high for a second or more after
  each key, and the hold cut the drive on that and kept the cut. Fixed 38430917 (under 1 W the level for 1 W goes out
  scaled, cut only by a clear over); PASSED 2026-11-24 07:36: the X6100 heard 0.2/0.5/1.0 W at -59/-55/-52 dB.
- TUNE for an external tuner (no tuner on the bench; the owner: "do that"): orch ext-tune from the FT8 app's Ext tune
  PASSED both ways 2026-11-24 (x6200>x6100 07:38: the tone at 1507 Hz for 1500, off 1.08 s after the second press, the
  app and dial kept, off by its 15 s limit after 15.5 s; x6100>x6200 1492.6 Hz, 0.87 s, 15.3 s). The remote's TUNE
  passes both ways (tune-carrier).
- The test rig, as the owner asked it (11-22 to 11-24): bench.py's state layer and page A at the end (cad0140f); the
  1:1 suite (7d84b748) and its batches; then orch.py (9816911d on): both radios set together and compared, a sync
  guard, never on from radios set wrong, each step confirmed, a live console over HTTP (the HackRF as a card beside
  the radios, pictures only on this run's fails), results kept so passed tests are not rerun, the caps map of where
  the radios differ, volume 0 and squelch held, stepped tones judged by each radio's spectrum and its audio, data
  sent against received (CW, FT8, JS8, RTTY, PSK31, Olivia, the CW Keyboard, WeFax, SSTV), the radios left on their
  last frequency on page A, the operator's control never taken.
- Fixed with it: the JS8/FT8/DMR use-after-free behind both GUIs' crashes (7da8c640, ASan; on both radios); the HTTPS
  key and sessions served (above); a remote without pictures costing the GUI a third of a core (085ce368); the
  Panel's CW filter; the Panel placed where the base is tuned (493fbde9); TCI answering a refused tune; D/E's ATT off
  taking the X6100's RF gain to 100 like the X6200 (2fc81863); G's own channel and CH LIST on F4, 3D only (3b23430a);
  the CW decoder's tone search (6cd00cb7); 1bit's pictures (5d1afc6d).
- WiFi: the X6200's own 2457.6/2470.6 MHz lines notched in rtw88 (bcbf1dcd; channel 11 33 % -> 99-100 %; channel 13
  too), sent upstream (#327); the receive audio's 2 s repeat (sun6i-dma, d545dd48). WSJT-X over TCI receives and
  transmits (Q-prefixed calls, 413debdf); the X6100's TCI transmit through the drive loop (67a1d5d5). RTL sticks
  take the gain they are opened with (6b1336e9, f6f54c04). TRIBAL KNOWLEDGE on F (9c465116). The X6100's 780.69 Hz
  lines and the weak adapter in the manual (c0f615ad, cba95266). G's spectrum keeps time on the air (14acbc53); G
  arms in the band's voice mode (42279ea2). The update's restart ticked by default (d4a244bb). The power watch logs
  over-power instead of stopping (23faf3c2). Upstream's 229 commits merged, nothing of ours replaced (73ac370c).
- Closed by the owner: BinModem (removed), the ardopcf and axspawn reports (not sent upstream), TX CAL on F SYSTEM,
  the gcfix5 base (the X6100 back on Xiegu's factory V1.1.9), the draft PR (deleted), Bluetooth headphone code (never).

**2026-11-23 night: the 1:1 suite's "cover" batch (tools/bench/suite_cover.py)**, 57 pass 0 fail on both radios (with FT8's MODE through its three protocols and the knob's TX Hz, channels from the web (60 MHz on the X6200 only), SmartCW's page guard, Pat's mailbox; a first FT8 run stopped after one MODE step left both radios' FT8 on another protocol: put back by hand (X6100 FT4, X6200 FT8, params read back) and the test now always steps back; TCI listen only and off with rigctl following, put back as found; TCI's Bluetooth service;
web-writes: a PSK macro and a test SSH key added and taken out, each file compared with its copy after; the own-name
guard: a change and the SSH keys' list refused under another name, plain reads answered by design).
Found and FIXED with it: a remote connection that wanted no screen pictures (the Panel, the bench) had the GUI thread
copy the screen and the waterfall plane for the picture encoder all the same: the X6200 on A 9 % of a core alone,
33 % with one Panel (the X6100 13 -> 35 %); now only while a client wants pictures (remote_server.c screen_want),
9.9 and 13.7 % with a Panel; the Screen page's pictures still cost ~34 %. The Panel's CW filter: two edges below
the key tone (0-500 at 700 Hz) gave a 1 Hz filter (each edge's setter took it as the width about the tone): now the
width whole (450-950), as the manual said. Not the merge: the pre-merge build measured the same 9 % alone.
Also from the merge's CI: upstream's matcher of a failed modem switch looked for "OpenX6X00:" in Pat's log while
ours writes "GoldCorpHF:" (switch failures went unseen): matched to ours, with upstream's tests; the DMR listening
log's heading GoldCorpHF; the FT8 CB and contest tests given each other's new sources; front_keys' stand-in our
"paged".

**2026-11-23: upstream merged (Guru-RF 51bf5bf3..6a5d262e, 229 commits: v0.3.13, Speech, APP > DMR over the internet,
RTTY sending, APP > Mail, Mercury 1.9.25, XARA from NR5P, FT8 Traffic/Tiles, JS8 board, WeFax tiles/Auto Plan/Night,
GPS / Clock, the 3D waterfall's thread, WF full screen, weather units, tuning steps, knob press, hand mic UP/DOWN)**.
The owner: "just NEVER EVER EVER stomp on something we are doing. you can add funcitonality and fix bugs. That's it".
- Kept ours: README (upstream's in README_original.md), the updater and its tests and web page (upstream's multiboot
  report in them left out; S01multiboot and the GUI's card warning taken), the key action numbers (WF_FULL 32, MAIL 135;
  VOICE_MODE reuses slot 6), FT8's clock chip write through x6100-rtc (upstream moved Time Sync to clock_sync.c), the
  band keys in the Sentry and on page I (upstream's front_keys.c repeats BAND instead of a long press: a held key now
  does nothing there, as before), our rtw88 0004 notch and ft8lib 0001 (upstream's copies of the same dropped).
- No mbelib (the owner: "hard no on the mbelib, USB-3000 is hard requirement"): upstream's DMR app gets its "mbe" from
  tools/dsdfme/mbe_dvsi, voice through the USB-3000 only; third-party/mbelib and its test removed.
- Added to upstream's work: its WeFax tiles, Fix view and map drawn in the theme's colours (wefax_screen_grey, as our
  live view); its new key actions in our key lists; its DMR helper named GoldCorpHF outside; ours "wide 1" frames moved
  to 0xFFFC (upstream's WeFax rows took 0xFFFE); d3_colmajor's extract stops before our GPU 3D code.
- Checked: every line ours added since the base is there or deliberately combined (merge_check.py), both GUIs build
  (no libssp, no mbelib symbols), hot-installed on both radios; Mac tests pass but tools/ft8_qso (it now links libft8
  from build-host/deps, CI only).
- To look at on the radios: the classic screen's 3D waterfall (upstream's drawing thread now runs on the X6100/X6200),
  WeFax's tiles and Night in amber, the WiFi device held over GUI restarts (upstream's device_take), Speech off by
  default. Pre-existing, not from the merge: Pat's log lines say "OpenX6X00:" while the manual says "GoldCorpHF:"; the
  USER page has no labels for upstream's 200+ apps.

**2026-11-22 afternoon: the test harness and the 1:1 suite** (the owner: "audit deeply the test harness and ensure this
stops happening"; "a comprehensive set of unit tests to cover EVERYTHING 1:1 on both radios"):
- DONE cad0140f, d5803f2e: bench.py's state layer (each radio said whole and read back, the path proved, unheard overs
  failed, finish to page A), the remote's new state, sim_test 34 cases in CI; acoustic.py (the mic on G both ways,
  PASS); the bench player ("play").
- SUPERSEDED 2026-11-24 by orch.py (the owner: one orchestration framework, essential tests only; open item at the
  top): features.json (1386 features from the manual and the code) and suite_more.py's batches rx, control,
  safety, paths: one test per feature on each radio and 1:1, coverage.md with each run (what is not covered yet).
- SETTLED 2026-11-23 (a limit, documented in the manual's wide zooms): the X6200's remote spectrum cannot resolve a
  tone inside the passband (one clean 1000 Hz TCI tone filled 0-2500 Hz alike at zoom 1 or 8; the X6100's peaks).
  The Sentry holds the X6200's base at 384 kHz while it is open (radio_psd_wide: A's trace and scans, D's counts, G's
  own picture all read it), 512 points 750 Hz apart, and every narrower view stretches the middle of those. Measured
  receive only (scratchpad panel_res): the Panel's frames at zoom 1 and 8 are straight lines between points 750 Hz
  apart (fit error 0.29-0.32 dB, the 1 dB rounding; 0.66 at 1500 Hz). The base sends one stream at one span, so a
  finer Panel would cost A and G theirs; alternating the base's span per packet was not tried (its settling after
  a change is unknown, a misfiled packet would give A false targets). The bench reads tones from TCI audio there.
- DONE 2fc81863 (the owner: "they should handle uniformly in the same fashion the 6200 does"): on the X6100 a D/E choice
  with ATT off takes RF gain to 100 (its attenuator); proven from RF gain 80 on 24 D bands and 15 E types.
- DONE 2fc81863: x6100_daemon kept 50 run logs only at boot; a crash loop on 11-21 left 54 on each radio. Pruned after
  every run now (the radios' copies take it at their next boot).
- suite_proto.py (control: TCI protocol, the remote's protocol, control, screen, web guards, Funnel; system: SSH reads):
  found the bench player taking a missing file in silence (fixed), the manual blurring rx_smeter (always) with
  rx_channel_sensors (after rx_sensors_enable).
- FIXED, found by the suite (2026-11-22 evening): the Files page served the HTTPS key (/mnt/https/key.pem) on both
  radios (3906e452; the radios keep serving it until the next image: the web server is bytecode there); a TCI vfo or
  modulation the radio refused (LOCK, an app's hold) went unanswered; the X6200's Panel drew its 192/384 kHz zooms
  over a 100 kHz scale (the copy for the browser labelled wf_span(): 2-4x off, a tap tuning beside the signal).
  CLOSED 2026-11-24 (the owner: "nope, we don't care"): the radios' HTTPS keys are kept, not made anew.
- DONE (the owner: "Yes, log Q9TST"): the Log tab's tests on both radios 1:1: log in USB (SSB 57/58, name and grid),
  logq sent, repeated (written once), asked, a conflict, an unknown number. WSJT-X UDP (enabled=1 on both, multicast
  and the gateway) switched off for the test and put back byte for byte, so no Q9TST reached a PC logger; the day's
  log removed after (it was not there before).
- CHECKED (the owner: "selecting things in D or E should automatically properly set the permutation for ATT, PRE and
  AGC"): D's LISTEN on 15 bands and E's OPEN on all 17 types set ATT, PRE and AGC as rx_setup says, on both radios,
  read back 0.3-4 s after (they stay). The ATT seen on was left by the bench's 10 W probe (image.py --rx-att 1), not
  D or E.
- The X6200 heard nothing of the X6100 with its battery at 7.0-7.6 V and everything on a full one (both pads fine):
  to prove at its next low battery (bench.listener_gate refuses a listener under 25 % meanwhile).

**2026-11-20: the owner's asks, audited 2026-11-21 02:10 UTC** (82 messages of 2026-11-20/21, direct and queued, checked
against git: 32 commits that day, v0.3.12.37-.40 released). Open first; then what changed state; the done ones last.

Open:
- 1:1 AUDIT of the two radios (the owner, 2026-11-22 ~03:00: "do a 1:1 audit of both radio features ... clean up the
  backlog"). A feature inventory from the manual and the code (the caps each base gives: the X6100 on Xiegu's factory
  V1.1.9 rev 0 has TX_IQ_OFFSET, ZOOM, IF_SHIFT, TX_SPECTRUM_ON_DIAL only; the X6200 ZOOM, DNF_AUTO, PSD, COMP,
  TX_LEVEL, SAM_WFM, KEEPALIVE, TX_NO_ALC, CW_SPECTRUM_ON_DIAL, CW_TONE_RISES), then suite.py core on both radios
  (every screen side by side). Gaps and their state:
  - FIXED b010fc02: page I / the web DF page said X6200 on the X6100; the LCD theme's RF bars 0-10 W on the X6200
    (8 W top); the Bluetooth CAT name X6100 on the X6200 (now the model, live on the X6200); manual: Mesa, the boot
    video's engine and the clock chip's drift on the X6100 too (checked on it), the TX AUDIO / TX CALIBRATION cards'
    models, CW peak on the X6200 acts on the decoder only.
  - FIXED (this night, after b010fc02): G's spectrum on the X6100 50 kHz wide (its IQ climbs from ~27 kHz out; A
    shows the middle 50 kHz; G's 100 kHz drew the humps as edge signals); its caption says so (50 KHZ on the X6100,
    100 KHZ on the X6200, shot on both).
  - suite.py core on both radios (2026-11-22 03:35, then the screens again): 55 pass + 37 screens pass; every Sentry
    page and app screen alike on both, no old-theme colour; RADE shut on both (needs headphones or a TCI client, as
    it should); SSTV on the X6100 opened after the 15 s wait (its picture showed it open): the wait is 25 s now.
    Settings that differ and are each radio's own: adsb_range 3/8, line_in 10/default, sql 4/0. Identity: the X6100
    bundle .43 (image .28), the X6200 bundle .42 (image 0.3.8), both GUIs the hot build.
  - CHECKED, nothing to change: the CW key's release when the GUI stops (gpiochip1 line 203 is the key on both:
    X6100_morse_key / X6200_morse_key); the X6100's base version reads V1.1.9 (G's panel); the settings the GUI sends
    whatever the base (codec/band gain, CESSB, TX filter, FM emphasis, CW peak, DNF auto) are all at their defaults
    on the X6100 (none saved), as upstream sends them to factory bases.
  - 6 m at 0.1 W (the 11-19 walkie finding), again 2026-11-22 04:00 on the 14 V supply (radios-walkie 6m,6m-fm, the
    X6100 sending): 1.0-1.2 W in AM and FM at a 0.1 W setting, logged, not stopped. Xiegu's factory base's floor on
    6 m (tools/base_update/README.md: "Xiegu's: 0.8 W on 6 m"; gcfix4/5 gave 0.1 W): not the GUI. The X6200 heard
    those overs only 7 dB over its spectrum's quiet (its own front end: ATT on, RF gain 39, both saved settings of
    its, from before tonight). The walkie leaves both radios on the first one's channel (the owner's 11-19 wish): the
    X6200 is on 21.300 MHz USB since, not its 14.204 USB-D.
  - DONE 2026-11-23: IF shift on the factory X6100 (it was claimed from the version alone). ifshift.py (the paths
    batch): a 0.1 W tone from the X6200, the X6100 at +2000/-2000: its pitch kept within 2 Hz, the base 2.1 kHz the
    other way, as the manual says. Found with it and FIXED (dsp.cpp frame_out): the browser Panel placed a base
    without a frequency (Xiegu's X6100 base, the X6200's spectrum) on the bare dial, not where the base is tuned as
    the waterfall and the Sentry do (the IF shift, CW's key tone): the tone drawn 2.1 kHz off at a 2000 Hz shift, now
    within a bin. The remote takes `set ifs` (refused on the X6200: no IF shift there, the dial alone would move) and
    `set comp`; the state says both, the bench puts both back.
  - LEFT (need the radio in a way not tried tonight): Comp on the factory X6100 (the button shown and sent, the cap
    says no compressor: what it does on the air; needs the mic path, acoustic.py's, as TCI audio has its own
    levelling on the X6100's base); the
    untested-on-one-model list in the manual (keyboard/mouse and the sideswiper on the X6100, KEY Train with a paddle,
    the X6200's TUNE for an external tuner, KrakenSDR on the X6100, Time Signal's AM filter on the X6100).
- G (the owner, 2026-11-22 ~03:00: "YOU should be setting the radio into the mode im only seeing spikes on it form the
  key up and key down"): DONE that night. (1) A/B arming on A and G puts CW/CW-R and LSB-D/USB-D into the band's voice
  mode (USB from 10 MHz and on 60 m, LSB below) and locking puts it back: scratchpad arm_mode_test.py 7 of 7 on both
  radios. The owner had keyed at 14.0694 MHz in CW (a carrier, 6 s and 3 s at 1.0 W) and at 14.204 MHz in USB-D (12 s
  at 0.0 W: the data mode takes the processor's audio). (2) The radio's own PTT over while B plays the stick on the
  speaker: the base out of play mode for the over (play mode has both mics at 0: silent). (3) G's voiceprint on the
  air: the X6100's base monitor raised to 30 for a PTT over on G (radio_set_tx_moni, as a screen recording's): the
  base puts the monitor on the processor's capture line (a 1000 Hz tone over TCI 21 dB over the line's floor at 30,
  none at 0; scratchpad moni_route.py), so the voiceprint draws what goes out. Not the X6200 (issue #15, its speaker
  rings into its built-in mic). Record mode (HW_RECORD) while keyed was tried first and DROPPED THE OVER within 1 s on
  the X6100's factory base (1 s at 0.6 W in the minute log instead of 6 s): not used. LEFT: the owner's voice on G to
  see it draw (the acoustic try failed: the X6200 played its RTL's FM station, the X6100's hand mic did not pick it
  up where it lies); whether the X6100's speaker playing the monitor at 30 bothers anyone.
- 2026-11-22 00:00 the owner: "on the X6200 I just connected 2 RTLSDR dongles, can you tell me which performs better?
  you can use the hackrf to test". BLOCKED on the hub: the X6200 sees only the Genesys 4-port hub (05e3:0610, the bad
  one); the first stick (RTL0, serial 00000001, R820T) came up twice on its port 2 and each time it streamed the
  controller logged "ep3 RX three-strikes error" and the whole hub dropped, then the kernel gave up on all four ports;
  the second stick never enumerated. Nothing transmitted, nothing changed. To do once they are on a powered hub or
  the HOST port one at a time: ppm against the HackRF, the same weak carrier's SNR and the noise floor at the same
  gain, spurs (2 m or 70 cm, or 433 ISM, -a 0, from -x 0).
  DONE the same night, one at a time on the HOST port (the fork's rtl_compare in the scratchpad): all three R820T,
  serial 00000001, no TCXO. Stick 1: -88 ppm, the best on 2 m (36 dB tone over floor) and NOAA (+25 dB), image
  rejection 35 dB, floor steady under a strong tone, 15 overload lines. Stick 2: -52 ppm, the best at 70 cm (34.5 dB
  SNR), 48 overload lines. Stick 3: -2 ppm (as good as a TCXO), the cleanest (5 overload lines, floor steady, no
  145.600 spur), 10 dB under stick 1 on 2 m and 20 on NOAA (antennas? over the air: swap them to tell). rtl_power's
  gain never took: FOUND the cause in librtlsdr (below) and FIXED it in our daemon the same night.
- RTL gain (2026-11-22, the owner: "Do this task here"): x6x00-rtlspec never took the gain a stick was opened with
  (B's -g at every start, replug or stick switch) until it was changed. rtl-sdr-blog's sample-rate step tunes to the
  stick's frequency, 0 after the open, so the R820T goes to direct sampling; the first real tune brings it back and
  initialises the tuner again, its gain to its own AGC, while librtlsdr still reports the gain set. Bench X6200
  (scratchpad rtl_gain/gaincheck.c, first.c, dgain.py): 0 dB asked before the first tune read -9.4 dBFS against
  -48.3 once set again; the old daemon's rows -43.4 dB at 0 dB asked, fixed -74.1 from the start. FIXED in
  tools/rtlspec/sdr_rtl.c (the last gain set again when a tune brings the tuner out of direct sampling); hot-installed
  on both radios (b725436b). rtl_power and any program that sets its gain before its first tune have the same trap;
  x6x00-df tunes first (right). DONE too 2026-11-22 ~04:00: buildroot rtlsdr-blog 0004 sets a manual gain again
  after the tuner's init in _rtlsdr_set_direct_sampling (rtlsdr_set_tuner_gain_mode keeps the mode): every program on
  the radio (rtl_power, rtl_fm, dump1090) keeps its gain; scratchpad rtl_gain/first.c against it on the X6200's stick:
  rate-gain-tune now -48.5 dBFS at 0 dB (before -9.4), 3 of 3 runs; the daemon built on it on both radios (94c761fe).
- 15 m and 12 m transmit image shifts during a session (-46..-53 dBc to -29 at the same balance, a 9 % I/Q gain
  change; 10, 17, 20 m steady). The owner: "dive deeper ... find a novel solution that *works*". The heat theory and
  its warm balance were wrong and REMOVED (eab501f7); the battery-vs-adapter theory is not supported by the minute
  log (shifted 49/77 in collapsed minutes, unshifted 20/32). NOT THE SUPPLY (2026-11-22 02:14-02:27, the owner's 14 V
  bench supply, its input 13.3-13.7 V and charging every minute, Xiegu's factory V1.1.9, no correction): the same
  recipe (image.py 17-15-12-10 m, 0.1 W, 900 Hz x3, six runs) shifted 15 m again, -43.4, -43.4, -30.4, -31.3, -33.3,
  -41.8 dBc (12 m -42.7..-40.4..-47.5; 17 m -46..-48, 10 m -42..-45 steady). So not the supply and not gcfix5: the
  X6100's transmitter on 15 m, coming and going within minutes. Left to try: the battery alone; the band order (15 m
  alone, no cycling); the radio's case temperature beside it. tools/base_update/README.md "Why 15 m varied".
- The 10 W table and the 5-10 W results were made with the X6100 partly on its own battery (the stock adapter is a
  charger): run again on a supply that holds before taking its power figures as the radio's. 2026-11-22 on the
  owner's 3 A bench supply (Velleman LABPS3003SMU at 14 V): 2.8 A drawn at 0.1 W while the battery charged; a 10 W
  over on 20 m put it in current limiting (its CC light, the owner), 5.55 and 6.7 W out, the X6200 hearing lines
  ~26 dB under the tone and its floor 17 dB higher than the 11-20 10 W run. Needs a 5 A supply (or the battery full,
  its charge current off the supply). Also seen there: the TCI drive (tx_hold, the level for 1 W scaled by
  10 log10 P) starts under Xiegu's knee at 10 W, ALC 0 (it climbed 3.6 dB in those two short overs, learned): the
  base's power is not the square of the drive up there. To do: a level kept per power (or a fitted slope) instead
  of one scaled by the power.
- X6200 TCI transmit power: DONE 2026-11-21 (src/tx_hold.c). TCI audio set the power, not the setting (-20 dBFS gave
  1.1 W at 0.1 W); now it goes through the apps' drive loop (tx_level.c, power_hold), the learned level kept as the
  peak for 1 W (params tci_tx_level) and scaled by PWR. Bench (tones.py x6200 x6100 --w 1, the tones at -10.5 dBFS,
  9.5 dB over the -20 that gave 1.1 W): at 0.1 W median 0.1 W, peak 0.2 W; at 1 W median 0.9 W, peak 1.0 W; the
  level -22.75 dBFS for 1 W after. tools/txlevel models it (10 TCI cases).
- WSJT-X over TCI TRANSMITS (2026-11-21 05:11-05:22, the X6200 into the X6100 through 80 dB): ten full overs in a row,
  0.1 W setting medians 0.1 W, 1 W setting medians 0.9-1.0 W, no Rig Control Error; the X6100's own FT8 app decoded
  "CQ N0CALL" at 0.1 W once, then nothing in the next run, attenuator on or off: the CQ reaches the air 1.5-2 s late
  (WSJT-X's audio over WiFi plus the X6200's 0.5 s), at the edge of FT8's DT. To do: measure its DT; consider asking
  WSJT-X for 24 kHz or int16 (JTDX's client reads float32 48 kHz only) or a smaller pre-roll. Fixed on the way
  (tci_server.c): the TX question kept when the client hangs up (its answer for its next connection); text ahead of
  the RX audio and the TCP send buffer capped (answers 0.1-2.3 s late, now 7-99 ms with audio streaming, and the
  stream at its full rate); silence counted from the key, the first audio given 2 s; no RX audio to the client
  while it transmits; its unkey answered at once; the remote state's "ask". The bench run is in the scratchpad
  (wsjtx_tx.py, enabletx.dylib presses Enable Tx, shmshim.dylib, the 127.0.0.1 relay), not in the repo.
- The X6100's TCI RX audio to a recorder on the Mac stopping after about a minute: DONE 2026-11-21. The server
  PINGs a client silent 20 s and drops one silent 60 s (tci_server.c IDLE_MS); tones.py's TciRx (and tci_tone_tx.py)
  never answered the PING. Both answer it now: 100 s from the X6100 at 99.8-100.1 % of real time.
- WSJT-X over TCI: RESOLVED 2026-11-21 evening (the X6200's WiFi notch, bcbf1dcd: TCI 100 %, 24eaeb61; WSJT-X then
  decoded live from the X6200's TCI once ft8lib 0002 sent WSJT-X's own call format, 413debdf). Was: PARKED 2026-11-21
  (its transmit side works, above). Why it never decoded from the X6200: it gets
  the X6200's audio at 31-40 % of real time after the first 15-20 s, so it never fills a 15 s period (its period
  counter in the shared memory reached 21 of 50). A plain reader on the Mac sees the same, so not WSJT-X: WSJT-X sets
  no format and takes TCI's default, 48 kHz float32 stereo = 3 Mbit/s, and the X6200's WiFi does not carry it (raw
  TCP from the radio 2.5 Mbit/s, then 0.8-1 Mbit/s after 15 s; its RTL8723DU at -68 dBm on another access point than
  the X6100, which gets 17-34 Mbit/s at -47 dBm). 24 kHz int16 mono (0.4 Mbit/s) holds 100 %. tools/bench/tci_rate.py
  now checks this over the network (X6100 wsjtx PASS 100 %, X6200 FAIL mean 31 %).
- RESOLVED the same day by the driver's notch for the X6200's own 2457.6/2470.6 MHz lines (bcbf1dcd, upstream
  Guru-RF/OpenX6X00#327): channel 11 from 33 % to 99-100 %, TCI at full rate. Was: the X6200's WiFi SENDS about
  1 Mbit/s (2026-11-21): raw TCP from it 0.5-2.5 Mbit/s, to it 9-13 Mbit/s; on 11-17 the
  same driver sent 8 Mbit/s to the same channel-6 access point at -57 dBm, now -66..-69 dBm (10 dB weaker: moved?).
  Not the CPU (90 % idle), not power saving (off, disable_lps_deep=Y), no "failed to get tx report", Bluetooth quiet
  1.3 against 1.0 Mbit/s; its rate falls to MCS 0 (6.5) or 5.5 Mbit/s while sending, retransmits near none. It stays
  on channel 6 by design (98a8bd8d keeps it off 9-11, where it stalled). Not moved (the owner: the radios sit side by
  side). Ruled out the same day: rate control (fixed MCS 4: nothing through; MCS 2 1.4, MCS 0 0.9 Mbit/s, where a
  sound MCS 0 link gives ~4), the A-MPDU cap our pin writes on the 8723D (0x458 = 0x7fff; 0xffff no better; mainline
  writes it for the 8814A only, 0d2a88690e58), mainline's TX-report erratum (5f936768300f: the MAC table overwrote
  0x421; ours reads 0x1f, intact), the antenna port (the efuse says AUX, 0x948 = 0x80; MAIN 4 dB worse), the
  transmit power (indexes 42-49, nothing clamped). Mainline rtw88 since the pin (111 commits, usb.c/tx.c/8723x
  diffed): leak fixes, broadcast queue routing, logging; nothing for 8723DU transmit. What is wrong is the link: the
  X6200 hears its access point at -64..-82 dBm with an SNR of 8-15 dB (its noise floor ~-84, 10 dB high; 366 OFDM
  false alarms in 2 s), the channel-11 AP 5-7 dB weaker than the X6100 beside it. Next: a 2.4 GHz sweep beside the
  X6200 (HackRF, receive only) with the radio on and off, and a USB WiFi adapter on a short lead away from it.
  LATER THE SAME DAY: mostly our own 98a8bd8d. A fresh driver joined the channel-11 AP at -47..-52 dBm: 17-35 Mbit/s up,
  WSJT-X's TCI stream 98 %; the rule then moved it within 4 s to the channel-6 AP at -76..-81 ("-80 or better"):
  back to 1 Mbit/s and 30 %. Its floor is -65 dBm now (test case 13). The driver (built as the image does by
  .github/workflows/rtw88.yml, byte-identical to the image's, swapped in over SSH with a fallback): no Bluetooth TDMA
  slot without a Bluetooth link (every uplink burst began at a third of its rate: mean 25.6 -> 31.8 Mbit/s) and the
  H2C 0x69 buffer over-read (mainline's para[6]), buildroot/x6200/rtw88 0002/0003. Still open: on channel 11 the X6200
  stalls 1-1.7 s now and then (two 5-min runs: 88 % and 79 % mean; the X6100 on that AP 100 %), not coex, not the
  GPS's paging: its 2457.6 MHz harmonic is the suspect (the HackRF sweep); its join scans see one access point of
  three (once missing a -47 dBm one) and with background scans off it stays on whatever the join took.
  PROVEN 2026-11-21 (HackRF beside the X6200 on a 2.4 GHz antenna, receive only, 30 s medians, lna 24 vga 24): with the
  X6200 on, steady lines at 2457.60 MHz (+7.3 dB over the band at 50 kHz bins, +11.9 at 10 kHz) and 2470.61 MHz
  (+10.6), both inside channel 11, smaller ones near 2444.6 and 2453; switched off, none of them, the rest of the band
  the same to 0.2 dB. 2457.6 MHz = 10 x 245.76 = 40 x 61.44 = 200 x 12.288 MHz, the X6200's own clocks.
  FIXED the same day: the 8723D's notch (NBI index 6, CSI mask bit 114 for 2457.6 and 27-28 for 2470.6, the register
  mapping worked back from the driver's own channel-13/14 cases), set by hand in debugfs then by rtw88 0004's
  host_spur_khz (rootfs/etc/modprobe.d/x6200-wifi-spur.conf). WSJT-X's TCI stream on channel 11, 3 min each: by
  hand off 33 % / on 99.9 % / off dropped at 74 s / on 100 %; the built option on 99.4 % / off dropped at 74 s / on
  99.7 %. Still: its join scan sometimes takes the -71 dBm channel-6 AP over the -50 channel-11 one. Open: why the stand-in
  server on the Mac did not decode either. The run (wsjtx_tx.py, enabletx.dylib, shmshim.dylib, the 127.0.0.1 relay,
  macOS's 4 MB kern.sysv.shmmax worked round) is in the scratchpad, not in the repo.
- The X6100's TCI receive audio (or tones.py's recorder) repeated 1.8 s of it exactly, 2.000 s later (correlation
  1.0000, so not RF) in the 1 W pass of that run, the X6100's receiver near full scale (AGC off, -3 dBFS):
  ~/.config/x6x00-bench/runs/20261121-020143-tones-tcihold/usb-1.0W.wav. Its tone fits from 2000 Hz on missed.
  FOUND AND FIXED the same evening (tools/audio_dma/README.md): the kernel's DMA position read (sun6i-dma on 6.1.82),
  now and then a period too far back when the engine changed descriptors between its two register reads; ALSA took
  it as its 2.000 s capture ring wrapping and PulseAudio gave every client the last 2 s again, 2 s late from then
  on (the GUI's decoders too: FT8 DT +2). Reproduced on both radios (pointer reads hammered: X6100 one jump in
  4.7 M reads, its TCI audio 2.00000 s repeated at that moment; X6200 5 in 19 M). Mainline's c90b6973daa3 tried on
  the running X6200 by a test module (x6x00_dmafix) in place of the built-in read: 0 jumps in 19 M reads against 5
  with the old read. In the kernel as patches 9105/9106 (both radios). PROVEN on the built kernel the same evening: the
  X6200 given d545dd48's bundle with --kernel --reboot (it came back by itself, the owner confirmed), two hammers 600 s:
  0 jumps in 19.67 M reads; after that boot the image's rtw88 had host_spur_khz set, the channel rule moved it from
  channel 6 (-77 dBm) to channel 11 (-51) in 2 s, and WSJT-X's TCI stream held 100.0 % mean (lowest 99.4 %). The
  X6100 gets it with the next release, the kernel box ticked.
- BinModem: ported to C and run between the radios (2026-11-21), then REMOVED from the port at the owner's word
  ("that was all fun, but we don't need it in this port"). Besides the X6200's TCI power above, what it found that
  stays true of the radios:
  - TCI audio reaches the air ~0.5 s after the key; at the unkey 0.3-0.5 s of it is still in the X6100's transmit
    path and 1.2-1.4 s in the X6200's (a TCI program that unkeys at its last sample loses that much).
  - A receiver's squelch (the X6100 at 4) takes steady tones from ~1.5 kHz up out of TCI audio: the squelch trap.
  - The X6100's 3.5 W at a 1 W setting was the gcfix5 base patch: the X6100 is back on Xiegu's factory V1.1.9
    (2026-11-21 01:12, the owner: "revert back to factory base, and ignore the nuances for now"); gcfix5's TX I/Q,
    DC per band and 6 m fixes are off the radio with it, its patches and write-up still in the repo.
- 12 m and 6 m calibration (6 m offsets left 0); 0.1 W carrier is about -25 dBc (the offsets were found at 5 W).
  On the factory base now: moot until a patched base goes back on.
- WSJT-X over TCI, transmit timing (2026-11-21 evening, the X6200 into the X6100 through 80 dB, 0.1 W, four overs):
  WSJT-X's own jt9 decoded all four CQs from the X6100's recording at DT +0.5 s (SNR +31..+36); the X6100's FT8 app
  decoded them too. +0.5 s is the X6200's TCI-to-air delay; the WiFi fix took away the rest (1.5-2 s this morning).
  scratchpad wsjtx_bench/dt_decode.py cuts a recording into UTC periods for jt9. Still open: (1) WSJT-X on the Mac
  logged no decodes from the X6200's TCI audio, at full rate, with the X6100's CQ in it (+22 dB in its band) and the
  X6200's own FT8 app decoding it (SNR 7); jt9 on the recording of that audio decodes nothing either, as recorded,
  mirrored or shifted: why is open (the X6100's FT8 signal or the X6200's audio). (2) The X6100 transmitting from
  TCI audio at -10.5 dBFS (tones.py) gives 0.4 W for a moment, then 0.0 W with the ALC at 9.9 for the rest of the
  over: the factory V1.1.9 base folds back when overdriven, and the GUI's drive loop (tx_hold.c) runs only on a base
  without ALC (the X6200). Fix: the X6100's TCI audio through the drive loop too. TRIED the same evening (tx_hold on
  every base, power_hold only without ALC, as tx_player runs the apps' loop): the ALC fell from 9.9 to a median of
  8.4, the power stayed 0.0 W (0.3 at most) and the X6200 still heard no tone, while the X6100's own FT8 app gave
  0.10 W on the same factory base the same day. Not committed.
  TAKEN APART step by step the same evening (tones.py --tx-mode; a debug build logging the TCI frames and the
  receive-only hold's callers; the codec's playback monitor; PulseAudio's streams polled): NOT 0 W. The TCI path is
  sound: every frame taken and played at -20 dBFS (the client's -10.5 levelled), the modem bit on, the radio on the
  air, the playback stream steady (350-375 ms queued). Eliminated: the mode (USB and USB-D the same), the drive
  level (the codec 18 dB down: the same), the sender, the key. What it is: (1) at a 0.1 W setting the X6100's
  meter reads 0.0-0.1 W, which is what was asked; the tones were in the X6200's audio (793-2893 Hz, 16-21 dB over
  the noise with its AGC off); tones.py missed its faint 1000 Hz mark and said "no tone found". (2) The factory
  V1.1.9 base starts a TCI over HOT: 0.4 W, then 0.9-1.0 W in the first second at a 0.1 W setting (Xiegu's ALC loop
  raises its gain through the pause; gcfix5's hold fixed it), and the bench's watch kills the over at ~2.6 s ("1.0 W
  on the air, 0.1 W asked"): that was the "audio stops after 2.5 s" and the re-hold (the bench's own disarm). The
  apps' player does not overshoot: its drive starts from the learned level and climbs.
  FIXED 2026-11-22 (the owner: "Yes, ramp it"; src/tx_hold.c on a base with ALC): a TCI over on the X6100 starts at
  TCI's own learned level (params tci_tx_level_alc; the apps' level, tx_player_level_1w, until it has one), the codec
  taking it (AIF1 DA0, back to the playback volume after) and the samples at -20 dBFS, the first 1.5 s faded in from
  6 dB under, then the apps' ALC rule. Why it works: Xiegu's loop is an AGC on the audio (tools/base_update/README.md),
  so the start of an over goes out over the setting by as far as the loop had come down; at -20 dBFS with the codec
  at 0 dB that was 36 dB (its ALC 9.9), at the apps' level a few. Bench, tones.py x6100 x6200 through 80 dB, USB-D:
  0.1 W: before, 0.6-1.0 W spikes at every tone start, then 0.0 W at ALC 9.9, the X6200's audio no higher than its
  -45 dBFS floor, the watch stopping the over; now the meter at the setting, ALC median 1.3-1.6, every tone
  300-2900 Hz found at -18..-20 dBFS (25 dB over the floor), offset -7.53 Hz, 2nd harmonic -46..-80 dBc. 1 W (never
  reached before: the 0.1 W probe was stopped at 0.9 W): at most 1.0 W, 0.9-1.0 W held, all tones at -14..-15 dBFS.
  FT8 over the X6100's TCI (scratchpad ft8_tci.py: "CQ Q1TST" by tci_tone_tx.py at the slot), the X6200's FT8 app
  decoding: 4 of 4 (0.1 W twice, 1 W twice, meter median 0.9 W and max 1.0 at 1 W). The learned level settled at
  -44.4 dBFS for 1 W; the apps' -37.48 dB untouched. tools/txlevel models Xiegu's loop (its gain's climb in a pause
  and its 0.13 s fall): the old level 36 dB over gives the hot start, the new one none (52 of 52); tools/tci checks
  the codec, the level and the fade. Left: every over on the factory base reads 0.3-0.4 W and ALC 9.5 for about
  half a second at the key, whatever the audio (at 1 W too, under the steady 0.9 W, while the TCI audio is still
  faded 6 dB under); the X6200's audio shows a 100 ms blip 15 dB under the signal there: Xiegu's loop sending its
  own noise at the key (the README), not the audio. Not worked on.
  (1) FOUND the same evening: not the audio. ft8_lib decodes that recording ("CQ Q1TST EN80", SNR 8, DT +0.1, 1319
  Hz); jt9 decodes nothing from it, nor from ft8_lib's own perfect synthesis of "CQ Q1TST EN80" (it does decode
  "CQ N0CALL"). WSJT-X's chkcall: "Real calls don't start with Q, but we'll allow the placeholder callsign QU1RK":
  any Q-prefixed call but QU1RK is nonstandard there (its ft8code sends "CQ Q1TST EN80" as type 4, "CQ Q1TST"),
  while ft8_lib's pack_basecall packs Q1TST as a standard call (type 1, with the grid), which WSJT-X then throws
  away. So the bench's test calls (Q1TST, Q2TST) are invisible to WSJT-X, and the radios' FT8 apps (ft8_lib)
  disagree with WSJT-X on them. No real callsign starts with Q. FIXED: ft8lib 0002 takes WSJT-X's whole rule (a
  letter in the first two, a digit second or third, one to three letters after, six at most, no Q; QU1RK, WSJT-X's
  own exception, comes back as 3XU1RK even in WSJT-X), which also catches real calls: Guinea's 3X and Eswatini's
  3DA0 calls went as standard through old shortcuts and WSJT-X dropped them too. Proven: 15 messages read the same
  by jt9 and ft8_lib (0 before for the Q, 3X, 3DA0 ones); ft8_lib's tests 2617 OK, 0 failed (the 3X/3DA0 calls
  moved to nonstandard checks); end to end on the radios (the library hot-installed on both): the X6100's FT8 app
  CQ Q1TST, the X6200 decoded it (SNR 3), WSJT-X live on the X6200's TCI decoded it (-1 dB, DT 0.0), the X6100
  decoded the reply "<Q1TST> Q2TST" (SNR 7), PASS. bench.py takes a test call bare or hashed.
- 2026-11-22 (the owner: "that rake is only around 14.69", i.e. 14.069): on the owner's 14 V bench supply (the stock
  charger off the radio, no RTL on its USB) no ~6 kHz comb at 14.05-14.09 MHz; the lines left near 14.07 move with the
  X6100's own dial (scratchpad rake_dial.py: at 14.074 MHz +-17.5 and +-35.2 kHz pairs and -5.5 kHz, at 14.100 only
  -47.6 kHz, at 14.060 +-44 kHz): its receiver's own spurs, none inside the 0-3 kHz it listens to. So the comb was
  most likely the stock charger; to confirm, the charger back for a minute (the owner's step). Was: the X6100's
  "rake" (the owner, 2026-11-21): steady carriers every ~6 kHz on 20 m (14.052, .058, .064, .070, .076,
  .082, .088, .094 MHz, 10-15 dB over its floor) on A and G. Not the X6200 (its dial to 40 m: the comb unchanged);
  the hump at 14.075 is real FT8 traffic leaking past the pads. The HackRF beside both radios (ANT500) saw no comb
  (too deaf at HF to rule out a weak radiator). Suspects: the RTL stick in the X6100's USB (RTL0) and its charger;
  next: unplug each for a minute while the spectrum is watched.
- On-screen checks DONE 2026-11-21 (screenshots over SSH, tools/screenshots/drive.py, 0.1 W into the 80 dB chain):
  G's VOL pop-up shows upright on G held up ("X6200 VOLUME 1", its bar). On the air the Sentry jumps to G (the bench
  arms there), and G's SPECTRUM stood still through a transmission (0 pixels changed between shots 1.5 s apart, both
  radios; the owner had seen it: "on TX the voicegram still pauses"): e9e3d890 kept A's and F's going, not G's. FIXED:
  G's rows of the same dim shade at its pace (ws_feed); its voiceprint's floor held on the air (it had followed the
  over's near-silence down and drew the hiss white ~2 s after each over) and dim columns when no audio comes, counted
  from the key so bursty audio is not doubled. Checked on both radios, PTT 5 s at 0.1 W: spectrum 5700-9400 pixels
  changed between shots on the air, the hiss after the over as before it. Still not done of the 2026-11-14 ask: the
  voiceprint OF THE MICROPHONE while keyed (it shows the capture, which on the air is not the mic unless the base is
  in record mode, as the SDR walkie sets it); to try with a voice at the mic.
- The X6100's processor temperature sensor: dead on that board (below); the cause (its analog supply or reference)
  only by schematic or a meter, if wanted.
  The heater cross-check ran 21:22-21:32 (build/temps_heater_2026-11-20.png): the three sensors agree, the X6200's
  processor a steady 22 C over its power chip; the owner called it enough at 10 minutes.
- The G WALKIE voiceprint looks rich even while the radio is not transmitting: structured patterns leaking into it,
  first seen on the X6100 (the owner, 2026-11-21: "figure out why"), and on the X6200 too ("2 happens on both
  radios I'm pretty sure"). To do: capture the X6100's
  receiver audio with the antenna side quiet (the 80 dB chain, the X6200 not keying), find the lines' frequencies
  and how they move (fixed tones: the board's own clocks, the charger, the display or WiFi; moving with the dial:
  the receiver), against the X6200 the same way; then the source and whether it can be filtered or fixed.
  FOUND 2026-11-21 (TCI audio with the 80 dB chain quiet, the dial moved 1 kHz between two 20 s recordings): mostly the
  voiceprint's own scaling: its dark was the 33rd percentile of 0-4 kHz, which the receiver filter's empty part pulled
  below the hiss (X6200: 3 dB, 79 % of a quiet passband lit; X6100 in CW 300-1100 Hz: 16 dB, 99 % lit). Now the median
  inside the filter (37 % lit, faint), app_sentry.c vp_column, both radios run it. Left: the X6100's audio carries a
  series of ~781 Hz (781/1562/2344/3125/3906 Hz, +9..15 dB over its noise) and lines at 97, 147, 290 Hz, fixed with
  the dial, so its own audio path (the backlight PWM is 100 Hz: not it; 781.25 Hz = 50 kHz / 64 fits a base DSP frame);
  the X6200's audio has none. Where they enter (the same day, AGC off, RF gain 63 then 0, CW 300-1100 Hz): the hiss
  is shaped by the CW filter and did not move with the RF gain (the receiver's own floor behind the 80 dB chain), the
  lines did not move either and are NOT filtered (1562-3906 Hz above the filter's 1100 Hz edge, 97/147 Hz below its 300
  Hz edge at +25 dB): added after the base's DSP, between its audio out and the SoC's codec. To do: a scope on the
  base's audio line, if wanted. Ruled out: the display (60.19 Hz: 27.5 MHz / 542 x 843), the backlight PWM (100 Hz),
  the base's flow packets (4136 bytes at 114688 bytes/s, 27.7 a second: ttyS1 streams at its line rate, no 48.5 Hz
  bursts).
  NARROWED the same evening (scratchpad vp_mic1/vp_mic2/vp_param/vp_fine.py, receive only, the mixer and params put
  back): the receiver audio reaches the codec on MIC1 (24 dB boost; Line In is off). MIC1's boost at 0 dB took the
  hiss and every line down the same 24-25 dB, so they are on the wire at MIC1's input. MIC2 at the same 24 dB boost
  (nothing on it) has none of them, its floor 45 dB under them: not the processor board, the codec, its supply or mic
  bias. Not the base's line-out gain either (params line_out 2, 10, 20: nothing moved, not even the hiss; that gain is
  not on this path) and not the charger (off, the battery log's charging 0: the same lines). 40 s at 0.18 Hz bins: the
  series is 780.69 Hz exactly and its x2-x5 (50 kHz / 64 = 781.25 is 0.07 % away); the low lines are two families,
  ~97.9 Hz (x1, x2, x4: their level varies run to run) and 145.35 Hz (x1, x2), not one 48.5 Hz. So they come out of
  the base board on its audio line, or in on the ground between the boards. +8..+15 dB over the receiver's own floor
  behind the 80 dB chain: an antenna's band noise mostly covers them; a comb notch in the GUI's receive audio at
  780.69 Hz x k would take the series out if ever wanted.

Changed state today:
- RTL sticks: back on both radios (the owner, 2026-11-21, after taking them off the same day): LISTEN RTL/BOTH and
  Kraken DF can be tried again. TX CAL on F SYSTEM: CLOSED by the owner 2026-11-21 ("we close out that research").
- ardopcf security review (2026-11-21): fixed in the vendored source (third-party/ardopcf/UPSTREAM.md lists the three:
  a DISC from anyone keyed the idle radio; a 16QAM frame wrote one int past the RS decoder's elp; an ARQCALL from Pat
  with spaces before its count overflowed the host reply). Proven with the audit's harnesses on ardopcf's own objects
  (before: keyed, ASan overflows; after: none) and on the X6200 (host commands to a no-sound ardopcf). Not reported
  upstream: DROPPED at the owner's word (2026-11-21, "ignore the ardocpcf and axspawn reports"). axspawn (linuxax25)
  is not in the image: its findings (the "sys" answer falls back to a cleartext login, root writes ~/.bcpasswd through
  a symlink, a clock-seeded challenge, ...) are upstream's only.
- Power: the X6100 on its stock adapter (a charger): its battery emptied 52 % -> 0 % and it switched off in a 10 W
  over at 19:31; at 8 % and charging at 01:00 UTC. bench.py's supply gate now refuses an over on a radio its supply
  is not holding (af73b3b0). MANUAL "Power supply" (fd0145dc): a regulated 13.8 V supply of 5 A or more.
- Pads: 40 + 40 dB (the X6100's 30 dB swapped, chain-ok 80 dB); at 1 W and under, --rx-att 0 for the margin.
- Host checks: fixed for good (autolink, ce660ea3/9f117153): a new call in the GUI's sources gets a stand-in made;
  green on every push since.
- X6100 temperature: was none (driver removed 11-19); now its power chip's (AXP223, "PMU", af8f5fae); the X6200's
  Walkie panel shows CPU and PMU by turns (50905e06). The processor sensor tested by hand (no driver): no clock,
  sample rate, acquisition, chopper, filter or quiet audio makes it convert; Xiegu's 5.8.9 kernel and gdyuldin's
  never had its driver.
- TX audio's 2nd harmonic (-32..-35 dBc): made in the processor-to-base hop; TCI levelled to -20 dBFS on the X6100
  (35f55221): -44..-46 dBc whatever the client sends.

Done: the X6200 kept off WiFi channels 9-11 (98a8bd8d; its firmware is the newest Realtek's); 5 and 10 W runs and the
image proved on the mirror (505af2c1); gcfix5 image and carrier calibration end to end, carrier kept through band
changes (e9e3d890), v0.3.12.40 released; one-file draft PR (never submitted; deleted 2026-11-21 at the owner's word); Sentry spectrogram time on air and G's
VOL pop-up (e9e3d890); the radio's fingerprint and calibration file (842d7a1f); a band without a saved calibration
gets 0, not the last band's (2772e164); the X6200 never calibrated or keyed for it (its last TX 06:01, FT8 tests);
10 W image/carrier on every band (cc607867); never wait on CI (practice); stale background waits closed.

**2026-11-19 afternoon: the owner's new list** (asked while v0.3.12.32 was pushed to the radios)

- Done (hot build on both radios, in the next release): F SYSTEM's spectrogram under WHAT IT DOES, A's rows drawn by
  the GPU, and the GPU % the time it spent drawing (13 % on the X6200 where the clock's place read 0 %; the X6100 has
  no Mesa till its card image: 0 % there is true); the base's firmware on every Sentry title, "base v1.1.9" on both;
  the X6100's bolt (its base's charging flag hardly ever up: a supply in with Charger On counts, as on the X6200);
  SSTV's picture starting dark as WeFax's (the owner chose dark).
- Done in the suite: the pictures taken with the X6100 made like the X6200 first (dial, bands, volumes, the apps'
  frequencies, the GUI state: B's frequency, the pages, the Direction finder's target and array), its own back after.
  DONE 2026-11-19 23:44: taken again on v0.3.12.36 (docs/parity/v0.3.12.36, redacted, OCR-checked): the X6100's
  Weather, Flights and Internet radio helpers answer; 56 pass, RADE to look at (headphones/TCI), SSTV on the X6100
  slower than the suite's 15 s (pictured before it tuned to 14.230).
- DONE 2026-11-19 23:28: both radios on v0.3.12.36 through their own updaters (`x6100-update upgrade --kernel --uboot
  --reboot`, "confirmed"), each back by itself in under 2 minutes, the X6100 by kexec; the cards had the bundle's
  kernel and U-Boot already. The X6100's SSTV opening in 9-17 s: GONE (2026-11-23, timed from the Sentry's A on
  v0.3.12.46's GUI, receive only: SSTV 0.1-0.2 s on the X6100 3 of 3, 0.2-2.4 s on the X6200; WeFax 0.1-0.2 s on both).
- Done in the bundle (the owner: "I should not have to write it, the updater should"): the image's Mesa, GStreamer
  and ffmpeg with the libraries they load (mkbundle.py CARRY_PKGS), and level 3 reached by the bundle's kernel and
  U-Boot (image-required's x6100-boot and x6200-boot lines): the X6100's card v0.3.12.28 gets the GPU through its
  update, no card to write. In the release after v0.3.12.32.
- Found by the Walkie test (bench.py radios-walkie, 2026-11-19), to fix:
  - the X6100 sends 0.9 W asked for 0.1 W on 6 m (its own meter; the watch stopped it): its low-power setting on 6 m
    (TX Cal?) before G's 6 m from the X6100 is tried again;
  - DONE: the X6200's S-meter in AM, SAM and WFM from its spectrum inside the filter, set against the base's reading
    learned in the other modes (dsp.cpp psd_meter): an X6100 AM over now reads -117 -> -69 dBm on 10 m;
  - DONE 2026-11-19 night: the X6200's reports to a page (meter, spectrum, state) stalling for seconds and A/B taps
    over WiFi missed: its Bluetooth GPS keeper paging the GLO 2 (not there, 2205 tries since the start) every
    ~16 s, each try taking the WiFi's shared chip and antenna for 1-3.6 s (17% of pings lost; 0 with the keeper
    stopped, 0 on the X6100 and the router at the same time). btgps-ctl backs off now (10 s doubling to 300 s,
    back to 10 s after a connection): 1.2% lost over 200 s while backing off, one stall per 5 min at the cap.
  - DONE 2026-11-20 night: the X6100's base patched (gcfix3, tools/base_update/README.md): 0.1 W right on 6, 10, 20
    and 40 m, the codec PLL fixed; shipped as a block patch applied on the radio to Xiegu's own file
    (x6x00-base-patch), no base distributed.
  - DONE 2026-11-20 morning: gcfix4, the X6100's power loop held through pauses (a hook, x6100_hooks/alc_hold.s):
    bursts after 50 ms to 3.2 s of pause start within 1 dB (Xiegu's: +5 to +18 dB, 0.7-1.0 W at 0.1 W, then 6-14 dB
    under). The X6200 "dropping changing tones with AGC off" was its squelch (at 4) muting its audio, and so was the
    image "101-110 dB down": tones.py opens the listener's squelch now.
  - the X6100's transmit image and carrier, MEASURED 2026-11-20 afternoon (tools/bench/image.py, 5 W, every band
    160-6 m; tools/base_update/README.md has the table): the image is the signal mirrored around the dial (to 0.09 Hz),
    -42.5 to -54.5 dBc (15 and 12 m worst), Xiegu's own V1.1.9 the same; the carrier -30 to -48 dBc at 5 W but about
    -12 dBc at 0.1 W on HF (constant milliwatts). The owner (2026-11-20): "that feels like something we'd want to
    fix ... a calibration tool for screen F seems relevant"; the X6200 not wanted. CLOSED 2026-11-21 (the owner: "close
    the tx cal on system since we close out that research"); the plan was: TX CAL on F SYSTEM (a
    dummy load, the lowest power, a second receiver: the other radio or an SDR on the X6100's USB) nulling the carrier
    with registers 50/51 (exist, zero, not yet tried) and the image with a second base hook (Q' = g Q + p I on the
    block before the DAC, per band, values from the GUI through a register). The X6100's calibration block is empty
    and its base has no TX phase/gain correction (registers 48/49, x6100_rphscmp/rmagcmp, correct the receiver only).
    Seen, not explained: the tone's 2nd harmonic at dial + 2 fa (-34 to -38 dBc, both bases); humps 38-41 kHz either
    side on the X6200's panadapter on 80, 10 and 6 m. 10 W not run (the bench's twice-the-power pad rule: the owner's
    word needed). A draft PR to RF.Guru's X6100-Baseband (the errata as an opt-in patcher, these findings) is in
    drafts/x6100-baseband-errata/, NOT submitted (the owner: "do NOT submit it"); deleted 2026-11-21 at the
    owner's word ("delete the draft PR"; to the macOS Trash), never sent.
    DONE 2026-11-20 evening (the owner: "please make it so"): gcfix5 (x6100_hooks/tx_iq.s, register 52), the GUI's
    IQ balance (Settings > TX Cal, remote `set txig/txip/txio/txqo`), tools/bench/txcal.py (one over a point: the
    base takes registers 50-52 between transmissions only), the X6100 calibrated on every band (tools/base_update/
    README.md has the results). 10 W run too (power-ok; the stock adapter, a charger, sets the top, gcfix and Xiegu's alike).
    Left: (screen F's TX CAL closed 2026-11-21); a combined build with RF.Guru's payload keeps their build name, so the GUI
    would not know gcfix5 is in it (a marker of its own needed); the image's change across the audio band (one G, P
    per band).
    Later the same evening (the owner: "dive deeper ... find a novel solution that *works*"): 15 m's (and 12 m's)
    image moves during a session, -46..-53 dBc to -29 (a 9 % I/Q gain change) and back; not the audio band, the
    base's Hilbert filter (equiripple, 84.5 dB), an I/Q skew, the base's power loop, the X6200, or intermod with the
    carrier; 10, 17, 20 m do not move. A "warm balance" switched by a heat estimate was built and REMOVED the same
    evening (the owner: "your warm balance theory was simply garbage ... revoke it"): the shift did not follow
    transmitting time. The X6100's own battery log found the likelier suspect: the stock adapter (a charger) was not
    holding the radio (0.2-3.9 V at its input in most TX minutes, ~8 V idle charging), so it ran on its 6.6-7.2 V
    battery, 52 % at 18:12 down to empty. To do: the shift (and the 10 W table) again on a supply that holds the
    radio. tools/base_update/README.md, "Why 15 m varied".
    2026-11-20 19:31 (80 dB, the owner away with 10 W allowed): the X6100 switched off in the first 10 W over of a
    txcal run on 15 m. Its battery log: the internal battery empty (1 % from 19:26), the adapter not holding it up all
    evening; no hang (the GUI watch never acted, no kexec). The owner found it off and switched it on (20:07, 0 %).
    Before more TX tests: a supply for transmitting (the stock adapter is a charger), the battery charged. MANUAL
    "Power supply": Xiegu's 9-15 V, 13.8 V and 3.5 A; ours, a regulated 13.8 V supply of 5 A or more.
    The tone's 2nd harmonic (both bases): TESTED 2026-11-20 evening. It follows the audio level 1 dB for 1 dB at a
    held power (0.05 of full scale -53 dBc, 0.1 -44, 0.3 -33, 0.5 -28; 20 m, 1 W) and the processor's Headphone output
    (-6 dB: -42, -12 dB: -47); the base's line-in gain (its codec PGA, page 1 reg 0x3B) changes nothing: made in the
    hop from the processor's headphone amp to the base, before the PGA. FIXED for TCI clients (the GUI's own modes play
    at -28.6 dB): tci_server.c brings a transmission's peak to -20 dBFS (X6100 only): full scale -45.5 dBc, 0.3 -43.5,
    0.03 -44.4, 0.9 W each. At the 10 W setting (on the battery, the adapter not holding) it cost ~0.4 dB of the top.
  - the X6200's WiFi, 2026-11-20 morning (v0.3.12.39): pinged at 10 Hz, 4-19% of replies over 100 ms, spikes of
    0.5-1.5 s, 0-7% lost (the X6100 beside it, same router: max 18-48 ms); its TCI audio reaches a client at 10-76%
    of real time (parec on the radio itself: full rate; the TCI queue holds 0.6 s and drops the rest). Not the GPS
    keeper (6-10 tries in 7-29 min) nor the RTL stream (the WiFi/Bluetooth chip is on the EHCI port, the RTL on the
    MUSB one). `bluetoothctl power off` made it seconds-long stalls till a restart: never again.
    Afternoon (2026-11-20): the network has several access points under one name. The X6200 joined the channel-11
    one at the morning's start (its log), so that was the morning's link; the X6100 is on that one too. Measured 60 s at 5 Hz:
    the X6200 on channel 11 at -58 dBm averaged 668 ms (stalls to 4.5 s), the access point sending to it at 2 Mbit/s;
    on the channel-6 one at -70 dBm, 30 ms (max 347 ms, 0 lost); the X6100 on channel 11 at -51 dBm, 5-7 ms.
    Worse on the stronger signal = something on channel 11 at the X6200, not the link. Suspect: the X6200's own clocks,
    whose common harmonic 2457.6 MHz (10 x 245.76 = 40 x 61.44 = 200 x 12.288 MHz) sits inside channel 11 (to check
    with a receiver beside it, radio on and off). Its firmware is not it: WiFi 48.0 and Bluetooth 0x828A96F1
    are the only ones Realtek ever put in linux-firmware (2019), and the driver's coexistence report (debugfs
    coex_info) says they match; the coexistence runs TDMA case 3 (Bluetooth idle, table 10) on both channels. The
    driver (lwfinger/rtw88, pinned 461b696, May 2025) is 134 commits behind, a coexistence update (26020420) among
    them. With background scans off it stays on whatever access point it joined at the start. Left as found: the X6200 on the
    channel-6 one. Left to look at: the 2457.6 MHz check, the driver update, a way for the X6200 to prefer
    another channel when it has one; the TCI queue could hold longer for audio-only clients.
  - the X6200's squelch (Xiegu's, in its base): at 4 it kept a steady -50 dBm tone muted for its whole 6 s with the
    AGC off (with the attenuator in too), and opened only after ~4 s with the AGC on (2026-11-20). Muted, the TCI
    audio is a flat -48 dBFS; tones.py and the decoders open it for their runs.
  - the X6100's 6 m over-power, studied 2026-11-20 (tools/base_update/README.md "Every base compared"): Xiegu's loop
    has 34 dB of range on 6 m and the meter is its own detector, so 0.8 W at 0.1 W means its floor or a carrier the
    loop cannot touch (the DC offsets are added after its gain; this radio's are all zero, never calibrated). The
    test (TX, the owner's go, the X6200 listening through the pads): 6 m USB with no audio at 0.1 W (a carrier =
    the leak: null it with TX Cal's IQ offsets on 6 m), and FM at 0.1/0.3/0.5/1 W (the same ~0.8 W = the floor: a
    base patch, the +6 dB at band >= 20 to 0 or the -40 dB clamp lower). Earlier notes:
  - the X6100's 6 m over-power, looked at 2026-11-19 night: real, not its meter (the X6200 heard its 6 m at
    -64/-66 dBm at a 0.1 W setting against -70/-71 on 10 and 12 m; the path back loses the same on 6 m). Its power
    is the base's (x6100_control_txpwr_set); the one Linux handle, the band output gain (dac_gain_set), works on
    gdyuldin's patched base r8+ only, not Xiegu's V1.1.9 this X6100 runs. The owner's call: the patched base, or
    the Walkie saying the X6100's 6 m floor (about 0.8 W), or leave it.
- The X6100's processor temperature sensor (2026-11-19, the owner's heater run: 16 min at 5 readings a second, a
  heater on both radios from 35 s): its raw values sit on round binary steps around mid-scale (2048, 2176, 2240, 1984,
  2016: 0x800, 0x880, 0x8C0, 0x7C0, 0x7E0), the ADC's conversion not settling, not a temperature with noise; a 30 s
  median, a 60 s mean and a Kalman filter followed which steps came up, not the heat. The X6200's in steps of one
  (2031-2036), 59.8 -> 60.2 °C under the heater (the processor's own heat rules it). Fixed by leaving the driver out
  of the X6100's kernel (fbe9d59c, v0.3.12.35).
- DONE 2026-11-19 (v0.3.12.36): the X6100 restarts by kexec and stays on, the default of x6x00-reboot (the update's
  restart, the power-on box's, x6x00-gui-watch's); the box says it comes back by itself where kexec can run. The
  fifth try: rcS at 3.4 s, the GUI at 23.9 s on DRM ("drm: 480x800"), usb 3-1 at 25 s, TCON0's divider 0x80000001.
  Was: the X6100 switches off at a restart (x6x00-reboot, a plain reboot there; 2026-11-19 after its update, the owner
  switched it on): the X6200's fix is its base's "s" left out, the X6100's base has no such word. Likely the base's
  control lines the GUI holds (bb_reset among them) let go while the processor resets, the base restarting off;
  to find out with the owner at the radio (each try switches it off).
  - kexec (the processor not reset), 2026-11-19 on the 977abb1c kernel (CONFIG_KEXEC in, kexec -l / -u fine): the
    card's kernel started (start trace: rcS at "up 67 s", 64 s later than a power-on's 3.4 s; the GUI at 117 s,
    "base answered", the relays clicked), but the GUI had no screen ("drm: no usable device, fbdev is used"; a
    power-on finds the JLT4013A panel on SPI, sun4i-drm) and the WiFi never answered; the owner held POWER. Its
    kernel log went with the power. Now a test only, `x6x00-reboot --kexec`: the start marked x6x00.kexec=1, both
    kernels' logs kept on DATA (app_logs/dmesg_before_kexec.txt, dmesg_kexec.txt at 0, 60 and 180 s).
  - Second try with those logs (2026-11-19 19:37): the same, and why. The display: the panel's power, reset and SPI
    set-up fine, but the controller never gave a vblank (WARNING "vblank wait timed out" from sun4i_tcon_probe's first
    modeset, then "flip_done timed out" every 10 s, sun4i_backend.c:461 its register load never latched): the old
    kernel left the pipeline running (6.1's sun4i-drm has no shutdown hook), the new one found it so. That first
    modeset ran in the deferred-probe worker and held it 64 s: the SD card, rootfs and USB probed only at 66 s. The
    WiFi: the PMIC's regulators kept their voltages (no "Bringing ... into 3300000" lines) and the chip its power
    (gpiochip0 line 5, 0 = on, the GUI's pin_wifi), so it was on the MUSB port all along and never attached again
    ("unhandled DISCONNECT transition (a_idle)", no usb 3-1). Fix to try: rcK unbinds sun4i-drm before kexec -e (the
    driver's own stop, its resets asserted); rcS of a kexec start puts the WiFi power off before anything binds
    the chip, the GUI's start powering it as at a power-on.
  - Third try with both (2026-11-19 21:42): no better. The unbind ran ("sun4i-drm unbound: done") and the new kernel
    still had no vblank, the same 64 s; the WiFi off at 67 s and on at the GUI's start (119.9 s), and still no usb
    3-1, the MUSB driver's "unhandled DISCONNECT transition (a_idle)" at its load again. A power-on start's clock
    tree, CCU, TCON0, backend and DRC registers saved as the baseline (pll-video 165 MHz, pll-mipi 27.5 MHz,
    lcd-ch0 27.5 MHz; CCU 0x040 0x90C00005). Next try: rcK powers the WiFi off and unbinds musb-sunxi (its reset
    asserted) before kexec, and x6x00-kexec-state saves the clocks and registers running, at the handover and in the
    kexec start, to set beside the power-on baseline: what a reset clears that the unbind does not.
  - Fourth try (2026-11-19 22:00): the WiFi came back by itself (usb 3-1, SSH answered at 136 s): the MUSB unbind
    was the fix. The display: the clock unit's display settings identical to a power-on's, but TCON0's dot-clock
    divider (0x044) 0x80000000 against 0x80000001, the line counter (0x0fc) standing. The TCON's reset leaves the
    divider 0, sun4i_dclk_recalc_rate reads 0 as 1, the inherited PLL-MIPI and lcd-ch0 already give the panel's
    27.5 MHz, so clk_set_rate(27.5 MHz) returns early and never writes the divider. `devmem 0x01c0c044 32
    0x80000001` on the running kexec start: the line counter ran and the 10 s flip_done timeouts stopped. Fix: rcK
    moves PLL-MIPI's M while it is off, so the next kernel's rate differs and it sets the clocks as at a power-on.
    The kernel-side fix, should this one fall short: sun4i_dclk_recalc_rate to return 0 for a divider of 0.
- Host checks on the Mac (item 6, "wrap 6 up"): liquid-dsp 1.4.0 built here; align and tx_meters run here now;
  cw_trainer (one case: a character decoded in silence, Mac arithmetic) and settings_keep (its copy under macOS's own
  SQLite) fail on a Mac only; cw_kbd, direct_hole, screen_saver, tci_ask and wf_image want the GUI's PC build (libnm);
  18 need Linux itself (a Linux VM here would close them: the owner's call).
- Walkie, end to end 2026-11-19 (both on G, one channel, one on the air at a time, no stops): the X6200 heard by the
  X6100 on every band 160 m - 6 m in AM and FM (26 of 26); the X6100 by the X6200 on every band and mode, its misses
  each an over the X6200 sent nothing to this computer for 2-8 s (measured), heard when run again with the harness
  waiting for what it held back. Open: the X6100's 0.7-0.9 W on 6 m asked for 0.1 W.
- Walkie, done with the owner's word on the cable and pads (given 2026-11-19, "cable is confirmed"): the Walkie (G) on both radios sending to and hearing each
  other on every band and mode G offers, at 0.1 W through the pads (CB refused by both radios' TX ranges).

**Open, in the order they are worked**

*The Kraken and USB first*

1. **The Kraken roadmap** (tools/rtlspec/x6x00-df/ROADMAP.md, the owner's asks of 2026-11-17): done, items 1-9
   (cores, page I's tuning and FOLLOW, the web page, gpsd, the bearing log, TNA/sources/FB/smoothing/ROOT-MUSIC,
   custom arrays, targets 2-4 (in the bearing log too), the IQ server), 10 (sample-rate slewing) and 11 (the antenna
   side calibrated on a source at a known bearing; 4+1, L and minimum-redundancy layouts; KrakenRF's whips' range).
   Left, with the owner's hands: a bearing over the air on the real array (a transmitter at a known bearing, the
   whips' spacing). The Kraken is unplugged for now (one RTL on the radio). Passive radar parked.

2. **A Bluetooth GPS for gpsd, beside Bluetooth CAT**: built 2026-11-17 (btgps-ctl, Settings > Bluetooth's GPS for
   gpsd, the web page's Direction finder GPS card): beside CAT rather than instead of it, as the radio is CAT's
   serial port and the GPS is its own. Tried on the radio with an address that is not there (it keeps trying, CAT
   untouched). The Garmin GLO 2 paired 2026-11-18 (no PIN, channel 1, NMEA at 10 Hz): the kernel makes no RFCOMM
   TTYs, so `rfcomm connect` could never have worked; btgps-bridge (a socket onto a pty) feeds gpsd instead, which
   reads it (NMEA0183). F SYSTEM > GPS shows its health (link, fix, accuracy, satellites, time, what uses it) and
   the titles say GPS; C/D's sun and AIRSPACE take the GPS, then the phone, then the locator. Left: its first fix
   under open sky, and that fix on page I.

*Hardware to plug in*

3. **DVSI USB-3000 back on USB**: back on the HOST port 2026-11-18 (the owner): the SDR daemon found it
   (AMBE3000P25F on /dev/ttyUSB0, DMR rate 33) and its self-test passed (a 1 kHz tone encoded and decoded came back
   at 1000 Hz). It has the HOST port alone now, so B has no SDR: the voice heard again on B waits for an SDR beside
   it (a powered hub). With five Kraken sticks streaming it would share an endpoint again. Tried through the X6200
   instead (2026-11-18 02:48, the bench harness's `hackrf-iq`): the D-STAR test transmission from the HackRF on
   29.600 MHz FM (-x 40, 70 dB of pads, -102 dBm at the radio), the X6200's receiver audio into dsd-fme on the radio
   by hand, its voice frames to this USB-3000 through the daemon (libmbe-dvsi: AMBE3000P25F at 127.0.0.1:2460):
   D-STAR synced all through, 14 s of speech back (radio_captures/bench/dstar_via_x6200_usb3000.wav), its slow
   data ("CABLE TST") with bit errors in every frame, the speech's envelope 0.54 alike the clean decode from the
   RTL. The errors are the X6200's FM audio (de-emphasised, through the codec; a 0-6 kHz filter changed nothing):
   GMSK wants the FM discriminator's flat output, which B's SDR gives. The X6200's own audio is not a source the
   daemon's decoders take; that would be a feature of its own (10 m and 6 m FM digital voice).
   B's own chain tried with no SDR (2026-11-18 03:35-03:45, tools/bench/dv_replay.py): the cable tests' recordings
   replayed into a test SDR daemon beside B's (resampled to 2.1168 MS/s), its dsd-fme's voice through this USB-3000
   via B's daemon: D-STAR (TO CQCQCQ FROM N0CALL TEST, "GOLDCORPHF CABLE TST", no bit errors), YSF (TO CQCQCQ FROM
   N0CALL), P25 phase 1 (TG 4321 FROM 1234567 NAC 123), NXDN48 and NXDN96 (TG 100 FROM 1234 RAN 1): every call
   decoded, speech peaks 25000-31000. DMR too from 2026-11-18 16:55 (a test daemon's DMR frames through B's
   daemon's port, as dsd-fme's go): TS1 CC1 TG 3100 FROM 3112345, the voice heard (peak 30000, 76% of it in
   300-3400 Hz). Found with it:
   a second daemon opened the chip's serial port beside B's (two writers wedge it): it now leaves the chip to the
   daemon that serves 127.0.0.1:2460; RTLSPEC_DSD_PORT gives a test daemon's dsd-fme speech ports of its own.
4. **NXDN96 over the cable**: done 2026-11-17 night (HackRF -> attenuators -> the one RTL, receive only): the generated
   call (TG 100, source 1234, RAN 1) decoded by the third dsd-fme, B showing "NXDN96 441.000 / TG 100 FROM 1234 RAN 1
   TALKING". Its voice waits for the USB-3000 (3), as every AMBE mode's does. Every HackRF -> X6200 cable test is done
   (WeFax, Time Signal, RADE, the channel scan's levels; WSPR cannot be, the HackRF drifts): that cable can go.

*The owner's decisions and longer work*

5. **Page I's decisions** (docs/plans/kraken_df_plan.md): answered 2026-11-17 but D5 (B moved or lent when I opens)
    and D7 (I on the ring only with a Kraken), both kept as built; D11 (five channels) settled by 9104.
6. **TCI audio over the radio's Wi-Fi** (nothing more to do on the radio; kept for the record): looked at 2026-11-17 evening. No stalls of 0.5-1.3 s then (the longest gap
    between frames 280 ms, all the audio there); the limit is how much the radio sends: about 1 MB/s (8 Mbit/s raw
    TCP), the same with no SDR, B's stick or all five Kraken sticks streaming, the CPU 70 % idle; the WiFi chip is on
    the SoC's EHCI (not the MUSB with the SDRs), one bulk-out endpoint. The link: TX MCS 5, RX MCS 1-3, CTS
    protection on (an 802.11b device on channel 6), -57 dBm, 2.4 GHz only. TCI's default audio (0.38 MB/s) then
    loses 5.8 % to a 40 MB copy at the same time. Tried and dropped: DSCP EF on TCI's socket made it far worse (74 %
    over 0.6 s late against 20 %, Wi-Fi's video class going out unaggregated). A longer queue would not help a link
    that is short for minutes. What would: a client asking for less (24 kHz mono int16), the AP on a quieter
    channel, wired Ethernet; perhaps a newer rtw88 (pinned at 461b696, 2025-05) if its USB TX ever gains throughput.
    The manual's TCI section says so.
7. **A GUI that hangs**: the owner chose a restart ("a reboot would be more graceful"). Done 2026-11-17 night:
    x6x00-gui-watch restarts the radio with x6x00-reboot when the GUI's radio-thread or screen-loop heartbeat stands
    still 60 s (MANUAL: A hung GUI restarts the radio).
8. **The 3D waterfall on an X6100**: done 2026-11-18/19 on the owner's X6100 (card image v0.3.12.28): the classic
    screen's 3D waterfall runs (drawn by the processor: that image has no Mesa), the Sentry's A, B and AIRSPACE run on
    it, the suite's every screen passes on both radios (item 12). Left for the X6100's new card image (Mesa, level 3:
    v0.3.12.29 on asks for it in the update box): the same waterfall drawn by the GPU, and the video decoder and CPU
    temperature its old kernel lacks (item 12's devices).
9. **The upstream merge**: done 2026-11-18 (Guru-RF's 73 commits: SVXConnect, Zello, the Weather, Internet radio,
    CB and HF Air Band apps, Flights, Field Ops' NOTA and maps, GhostNet, the first-start guide's language and
    country, the APP grid's scrolling, the spectrum's fill by level, translations, SSH security keys, the speaker's
    one play-mode owner). None of ours lost: every line this fork added or removed since the last merge checked
    against the result; none of upstream's updater taken (it changed none). 14 files in conflict, all kept both
    ways; the image's one ffmpeg made for the Internet radio's audio and our boot video's video together; User keys
    kept on APP 4 (upstream's apps on APP 4-6); the new helpers name themselves GoldCorpHF. Tried on the radio:
    pages A-E, B on the bladeRF, the APP grid with the new tiles; and 2026-11-18 ~17:45 UTC, from the receive-only
    remote session (the radio held receive only): Weather (now and 5 days, NWS warnings), Flights (71 aircraft
    from adsb.lol), HF Air Band (VOLMET list, TUNE to 2950 kHz USB), CB (the FCC plan, a channel tuned), Web radio
    (radio-browser.info's countries and stations, an MP3 stream played and stopped), Zello and SVXConnect opened
    (not set up: nothing sent). Fixed: CB's chosen row lost its name in the Sentry's theme. Not tried: AAC
    stations (a new card image, level 3), GhostNet (JS8's), Zello and SVXConnect connected.
10. **WiFi kept from the first start**: closed by the owner 2026-11-19 ("we can close 10"). What it came to: the
    X6100 dropped off WiFi after GUI restarts. The X6100 control library's x6100_gpio_init asks for the WiFi chip's
    power line (gpiochip0 line 5) at 1, off (the radio's own libaether_x6100_control.so.0.15.3: `mov r2, #1`), the
    X6200's at 0; so each X6100 GUI start cut the chip's power for about a second, and now and then it came back
    unable to see the saved network (2026-11-18 23:21: 680 joins "not in range", 27 s apart, for 2.6 hours, until
    WiFi off and on). The X6100 driver now holds its five control lines itself (src/hw/x6100/hw_x6100.c pins_init,
    WiFi at 0 as the X6200's): five GUI restarts on the X6100 2026-11-19 with no USB disconnect of the chip and no
    WiFi state change. Kept as safety nets (src/wifi.cpp): a failed join NetworkManager started is asked for again
    (rejoin_later; reason 7, "no secrets", blocks the network otherwise) and six failed joins in a row with networks
    in sight restart the chip (no_network_cb; untried against a real wedge). The first-start guide left early (the
    owner's first report) was on Xiegu's own system, its SD card out.
11. **The X6100's keys against the X6200's**: audited 2026-11-19 (the owner: "do that audit and decide if we are
    good"): we are. Both radios turn their key codes into the same key names and the same handlers, so a key of the
    same label does the same job; the X6200's MFK inner ring is the MFK held + the main dial on the X6100. Fixed from
    the audit: that gesture took the dial in every app (in the APP grid the MFK let go opened the tile under it): now
    only in the windows with pages (the Sentry, AIRSPACE, I; app_window.c dialog_has_own_keys); the X6100's AGC hold
    switched split while an app held the VFOs (as ACTION_SPLIT now, not then); the X6100's PRE, AGC and LOCK in the
    Sentry were in no help (GEN's list and the manual name them now). Left as they are: the classic screen's MFK
    control picked in two more presses on the X6100 (push, turn, push); the H-22's F3 has no X6100 slot (its own
    mic); V/M does nothing in the Sentry; the X6100's key codes are a rotation of four (APP, KEY, DFN, MSG), the
    labels doing the same jobs on both.
12. **The X6100 against the X6200, what the 1:1 runs found** (2026-11-18/19, tools/bench/suite.py, radios-ft8 through
    40 + 30 dB):
    - FT8 radio to radio at 0.1 W, "14 dB apart": not the radios. Settled 2026-11-19 with the levels radios-ft8
      reports now: each radio hears the other at about the same level by its own S-meter (the X6100 -69.7 dBm, the
      X6200 -72.0), each sender's meter at its 0.1 W (the X6100's reads 0.2 at that end of its scale). With both at RF
      gain 100 the decodes ran -10 to +11 dB on either radio for that same level, transmission to transmission, so
      FT8's SNR of a strong signal is no measure of a receiver; the earlier gap sat inside that spread. One real setup
      difference found: the X6100 ran RF gain 63, its default, and below 100 its base switches in a stepped
      attenuator (100 - rfg steps; strijar/x6100-base docs/protocol.md); its reply decodes went from -7..-3 to +10/+11
      with 100. `--rx-alike` sets RF gain 100 on both now (and puts it back). A weak-signal 1:1 needs a HackRF at a
      known level near the noise into each radio (the pads to the other radio off).
    - The X6100's own spectrum climbing from about 27 kHz either side of the dial: normal for the model, not this
      unit. Its IQ is a Cirrus CS4272 audio codec at 100 kHz (Georgy Dyuldin, OpenX6100/x6100_gui#192; his base patch
      source), whose filter cuts only from 0.68 fs: what lies 50-68 kHz off the dial folds onto the edges. groups.io
      xiegu-x6100 topic 108431112 (2024-09): strong signals near +-50 kHz show on the opposite edge, on every band,
      with R1CBU's firmware too, confirmed by other owners; his proposal, hiding the outer 10-12 kHz. Base V1.1.9 does
      not change it. The Sentry's A shows the middle 50 kHz; the classic screen still shows all 100 kHz (hiding the
      edges there: declined by the owner 2026-11-19). The CS4272's ADC filter (datasheet): double speed, as now, flat
      to 0.45 Fs and stopped from 0.68 Fs, so only +-32 kHz is free of aliases; quad speed at 200 kHz (flat to
      0.24 Fs, stopped from 0.78 Fs) with a sharp /2 in the STM32 would give about +-45 kHz clean. Not as a patch to
      Xiegu's base: the TLV320's one frame clock runs all four streams (RX and TX I/Q, speaker, mic), each would need
      a /2 or x2 stage, both converters are 192 kHz parts, and the gain is the display's edges only. An open base
      (fventuri/x6100-bbfw) is where it would belong.
    - AIRSPACE about 2/3 of the X6200's messages: the antennas' places (the owner, 2026-11-19), not the radio.
    - The AM filter, 0-5000 Hz on the X6200 and 0-4000 on the X6100: not a default (both radios' code has 0-4000 for
      AM), a value kept on the X6200 (mode_params, mode 6; mode 1 too, 200-3200 against 100-2900). suite.py align and
      core's same-settings now take the per-mode filter edges too.
    - The classic screen in CW: the decoders' text panel lies over the waterfall; the picture themes draw it
      see-through, the flat ones (the Sentry's gold on black) filled it, so with the CW decoder on there was no
      waterfall. See-through (60 %) in every theme now, solid in High contrast (gui_styles.c message_colours).
    - The S-meters cannot be compared between the two (the X6200's base meter floors at -121 dBm in coarse steps,
      the X6100's is summed from its IQ).
    - The X6100's WiFi crawls (10-30 KB/s) on the farther access point (channel 6, -64 dBm): where it stands.
    - Devices, 2026-11-19 (dmesg and device tree on both): the same SoC (R16), board tree, codec, panel, keys,
      encoders but one (the X6200's rotary2, its MFK inner ring), PMIC, RTC, watchdog and regulators. Different by
      design: I2C (the X6200 the SoC's controllers, the X6100 bit-banged GPIO), WiFi (the X6200 RTL8723DU on rtw88 on
      the EHCI; the X6100 RTL8723BU on Realtek's own driver on the MUSB, the RTL-SDR on the EHCI), eMMC 4 against 8 GB.
      Missing on the X6100 until its kernel set comes with the update (v0.3.12.29 on): the video decoder (Cedrus,
      /dev/video0), the CPU temperature sensor (sun4i-gpadc: no thermal zone) and iio-hwmon.
    - Done: the Sentry, its pages, AIRSPACE and the theme on the X6100; the base flasher's X6100 path checked
      against Xiegu's own app, base V1.1.9 written; the Sentry holding the base at its widest (166f3f08); every app
      and page photographed on both and published redacted (docs/parity/2026-11-18); settings aligned (undo kept).
    - The suite: 2026-11-19 03:37 core,rtl,update,cable on both radios' new builds, the per-mode filters aligned
      first: 62 pass, 0 fail, 2 to look at (RADE needs Bluetooth headphones or TCI; AIRSPACE goes by its picture);
      0 settings differ; both offered v0.3.12.30, "card image needed" on both; FT8 both ways at -70/-72 dBm. Field
      Ops, the one screen with colour left (0.4-0.6 % cool: its tiles' pictures), is tinted gold in the Sentry theme
      now: 0.0 %. Both
      radios run hot-installed GUIs: the release itself, through the updater on each, is the test still to do (the
      owner's YES on each radio). While the suite runs each radio shows a banner: the test, which radio sends and
      which listens, with the radio's own TX or RX in front (bench_banner.c, "banner TEXT").

**Waiting on hardware**

- The bench harness (tools/bench, 2026-11-18: the owner, "YOU will be running the automated tests"): bench.py with
  the rules in code (README.md). Tried: a receive-only remote session on the X6200, status, the preflight refusing
  without the owner's word on the cable; then, with it (02:27 local), `hackrf-ft8` on 20 m: refused first because
  the FT8 app is on Auto (allowed now on a radio with no callsign, which sends nothing), then PASS at -x 30 (SNR
  9 dB, 1447 Hz; the S-meter -121 -> -108 dBm) and decoded at -x 40 (-100 dBm), the radio back in the Sentry each
  time. `radios-ft8` ran 2026-11-18 evening, the X6100 and the X6200 through 40 + 30 dB at 0.1 W: PASS both ways.
  Its logic tried against two simulated radios (tools/bench/sim_test.py, a host check now): a CQ and its reply
  pass; a radio that stays keyed, an SWR of 3, 5 W for 0.1 asked, and a receiver hearing +10 dBm (pads missing)
  each stop everything and refuse.
- Done 2026-11-18: the bladeRF dropping off USB as it started (00:05 local; its own supply plugged in, the owner
  said) not reproduced on its DC jack. Plugged in again at 00:14 it opened on DC (now logged at each open), and at
  05:57 UTC the radio loaded its FPGA image again (2.0 s, 0.53 A after it), opened it so its AD9361 started (1.8 s,
  0.66 A) and received 5 s at 100 % of 2.646 MS/s (0.69 A), on USB throughout (a test program, B's daemon held off
  for it). The start the first plug died in works on DC. Its 0.66-0.69 A at 5 V is more than USB 2.0's 0.5 A, so
  the first plug fits the board having been on the HOST port's power at that moment (DC plugged after USB, say). If
  it drops again, rtlspec's log says which power it opened on; B keeps "DROPPED OFF USB AS IT STARTED".
- Done 2026-11-18: the bladeRF's filter at 2.646 MS/s is 2.3814 MHz (9/10 of the rate; 1.905 at 2.1168), 99 % of the
  samples (5.29 MB/s). The HackRF's done 2026-11-17 night: 2.646 MS/s at 99 % (5.29 MB/s), its baseband filter 2.5
  MHz there (1.75 at 2.1168), B's picture 2.2 MHz wide, heard, RDS PI on 92.3; B took it by itself when the RTL was
  swapped for it (bd5592a).

**Parked or later**

- Closed by the owner 2026-11-19 ("we literally don't care"): Meteor off the air on a real antenna, WSPR off the air,
  and the board variant check for Franco's base firmware or CESSB; the GLO 2's outdoor fix is confirmed.
- Meteor-M LRPT outdoors (closed 2026-11-19, kept for the record; deferred by the owner 2026-11-17): decoded from a real 2019 Meteor-M2-2 recording played
  into the stick over a cable (a255320, c5e864b, d02958f); the first live pass (Meteor-M2-4) gave nothing on the
  antenna used. A pass on a 137 MHz antenna outdoors (QFH or V-dipole) is the test; the 80k mode is unverified.
- Screenshots of every menu for the README and the manual (deferred, the owner 2026-11-18): at the owner's word.
  Every app and Sentry page of both radios is in docs/parity/2026-11-18 (redacted); the classic screen, the Modes
  menu, RADE and Settings' own pages are not.
- WSPR (closed 2026-11-19; deferred by the owner 2026-11-17): never tested. Off the air is the real test (the X6200 on its antenna, 14.0956
  USB by day, 7.0386 by night: spots every two minutes). Over a cable it needs a steadier source than the HackRF: a
  bladeRF 2.0's VCTCXO, but it tunes from 47 MHz, so 6 m (50.293) only.

- Passive radar with the KrakenSDR (KrakenRF's krakensdr_pr, a fork at github.com/mfkiwl/krakensdr_pr; the older
  pyapril had the clutter cancellation and cross-ambiguity): back burner, till the owner has a use for it. With the IQ
  server in Heimdall's format, krakensdr_pr on a computer could take the radio's samples.
- P25 phase 2: its voice is scrambled with the system's WACN, SysID and colour code, so it needs trunk following
  first; trunk following would be a feature of its own.
- DMR transmit on G: later, with the owner's own DMR radio and ID, HackRF/bladeRF only.
- Iridium (ACARS over Iridium): looked at only (gr-iridium, iridium-toolkit); not to be built.
- Upstream #88 is the fork's static Wi-Fi (#17, 0ac3b6d): upstream not told yet, at the owner's word.

**Done 2026-11-16 and 2026-11-17** (for the record; details in git)

- x6x00-df's IQ server in Heimdall's format (ports 5000 and 5001; x6x00-df e1b5d2f): 25 kHz round the target at
  64 kS/s, calibrated, typed frames; FREQ and GAIN from a program, page I and the web page taking its tuning. Tried
  from a computer with a client written as KrakenRF's receiver (2.45 frames a second over WiFi, FREQ retuned); not
  yet with KrakenRF's own software.
- The web page's bearing no longer blinks: it held by the state (twice a second) while bearings come ten times a
  second; now by each bearing's own age, fading over 30 s with HOLD n s.
- A Direction finder page in the web configurator (Remote > Direction finder, /df): page I's compass, bearing,
  band, channels, tuning (a frequency typed, a click on the spectrum, FOLLOW) and every setting, the sources and
  forward-backward included, and gpsd; it runs DF on its own while open (page I closed) and lets the Kraken go 20 s
  after the page closes. Through the GUI's configurator socket (df_get, df_set, df_run), so page I and the page share
  one set of settings. Tried on the radio through its API (DF started from it, a setting set and refused); the page
  drawn in a browser from the radio's own data.
- Kraken roadmap, first pieces (x6x00-df 75f2868): its own cores (the musb interrupt off CPU0, readers on CPU2 at
  SCHED_FIFO 10, the engine on CPU3); gpsd's compass or course as page I's heading (tried with a made-up GPS on the
  radio's gpsd: north up, GPS COURSE 135); TNA; several sources (2, 3, AUTO) and forward-backward averaging; page I's
  F1 TUNE with FOLLOW A or B (tried: page I took A's 106.6 MHz by itself) and the BAND keys on the band presets.
  Page I ran 31 minutes with all five channels without a slip.
- JTTY on E DECODE (95cab4e), on WSJT-X's frequencies.
- Page I with all five channels for 31 minutes on 9104: no slips, 21 calibrations, the daemon up throughout (the one
  slip of the first over-the-air run did not come back).
- Page I over the air with five channels (2026-11-17, 9104, a 915 MHz ISM carrier from the HackRF on an ANT500,
  5 s on and 3 s off): all five calibrated and in the bearing at +21 to +31 dB, TRACKING 054.3-057.3 degrees +-1.0
  (fit 73-83 %), HOLD in the gaps; HOLD now keeps the last bearing with a fit worth holding (it once showed 198.7
  degrees, an estimate across a burst's end). Screenshots in radio_captures/kraken_df. The absolute bearing waits for
  the array's real layout (D8).
- The KrakenSDR's fifth stream (D11): kernel patch 9104 (08957fb) asks the Kraken's hub about its ports over ep0
  once a second instead of keeping one of the R16 musb's receive endpoints for its status, so each of its five
  sticks has an endpoint of its own; five streamed whole for 20 minutes. The bench radio boots with it (the test
  setup undone 17:44 UTC 11-17, the HOST port up by itself, all five sticks there). 9103 (ep1 in turns) withdrawn.
- C LOG's Listen choice seen on the radio (00e4d5a): an FM entry both radios take asks "F1 ALL, F2 THE X6200 ONLY,
  F3 THE RTL ONLY, F5 BACK"; BACK tunes nothing (radio_captures/sentry/c_log_listen_choice.png).
- The SDR daemon resets a stuck stick's hub port only while no other device on its hub is in use: on the Kraken's
  hub one port's reset took another down (tried on the radio with a second stick streaming: not reset, the other at
  full rate; alone: reset, back in 2 s).
- Page I's compass dimmed while nothing is over the squelch (0e14fe9). A build from a tree without git takes its
  commit from build-commit.txt, as the image build writes it: a copied tree needs that file, not a change.
- The X6200's base switched the radio off about three minutes after the GUI was stopped: x6200-base-keeper feeds it
  the GUI's last register block, TX off, while no GUI runs (16a2dec; the GUI stopped 8 minutes on the radio, up).
- An update checks the root filesystem's room before it changes anything (8a92627).
- SCAN CFG in plain styles (BROWSE, SCANNER, SAMPLER, STOP & STAY, SURVEY, CUSTOM) over three plain settings, A
  and B alike (B stops on a signal too), with what each scan is doing on the screen.
- D BANDS and E DECODE in sync: one rule for which radio receives what (HF and 6 m the X6200, VHF/UHF the SDR), the
  preamp, attenuator, AGC and SDR gain each choice sets shown on both, who may transmit, E with every receiving
  function of both radios (21 types), VOL naming its radio.
- Digital voice on B through dsd-fme and the DVSI chip (no mbelib): DMR, D-STAR, P25 phase 1, YSF, NXDN48 (three
  dsd-fme), each sent over a cable and heard.
- Cable tests on the X6200 (a HackRF through 70 dB of attenuators, receive only): WeFax with a grey ramp, Time Signal
  (WWV read two minutes in a row), RADE. The other HF decoders were tested over the air on 2026-11-12 and not again.
- Decoder apps open the squelch while open and give it back, also after a restart or power-off (0fd98b8, fd0b59c).
- WeFax drawn in the theme's colours on screen, the saved PNGs black on white (859dffc).
- The Mac cross build links RADE (4284ffc); before that a GUI built there had no RADE decoder.
- ACARS on AIRSPACE, network rigctld, the CI host checks green, the GUI's CPU cut by a third on the Sentry.
- The image builds dsd-fme (with our patch, against the DVSI shim) for both radios and the update bundle carries
  it (seen in the 12065ef build). The image's SDR daemon had no lrpt.c and no zlib: both builds now take one source
  list, tools/rtlspec/daemon_sources.

## Waterfall: the 3D band scope on the radios (PR #161, merged)

Merged on main 5 November (Calin Rigo, timerealm, PR #161, squashed): Settings > Display > WATERFALL > 3D, all
drawing in src/waterfall_view.c (its "3D" section), for the DRM plane and the LVGL image. On by default since 5
November (the owner's call): a radio that never switched it starts in 3D, one switched off keeps its stored 0; under
the Amber Trey display it stays off. Run on the PC build only: d3_render() 0.18-0.65 ms a picture there (our timing, his
~175 us), the GUI thread from 46 % to 84-88 % of a PC core with 3D on, most of it LVGL drawing the picture, as the PC
has no DRM planes. The radios take the DRM path, which nobody has run yet; drm_wf_present_ring() then runs up to 30
times a second instead of 15. 1.5 MB for the background, allocated the first time 3D is on.

- **Before the next release, on an X6100 and an X6200, 3D on and off**: the DRM path (the picture the right way round,
  the labels at the front edge, the 2D waterfall back whole after 3D, a theme change and a new spectrum height with 3D
  on); CPU with `top -d 1 -p $(pidof x6100_gui)` in 2D and 3D receiving on a busy band, the dial turning fast, depth
  64, FT8 open, 3D glide on and off; FT8 decodes and CW keying with 3D on; the 1bit theme and the MFK 4:4 page; Hide
  buttons on (the front edge down to the bottom while hidden); walkie mode and the screen dark draw nothing (read from
  the code only), a remote viewer still gets the picture. If 30 pictures a second are heavy on the X6100, 3D glide off
  is the step at each row. He has no X6100; his `top` numbers from an X6200 may come first. As it is on by default: an
  update from v0.3.8 starts in 3D, and a radio with 3D switched off stays flat after the next update. So the release
  waits for this check on both radios. If the DRM path draws wrongly, takes the CPU FT8 needs, or the GUI falls over
  in 3D, the default goes back to 0 for that release: a GUI that does not stay up cannot switch it off (the web
  Display page goes through the GUI's socket), only wf_3d = 0 in params.db on the card would.
- **The X6200, checked 2026-11-15** (v0.3.12.23, the Mali-400 drawing the classic screen's 3D): the picture the right
  way round, the labels on the front edge, the 2D waterfall back whole after 3D and 3D after it. The GUI's CPU
  (`top`, all four cores): 3D 3.4-3.6 %, 2D 1.6 %, the Sentry's A 5 %. The X6100 (no GPU path) is still to check.
- **After that**: 3D shots for the README gallery from the radio, on an antenna. The front labels sit under the Knob
  info lines at the bottom left (seen on the PC build, zoomed in).

## Screensaver: Matrix rain on the radios

Settings > Display > BACKLIGHT, Screensaver and Screensaver (min) (src/screen_saver.c, the times src/screen_idle.c):
the rain of the VFO-Knob's screensaver after 5 minutes without use, never dark until Screen off is set; a radio whose
Screen off was changed keeps its moment (migrations.c _16). An LVGL object over the screen, the GUI under it not
drawn, the waterfall, 3D and the spectrum paused. Run on the PC build only: 14 pictures a second, ~90 000 pixels and
~40 areas each, 0.18 ms a picture of LVGL's own work there (lv_timer_handler with the screensaver up, less the SDL
flush and the dark screen's), so some 4 to 8 % of an A7 core by the usual factor of 15 to 25, plus the DRM copies
(MAX_RECTS raised to 128 for its two areas a column).

- **On an X6100 and an X6200**: `top -d 1 -p $(pidof x6100_gui)` with the screensaver up (Screensaver (min) 1), lit
  in 3D and dark, receiving with FT8 open; the rain on the panel (the overlay plane the right way round, no GUI
  showing through, the top layer's messages hidden, every glyph whole: tools/screen_saver draws it the radios' way
  off the radio), RF.Guru, 1bit and Amber Trey; the first key, a knob turn (all of it) and a hand mic key only bring
  the screen back; FT8 auto's TX and an SWR scan end it at once and none comes while on the air; saver then dark,
  then a key: the GUI at once, not the rain, the waterfall and the spectrum current; POWER on it, a theme change
  from the web page and the walkie mode under it; a radio updated with Screen off 3 goes dark at 3 with the rain
  from 2, on FT8 auto too. If the X6100 finds it heavy: a slower rain or fewer columns (the work follows the cells
  the columns move over a second, hardly the pictures a second).
- **The X6200, checked 2026-11-15**: Screensaver (min) 1 on the classic screen, the rain up after a minute in the
  theme's colour, every glyph whole and the right way round, the GUI's CPU 3.5 % of all four cores (the same as the
  3D waterfall it covers); an MFK press only brought the screen back (the MFK's mode unchanged), the 3D current. The
  X6100 is still to check.

## X6200: the short pulse at the start of each transmission, register 32 (#53)

Seen 5 November on an external transient analyser: the X6200 sends a narrow RF pulse at the start of each
transmission, well above the steady level at low power; another radio is clean. Why is not known. On main
(unreleased): at each start the X6200 sets its base's register 32 to 0x0a, as Xiegu's firmware does (the X6200
control library's copy of the stock app's writes, commented out in its low/control.c: 0, then 0x0a, between the CW
keyer's register and the QSK time's); before, it stayed at the library's 0. What the register does is not known, a
TX timing value maybe. Host test tools/hw_x6200: the real hw_x6200.c and control library on a base of the test's.
- **Next**: the same captures with this build (the GUI log says "base: register 32 = 0x0a, as Xiegu's firmware" at
  the start), at low power and at 8 W, SSB and CW: the pulse gone, smaller, or as it was. As it was: register 32 is
  not the cause, and it stays at Xiegu's value.

## FT8/FT4/FT2: answer other stations' CQs by themselves (when not worked yet)

Asked 2026-10-24. Today Auto answers the stations that call *you*, also while you call CQ (what
other programs call "Res"), and sends the next message of a QSO by itself; it never answers
somebody else's CQ (docs/MANUAL.md, FT8 section, "Auto"). The wish: an automatic hunt mode that
answers a CQ it decodes when that station is not in the log yet for this band and mode.

- **Worked before, with a time window**: "not in the log" counts QSOs within a window the
  operator chooses: this band and mode ever, this year, the last month, the last week, today.
  The log already answers worked / worked in this mode for a call and band
  (`qso_log_search_worked()` in src/qso_log.c, used for the list's colours); it needs a date
  limit (`QSO_DATE`/`TIME_ON` in the qso_log table) and the window as a setting.
- **Which CQs**: honour the CQ's modifier (`CQ DX` from the own continent, `CQ NA`, `CQ POTA`...
  only when it applies), skip stations already being worked, prefer the strongest or a POTA/
  SOTA/TOTA... activator (the cluster's activity and the FT8 POTA tag), never answer while a QSO
  of our own is going on, keep Hold Freq and TX Call as they are.
- **Safety**: an explicit switch, off at every start (the radio transmits by itself), visible on
  the screen and in the browser panel; a stop after N answers without a reply or after a time;
  the same retries as a normal QSO; logged as usual.
- **Other digital modes**: FT4 and FT2 share the FT8 app and come with it. JS8 deliberately
  never transmits to the group by itself (MANUAL, JS8 section): an auto answer there would be a
  separate decision. RTTY/PSK are keyboard modes and WSPR a beacon: not candidates.
- **Gatherer first** (#156, asked 2026-11-06 as the priority): CQ again by itself once a QSO is logged and the RR73
  or 73 has gone out, while nobody else calls; the same switch, time limit and stop on any key or knob as above.
  The smaller half, so it can come before the hunt.

## X6100: the patched base (gdyuldin/x6100_base_patch r13) on radios that came with base V1.1.8

Tested 2026-10-24 on RF.Guru's bench X6100 (upstream, Joeri Van Dooren; shipped with stock base V1.1.8; not the
owner's X6100, which came 2026-11-18), then put back on stock V1.1.8,
where it stays for now. Backups, test tools and full measurements: ../x6100-baseband-backup/ on the
build PC (tx-test-2026-10-24.txt, i2c-baseline-2026-10-24.txt) and /mnt/baseband/ on the radio.

- **Flashing from OpenX6X00 works**, no SD card swap: fventuri/x6100-base-experiments
  `base-flasher/x6100_base_flash.py --gpio-backend sysfs --no-verify-addr` (our libgpiod CLI is v1, the
  flasher's cli backend expects v2), GUI stopped first, external power. r13 and back to stock each
  about a minute. The base cannot be read back: Xiegu's official X6100_BBFW_V1.1.8_240915003.xgf
  (same build string) is the way back.
- **What r13 changes here** (the radio's own meter, FM carrier into a dummy load):
  - TX level about 10 % high at 5 W and 12 W or more at the 10 W setting before calibration (stock: 4.9 /
    10.9 W, ALC holding it); "TX codec gain" alone does not fix it, the bands drift apart, per-band
    "Band output gain" does (160, 40, 20, 15, 10 m calibrated within -1.3..+1.2 dB at codec -3 dB;
    partial values in /mnt/baseband/r13-partial-calibration.txt).
  - **The first carrier after a band change sometimes has next to no power** (6 m nearly always; 17, 80,
    30, 12 m now and then); the second is fine. Never seen on stock in 40 tries. This blocks a full
    calibration and would send a first over on a new band with no power.
  - External supply voltage reported as 0 V (the GUI then shows a battery runtime instead of external
    power); whether charging still works on r13 is untested.
  - The calibration window over I2C (0xF000) reads zeros on r13; the stock calibration came back intact.
  - Linux-side I2C to the base: no errors (1000 reads), RTC fine; RX not compared (no antenna on the
    bench).
- **2026-11-20, studied again** (tools/base_update/README.md "Georgy Dyuldin's patched base"): r13 is still built on
  V1.1.6 only, and 1.1.6 code on V1.1.8-generation boards misreads the battery and supply and loses power on band
  changes (#3, and RF.Guru's test below on their bench X6100, not the owner's): not for this X6100. The 6 m floor (0.7-0.9 W at 0.1 W) is Xiegu's ALC-only power
  control, which r1's true power control fixes. The way for this radio: that power control as a hook on V1.1.9,
  next to RF.Guru's GPL hooks (Guru-RF/X6100-Baseband), written new (gdyuldin's code has no licence), delivered by
  base_flash (it writes X6100 bases; the radio's screen and fetch are the X6200's only, the owner's choice of
  2026-11-13). Waits for the owner's go.
- **To look at later**: whether the band-change dropout is the patch's band/PA switching on this board
  revision (issue #3 reports I2C trouble on V1.1.8 radios, base-side), a TX hold-off after a band change
  in the GUI as a workaround, the external-voltage report, and Xiegu's stock V1.1.9 base (found unflashed
  on the radio's eMMC: OpenX6X00 already enables zoom and IF shift for it) as the low-risk alternative.

## CESSB in the base firmware, X6100 and X6200 (later: once there is a stable build)

Asked 2026-10-25 (CESSB first, the other TX options when they come for free); the owner, 5 November: CESSB is only
possible in the base's firmware, so this waits until OpenX6X00 has a stable build. CESSB (controlled-envelope SSB,
W9GR) gives SSB more average power without splatter: the SSB envelope is clipped and the overshoot the filtering
brings back is controlled. Today only gdyuldin's patched X6100 base has it (x6100_base_patch rev 12 and up, built on
Xiegu's base V1.1.6; `HW_CAP_CESSB`, Settings > TX CAL), and r13 is not usable on V1.1.8 radios (the section above).
Xiegu's newest bases, X6100 V1.1.9 and X6200 1.0.8, have no CESSB, and the X6200's no TX filter either
(src/hw/x6200/hw_x6200.c, `hw_caps()`).

- **Not on the Linux side.** The SSB signal is made in the base: its SSB filter, ALC and PA come after anything
  the GUI could do to the audio, so an envelope controlled on the audio would not stay controlled (the filtering
  after the clipper brings the overshoot back), and the GUI cannot see or steer those stages: the base doesn't
  expose them. The idea of a CESSB chain on the voice path (the mic recorded by Linux and played into the
  transmitter as voice messages are) is dropped for that reason; the same goes for a wider SSB bandwidth (#69).
- **The way**: see whether the bases' firmware can be reverse-engineered, as gdyuldin did for the X6100 from V1.1.6,
  and CESSB added for both radios: X6100 V1.1.8/V1.1.9 and the X6200's 1.0.8. Reverse engineering for
  interoperability is allowed in the EU. Start from gdyuldin's patch (what it hooks, how the GUI controls it),
  then the X6200 base, which is another firmware.
- **With it, maybe**: r13's other TX options on the stock bases: a TX filter (the X6200 has none), the compressor's
  shape, FM pre-emphasis, a TX equaliser.
- **Before any image carries a patched base**: how it is flashed and rolled back safely (flashing a base needs the
  owner's go), and that a radio without it keeps working as today. The flashing half is there for the X6200:
  tools/base_update writes any of Xiegu's .xgf files from OpenX6X00 as Xiegu's own UPGRADE screen does, from the
  radio's screen or a computer (on the bench radio 2026-11-13: 1.0.7 to 1.0.8, back to 1.0.7, to 1.0.8 again; each
  time the base switched the radio off about three minutes after the write). A patched base has to be
  encrypted for the bootloader with Xiegu's key: AES-256-ECB, no padding, the key in fventuri/x6100-bbfw
  common/build_common.sh (MIT; `openssl enc -d -aes-256-ecb -nopad -K KEY` reads an .xgf, the same without
  `-d` makes one).
- **Others' base work, read 2026-11-19** (the owner's links; tools/base_update/README.md has the details):
  strijar/x6100-base (R1CBU) maps Xiegu's X6100 base V1.1.9, the build this radio runs, from Ghidra (no code, no
  licence: read, not copied); fventuri/x6100-base branch WIP-franco adds bench-checked pins, the libraries
  identified and a dump of the IAP bootloader; fventuri/x6100-bbfw (MIT) is a clean-room base built on them:
  receive only so far (AM, SSB, FT8, the panadapter), no TX, charging, attenuator/preamp or filter edges, and
  not the PCA9535 board variant. Georgy Dyuldin's method (groups.io xiegu-x6100 topic 112186023, April 2025):
  code built for the STM32 placed past the end of Xiegu's image with jumps into it, his addresses for 1.1.8.
  On 1.1.9 the TX chain is: /8 with a 64-tap FIR, 300/3000 Hz biquads at 12.5 kHz, a 101-tap Hilbert,
  x8 interpolation, a block ALC (one gain per 128 samples on I and Q), Q31 out. CESSB would go after the
  Hilbert and before the interpolator, where the block ALC does not undo it. So the nearer way for this radio is a
  Dyuldin-style patch on 1.1.9 from WIP-franco's map (Xiegu's TX, ATU and charger kept); bbfw is the long-term open
  base once it transmits and charges. First: which board variant this X6100 is (PCA9535 or direct GPIO for the band
  switching; CS4272 or another converter).

## FreeDV: RADE and the Codec 2 modes (#37, #52)

Asked 2026-10-27 (#37; #52 asked the same and was closed for it). FreeDV's RADE (https://github.com/freedv/rade_c,
with the FARGAN vocoder model from the Opus project) as a digital voice mode in APP, and FreeDV's older Codec 2
modes (1600, 700D) with it. No date.

- **In (experimental), from PR #130** (jochenhammes, 2026-10-31), finished by us 2026-11-12 for both radios: RADE V1
  as APP > Modes > RADE, receive and send as record-then-send, like a voice message, through `tx_player_stream()` at
  RADE's own power (`rade_pwr`, 0.5-3 W). The speech goes to Bluetooth headphones and to the TCI clients (the
  VFO-Knob, the browser's Panel and Remote pages: `tci_server_speech_write()` in place of the RX audio); the app opens
  only with one of them there. Built in the image and the bundle by tools/rade/build.sh from local-build.sh, every
  download pinned. Decoded on an X6200 over a cable (2026-11-17): overs from the radio's own RADE TX code, sent by a
  HackRF on 14.236 MHz through 70 dB of attenuators: sync 0.2-0.3 s into each over, 15-16 dB SNR, the End-of-Over
  callsign, every over whole (docs/MANUAL.md, RADE). Still open: a real over off the air, the X6100 and its CPU, live
  speech (needs the mic in Linux while transmitting, as below), RADE V2 and the Codec 2 modes.
- **The audio path is there**: RADE is a modem in an SSB passband, and the digital apps (FT8, Winlink) already take
  the receiver's audio into Linux and transmit audio from Linux (`radio_set_modem()`). What is new: the decoded
  voice on the speaker while receiving, and the mic recorded in Linux while transmitting (`AUDIO_PLAY_VOICE_REC`),
  the same path the CESSB entry above needs.
- **CPU and memory are the question**: fventuri's multithreaded radae-nopy decodes in real time on an X6100, using
  both cores (https://github.com/fventuri/x6100-buildroot, board/X6100/patches/radae-nopy). The encoder is the
  heavy part (strijar, on a dual-core ARM), and transmitting needs it next to the GUI and the spectrum. RADE's
  developers have efficiency work coming (peterbmarks, 2026-10-30): measure again when it is out. The FARGAN
  model's memory on 1 GB.
- **Not counted on**: fventuri's base firmware that streams audio and IQ over USART6 to Linux and back is a base
  of its own, so a card image cannot rely on it.
- **Codec 2 1600 and 700D** need far less CPU (they run on a microcontroller): a first step that
  needs only the audio path.
- **Reporting**: FreeDV Reporter (https://qso.freedv.org), #37: Reporter in the RADE app (off by default),
  rootfs/usr/sbin/x6100-freedv-reporter as role report_wo. To see on the air: whether a QSY request reaches a
  report_wo client (FreeDV's radio integrations connect that way; the server's code is not public), and the
  callsigns FreeDV's own program sends once the receiver reads them.

## Winlink over HF: Pat with ARDOP on the radio, Mercury and XARA (#18, #19, #43)

Asked 2026-10-26 (issues #18 and #19). Winlink email from the radio alone, with a phone on its WiFi to read and
write it: Pat (getpat.io, Go, MIT) as the client, ARDOP as the HF modem.

- **In v0.3.6**: Pat v1.0.0 built with a pinned Go (tools/pat, a few patches: its page answers
  only to the radio's own names and refuses other sites' requests, no release or password-recovery check) and
  ardopcf (github.com/pflarue/ardop, MIT, C) in third-party/ardopcf, both in the image and the bundle. ardopcf's
  audio and PTT go through the GUI (src/winlink.c: a socket, 12 kHz, TX through tx_player), which also answers
  Pat as a rigctld on 127.0.0.1:4532: it tunes and sets USB-D. A supervisor (x6100-winlink, S98winlink) runs both
  while Winlink is on; the web configurator's Operating > Winlink sets the callsign and locator (the radio's by
  default), the Winlink password (root filesystem, 0600, never shown, not in the Backup) and the ARDOP bandwidth,
  and links Pat's mailbox on port 8080 ([manual](docs/MANUAL.md#winlink)).
- **On the air, reported** (#18, #80): sessions with RMS gateways work (KM4GKD X6100 80 m, M7SVA 40 m UK to
  DM6TS, a message each; EA5JTT's X6200 reached HB9MM and stopped at the unset password). M7SVA: the gateway
  list's 2000MAX did not connect, 200 did: the page's bandwidth now caps the list's (Pat patch 0003; 200, 500,
  1000, 2000 Hz, 500 the default).
- **In v0.3.7** (#43): the radio's state before a connection (/run/winlink/radio from the GUI, the
  page's Before you connect card), the outcomes kept apart (service, modem, session, last connection with what to
  do next, also in the mailbox's log through Pat patch 0004), no key while a digital app is open, a refused QSY
  said, the SWR hold's minutes, where the password is set, the logs as a download, Funnel refused by Pat.
- **Still to do on the bench / air**: the acceptance write-up for both radios (ARQ at 200, 500 and 2000;
  turnaround; per-frame power and ALC with the shared level steady; split, a dial turned, the 30 s key limit and
  an SWR trip at low power into a mismatch; CPU). A linearity check of ARDOP's multi-carrier frames
  (TWOTONETEST, TXFRAME 4PSK.1000.100 and 16QAM.2000.100 into the dummy load) decides whether ARDOP needs a drive
  below the FT8-learned level and whether to start ardopcf with `-H "FASTSTART FALSE"`. P2P listen stays off
  (Pat's own setting is undone at each start) until it has its own refusal rules and an on-air P2P test.
  ardopcf's maintainer has stepped back (its README asks for forks), so fixes may have to be ours.
- **Mercury** (github.com/Rhizomatica/mercury, GPL-3.0-or-later): faster OFDM with a VARA-style TCP interface,
  which Pat's varahf transport talks to. Some Winlink RMS gateways do run it, behind RMS Trimode's or BPQ32's
  VARA driver: KO0OOO (its banner gives 7.101.5) and K7EK-10, seen in Mercury's issues (#162, #384, #442).
  Mercury's versions connect in groups (its docs/RELEASE-1.9.25.md and discussion #637): 1.9.25 to 1.9.25 only,
  1.9.20-1.9.24 to one another, 1.9.19 and 1.9.18 each to itself, 1.9.17 and older to one another. VARA itself is
  closed and Windows-only: out.
  - **v1.9.25 vendored** (#328, 2026-11-22; v1.9.21 from v0.3.7 to v0.3.13): it does not connect to 1.9.24 or
    older in either direction, so the radio follows the gateways. Mercury's own level kept as before
    (tx_gain_db = -4: 1.9.25 alone is 4 dB louder), a stop with a session up ends it on the air first (Mercury's
    DISCONNECT through the bridge, 42 s at most, the status saying so; at once with none), an answer from
    1.9.24 or older told as that, the Filter row's spans (375-2625 Hz at 2300, 125-2875 Hz at 2750). Tested on
    the PC only (make modems, two 1.9.25 stations): **not on a radio and not on the air yet**; open there: the
    processor load (more modes, two MFSK call listeners), the drive and ALC at the kept level, a session with a
    gateway on 1.9.25, and the goodbye at a stop as the radio keys it.
  - **Built, experimental, chosen on the Winlink page** (v0.3.7): Mercury vendored as its daemon build
    (third-party/mercury, two local patches: the TNC and broadcast ports on 127.0.0.1, VERSION answered with a
    VERSION line that names Mercury and its version), linked into x6x00-mercury, which takes the audio and the PTT through the
    GUI's modem socket in its protocol 2 (8000 Hz, keys of up to 40 s; ardopcf stays on protocol 1). In the image
    and the bundle. Operating > Winlink's Modem (ARDOP or Mercury, one at a time) and Mercury's bandwidth (500,
    2300, 2750); x6100-winlink runs x6x00-mercury in place of ardopcf with /run/winlink/mercury.ini, Pat's varahf
    on 127.0.0.1:8300 (P2P listening off), Mercury's readiness and outcomes, its decoders falling behind ("decoder
    ring full", "RX backlog exceeded") said on the page and in Pat's log ([manual](docs/MANUAL.md#mercury-experimental)). tools/winlink
    tests it on the PC: Pat to Pat peer to peer over two stubbed radios (make modems), the supervisor's and the
    page's side (winlink_test.py, winlink_web_test.py).
  - **Go/no-go on the bench X6100, 2026-10-29** (dummy load): go, as experimental. With `top -H` the radio at
    38.6 % of its four cores with Mercury listening (17.6 % with ARDOP), 51 % at most in calls, no "decoder ring
    full" or "RX backlog exceeded" in mercury.log; keys of up to 13.5 s at 5.7-6.3 W on average, 7.0 W at most,
    the shared digital level from -23.58 to -22.08 dB. The X6100 has no temperature sensor.
  - **Next**: the same on the bench X6200; a 30-40 s key at up to 7 W, PTT to RF and unkey to RX audio, the
    drive level loop on OFDM (QAM16C2 needs a linear chain). Then on the air: P2P with a Mercury station, then a
    gateway (KO0OOO, K7EK-10) once the Winlink account works. Upstream reports to post: the VERSION reply (Pat's
    start race), the daemon build's two missing Android hooks, an option for the TNC ports' address, nanorq's and
    oblas's licence files, PUBLIC in TNC.md.
- **XARA** (x0rro's, MIT, a VARA-compatible modem in Rust, discontinued by him on 8 November; NR5P's fork
  github.com/NR5P/xara since, the owner's decision of 9 November; #120): built, experimental, the third Winlink
  modem (branch gh44-xara, 2026-11-04; the owner's decision of 4 November), in v0.3.9 as x0rro's v0.1.3 from its
  release tarballs. Since branch gh54-xara (2026-11-09) NR5P's main bd8300a, from GitHub's tarball of the commit and
  crates.io, with a pinned Rust 1.99.1 (tools/xara, its UPSTREAM.md): 0001 NR5P's port of x0rro's 0.1.3 modem core
  (its branch upstream-0.1.3-fixes, not merged), 0002 0.1.3's resampler, our 0003 for the GUI's modem socket
  (--audio x6x00, --ptt x6x00), 0004 for the CW ID with any callsign when Pat asks (CWID ON; the owner's decision
  of 4 November: XARA identifies only with a US call) and 0005, a call Pat cancels given up at once, its ID on the
  call's frequency before Pat tunes back. VARA's free speed by default; its `registered` feature is
  compiled in and used only with the Winlink page's VARA HF licence tick box, the user's own statement, which nothing
  checks. Tested between two stations on a PC (0.1.3: make modems, eight runs: free, levels 2 to 4; ticked, the
  highest level 9 to 14; NR5P's: without Pat only, free level 4, ticked up to 11; make modems owed, it needs a Pat
  for the PC); on the air with 0.1.3 (km4gkd, #120).
  - **Bench** (the owner; no air yet): `top -H` on the X6100 and the X6200 with XARA listening and in calls (48 kHz
    f64 DSP next to the GUI; "xara is falling behind real time" in xara.log). The audio path XARA times its
    replies by: path()'s 25/50/80 ms (capture, keying, out) are guesses; measure with XARA's `--record` (a FLAC
    with PTT) against the GUI's log and the dummy load, per radio, then set them (xara_path_ms= for the test, the
    patch's PATH_MS after). VARA calling XARA leaves about 150 ms of margin for XARA's answer. A GUI playback
    underrun (XARA's audio comes just in time, the stream's 60 ms prebuf is the only cushion) would cut a frame:
    listen for gaps in the dummy load's audio. P2P between the X6100 and the X6200 into dummy loads at 2300 Hz,
    free and ticked; a 30 s key; the PA's linearity on the full-speed OFDM levels (PAPR 8-9 dB) at up to 7 W and
    the shared digital level on XARA's frames (drive-db 0). The CW ID with an ON call at each session's end and after
    a call with no answer, heard whole on a receiver; the station that did not end the link keys about 3 s silent
    first, waiting out the other's ID: tx_level takes no readings in silence nor in the 0.75 s after it
    (tools/txlevel). Check that key on its own, in the GUI's log (/tmp/x6100_log.txt, then app_logs/): each key that
    moved the shared level has a "tx_player: level A -> B dB, N windows decided in M blocks, S of them silent" line.
    The ID-wait key (S about 58 to 77, 2.7 to 3.6 s) should have none, or N at most (M - S - 16) / 12 (about 3) and
    no more of a rise than the radio's own ID key at the end of a session it ends (S 0); before the fix about 7
    windows, 3 or 4 of them stepping up on the silence. The level before and after a whole session says nothing: on
    the X6100 every key's level is kept, and the session's data keys and the ID's audio move it too. A call
    cancelled in Pat (Dialing..., then the second click that forces it): the ID at once on the call's frequency,
    Pat's tune back after it (0005; make modems tunes half a second after ABORT). At many a key's start the first
    read waits up to 70 ms for an R the GUI no longer sends (as in 0.1.3; NR5P's pump says "audio loop slow while
    keyed"): listen for a gap at the start of a key in the dummy load's audio; Audio::keyed() could end that wait.
  - **Then on the air**: yo4cil against his own gateway (YO4CIL, VARA and ARDOP on 4 bands), free speed first; a
    test build announced on #120 first.
  - **Upstream**: 0004 and 0005 apply to NR5P's main as they are (not offered yet; 0004 was prepared for x0rro
    with a PR text); drop 0004 once NR5P acts on CWID ON, 0005 once a cancelled call gives up at once there, 0001
    once NR5P merges upstream-0.1.3-fixes, 0002 once its resampler is 0.1.2's.
  - **Watch**: NR5P's main (OpenSlot builds its modem from it too). x0rro's 0.1.4 ("libxara") will not come, and
    his open PRs for slow audio paths (#39 the extra answer lead, #40 the detector before a reply deadline, #38 the
    ACK margin, #37) went with his repository; any statement by EA5HVK (VARA's author).
- **Mail on the radio** (#294): APP > Mail reads, writes and sends Winlink mail with no phone or WiFi, through
  Pat's own API (rootfs/usr/sbin/x6100-winlink-mail; src/winlink_mail.c). Tested on the PC build against Pat 1.0.1
  (tools/winlink, make mail; the GUI driven headless): lists, reading, a message typed and sent into the outbox,
  the gateways, Pat's questions answered on the buttons, a connection that fails without a modem. Still to do: a
  session with a gateway on the air from the app, on the X6200 (the issue's reporter offers it) and the X6100;
  attachments and forms stay in the web mailbox, P2P is not offered (Pat listens for no P2P call on the radio).

## X6200: the H-22 hand mic's keys (#56)

Reported 2026-10-27 (X6200, base 1.0.7, the H-22 that comes with the radio). The GUI knows the X6100 mic's codes
only (the X6200 control library copied them from the X6100's; R1CBU's X6200 GUI notes list the hand mic as not
working). On the H-22 only F1, the step keys, 1, 2 and F-INP work; F2, F3, HOME, the volume keys, 3-9, 0 and the
dot do nothing. Every code the base reports is in the log now: `hand mic: key 0x.. (..)` (none while a text
window is open, which can hold a WiFi password).
- What the report shows: 3-9, 0 and the dot do nothing on the main screen and in the F-INP box alike, where 1 and 2
  work, and mic_keys.c takes every digit the same way. F2 tells nothing yet: it has no action until one is set in
  Settings > Keys (None by default). F3, HOME and the volume keys are not on the X6100's mic.
- The table, 2026-10-30 (ea4iln-debug, X6200 with v0.3.7 and base 1.0.8, next to what Xiegu's app 1.0.8 does with
  each code): 1 and 2 0xE1/0xE2, 3-9 0x18-0x1E, 0 0x17, * (the dot) 0x1F, # 0x28 (F-INP), F1/F2/F3 0x48/0x44/0x13,
  the arrows 0x84/0x04, HOME 0x24 (Xiegu's app: backspace, not a home frequency), VOL+/VOL- 0x14/0x15 (the RX
  volume +1/-1), A/B 0x16 (Xiegu's app: as XFC), MODE 0xE8, V/M 0x81. The side UP/DOWN keys send nothing (no line,
  short or held); Xiegu's app knows 0x12/0x11 as band up/down, never seen.
- Done in v0.3.8: the X6200 knows the H-22's own codes besides the X6100 mic's (no code in common), F3
  with its own actions (Settings > Keys, X6200 only), VOL+/VOL- the volume a step a press and repeating while held,
  A/B as the front A/B key, 0x12/0x11 as the BAND keys; the X6100 keeps its mic's table. Harness `mic-keys.c` walks
  both mics on both boards; manual: Getting started > Hand mic.
- After v0.3.8 (#145): PR #136 gave every digit on the X6200 the H-22's band, the X6100 mic's too (its 3 at 60 m).
  A key sent with one of the H-22's own codes is marked now and takes the H-22's print, the rest the X6100 mic's.
  This rests on the X6100 mic sending its own codes on the X6200, as the X6200 library's table has them: no log of
  it from a radio yet (on v0.3.8, which #145 reports right, 0xE4 and 0x18 both give 40 m).
- **Next**: on the bench X6200 with its H-22, on the main screen, each key pressed once (and VOL+, A/B, a digit
  held): the band on 3-9 and 0, the dot and HOME in the F-INP box, F3 with an action set, the volume stepping on
  while VOL+ is held and stopping when let go, A/B swapping and its hold copying. Then the X6100's mic in the same
  radio (#145): 3, 9 and the dot on the main screen log `hand mic: key 0xE4 (3)`, `0xA4 (9)` and `0x21 (.)` and
  give 40 m, 10 m and no band, and the H-22's 3 again `0x18 (3)` and 60 m. If the X6100 mic sends 0x18 there, the
  radio cannot tell the mics apart by their codes: the manual's line that it does goes, and #145 is open again.
- Open: the H-22's side UP/DOWN. If they are to work, they are outside the packet's key word (Xiegu's manual names
  two data lines on the mic's plug, MDATA and MSVSW): the packet's other words (`reserved_2`, `reserved_3`, the flag
  bits the library leaves unnamed) logged when they change, in hw_x6200.c.

## Keyboard: CapsLock's chords, the quick QSO log and PTT, on a radio (#175)

Asked 2026-11-04 (F4ESO, X6200, random SSB QSOs logged from a Bluetooth keyboard). Done after v0.3.8: CapsLock is the
keyboard's hyper key (src/kbd_hyper.c), the one way to shortcuts, so they work on any keyboard: L logs a QSO (the CW
line's in CW, else the main screen's Log QSO box; the owner: "reuse L"), K the dial lock, P a screenshot, S Settings,
F the frequency box, 1-5 the CW messages (1-8 JTTY's macros), Up/Down the band, Space a PTT toggle (src/kbd_ptt.c).
The keyboard's F keys and PgUp/PgDn do nothing any more. Field Ops' Log QSO takes the report sent first, as the box
does. Checked with tools/keyboard (kbd_hyper, kbd_keymap, kbd_ptt) and the two PC builds only: not on a radio, nor in
the PC build's window.
- **Next**: on the bench X6200 with a USB keyboard and a Bluetooth one, on the main screen in USB: CapsLock+L opens
  `Call [sent [rcvd]]:`, `F4ABC 59 57` and Enter log `QSO logged: F4ABC 59 57 SSB` (59 sent on the Logs page), no L
  typed and Caps Lock still off after it (a letter typed comes out small), the Caps Lock light off again once L
  went down. CapsLock tapped alone: the light and capitals on, tapped again both off. CapsLock+L in Settings, FT8
  and the box itself: nothing. In CW with `F4ABC 599` typed in the CW line: CapsLock+L logs it, the line empties,
  nothing keyed; with the line empty (sending) the box opens and the line stays. Field Ops, a test activation:
  `F4ABC 59 57` logs 59 sent, 57 received. Then the X6100 (the light is the console's: KDSKBLED on /dev/tty0).
- **Next, the same bench**: CapsLock+K and Scroll Lock say "Dial locked" / "Dial unlocked"; CapsLock+P and Print
  Screen take a screenshot (P also in Settings, an app and the quick log's box); CapsLock+S opens Settings; CapsLock+F
  the frequency box, in CW too (f goes into the CW line there); CapsLock+Up/Down one band a press, held too; in CW
  CapsLock+1-5 key the CW messages; in JTTY CapsLock+1-8 its macros. F1-F10, PgUp and PgDn: nothing anywhere. In
  the WiFi password box and the quick log's box none of them types. The Scroll Lock light is the console's own,
  which turns at each press: it can show the lock the wrong way round (left as it is). CapsLock tapped and A typed
  fast over it: a capital A, Caps Lock on. In walkie mode CapsLock+S, L and Space: nothing. The screen dark (Screen
  off 1 min): the first CapsLock+L only lights it. In USB after CW with a call decoded: CapsLock+L's box empty.
- **Next, PTT, into a dummy load**: CapsLock+Space keys (`PTT on: CapsLock+Space for off`, the TX panel or the S-meter
  box's power and SWR), again lets go; in CW the carrier. Then, each while keyed: the side PTT pressed and let go
  (unkeyed, `PTT off: CapsLock+Space for on`; the next chord keys), Settings opened from the front key then
  CapsLock+Space (lets go), the USB keyboard pulled and the Bluetooth one switched off (`PTT off: keyboard gone`), CW
  keyed then the mode to USB and CapsLock+Space (no carrier left down), CAT PTT off in CW (no carrier left down),
  three minutes keyed (`PTT off: three minutes on the air`). Out of band: `No TX on ...`, the next chord tries again.
  A browser paired meanwhile: its PTT refused ("controlled by the radio's operator").
- Open: more chords, a line each in kbd_hyper.c's table.
- **Asked next (F4ESO, 2026-11-06)**: name, QTH/locator and a comment in the same Log QSO box, after the reports, each
  after a comma, all optional (`F4ABC 59 57, Jean, Paris JN18du, nice signal`): a locator into GRIDSQUARE, the rest
  of the QTH into QTH, the comment into COMMENT. Settle a comment without a name or a QTH, and a comma inside one.
  A QRZ.com lookup: not now (the PC logger fills these in at import).

## Winlink: VOACAP predictions in Pat's gateway list (#153)

**Done 2026-11-18** (the owner's go-ahead for the downloads): tools/voacap, a `voacapl` of ours on hfcast-engine
(2.14.0, bf34e8f, Apache-2.0, its coefficients compiled in) that Pat runs as it runs the Fortran one, no Pat patch;
x6100-winlink turns Pat's engine on with it, GOMAXPROCS=2, the power capped at 7 W (X6100_VOACAP_W). Checked: Pat's
nine Fortran listings' MUF columns identical (the operating frequency's within 0.1 degree and 1 km, their sample
antenna against the isotrope), Pat's own parser reads its listings, on the X6200 `pat rmslist -m ardop -b 40m -q`
rated 243 gateways in 9.5 s (about 50 ms a prediction). Host check `voacap`; image: buildroot/local-build.sh builds
it as XARA is built. The public image and bundle carry it with the CCIR coefficients: the owner's decision,
2026-11-18, "Ship it" (tools/voacap/UPSTREAM.md: the ITU publishes them free from copyright assertions for
implementers of its Recommendations, VOACAP not one; hfcast's crate leaves them out of crates.io for that reason).

Asked 2026-11-02 (km4gkd, #153). Pat's gateway list (the mailbox's Connect dialog) has a VOACAP column with each
gateway's predicted path quality; on the radio it stays hidden, because x6100-winlink sets Pat's `prediction` engine
to `disabled` (the image has no `voacapl`). The wish: that column on the radio, to choose a gateway and a frequency in
the field. For the reporter a minute or two for the list to fill in is fine, and so is a VOACAP server on the device
Winlink is used from (the #153 replies of 3 and 4 November).

- **How Pat predicts**: it writes a deck (method 30, the CCIR maps, 50 W: app/rmslist.go and
  internal/propagation/voacap/encode.go in Pat v1.0.1), runs `voacapl` with an itshfbc data tree once per gateway
  frequency, as many at a time as GOMAXPROCS (all four cores), and reads its printed listing. Its `voacap-api` engine
  POSTs the same deck to `<address>/predict` and reads the same listing back.
- **The engine**: `voacapl` is Fortran, which the radios' build does not compile. hfcast-engine
  (github.com/jonathanmcsweet/hfcast-engine, linked on #153 by doodlecomms, 5 November) is VOACAP translated into
  Rust (Apache-2.0, no dependencies, rust-version 1.87), checked cell by cell against the Fortran, with the
  coefficients compiled in through its `embedded-coefficients` feature (a build from the repository only) so no
  itshfbc tree; it covers method 30 with the CCIR maps. It would build with tools/xara's pinned Rust (1.99.1, armv7).
  It takes JSON (its `predict`) or a library call, not Pat's deck: a small program of ours on the library, Pat's deck
  in and the method-30 listing out (its `api::listing`), either as a `voacapl` for Pat's `voacap` engine or as a
  server on 127.0.0.1 for Pat's `voacap-api` engine, which needs no Pat patch.
- **Speed**: fewer workers than Pat's default, next to the GUI; a full gateway list measured on an X6100. A prediction
  must never disturb a Winlink session or the radio.
- **Power**: our Pat build predicts for the radio's 7 W at most instead of Pat's fixed 50 W, or the list looks better
  than the radio can do.
- **The data's status**: hfcast's own notes (NOTICE, docs/licence.md, embedded/README.md) leave the redistribution of
  the coefficient files partly open (CCIR Reports 322 and 340, which ITU-R Study Group 3 publishes "free from any
  copyright assertions", plus NTIA/ITS work). The same data comes in `voacapl`'s itshfbc, so either route ships it:
  settle it before an image carries it.
- **Then** x6100-winlink sets the engine when the program is in the image, instead of `disabled`.

## Updates: an A/B partition for the firmware binaries (later, to explore)

The owner's idea (14 November 2026), for later: the radio's firmware binaries (the GUI, the web server, the
helpers, their libraries, maybe the kernel set) on two partitions of the SD card, A and B, the update written
whole to the one not running and the start switched over to it only once it came up, the other kept as it was to
go back to. The configuration and everything of the user's (params.db, logs, recordings, `/mnt`) live elsewhere
and stay as they are on both. What it would bring over today's in-place update (x6100-update: each file checked,
swapped in with the old one kept, journalled, taken back after three failed starts): files a newer version no
longer ships go with the old partition (today they stay behind), a version is always whole on its partition, and
the fallback can include the kernel (U-Boot's boot.scr choosing the partition, with a boot counter). To look at:
the card's layout (a second root partition: room on the 32 GB cards, a new card image for the change), U-Boot's
environment on the card for the choice and the counter, how the GUI's settings and the web server's state are
found from either, what the X6100's smaller cards allow, and the image-required levels.


## Time Signal: the European stations (#275)

WWV and WWVH are in (Time Signal, aec9d17). CHU is off the list: Canada's NRC closed it for good on 22 June 2026
(a recording on 7.850 MHz at 03:00 EST on 16 November 2026 was noise, as it would be). Of #275's others, DCF77
(77.5 kHz) and MSF (60 kHz) are below the radio's 100 kHz; TDF (162 kHz), EBC and IAM would need a European
listener to test with.

## A hardware watchdog for a hung radio (#146)

The X6200's SoC has one: `sunxi-wdt` at boot ("Watchdog enabled (timeout=16 sec, nowayout=0)"), `/dev/watchdog`.
Nothing feeds it, so it does nothing. A feeder (busybox's `watchdog -T 16 -t 5 /dev/watchdog`, or a thread of
x6100_daemon) would have the SoC reset itself when Linux hangs, as it did twice on 2026-11-16 when a USB disconnect
oopsed the musb host with its lock held (fixed since in the kernel, 9101/9102, but a net under any such hang).
To find out with the owner at the radio, before it ships: what a SoC reset does to the base's power (a plain
`reboot` switches the X6200 off; `x6x00-reboot` keeps the base on), whether the radio comes back by itself, and that
a stopped feeder (killed GUI, a full disk) never resets a radio that is fine. Off by default, a setting to turn on.

## Deep backlog: the radio as a Bluetooth microphone for a PC (#278)

Not wanted now (the owner, 2026-11-15: no Bluetooth headset or Bluetooth audio in use). Kept only as a note: a PC
paired with an X6200 sees an audio output and no input, because the radio offers no headset profile (HFP/HSP). On the
X6100's chip SCO gives no audio and drops the WiFi; the X6200's is a Realtek RTL8723DU on USB (btusb, SCO over HCI:
SCO MTU 255:12), so the headset role might work there. A test needs a PC or tablet paired with the radio, and may
drop WiFi while SCO runs.

## GSOC: the G90 driven from Xiegu's GSOC touchscreen head (this week, from cjlawson02's fork)

**Ported 2026-11-18** (the owner's go-ahead): the branch's 24 commits not yet on main, each with Chris Lawson as its
author, conflicts kept both ways (our GPU waterfall, the Sentry's claimed plane, the key-speed limits, upstream's
fill by level all as they were; his strip-plane calls renamed drm_strip_back*, as main has a drm_wf_back of its
own); libg90 vendored at 4eefd6e (third-party/libg90, MIT, nothing fetched at build time). The X6100 and X6200
unchanged: the Power button's label, WiFi off/on (BOARD_WIFI_NO_POWER_PIN) and the clock's tap (BOARD_GSOC) kept
to the G90 and GSOC. `tools/cross_x6200/check_g90.sh` builds and links the GSOC/G90 GUI; the X6200's builds,
check_x6100.sh passes. Manual: "The GSOC head with a G90"; README credits him. **Left**: a GSOC and a G90 on the
bench to try it; a buildroot tree, card image and bundle for the GSOC (none in his branch); his
tools/d3_colmajor_test and drm_landscape_test extract parts of waterfall_view.c and drm.c by text, and ours carry
the GPU and Sentry code there, so they do not build against our files yet.

Planned 2026-11-16. cjlawson02's fork (cjlawson02/OpenX6X00, branch `gsoc/g90-probe`, 25 commits on our main of
about 13 Nov) runs OpenX6X00 on the GSOC head driving a G90 over its DE-9 cable: a third board next to the X6100 and
X6200 (`HW_BACKEND=g90`, `hw_g90`, a GSOC panel profile and `gsoc_input`). We take it in ourselves and credit them;
no pull request asked. What their branch has, to port onto main as it is then:

- **The board**: board.h ranges for the G90 body, the dialogs, F-bar and meter widths from `BOARD_DISP_W` (the GSOC
  panel is landscape, 1024 x 600), the rotary encoder (one detent one step), the clock tap opening APP, the boot
  animation centred, dimmer defaults, the shutdown pulse.
- **The link to the body**: their `libg90` library (github.com/cjlawson02/libg90, pinned by commit through
  FetchContent; check its licence and build it like our other libraries, not fetched at build time), the body's
  firmware profiles 1.78/1.81 picked from its replies, starting without the body and picking it up when it answers
  (`BOARD_BASE_DETACHABLE`, Connected/Disconnected by the S-meter).
- **Audio**: the head's codec unmuted, the headphones bit, the AF on the cabinet speaker through a loopback or an
  analog path (`G90_AF_ANALOG`), VOL on the body's VOL byte.
- **IQ and spectrum**: the IQ from the head's DMA buffer (`G90_IQ_MMAP`), the body band + codec skirts fuse
  (`G90_FUSE_IQ`) with its quickselect percentile.
- **Display speed**: DRM planes on the landscape panel with the strips on planes of their own, and the 3D waterfall
  drawn in columns and turned over in 64 x 64 tiles on a thread of its own (on the GSOC: the GUI from a full core to
  about 42 %; tools/d3_colmajor_test checks the pictures). Only landscape framebuffers take that path; the X6100 and
  X6200 (portrait, `BOARD_FB_ROTATE_90`) draw as today.
- **WiFi**: the head's WiFi is a USB adapter with no power pin; Off also disconnects it and turns autoconnect off,
  On asks NetworkManager to join again (the rfkill switch left that adapter dead until replugged).
- **Images and updates**: a buildroot tree for the GSOC (theirs or ours), its card image and update bundle, the
  model name in x6100-update and the web server, and an image level line in rootfs/usr/share/x6100/image-required.
- **Already on main** from that branch: dB <-> power without powf/log10f, and the paddle keyer's start after the
  settings (both 16 Nov).
- **Needs**: a GSOC head and a G90 on the bench.
