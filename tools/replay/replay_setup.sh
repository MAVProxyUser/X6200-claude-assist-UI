#!/bin/bash
# on: a null sink "ox_replay" on the radio's PulseAudio and the GUI's capture on its monitor (/tmp/ox_capture_source,
# read at the GUI's start; tmpfs, gone at the next boot); the GUI restarted. off: all of it undone.
RADIO=root@192.168.0.224
case "$1" in
  on)  ssh $RADIO 'pactl list short sinks | grep -q ox_replay || pactl load-module module-null-sink sink_name=ox_replay rate=44100 channels=1 > /tmp/ox_replay.module
                   echo ox_replay.monitor > /tmp/ox_capture_source; kill $(pidof x6100_gui); sleep 7; echo "GUI pid $(pidof x6100_gui)"; pactl list short sources | grep ox_replay' ;;
  off) ssh $RADIO 'rm -f /tmp/ox_capture_source /tmp/replay.raw /tmp/replay_radio.py; m=$(cat /tmp/ox_replay.module 2>/dev/null); [ -n "$m" ] && pactl unload-module $m; rm -f /tmp/ox_replay.module
                   kill $(pidof x6100_gui); sleep 7; echo "GUI pid $(pidof x6100_gui)"; pactl list short sinks' ;;
esac
