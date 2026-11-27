#!/usr/bin/env python3
"""One TCI start from this machine to HOST:PORT: connects, waits for 'ready;', sends 'trx:0,true;', prints the
radio's trx answer (or 'none'), sends 'trx:0,false;' and closes. Run on one radio against the other, it is a client
from another address than the bench's (src/tci_server.c ask_open: a start from another address while the radio asks
about one is refused without a question). Standard library only (the radios' Python).

    tci_start_once.py HOST [PORT]

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import os
import socket
import struct
import sys
import time


def main():
    host, port = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 50001
    s = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        got = s.recv(4096)
        if not got:
            print("closed")
            return
        buf += got
    buf = buf.split(b"\r\n\r\n", 1)[1]

    def send(text):
        d, m = text.encode(), os.urandom(4)
        n = len(d)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        s.sendall(head + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))

    def frames(until):
        nonlocal buf
        while time.time() < until:
            while len(buf) >= 2:
                op, n = buf[0] & 0x0F, buf[1] & 0x7F
                k = 2
                if n == 126:
                    if len(buf) < 4:
                        break
                    n, k = struct.unpack(">H", buf[2:4])[0], 4
                elif n == 127:
                    if len(buf) < 10:
                        break
                    n, k = struct.unpack(">Q", buf[2:10])[0], 10
                if len(buf) < k + n:
                    break
                data, buf = buf[k:k + n], buf[k + n:]
                if op == 1:
                    yield data.decode(errors="replace")
            try:
                s.settimeout(max(0.05, until - time.time()))
                got = s.recv(65536)
            except socket.timeout:
                continue
            if not got:
                return
            buf += got

    for t in frames(time.time() + 4):
        if t.lower().startswith("ready"):
            break
    send("trx:0,true;")
    ans = "none"
    for t in frames(time.time() + 3):
        if t.lower().startswith("trx:0,"):
            ans = t.strip()
            break
    print(ans)
    send("trx:0,false;")
    time.sleep(0.3)
    s.close()


if __name__ == "__main__":
    main()
