"""Production DNS compiler and pinned kernel against isolated TLS DNS servers."""
import contextlib
import http.server
import json
import pathlib
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import dns_name, port, query, read_exact


def answer(data):
    _, pos = dns_name(data, 12)
    return (data[:2] + struct.pack("!HHHHH", 0x8180, 1, 1, 0, 0) + data[12:pos + 4]
            + b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + socket.inet_aton("192.0.2.42"))


class TLSAccept:
    def get_request(self):
        stream, address = super().get_request()
        stream.settimeout(3)
        try:
            return self.context.wrap_socket(stream, server_side=True), address
        except ssl.SSLError as error:
            self.failures.append(error.reason)
            stream.close()
            raise OSError("test TLS certificate alert") from error
        except OSError:
            stream.close()
            raise OSError("test TLS handshake rejected")


class TLSServer(TLSAccept, socketserver.ThreadingTCPServer):
    daemon_threads = True


class HTTPSServer(TLSAccept, http.server.ThreadingHTTPServer):
    daemon_threads = True


class DoT(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            while True:
                size = struct.unpack("!H", read_exact(self.request, 2))[0]
                data = read_exact(self.request, size)
                self.server.messages.append(data)
                response = answer(data)
                self.request.sendall(struct.pack("!H", len(response)) + response)
        except (EOFError, OSError):
            pass


class DoH(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        assert self.path == "/custom-query", self.path
        assert self.headers["Host"] == self.server.expected_host, self.headers["Host"]
        data = self.rfile.read(int(self.headers["Content-Length"]))
        self.server.messages.append(data)
        response = answer(data)
        self.send_response(200)
        self.send_header("Content-Type", "application/dns-message")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


@contextlib.contextmanager
def kernel(compiler, engine, folder, endpoint, certificate=None):
    source, generated = folder / "input.yaml", folder / "compiled.json"
    source.write_text("hosts: {resolver.test: 127.0.0.1}\ndns:\n  ipv6: false\n"
                      f"  nameserver: ['{endpoint}']\nrules: ['MATCH,DIRECT']\n", encoding="utf-8")
    subprocess.run([compiler, "--compile-config", str(source), str(generated)], check=True, capture_output=True)
    config = json.loads(generated.read_text())
    if certificate:
        # Test-only root injection verifies hostname checks independently of
        # host OS trust stores; production compilation never injects a CA.
        for server in config["dns"]["servers"]:
            if server.get("type") in ("tls", "https"):
                server.setdefault("tls", {})["certificate"] = [certificate]
    dns_port, mixed = port(socket.SOCK_DGRAM), port()
    config["inbounds"] = [
        {"type": "direct", "tag": "dns-test", "listen": "127.0.0.1", "listen_port": dns_port, "network": "udp"},
        {"type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": mixed}]
    config["route"]["rules"].insert(0, {"inbound": ["dns-test"], "action": "hijack-dns"})
    config["experimental"] = {}
    config["log"] = {"level": "error"}
    generated.write_text(json.dumps(config))
    subprocess.run([engine, "check", "-c", str(generated)], check=True, capture_output=True)
    with (folder / "kernel.log").open("wb") as log:
        process = subprocess.Popen([engine, "run", "-c", str(generated)], cwd=folder, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", mixed), timeout=0.1):
                        break
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError((folder / "kernel.log").read_text())
                    time.sleep(0.01)
            yield dns_port
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


def main():
    cases = 0
    with tempfile.TemporaryDirectory(prefix="clash-flux-dns-tls-test-") as temp:
        folder = pathlib.Path(temp)
        key, cert = folder / "key.pem", folder / "cert.pem"
        subprocess.run([sys.argv[3], "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=resolver.test",
                        "-addext", "subjectAltName=DNS:resolver.test"], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        sni = []
        context.set_servername_callback(lambda _, name, __: sni.append(name))
        for scheme, server_type, handler in [("tls", TLSServer, DoT), ("https", HTTPSServer, DoH)]:
            service = server_type(("127.0.0.1", 0), handler)
            service.context, service.messages, service.failures = context, [], []
            service.expected_host = f"resolver.test:{service.server_address[1]}"
            threading.Thread(target=service.serve_forever, daemon=True).start()
            try:
                path = "/custom-query" if scheme == "https" else ""
                base = f"{scheme}://resolver.test:{service.server_address[1]}{path}"
                for flag, trust, ok in [("", None, False), ("false", None, False), ("true", None, True),
                                        ("false", cert.read_text(), True)]:
                    endpoint = base + (f"#skip-cert-verify={flag}" if flag else "")
                    before, sni_before, failures_before = len(service.messages), len(sni), len(service.failures)
                    with kernel(sys.argv[1], sys.argv[2], folder, endpoint, trust) as dns_port:
                        try:
                            rcode, records = query(dns_port, "answer.test", 1)
                        except TimeoutError:
                            if ok:
                                raise
                            rcode, records = None, []
                        if ok:
                            assert rcode == 0 and records == [("192.0.2.42", 60)], (rcode, records)
                            assert len(service.messages) > before, "expected actual TLS DNS exchange"
                            assert all(dns_name(data, 12)[0] == "answer.test"
                                       for data in service.messages[before:]), "only fixture query may reach TLS DNS"
                        else:
                            assert rcode != 0 and not records, "untrusted certificate must fail DNS lookup"
                            assert len(service.messages) == before, "rejected TLS must carry no DNS payload"
                            assert any("CERTIFICATE" in error or "UNKNOWN_CA" in error
                                       for error in service.failures[failures_before:]), service.failures
                        assert sni[sni_before:] and all(name == "resolver.test" for name in sni[sni_before:]), sni
                    cases += 1
                # Trusted certificate still cannot authenticate an IP endpoint:
                # it has only resolver.test SAN. No silent server_name override.
                for flag, ok in [("false", False), ("true", True)]:
                    endpoint = f"{scheme}://127.0.0.1:{service.server_address[1]}{path}#skip-cert-verify={flag}"
                    before, failures_before = len(service.messages), len(service.failures)
                    service.expected_host = f"127.0.0.1:{service.server_address[1]}"
                    with kernel(sys.argv[1], sys.argv[2], folder, endpoint, cert.read_text()) as dns_port:
                        try:
                            rcode, records = query(dns_port, "answer.test", 1)
                        except TimeoutError:
                            if ok:
                                raise
                            rcode, records = None, []
                        assert (rcode == 0 and bool(records)) == ok, (rcode, records)
                        assert (len(service.messages) > before) == ok
                        if not ok:
                            assert any("CERTIFICATE" in error or "UNKNOWN_CA" in error
                                       for error in service.failures[failures_before:]), service.failures
                    cases += 1
            finally:
                service.shutdown()
                service.server_close()
    print(f"dns_tls_runtime: {cases} cases passed (trust defaults, explicit flags, SNI and hostname checks)")


if __name__ == "__main__":
    main()
