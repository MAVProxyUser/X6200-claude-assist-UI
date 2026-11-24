# What the orchestrated tests found on the two radios

The faults and differences `orch.py` found that the tests cannot put right themselves. Each line has its evidence:
the run's folder under `~/.config/x6x00-bench/runs/<time>-orch/` (events.jsonl, results.json, the WAVs). The test
results are kept in `~/.config/x6x00-bench/orch_results.json` (`orch.py list`), and the differences between the radios
in `orch_caps.json` (the caps test, shown on the console).

## Faults

| found | what | evidence | status |
|---|---|---|---|
| 2026-11-23 | **Both radios serve the HTTPS private key and the remote's session tokens** to anyone on the network, no login (`/files/https/key.pem`, `/files/remote_sessions.conf`: 200). The tokens let anyone operate and transmit. | web-secrets, every run | Fixed in the repo (3906e452 the key, 7b01807b the sessions); the radios run the 0.3.12.46 image's web server, which cannot be hot-installed (its settings lack CERT_DIR/DMR_DIR). Passes once they run a newer image (a release: the owner's word). KNOWN in orch.py. |
| 2026-11-23 20:36, 21:54 | **Both GUIs crashed around the JS8 app**: the X6200 (SIGSEGV, exit 139) as its JS8 app closed back to the Sentry's G right after a JS8 over and its decode (G then tuned CB 20, 27.205 AM); the X6100 while its JS8 app was stepped 80 m -> 20 m with BAND_UP just after receiving JS8 (its log ends mid-line). | start_trace "exit code 139"; app_logs/x6100_log_2026-11-24_01-36-32.txt (X6200), x6100_log_2026-11-24_02-54-47.txt (X6100) | Not reproduced in 5 rounds without a transmission (js8-close-to-g); js8-text passed again after. Under investigation with AddressSanitizer (session js8-crash-asan): docs/security/js8-crash-2026-11-23.md when done. |
| 2026-11-23 | **The X6100's CW reaches the X6200 with its word spaces and some letters lost** (e.g. 'ÄVVTESTDE<?>1TST'), 3 runs in 5: the X6100 leaks carrier about 15 dB under its mark between elements, and the X6200's decoder reads it. | cw-text x6100>x6200 | A TX DC-offset calibration of the X6100 (the owner's call). The other way copies clean. |
| 2026-11-23 | **The X6200 does not hold a TCI over's power to its setting below 1 W**: its ALC reads 0, so the GUI holds power by the radio's own meter (`tx_level.c` power_hold), which reads 0.1 W steps. At 0.1 W it came 4 dB low (4 runs in 5); stepping 0.2/0.5/1.0 W it read 0.3/0.8/0.9 W on its meter and the X6100 heard 0.5 and 1.0 W alike (4 runs in 6). | power-steps x6200>x6100 | A GUI fault to fix (the hold's steps and its overshoot); KNOWN in orch.py. The X6100's steps pass. |
| 2026-11-23 | **The X6100's transmitted audio rises ~15 dB from 600 to 2400 Hz** in SSB (the X6200's 4-6 dB), 14 dB in AM. Heard either way; noted, not judged. | the steps' "spread" in usb/lsb/am-steps | Noted. |
| 2026-11-23 | **The X6200 does not take LSB-D (mode 1)**: "set mode 1" answers nothing and it stays in USB; the X6100 takes it. | caps "mode 1" | Open (a difference or a fault of the X6200's mode list). |
| 2026-11-23 | **The X6200's link to the bench stalls for 3-6 s now and then** (no state, the same GUI process: its WiFi), as air_c saw. | "NO WORD" during rtty-text 21:08; air_c's Watch stops | The orchestrator tells a stall (the same GUI process after) from a crash (another): a stall is said, not failed. |

## Where the radios differ (the caps test, by design)

- The X6200 tunes 88-118 MHz (its band table has FM broadcast and Airband) and WFM; the X6100 refuses them ("not a
  frequency this radio tunes", "mode: not on this radio"). Tests never take both radios there: they cannot be on one
  frequency (the orchestrator refuses such a value up front).
- IF shift: only the X6100's base has it.
- The same on both: every HF/6 m frequency probed, the modes but WFM and LSB-D, each mode's tuning step, a knob
  detent, the filters in USB/CW/AM/NFM, zoom, steps, AGC, NB, NR.
