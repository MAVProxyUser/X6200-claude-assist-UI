# The X6100 and the X6200 side by side, v0.3.12.36

Every screen of both radios at the same moment, from `tools/bench/suite.py run core` on 2026-11-19 at 23:33-23:44 EST,
both radios on **v0.3.12.36 as released** (566566dc), each updated through its own updater (`x6100-update upgrade
--kernel --uboot --reboot`), not a hot build: the X6100's own driver on the X6100, the Sentry theme, and the same
settings (the X6100 made like the X6200 for the pictures, its own back after; 0 settings different). Each app the
radios have and every page of the Sentry, photographed on both over SSH and put side by side, the X6100 on the left.
The suite: 56 pass, 1 to look at, 1 fail. The run before, 2026-11-19 morning on the builds of v0.3.12.31, is in
[../2026-11-19](../2026-11-19/README.md).

- **New on the X6100 since then**: its update brought Weather, Flights and Internet radio their helpers (on its card
  v0.3.12.28 they had none), and both radios restart by themselves after an update (the X6100 by kexec).
- **To look at**: RADE did not open on either (it opens only with Bluetooth headphones or a TCI client), as before.
- **The fail**: SSTV on the X6100 took longer than the suite's 15 s to open (9-17 s on 2026-11-18): its picture was
  taken before it had tuned to SSTV's calling frequency (it still shows 14.062.700 CW, the X6200 14.230.000 USB).
- The X6200's Bluetooth GPS keeper was paused for the run: v0.3.12.36's keeper calls a GPS that is not there every
  16 s, which stalls the X6200's WiFi for seconds (the first run stopped on it); fixed after this release.

Each picture's colours are counted against the Sentry theme's gold on black: **cool** is cyan, blue or purple, what
a screen still in an old theme shows (flagged over 1 %).

**Redacted:** what would place the station (its position, its locator, the networks and stations around it) is
pixelated or flattened in place, on both radios alike; the screens it touches say what. Checked by eye and by OCR
(every picture read for locators, coordinates and the WiFi's names: none found).

## Sentry A: MF/HF/VHF spectrum (the radio's own receiver)

![Sentry A: MF/HF/VHF spectrum (the radio's own receiver)](sentry_A.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Sentry B: HF/VHF/UHF spectrum (the RTL-SDR)

![Sentry B: HF/VHF/UHF spectrum (the RTL-SDR)](sentry_B.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the local FM station's RDS name and call letters.

## Sentry C: Signal log

![Sentry C: Signal log](sentry_C.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Sentry D: Band intelligence

![Sentry D: Band intelligence](sentry_D.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Sentry E: Decoder sentry

![Sentry E: Decoder sentry](sentry_E.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Sentry F: System control

![Sentry F: System control](sentry_F.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Sentry G: Walkie-talkie

![Sentry G: Walkie-talkie](sentry_G.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Sentry H: AIRSPACE (ADS-B)

![Sentry H: AIRSPACE (ADS-B)](sentry_H.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the GPS position, the scope, the aircraft's distances and bearings, the target's position.

## FT8 (APP 101)

![FT8 (APP 101)](app101_FT8.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## GPS (APP 103)

![GPS (APP 103)](app103_GPS.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: latitude, longitude and grid.

## Settings (APP 104)

![Settings (APP 104)](app104_Settings.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the QTH locator.

## Recorder (APP 105)

![Recorder (APP 105)](app105_Recorder.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## WiFi (APP 108)

![WiFi (APP 108)](app108_WiFi.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the WiFi networks' names.

## WeFax (APP 109)

![WeFax (APP 109)](app109_WeFax.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## NavTex (APP 110)

![NavTex (APP 110)](app110_NavTex.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## JS8 (APP 111)

![JS8 (APP 111)](app111_JS8.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## SSTV (APP 112)

![SSTV (APP 112)](app112_SSTV.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Cluster (APP 113)

![Cluster (APP 113)](app113_Cluster.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## CW Trainer (APP 115)

![CW Trainer (APP 115)](app115_CW_Trainer.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## CW Keyboard (APP 117)

![CW Keyboard (APP 117)](app117_CW_Keyboard.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Field Ops (APP 118)

![Field Ops (APP 118)](app118_Field_Ops.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Bands (APP 119)

![Bands (APP 119)](app119_Bands.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Snake (APP 120)

![Snake (APP 120)](app120_Snake.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## PSK31 (APP 121)

![PSK31 (APP 121)](app121_PSK31.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Olivia (APP 122)

![Olivia (APP 122)](app122_Olivia.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## JTTY (APP 123)

![JTTY (APP 123)](app123_JTTY.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Channels (APP 124)

![Channels (APP 124)](app124_Channels.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Birdie Cal (APP 125)

![Birdie Cal (APP 125)](app125_Birdie_Cal.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Propagation (APP 126)

![Propagation (APP 126)](app126_Propagation.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the map (its station marker).

## Scanner (APP 127)

![Scanner (APP 127)](app127_Scanner.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Airspace (APP 130)

![Airspace (APP 130)](app130_Airspace.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the GPS position, the scope, the aircraft's distances and bearings, the target's position.

## Time Signal (APP 132)

![Time Signal (APP 132)](app132_Time_Signal.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## On the Air (APP 133)

![On the Air (APP 133)](app133_On_the_Air.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Direction finder (APP 134)

![Direction finder (APP 134)](app134_Direction_finder.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Weather (APP 200)

![Weather (APP 200)](app200_Weather.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the place's name, sunrise and sunset, and the locator the header shows with no GPS. Both answered by their helper now (the X6100's came with its update).

## Internet radio (APP 201)

![Internet radio (APP 201)](app201_Internet_radio.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Zello (APP 202)

![Zello (APP 202)](app202_Zello.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## Flights (APP 203)

![Flights (APP 203)](app203_Flights.png)

Cool colour: X6100 0.0 %, X6200 0.0 %. Redacted: the map, the distances and directions, and the locator the header shows with no GPS. Both answered by their helper now (the X6100's came with its update).

## HF Air Band (APP 204)

![HF Air Band (APP 204)](app204_HF_Air_Band.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## CB (APP 205)

![CB (APP 205)](app205_CB.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.

## SVXConnect (APP 206)

![SVXConnect (APP 206)](app206_SVXConnect.png)

Cool colour: X6100 0.0 %, X6200 0.0 %.
