"""Production HY2/TUIC/AnyTLS URI compiler against pinned loopback native servers."""
import base64
import contextlib
import http.server
import json
import pathlib
import select
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import port, read_exact, request
from test_protocol_runtime import CountingEcho
from test_shadowsocks_runtime import DatagramEcho, running, udp_request

UUID = "11111111-2222-3333-4444-555555555555"
PASSWORD = " user: exact password +%40: 😀 "


def hello_names(record):
    """Observe the actual plaintext ClientHello without terminating or changing TLS."""
    assert record[0] == 1  # ClientHello handshake
    body = record[4:]
    offset = 35 + body[34]  # legacy version, random, session ID
    size = int.from_bytes(body[offset:offset + 2], "big")
    offset += 2 + size
    offset += 1 + body[offset]  # compression methods
    size = int.from_bytes(body[offset:offset + 2], "big")
    offset += 2
    end, names = offset + size, []
    while offset < end:
        kind, length = struct.unpack("!HH", body[offset:offset + 4])
        data = body[offset + 4:offset + 4 + length]
        offset += 4 + length
        if kind == 0:
            cursor = 2
            while cursor < len(data):
                name_type, name_size = struct.unpack("!BH", data[cursor:cursor + 3])
                assert name_type == 0
                names.append(data[cursor + 3:cursor + 3 + name_size].decode("ascii"))
                cursor += 3 + name_size
    return names


class TlsTap(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            with socket.create_connection(("127.0.0.1", self.server.destination), timeout=2) as remote:
                header = read_exact(self.request, 5)
                assert header[0] == 22
                data = read_exact(self.request, int.from_bytes(header[3:5], "big"))
                self.server.names.append(hello_names(data))
                remote.sendall(header + data)
                while True:
                    ready, _, _ = select.select([self.request, remote], [], [], 5)
                    if not ready:
                        return
                    for source in ready:
                        data = source.recv(65536)
                        if not data:
                            return
                        (remote if source is self.request else self.request).sendall(data)
        except (OSError, EOFError):
            pass  # Normal peer shutdown; missing observations are asserted by the owning test.


@contextlib.contextmanager
def optional_tap(destination, enabled):
    if not enabled:
        yield destination, None
        return
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), TlsTap) as tap:
        tap.daemon_threads = True
        tap.destination, tap.names = destination, []
        threading.Thread(target=tap.serve_forever, daemon=True).start()
        try:
            yield tap.server_address[1], tap
        finally:
            tap.shutdown()


def compile_client(compiler, root, kind, endpoint, query, password, provider, certificate, hopping):
    quote = lambda value: urllib.parse.quote(value, safe="")
    userinfo = (UUID + ":" if kind == "tuic" else "") + quote(password)
    if kind == "hysteria2" and password == "":
        userinfo = ""
    uri = ("hy2" if kind == "hysteria2" else kind) + "://" + (userinfo + "@" if userinfo else "")
    uri += f"127.0.0.1:{endpoint}" + (f"-{endpoint}" if hopping else "") + "/?"
    uri += "&".join(quote(key) + "=" + quote(value) for key, value in query.items()) + "#URI%20fixture"
    source, generated = root / "source.txt", root / "compiled.json"
    source.write_text(base64.b64encode(uri.encode()).decode() if provider else uri, encoding="utf-8")
    subprocess.run([compiler, "--uri-provider-config" if provider else "--compile-config", str(source), str(generated)],
                   check=True, capture_output=True, timeout=10)
    config = json.loads(generated.read_text(encoding="utf-8"))
    out = next(item for item in config["outbounds"] if item.get("type") == kind)
    assert out["password"] == password and out["tls"]["enabled"] and not out["tls"].get("insecure", False)
    assert ("utls" in out["tls"]) == (kind == "anytls")
    if hopping:
        assert "server_port" not in out and out["server_ports"] == [f"{endpoint}:{endpoint}"]
        assert out["hop_interval"] == out["hop_interval_max"] == "30s"
    if "udp_relay_mode" in query:
        assert out["udp_relay_mode"] == query["udp_relay_mode"]
    assert config["route"]["final"] != "direct"
    # This private test root does not change URI SNI or enabled certificate verification.
    out["tls"]["certificate"] = [certificate]
    mixed = port()
    config["inbounds"] = [{"type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": mixed}]
    config["experimental"] = {}
    config["log"] = {"level": "error"}
    return config, mixed


