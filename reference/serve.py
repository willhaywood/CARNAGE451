#!/usr/bin/env python3
"""Static file server for the reference build, with caching disabled.

Browsers will happily reuse a cached rr.wasm / rr.data across rebuilds. The page
then silently runs an *old* build, which looks exactly like a code change having
no effect -- you edit, rebuild, reload, and see the previous behaviour. Emscripten
output is especially prone to this because the filenames never change.

Sending no-store on every response makes a reload always fetch the current build.

Usage: python3 serve.py [port]     (default 8321, serves ./build)
"""

import functools
import http.server
import os
import sys


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, fmt, *args):
        # Quieter: only report failures, not every 200.
        if args and str(args[1]).startswith(("4", "5")):
            super().log_message(fmt, *args)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8321
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
    handler = functools.partial(NoCacheHandler, directory=root)
    with http.server.ThreadingHTTPServer(("", port), handler) as httpd:
        print(f"serving {root} at http://localhost:{port}/rr.html (no-store)")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
