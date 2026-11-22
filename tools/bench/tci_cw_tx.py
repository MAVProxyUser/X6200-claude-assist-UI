#!/usr/bin/env python3
"""CW text keyed by a radio's own keyer through its own TCI server, for tools/bench (suite_more.py's CW path). Run ON
the radio (a client from 127.x is not asked on the screen): "cw_macros:0,<text>;" once the server is ready, then
waits for cw_macros_empty or its own deadline, and leaves (the server unkeys a client that leaves). The radio must be
in CW and armed. Standard library only.

    tci_cw_tx.py TEXT MAX_S [PORT]
Prints "start EPOCH" when the text is sent and "end EPOCH" as it leaves.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import base64
import os
import socket
import struct
import sys
import time

text, max_s = sys.argv[1], float(sys.argv[2])
port = int(sys.argv[3]) if len(sys.argv) > 3 else 50001
s = socket.create_connection(("127.0.0.1", port), timeout=3)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}"
           f"\r\nSec-WebSocket-Version: 13\r\nUser-Agent: GoldCorpHF bench cw\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(1024)
buf = buf.split(b"\r\n\r\n", 1)[1]


def send(t):
    d, m = t.encode(), os.urandom(4)
    head = bytes([0x81, 0x80 | len(d)]) if len(d) < 126 else bytes([0x81, 0xFE]) + struct.pack(">H", len(d))
    s.sendall(head + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))


def frames():
    global buf
    s.settimeout(0.3)
    try:
        buf += s.recv(65536)
    except socket.timeout:
        pass
    out = []
    while len(buf) >= 2:
        op, n = buf[0] & 15, buf[1] & 127
        h = 2 + (2 if n == 126 else 8 if n == 127 else 0)
        if len(buf) < h:
            break
        n = struct.unpack(">H", buf[2:4])[0] if n == 126 else struct.unpack(">Q", buf[2:10])[0] if n == 127 else n
        if len(buf) < h + n:
            break
        if op == 1:
            out.append(buf[h:h + n].decode(errors="replace"))
        buf = buf[h + n:]
    return out


t0 = time.time()
while time.time() - t0 < 5 and not any(f.startswith("ready") for f in frames()):
    pass
send(f"cw_macros:0,{text};")
print(f"start {time.time():.3f}", flush=True)
while time.time() - t0 < max_s:
    got = frames()
    if any(f.startswith("cw_macros_empty") for f in got):
        break
    if any(f.startswith("trx:0,false") for f in got) and time.time() - t0 > 2:
        print("refused or ended: trx:0,false", flush=True)
        break
s.close()
print(f"end {time.time():.3f}", flush=True)
