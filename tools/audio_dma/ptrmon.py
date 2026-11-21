#!/usr/bin/env python3
"""ptrmon.py HOST SECONDS: runs ptrmon.sh on HOST, flags offset steps of half a buffer or more"""
import subprocess, sys, time
host, sec = sys.argv[1], int(sys.argv[2])
sh = open(__file__.replace(".py", ".sh")).read()
p = subprocess.Popen(["ssh", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=15", f"root@{host}", f"sh -s {sec}"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
p.stdin.write(sh); p.stdin.close()
last = None; n = 0; steps = 0
for line in p.stdout:
    f = line.split()
    if len(f) != 5 or f[0] != "S":
        continue
    tr, ts, hw, off = float(f[1]), float(f[2]), int(f[3]), float(f[4])
    n += 1
    if last and last[0] == tr and abs(off - last[1]) >= 44100:
        steps += 1
        print(f"{time.strftime('%H:%M:%S')} {host}: STEP {off - last[1]:+.0f} frames ({(off - last[1]) / 88200:+.2f} buffers) at tstamp {ts:.1f}", flush=True)
    elif last and last[0] != tr:
        print(f"{time.strftime('%H:%M:%S')} {host}: capture restarted (trigger {tr:.1f})", flush=True)
    elif not last:
        print(f"{time.strftime('%H:%M:%S')} {host}: offset {off:+.0f} frames at start", flush=True)
    last = (tr, off)
print(f"{time.strftime('%H:%M:%S')} {host}: {n} samples, {steps} steps; offset at end {last[1] if last else 0:+.0f}", flush=True)
