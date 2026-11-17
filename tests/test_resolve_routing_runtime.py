"""Exercise generated route resolution with a pinned kernel and loopback IO."""
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

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Echo, Node, dns_name, kernel, request, run_kernel


class Resolver(socketserver.BaseRequestHandler):
    def handle(self):
        packet, stream = self.request
        name, end = dns_name(packet, 12)
        kind = struct.unpack_from("!H", packet, end)[0]
        self.server.queries.append((name, kind))
        missing = name.startswith("missing.")
        ip = "192.0.2.123" if name.startswith("poison.") else "127.0.0.1"
        if name.startswith("timeout."):
            return
        answers = []
        if not missing and kind == 1:
            answers.append(socket.inet_aton(ip))
            if name.startswith("multiple."):
                answers.append(socket.inet_aton("127.0.0.2"))
        if not missing and kind == 28 and name.startswith("multiple."):
            answers = [socket.inet_pton(socket.AF_INET6, addr) for addr in ("::1", "2001:db8::2")]
        response = packet[:2] + struct.pack("!HHHHH", 0x8183 if missing else 0x8180,
                                            1, len(answers), 0, 0) + packet[12:end + 4]
        for answer in answers:
            response += b"\xc0\x0c" + struct.pack("!HHIH", kind, 1, 0, len(answer)) + answer
        stream.sendto(response, self.client_address)


class RecordingNode(Node):
    def connected(self, address, port_number):
        self.server.targets.append((address, port_number))


