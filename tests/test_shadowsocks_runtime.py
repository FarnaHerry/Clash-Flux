"""Production SS compiler + pinned client/server TCP, UDP and UOT forwarding."""
import base64
import contextlib
import http.server
import json
import pathlib
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
from test_dns_hosts_runtime import Echo, port, read_exact, request


class DatagramEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, stream = self.request
        stream.sendto(b"ss-udp:" + data, self.client_address)


@contextlib.contextmanager
def running(engine, folder, config):
    path = folder / "config.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    subprocess.run([engine, "check", "-c", str(path), "-D", str(folder)],
                   check=True, capture_output=True, timeout=10)
    log_path = folder / "kernel.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen([engine, "run", "-c", str(path), "-D", str(folder)],
                                   cwd=folder, stdout=log, stderr=log)
        try:
            yield process, log_path
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


def udp_request(mixed, destination):
    with socket.create_connection(("127.0.0.1", mixed), timeout=1) as control:
        control.sendall(b"\x05\x01\x00")
        assert read_exact(control, 2) == b"\x05\x00"
        control.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
        reply = read_exact(control, 4)
        assert reply[:3] == b"\x05\x00\x00", reply
        if reply[3] == 1:
            read_exact(control, 4)
        elif reply[3] == 4:
            read_exact(control, 16)
        else:
            read_exact(control, read_exact(control, 1)[0])
        relay = struct.unpack("!H", read_exact(control, 2))[0]
        payload = b"P3 exact UDP payload \x00\xff+%40"
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stream:
            stream.bind(("127.0.0.1", 0))
            stream.settimeout(0.5)
            stream.sendto(b"\x00\x00\x00\x01\x7f\x00\x00\x01" +
                          struct.pack("!H", destination) + payload, ("127.0.0.1", relay))
            data, _ = stream.recvfrom(4096)
            assert data[:4] == b"\x00\x00\x00\x01", data
            assert data[4:8] == socket.inet_aton("127.0.0.1"), data
            assert struct.unpack("!H", data[8:10])[0] == destination, data
            assert data[10:] == b"ss-udp:" + payload, data


