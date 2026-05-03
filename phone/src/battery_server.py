#!/usr/bin/env python3
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8080

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/battery':
            self.send_error(404)
            return
        try:
            out = subprocess.run(
                ['termux-battery-status'],
                capture_output=True, text=True, timeout=15, check=True
            ).stdout
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(out.encode())
        except Exception as e:
            self.send_error(500, str(e))

    def log_message(self, *a):
        pass

HTTPServer(('0.0.0.0', PORT), Handler).serve_forever()
