"""The live view of a modem link between the two radios (bench.py radios-modem): a page on this Mac, fed by both radios'
x6x00-modem ends (their JSON lines, ten a second each) over server-sent events. Display only: what is sent, and when,
is decided by bench.py and the ends.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import http.server
import json
import os
import queue
import socketserver
import threading
import time

PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modem_view.html")


class View:
    def __init__(self, port, title):
        self.title = title
        self.clients, self.lock = [], threading.Lock()
        self.last = {}          # each radio's latest line, for a page opened late
        self.logs = {}          # and every event line so far
        view = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path == "/":
                    body = open(PAGE, "rb").read().replace(b"@TITLE@", view.title.encode())
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                elif self.path == "/events":
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-cache")
                    self.end_headers()
                    q = queue.Queue(maxsize=400)
                    with view.lock:
                        for name, lines in view.logs.items():
                            q.put(json.dumps({"name": name, "replay": lines}))
                        for doc in view.last.values():
                            q.put(json.dumps(doc))
                        view.clients.append(q)
                    try:
                        while True:
                            try:
                                msg = q.get(timeout=5)
                            except queue.Empty:
                                msg = None
                            self.wfile.write(b": keepalive\n\n" if msg is None else f"data: {msg}\n\n".encode())
                            self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        pass
                    finally:
                        with view.lock:
                            view.clients.remove(q)
                else:
                    self.send_error(404)

        class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        self.httpd = Server(("127.0.0.1", port), Handler)
        self.url = f"http://127.0.0.1:{port}/"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def publish(self, doc):
        """A radio's line, or a note from the harness ({"name": "bench", "note": ...})"""
        doc = dict(doc, at=time.time())
        with self.lock:
            name = doc.get("name", "?")
            if doc.get("log"):
                self.logs.setdefault(name, []).extend(doc["log"])
            if "note" not in doc:
                self.last[name] = doc
            msg = json.dumps(doc)
            for q in self.clients:
                try:
                    q.put_nowait(msg)
                except queue.Full:
                    pass

    def close(self):
        self.httpd.shutdown()