def run_case(compiler, engine, root, http_port, udp_port, name, method, version, network, provider, share=""):
    root.mkdir()
    server_dir, client_dir = root / "server", root / "client"
    server_dir.mkdir()
    client_dir.mkdir()
    server_port, mixed = port(), port()
    password = (base64.b64encode(bytes(range(32))).decode() if method.startswith("2022")
                else " exact SS password +%40 ")
    inbound = {"type": "shadowsocks", "tag": "ss-server", "listen": "127.0.0.1",
               "listen_port": server_port, "method": method, "password": password}
    if network:
        inbound["network"] = network
    server = {"log": {"level": "info"}, "inbounds": [inbound],
              "outbounds": [{"type": "direct", "tag": "direct"}], "route": {"final": "direct"}}
    fields = ["name: local-ss", "type: shadowsocks" if provider else "type: ss",
              "server: 127.0.0.1", f"port: {server_port}", f"cipher: {method}",
              "password: " + json.dumps(password)]
    if version is not None:
        fields += ["udp-over-tcp: " + ("false" if name == "disabled-v2" else "true"),
                   f"udp-over-tcp-version: {version}"]
    if provider:
        yaml = "proxy-providers:\n  fixture:\n    type: inline\n    payload:\n      - {" + ", ".join(fields) + "}\n"
    else:
        yaml = "proxies:\n  - {" + ", ".join(fields) + "}\n"
    yaml += "rules:\n  - MATCH,local-ss\n"
    source, generated = root / "source.yaml", root / "compiled.json"
    if share:
        userinfo = (urllib.parse.quote(method, safe="") + ":" + urllib.parse.quote(password, safe="")
                    if method.startswith("2022") else
                    base64.urlsafe_b64encode((method + ":" + password).encode()).decode().rstrip("="))
        body = f"ss://{userinfo}@127.0.0.1:{server_port}#local-ss\n"
        if share == "legacy":
            raw = f"{method}:{password}@127.0.0.1:{server_port}"
            body = "ss://" + base64.b64encode(raw.encode()).decode().rstrip("=") + "#local-ss\n"
        if share == "mixed":
            vmess = {"ps": "unused-vmess", "add": "example.invalid", "port": "443",
                     "id": "11111111-2222-3333-4444-555555555555", "net": "tcp"}
            body += "vmess://" + base64.b64encode(json.dumps(vmess).encode()).decode()
        yaml = base64.b64encode(body.encode()).decode() if provider else body
    source.write_text(yaml, encoding="utf-8")
    command = "--ss-uri-provider-config" if share and provider else "--compile-config"
    subprocess.run([compiler, command, str(source), str(generated)],
                   check=True, capture_output=True, timeout=10)
    client = json.loads(generated.read_text(encoding="utf-8"))
    ss = next(out for out in client["outbounds"] if out["type"] == "shadowsocks")
    assert ss["method"] == method and ss["password"] == password and not ss.get("plugin")
    if version is None:
        assert "udp_over_tcp" not in ss
    else:
        assert ss["udp_over_tcp"] == {"enabled": name != "disabled-v2", "version": version}
    if share:
        group = next(out for out in client["outbounds"] if out["tag"] == client["route"]["final"])
        assert group["type"] == "selector" and group["outbounds"][0] == ss["tag"]
    else:
        assert client["route"]["final"] == ss["tag"]
    # Only replace managed listeners/control/logging; keep compiled outbounds and routing intact.
    client["inbounds"] = [{"type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": mixed}]
    client["experimental"] = {}
    client["log"] = {"level": "error"}
    with running(engine, server_dir, server) as (server_process, server_log):
        with running(engine, client_dir, client) as (client_process, client_log):
            deadline = time.monotonic() + 6
            while True:
                try:
                    if network != "udp":
                        assert request(mixed, "127.0.0.1", http_port) == b"echo-ok"
                    udp_request(mixed, udp_port)
                    break
                except (OSError, EOFError, AssertionError):
                    if server_process.poll() is not None or client_process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(server_log.read_text(errors="replace") + client_log.read_text(errors="replace"))
                    time.sleep(0.05)
            log = server_log.read_text(errors="replace")
            assert "inbound/shadowsocks[ss-server]" in log, log
            assert "inbound connection to" in log or "inbound packet connection to" in log, log
            if version is not None and name != "disabled-v2":
                assert "udp-over-tcp.arpa" in log, log
            print(f"shadowsocks_runtime: {name} {'UDP' if network == 'udp' else 'TCP + UDP'} forwarding passed")


def main():
    compiler, engine = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
    http_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Echo)
    udp = socketserver.ThreadingUDPServer(("127.0.0.1", 0), DatagramEcho)
    services = [http_server, udp]
    for service in services:
        service.daemon_threads = True
        threading.Thread(target=service.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-shadowsocks-") as temp:
            for name, method, version, network, provider, share in [
                ("aead", "aes-256-gcm", None, "", False, ""),
                ("ss2022-provider", "2022-blake3-aes-256-gcm", None, "", True, ""),
                ("uot-v1-provider", "aes-256-gcm", 1, "tcp", True, ""),
                ("uot-v2", "aes-256-gcm", 2, "tcp", False, ""),
                ("disabled-v2", "aes-256-gcm", 2, "udp", False, ""),
                ("sip002-aead", "aes-256-gcm", None, "", False, "uri"),
                ("sip002-2022-provider", "2022-blake3-aes-256-gcm", None, "", True, "uri"),
                ("sip002-mixed-list", "aes-128-gcm", None, "", False, "mixed"),
                ("legacy-ss", "aes-256-gcm", None, "", False, "legacy"),
                ("legacy-ss-provider", "chacha20-ietf-poly1305", None, "", True, "legacy"),
            ]:
                run_case(compiler, engine, pathlib.Path(temp) / name, http_server.server_port,
                         udp.server_address[1], name, method, version, network, provider, share)
    finally:
        for service in services:
            service.shutdown()
            service.server_close()


if __name__ == "__main__":
    main()
