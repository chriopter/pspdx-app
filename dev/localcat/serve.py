#!/usr/bin/env python3
"""One directory over TLS 1.3 on the loopback address, which is all a catalog
on the host has to be. The client is built to trust the CA that signed this
certificate and nothing else, so nothing outside the work directory is
reachable from a build that talks to it.

    serve.py <site dir> <cert> <key> [port]

A file called faults.json beside the site directory -- <site>/../faults.json,
which is the work directory the rig already owns -- breaks requests on
purpose, for the soak's edge campaign. It is read per request, so a fault can
be turned on and off while the emulator is running, and a work directory
without one serves every byte as it always did:

    {"rules": [
      {"path": "/pkgs/x.zip",   "mode": "status",   "status": 404},
      {"path": "/catalog.json", "mode": "truncate", "after": 4000},
      {"path": "/pkgs/y.zip",   "mode": "stall",    "after": 8000,
                                "seconds": 22}
    ]}

mode "status" answers with that status and a short body; "truncate" announces
the whole length and sends `after` bytes before closing, which is what a
killed server or a cut link looks like from the client; "stall" sends `after`
bytes, waits, and then sends the rest, which is what a link that goes quiet
and comes back looks like. A rule matches on the exact path, or on "prefix".
"""
import http.server, json, os, ssl, sys, time

site, cert, key = sys.argv[1], sys.argv[2], sys.argv[3]
port = int(sys.argv[4]) if len(sys.argv) > 4 else 8443
site = os.path.abspath(site)
FAULTS = os.path.join(os.path.dirname(site), "faults.json")
os.chdir(site)

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(cert, key)


def fault_for(path):
    """The rule that governs this request, or None. Read every time: a
    campaign turns a fault on between two requests of the same run."""
    try:
        with open(FAULTS) as fh:
            rules = json.load(fh)
    except (OSError, ValueError):
        return None
    for rule in rules.get("rules", ()):
        if rule.get("path") and rule["path"] == path:
            return rule
        if rule.get("prefix") and path.startswith(rule["prefix"]):
            return rule
    return None


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        rule = fault_for(self.path.split("?")[0])
        if not rule:
            return super().do_GET()
        return self.broken(rule)

    def broken(self, rule):
        mode = rule.get("mode", "status")
        if mode == "status":
            body = b"fault\n"
            self.send_response(int(rule.get("status", 500)))
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return
        try:
            with open(self.translate_path(self.path), "rb") as fh:
                body = fh.read()
        except OSError:
            self.send_error(404)
            return
        cut = int(rule.get("after", 0))
        cut = max(0, min(cut, len(body)))
        # The announced length is the whole file either way: a truncation the
        # client cannot see in the head is the one worth testing.
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(rule.get("declare", len(body))))
        self.end_headers()
        self.wfile.write(body[:cut])
        self.wfile.flush()
        if mode == "stall":
            time.sleep(float(rule.get("seconds", 10)))
            self.wfile.write(body[cut:])
            self.wfile.flush()
        self.close_connection = True


srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Quiet)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
