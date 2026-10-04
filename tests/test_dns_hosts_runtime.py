"""Production compiler + pinned kernel, isolated loopback DNS and SOCKS IO."""
import contextlib
import http.server
import ipaddress
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


def read_exact(stream, size):
    result = b""
    while len(result) < size:
        part = stream.recv(size - len(result))
        if not part:
            raise EOFError("short SOCKS reply")
        result += part
    return result


def dns_name(data, pos):
    parts = []
    while data[pos]:
        size = data[pos]
        parts.append(data[pos + 1:pos + 1 + size].decode("ascii"))
        pos += size + 1
    return ".".join(parts), pos + 1


class Upstream(socketserver.BaseRequestHandler):
    queries = []

    def handle(self):
        data, stream = self.request
        name, pos = dns_name(data, 12)
        kind = struct.unpack_from("!H", data, pos)[0]
        self.queries.append((name.lower(), kind))
        answer = {1: socket.inet_aton("127.0.0.250"), 28: socket.inet_pton(socket.AF_INET6, "::1"),
                  16: b"\x08upstream"}.get(kind, b"")
        response = data[:2] + struct.pack("!HHHHH", 0x8180, 1, int(bool(answer)), 0, 0) + data[12:pos + 4]
        if answer:
            response += b"\xc0\x0c" + struct.pack("!HHIH", kind, 1, 60, len(answer)) + answer
        stream.sendto(response, self.client_address)


class Echo(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", "7")
        self.end_headers()
        self.wfile.write(b"echo-ok")


class Node(socketserver.BaseRequestHandler):
    requests = 0

    def handle(self):
        stream = self.request
        stream.settimeout(3)
        greeting = read_exact(stream, 2)
        read_exact(stream, greeting[1])
        stream.sendall(b"\x05\x00")
        header = read_exact(stream, 4)
        if header[3] == 1:
            read_exact(stream, 4)
        elif header[3] == 4:
            read_exact(stream, 16)
        else:
            read_exact(stream, read_exact(stream, 1)[0])
        read_exact(stream, 2)
        stream.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
        self.__class__.requests += 1
        data = b""
        while b"\r\n\r\n" not in data:
            data += read_exact(stream, 1)
        stream.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nnode-ok")


def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as stream:
        stream.bind(("127.0.0.1", 0))
        return stream.getsockname()[1]


def query(port_number, domain, kind):
    name = b"".join(bytes([len(part)]) + part.encode("ascii") for part in domain.split(".")) + b"\0"
    message = struct.pack("!HHHHHH", 0x4321, 0x100, 1, 0, 0, 0) + name + struct.pack("!HH", kind, 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stream:
        stream.settimeout(0.3)
        stream.sendto(message, ("127.0.0.1", port_number))
        data, _ = stream.recvfrom(65535)
    flags, count = struct.unpack_from("!H", data, 2)[0], struct.unpack_from("!H", data, 6)[0]
    pos = 12 + len(name) + 4
    records = []
    for _ in range(count):
        if data[pos] & 0xC0 == 0xC0:
            pos += 2
        else:
            _, pos = dns_name(data, pos)
        record_type, _, ttl, size = struct.unpack_from("!HHIH", data, pos)
        pos += 10
        raw = data[pos:pos + size]
        records.append((str(ipaddress.ip_address(raw)) if record_type in (1, 28) else raw, ttl))
        pos += size
    return flags & 15, records


def request(mixed, domain, dest_port):
    with socket.create_connection(("127.0.0.1", mixed), timeout=3) as stream:
        stream.sendall(b"\x05\x01\x00")
        assert read_exact(stream, 2) == b"\x05\x00"
        raw = domain.encode("ascii")
        stream.sendall(b"\x05\x01\x00\x03" + bytes([len(raw)]) + raw + struct.pack("!H", dest_port))
        header = read_exact(stream, 4)
        assert header[1] == 0, "kernel SOCKS connection failed"
        if header[3] == 1:
            read_exact(stream, 4)
        elif header[3] == 4:
            read_exact(stream, 16)
        else:
            read_exact(stream, read_exact(stream, 1)[0])
        read_exact(stream, 2)
        stream.sendall(b"GET / HTTP/1.1\r\nHost: " + raw + b"\r\nConnection: close\r\n\r\n")
        response = b""
        while True:
            part = stream.recv(65535)
            if not part:
                break
            response += part
        return response.split(b"\r\n\r\n", 1)[1]


@contextlib.contextmanager
def kernel(compiler, engine, folder, yaml):
    source, generated = folder / "input.yaml", folder / "compiled.json"
    source.write_text(yaml, encoding="utf-8")
    subprocess.run([compiler, "--compile-config", str(source), str(generated)], check=True, capture_output=True)
    config = json.loads(generated.read_text())
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
                    query(dns_port, "ready.test", 1)
                    break
                except (TimeoutError, ConnectionError):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError((folder / "kernel.log").read_text())
            yield dns_port, mixed
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


def main():
    upstream = socketserver.ThreadingUDPServer(("127.0.0.1", 0), Upstream)
    node = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Node)
    echo = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Echo)
    services = [upstream, node, echo]
    for service in services:
        service.daemon_threads = True
        threading.Thread(target=service.serve_forever, daemon=True).start()
    yaml = pathlib.Path(sys.argv[3]).read_text().replace(":15353", f":{upstream.server_address[1]}")
    yaml = yaml.replace("port: 11080", f"port: {node.server_address[1]}")
    cases = 0
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-hosts-test-") as temp:
            folder = pathlib.Path(temp)
            with kernel(sys.argv[1], sys.argv[2], folder, yaml) as (dns_port, mixed):
                for domain, kind, expected in [
                    ("exact.test", 1, ["192.0.2.10", "192.0.2.11"]),
                    ("EXACT.TEST", 28, ["2001:db8::10"]),
                    ("only4.test", 28, []), ("only6.test", 1, [])]:
                    rcode, records = query(dns_port, domain, kind)
                    assert rcode == 0 and [value for value, _ in records] == expected, (domain, records)
                    assert all(ttl == 10 for _, ttl in records), records
                    cases += 1
                assert query(dns_port, "echo.test", 16)[1][0][0] == b"\x08upstream"
                assert query(dns_port, "child.exact.test", 1)[1][0][0] == "127.0.0.250"
                cases += 2
                before = len(Upstream.queries)
                assert request(mixed, "echo.test", echo.server_port) == b"echo-ok"
                assert len(Upstream.queries) == before, "mapped connection must not query nameserver"
                before_node = Node.requests
                assert request(mixed, "through-node.test", echo.server_port) == b"node-ok"
                assert Node.requests == before_node + 1, "mapped node endpoint must be used"
                assert not any(name in ("node.test", "resolver.test", "exact.test", "only4.test", "only6.test")
                               for name, _ in Upstream.queries), "mapped names leaked to fallback DNS"
                cases += 2
            with kernel(sys.argv[1], sys.argv[2], folder, yaml.replace("use-hosts: true", "use-hosts: false")) as (dns_port, mixed):
                assert query(dns_port, "echo.test", 1)[1][0][0] == "127.0.0.250"
                assert query(dns_port, "node.test", 1)[1][0][0] == "127.0.0.250"
                assert request(mixed, "echo.test", echo.server_port) == b"echo-ok"
                assert request(mixed, "through-node.test", echo.server_port) == b"node-ok"
                cases += 4
        print(f"dns_hosts_runtime: {cases} cases passed (DNS replies, direct connection, node and endpoint resolution)")
    finally:
        for service in services:
            service.shutdown()
            service.server_close()


if __name__ == "__main__":
    main()
