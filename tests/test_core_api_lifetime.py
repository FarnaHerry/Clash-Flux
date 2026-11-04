"""Hold a real loopback request across production CoreStore hydration.

Private HOME/database; no core, system proxy, TUN or service operations.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


def check(executable, seeded):
    with tempfile.TemporaryDirectory(prefix="clashflux-api-lifetime-") as directory:
        root = Path(directory)
        authorizations = []
        env = {**os.environ, "HOME": directory, "APPDATA": directory,
               "XDG_DATA_HOME": directory, "XDG_CONFIG_HOME": directory,
               "XDG_CACHE_HOME": directory}
        data = root / ("Library/Application Support/clash-flux" if sys.platform == "darwin" else "clash-flux")
        if seeded:
            subprocess.run([executable, "--seed", directory], env=env, check=True,
                           capture_output=True, text=True, timeout=20)

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                authorizations.append(self.headers.get("Authorization"))
                if len(authorizations) == 1:
                    (root / "request-started").touch()
                    deadline = time.monotonic() + 10
                    while not (root / "release-request").exists() and time.monotonic() < deadline:
                        time.sleep(0.005)
                body = b'{"proxies":{}}'
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run([executable, f"http://127.0.0.1:{server.server_port}", directory],
                                    env=env, capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert authorizations == ["Bearer early-request", "Bearer rotated-31"], authorizations
            if seeded:
                with sqlite3.connect(data / "clash-flux.db") as database:
                    rows = database.execute("SELECT id,name,file,selected FROM profiles ORDER BY id").fetchall()
                assert rows == [(1, "preserved-1", "1.yaml", 1), (2, "preserved-2", "2.yaml", 0)], rows
                for identity in (1, 2):
                    assert (data / "profiles" / f"{identity}.yaml").read_bytes() == b"rules: ['MATCH,DIRECT']\n"
            print(("existing: " if seeded else "fresh: ") + result.stdout, end="")
        finally:
            (root / "release-request").touch()
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    executable = str(Path(sys.argv[1]).resolve(strict=True))
    check(executable, False)
    check(executable, True)
