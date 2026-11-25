# Tests the owner facilitates

From 20261125-124652-coverage (`orch.py coverage`, then `python3 facilitate.py`). 271 features still wait on you: internet services, Bluetooth, GPS or your hands. Each session says what you do and what the bench does; a ticked line is covered.

## The X6100 as the X6200's Bluetooth client (your OK, no hands)

- **You:** Say yes once: the X6100 pairs with the X6200 as a Bluetooth CAT/TCI client (Just Works, in the X6200's 3-minute pairing window, opened over SSH). Both radios keep that pairing until it is forgotten.
- **The bench:** Every CI-V command over SPP, transceive, the paired-only rule, TCI over RFCOMM with its ask-before-TX and watchdog (TX at 0.1 W over the cable), the Bluetooth CAT card and icon.
- **Your time:** none (about an hour of the bench's time)
- **Left:** 23 of 23

  - [ ] `btcat-paired-only` (cat): CAT accepts paired devices only (RFCOMM security medium)
  - [ ] `btcat-ci-v-over-rfcomm` (cat): A paired PC/phone gets a serial port (rfcomm channel 1, COM/SPP on Windows/Android) carrying the radio's CI-V CAT; the Clients row counts it
  - [ ] `btcat-spp` (cat): Bluetooth CAT: SPP on RFCOMM channel 1 (UUID 00001101-...), paired once in the 3-minute window (Just Works), device name X6100 or X6200 (BTC
  - [ ] `civ-read-freq-mode` (cat): CI-V 03 read frequency, 04 read mode
  - [ ] `civ-set-freq-mode` (cat): CI-V 05 set frequency (5 BCD), 06 set mode (00 LSB, 01 USB, 02 AM, 03 CW, 05 NFM, 07 CW-R)
  - [ ] `civ-transceive` (cat): Transceive: 00 frequency and 01 mode sent on every change; 07 00/07 01 sent when the VFO changes
  - [ ] `civ-vfo` (cat): CI-V 07: read current VFO, select A (00), B (01), A=B (A0), swap (B0)
  - [ ] `civ-split-step-att` (cat): CI-V 0F split off/on, 10 tuning step (00-04), 11 attenuator
  - [ ] `civ-levels` (cat): CI-V 14: 01 AF volume, 02 RF gain, 03 squelch, 09 CW pitch (300-900 Hz), 0A TX power (0-255 = 0-10 W, X6200 stops at 8 W), 15 monitor level 
  - [ ] `civ-meters` (cat): CI-V 15: 02 S-meter (120 = S9), 11 TX power, 12 SWR (0=1.0, 48=1.5, 80=2.0, 120=3.0, 200=5.0), 13 ALC; 0 before the first reading
  - [ ] `civ-funcs` (cat): CI-V 16: 02 preamp, 22 noise blanker, 40 noise reduction; 44 compressor and 46 VOX always off (write ignored)
  - [ ] `civ-id` (cat): CI-V 19 00 transceiver ID returns A4
  - [ ] `civ-band-filter-data` (cat): CI-V 1A 01 band (write tunes the band centre), 1A 03 IF filter width index, 1A 06 data mode + filter (write adds/drops USB-D/LSB-D)
  - [ ] `civ-ptt` (cat): CI-V 1C 00 PTT; a TX the radio refuses (outside the amateur bands or TX ranges, receive only, no base data) answers NG (FA)
  - [ ] `civ-sel-vfo` (cat): CI-V 25 frequency and 26 mode + data mode of the selected / unselected VFO
  - [ ] `civ-scope-stubs` (cat): CI-V 27: 10 scope on (always), 11 wave data (stub), 14 centre mode, 15 span (follows the zoom; write sets it), 19 reference level (stub)
  - [ ] `civ-rit` (cat): CI-V 21 00 RIT offset, 21 01 RIT on/off, 21 02 XIT on/off (in the code, not in the manual's table; OmniRig polls them)
  - [ ] `civ-unknown-ng` (cat): Anything else returns FA (NG)
  - [ ] `bt-tci-websocket` (tci): The same TCI WebSocket over the RFCOMM link (same HTTP upgrade; no Origin = a TCI program); paired, encrypted devices only; GUI log 'tci: Bl
  - [ ] `bt-tci-ask-before-tx` (tci): A Bluetooth client's first key asks on the screen 'Bluetooth AA:BB:.. (name) wants to transmit'; Always keeps the address in the TCI page's 
  - [ ] `bt-tci-follows-tci-switch` (tci): TCI off (Settings > General > TCI SERVER or web System > TCI) closes and refuses Bluetooth TCI too; Listen only applies to it
  - [ ] `bt-tci-ptt-watchdog` (tci): A Bluetooth client's TX ends at once when the link/WebSocket closes, after 0.5 s without its TX audio when keyed with audio, after 5 s of si
  - [ ] `settings-bluetooth-cat` (settings-bluetooth): BLUETOOTH CAT: clients connected and port

## Internet, read only, no account (your OK, no hands)

- **You:** Say yes: the radios fetch what these apps fetch for anyone (Open-Meteo and the weather services with the position rounded to 0.1 deg, airplanes.live/adsb.lol, radio-browser and the streams, hamqsl.com, EiBi, MyMemory for JS8 translation, xiegu.eu's downloads page, this fork's GitHub releases, NTP).
- **The bench:** Weather, Flights, Internet radio, Conditions, the broadcast list, JS8/GhostNet translation, the daily update and base checks, the update box and Settings > Info > Install (the bundle installed is this fork's latest release, put on the radio like any update), Xiegu's page read (Check only), NTP setting the clock.
- **Your time:** none
- **Left:** 28 of 28

  - [ ] `weather-now-view` (app-weather): Now: temperature, sky, feels-like, low/high, sunrise/sunset, wind/gusts, humidity, pressure, rain chance and amount, clouds, dew point, visi
  - [ ] `weather-5days-view` (app-weather): 5 days: each day's high/low, sky, rain chance and amount, strongest wind and gusts, UV 3+
  - [ ] `weather-warnings-view` (app-weather): Warnings: each warning in force (level, what, from/to, issued, areas, the service's text and instructions), the place's own first (*here*), 
  - [ ] `weather-update-polling` (app-weather): Update (or MFK press) asks now; on opening and every 15 min while open and lit, not in the background (unless Notices on); the app and web p
  - [ ] `weather-privacy-place-name` (app-weather): Sends the position rounded to 0.1 deg, the screen language and 'OpenX6X00/<version>', never the callsign; the place name from Nominatim only
  - [ ] `weather-warning-services` (app-weather): Warnings from the NWS (US), MeteoAlarm (38 countries, area-name matching: other orange/red as nearby, max 12), SMN Argentina and INMET Brazi
  - [ ] `weather-forecast-warnings` (app-weather): Forecast-derived warnings for today/tomorrow: gusts 65/90/120 km/h, rain 50/100/150 mm, thunderstorms (hail orange), heat 36/40/44 C, cold -
  - [ ] `weather-notices-background` (app-weather): With Notices on the radio checks 2 min after start then every 30 min (Open-Meteo + the service, never Nominatim; none while the app is open)
  - [ ] `flights-plot-list` (app-flights): Plot (north up, you in the middle, range rings, dots with track strokes, named when 15 or fewer, ground grey, emergency red) and list (neare
  - [ ] `flights-update-polling` (app-flights): Update (or MFK press) asks now; otherwise on opening then every 20 s (60 s from adsb.lol), only while open and lit, never in the background;
  - [ ] `flights-source-privacy` (app-flights): Data from airplanes.live, adsb.lol as fallback; bottom line names the source and fetch time; sends only the position rounded to 0.1 deg, the
  - [ ] `iradio-open-lists` (app-iradio): Internet radio (APP 4:6, Web radio tile, long-press) opens on favourites, else the country's stations, else the countries; lists ordered by 
  - [ ] `iradio-play-stop` (app-iradio): Play / Stop (MFK press or tap): plays on the speaker at VOL (or Bluetooth headphones standing in), header Connecting/Playing/Stopped/Error, 
  - [ ] `iradio-favourite` (app-iradio): Favourite toggles the selected station in/out of /mnt/iradio/favorites.tsv
  - [ ] `iradio-list-country-search` (app-iradio): List cycles Favourites, country, world's most listened, last search; Country chooses from the directory's countries (kept in /mnt/iradio/cou
  - [ ] `iradio-error-reasons` (app-iradio): Error says why a station did not play ('not a stream: a web page', 'the station answered HTTP 404')
  - [ ] `iradio-network-policy` (app-iradio): A list is fetched when shown and kept an hour (countries a day); the stream only while playing; nothing while closed; says 'OpenX6X00/<versi
  - [ ] `prop-conditions-fetch` (app-propagation): Conditions view: N0NBH/hamqsl.com solar data fetched on opening and hourly while open (Refresh if the last is 5+ minutes old), kept in /mnt/
  - [ ] `settings-info-broadcast-list` (settings-info): Broadcast list Update (EiBi, season A/B, previous season fallback): 'In use: season ..., N stations', progress, success and failure messages
  - [ ] `js8-translate` (app-js8): Translate (page 7) via MyMemory into the GUI's language (from English or Spanish): a 'NL: ...' line under the message; callsigns/locators/gr
  - [ ] `ghostnet-translate-arriving` (app-js8): Translate arriving messages (GhostNet popup, off by default): each English/Spanish message translated the same way
  - [ ] `ghostnet-mine-into-english` (app-js8): Mine into English first (off by default): the first OK puts your text into English in JS8's letters, the second OK sends; callsigns/groups a
  - [ ] `system-update-daily-check` (system): Daily update check 5-35 min after start then every 24 h, an on-screen notice once per version, /mnt/update/no-check turns it off
  - [ ] `system-update-apply-prompt` (system): Update box 'Apply GoldCorpHF X?' (version, commit, MD5, repo): MFK between 'Yes, apply it' and 'No' (No preselected), keyboard arrows/Enter/
  - [ ] `system-update-install-settings` (system): Settings > Info > Update Install downloads the bundle (~26 MB, ~37 MB with kernel and U-Boot) and asks the same question with the tick boxes
  - [ ] `system-x6200-base-fw-check` (system): Check (F3) reads xiegu.eu's downloads page now
  - [ ] `system-x6200-daily-base-check` (settings-info): Daily base check (Settings > Info, off by default): once a day online reads xiegu.eu, one notice when a newer base is out, shown next to Bas
  - [ ] `network-ntp-clock` (network): Clock set by NTP (three tries 10 s apart) and written to the clock chip; ntpd restarted if it found no server; /var/log/ntpdate.log

## The DX cluster under your callsign (your OK, no hands)

- **You:** Say yes to the radios logging in to a DX cluster as you, read only (they carry test calls Q1TST/Q2TST, which clusters refuse), or name a cluster that takes a test call. Nothing is spotted.
- **The bench:** The Cluster app, its filters and alerts, spots on the waterfall and the column, spot snap, the Bands tile, the DX icon, the remote panel's spots, Log QSO's spot call, FT8's POTA tag (spots from pota.app).
- **Your time:** none
- **Left:** 24 of 24

  - [ ] `cluster-latest-spots-and-age` (app-cluster): After login the client asks for the latest 50 spots (sh/dx); spots older than 60 minutes are dropped
  - [ ] `cluster-app-band-list` (app-cluster): Cluster app (CLUSTER 1:2 / 2:2): spots of the current band sorted by frequency around a VFO marker row; the list follows tuning and selects 
  - [ ] `cluster-mfk-tune-spot` (app-cluster): MFK turn browses the list (VFO stops following); MFK press or Tune tunes the spot, mode from the comment (CW, FT8, SSB...) else from the ban
  - [ ] `cluster-vhf-spots-transverter` (app-cluster): 4 m, 2 m and 70 cm spots are listed (144.300 format) and tuned through a transverter covering them or, on the X6200, the WR12 (RX only); wit
  - [ ] `cluster-mode-view-filter` (app-cluster): Page 1 Mode button: quick view filter All > CW > SSB > DIG
  - [ ] `cluster-filters-mode-band` (app-cluster): Cluster filters (Settings > Cluster > CLUSTER FILTERS, also the web page): modes CW/SSB/DIG/AM-FM and bands 160 m-6 m plus 4 m/2 m/70 cm; on
  - [ ] `cluster-filter-activity` (app-cluster): Activity filter POTA, SOTA, WWBOTA, HOTA, WWFF, IOTA, WCA, TOTA, MOTA, LH, LLOTA (any mix, none = all), told from the comment's word or refe
  - [ ] `cluster-activator-feeds` (app-cluster): Besides the cluster the radio reads pota.app, wwbota.org, cqhota.app and llota.app activator spots once a minute into the same list (llota s
  - [ ] `cluster-web-age-skip-spotters` (app-cluster): Web Cluster page filters add maximum age and spotters to skip (* wildcard)
  - [ ] `cluster-heard-in` (app-cluster): Heard in (web Filters card): only spots whose spotter is in the listed countries (prefixes by cty.dat, up to 16; -2/-# ignored; unknown spot
  - [ ] `cluster-alerts` (app-cluster): Alerts (web): calls or prefixes (3B8M, VP8*, */MM), optional bands/mode; matching spots magenta with ALERT, never filtered; a new one pops u
  - [ ] `cluster-spots-on-waterfall` (app-cluster): Settings > Display > MAIN SCREEN Cluster spots: short cyan line and callsign at each spot frequency at the top of the waterfall, two stagger
  - [ ] `cluster-column` (app-cluster): Cluster column: box at the right of the waterfall with kHz, call, mode, age around the VFO row, coloured by age; title 'Cluster 40m - Freq';
  - [ ] `cluster-spot-snap` (app-cluster): Spot snap hold (0-5) and speed (1-5): turning the main knob slowly stops the VFO on a spot it passes for N more detents (only with cluster s
  - [ ] `settings-cluster-filters` (settings-cluster): CLUSTER FILTERS Modes, Bands, Activity (POTA, SOTA, ...; none lit = all spots)
  - [ ] `settings-cluster-view` (settings-cluster): CLUSTER VIEW Newest first, All bands, Spot snap hold, Spot snap speed
  - [ ] `settings-display-cluster-overlay` (settings-display): Cluster spots on the waterfall and the Cluster column (spot list on the right)
  - [ ] `panel-spots` (remote): DX cluster spots as the radio filters them ('spots [...]'), newest first, tap tunes; calls on the waterfall, alerts orange
  - [ ] `main-screen-status-dx-icon` (main-screen): DX icon: green cluster connected, grey connecting or retrying, dark stopped
  - [ ] `cluster-status-line` (system): The Cluster app's status line (also on the web Cluster page's Link card, 'Not taken up by the radio yet')
  - [ ] `log-quick-qso-spot-call` (system): With the dial on a DX Cluster spot the spot's call is pre-filled (1 kHz SSB/AM/FM, 200 Hz CW/data; none when two are close); POTA/SOTA/WWFF/
  - [ ] `bands-activity-rows` (app-bands): Each band: activity bar and word, cluster/RBN counts, who heard you (best SNR); the best band framed; MFK moves the filled row
  - [ ] `field-bands-screen` (app-field-ops): Bands tile: per band cluster spots (30 min), RBN skimmer spots (15 min), skimmers that heard you (10 min, best SNR), activity bar HIGH/GOOD/
  - [ ] `ft8-pota-tag` (app-ft8): POTA tag after the message for a station spotted on POTA now (cluster's pota.app spots) and any CQ POTA

## Public posts under your callsign (your yes for each)

- **You:** Each of these publishes on the internet under your call: say which (if any) to post. Their first press (what would be posted, nothing sent) the bench checks without asking.
- **The bench:** POTA self-spot and re-spots, the DX cluster spot, FreeDV Reporter, WSPRnet upload, PSK Reporter (off on both radios while they carry test calls).
- **Your time:** a few minutes each, while you are really on the air
- **Left:** 7 of 7

  - [ ] `field-pota-spot-two-press` (app-field-ops): Spot: POTA self-spot on pota.app with the radio's frequency and mode; first press shows what will be posted and the button reads 'Spot?', a 
  - [ ] `field-pota-auto-spots` (app-field-ops): Re-spot (on by default): in POTA the radio spots at Start and after a QSY (1 kHz+ or mode change) once it has transmitted and stayed a minut
  - [ ] `field-spot-dx-cluster` (app-field-ops): Spot also posts on the DX cluster ('POTA IT-0123 SSB', 'SOTA I/LO-123 CW', 'TOTA OKR-0150 SSB', 'WCA DL-01156 CW', 'MOTA X42018 SSB', 'LH AR
  - [ ] `rade-freedv-reporter` (app-rade): FreeDV Reporter (page 2, off by default) publishes callsign, locator, frequency, TX state and heard callsigns to qso.freedv.org over TLS whi
  - [ ] `wspr-upload` (app-wspr): Each cycle's spots uploaded to WSPRnet (plain HTTP over WiFi) with that cycle's dial; status line counts sent and failed
  - [ ] `settings-cluster-psk-reporter` (settings-cluster): PSK REPORTER Report decodes (on by default)
  - [ ] `pskreporter-upload` (settings-cluster): PSK Reporter (Settings > Cluster > PSK REPORTER > Report decodes, on by default): FT8/FT4/FT2/JS8/JTTY decodes, and PSK/Olivia/Contestia 'DE

## Zello (your account and a test channel)

- **You:** Type a Zello account and a channel only you use into the radio's web Zello page; talk into the hand mic once.
- **The bench:** Logon test, connect, receive, the link both ways over the cable at 0.1 W (CW ID, limits, yielding), the radio's own TX pausing it, Internet radio ducking for it.
- **Your time:** 10 min (then the bench alone)
- **Left:** 14 of 14

  - [ ] `zello-web-test` (app-zello): Test (web): logs on with the saved account on the first channel and off again ('Logged on: Test channel, 3 online')
  - [ ] `zello-connect-channel` (app-zello): Zello app: MFK + press (or Connect) joins a channel; top line Connecting / channel + online count / not online / 'lost: again in 4 s'; press
  - [ ] `zello-receive-plays` (app-zello): Channel messages play on the speaker at VOL (or Bluetooth headphones), the talker named; between messages the receiver
  - [ ] `zello-connection-lifecycle` (app-zello): Connected only while the app is open on a channel or the Simplex repeater is on; ESC leaves; nothing at power on; reconnects at once then 1,
  - [ ] `zello-link-status-persist` (app-zello): Main screen 'ZELLO LINK' tag with Z>RF/RF>Z, '...' when off the channel, red [TX]/[ID], [RX>Z], 'Z>RF(off)'; app top line SIMPLEX REPEATER; 
  - [ ] `zello-talk-hand-mic` (app-zello): Talk: hold the radio's PTT key (or Talk) and speak into the hand mic without its PTT; built-in mic unusable; 'No audio: talking on Zello use
  - [ ] `iradio-ducks-for-zello-svx` (app-iradio): A Zello message or an SVXConnect over (with the Simplex repeater session on) drops the station out and it comes back after, on speaker and h
  - [ ] `tx-simplex-repeaters-ham-only` (tx): Zello and SVXConnect simplex repeaters key only inside the table or an enabled transverter range; CB/air/broadcast audio goes to the network
  - [ ] `zello-ptt-captured` (app-zello): While the app is open the PTT key talks on Zello and never keys the radio; it keys the radio again once the app closes; a PTT held when the 
  - [ ] `zello-pauses-during-radio-tx` (app-zello): While the radio transmits for anything else (hand mic PTT, CAT, TCI, browser PTT, an app, the tuner) channel audio pauses and a message bein
  - [ ] `zello-link-zello-to-rf` (app-zello): Zello>RF (page 2): channel messages transmitted on the frequency in the radio's mode (LSB/USB/AM/FM, never CW/data) at the voice-message lev
  - [ ] `zello-link-rf-to-zello` (app-zello): RF>Zello (page 2): what the receiver hears while the squelch is open goes to the channel, ending 1.5 s after it closes
  - [ ] `zello-link-id-limit-pause` (app-zello): CW ID (800 Hz, 20 WPM; end of the first transmission then each 5-120 min, default 60, or Off; owed ID sent when Zello>RF is turned off); TX 
  - [ ] `zello-link-yields` (app-zello): Never over anyone: no key while the squelch is open, the radio transmits for anything else, you talk on Zello, or 0.5 s after; the radio's P

## SVXLink reflector (your certificate)

- **You:** Get a signed certificate from a reflector and enter it on the radio's SVXConnect page; talk once.
- **The bench:** Connect, talkgroups, priority pull, receive, the link both ways over the cable.
- **Your time:** 10 min (then the bench alone)
- **Left:** 9 of 9

  - [ ] `svx-connect` (app-svx): Connect (page 1) logs in with the saved settings; top line Connecting / reflector, selected talkgroup (locked), node count / why not ('certi
  - [ ] `svx-talkgroups-lock-recent` (app-svx): Talkgroup table (> selected, + priority, 'listen' monitored-only, talker and time); MFK + press selects a switchable one; Lock keeps it; Rec
  - [ ] `svx-receive-plays` (app-svx): The selected talkgroup's overs play on the speaker at VOL; the talker named; between overs the receiver; an over wins over the Internet radi
  - [ ] `svx-link-priority-pull` (app-svx): A talker on a higher-priority talkgroup pulls the selection over and its overs go on the air too; Lock for a link relaying one TG
  - [ ] `svx-link-status-persist` (app-svx): Main screen 'SVX LINK' with S>RF/RF>S, red [TX]/[ID], [RX>S]; closing the app keeps the link ('SVXConnect link stays on'); Disconnect stops;
  - [ ] `svx-talk-hand-mic` (app-svx): Talk: hold the radio's PTT key (or Talk) and speak into the hand mic; to the selected TG; 'not sent: X is talking on TG 8'; ends at the TX t
  - [ ] `svx-ptt-captured` (app-svx): Talk via, the PTT key held by the app, the pause while the radio transmits and the connection rules are as in Zello
  - [ ] `svx-link-directions` (app-svx): SVX>RF and RF>SVX (page 2): the selected talkgroup's overs transmitted on the frequency, the receiver (squelch open) sent to the talkgroup; 
  - [ ] `svx-link-id-limit-pause` (app-svx): CW ID, TX limit and Pause as Zello's, same defaults (svx_id, svx_id_min, svx_limit, svx_pause)

## Wavelog, QRZ Logbook, Winlink (your accounts)

- **You:** Type a Wavelog URL and API key (yours, or a throwaway Wavelog), a QRZ Logbook key, Pat's Winlink password into the radio's pages. Test QSOs go to them under the test call; delete them after.
- **The bench:** Wavelog test, uploads, station profile, refusals; QRZ upload; Winlink over telnet and the gateway list.
- **Your time:** 10 min
- **Left:** 6 of 6

  - [ ] `wavelog-setup-test` (wavelog): Wavelog page: URL, read & write API key, Load station profiles, Upload on; Test connection checks the version and the key's rights (read-onl
  - [ ] `wavelog-upload-qsos` (wavelog): Every QSO the radio logs (FT8/FT4/FT2/JS8, voice, CW, keyboard modes, Field Ops) goes to Wavelog a few seconds after, from the moment Upload
  - [ ] `wavelog-station-profile-auto` (wavelog): Station profile Automatic: each QSO to the profile with the radio's callsign; with none, 'Wavelog: no station profile for X, 1 QSO waiting' 
  - [ ] `wavelog-refused-and-edits` (wavelog): A QSO Wavelog refuses goes to /mnt/field/wavelog.failed.adi (not resent unless edited); a refused key stops the upload; an edited QSO goes a
  - [ ] `field-qrz-upload` (app-field-ops): Upload QRZ (experimental) sends the whole activation log to QRZ.com: 'QRZ: 5 sent, 2 already there', refused ones named; 'No log to upload',
  - [ ] `winlink-telnet-and-gateway-list` (winlink): With internet, Pat's Telnet transport sends and fetches mail with no radio; the gateway list comes from winlink.org the first time (kept on 

## Tailscale and Funnel (your tailnet)

- **You:** Log the radio in to your tailnet (the login link opens on your side). Funnel puts the Panel and Screen pages on the internet: say yes for that part, for a few minutes.
- **The bench:** Reach over the tailnet, what userspace mode exposes, priority, log out/off, Taildrop refused, Funnel's pages only, ownership, persistence.
- **Your time:** 10 min
- **Left:** 10 of 10

  - [ ] `tailscale-reach-services` (network): Over the tailnet: http://<name>/, /panel, /remote, TCI on 50001; pairing still asked before operating
  - [ ] `tailscale-localhost-exposure` (network): Userspace mode hands connections to 127.0.0.1: Pat's rigctld 4532, ardopcf 8515/8516, gpsd 2947 open to the tailnet while Tailscale runs
  - [ ] `tailscale-no-logs-priority` (network): tailscaled runs with --no-logs-no-support, no DNS on the radio, at lower priority than the GUI and audio, ~30 MB
  - [ ] `tailscale-logout-off` (network): Log out forgets the login (asks a new one at once while on); Off stops and keeps the login; on the radio Log out takes two presses
  - [ ] `tailscale-taildrop-refused` (network): Files sent with Taildrop are refused (1 MB of memory only)
  - [ ] `funnel-enable` (network): Funnel on (Settings > Wireless > TAILSCALE or Remote > Tailscale): Public shows https://<name>.<tailnet>.ts.net/panel; until the tailnet all
  - [ ] `funnel-pages-only` (network): Only the Panel and the Screen page go on the internet, served by tailscaled from /run/x6100-funnel, no callsign or state before a key, any o
  - [ ] `funnel-owns-funnel` (network): The radio owns Funnel: a hand-made funnel is taken off the internet within a minute; serving to the tailnet stays
  - [ ] `funnel-persist` (network): Funnel stays on through restarts; Tailscale off and shutdown take it out of tailscaled, the next start puts it back within seconds
  - [ ] `system-update-funnel-off-downgrade` (network): Going back to v0.3.6 first turns Tailscale Funnel off and removes funnels from tailscaled; refuses (page says why) if it cannot within 30 s;

## The Rii keyboard (already paired to the X6200)

- **You:** Switch the Rii X1 Plus on and type what the bench asks (it says each line: a word, CapsLock chords, [ ] in CW, the touchpad). With a uinput module built for the radios' kernel (none there now) the bench could type most of these itself and you would only confirm the real keyboard once.
- **The bench:** Watches state, screenshots and the other radio's decoder; CW from the keyboard over the cable at 0.1 W.
- **Your time:** 30 min
- **Left:** 22 of 22

  - [ ] `cwkbd-usb-keyboard` (app-cw-kbd): With a keyboard: type the line under the log, Enter keys it, queue; Backspace corrects; Esc stops what is on air and queued, with nothing to
  - [ ] `jtty-capslock-macros` (app-jtty): With a keyboard CapsLock+1 to CapsLock+8 send the macros
  - [ ] `input-kbd-capslock-cw-messages` (cw): CapsLock+1..5 in CW key MSG > CW messages 1-5 from the CW line
  - [ ] `log-cw-line-capslock-l` (cw): In CW with a line typed in the CW line, CapsLock+L logs that line instead of sending it
  - [ ] `cw-kbd-line-main` (cw): USB/Bluetooth keyboard keys CW from the main screen in CW/CW-R inside a CW segment with no app open: a line over the waterfall, Backspace, E
  - [ ] `cw-kbd-mfk-speed` (cw): While the line is open the MFK turns the key speed (yellow MFK tag), back to its control when it closes
  - [ ] `cw-kbd-capslock` (cw): CapsLock+1..5 key MSG > CW messages 1-5; CapsLock+L logs the typed line; CapsLock+F opens the frequency box; a window drops the queue
  - [ ] `cw-kbd-bracket-paddle` (cw): In CW [ and ] are the paddle's dit and dah (any USB keyboard too); out of CW they are the volume keys
  - [ ] `input-kbd-shortcuts-setting` (input): Settings > Keys > KEYBOARD AND MOUSE > Shortcuts: off = keyboard only types (text boxes, CW keyboard, JTTY, shell); on = knob keys, CapsLock
  - [ ] `input-kbd-layout` (input): Keyboard layout (US, UK, German, Swiss German, French, Belgian, Spanish, Italian, Portuguese, Swedish, Finnish, Danish, Norwegian, Polish, C
  - [ ] `input-kbd-main-screen-keys` (input): Main-screen keyboard keys: arrows = MFK, Enter = MFK press, { } choose / [ ] turn VOL's control, - = knobs, f = frequency box, Scroll Lock =
  - [ ] `input-kbd-capslock-chords` (input): CapsLock chords: K lock, P screenshot, S Settings, F frequency box, Up/Down band, 1-8 JTTY macros; main screen only, once per press, wake a 
  - [ ] `input-mouse` (input): Mouse (USB, Bluetooth, BT keyboard touchpad) with Settings > Keys > KEYBOARD AND MOUSE > Mouse on: arrow, left click = tap (held = long pres
  - [ ] `sentry-touch-mouse-tap` (sentry-common): A tap or mouse click picks the option, row or signal (A's picture: lock on the signal; B's half: take that channel)
  - [ ] `scancfg-a-tap-mark` (sentry-scancfg): A tap on A's picture marks a signal and keeps A from stopping elsewhere 10 s; a second tap stops on it, held
  - [ ] `settings-keys-keyboard-mouse` (settings-keys): KEYBOARD AND MOUSE: layout of a USB or Bluetooth keyboard (US default, UK, German, French...), Shortcuts (off), Mouse (off)
  - [ ] `input-kbd-capslock-ptt` (tx): CapsLock+Space PTT toggle with the radio's refusals, 0 W hold, SWR protection; lets go on CapsLock+Space, not on air in 2 s, keyboard gone, 
  - [ ] `tx-cw-line-band-plan` (tx): The main screen's CW line keys only in a CW segment (not NOT YOUR CLASS; Region 3 not above 3535/7030 kHz)
  - [ ] `bt-keyboard-pairing` (bluetooth): Keyboards found by Scan marked *keyboard* (unnamed: 'Keyboard' + address); press the keyboard's pairing key when asked; a code to type shows
  - [ ] `settings-bluetooth-scan-pair` (settings-bluetooth): DEVICES Scan (16 s) and the list with tags (headphones, keyboard, phone, CAT, GPS, new, connected); MFK on a new device pairs and connects (
  - [ ] `settings-bluetooth-device-menu` (settings-bluetooth): Paired-device menu: Connect/Disconnect, Connect at startup (headphones), GPS for gpsd, Forget (asks again, Cancel first)
  - [ ] `main-screen-status-bt-icons` (main-screen): Bluetooth icon (blue device connected, grey on, dark off), Note (A2DP headphones), Keyboard (amber BT CAT client, grey CAT bridge ready)

## Bluetooth headphones, a phone or a PC

- **You:** Pair headphones (if you have a pair) in Settings > Bluetooth; listen where the bench asks. Pair a phone or PC once to see it offered a speaker and no microphone; a headset button as the browser's PTT.
- **The bench:** RX loopback, volume, speaker mute, A2DP only, BOTH's mix, what a PC sends dropped.
- **Your time:** 20 min
- **Left:** 11 of 11

  - [ ] `bt-headphones-pair` (bluetooth): Settings > Bluetooth DEVICES: Scan (16 s); an MFK press on a *new* device pairs, trusts and connects it and opens the pairing window; Activi
  - [ ] `bt-rx-to-headphones` (bluetooth): HEADPHONES: RX to headphones (PulseAudio loopback from alsa_input.platform-sound.stereo-fallback, mono, ~0.4-0.7 s late) and Volume (the sin
  - [ ] `bt-mute-speaker` (bluetooth): Mute speaker: AF volume and TX monitor 0 while headphones are connected, the previous values back on disconnect; AF volume does not change t
  - [ ] `bt-headset-a2dp-only` (bluetooth): Bluetooth headsets use A2DP only: no HFP/HSP microphone
  - [ ] `btaudio-headphones` (bluetooth): Bluetooth headphones: RX loopback (rx on|off), volume 0-100 %, speaker mute while connected, Connect at startup
  - [ ] `settings-bluetooth-headphones` (settings-bluetooth): HEADPHONES RX to headphones, Volume, Mute speaker; one pair at a time
  - [ ] `sentry-both-mix-headphones` (sentry-common): BOTH mixes the X6200 and the stick only on Bluetooth headphones (speaker: the stick alone, base in play mode); each radio keeps its own volu
  - [ ] `bt-cw-sidetone-headphones` (bluetooth): CW on headphones: X6100 hears its base's full-scale sidetone (late); the X6200's base puts it in at about -89 dBFS (nearly nothing, #192)
  - [ ] `bt-pc-speaker-dropped` (bluetooth): A paired PC/phone sees the radio as an A2DP speaker; what it sends is dropped and never reaches the TX audio
  - [ ] `bt-pc-no-input` (bluetooth): A PC on Bluetooth gets an audio output that plays nowhere and no input
  - [ ] `ptt-option-bt-headset` (web): Bluetooth headset button as PTT (Play/pause, Next, Previous as tap on/off, or Speaker mic held to talk) via the page as media player

## The Bluetooth GPS (GLO 2)

- **You:** Switch the GLO 2 on by a window; once, switch it off for a minute and on again.
- **The bench:** gpsd's bridge, reconnects and back-off, the GPS panel, the icon, title tag, DF heading, the web block.
- **Your time:** 15 min
- **Left:** 10 of 10

  - [ ] `btgps-gpsd-bridge` (bluetooth): btgps-bridge gives gpsd a pseudo-terminal (/var/run/btgps.tty while connected) since the kernel makes no Bluetooth serial devices; positions
  - [ ] `btgps-reconnect-backoff` (bluetooth): btgps-ctl reconnects: 10 s after a drop, doubling to 5 minutes (a new choice restarts at 10 s), ~5 s a try; log /tmp/btgps.log
  - [ ] `btgps-ctl` (bluetooth): btgps-ctl set MAC [CHANNEL] / off / status: the Bluetooth GPS gpsd reads (BTGPS_MAC, BTGPS_CH in /mnt/btcat.conf)
  - [ ] `main-screen-status-gps-icon` (main-screen): Arrow icon: green with a GPS fix in the last 10 s, grey GPS on without a fix
  - [ ] `gps-panel` (sentry-F): F SYSTEM > GPS: sky plot by system colour, signal bars, systems used/in view, LINK, GPSD device/driver/rate, FIX (2D/3D, HDOP word, PDOP), E
  - [ ] `df-gpsd-heading` (sentry-I): Heading from gpsd: a USB compass, else GPS course when moving >2 m/s (held 2 min); HEADING row 'IN USE: GPS COURSE 135 (GPSD)', N UP GPS/CMP
  - [ ] `gps-title-tag` (sentry-common): While a GPS reports every title says GPS (GPS NO FIX till it has one)
  - [ ] `settings-bluetooth-gps` (settings-bluetooth): GPS for gpsd on a paired Bluetooth device
  - [ ] `gps-reconnect` (system): GPS arrow green with a fix in the last 10 s; reconnects to gpsd after 5 s of nothing; restarts gpsd after a minute then 2, 4, 8 ... up to 30
  - [ ] `web-df-gps` (web): Web DF GPS block: gpsd fix, position, speed, heading; Bluetooth GPS choice and its link

## The hand mics (H-22 on the X6200, the X6100's)

- **You:** Press the mic keys the bench names; record a CQ, a RADE over and a voice message; speak for VOX; key the hand mic once outside the bands.
- **The bench:** Reads state after each key; plays the recordings over the cable at 0.1 W to the other radio.
- **Your time:** 25 min
- **Left:** 15 of 15

  - [ ] `input-handmic-band-digits` (input): Hand mic digits 1-9, 0 (and H-22 dot = 10 m) select the band printed on the key (X6100 mic and H-22 maps differ); long press recalls memory 
  - [ ] `input-handmic-frequency-box` (input): F-INP (H-22 red F-INP/ENT) opens the frequency box, digits and dot type MHz, CE deletes, F-INP again tunes; not while locked; digits type in
  - [ ] `input-handmic-f-keys` (input): Hand mic F1, F2 (H-22 F3) press/long press take the action set in Settings > Keys > HAND MIC KEYS (F1 steps up/down by default)
  - [ ] `input-handmic-up-down-channels` (input): UP/DOWN (H-22 arrows) recall the channel before/after with its name (LOCK lets it through); long press band up/down; H-22 side keys change b
  - [ ] `input-handmic-mode-vm` (input): MODE: next mode (LSB, USB, CW, CW-R, AM, NFM, X6200 SAM/WFM), long press DIG on/off in SSB; V/M opens the CHANNELS window
  - [ ] `input-handmic-h22-vol-ab` (input): H-22 VOL+/VOL- step the RX volume (held: auto-repeat ~10/s from 0.4 s); A/B toggles VFO, long press copies VFO
  - [ ] `input-handmic-x6100-keys` (input): X6100 mic keys FIL (filter pages / DFL long), XFC (VFO A/B), SPCH (lock), TUNER (tuner on/off, long = tune cycle, transmits), GENE (SWL pane
  - [ ] `channels-hand-mic-keys` (app-channels): Hand mic V/M opens the window (not over WeFax, NavTex, SSTV, JS8, PSK, Olivia, JTTY); UP/DOWN tune the channel after/before the last one tun
  - [ ] `field-rec-cq` (app-field-ops): Rec CQ (page 2): record the voice CQ from the hand mic, 60 s max, Stop rec; kept as 'CQ SOTA' / 'CQ POTA' in /mnt/msg (MSG > Voice lists it)
  - [ ] `rade-rec` (app-rade): Rec from the hand mic (up to 60 s) encoded as you speak, receiver off meanwhile, 'encoding' then 'over ready'; dropped if the radio keys; bu
  - [ ] `rade-send` (app-rade): Send keys at RADE's own Power (radio PWR and the apps' level untouched); the End-of-Over carries your callsign; Send again, Discard, Stop; r
  - [ ] `voice-msg-hand-mic-only` (system): Voice messages record only from the hand mic
  - [ ] `voice-msg-record-hand-mic` (tx): Voice messages (MSG > Voice Rec, Field Ops Rec CQ) record only from the hand mic; without it 'No audio: recording uses the hand mic' after 3
  - [ ] `settings-audio-vox` (settings-audio): VOX On, Gain, Anti-VOX, Delay
  - [ ] `tx-base-hold-outside` (tx): Outside the bands the base is held for what it keys itself (hand mic PTT, VOX, KEY jack): PWR 0 W, VOX off, KEY Train on; first such key say

## WSPR and SWR Scan (opened only at the radio)

- **You:** Open WSPR, then SWR Scan, at the radio (neither opens remotely, by design); press TX next / Run when asked.
- **The bench:** Everything after the open over the cable at 0.2-1 W: decodes, TX %, power, hop, the ATU rule, the transverter limit, sweeps, spans, band limits, refusals, the look.
- **Your time:** 40 min
- **Left:** 15 of 15

  - [ ] `wspr-open` (app-wspr): APP > Modes > WSPR tunes the nearest WSPR dial (USB-D, WSJT-X's list), band keys step; always opens with TX % 0 (also after a power-off)
  - [ ] `wspr-rx-decode` (app-wspr): Every RX cycle decoded at 1:54: table with UTC, band, SNR, DT, audio Hz, drift, call, grid, dBm, km; band of the cycle heard
  - [ ] `wspr-tx-percent-next` (app-wspr): TX % (0/10/20/33/50, random cycles) and TX next (one beacon next cycle, again cancels; TX % 0 cancels a chosen one); without callsign or loc
  - [ ] `wspr-stop-tx` (app-wspr): Stop TX (the TX next button while a beacon is on) takes it off at once, TX % 0, TX next off; closing WSPR (ESC) also stops and sets TX % 0
  - [ ] `wspr-power` (app-wspr): WSPR's own Power (page 2): 0.1/0.2/0.5/1/2/5 W, 0.2 W default; the radio's PWR is not changed (beacon at WSPR's power, back after)
  - [ ] `wspr-knob-offset` (app-wspr): Main knob moves the TX audio offset (1410-1590 Hz)
  - [ ] `wspr-hop` (app-wspr): Band hopping (page 2 F5 / page 3 F4): WSJT-X's coordinated schedule by minute (:00 160m ... :18 10m), Off slots shared in turn (6 m takes Of
  - [ ] `wspr-hop-bands-page3` (app-wspr): Page 3 band table: F2 Band, F3 Use Off/RX/RX+TX, F4 Hop, F5 Go to; columns frequency, use, slots, ATU match Stored/None
  - [ ] `wspr-hop-atu-rule` (app-wspr): With the ATU on, an RX+TX band transmits on its hop only when an ATU match is stored for its WSPR frequency ('40m: no ATU match stored, RX o
  - [ ] `transverter-wspr-at-limit` (app-wspr): WSPR's beacon goes out at the transverter limit when its own Power is higher, still reporting its own Power
  - [ ] `swrscan-run-stop` (app-swrscan): SWR Scan (APP 1:6): Run sweeps a carrier (AM X6100, FM X6200) around the TX frequency (split's other VFO) and draws SWR; Stop or closing res
  - [ ] `swrscan-x6200-sweep-mode` (app-swrscan): X6200 Sweep: Carrier (default: plain FM carrier, waits for power, 3 readings averaged, ends first then the gaps, a thin line marks the point
  - [ ] `swrscan-span-scale` (app-swrscan): Span 50/100/200/500/1000/1500/2000/2500 kHz and Scale Log/Linear (not while scanning); 50 readings (25 with Carrier), 100 from 1000 kHz
  - [ ] `swrscan-band-limited` (app-swrscan): The sweep stays inside the band of the band list for the IARU region (Region 1 40 m 7.000-7.200), moves inside near the HF/transverter/band/
  - [ ] `swrscan-refusals` (app-swrscan): Run refused while transmitting or tuning ('SWR scan: not while transmitting or tuning'), out of band ('No TX on ...'), without base data; th

## At the radio: knobs, lights, pairing, boot, by ear

- **You:** Turn the X6200's MFK inner ring; a knob while a browser has control; look at the key lights; Pair code and Forget all (the bench pairs again after); watch one reboot from the logo; listen to the boot sound, a scan and Internet radio replacing the receiver.
- **The bench:** Reads state and the remote's control line meanwhile.
- **Your time:** 20 min
- **Left:** 11 of 11

  - [ ] `input-x6200-mfk-inner-ring` (input): X6200 MFK inner ring chooses the MFK's control (and moves the walk frame); outer ring changes the value without a press
  - [ ] `control-local-takes-back` (remote): The radio's own keys and knobs always take control back ('At the radio')
  - [ ] `settings-display-buttons-brightness` (settings-display): Buttons brightness Always Off / Always On / Temporarily On (the key lights, through the base)
  - [ ] `pair-code-at-radio` (system): Settings > General > BROWSER REMOTE > Pair code shows a code at once and starts the waits over
  - [ ] `sessions-forget-all` (system): Settings > General > BROWSER REMOTE > Forget all revokes every browser (and the .bak); the next pairing is the new admin
  - [ ] `boot-picture-at-start` (system): At start the picture fades in over ~3 s from its top-left pixel's colour (transparent shows black), ending with 'CQ CQ' when the boot sound 
  - [ ] `boot-dark-before-video` (system): Before the video the screen stays dark: no kernel logo, no U-Boot picture on the X6200
  - [ ] `settings-audio-boot-sound` (settings-audio): Boot sound: CQ CQ in CW with the boot logo, quiet, at volume 28 when VOL is lower; off by default; none with Speaker always off
  - [ ] `iradio-replaces-receiver` (app-iradio): While a station plays the speaker plays it in place of the receiver; the receiver comes back at Stop
  - [ ] `scanner-quiet-while-looking` (app-scanner): The speaker is quiet while the scan looks, so there is no burst of noise at every step
  - [ ] `digi-tx-monitor-muted` (tx): With AF volume 0 or the speaker muted (mute action, BT headphones Mute speaker, a TCI client transmitting) the TX monitor does not play the 

## Paddles and CW keyers (the ones you have)

- **You:** Plug or pair each you own (TinyMIDI, a Vail-type USB adapter, a mouse-type or MIDI or serial paddle, SmartCW) and key a few letters; Learn and Swap once.
- **The bench:** Reads its own 'cw' text with Train on (no TX); SmartCW's network and page through the radio.
- **Your time:** 10 min per device
- **Left:** 14 of 14

  - [ ] `cwtrainer-key-modes` (app-cw-trainer): KEY 2:2 Mode for the trainer: Manual (straight key; an X6200 KEY-jack paddle acts as a sideswiper; USB/BT paddle dit only), Auto-L/Auto-R ia
  - [ ] `tinymidi-paddle` (cw): N6ARA TinyMIDI (Keyboard Mode, BLE) pairs as a keyboard: dit '[' and dah ']' key the radio in CW (volume keys out of CW), following Speed, I
  - [ ] `usb-paddle-keyboard-type` (cw): Keyboard-type USB paddle (e.g. Vail adapter): Left Ctrl dit, Right Ctrl dah, [ ] always; its press stops a CW message on the air
  - [ ] `usb-paddle-mouse-type` (cw): Mouse-type USB paddle: left button dit, right dah; an ordinary mouse is not taken
  - [ ] `usb-paddle-midi` (cw): USB MIDI paddle (experimental): notes 1 dit, 2 dah; one MIDI device, the first plugged
  - [ ] `usb-paddle-serial` (cw): USB serial paddle (CH340, FTDI, CP210x, PL2303, CDC ACM): CTS dit, DSR dah; Serial port off by default; first ttyUSB/ttyACM, never the radio
  - [ ] `usb-paddle-iambic` (cw): USB paddles follow KEY mode, Iambic A/B (squeeze release), speed and ratio; a contact pressed during an element and held after the other is 
  - [ ] `usb-paddle-learn-swap` (cw): SET > KEYS > PADDLE: connected kinds, Learn dit and dah (10 s each, saved per kind), Swap dit and dah for all USB paddles
  - [ ] `settings-keys-paddle` (settings-keys): PADDLE: USB paddles Connected, dit/dah of each kind, Learn dit and dah, Swap, Serial port
  - [ ] `smartcw-usb-network` (cw): SmartCW recognised by itself: USB network (SmartCW 192.168.7.1, radio .16-.23), never the default route or DNS, not on the ETHERNET card; it
  - [ ] `smartcw-web-proxy` (cw): SmartCW's own page through the radio on port 8073 (Operating > SmartCW while plugged), the whole page working, plain http
  - [ ] `smartcw-midi-keying` (cw): SmartCW MIDI: keyed mode (note 65 keyer, 64 straight key) keys as SmartCW times it (its text, memories, beacon go out; radio KEY mode/swap d
  - [ ] `web-smartcw-link` (web): Operating > SmartCW: SmartCW's own page on port 8073, in the menu while SmartCW is plugged in
  - [ ] `tci-vfo-knob` (tci): VFO-Knob (AetherSDR firmware) works the radio over TCI: tunes, mode, filter, AGC, PTT, meters, RX audio, mic

## Power and battery (a bench supply, a battery run)

- **You:** Run a radio on battery down to its warnings; a bench supply under 12 V on the X6200; one plain poweroff and a power cycle; stop the base's data (cable or base off) for its warnings.
- **The bench:** Logs voltages and the warnings; checks the clock chip and the shutdown's 's'.
- **Your time:** an afternoon on battery, 20 min otherwise
- **Left:** 10 of 10

  - [ ] `system-low-battery-warnings` (system): On battery at <=10 % a message every minute ('Battery 8%: about 15 min left'), from 5 % 'Battery 5%: connect power'
  - [ ] `system-low-battery-poweroff` (system): At 6.0 V (X6200 7.0 V) 'Low battery! Turning off in 30s', then the charger set back On, saved and switched off; called off at 7.2 V (X6100 6
  - [ ] `system-x6200-dcin-under-12v` (system): X6200: a supply read under 12.0 V does not stop the low-battery power-off ('Low battery, DC IN under 12 V! Turning off in 30s.'); 12.0 V cou
  - [ ] `system-x6200-base-warnings` (system): X6200 base warnings 'BATTERY HIGH TEMP!', 'BATTERY LOW TEMP!', 'PA HIGH TEMP!', 'DCIN TOO LOW!' shown when one comes on and every 30 s while
  - [ ] `system-battery-percent-from-voltage` (system): The shown % is checked against the voltage on battery (voltage's % shown when >25 points apart, not from 7.3 V down; a base at 0 % at >=7.2 
  - [ ] `settings-txcal-x6200-att-supply-battery` (settings-txcal): X6200 two attenuator figures per band (on supply / on battery), switched after 5 s steady, defaults from the table, older single values migr
  - [ ] `no-base-data-warnings` (system): No data from the base: after 30 s in RX (60 s after start) four messages of 4 s; base_i2c.log 'no data from the base for N ms'; TX refused '
  - [ ] `panel-ptt-refused-no-base-data` (remote): PTT refused while the base sends no data ('no data from the base')
  - [ ] `system-x6200-shutdown-poweroff` (system): Every X6200 shutdown ends with 's' to the base (a plain reboot or poweroff switches it off), repeated every 4 s for up to a minute
  - [ ] `clock-chip-start` (system): A clock chip that came stopped (STOP/TEST1 bit) is started at boot ('Clock chip ... was stopped ... started'); the time then kept through po

## SD cards and firmware (a spare card; your call on each)

- **You:** A spare SD card for new-card starts (keys restored, DATA formatted, files found on system); a power cut mid-update; the kernel and U-Boot boxes; Xiegu base firmware fetched and flashed on each radio; the X6100 test base.
- **The bench:** Writes the card image, reads start_trace and the journal after each start.
- **Your time:** an hour
- **Left:** 13 of 13

  - [ ] `ssh-keys-restore-new-card` (system): At the first start of a newly written card the keys and host keys come back from backups/backup.tgz
  - [ ] `data-partition-format` (system): The first start recognises an unformatted DATA partition, formats it FAT32 and moves the system /mnt files onto it; exFAT/NTFS/FAT16/other o
  - [ ] `found-on-system` (system): Files written into the system /mnt while DATA was not mounted are put in found_on_system/ (.2, .3 ...) at the next start that mounts DATA
  - [ ] `start-trace-boot` (system): start_trace.txt at boot: 'boot: GoldCorpHF ..., card image ...', S00adata start/end per script with exit N, 'init scripts done'; start_trace
  - [ ] `system-update-boot-journal` (system): Card-part steps journalled in /var/lib/x6100-update/boot-journal.json: a later failure puts everything back, and a power cut while they are 
  - [ ] `settings-version-rollback` (system): 'Settings not usable by this version': defaults not saved; an update is taken back to the previous version after three such starts
  - [ ] `system-update-allow-unsigned-old-image` (system): A radio on an old card image (RF.Guru's updater) takes this fork's bundle once after `touch /mnt/update/allow-unsigned` and the web Update p
  - [ ] `system-update-kernel-uboot-tickboxes` (system): The update box offers unticked 'Also update the kernel on the SD card (6.1.x)' and 'Also update the bootloader (U-Boot) on the SD card' only
  - [ ] `f-system-base-firmware` (system): BASE FIRMWARE (X6200): fetches any Xiegu X6200 base release from xiegu.eu and writes it as Xiegu's upgrade does; the base powers off after; 
  - [ ] `system-x6200-base-fw-get` (system): Get (F2) streams Xiegu's update zip from xiegu.eu and keeps only the base file in /mnt/base_update
  - [ ] `system-x6200-base-fw-flash` (system): Flash (F4) then FLASH NOW writes the chosen base (~30 s, log /mnt/base_update.log; UNKNOWN files asked again); the base switches the radio o
  - [ ] `system-x6100-base-flash` (system): flash.sh --flash --file on the X6100 (FLASH typed): stops the GUI, writes ~15 s, restarts the GUI, no switch-off; known files by SHA-256 (V1
  - [ ] `tx-x6100-test-base` (tx): X6100 test base (V1.1.9 tx1a2-test): recognised by version string, TEST ONLY notes, compressor/CESSB off and TX filter 300-3000 Hz at each s

## WiFi, Ethernet, tethering, a security key

- **You:** Switch the access point off and on; a second access point (and an 8723bu-shy one if you have it); the radio's WiFi switch (drops the bench's link: switch it back at the radio); a USB Ethernet adapter on a switch with no gateway; an Android phone tethering; a FIDO2 key for SSH.
- **The bench:** Watches NetworkManager's retries and joins over SSH once back.
- **Your time:** 30 min
- **Left:** 7 of 7

  - [ ] `network-wifi-auto-retry` (network): Failed self-started joins retried with the best saved network (15 s, 30 s, 1, 2, 5 min, then every 10 min); after six failures the chip is p
  - [ ] `settings-wireless-wifi-join` (settings-wireless): Connect: password 8-63 (hand mic digits type), open joins at once, WPA2/WPA3 in WPA2, WEP/Enterprise/OWE/WPA3-only/hidden refused with a mes
  - [ ] `network-x6100-8723bu-workaround` (network): X6100: access points that never answer 8723bu's association (MediaTek GL-MT3000/MT3600BE, UniFi U6+) fixed in the card image's driver; rtl8x
  - [ ] `settings-wireless-wifi-power` (settings-wireless): WiFi/BT power switch (one chip: Bluetooth off with it)
  - [ ] `ethernet-no-gateway` (network): A cable without a gateway reaches only its subnet: WiFi stays on with the internet; Static with empty Gateway and DNS; DHCP with no server r
  - [ ] `usb-tethering` (network): Android USB tethering on the USB-A port gives the radio an address (experimental); not on the ETHERNET card; no iPhone
  - [ ] `ssh-fido-login` (system): Logins with FIDO2 security keys (verify-required asks the PIN)

## A mismatched load, and an HF antenna

- **You:** Put a mismatched load (SWR 3) on one radio for SWR protection, the tuner's give-up and TUNE above 1 W; an HF antenna for NAVTEX, DSC, WWV and busy FT8/JS8 bands (receive, and FT8/JS8 replies only on your word).
- **The bench:** Runs the TX at the power you allow; decodes and compares.
- **Your time:** 30 min with the load; antenna tests run unattended once it is up
- **Left:** 11 of 11

  - [ ] `settings-txcal-swr-protection` (settings-txcal): SWR protection Off or 1.5-5.0 (2.0): TX stops after 0.3 s over ('TX stopped: SWR 3.4'), not during ATU tune or external TUNE, nothing locked
  - [ ] `atu-tune-giveup` (tx): Radio's tuner (ATU key held / 'tuner tune'): 'Tuner did not start' if the base does not key in 3 s, 'Tuner: no match' 1 s after the base ret
  - [ ] `ext-tune-atu-off-above-1w` (tx): Above 1 W TUNE switches the radio's ATU off first ('TUNE 5.0 W, 15 s, ATU off') and it stays off; the ATU key during such a tune stops it ('
  - [ ] `ext-tune-swr-protection-window` (tx): During TUNE the SWR protection stands aside only for readings up to twice the TUNE power + 0.2 W and until the time limit + 1 s; everything 
  - [ ] `navtex-decode` (app-navtex): NAVTEX (SITOR-B) text decoding
  - [ ] `dsc-rx` (app-navtex): DSC receive: one row per call (time, category, MMSI + country, nature/called MMSI, proposed mode/frequency), distress in red with position, 
  - [ ] `timesig-decode-and-set-clock` (app-timesig): Decodes WWV/WWVH ticks (1000/1200 Hz), the 100 Hz time code minute by minute (date, UTC, DUT1, leap warning) and after two good minutes (thr
  - [ ] `ft8-auto-pota-first` (app-ft8): With Auto, when several stations call in the same slot a POTA activator is answered first
  - [ ] `ft8-auto-sync` (app-ft8): Auto Sync (page 5, off by default): FT8/FT4 clock moved by the median DT of >= 5 agreeing stations in >= 3 slots within 4 min (>= 0.2 s, mid
  - [ ] `js8-relay` (app-js8): Relays and store-and-forward through another station (not yet proven on the air)
  - [ ] `js8-aprs` (app-js8): APRS > through JS8 gateways: grid/GPS position beacons (with a message), POTA/SOTA spots, SMS, email, Winlink

## SDR hardware by hand

- **You:** Unplug and replug a stick (B's, a watch channel's); an unpowered hub; a bladeRF with an older firmware file; the WR12 if you have one; your DF array on a known-bearing source; say yes to the G WALKIE SDR transmitting on an amateur channel into a dummy load.
- **The bench:** Watches the pool, the daemon and the picture.
- **Your time:** 30 min
- **Left:** 11 of 11

  - [ ] `sentry-stick-hotplug` (sentry-B): A stick plugged in joins RTL/BOTH audio at once on any screen; pulled out or gone a moment changes nothing (RECEIVER says why), heard again 
  - [ ] `pool-serial-hold-reassign` (sentry-pool): B and watch channels held by serial and USB location; a stick gone for good is replaced after 3 s by the open/free one (RTL first), a stick 
  - [ ] `sdr-hub-brownout-kernel` (sdr): Kernel patches (bundles after v0.3.12.26 with the kernel box ticked) keep a browning-out hub from hanging the radio
  - [ ] `sdr-bladerf-fw-update` (sdr): bladeRF USB firmware written from the radio when /mnt/bladerf holds a newer bladeRF_fw_vX.img (tried once per file)
  - [ ] `g-sdr-tx-fm` (sentry-G): SDR PTT transmits hand-mic audio as narrow FM (300-3000 Hz, pre-emphasis, 4 kHz clip, <5 kHz peak) while held; ends after 3 min, 2 s without
  - [ ] `main-screen-wr12-tag` (main-screen): WR12 tag in the frequency strip (info colour, left of NR NB DNF; under the other VFO in Black Metal/Total Black; not in Amber Trey) while th
  - [ ] `radio-core-wr12-setup` (radio-core): With WR12 on HOST the radio finds the WR12 (USB 0483:5740), sets it up (AT+INIT, gains, AGC), bypass; 'WR12 found on HOST' / 'WR12 unplugged
  - [ ] `radio-core-wr12-receive` (radio-core): From 54 MHz the WR12 is tuned (AT+FREQ) and out of bypass, the radio's receiver on its 5.06 MHz IF (I/Q swapped); screen, S-meter band setti
  - [ ] `system-wr12-bypass-on-exit` (system): A GUI stop/restart/update/shutdown puts the WR12 back in bypass; after a crash the start loop does ('x6100-wr12-bypass: ttyACM0: AT+BYPASS=1
  - [ ] `tx-wr12-inhibit` (tx): While the WR12 is out of bypass (or may be) nothing transmits on any frequency, split on HF too (PTT, apps, CW, TUNE, tuner, SWR scan: 'WR12
  - [ ] `df-antenna-calibration` (sentry-I): Antenna-side calibration on a known-bearing source (F2 ANT CAL or web 'Calibrate on it'): phase per antenna taken out, kept in /mnt/df_antca

## What the bench runs alone, still to write and run

No hands: over the cable between the radios (your cable word, at most 1 W), the SDRs on HOST, the HackRF into the X6200 (receive only), or nothing at all.

- **cable** (112): cw 18, tx 12, app-ft8 10, app-js8 9, app-sstv 8, winlink 8, app-psk 4, web 4, settings-txcal 4, app-field-ops 4, system 3, settings-display 2, lookfeel 2, sentry-scancfg 2, radio-core 2, remote 2, app-wefax 2, app-cw-kbd 2, app-walkie 2, app-scanner 2, sentry-G 2, settings-audio 2, sentry-A 1, app-bands 1, app-propagation 1, display 1, app-iradio 1, app-rade 1
- **rtl** (51): sentry-B 11, sdr 9, sentry-H 7, sentry-pool 4, sentry-common 4, sentry-I 3, sentry-G 3, sentry-E 3, network 2, sentry-D 1, tx 1, sentry-A 1, sentry-C 1, sentry-scancfg 1
- **hackrf** (4): sentry-B 3, sentry-G 1
- **none** (302): system 31, tx 29, app-field-ops 25, settings-display 20, remote 16, display 15, web 15, settings-general 10, app-js8 9, app-cw-trainer 8, app-first-start 7, main-screen 7, sentry-common 7, tci 7, cw 6, app-walkie 5, settings-audio 5, winlink 5, app-zello 5, radio-core 5, app-channels 4, app-wefax 4, app-ft8 3, network 3, settings-info 3, sentry-C 3, sentry-G 3, sentry-F 3, app-sstv 2, app-bands 2, settings-txcal 2, app-rade 2, settings-walkie 2, input 2, app-hf-air 2, lookfeel 2, app-flights 2, sentry-I 2, app-svx 2, settings-bluetooth 2, app-birdie 1, app-cb 1, sentry-H 1, app-weather 1, settings-wireless 1, app-cw-kbd 1, sentry-E 1, sentry-scancfg 1, app-psk 1, sentry-A 1, sentry-D 1, app-wspr 1, wavelog 1, app-propagation 1, app-timesig 1
