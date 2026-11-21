#!/bin/sh
# ptrmon.sh SECONDS: the capture's hw_ptr against its timestamp every 5 s; a step of the offset over 44100 frames
# (a buffer is 88200) is ALSA counting a wrap that did not happen. Runs in the ssh session that starts it.
end=$(( $(cut -d. -f1 /proc/uptime) + $1 ))
prev=""
while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
    awk -v prev="$prev" '/trigger_time/{tr=$2} /^tstamp/{ts=$3} /^hw_ptr/{hw=$3}
        END{off=hw-(ts-tr)*44100; printf "%s %.3f %.3f %d %.0f\n", "S", tr, ts, hw, off}' /proc/asound/card0/pcm0c/sub0/status
    sleep 5
done
