"""Pinned loopback HY1/HY2/TUIC/AnyTLS/Snell servers and production client compiler."""
import base64
import http.server
import json
import pathlib
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Echo, port, request
from test_shadowsocks_runtime import DatagramEcho, running, udp_request


class CountingEcho(Echo):
    requests = 0

    def do_GET(self):
        type(self).requests += 1
        super().do_GET()


def compile_client(compiler, folder, fields, provider, certificate):
    text = ", ".join(key + ": " + json.dumps(value, ensure_ascii=False) for key, value in fields.items())
    if provider:
        yaml = "proxy-providers:\n  fixture:\n    type: inline\n    payload:\n      - {" + text + "}\n"
    else:
        yaml = "proxies:\n  - {" + text + "}\n"
    source, generated = folder / "source.yaml", folder / "compiled.json"
    source.write_text(yaml + "rules:\n  - MATCH,loopback-node\n", encoding="utf-8")
    subprocess.run([compiler, "--compile-config", str(source), str(generated)], check=True, capture_output=True, timeout=10)
    config = json.loads(generated.read_text(encoding="utf-8"))
    out = next(item for item in config["outbounds"] if item.get("server") == "127.0.0.1")
    assert config["route"]["final"] == out["tag"]
    if out["type"] != "snell":
        assert out["tls"]["enabled"] and not out["tls"].get("insecure", False)
        assert ("utls" in out["tls"]) == (out["type"] == "anytls")
        # Trust only this test's certificate; preserve enabled verification and the compiled server name.
        out["tls"]["certificate"] = [certificate]
    mixed = port()
    config["inbounds"] = [{"type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": mixed}]
    config["experimental"] = {}
    config["log"] = {"level": "error"}
    return config, mixed


