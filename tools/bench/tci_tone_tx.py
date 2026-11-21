#!/usr/bin/env python3
"""Tones into a radio's transmitter through its own TCI server, for tools/bench/tones.py. Run ON the radio: a client
from 127.0.0.1 is not asked about on the screen (src/tci_server.c), one from the network would be. Keyed with
"trx:0,true,tci;", each TX_CHRONO the server sends (every 21.3 ms) is answered with 512 int16 samples at 24 kHz of the
plan's tone (made whole before keying), then "trx:0,false;". Its own deadline besides: past MAX_S it unkeys and leaves, and the
server itself unkeys 500 ms after the audio stops. Standard library only (the radios have no WebSocket package).

    tci_tone_tx.py PLAN_JSON AMPLITUDE MAX_S [PORT [FADE_S]]   PLAN: [[hz, seconds], ...]; 0 Hz is silence; FADE_S the
                                                               fade in and out of each stretch of tone (0.3 s; tones.py
                                                               --bursts: a hard start, 2 ms)
Prints "start EPOCH" at the first frame sent and "end EPOCH SAMPLES" at the unkey.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64, hashlib, json, math, os, signal, socket, struct, sys, time

PID_FILE = "/tmp/tci_tone_tx.pid"     # tones.py stops a run with kill (the X6100's BusyBox has no pkill)

FS, N = 24000, 512


class WS:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                        f"User-Agent: GoldCorpHF bench tones\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            d = self.s.recv(1024)
            if not d:
                raise RuntimeError("closed during the handshake")
            resp += d
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        if b" 101" not in head.split(b"\r\n")[0] or want not in head:
            raise RuntimeError("not a WebSocket answer")

    def send(self, op, payload):
        mask = bytes(4)         # a zero mask, as RFC 6455 allows: no per-byte work on the radio's slow Python
        n = len(payload)
        head = bytes([0x80 | op])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.s.sendall(head + mask + payload)

    def _take(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise RuntimeError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def frame(self):
        b0, b1 = self._take(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._take(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._take(8))[0]
        mask = self._take(4) if b1 & 0x80 else None
        p = self._take(n)
        if mask:
            p = bytes(b ^ mask[i % 4] for i, b in enumerate(p))
        return b0 & 0x0F, p


def main():
    plan = json.loads(sys.argv[1])
    amp, max_s = float(sys.argv[2]), float(sys.argv[3])
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 50001
    fade_s = float(sys.argv[5]) if len(sys.argv) > 5 else 0.3
    # The whole plan made before keying (a frame is then a slice): the radio's Python is too slow for 512 sines
    # inside each 21.3 ms
    # The phase carried across tone changes (no click: a click is a burst the transmitter's ALC answers by cutting its
    # gain), and the audio faded in and out over fade_s (an onset from silence is what the ALC overshoots on)
    from array import array
    sin, a = math.sin, amp * 32767
    out, phase, first, pos = array("h"), 0.0, None, 0
    for hz, sec in plan:
        n, w = int(round(sec * FS)), 2 * math.pi * float(hz) / FS
        if hz > 0:
            out.extend(int(a * sin(phase + w * i)) for i in range(n))
            phase = math.fmod(phase + w * n, 2 * math.pi)
            first = pos if first is None else first
            last = pos + n
        else:
            out.extend([0] * n)
        pos += n
    # Every stretch of tone between silences faded in and out: the whole run when the tones follow each other, each
    # tone when they have gaps between them (the shorter fade of a short stretch)
    pos, runs, cur = 0, [], None
    for hz, sec in plan:
        n = int(round(sec * FS))
        if hz > 0 and n:
            cur = [pos, pos + n] if cur is None else [cur[0], pos + n]
        elif n and cur is not None:
            runs.append(cur)
            cur = None
        pos += n
    if cur is not None:
        runs.append(cur)
    for r0, r1 in runs:
        nf = max(1, min(int(fade_s * FS), (r1 - r0) // 8))
        for i in range(nf):
            g = i / nf
            out[r0 + i] = int(out[r0 + i] * g)
            out[r1 - 1 - i] = int(out[r1 - 1 - i] * g)
    if sys.byteorder != "little":
        out.byteswap()
    pcm = out.tobytes()
    total = len(pcm) // 2
    with open(PID_FILE, "w") as f:
        f.write(f"{os.getpid()}\n")
    # killed: the finally below unkeys (else the server would, 500 ms after the audio stops)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    ws = WS(port)
    ws.send(1, b"trx:0,true,tci;")
    t0, sent, start = time.time(), 0, None
    keyed = False
    try:
        while sent < total and time.time() - t0 < max_s:
            op, p = ws.frame()
            if op == 9:                 # the server's PING: answered, or it drops a client silent 60 s
                ws.send(10, p)
            elif op == 1:
                txt = p.decode(errors="replace")
                if "trx:0,true" in txt:
                    keyed = True
                elif "trx:0,false" in txt and keyed:
                    print("unkeyed by the radio", flush=True)
                    break
                elif "trx:0,false" in txt and time.time() - t0 > 3:
                    print("refused: trx:0,false", flush=True)
                    break
            elif op == 2 and len(p) >= 32 and struct.unpack("<8I", p[:32])[6] == 3:
                chunk = bytes(pcm[2 * sent:2 * (sent + N)])
                chunk += bytes(2 * N - len(chunk))
                hdr = struct.pack("<8I", 0, FS, 0, 0, 0, N, 2, 1) + bytes(32)
                ws.send(2, hdr + chunk)
                if start is None:
                    start = time.time()
                    print(f"start {start:.3f}", flush=True)
                sent += N
            elif op == 8:
                break
    finally:
        try:
            ws.send(1, b"trx:0,false;")
            time.sleep(0.3)
            ws.send(8, struct.pack(">H", 1000))
        except OSError:
            pass
        print(f"end {time.time():.3f} {sent}", flush=True)
        try:
            os.unlink(PID_FILE)
        except OSError:
            pass


if __name__ == "__main__":
    main()
