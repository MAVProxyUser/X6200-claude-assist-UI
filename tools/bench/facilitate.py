#!/usr/bin/env python3
"""The tests the owner facilitates: every feature of features.json that needs internet services, Bluetooth, GPS or the
owner's hands, in sessions (what the owner does, what the bench does, roughly how long), each feature marked done once
the latest coverage run (orch.py coverage) covers it. What the bench runs alone (the cable between the radios, the SDRs
on HOST, the HackRF, nothing at all) is counted by area at the end.

    python3 facilitate.py            # FACILITATE.md beside it, from the latest runs/*-coverage
    python3 facilitate.py --check    # every owner/BT/GPS/internet feature placed in one session, no unknown ids

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OWNER_NEEDS = ("internet", "bt", "gps", "owner")

# (key, title, what the owner does, what the bench does, about how long with the owner, ids)
SESSIONS = [
    ("ok-bt-client", "The X6100 as the X6200's Bluetooth client (your OK, no hands)",
     "Say yes once: the X6100 pairs with the X6200 as a Bluetooth CAT/TCI client (Just Works, in the X6200's "
     "3-minute pairing window, opened over SSH). Both radios keep that pairing until it is forgotten.",
     "Every CI-V command over SPP, transceive, the paired-only rule, TCI over RFCOMM with its ask-before-TX and "
     "watchdog (TX at 0.1 W over the cable), the Bluetooth CAT card and icon.",
     "none (about an hour of the bench's time)",
     ["btcat-paired-only", "btcat-ci-v-over-rfcomm", "btcat-spp", "civ-read-freq-mode", "civ-set-freq-mode",
      "civ-transceive", "civ-vfo", "civ-split-step-att", "civ-levels", "civ-meters", "civ-funcs", "civ-id",
      "civ-band-filter-data", "civ-ptt", "civ-sel-vfo", "civ-scope-stubs", "civ-rit", "civ-unknown-ng",
      "bt-tci-websocket", "bt-tci-ask-before-tx", "bt-tci-follows-tci-switch", "bt-tci-ptt-watchdog",
      "settings-bluetooth-cat"]),
    ("ok-internet-read", "Internet, read only, no account (your OK, no hands)",
     "Say yes: the radios fetch what these apps fetch for anyone (Open-Meteo and the weather services with the "
     "position rounded to 0.1 deg, airplanes.live/adsb.lol, radio-browser and the streams, hamqsl.com, EiBi, "
     "MyMemory for JS8 translation, xiegu.eu's downloads page, this fork's GitHub releases, NTP).",
     "Weather, Flights, Internet radio, Conditions, the broadcast list, JS8/GhostNet translation, the daily update "
     "and base checks, the update box and Settings > Info > Install (the bundle installed is this fork's latest "
     "release, put on the radio like any update), Xiegu's page read (Check only), NTP setting the clock.",
     "none",
     ["weather-now-view", "weather-5days-view", "weather-warnings-view", "weather-update-polling",
      "weather-privacy-place-name", "weather-warning-services", "weather-forecast-warnings",
      "weather-notices-background", "flights-plot-list", "flights-update-polling", "flights-source-privacy",
      "iradio-open-lists", "iradio-play-stop", "iradio-favourite", "iradio-list-country-search",
      "iradio-error-reasons", "iradio-network-policy", "prop-conditions-fetch", "settings-info-broadcast-list",
      "js8-translate", "ghostnet-translate-arriving", "ghostnet-mine-into-english", "system-update-daily-check",
      "system-update-apply-prompt", "system-update-install-settings", "system-x6200-base-fw-check",
      "system-x6200-daily-base-check", "network-ntp-clock"]),
    ("ok-cluster", "The DX cluster under your callsign (your OK, no hands)",
     "Say yes to the radios logging in to a DX cluster as you, read only (they carry test calls Q1TST/Q2TST, which "
     "clusters refuse), or name a cluster that takes a test call. Nothing is spotted.",
     "The Cluster app, its filters and alerts, spots on the waterfall and the column, spot snap, the Bands tile, "
     "the DX icon, the remote panel's spots, Log QSO's spot call, FT8's POTA tag (spots from pota.app).",
     "none",
     ["cluster-latest-spots-and-age", "cluster-app-band-list", "cluster-mfk-tune-spot", "cluster-vhf-spots-transverter",
      "cluster-mode-view-filter", "cluster-filters-mode-band", "cluster-filter-activity", "cluster-activator-feeds",
      "cluster-web-age-skip-spotters", "cluster-heard-in", "cluster-alerts", "cluster-spots-on-waterfall",
      "cluster-column", "cluster-spot-snap", "settings-cluster-filters", "settings-cluster-view",
      "settings-display-cluster-overlay", "panel-spots", "main-screen-status-dx-icon", "cluster-status-line",
      "log-quick-qso-spot-call", "bands-activity-rows", "field-bands-screen", "ft8-pota-tag"]),
    ("ok-public-posts", "Public posts under your callsign (your yes for each)",
     "Each of these publishes on the internet under your call: say which (if any) to post. Their first press "
     "(what would be posted, nothing sent) the bench checks without asking.",
     "POTA self-spot and re-spots, the DX cluster spot, FreeDV Reporter, WSPRnet upload, PSK Reporter (off on "
     "both radios while they carry test calls).",
     "a few minutes each, while you are really on the air",
     ["field-pota-spot-two-press", "field-pota-auto-spots", "field-spot-dx-cluster", "rade-freedv-reporter",
      "wspr-upload", "settings-cluster-psk-reporter", "pskreporter-upload"]),
    ("acct-zello", "Zello (your account and a test channel)",
     "Type a Zello account and a channel only you use into the radio's web Zello page; talk into the hand mic "
     "once.",
     "Logon test, connect, receive, the link both ways over the cable at 0.1 W (CW ID, limits, yielding), the "
     "radio's own TX pausing it, Internet radio ducking for it.",
     "10 min (then the bench alone)",
     ["zello-web-test", "zello-connect-channel", "zello-receive-plays", "zello-connection-lifecycle",
      "zello-link-status-persist", "zello-talk-hand-mic", "iradio-ducks-for-zello-svx",
      "tx-simplex-repeaters-ham-only", "zello-ptt-captured", "zello-pauses-during-radio-tx", "zello-link-zello-to-rf",
      "zello-link-rf-to-zello", "zello-link-id-limit-pause", "zello-link-yields"]),
    ("acct-svx", "SVXLink reflector (your certificate)",
     "Get a signed certificate from a reflector and enter it on the radio's SVXConnect page; talk once.",
     "Connect, talkgroups, priority pull, receive, the link both ways over the cable.",
     "10 min (then the bench alone)",
     ["svx-connect", "svx-talkgroups-lock-recent", "svx-receive-plays", "svx-link-priority-pull",
      "svx-link-status-persist", "svx-talk-hand-mic", "svx-ptt-captured", "svx-link-directions",
      "svx-link-id-limit-pause"]),
    ("acct-logs", "Wavelog, QRZ Logbook, Winlink (your accounts)",
     "Type a Wavelog URL and API key (yours, or a throwaway Wavelog), a QRZ Logbook key, Pat's Winlink password "
     "into the radio's pages. Test QSOs go to them under the test call; delete them after.",
     "Wavelog test, uploads, station profile, refusals; QRZ upload; Winlink over telnet and the gateway list.",
     "10 min",
     ["wavelog-setup-test", "wavelog-upload-qsos", "wavelog-station-profile-auto", "wavelog-refused-and-edits",
      "field-qrz-upload", "winlink-telnet-and-gateway-list"]),
    ("acct-tailscale", "Tailscale and Funnel (your tailnet)",
     "Log the radio in to your tailnet (the login link opens on your side). Funnel puts the Panel and Screen "
     "pages on the internet: say yes for that part, for a few minutes.",
     "Reach over the tailnet, what userspace mode exposes, priority, log out/off, Taildrop refused, Funnel's pages "
     "only, ownership, persistence.",
     "10 min",
     ["tailscale-reach-services", "tailscale-localhost-exposure", "tailscale-no-logs-priority", "tailscale-logout-off",
      "tailscale-taildrop-refused", "funnel-enable", "funnel-pages-only", "funnel-owns-funnel", "funnel-persist",
      "system-update-funnel-off-downgrade"]),
    ("bt-keyboard", "The Rii keyboard (already paired to the X6200)",
     "Switch the Rii X1 Plus on and type what the bench asks (it says each line: a word, CapsLock chords, [ ] in "
     "CW, the touchpad). With a uinput module built for the radios' kernel (none there now) the bench could type "
     "most of these itself and you would only confirm the real keyboard once.",
     "Watches state, screenshots and the other radio's decoder; CW from the keyboard over the cable at 0.1 W.",
     "30 min",
     ["cwkbd-usb-keyboard", "jtty-capslock-macros", "input-kbd-capslock-cw-messages", "log-cw-line-capslock-l",
      "cw-kbd-line-main", "cw-kbd-mfk-speed", "cw-kbd-capslock", "cw-kbd-bracket-paddle",
      "input-kbd-shortcuts-setting", "input-kbd-layout", "input-kbd-main-screen-keys", "input-kbd-capslock-chords",
      "input-mouse", "sentry-touch-mouse-tap", "scancfg-a-tap-mark", "settings-keys-keyboard-mouse",
      "input-kbd-capslock-ptt", "tx-cw-line-band-plan", "bt-keyboard-pairing", "settings-bluetooth-scan-pair",
      "settings-bluetooth-device-menu", "main-screen-status-bt-icons"]),
    ("bt-audio", "Bluetooth headphones, a phone or a PC",
     "Pair headphones (if you have a pair) in Settings > Bluetooth; listen where the bench asks. Pair a phone or "
     "PC once to see it offered a speaker and no microphone; a headset button as the browser's PTT.",
     "RX loopback, volume, speaker mute, A2DP only, BOTH's mix, what a PC sends dropped.",
     "20 min",
     ["bt-headphones-pair", "bt-rx-to-headphones", "bt-mute-speaker", "bt-headset-a2dp-only", "btaudio-headphones",
      "settings-bluetooth-headphones", "sentry-both-mix-headphones", "bt-cw-sidetone-headphones",
      "bt-pc-speaker-dropped", "bt-pc-no-input", "ptt-option-bt-headset"]),
    ("gps", "The Bluetooth GPS (GLO 2)",
     "Switch the GLO 2 on by a window; once, switch it off for a minute and on again.",
     "gpsd's bridge, reconnects and back-off, the GPS panel, the icon, title tag, DF heading, the web block.",
     "15 min",
     ["btgps-gpsd-bridge", "btgps-reconnect-backoff", "btgps-ctl", "main-screen-status-gps-icon", "gps-panel",
      "df-gpsd-heading", "gps-title-tag", "settings-bluetooth-gps", "gps-reconnect", "web-df-gps"]),
    ("hand-mic", "The hand mics (H-22 on the X6200, the X6100's)",
     "Press the mic keys the bench names; record a CQ, a RADE over and a voice message; speak for VOX; key the "
     "hand mic once outside the bands.",
     "Reads state after each key; plays the recordings over the cable at 0.1 W to the other radio.",
     "25 min",
     ["input-handmic-band-digits", "input-handmic-frequency-box", "input-handmic-f-keys",
      "input-handmic-up-down-channels", "input-handmic-mode-vm", "input-handmic-h22-vol-ab",
      "input-handmic-x6100-keys", "channels-hand-mic-keys", "field-rec-cq", "rade-rec", "rade-send",
      "voice-msg-hand-mic-only", "voice-msg-record-hand-mic", "settings-audio-vox", "tx-base-hold-outside"]),
    ("at-radio-apps", "WSPR and SWR Scan (opened only at the radio)",
     "Open WSPR, then SWR Scan, at the radio (neither opens remotely, by design); press TX next / Run when asked.",
     "Everything after the open over the cable at 0.2-1 W: decodes, TX %, power, hop, the ATU rule, the "
     "transverter limit, sweeps, spans, band limits, refusals, the look.",
     "40 min",
     ["wspr-open", "wspr-rx-decode", "wspr-tx-percent-next", "wspr-stop-tx", "wspr-power", "wspr-knob-offset",
      "wspr-hop", "wspr-hop-bands-page3", "wspr-hop-atu-rule", "transverter-wspr-at-limit", "swrscan-run-stop",
      "swrscan-x6200-sweep-mode", "swrscan-span-scale", "swrscan-band-limited", "swrscan-refusals"]),
    ("at-radio-look", "At the radio: knobs, lights, pairing, boot, by ear",
     "Turn the X6200's MFK inner ring; a knob while a browser has control; look at the key lights; Pair code and "
     "Forget all (the bench pairs again after); watch one reboot from the logo; listen to the boot sound, a scan "
     "and Internet radio replacing the receiver.",
     "Reads state and the remote's control line meanwhile.",
     "20 min",
     ["input-x6200-mfk-inner-ring", "control-local-takes-back", "settings-display-buttons-brightness",
      "pair-code-at-radio", "sessions-forget-all", "boot-picture-at-start", "boot-dark-before-video",
      "settings-audio-boot-sound", "iradio-replaces-receiver", "scanner-quiet-while-looking",
      "digi-tx-monitor-muted"]),
    ("keys-paddles", "Paddles and CW keyers (the ones you have)",
     "Plug or pair each you own (TinyMIDI, a Vail-type USB adapter, a mouse-type or MIDI or serial paddle, "
     "SmartCW) and key a few letters; Learn and Swap once.",
     "Reads its own 'cw' text with Train on (no TX); SmartCW's network and page through the radio.",
     "10 min per device",
     ["cwtrainer-key-modes", "tinymidi-paddle", "usb-paddle-keyboard-type", "usb-paddle-mouse-type",
      "usb-paddle-midi", "usb-paddle-serial", "usb-paddle-iambic", "usb-paddle-learn-swap", "settings-keys-paddle",
      "smartcw-usb-network", "smartcw-web-proxy", "smartcw-midi-keying", "web-smartcw-link", "tci-vfo-knob"]),
    ("power", "Power and battery (a bench supply, a battery run)",
     "Run a radio on battery down to its warnings; a bench supply under 12 V on the X6200; one plain poweroff and "
     "a power cycle; stop the base's data (cable or base off) for its warnings.",
     "Logs voltages and the warnings; checks the clock chip and the shutdown's 's'.",
     "an afternoon on battery, 20 min otherwise",
     ["system-low-battery-warnings", "system-low-battery-poweroff", "system-x6200-dcin-under-12v",
      "system-x6200-base-warnings", "system-battery-percent-from-voltage",
      "settings-txcal-x6200-att-supply-battery", "no-base-data-warnings", "panel-ptt-refused-no-base-data",
      "system-x6200-shutdown-poweroff", "clock-chip-start"]),
    ("cards-firmware", "SD cards and firmware (a spare card; your call on each)",
     "A spare SD card for new-card starts (keys restored, DATA formatted, files found on system); a power cut "
     "mid-update; the kernel and U-Boot boxes; Xiegu base firmware fetched and flashed on each radio; the X6100 "
     "test base.",
     "Writes the card image, reads start_trace and the journal after each start.",
     "an hour",
     ["ssh-keys-restore-new-card", "data-partition-format", "found-on-system", "start-trace-boot",
      "system-update-boot-journal", "settings-version-rollback", "system-update-allow-unsigned-old-image",
      "system-update-kernel-uboot-tickboxes", "f-system-base-firmware", "system-x6200-base-fw-get",
      "system-x6200-base-fw-flash", "system-x6100-base-flash", "tx-x6100-test-base"]),
    ("network", "WiFi, Ethernet, tethering, a security key",
     "Switch the access point off and on; a second access point (and an 8723bu-shy one if you have it); the "
     "radio's WiFi switch (drops the bench's link: switch it back at the radio); a USB Ethernet adapter on a "
     "switch with no gateway; an Android phone tethering; a FIDO2 key for SSH.",
     "Watches NetworkManager's retries and joins over SSH once back.",
     "30 min",
     ["network-wifi-auto-retry", "settings-wireless-wifi-join", "network-x6100-8723bu-workaround",
      "settings-wireless-wifi-power", "ethernet-no-gateway", "usb-tethering", "ssh-fido-login"]),
    ("rf-loads", "A mismatched load, and an HF antenna",
     "Put a mismatched load (SWR 3) on one radio for SWR protection, the tuner's give-up and TUNE above 1 W; an "
     "HF antenna for NAVTEX, DSC, WWV and busy FT8/JS8 bands (receive, and FT8/JS8 replies only on your word).",
     "Runs the TX at the power you allow; decodes and compares.",
     "30 min with the load; antenna tests run unattended once it is up",
     ["settings-txcal-swr-protection", "atu-tune-giveup", "ext-tune-atu-off-above-1w",
      "ext-tune-swr-protection-window", "navtex-decode", "dsc-rx", "timesig-decode-and-set-clock",
      "ft8-auto-pota-first", "ft8-auto-sync", "js8-relay", "js8-aprs"]),
    ("sdr-hands", "SDR hardware by hand",
     "Unplug and replug a stick (B's, a watch channel's); an unpowered hub; a bladeRF with an older firmware file; "
     "the WR12 if you have one; your DF array on a known-bearing source; say yes to the G WALKIE SDR transmitting "
     "on an amateur channel into a dummy load.",
     "Watches the pool, the daemon and the picture.",
     "30 min",
     ["sentry-stick-hotplug", "pool-serial-hold-reassign", "sdr-hub-brownout-kernel", "sdr-bladerf-fw-update",
      "g-sdr-tx-fm", "main-screen-wr12-tag", "radio-core-wr12-setup", "radio-core-wr12-receive",
      "system-wr12-bypass-on-exit", "tx-wr12-inhibit", "df-antenna-calibration"]),
]


def latest_coverage():
    runs = sorted(glob.glob(os.path.expanduser("~/.config/x6x00-bench/runs/*-coverage/coverage.md")))
    if not runs:
        return None, set()
    nc = set()
    for line in open(runs[-1]):
        if line.startswith("- `"):
            nc.add(line[3:line.index("`", 3)])
    return runs[-1], nc


def main():
    feats = {f["id"]: f for f in json.load(open(os.path.join(HERE, "features.json")))}
    placed = {}
    bad = []
    for key, _, _, _, _, ids in SESSIONS:
        for fid in ids:
            if fid not in feats:
                bad.append(f"{key}: no feature {fid}")
            elif fid in placed:
                bad.append(f"{key}: {fid} already in {placed[fid]}")
            placed[fid] = key
    src, nc = latest_coverage()
    unplaced = sorted(fid for fid in nc if feats.get(fid, {}).get("needs") in OWNER_NEEDS and fid not in placed)
    if "--check" in sys.argv:
        for b in bad:
            print(b)
        for fid in unplaced:
            print(f"not placed: {fid} ({feats[fid]['needs']})")
        sys.exit(1 if bad or unplaced else 0)
    owner_nc = [fid for fid in placed if fid in nc]
    lines = ["# Tests the owner facilitates", "",
             f"From {os.path.basename(os.path.dirname(src)) if src else 'no coverage run'} (`orch.py coverage`, "
             f"then `python3 facilitate.py`). {len(owner_nc)} features still wait on you: internet services, Bluetooth, "
             f"GPS or your hands. Each session says what you do and what the bench does; a ticked line is covered.", ""]
    for key, title, you, me, how_long, ids in SESSIONS:
        left = [fid for fid in ids if fid in nc]
        lines += [f"## {title}", "", f"- **You:** {you}", f"- **The bench:** {me}", f"- **Your time:** {how_long}",
                  f"- **Left:** {len(left)} of {len(ids)}", ""]
        for fid in ids:
            f = feats[fid]
            lines.append(f"  - [{' ' if fid in nc else 'x'}] `{fid}` ({f['area']}): {f['feature'][:140]}")
        lines.append("")
    if unplaced:
        lines += ["## Not placed in a session yet", ""] + [f"- `{fid}` ({feats[fid]['needs']})" for fid in unplaced] + [""]
    alone = {}
    for fid in nc:
        f = feats.get(fid)
        if f and f["needs"] not in OWNER_NEEDS and fid not in placed:
            alone.setdefault(f["needs"], {}).setdefault(f["area"], 0)
            alone[f["needs"]][f["area"]] += 1
    lines += ["## What the bench runs alone, still to write and run", "",
              "No hands: over the cable between the radios (your cable word, at most 1 W), the SDRs on HOST, the "
              "HackRF into the X6200 (receive only), or nothing at all.", ""]
    for needs in ("cable", "rtl", "hackrf", "none"):
        if needs in alone:
            areas = alone[needs]
            lines.append(f"- **{needs}** ({sum(areas.values())}): " +
                         ", ".join(f"{a} {n}" for a, n in sorted(areas.items(), key=lambda x: -x[1])))
    with open(os.path.join(HERE, "FACILITATE.md"), "w") as fo:
        fo.write("\n".join(lines) + "\n")
    print(os.path.join(HERE, "FACILITATE.md"), f"{len(owner_nc)} owner features left;",
          f"{len(bad)} problems, {len(unplaced)} not placed")


if __name__ == "__main__":
    main()