def run_case(compiler, engine, root, cert_path, key_path, http_port, udp_port, name, kind, provider, obfs="", bad_sni=False):
    root.mkdir()
    server_dir, client_dir = root / "server", root / "client"
    server_dir.mkdir()
    client_dir.mkdir()
    listen = port()
    secret, obfs_key = " exact protocol password +%40 ", " exact obfs password "
    uuid = "11111111-2222-3333-4444-555555555555"
    inbound = {"type": kind, "tag": "protocol-server", "listen": "127.0.0.1", "listen_port": listen}
    fields = {"name": "loopback-node", "type": "hy2" if kind == "hysteria2" else kind, "server": "127.0.0.1", "udp": True}
    if kind in ("hysteria", "hysteria2"):
        fields.update({"ports": str(listen), "hop-interval": "10-20" if kind == "hysteria2" else 10})
    else:
        fields["port"] = listen
    if kind != "snell":
        inbound["tls"] = {"enabled": True, "certificate_path": str(cert_path), "key_path": str(key_path)}
        fields["servername"] = "wrong.example.test" if bad_sni else "localhost"
    if kind == "hysteria":
        auth = base64.b64encode(secret.encode()).decode()
        inbound.update({"up_mbps": 64, "down_mbps": 128, "users": [{"name": "fixture", "auth": auth}], "obfs": obfs_key})
        fields.update({"up": "8 MBps", "down": 128, "auth": auth, "auth-str": "ignored text", "obfs": obfs_key,
                       "recv-window-conn": 1048576, "recv-window": 4194304})
    elif kind == "hysteria2":
        inbound.update({"users": [{"name": "fixture", "password": secret}], "obfs": {"type": obfs, "password": obfs_key}})
        fields.update({"password": secret, "obfs": obfs, "obfs-password": obfs_key})
        if obfs == "gecko":
            fields.update({"obfs-min-packet-size": 0, "obfs-max-packet-size": 0})
    elif kind == "tuic":
        inbound.update({"users": [{"name": "fixture", "uuid": uuid, "password": secret}], "congestion_control": "bbr"})
        inbound["tls"]["alpn"] = ["h3"]
        fields.update({"uuid": uuid, "password": secret, "congestion-controller": "bbr", "disable-sni": False,
                       "reduce-rtt": False, "alpn": ["h3"]})
    elif kind == "anytls":
        inbound["users"] = [{"name": "fixture", "password": secret}]
        fields.update({"password": secret, "client-metadata": " fixture metadata 😀 ", "min-idle-session": 0})
    else:
        # Fixed native v5 server's non-QUIC wire protocol is compatible with the mapped v4 client.
        inbound.update({"version": 5, "psk": secret})
        fields.update({"version": 4, "psk": secret, "reuse": True})
        if obfs:
            inbound["obfs_mode"] = obfs
            fields["obfs-opts"] = {"mode": obfs, "host": "localhost"}
    server = {"log": {"level": "info"}, "inbounds": [inbound], "outbounds": [{"type": "direct", "tag": "direct"}], "route": {"final": "direct"}}
    client, mixed = compile_client(compiler, root, fields, provider, cert_path.read_text(encoding="utf-8"))
    before = CountingEcho.requests
    with running(engine, server_dir, server) as (server_process, server_log):
        with running(engine, client_dir, client) as (client_process, client_log):
            deadline = time.monotonic() + 8
            while True:
                try:
                    assert request(mixed, "127.0.0.1", http_port) == b"echo-ok"
                    if bad_sni:
                        raise RuntimeError("invalid certificate identity reached the HTTP target")
                    udp_request(mixed, udp_port)
                    break
                except (OSError, EOFError, AssertionError, IndexError):
                    if bad_sni and "certificate" in client_log.read_text(errors="replace"):
                        logs = server_log.read_text(errors="replace") + client_log.read_text(errors="replace")
                        assert "bad certificate" in logs or "x509" in logs, logs
                        assert CountingEcho.requests == before, logs
                        print("protocol_runtime: TLS identity rejection passed without HTTP payload")
                        return
                    if server_process.poll() is not None or client_process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(server_log.read_text(errors="replace") + client_log.read_text(errors="replace"))
                    time.sleep(0.05)
            logs = server_log.read_text(errors="replace")
            assert f"inbound/{kind}[protocol-server]" in logs, logs
            assert "inbound connection to" in logs or "inbound packet connection to" in logs, logs
            print(f"protocol_runtime: {name} TCP + UDP forwarding passed")


def main():
    compiler, engine, openssl = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
    if not pathlib.Path(openssl).is_file():
        raise RuntimeError("OpenSSL CLI is required for isolated TLS fixtures")
    echo = http.server.ThreadingHTTPServer(("127.0.0.1", 0), CountingEcho)
    udp = socketserver.ThreadingUDPServer(("127.0.0.1", 0), DatagramEcho)
    for service in (echo, udp):
        service.daemon_threads = True
        threading.Thread(target=service.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-protocol-") as temp:
            root = pathlib.Path(temp)
            cert, key = root / "certificate.pem", root / "key.pem"
            subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-sha256", "-nodes",
                            "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                            "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True, timeout=15)
            for name, kind, provider, obfs in [
                ("hy1-bytes-hopping", "hysteria", False, ""),
                ("hy2-gecko-provider", "hysteria2", True, "gecko"),
                ("hy2-salamander", "hysteria2", False, "salamander"),
                ("tuic-provider", "tuic", True, ""),
                ("anytls", "anytls", False, ""),
                ("snell-v4", "snell", False, ""),
                ("snell-http-provider", "snell", True, "http"),
            ]:
                run_case(compiler, engine, root / name, cert, key, echo.server_port, udp.server_address[1], name, kind, provider, obfs)
            run_case(compiler, engine, root / "bad-sni", cert, key, echo.server_port, udp.server_address[1],
                     "bad-sni", "anytls", False, bad_sni=True)
    finally:
        for service in (echo, udp):
            service.shutdown()
            service.server_close()


if __name__ == "__main__":
    main()
