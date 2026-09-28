#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# websrv.py - the host-side web servers curltest.sh and wgettest.sh
# fetch from. The machine reaches them at 10.0.2.2.
#
#   websrv.py ROOT HTTP_PORT HTTPS_PORT CERT KEY LOG H2_PORT
#
# Plain HTTP and HTTPS on the first two ports; HTTP/2 ONLY (ALPN "h2")
# on H2_PORT unless it is 0, which needs python's h2 (requirements.txt).
# Every request is logged to LOG with its Range, Accept-Encoding and
# Host headers, so a suite can check what the client actually ASKED for
# rather than only what it got.
#
#   GET  /FILE            a file from ROOT; honours Range
#   GET  /redir           302 to /small.txt
#   GET  /gz/FILE         gzip, if the client asked for gzip (else 406)
#   GET  /force-gz/FILE, /force-br/FILE   encoded even when NOT asked for
#   (every response carries a Content-Type guessed from the name)
#   GET  /br/big.bin      ROOT/big.bin.br as "br", if asked for
#   GET  /zstd/big.bin    ROOT/big.bin.zst as "zstd", if asked for
#   GET  /cookie          sets one cookie for example.co.uk and one for
#                         all of co.uk -- a PSL-aware client keeps one
#   GET  /echo-cookie     answers with the Cookie header it was sent
#   POST /anything        answers with the SHA-256 of the body

import gzip, hashlib, http.server, mimetypes, os, ssl, sys, threading
root, port, sport, cert, key, log, h2port = sys.argv[1:8]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        with open(log, "a") as f:
            f.write("%s %s range=%s ae=%s host=%s\n" % (self.command, self.path,
                    self.headers.get("Range"), self.headers.get("Accept-Encoding"),
                    self.headers.get("Host")))
    def send(self, code, body, extra=()):
        self.send_response(code)
        for k, v in extra: self.send_header(k, v)
        # A browser needs to be told it is HTML; curl and wget do not care.
        if not any(k.lower() == "content-type" for k, _ in extra):
            t = mimetypes.guess_type(self.path.split("?")[0])[0]
            self.send_header("Content-Type", t or "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        self.log_message()
        if self.path == "/redir":
            return self.send(302, b"", [("Location", "/small.txt")])
        if self.path == "/cookie":
            return self.send(200, b"set\n", [
                ("Set-Cookie", "wide=1; Domain=co.uk; Path=/"),
                ("Set-Cookie", "site=1; Domain=example.co.uk; Path=/")])
        if self.path == "/echo-cookie":
            return self.send(200, ("cookie=%s\n" % self.headers.get("Cookie")).encode())
        for enc, ext in (("br", ".br"), ("zstd", ".zst")):
            if self.path == "/%s/big.bin" % enc:
                if enc not in (self.headers.get("Accept-Encoding") or ""):
                    return self.send(406, b"ask for it\n")
                return self.send(200, open(os.path.join(root, "big.bin" + ext), "rb").read(),
                                 [("Content-Encoding", enc)])
        # /force-gz/ and /force-br/ encode WHETHER OR NOT the client
        # asked, as some real servers do (lynx.invisible-island.net
        # sends brotli to a client that offered nothing).
        for enc, pre, fn in (("gzip", "/force-gz/", gzip.compress),
                             ("br", "/force-br/", None)):
            if self.path.startswith(pre):
                p = os.path.join(root, os.path.basename(self.path))
                if fn is None:
                    import subprocess
                    body = subprocess.run(["brotli", "-c", p], capture_output=True).stdout
                else:
                    body = fn(open(p, "rb").read())
                return self.send(200, body, [("Content-Encoding", enc),
                    ("Content-Type", mimetypes.guess_type(p)[0] or "text/plain")])
        gz = self.path.startswith("/gz/")
        p = os.path.join(root, os.path.basename(self.path))
        if not os.path.isfile(p):
            return self.send(404, b"no\n")
        data = open(p, "rb").read()
        if gz:
            if "gzip" not in (self.headers.get("Accept-Encoding") or ""):
                return self.send(406, b"ask for gzip\n")
            return self.send(200, gzip.compress(data), [("Content-Encoding", "gzip")])
        r = self.headers.get("Range")
        if r and r.startswith("bytes="):
            a, _, b = r[6:].partition("-")
            a = int(a); b = int(b) if b else len(data) - 1
            return self.send(206, data[a:b + 1], [("Content-Range",
                    "bytes %d-%d/%d" % (a, b, len(data)))])
        self.send(200, data)
    def do_POST(self):
        self.log_message()
        n = int(self.headers.get("Content-Length", 0))
        self.send(200, (hashlib.sha256(self.rfile.read(n)).hexdigest() + "\n").encode())
http.server.ThreadingHTTPServer.allow_reuse_address = True
a = http.server.ThreadingHTTPServer(("0.0.0.0", int(port)), H)
b = http.server.ThreadingHTTPServer(("0.0.0.0", int(sport)), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
b.socket = ctx.wrap_socket(b.socket, server_side=True)
threading.Thread(target=b.serve_forever, daemon=True).start()

# HTTP/2 ONLY: ALPN offers "h2" and nothing else, and what comes in is
# parsed as HTTP/2 frames. A client that cannot speak it gets nothing.
def h2_serve():
    import socket, h2.config, h2.connection, h2.events
    c2 = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c2.load_cert_chain(cert, key)
    c2.set_alpn_protocols(["h2"])
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", int(h2port))); ls.listen(5)
    def one(raw):
        try:
            s = c2.wrap_socket(raw, server_side=True)
            conn = h2.connection.H2Connection(h2.config.H2Configuration(client_side=False))
            conn.initiate_connection(); s.sendall(conn.data_to_send())
            while True:
                data = s.recv(65535)
                if not data: break
                for ev in conn.receive_data(data):
                    if isinstance(ev, h2.events.RequestReceived):
                        path = dict(ev.headers)[b":path"].decode()
                        with open(log, "a") as f: f.write("H2 %s\n" % path)
                        body = open(os.path.join(root, os.path.basename(path)), "rb").read()
                        conn.send_headers(ev.stream_id, [(":status", "200"),
                                          ("content-length", str(len(body)))])
                        conn.send_data(ev.stream_id, body, end_stream=True)
                s.sendall(conn.data_to_send())
        except Exception as e:
            with open(log, "a") as f: f.write("H2 error %r\n" % e)
    while True:
        raw, _ = ls.accept()
        threading.Thread(target=one, args=(raw,), daemon=True).start()
if h2port != "0":
    threading.Thread(target=h2_serve, daemon=True).start()
a.serve_forever()
