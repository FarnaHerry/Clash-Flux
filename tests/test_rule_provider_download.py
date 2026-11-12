"""Exercise the production curl/validate/atomic replace path without user data."""
import argparse
import base64
import http.server
import gzip
import os
import pathlib
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time

GOOD = b"payload: [valid.test]\n"
# Zstandard MRSv1 IPCIDR fixture: exactly 192.0.2.0/24, including NUL bytes.
MRS = base64.b64decode("KLUv/SA+HQEAwE1SUwEBAAEAAQABAP//wAACAP//wAAC/wUQALsUPHDGQxE=")


class PoolHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    lock = threading.Lock()
    active = 0
    peak = 0

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *_):
        pass

    def do_GET(self):
        expected = {"/token-a": "Bearer pool-A", "/token-b": "Bearer pool-B",
                    "/conditional": "Bearer pool-A", "/no-validator": "Bearer pool-A"}
        authorized = self.path not in expected or self.headers.get("Authorization") == expected[self.path]
        if self.path == "/direct-denied":
            authorized = False
        if (self.path == "/reuse" or self.path.startswith("/concurrent-")) and self.headers.get("Authorization"):
            authorized = False
        if self.path == "/no-validator":
            authorized = authorized and self.headers.get("If-None-Match") is None
        status = 200 if authorized else 403
        body = b"payload: [pool.test]\n"
        if self.path == "/conditional" and self.headers.get("If-None-Match") == '"pool"':
            status, body = 304, b""
        if self.path.startswith("/concurrent-"):
            with self.lock:
                type(self).active += 1
                type(self).peak = max(type(self).peak, type(self).active)
            try:
                time.sleep(0.1)
                body = f"payload: [{self.path[1:]}.test]\n".encode()
            finally:
                with self.lock:
                    type(self).active -= 1
        # Only advertise/emit gzip when the client actually supports decoding it.
        compressed = (self.path in ("/gzip", "/gzip-large", "/gzip-short")
                      and "gzip" in self.headers.get("Accept-Encoding", ""))
        if compressed:
            body = gzip.compress(body if self.path != "/gzip-large" else b"X" * 4096)
        self.send_response(status)
        if compressed: self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(body) + (100 if self.path in ("/truncate", "/gzip-short") else 0)))
        self.end_headers()
        try:
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        if self.path in ("/truncate", "/gzip-short"):
            self.close_connection = True


def pool_test(binary, folder, key=None, cert=None):
    servers = [http.server.ThreadingHTTPServer(("127.0.0.1", 0), PoolHandler) for _ in range(3)]
    tls_address = ""
    if key and cert:
        tls = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PoolHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        tls.socket = context.wrap_socket(tls.socket, server_side=True)
        servers.append(tls)
        tls_address = f"https://127.0.0.1:{tls.server_port}"
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        addresses = [f"http://127.0.0.1:{s.server_port}" for s in servers[:3]]
        result = subprocess.run([binary, "--download-pool", addresses[0], tls_address,
                                 addresses[1], addresses[2], str(folder)], capture_output=True, text=True, timeout=30,
                                env={**os.environ, "NO_PROXY": "", "no_proxy": ""})
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert PoolHandler.peak >= 2, "pool serialized independent transfers"
        print(result.stdout, end="")
        print(f"download pool: observed concurrent HTTP handlers={PoolHandler.peak}")
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


