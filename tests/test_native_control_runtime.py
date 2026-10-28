"""Pinned native API and local rule-set updates; loopback only, no TUN/VPN."""
import http.client
import http.server
import json
import os
import pathlib
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exact(sock, count):
    result = b""
    while len(result) < count:
        data = sock.recv(count - len(result))
        if not data:
            raise EOFError("short socket reply")
        result += data
    return result


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        try:
            while data := self.request.recv(4096):
                self.request.sendall(data)
        except (TimeoutError, ConnectionError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def socks(proxy, target):
    sock = socket.create_connection(("127.0.0.1", proxy), timeout=1)
    try:
        sock.sendall(b"\x05\x01\x00")
        assert exact(sock, 2) == b"\x05\x00"
        sock.sendall(b"\x05\x01\x00\x01\x7f\x00\x00\x01" + struct.pack("!H", target))
        reply = exact(sock, 4)
        if reply[1]:
            raise ConnectionError("connection rejected")
        size = {1: 4, 4: 16}.get(reply[3])
        exact(sock, size if size is not None else exact(sock, 1)[0])
        exact(sock, 2)
        sock.sendall(b"probe")
        assert exact(sock, 5) == b"probe"
        return sock
    except BaseException:
        sock.close()
        raise


def native(api, method, token="test-native-secret"):
    connection = http.client.HTTPConnection("127.0.0.1", api, timeout=1)
    try:
        connection.request("POST", "/daemon.StartedService/" + method, b"\0\0\0\0\0", {
            "Content-Type": "application/grpc-web+proto", "X-Grpc-Web": "1",
            "Authorization": "Bearer " + token,
        })
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            raise ConnectionError(f"native API HTTP {response.status}")
        frames = []
        while body:
            assert len(body) >= 5
            flag, length = body[0], struct.unpack("!I", body[1:5])[0]
            assert len(body) >= 5 + length
            frames.append((flag, body[5:5 + length]))
            body = body[5 + length:]
        trailer = b"".join(data for flag, data in frames if flag == 128)
        if b"grpc-status: 0" not in trailer and response.getheader("grpc-status") != "0":
            raise ConnectionError("native API rejected call: " + trailer.decode(errors="replace"))
        messages = [data for flag, data in frames if flag == 0]
        assert len(messages) == 1
        return messages[0]
    finally:
        connection.close()


def wait_for(action, timeout=8):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return action()
        except (OSError, AssertionError, EOFError) as error:
            last = error
            time.sleep(0.05)
    raise AssertionError(f"condition timed out: {last}")


def faults(driver):
    mode = "auth"

    def frame(flag, payload):
        return bytes([flag]) + struct.pack("!I", len(payload)) + payload

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            try:
                if mode == "http-permission":
                    self.send_error(403)
                    return
                if mode == "oversize":
                    body = b"x" * 65537
                elif mode in ("auth", "permission", "unsupported"):
                    status = {"auth": 16, "permission": 7, "unsupported": 12}[mode]
                    body = frame(128, f"grpc-status: {status}\r\n".encode())
                elif mode == "malformed":
                    body = frame(0, b"invalid protobuf") + frame(128, b"grpc-status: 0\r\n")
                else:
                    # Each RPC can finish within one second, but BOTH together
                    # exceed it. The production client must share one budget.
                    time.sleep(0.65)
                    payload = b"\x0a\x06" + b"1.14.3" + b"\x10\x04" if self.path.endswith("GetVersion") else b"\x08\x01"
                    body = frame(0, payload) + frame(128, b"grpc-status: 0\r\n")
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        for mode, phase in [("auth", "authentication"), ("permission", "permission"),
                            ("http-permission", "permission"), ("unsupported", "unsupported"),
                            ("malformed", "invalid-response"), ("oversize", "transport"),
                            ("slow", "transport")]:
            began = time.monotonic()
            result = subprocess.run([str(driver), "--native-runtime", f"http://127.0.0.1:{server.server_port}", "test"],
                                    capture_output=True, timeout=4)
            assert result.returncode != 0 and phase.encode() in result.stderr, (mode, result.stderr)
            if mode == "slow":
                assert time.monotonic() - began < 1.6, "RPCs did not share their deadline"
        server.shutdown()
    print("PASS: native auth/permission/capability/schema/size failures and total RPC budget")


def main():
    assert len(sys.argv) == 3, "fixed core and production C++ probe are required"
    core = pathlib.Path(sys.argv[1]).resolve()
    driver = pathlib.Path(sys.argv[2]).resolve()
    expected_version = subprocess.check_output([str(core), "version"], text=True).splitlines()[0]
    assert expected_version.startswith("sing-box version "), expected_version
    expected_version = expected_version.removeprefix("sing-box version ").encode()
    faults(driver)
    with tempfile.TemporaryDirectory(prefix="clash-flux-native-") as temporary, Server(("127.0.0.1", 0), Echo) as echo:
        root = pathlib.Path(temporary)
        api, proxy = port(), port()
        target = echo.server_address[1]
        threading.Thread(target=echo.serve_forever, daemon=True).start()
        rule_file = root / "live.json"

        def rules(block):
            content = {"version": 3, "rules": [{"ip_cidr": ["127.0.0.1/32" if block else "192.0.2.0/24"]}]}
            staged = root / "live.next"
            staged.write_text(json.dumps(content), encoding="utf-8")
            os.replace(staged, rule_file)

        rules(False)
        config = {
            "log": {"level": "debug", "timestamp": False},
            "inbounds": [{"type": "mixed", "tag": "in", "listen": "127.0.0.1", "listen_port": proxy}],
            "outbounds": [{"type": "direct", "tag": "direct"},
                          {"type": "direct", "tag": "not-yet-connected", "bind_interface": "ClashFluxMissingTest"}],
            "route": {"rules": [{"rule_set": ["live"], "action": "reject"}], "final": "direct",
                      "rule_set": [{"type": "local", "tag": "live", "format": "source", "path": str(rule_file)}]},
            "services": [{"type": "api", "tag": "native", "listen": "127.0.0.1", "listen_port": api,
                          "secret": "test-native-secret", "dashboard": False}],
        }
        config_path = root / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        checked = subprocess.run([str(core), "check", "-c", str(config_path)], capture_output=True)
        assert checked.returncode == 0, checked.stderr.decode(errors="replace")
        with (root / "core.log").open("wb") as log:
            process = subprocess.Popen([str(core), "run", "-c", str(config_path), "-D", str(root)], stdout=log, stderr=log)
            try:
                version = wait_for(lambda: native(api, "GetVersion"))
                assert expected_version in version, version
                started = native(api, "GetStartedAt")
                def production_probe(token="test-native-secret"):
                    if len(sys.argv) < 3:
                        return None
                    return subprocess.run([str(pathlib.Path(sys.argv[2]).resolve()), "--native-runtime",
                                           f"http://127.0.0.1:{api}", token], capture_output=True, timeout=5)

                production = production_probe()
                if production is not None:
                    assert production.returncode == 0, production.stderr.decode(errors="replace")
                    assert production.stdout.startswith(expected_version + b" 4 ")
                    assert production_probe("wrong-secret").returncode != 0
                try:
                    native(api, "GetVersion", "wrong-secret")
                except ConnectionError:
                    pass
                else:
                    raise AssertionError("native API accepted wrong token")
                with wait_for(lambda: socks(proxy, target)) as persistent:
                    rules(True)

                    def blocked():
                        try:
                            with socks(proxy, target):
                                pass
                        except (ConnectionError, EOFError):
                            return True
                        raise AssertionError("new connection still allowed")

                    wait_for(blocked)
                    persistent.sendall(b"still-alive")
                    assert exact(persistent, 11) == b"still-alive"
                    assert native(api, "GetStartedAt") == started
                    if production is not None:
                        assert production_probe().stdout == production.stdout
                    # Invalid updates retain the last valid reject rule.
                    staged = root / "live.next"
                    staged.write_text('{"version":999,"rules":[]}', encoding="utf-8")
                    os.replace(staged, rule_file)
                    def invalid_observed():
                        assert "reload rule-set live" in (root / "core.log").read_text(errors="replace")
                    wait_for(invalid_observed)
                    blocked()
                    rules(False)
                    with wait_for(lambda: socks(proxy, target)):
                        pass
                    assert native(api, "GetStartedAt") == started
                print("PASS: native gRPC-Web auth/readiness; predeclared missing-interface outbound; atomic rule-set update; existing socket retained; invalid-update retention; recovery without restart")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                if process.returncode not in (0, 1, -15):
                    print((root / "core.log").read_text(errors="replace"), file=sys.stderr)
        echo.shutdown()


if __name__ == "__main__":
    main()
