"""A client of the radio's browser remote (src/remote_server.c: a WebSocket server on port 50002), for the bench
harness: text commands out, text answers in (a queue), binary frames (the screen, the spectrum) dropped. No Origin
header: a program, not a page, as TCI programs are (tci_ws.c). The standard library only.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import collections
import os
import queue
import socket
import struct
import threading
import time


class Remote:
    def __init__(self, host, port=50002, timeout=10):
        self.host = host
        self.s = socket.create_connection((host, port), timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        got = b""
        while b"\r\n\r\n" not in got:
            d = self.s.recv(4096)
            if not d:
                raise ConnectionError(f"{host}: the remote closed during the handshake")
            got += d
        head, self.buf = got.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(f"{host}: no WebSocket: {head[:120]!r}")
        self.s.settimeout(None)
        self.texts = queue.Queue()
        # The panel's spectrum frames (binary: u16 0xFFFF, u16 bins, u32 centre Hz, u32 width Hz, u32 sequence, an int8
        # dB a bin), the last 300 with when they came: (time, centre, width, dBs). What the receiver itself takes in
        # (the radios' walkie test: the X6200's S-meter did not follow an AM signal its spectrum showed 41 dB up)
        self.spectra = collections.deque(maxlen=300)
        self.alive = True
        self.wlock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        with self.wlock:
            self.s.sendall(head + mask + body)

    def _exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _reader(self):
        try:
            while True:
                b0, b1 = self._exact(2)
                op, n = b0 & 0x0F, b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._exact(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._exact(8))[0]
                mask = self._exact(4) if b1 & 0x80 else None
                data = self._exact(n)
                if mask:
                    data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
                if op == 1:
                    self.texts.put(data.decode(errors="replace"))
                elif op == 2 and len(data) > 16 and data[:2] == b"\xff\xff":
                    nb, cen, wid, _ = struct.unpack_from("<HIII", data, 2)
                    if len(data) >= 16 + nb:
                        self.spectra.append((time.time(), cen, wid, struct.unpack_from(f"<{nb}b", data, 16)))
                elif op == 8:
                    break
                elif op == 9:           # ping: pong with its data
                    with self.wlock:
                        m = os.urandom(4)
                        self.s.sendall(bytes([0x8A, 0x80 | len(data)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(data)))
        except (OSError, ConnectionError, ValueError):
            pass
        self.alive = False
        self.texts.put(None)

    def recv(self, timeout=None):
        """The next text, None when the connection went or nothing came in timeout seconds"""
        try:
            return self.texts.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        try:
            with self.wlock:
                self.s.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        except OSError:
            pass
        try:
            self.s.close()
        except OSError:
            pass
