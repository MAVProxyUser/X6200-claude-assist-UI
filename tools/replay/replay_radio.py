"""On the radio: plays RAW (s16le mono at RATE) into the PulseAudio sink SINK, its first sample at the radio's UTC
START (epoch seconds; 0 = at once). Standard library only.   replay_radio.py RAW RATE START SINK"""
import subprocess, sys, time
raw, rate, start, sink = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
if start > 0:
    time.sleep(max(0.0, start - time.time() - 0.15))   # pacat's start-up, measured ~0.1 s
p = subprocess.Popen(["pacat", "--playback", "-d", sink, "--rate=%d" % rate, "--channels=1", "--format=s16le",
                      "--raw", "--latency-msec=40"], stdin=subprocess.PIPE, env={"PULSE_RUNTIME_PATH": "/var/run/pulse"})
t0 = time.time()
with open(raw, "rb") as f:
    while True:
        d = f.read(rate // 10 * 2)
        if not d:
            break
        p.stdin.write(d)
p.stdin.close()
p.wait()
print("played %s from %.3f (radio UTC) for %.1f s" % (raw, t0, time.time() - t0), flush=True)
