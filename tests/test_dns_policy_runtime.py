"""Verify production wildcard policy priorities using the pinned kernel's IO."""
import pathlib
import socket
import socketserver
import struct
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Node, dns_name, kernel, query, request


class Resolver(socketserver.BaseRequestHandler):
    def handle(self):
        data, stream = self.request
        name, pos = dns_name(data, 12)
        kind = struct.unpack_from("!H", data, pos)[0]
        self.server.queries.append((name.lower(), kind))
        raw = socket.inet_aton("127.0.0.1") if kind == 1 else b""
        reply = data[:2] + struct.pack("!HHHHH", 0x8180, 1, int(bool(raw)), 0, 0) + data[12:pos + 4]
        if raw:
            reply += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, len(raw)) + raw
        stream.sendto(reply, self.client_address)


def main():
    cases = [
        ("example.test", 10), ("child.example.test", 12), ("fixed.example.test", 13),
        ("child.fixed.example.test", 21), ("a.deep.example.test", 14), ("x.a.deep.example.test", 15),
        ("deep.example.test", 12), ("left.a.example.test", 17), ("left.b.example.test", 16),
        ("other.b.example.test", 21), ("other.a.example.test", 17), ("x.other.a.example.test", 18),
        ("deep.a.test", 20), ("localhost", 22), ("x.localhost", 1),
        ("notexample.test", 1), ("example.test.evil", 1), ("UPPER.EXAMPLE.TEST", 12),
        ("x.y.z.example.test", 11), ("unrelated.test", 1),
        ("scope.b.test", 23), ("x.scope.b.test", 23), ("scope.b.c.test", 1),
        ("tail.b.test", 1), ("x.tail.b.test", 24), ("x.y.tail.b.test", 24), ("x.tail.b.c.test", 1),
        ("_node.example.test", 12), ("left._node.example.test", 16), ("_service", 22),
    ]
    services = {}
    node = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Node)
    node.daemon_threads = True
    threading.Thread(target=node.serve_forever, daemon=True).start()
    try:
        yaml = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")
        for identifier in [1] + list(range(10, 25)):
            server = socketserver.ThreadingUDPServer(("127.0.0.1", 0), Resolver)
            services[identifier] = server
            server.queries, server.daemon_threads = [], True
            threading.Thread(target=server.serve_forever, daemon=True).start()
            yaml = yaml.replace(f":{15300 + identifier}'", f":{server.server_address[1]}'")
        yaml = yaml.replace("port: 11080", f"port: {node.server_address[1]}")
        count = 0
        with tempfile.TemporaryDirectory(prefix="clash-flux-dns-policy-test-") as temp:
            folder = pathlib.Path(temp)
            for dedicated in (False, True):
                source = yaml
                if dedicated:
                    source = source.replace("proxies:\n", (
                        f"  proxy-server-nameserver: ['udp://127.0.0.1:{services[22].server_address[1]}']\n"
                        "  proxy-server-nameserver-policy:\n"
                        f"    'left.*.example.test': 'udp://127.0.0.1:{services[12].server_address[1]}'\n"
                        "proxies:\n"))
                with kernel(sys.argv[1], sys.argv[2], folder, source) as (dns_port, mixed):
                    for domain, selected in cases:
                        before = {key: len(server.queries) for key, server in services.items()}
                        rcode, records = query(dns_port, domain, 1)
                        assert rcode == 0 and records == [("127.0.0.1", 60)], (domain, records)
                        queried = {key for key, server in services.items() if len(server.queries) > before[key]}
                        assert queried == {selected}, (domain, dedicated, queried, selected)
                        assert all(name == domain.lower() and kind == 1
                                   for name, kind in services[selected].queries[before[selected]:])
                        count += 1
                    if dedicated:
                        # Warm the ordinary resolver cache for this exact node
                        # name. Its answer must not replace node policy's query.
                        ordinary = len(services[16].queries)
                        assert query(dns_port, "left.node.example.test", 1)[1] == [("127.0.0.1", 60)]
                        assert ("left.node.example.test", 1) in services[16].queries[ordinary:]
                        count += 1
                    before = {key: len(server.queries) for key, server in services.items()}
                    requests = Node.requests
                    assert request(mixed, "through-node.test", 80) == b"node-ok"
                    assert Node.requests == requests + 1
                    endpoint = 12 if dedicated else 16
                    assert ("left.node.example.test", 1) in services[endpoint].queries[before[endpoint]:]
                    assert all(not any(name == "left.node.example.test" for name, _ in server.queries[before[key]:])
                               for key, server in services.items() if key != endpoint), "node policy must select one resolver"
                    count += 1
        print(f"dns_policy_runtime: {count} cases passed (wildcard boundaries, trie priority and node resolver isolation)")
    finally:
        node.shutdown()
        node.server_close()
        # Shutdown together; avoid accumulating one poll interval per server.
        threads = [threading.Thread(target=server.shutdown) for server in services.values()]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        for server in services.values():
            server.server_close()


if __name__ == "__main__":
    main()
