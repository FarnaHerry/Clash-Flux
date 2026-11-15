"""Production URI compiler with fixed native VLESS/Trojan servers, including Reality."""
import base64
import http.server
import json
import pathlib
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Echo, port, request
from test_protocol_runtime import CountingEcho
from test_shadowsocks_runtime import DatagramEcho, running, udp_request

UUID = "11111111-2222-3333-4444-555555555555"
PASSWORD = " exact URI password +%40: 😀 "


def compile_client(compiler, root, kind, listen, query, provider, certificate):
    quote = lambda text: urllib.parse.quote(text, safe="")
    uri = f"{kind}://{quote(UUID if kind == 'vless' else PASSWORD)}@127.0.0.1:{listen}?"
    uri += "&".join(quote(key) + "=" + quote(value) for key, value in query.items()) + "#URI%20fixture"
    source, generated = root / "source.txt", root / "compiled.json"
    source.write_text(base64.b64encode(uri.encode()).decode() if provider else uri, encoding="utf-8")
    subprocess.run([compiler, "--uri-provider-config" if provider else "--compile-config", str(source), str(generated)],
                   check=True, capture_output=True, timeout=10)
    config = json.loads(generated.read_text(encoding="utf-8"))
    out = next(item for item in config["outbounds"] if item.get("type") == kind)
    assert out["uuid" if kind == "vless" else "password"] == (UUID if kind == "vless" else PASSWORD)
    assert config["route"]["final"] != "direct"
    # Trust only this fixture, preserving URI verification and SNI; Reality uses its own authenticated verifier.
    if "tls" in out:
        assert not out["tls"].get("insecure", False)
        if not out["tls"].get("reality", {}).get("enabled"):
            out["tls"]["certificate"] = [certificate]
    mixed = port()
    config["inbounds"] = [{"type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": mixed}]
    config["experimental"] = {}
    config["log"] = {"level": "error"}
    return config, mixed


def run_case(compiler, engine, root, cert, key, http_port, udp_port, kind, transport="tcp", security="tls",
             provider=False, vision=False, reality=None, bad_sni=False):
    root.mkdir()
    server_dir, client_dir = root / "server", root / "client"
    server_dir.mkdir()
    client_dir.mkdir()
    listen = port()
    inbound = {"type": kind, "tag": "uri-server", "listen": "127.0.0.1", "listen_port": listen,
               "users": [{"name": "fixture", "uuid" if kind == "vless" else "password": UUID if kind == "vless" else PASSWORD}]}
    query = {"security": security, "type": transport}
    if vision:
        query["flow"] = "xtls-rprx-vision"
        inbound["users"][0]["flow"] = "xtls-rprx-vision"
    if security != "none":
        query["sni"] = "wrong.example.test" if bad_sni else "localhost"
        inbound["tls"] = {"enabled": True, "certificate_path": str(cert), "key_path": str(key)}
    if security == "reality":
        private, public, disguise = reality
        inbound["tls"] = {"enabled": True, "server_name": "localhost", "reality": {
            "enabled": True, "private_key": private, "short_id": ["abcd"],
            "handshake": {"server": "127.0.0.1", "server_port": disguise}}}
        query.update({"fp": "chrome", "pbk": public, "sid": "abcd"})
    if transport in ("ws", "httpupgrade", "http"):
        # Host is deliberately different from SNI: the URI must not replace the TLS identity.
        query.update({"host": "transport.example.test", "path": "/uri-path"})
        inbound["transport"] = {"type": transport, "path": "/uri-path"}
        if transport == "http":
            inbound["transport"]["host"] = ["transport.example.test"]
        elif transport == "httpupgrade":
            inbound["transport"]["host"] = "transport.example.test"
    elif transport == "grpc":
        query["serviceName"] = "uri-service"
        inbound["transport"] = {"type": "grpc", "service_name": "uri-service"}
    if security == "tls" and transport != "tcp":
        alpn = "h2" if transport in ("http", "grpc") else "http/1.1"
        query["alpn"] = alpn
        inbound["tls"]["alpn"] = [alpn]
    server = {"log": {"level": "info"}, "inbounds": [inbound], "outbounds": [{"type": "direct", "tag": "direct"}], "route": {"final": "direct"}}
    client, mixed = compile_client(compiler, root, kind, listen, query, provider, cert.read_text(encoding="utf-8"))
    before = CountingEcho.requests
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
                        print("v2ray_uri_runtime: TLS identity rejected without target payload")
                        return
                    if server_process.poll() is not None or client_process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(root.name + "\n" + logs)
                    time.sleep(0.05)
            logs = server_log.read_text(errors="replace")
            assert f"inbound/{kind}[uri-server]" in logs and "inbound connection to" in logs, logs
            print(f"v2ray_uri_runtime: {root.name} TCP + UDP forwarding passed")


def main():
    compiler, engine, openssl = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
    echo = http.server.ThreadingHTTPServer(("127.0.0.1", 0), CountingEcho)
    udp = socketserver.ThreadingUDPServer(("127.0.0.1", 0), DatagramEcho)
    services = [echo, udp]
    for service in services:
        service.daemon_threads = True
        threading.Thread(target=service.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-v2ray-uri-") as temp:
            root = pathlib.Path(temp)
            cert, key = root / "certificate.pem", root / "key.pem"
            subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-sha256", "-nodes",
                            "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                            "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True, timeout=15)
            disguise = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Echo)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_3
            context.load_cert_chain(cert, key)
            disguise.socket = context.wrap_socket(disguise.socket, server_side=True)
            disguise.daemon_threads = True
            services.append(disguise)
            threading.Thread(target=disguise.serve_forever, daemon=True).start()
            keys = subprocess.run([engine, "generate", "reality-keypair"], check=True, capture_output=True, text=True, timeout=10).stdout
            keys = dict(line.split(": ", 1) for line in keys.splitlines() if ": " in line)
            reality = (keys["PrivateKey"], keys["PublicKey"], disguise.server_port)
            cases = [
                ("vless-plain", "vless", "tcp", "none", False, False),
                ("vless-tls", "vless", "tcp", "tls", False, False),
                ("vless-vision", "vless", "tcp", "tls", False, True),
                ("vless-ws-plain", "vless", "ws", "none", False, False),
                ("vless-grpc-plain", "vless", "grpc", "none", False, False),
                ("vless-grpc-provider", "vless", "grpc", "tls", True, False),
                ("vless-http2", "vless", "http", "tls", False, False),
                ("vless-upgrade-plain", "vless", "httpupgrade", "none", False, False),
                ("vless-reality-vision", "vless", "tcp", "reality", False, True),
                ("trojan-tls", "trojan", "tcp", "tls", False, False),
                ("trojan-ws", "trojan", "ws", "tls", False, False),
                ("trojan-grpc", "trojan", "grpc", "tls", False, False),
                ("trojan-http2-provider", "trojan", "http", "tls", True, False),
                ("trojan-upgrade", "trojan", "httpupgrade", "tls", False, False),
            ]
            for name, kind, transport, security, provider, vision in cases:
                run_case(compiler, engine, root / name, cert, key, echo.server_port, udp.server_address[1], kind,
                         transport, security, provider, vision, reality)
            run_case(compiler, engine, root / "bad-sni", cert, key, echo.server_port, udp.server_address[1], "trojan", bad_sni=True)
    finally:
        for service in services:
            service.shutdown()
            service.server_close()


if __name__ == "__main__":
    main()
