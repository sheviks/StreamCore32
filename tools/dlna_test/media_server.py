#!/usr/bin/env python3
"""Test media server: Range support and the awkward cases.
 /f/<name>        file, Range honoured (206)
 /norange/<name>  file, always 200
 /redir/<name>    302 -> /f/<name>
 /chunked/<name>  200, Transfer-Encoding: chunked, no length
 /live/<name>     200, no length, ends by closing the connection
 /drop/<name>     like /f but closes the connection after 50 KB
"""
import os, sys, socketserver
from http.server import BaseHTTPRequestHandler
ROOT = sys.argv[1]
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        parts = self.path.split("/", 2)
        if len(parts) < 3: return self.send_error(404)
        mode, name = parts[1], parts[2]
        p = os.path.join(ROOT, name)
        if not os.path.isfile(p): return self.send_error(404)
        data = open(p, "rb").read()
        if mode == "redir":
            self.send_response(302); self.send_header("Location", "/f/" + name)
            self.send_header("Content-Length", "0"); self.end_headers(); return
        start = 0
        rng = self.headers.get("Range")
        if rng and mode in ("f", "drop"):
            start = int(rng.split("=")[1].split("-")[0])
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}")
            self.send_header("Content-Length", str(len(data) - start))
        else:
            self.send_response(200)
            if mode == "chunked": self.send_header("Transfer-Encoding", "chunked")
            elif mode != "live": self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", "audio/x-test")
        self.send_header("Connection", "close")
        self.end_headers()
        body = data[start:]
        try:
            if mode == "chunked":
                for i in range(0, len(body), 3000):
                    c = body[i:i+3000]; self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            elif mode == "drop":
                self.wfile.write(body[:50000])
            else:
                self.wfile.write(body)
        except BrokenPipeError:
            pass
        self.close_connection = True
class S(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True; daemon_threads = True
S(("127.0.0.1", int(sys.argv[2])), H).serve_forever()
