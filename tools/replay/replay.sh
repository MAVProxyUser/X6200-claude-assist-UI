#!/bin/bash
# Plays a recording into the radio's GUI as if its receiver heard it (the GUI's capture on the ox_replay sink's
# monitor: replay_setup.sh). START: "now", or UTC "HH:MM:SS[.s]" today, or "slot:PERIOD[:LEAD]" = the next
# PERIOD-second boundary with the audio's first sample LEAD s before it (default 0).
#     replay.sh FILE START [SECONDS_FROM SECONDS_LEN]
R=$(cd "$(dirname "$0")" && pwd); RADIO=root@192.168.0.224
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT   # the converted audio, never in the repository
f=$1; start=$2; ss=${3:-0}; len=${4:-0}
args=(-nostdin -loglevel error -y -ss "$ss" -i "$f"); [ "$len" != 0 ] && args+=(-t "$len")
ffmpeg "${args[@]}" -ac 1 -ar 44100 -f s16le "$T/replay.raw" || exit 1
scp -q "$T/replay.raw" "$R/replay_radio.py" $RADIO:/tmp/
case "$start" in
  now) at=0 ;;
  slot:*) IFS=: read -r _ per lead <<< "$start"; lead=${lead:-0}
          at=$(ssh $RADIO "python3 -c \"import time,math;p=$per;t=time.time();s=math.ceil((t+2)/p)*p;print(s-$lead)\"") ;;
  *) at=$(python3 -c "import datetime,sys;d=datetime.datetime.now(datetime.timezone.utc);h,m,s=sys.argv[1].split(':');print(d.replace(hour=int(h),minute=int(m),second=0,microsecond=0).timestamp()+float(s))" "$start") ;;
esac
ssh $RADIO "python3 /tmp/replay_radio.py /tmp/replay.raw 44100 $at ox_replay"
