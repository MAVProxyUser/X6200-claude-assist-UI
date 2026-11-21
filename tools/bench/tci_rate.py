#!/usr/bin/env python3
"""TCI receive audio over the real network: does a radio's stream keep up with real time?

The TCI tests under tools/tci run on the radio itself over 127.0.0.1, so they never see the radio's WiFi, nor a
client the server drops (it PINGs one silent 20 s and drops it at 60 s). This one runs on the Mac against the radio's
address, as a TCI program would: it asks for a format, answers the server's PINGs, takes every frame as it comes and
reports, every 5 s, the audio received as a share of real time and the longest gap between two frames. It only
listens: nothing is keyed.

    tci_rate.py HOST [--seconds 120] [--format wsjtx|bench] [--min 95]

    --format wsjtx   48 kHz float32 stereo, 2048 a frame: TCI's default, what WSJT-X takes (3 Mbit/s)
    --format bench   24 kHz int16 mono, 512 a frame: tones.py's recorder (0.4 Mbit/s)

PASS when every 5 s window after the first holds --min percent of real time and the server never closes the
connection; exit status 0, else 1. 120 s by default: past the server's 60 s drop of a client that does not answer.
"""
import argparse, base64, os, socket, struct, sys, time

FORMATS = {"wsjtx": (48000, "float32", 2, 2048), "bench": (24000, "int16", 1, 512)}
BYTES = {"int16": 2, "int24": 3, "int32": 4, "float32": 4}


class Ws:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = bytearray()
        while b"\r\n\r\n" not in buf:
            d = self.s.recv(4096)
            if not d:
                raise OSError("closed during the handshake")
            buf += d
        head, _, rest = bytes(buf).partition(b"\r\n\r\n")
        if b" 101" not in head.split(b"\r\n")[0]:
            raise OSError(f"no WebSocket answer: {head[:60]!r}")
        self.buf, self.pos = bytearray(rest), 0
        self.s.settimeout(1.0)

    def send(self, op, p):
        mask = os.urandom(4)
        n = len(p)
        head = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))

    def frames(self):
        """(opcode, payload memoryview) for each whole frame received so far; None at the end of the stream"""
        try:
            d = self.s.recv(262144)
        except socket.timeout:
            return []
        if not d:
            return None
        self.buf += d
        out = []
        b = self.buf
        while True:
            if len(b) - self.pos < 2:
                break
            b0, b1 = b[self.pos], b[self.pos + 1]
            n, i = b1 & 0x7F, self.pos + 2
            if n == 126:
                if len(b) - i < 2:
                    break
                n, i = struct.unpack_from(">H", b, i)[0], i + 2
            elif n == 127:
                if len(b) - i < 8:
                    break
                n, i = struct.unpack_from(">Q", b, i)[0], i + 8
            if len(b) - i < n:
                break
            op = b0 & 0x0F
            out.append((op, bytes(b[i:i + (64 if op == 2 else n)]), n))     # audio: only its header is needed
            self.pos = i + n
        if self.pos > 1 << 20:
            del b[:self.pos]
            self.pos = 0
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=50001)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--format", choices=FORMATS, default="wsjtx")
    ap.add_argument("--min", type=float, default=95, help="the least share of real time a window may hold (%%)")
    a = ap.parse_args()
    rate, typ, ch, ns = FORMATS[a.format]
    bps = BYTES[typ] * ch
    ws = Ws(a.host, a.port)
    ws.send(1, (f"audio_samplerate:{rate};audio_stream_sample_type:{typ};audio_stream_channels:{ch};"
                f"audio_stream_samples:{ns};audio_start:0;").encode())
    print(f"{a.host}: {a.format} ({rate} Hz {typ} x{ch}, {rate * bps * 8 / 1e6:.2f} Mbit/s), {a.seconds:.0f} s, "
          f"PASS at {a.min:.0f} % of real time", flush=True)
    t0 = tw = time.time()
    got = 0.0
    last = None
    gap = 0.0
    pings = 0
    shares = []
    closed = None
    while time.time() - t0 < a.seconds:
        fr = ws.frames()
        if fr is None:
            closed = time.time() - t0
            break
        now = time.time()
        for op, p, n in fr:
            if op == 9:                     # the server's PING: a client that does not answer is dropped at 60 s
                ws.send(0xA, p)
                pings += 1
            elif op == 2 and n > 64 and struct.unpack_from("<8I", p, 0)[6] == 1:     # RX audio
                got += (n - 64) / bps
                if last is not None:
                    gap = max(gap, now - last)
                last = now
            elif op == 8:
                closed = now - t0
        if closed is not None:
            break
        if now - tw >= 5:
            share = got / rate / (now - tw) * 100
            shares.append(share)
            print(f"{now - t0:6.1f} s  {share:5.1f} % of real time  longest gap {gap * 1000:5.0f} ms", flush=True)
            tw, got, gap = now, 0.0, 0.0
    try:
        ws.send(1, b"audio_stop:0;")
        ws.s.close()
    except OSError:
        pass
    late = shares[1:]
    bad = [s for s in late if s < a.min]
    if closed is not None:
        print(f"FAIL: the server closed the connection at {closed:.0f} s ({pings} PINGs answered)")
        return 1
    if not late:
        print("FAIL: no audio measured")
        return 1
    print(f"{'FAIL' if bad else 'PASS'}: {len(late) - len(bad)} of {len(late)} windows at {a.min:.0f} % or more "
          f"(lowest {min(late):.1f} %, mean {sum(late) / len(late):.1f} %), {pings} PINGs answered")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
