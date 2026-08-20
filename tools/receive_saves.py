#!/usr/bin/env python3
"""Receive backups sent by IESM's 'Send to PC': run on the PC, note its LAN IP.
Files land in ./received/."""

import os
from http.server import BaseHTTPRequestHandler, HTTPServer


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(n)
        name = os.path.basename(self.path) or "save.bin"
        os.makedirs("received", exist_ok=True)
        with open(os.path.join("received", name), "wb") as f:
            f.write(data)
        print(f"received {name} ({len(data)} bytes)")
        self.send_response(200)
        self.end_headers()

    def log_message(self, *a):
        pass


print("listening on port 8123 — set this PC's IP in IESM")
HTTPServer(("", 8123), H).serve_forever()
