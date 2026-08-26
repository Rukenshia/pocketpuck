#!/usr/bin/env python3
"""Expose live Amp thread counts as a tiny HTTP endpoint."""

import argparse
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Stats:
    def __init__(self):
        self._lock = threading.Lock()
        self._value = None

    def update(self, event):
        threads = event.get("threads")
        if not isinstance(threads, list):
            return
        value = {
            "running": sum(thread.get("working") is True for thread in threads),
            "idle": sum(thread.get("working") is False for thread in threads),
            "updatedAt": event.get("updatedAt"),
            "reconnecting": event.get("reconnecting", False),
        }
        with self._lock:
            self._value = value

    def read(self):
        with self._lock:
            return self._value

    def clear(self):
        with self._lock:
            self._value = None


def follow_amp(stats, amp_command):
    while True:
        stats.clear()
        try:
            with subprocess.Popen(
                [amp_command, "top", "--stream-jsonl"],
                stdout=subprocess.PIPE,
                stderr=None,
                text=True,
                bufsize=1,
            ) as process:
                for line in process.stdout:
                    try:
                        stats.update(json.loads(line))
                    except json.JSONDecodeError:
                        print(f"Ignoring invalid amp output: {line.rstrip()}")
        except OSError as error:
            print(f"Unable to start amp: {error}")
        stats.clear()
        print("amp top stopped; restarting in 5 seconds")
        time.sleep(5)


def make_handler(stats):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/stats":
                self.send_error(404)
                return

            value = stats.read()
            if value is None:
                body = json.dumps({"error": "waiting for Amp"}).encode()
                status = 503
            else:
                body = json.dumps(value, separators=(",", ":")).encode()
                status = 200

            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format, *args):
            print(f"{self.client_address[0]} - {format % args}")

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--amp-command", default="amp")
    args = parser.parse_args()

    stats = Stats()
    threading.Thread(
        target=follow_amp, args=(stats, args.amp_command), daemon=True
    ).start()
    server = ThreadingHTTPServer((args.host, args.port), make_handler(stats))
    print(f"PocketPuck bridge listening on http://{args.host}:{args.port}/stats")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