def run_case(compiler, engine, root, cert, key, http_port, udp_port, kind, *, obfs="", provider=False, hopping=False,
             empty_password=False, mode=None, congestion=None, disable_sni=False, zero_rtt=False, ip_sni=False, bad_sni=False):
    root.mkdir()
    server_dir, client_dir = root / "server", root / "client"
    server_dir.mkdir()
    client_dir.mkdir()
    listen = port()
    password = "" if empty_password else PASSWORD
    inbound = {"type": kind, "tag": "uri-server", "listen": "127.0.0.1", "listen_port": listen,
               "users": [{"name": "fixture", "password": password}],
               "tls": {"enabled": True, "certificate_path": str(cert), "key_path": str(key)}}
    query = {"sni": "wrong.example.test" if bad_sni else "127.0.0.1" if ip_sni else "localhost"}
    if obfs:
        inbound["obfs"] = {"type": obfs, "password": " exact obfs +%40: 😀 "}
        query.update({"obfs": obfs, "obfs-password": inbound["obfs"]["password"]})
    if kind == "tuic":
        inbound["users"][0]["uuid"] = UUID
        if mode:
            query["udp_relay_mode"] = mode
        if congestion:
            query["congestion_control"] = congestion
            inbound["congestion_control"] = congestion
        if disable_sni:
            query["disable_sni"] = "1"
        if zero_rtt:
            query["zero_rtt_handshake"] = "true"
        if provider:
            query["alpn"] = "uri-test"
            inbound["tls"]["alpn"] = ["uri-test"]
    if kind == "anytls" and provider:
        query["fp"] = "firefox"
        query["alpn"] = "uri-test"
        inbound["tls"]["alpn"] = ["uri-test"]
    server = {"log": {"level": "info"}, "inbounds": [inbound], "outbounds": [{"type": "direct", "tag": "direct"}], "route": {"final": "direct"}}
    before = CountingEcho.requests
    with optional_tap(listen, kind == "anytls" and ip_sni) as (endpoint, tap):
        client, mixed = compile_client(compiler, root, kind, endpoint, query, password, provider, cert.read_text(encoding="utf-8"), hopping)
        with running(engine, server_dir, server) as (server_process, server_log):
            with running(engine, client_dir, client) as (client_process, client_log):
                deadline = time.monotonic() + 8
                while True:
                    try:
                        assert request(mixed, "127.0.0.1", http_port) == b"echo-ok"
                        if bad_sni:
                            raise RuntimeError("invalid TLS identity reached the target")
                        udp_request(mixed, udp_port)
                        break
                    except (OSError, EOFError, AssertionError, IndexError):
                        logs = server_log.read_text(errors="replace") + client_log.read_text(errors="replace")
                        if bad_sni and "certificate" in logs and ("x509" in logs or "bad certificate" in logs):
                            assert CountingEcho.requests == before, logs
                            print(f"secure_uri_runtime: {kind} TLS identity rejected without target payload")
                            return
                        if server_process.poll() is not None or client_process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError(root.name + "\n" + logs)
                        time.sleep(0.05)
                logs = server_log.read_text(errors="replace")
                assert f"inbound/{kind}[uri-server]" in logs and "inbound connection to" in logs, logs
                if tap:
                    assert tap.names and all(names == [] for names in tap.names), tap.names
                    print("secure_uri_runtime: AnyTLS IP identity verified with no SNI in actual ClientHello")
                print(f"secure_uri_runtime: {root.name} TCP + UDP forwarding passed")


def main():
    compiler, engine, openssl = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
    echo = http.server.ThreadingHTTPServer(("127.0.0.1", 0), CountingEcho)
    udp = socketserver.ThreadingUDPServer(("127.0.0.1", 0), DatagramEcho)
    for service in (echo, udp):
        service.daemon_threads = True
        threading.Thread(target=service.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-secure-uri-") as temp:
            root = pathlib.Path(temp)
            cert, key = root / "certificate.pem", root / "key.pem"
            subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-sha256", "-nodes",
                            "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                            "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True, timeout=15)
            for name, kind, options in [
                ("hy2-userpass", "hysteria2", {}),
                ("hy2-no-auth", "hysteria2", {"empty_password": True}),
                ("hy2-salamander", "hysteria2", {"obfs": "salamander"}),
                ("hy2-gecko-provider", "hysteria2", {"obfs": "gecko", "provider": True}),
                ("hy2-range-bootstrap", "hysteria2", {"hopping": True}),
                ("tuic-defaults", "tuic", {}),
                ("tuic-native-zero-rtt", "tuic", {"mode": "native", "congestion": "bbr", "zero_rtt": True}),
                ("tuic-quic-provider", "tuic", {"mode": "quic", "congestion": "new_reno", "disable_sni": True, "provider": True}),
                ("tuic-empty-password", "tuic", {"empty_password": True, "congestion": "cubic"}),
                ("anytls-defaults", "anytls", {}),
                ("anytls-ip-sni", "anytls", {"ip_sni": True}),
                ("anytls-provider", "anytls", {"provider": True}),
            ]:
                run_case(compiler, engine, root / name, cert, key, echo.server_port, udp.server_address[1], kind, **options)
            for kind in ("hysteria2", "tuic", "anytls"):
                run_case(compiler, engine, root / (kind + "-bad-sni"), cert, key, echo.server_port, udp.server_address[1], kind,
                         bad_sni=True, disable_sni=kind == "tuic")
    finally:
        for service in (echo, udp):
            service.shutdown()
            service.server_close()


if __name__ == "__main__":
    main()
