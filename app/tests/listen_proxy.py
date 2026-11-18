# Dev proxy for Listen-pane UI work (see ENGINE_API.md): /api/listen* -> tests/listen_serve; a read-only allowlist -> the running engine (17770); everything else mocked.
#   python3 tests/listen_proxy.py 17842 http://127.0.0.1:17841   then open ui/index.html?port=17842 from a static server
import http.server, socketserver, sys, urllib.request, os
LISTEN, ENGINE = sys.argv[2], "http://127.0.0.1:17770"
READ_ONLY = {"/api/rack", "/api/telemetry", "/api/scope", "/api/library", "/api/components", "/api/audio", "/api/tracks",
             "/api/cabinets", "/api/nam", "/api/plugins", "/api/usercomponents", "/api/trainer", "/api/tempo", "/api/looper", "/api/backing"}
import json, urllib.parse
GAINS = {"rig_input_db": 0.0, "output_gain_db": 0.0}   # emulated top-bar levels (never forwarded to the engine)
def with_gains(body):
    try:
        d = json.loads(body); cur = d.get("current")
        if isinstance(cur, dict):
            for k, v in GAINS.items(): cur.setdefault(k, v)
        return json.dumps(d).encode()
    except Exception: return body
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        path, _, q = self.path.partition("?")
        if path == "/api/audio/set":
            qs = urllib.parse.parse_qs(q)
            if qs and set(qs) <= set(GAINS):
                for k in qs: GAINS[k] = float(qs[k][0])
                self.path = "/api/audio"; path, q = "/api/audio", ""
        if path.startswith("/api/listen"): url = LISTEN + self.path
        elif path in READ_ONLY and (not q or path == "/api/scope"): url = ENGINE + self.path
        else:
            body = b'{"ok":false,"mock":true}'
            self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"X-Headroom-Token": os.environ.get("HEADROOM_API_TOKEN", "")} if url.startswith(ENGINE + "/") else {}), timeout=3) as r: body, code = r.read(), r.status
            if path == "/api/audio":
                d = json.loads(body); d.setdefault("current", {}).update(GAINS); body = json.dumps(d).encode()
        except Exception as e: body, code = b'{"error":"proxy"}', 502
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        body = b'{"ok":false,"mock":true}'
        self.send_response(200); self.send_header("Access-Control-Allow-Origin", "*"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
class T(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
T(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
