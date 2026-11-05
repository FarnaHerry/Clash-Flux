"""DNS provider selection and IP-only routing through the production compiler."""
import pathlib
import socket
import socketserver
import struct
import tempfile
import threading

from test_dns_hosts_runtime import dns_name, kernel, query, read_exact


class Resolver(socketserver.BaseRequestHandler):
    def handle(self):
        data, stream = self.request
        name, end = dns_name(data, 12)
        kind = struct.unpack_from('!H', data, end)[0]
        self.server.queries.append((name.lower(), kind))
        raw = socket.inet_aton('127.0.0.1') if kind == 1 else socket.inet_pton(socket.AF_INET6, '::1') if kind == 28 else b''
        response = data[:2] + struct.pack('!HHHHH', 0x8180, 1, bool(raw), 0, 0) + data[12:end + 4]
        if raw:
            response += b'\xc0\x0c' + struct.pack('!HHIH', kind, 1, 60, len(raw)) + raw
        stream.sendto(response, self.client_address)


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        assert read_exact(self.request, 11) == b'flux-probe\n'
        self.request.sendall(b'direct')


class Proxy(socketserver.BaseRequestHandler):
    def handle(self):
        stream = self.request
        stream.settimeout(3)
        greeting = read_exact(stream, 2)
        read_exact(stream, greeting[1])
        stream.sendall(b'\x05\x00')
        header = read_exact(stream, 4)
        read_exact(stream, 4 if header[3] == 1 else 16 if header[3] == 4 else read_exact(stream, 1)[0])
        read_exact(stream, 2)
        stream.sendall(b'\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00')
        assert read_exact(stream, 11) == b'flux-probe\n'
        stream.sendall(b'proxy')


def ip_request(mixed, address, destination):
    with socket.create_connection(('127.0.0.1', mixed), timeout=3) as stream:
        stream.sendall(b'\x05\x01\x00')
        assert read_exact(stream, 2) == b'\x05\x00'
        family, atyp = (socket.AF_INET6, 4) if ':' in address else (socket.AF_INET, 1)
        stream.sendall(b'\x05\x01\x00' + bytes([atyp]) + socket.inet_pton(family, address) + struct.pack('!H', destination))
        header = read_exact(stream, 4)
        assert header[1] == 0, header
        read_exact(stream, 4 if header[3] == 1 else 16 if header[3] == 4 else read_exact(stream, 1)[0])
        read_exact(stream, 2)
        # No HTTP Host / TLS SNI / QUIC: sniffing cannot recover this name.
        stream.sendall(b'flux-probe\n')
        return stream.recv(64)


def run(compiler, engine):
    servers = [socketserver.ThreadingUDPServer(('127.0.0.1', 0), Resolver) for _ in range(3)]
    direct = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Echo)
    proxy = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Proxy)
    for service in servers + [direct, proxy]:
        service.daemon_threads = True
        service.queries = []
        threading.Thread(target=service.serve_forever, daemon=True).start()
    default, domestic, exact = [f'udp://127.0.0.1:{s.server_address[1]}' for s in servers]
    yaml = f'''rule-providers:
  cn: {{type: inline, behavior: domain, payload: ['+.domestic.test']}}
  other: {{type: inline, behavior: domain, payload: ['+.other.test']}}
dns:
  ipv6: false
  nameserver: ['{default}']
  nameserver-policy:
    'rule-set:cn,other': '{domestic}'
    'exact.domestic.test': '{exact}'
    'exact.foreign.test': '{exact}'
proxies:
  - {{name: node, type: socks5, server: 127.0.0.1, port: {proxy.server_address[1]}}}
rules: ['RULE-SET,cn,DIRECT', 'RULE-SET,other,DIRECT', 'MATCH,node']
'''
    try:
        with tempfile.TemporaryDirectory(prefix='clashflux-dns-routing-') as temp:
            folder = pathlib.Path(temp)
            # Negative controls reproduce both old omissions independently.
            for mode in ('old-reverse', 'old-policy', 'fixed'):
                def transform(config):
                    if mode == 'old-reverse':
                        config['dns']['reverse_mapping'] = False
                    if mode == 'old-policy':
                        config['dns']['rules'] = [r for r in config['dns']['rules'] if 'rule_set' not in r]
                with kernel(compiler, engine, folder, yaml, transform) as (dns_port, mixed):
                    for domain, expected in [('child.domestic.test', 1), ('root.other.test', 1),
                                             ('exact.domestic.test', 1), ('exact.foreign.test', 2),
                                             ('foreign.test', 0)]:
                        before = [len(s.queries) for s in servers]
                        assert query(dns_port, domain, 1)[1] == [('127.0.0.1', 60)]
                        selected = {i for i, s in enumerate(servers) if len(s.queries) > before[i]}
                        wanted = (2 if domain == 'exact.domestic.test' else 0) if mode == 'old-policy' and expected == 1 else expected
                        assert selected == {wanted}, (mode, domain, selected, wanted)
                        expected_route = b'direct' if expected == 1 and mode != 'old-reverse' else b'proxy'
                        assert ip_request(mixed, '127.0.0.1', direct.server_address[1]) == expected_route, (mode, domain)
        print('dns_rule_set_runtime: old DNS/routing omissions reproduced; fixed provider DNS priority and IP-only direct/proxy split passed')
    finally:
        workers = [threading.Thread(target=s.shutdown) for s in servers + [direct, proxy]]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join()
        for service in servers + [direct, proxy]:
            service.server_close()
