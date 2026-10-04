"""Exercise the production curl/validate/atomic replace path without user data."""
import argparse
import base64
import http.server
import pathlib
import shutil
import ssl
import subprocess
import tempfile
import threading

GOOD = b"payload: [valid.test]\n"
# Zstandard MRSv1 IPCIDR fixture: exactly 192.0.2.0/24, including NUL bytes.
MRS = base64.b64decode("KLUv/SA+HQEAwE1SUwEBAAEAAQABAP//wAACAP//wAAC/wUQALsUPHDGQxE=")


class Handler(http.server.BaseHTTPRequestHandler):
    visits = []
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.visits.append(self.path)
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


def run(binary, folder, address, name, ok, headers=None, expected=GOOD):
    # Two files model different protected sources; requests may touch only one.
    dest, untouched = folder / "one.yaml", folder / "two.yaml"
    before = dest.read_bytes()
    args = [binary, address + name, str(dest), "256", "ok" if ok else "fail"]
    if headers is not None:
        args.append(headers)
    subprocess.run(args,
                   check=True, timeout=10, capture_output=True)
    assert dest.read_bytes() == (expected if ok else before), name
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
                print("rule_provider_download: 15 cases passed (including headers and default TLS rejection)")
            else:
                print("rule_provider_download: 14 cases passed; TLS fixture unavailable")
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