@contextlib.contextmanager
def running(service):
    with service:
        service.daemon_threads = True
        thread = threading.Thread(target=service.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        thread.start()
        try:
            yield service
        finally:
            service.shutdown()
            thread.join()


def main(compiler, engine, baseline=False):
    with tempfile.TemporaryDirectory(prefix="clashflux-resolve-") as directory, \
            running(socketserver.ThreadingUDPServer(("127.0.0.1", 0), Resolver)) as dns, \
            running(socketserver.ThreadingUDPServer(("127.0.0.1", 0), Resolver)) as direct_dns, \
            running(socketserver.ThreadingTCPServer(("127.0.0.1", 0), RecordingNode)) as node, \
            running(http.server.ThreadingHTTPServer(("127.0.0.1", 0), Echo)) as echo:
        root = pathlib.Path(directory)
        dns.queries = []
        direct_dns.queries = []
        node.targets = []
        subprocess.run([compiler, "--resolve-fixtures", str(root)], check=True)

        def transform(config):
            # Retain compiled routes and tags. Supply a deterministic resolver
            # and a SOCKS recorder in place of the subscription's transport.
            config["dns"]["servers"][0] = {
                "type": "udp", "tag": config["dns"]["servers"][0]["tag"],
                "server": "127.0.0.1", "server_port": dns.server_address[1],
            }
            config["dns"]["timeout"] = "300ms"
            outbound = next(item for item in config["outbounds"] if item["type"] != "direct"
                            and item["type"] != "selector")
            tag = outbound["tag"]
            outbound.clear()
            outbound.update(type="socks", tag=tag, server="127.0.0.1", server_port=node.server_address[1])

        def exercise(mixed, domain, expected, query_expected, target=None, literal=False):
            before = len(node.targets), len(dns.queries)
            if expected is None:
                try:
                    request(mixed, domain, echo.server_port, literal)
                except TimeoutError:
                    raise AssertionError(f"kernel did not close the connection to {domain} in time")
                except (OSError, EOFError):
                    pass
                else:
                    raise AssertionError(f"{domain} unexpectedly reached an outbound")
            else:
                assert request(mixed, domain, echo.server_port, literal) == expected, domain
            assert node.targets[before[0]:] == ([(target, echo.server_port)] if target else []), (
                domain, node.targets[before[0]:])
            queried = dns.queries[before[1]:]
            assert bool(queried) == query_expected and all(item[0] == domain for item in queried), (domain, queried)
            return {kind for _, kind in queried}

        for case in ("bare", "mixed"):
            config = json.loads((root / f"{case}.json").read_text(encoding="utf-8"))
            with run_kernel(engine, root, config, transform) as (_, mixed):
                early = "poison.early.test"
                if case == "bare":
                    exercise(mixed, early, b"node-ok", False, early)
                    continue
                exercise(mixed, early, b"node-ok", baseline, "192.0.2.123" if baseline else early)
                missing = "missing.early.test"
                exercise(mixed, missing, None if baseline else b"node-ok", baseline, None if baseline else missing)
                exercise(mixed, "target-ip.test", b"echo-ok", True)
                exercise(mixed, "127.0.0.1", b"echo-ok", False, literal=True)
                exercise(mixed, "missing.required.test", None, True)
                exercise(mixed, "timeout.required.test", None, True)
        if baseline:
            print("resolve_routing_runtime: bare and mixed node routing passed (R0)")
            return

        def clash(*rules, providers="", hosts="", direct_nameserver=False):
            nameservers = f"nameserver: ['udp://127.0.0.1:{dns.server_address[1]}']"
            if direct_nameserver:
                nameservers += f", direct-nameserver: ['udp://127.0.0.1:{direct_dns.server_address[1]}']"
            yaml = (f"proxies: [{{name: egress, type: socks5, server: 127.0.0.1, port: {node.server_address[1]}}}]\n"
                    f"dns: {{ipv6: true, {nameservers}}}\n"
                    f"{providers}{hosts}rules:\n")
            yaml += "".join(f"  - '{rule}'\n" for rule in (*rules, "MATCH,egress"))
            def set_timeout(config):
                config["dns"]["timeout"] = "300ms"

            return kernel(compiler, engine, root, yaml, set_timeout)

        required = "IP-CIDR,127.0.0.1/32,DIRECT"
        no_resolve = required + ",no-resolve"
        required_miss = "IP-CIDR,198.51.100.0/24,egress"
        with clash("DOMAIN-SUFFIX,early.test,egress", required) as (_, mixed):
            for domain in ("poison.early.test", "missing.early.test", "timeout.early.test"):
                exercise(mixed, domain, b"node-ok", False, domain)
            exercise(mixed, "target-ip.test", b"echo-ok", True)
            exercise(mixed, "missing.required.test", None, True)
            exercise(mixed, "timeout.required.test", None, True)
        with clash("DOMAIN,direct.early.test,DIRECT", required, direct_nameserver=True) as (_, mixed):
            exercise(mixed, "direct.early.test", b"echo-ok", False)
            assert {name for name, _ in direct_dns.queries} == {"direct.early.test"}
            before = len(direct_dns.queries)
            exercise(mixed, "direct.late.test", b"echo-ok", True)
            assert len(direct_dns.queries) == before
        with clash(no_resolve) as (_, mixed):
            exercise(mixed, "poison.skip.test", b"node-ok", False, "poison.skip.test")
            exercise(mixed, "127.0.0.1", b"echo-ok", False, literal=True)
        with clash(required_miss, no_resolve) as (_, mixed):
            exercise(mixed, "previous-address.test", b"echo-ok", True)
        with clash(no_resolve, required_miss) as (_, mixed):
            exercise(mixed, "later-address.test", b"node-ok", True, "127.0.0.1")
        with clash(no_resolve, hosts="hosts: {pinned.test: 127.0.0.1}\n") as (_, mixed):
            exercise(mixed, "pinned.test", b"echo-ok", False)
        for cidr in ("IP-CIDR,127.0.0.2/32", "IP-CIDR6,2001:db8::2/128"):
            with clash(required_miss, f"{cidr},REJECT,no-resolve") as (_, mixed):
                assert exercise(mixed, "multiple.record.test", None, True) == {1, 28}

        nested_no_resolve = ("AND,((NETWORK,TCP),(AND,((IP-CIDR,127.0.0.0/8,no-resolve),"
                             "(IP-CIDR,127.0.0.1/32,no-resolve)))),DIRECT")
        for optional_ip in (nested_no_resolve, "GEOIP,private,DIRECT,no-resolve"):
            with clash(optional_ip) as (_, mixed):
                exercise(mixed, "missing.optional.test", b"node-ok", False, "missing.optional.test")
                exercise(mixed, "127.0.0.1", b"echo-ok", False, literal=True)
            with clash(required_miss, optional_ip) as (_, mixed):
                exercise(mixed, "previous-required.test", b"echo-ok", True)

        optional_or = "OR,((DOMAIN,inside.test),(IP-CIDR,127.0.0.0/8,no-resolve),(IP-CIDR6,::1/128,no-resolve)),REJECT"
        optional_not = "NOT,((IP-CIDR,127.0.0.0/8,no-resolve)),REJECT"
        optional_double_not = "NOT,((NOT,((IP-CIDR,127.0.0.0/8,no-resolve)))),REJECT"
        with clash(optional_or) as (_, mixed):
            exercise(mixed, "inside.test", None, False)
            exercise(mixed, "missing.or.test", b"node-ok", False, "missing.or.test")
            exercise(mixed, "127.0.0.1", None, False, literal=True)
            exercise(mixed, "::1", None, False, literal=True)
        with clash(optional_not) as (_, mixed):
            exercise(mixed, "missing.not.test", None, False)
            exercise(mixed, "127.0.0.1", b"node-ok", False, "127.0.0.1", literal=True)
            exercise(mixed, "192.0.2.1", None, False, literal=True)
        with clash(optional_double_not) as (_, mixed):
            exercise(mixed, "missing.double.test", b"node-ok", False, "missing.double.test")
            exercise(mixed, "127.0.0.1", None, False, literal=True)
        for optional in (optional_or, optional_double_not):
            with clash(required_miss, optional) as (_, mixed):
                exercise(mixed, "prepared.logical.test", None, True)
        with clash(required_miss, optional_not) as (_, mixed):
            exercise(mixed, "prepared.not.test", b"node-ok", True, "127.0.0.1")
        with clash(optional_or, required_miss) as (_, mixed):
            exercise(mixed, "later.logical.test", b"node-ok", True, "127.0.0.1")

        guarded = "AND,((DOMAIN,guarded.test),(DST-PORT,{port}),(IP-CIDR,127.0.0.1/32)),DIRECT"
        guarded = guarded.format(port=echo.server_port)
        with clash(guarded) as (_, mixed):
            exercise(mixed, "poison.outside.test", b"node-ok", False, "poison.outside.test")
            exercise(mixed, "guarded.test", b"echo-ok", True)
        with clash(guarded, required) as (_, mixed):
            exercise(mixed, "later-required.test", b"echo-ok", True)
        with clash("OR,((DOMAIN,inside.test),(IP-CIDR,127.0.0.1/32)),DIRECT") as (_, mixed):
            exercise(mixed, "other-branch.test", b"echo-ok", True)
        with clash(guarded, hosts="hosts: {guarded.test: 127.0.0.1}\n") as (_, mixed):
            exercise(mixed, "guarded.test", b"echo-ok", False)
            exercise(mixed, "poison.outside.test", b"node-ok", False, "poison.outside.test")

        def provider(behavior, *payload):
            return (f"rule-providers:\n"
                    f"  sample: {{type: inline, behavior: {behavior}, payload: {json.dumps(payload)}}}\n")

        with clash("RULE-SET,sample,REJECT", providers=provider("domain", "inside.test")) as (_, mixed):
            exercise(mixed, "inside.test", None, False)
            exercise(mixed, "poison.outside.test", b"node-ok", False, "poison.outside.test")
        with clash("RULE-SET,sample,REJECT",
                   providers=provider("classical", "SRC-IP-CIDR,127.0.0.1/32")) as (_, mixed):
            exercise(mixed, "poison.src.test", None, False)
        with clash("RULE-SET,sample,DIRECT", providers=provider("ipcidr", "127.0.0.1/32")) as (_, mixed):
            exercise(mixed, "provider-ip.test", b"echo-ok", True)
        with clash("RULE-SET,sample,DIRECT",
                   providers=provider("classical", "DOMAIN,inside.test", "IP-CIDR,127.0.0.1/32")) as (_, mixed):
            exercise(mixed, "mixed-provider.test", b"echo-ok", True)
        mixed_provider = provider("classical", "DOMAIN,inside.test", "IP-CIDR,127.0.0.1/32")
        optional_set = "RULE-SET,sample,REJECT,no-resolve"
        with clash(optional_set, providers=mixed_provider) as (_, mixed):
            exercise(mixed, "inside.test", None, False)
            exercise(mixed, "missing.outside.test", b"node-ok", False, "missing.outside.test")
            exercise(mixed, "127.0.0.1", None, False, literal=True)
        with clash(required_miss, optional_set, providers=mixed_provider) as (_, mixed):
            exercise(mixed, "prepared-provider.test", None, True)
        ip_first = "AND,((IP-CIDR,198.51.100.0/24),(DOMAIN,guarded.test)),DIRECT"
        domain_first = "AND,((DOMAIN,guarded.test),(IP-CIDR,198.51.100.0/24)),DIRECT"
        with clash(ip_first, optional_set, providers=mixed_provider) as (_, mixed):
            exercise(mixed, "guard-miss.test", None, True)
        with clash(domain_first, optional_set, providers=mixed_provider) as (_, mixed):
            exercise(mixed, "guard-miss.test", b"node-ok", False, "guard-miss.test")
        with clash(optional_set, "RULE-SET,sample,DIRECT", providers=mixed_provider) as (_, mixed):
            exercise(mixed, "second-reference.test", b"echo-ok", True)
        optional_entries = provider("classical", "DOMAIN,inside.test", "IP-CIDR,127.0.0.0/8,no-resolve",
                                    "AND,((NETWORK,TCP),(IP-CIDR6,::1/128,no-resolve))")
        ordinary_set = "RULE-SET,sample,REJECT"
        with clash(ordinary_set, providers=optional_entries) as (_, mixed):
            exercise(mixed, "inside.test", None, False)
            exercise(mixed, "missing.entry.test", b"node-ok", False, "missing.entry.test")
            exercise(mixed, "127.0.0.1", None, False, literal=True)
            exercise(mixed, "::1", None, False, literal=True)
        with clash(required_miss, ordinary_set, providers=optional_entries) as (_, mixed):
            exercise(mixed, "prepared.entry.test", None, True)
        with clash("RULE-SET,sample,DIRECT", providers=provider(
                "classical", "IP-CIDR,192.0.2.0/24,no-resolve", "IP-CIDR,127.0.0.1/32")) as (_, mixed):
            exercise(mixed, "mixed.entry.test", b"echo-ok", True)
        for logical in (optional_or, optional_not, optional_double_not):
            entries = provider("classical", logical.removesuffix(",REJECT"))
            with clash(ordinary_set, providers=entries) as (_, mixed):
                inverted = logical == optional_not
                exercise(mixed, "missing.logical-set.test", None if inverted else b"node-ok",
                         False, None if inverted else "missing.logical-set.test")
                exercise(mixed, "127.0.0.1", b"node-ok" if inverted else None,
                         False, "127.0.0.1" if inverted else None, literal=True)
    print("resolve_routing_runtime: node and Clash routing, logical no-resolve, AND guards and inline sets passed")


if __name__ == "__main__":
    main(*sys.argv[1:3], baseline="--baseline" in sys.argv[3:])
