import http.server, sys, time
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        with open(sys.argv[1], "a", encoding="utf-8") as f: f.write(time.strftime("%H:%M:%S ") + "GET " + self.path + " UA=" + str(self.headers.get("User-Agent")) + "\n")
        self.send_response(404); self.end_headers()
    do_POST = do_GET
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", 1145), H).serve_forever()