class Handler(http.server.BaseHTTPRequestHandler):
    visits = []
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.visits.append(self.path)
        if self.path == "/always304" or (self.path == "/etag" and
                self.headers.get("If-None-Match") == '"fixture"'):
            self.send_response(304)
            self.send_header("ETag", '"fixture"')
            # 304 Content-Length can describe the representation, not a body.
            self.send_header("Content-Length", str(len(GOOD)))
            self.end_headers()
            return
        if self.path == "/headers" and not (
                self.headers.get_all("User-Agent") == ["provider-test/1"]
                and self.headers.get_all("Authorization") == ["Bearer test-only"]
                and self.headers.get_all("X-Token") == ["local-test"]):
            self.send_response(403)
            self.end_headers()
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/good")
            self.end_headers()
            return
        if self.path == "/scheme":
            self.send_response(302)
            self.send_header("Location", "file:///definitely-not-a-rule-provider")
            self.end_headers()
            return
        payload = {"/good": GOOD, "/large": GOOD + b"#" * 4096,
                   "/unknown-size": GOOD + b"#" * 4096,
                   "/bad": b"payload: [valid.test, 'part*.unsupported.test']\n",
                   "/html": b"<html>HTTP 200 error</html>",
                   "/mrs": MRS, "/mrs-bad": MRS[:-1],
                   "/short": GOOD, "/error": GOOD}.get(self.path, GOOD)
        self.send_response(503 if self.path == "/error" else 200)
        if self.path != "/unknown-size":
            self.send_header("Content-Length", str(len(payload) + (100 if self.path == "/short" else 0)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def run(binary, folder, address, name, ok, headers=None, expected=GOOD, preserve=False):
    # Two files model different protected sources; requests may touch only one.
    dest, untouched = folder / "one.yaml", folder / "two.yaml"
    before = dest.read_bytes()
    args = [binary, address + name, str(dest), "256", "ok" if ok else "fail"]
    if headers is not None:
        args.append(headers)
    subprocess.run(args,
                   check=True, timeout=10, capture_output=True)
    assert dest.read_bytes() == (expected if ok and not preserve else before), name
    assert untouched.read_bytes() == b"other stable source", name
    assert not list(folder.glob(".clash-flux-download-*")), name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--openssl", default=shutil.which("openssl"))
    parser.add_argument("--require-tls", action="store_true")
    args = parser.parse_args()
    openssl = args.openssl
    if args.require_tls and not openssl:
        parser.error("required TLS fixture needs the OpenSSL CLI")
    if openssl:
        try:
            subprocess.run([openssl, "version"], check=True, capture_output=True, timeout=10)
        except (OSError, subprocess.SubprocessError) as error:
            parser.error(f"OpenSSL CLI cannot run: {error}")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix="clash-flux-http-test-") as temp:
        folder = pathlib.Path(temp)
        (folder / "one.yaml").write_bytes(b"old source")
        (folder / "two.yaml").write_bytes(b"other stable source")
        address = f"http://127.0.0.1:{server.server_port}"
        try:
            for name, ok in [("/good", True), ("/bad", False), ("/html", False),
                             ("/large", False), ("/unknown-size", False), ("/short", False),
                             ("/error", False), ("/scheme", False), ("/redirect", True)]:
                run(args.binary, folder, address, name, ok)
            run(args.binary, folder, address, "/mrs", True, "mrs", MRS)
            run(args.binary, folder, address, "/mrs-bad", False, "mrs", MRS)
            run(args.binary, folder, address, "/headers", False)
            run(args.binary, folder, address, "/headers", True, "valid")
            visits = list(Handler.visits)
            run(args.binary, folder, address, "/headers", False, "invalid")
            assert Handler.visits == visits, "invalid headers must fail before networking"
            visits = list(Handler.visits)
            run(args.binary, folder, address, "/redirect", False, "valid")
            assert Handler.visits == visits + ["/redirect"], "custom headers must not reach redirect target"
            dest = folder / "one.yaml"
            dest.write_bytes(b"verified existing representation")
            run(args.binary, folder, address, "/etag", True, "not-modified", preserve=True)
            run(args.binary, folder, address, "/always304", False)
            visits = list(Handler.visits)
            run(args.binary, folder, address, "/redirect", False, "not-modified")
            assert Handler.visits == visits + ["/redirect"], "conditional validators must not reach redirect target"
            # No listener: transport failure must not remove an existing file.
            stopped = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            port = stopped.server_port
            stopped.server_close()
            run(args.binary, folder, f"http://127.0.0.1:{port}", "/offline", False)
            if openssl:
                key, cert = folder / "key.pem", folder / "cert.pem"
                subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                "-days", "1", "-subj", "/CN=localhost", "-keyout", str(key),
                                "-out", str(cert)], check=True, capture_output=True, timeout=10)
                tls = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(cert, key)
                tls.socket = context.wrap_socket(tls.socket, server_side=True)
                threading.Thread(target=tls.serve_forever, daemon=True).start()
                try:
                    run(args.binary, folder, f"https://127.0.0.1:{tls.server_port}", "/good", False)
                finally:
                    tls.shutdown()
                    tls.server_close()
                pool_test(args.binary, folder, key, cert)
                print("rule_provider_download: conditional caching, headers and default TLS rejection passed")
            else:
                pool_test(args.binary, folder)
                print("rule_provider_download: conditional caching and headers passed; TLS fixture unavailable")
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
