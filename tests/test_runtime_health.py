#!/usr/bin/env python3
"""Private controller fault injection + uninterrupted real sing-box forwarding."""
import base64
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    driver, core, openssl = sys.argv[1:]
    mixed, controller = port(), port()
    lock = threading.Lock()
    opens = {}
    done = threading.Event()
    forwarded = []
    failures = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *_):
            pass

        def do_HEAD(self):
            if self.path.endswith('/slow'):
                time.sleep(4)
            self.send_response(403 if self.path.endswith('/denied') else 200)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def do_GET(self):
            if self.path == '/payload':
                self.send_response(200)
                self.send_header('Content-Length', str(len(b'health-forwarding-ok')))
                self.end_headers()
                self.wfile.write(b'health-forwarding-ok')
                return
            scenario = self.path.split('/')[1]
            if self.headers.get('Upgrade', '').lower() != 'websocket':
                if scenario == 'timeout':
                    time.sleep(3)
                try:
                    request = urllib.request.Request(f'http://127.0.0.1:{controller}/version',
                        headers={'Authorization': 'Bearer health-fixture-token'})
                    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=1) as response:
                        body = response.read(65536)
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            if scenario in ('auth', 'forbidden', 'unsupported'):
                self.send_error({'auth': 401, 'forbidden': 403, 'unsupported': 404}[scenario])
                return
            key = scenario, self.path.split('?')[0].rsplit('/', 1)[-1]
            with lock:
                opens[key] = opens.get(key, 0) + 1
                ordinal = opens[key]
            digest = hashlib.sha1((self.headers['Sec-WebSocket-Key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()
            self.send_response(101)
            self.send_header('Upgrade', 'websocket')
            self.send_header('Connection', 'Upgrade')
            self.send_header('Sec-WebSocket-Accept', base64.b64encode(digest).decode())
            self.end_headers()
            index = 0
            try:
                while not done.is_set():
                    if select.select([self.connection], [], [], 0.2)[0]:
                        data = self.connection.recv(4096)
                        if not data:
                            break
                        if data[0] & 15 == 8:
                            self.wfile.write(b'\x88\x00')
                            self.wfile.flush()
                            break
                    if key[1] == 'logs':
                        continue  # legal event silence
                    body = b'{"uploadTotal":42,"downloadTotal":84,"connections":[]}'
                    if key[1] == 'traffic':
                        body = b'{"up":0,"down":0}' if ordinal > 1 or index < 2 else b'{}'
                    self.wfile.write(bytes([0x81, len(body)]) + body)
                    self.wfile.flush()
                    index += 1
            except (OSError, ConnectionError):
                pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='clash-flux-health-') as temp:
        root = Path(temp)
        config = {'log': {'level': 'error'}, 'inbounds': [{'type': 'mixed', 'listen': '127.0.0.1', 'listen_port': mixed}],
                  'outbounds': [{'type': 'direct', 'tag': 'direct'}],
                  'experimental': {'clash_api': {'external_controller': f'127.0.0.1:{controller}', 'secret': 'health-fixture-token'}}}
        (root / 'config.json').write_text(json.dumps(config))
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
                        '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem')],
                       check=True, capture_output=True, timeout=15)
        tls_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(root / 'cert.pem', root / 'key.pem')
        tls_server.socket = tls_context.wrap_socket(tls_server.socket, server_side=True)
        threading.Thread(target=tls_server.serve_forever, daemon=True).start()
        with (root / 'core.log').open('w') as output:
            process = subprocess.Popen([core, 'run', '-c', str(root / 'config.json'), '-D', temp], stdout=output, stderr=output)
            try:
                for _ in range(100):
                    try:
                        with socket.create_connection(('127.0.0.1', mixed), timeout=0.2):
                            break
                    except OSError:
                        if process.poll() is not None:
                            raise AssertionError((root / 'core.log').read_text())
                        time.sleep(0.05)
                def traffic():
                    while not done.is_set():
                        try:
                            connection = http.client.HTTPConnection('127.0.0.1', mixed, timeout=2)
                            connection.request('GET', f'http://127.0.0.1:{server.server_port}/payload')
                            response = connection.getresponse()
                            assert response.status == 200 and response.read() == b'health-forwarding-ok'
                            connection.close()
                            forwarded.append(time.monotonic())
                        except Exception as error:
                            failures.append(str(error))
                        done.wait(0.25)
                threading.Thread(target=traffic, daemon=True).start()
                env = dict(os.environ, XDG_DATA_HOME=temp, XDG_CONFIG_HOME=temp, HOME=temp)
                result = subprocess.run([driver, f'ws://127.0.0.1:{server.server_port}', str(mixed),
                                         f'http://127.0.0.1:{server.server_port}/target',
                                         f'https://127.0.0.1:{tls_server.server_port}/target'], env=env,
                                        capture_output=True, text=True, timeout=55)
                assert result.returncode == 0, result.stdout + result.stderr + str(opens) + str([
                    path.read_text()[-6000:] for path in root.rglob('logs.log')])
                assert not failures and len(forwarded) >= 30, (len(forwarded), failures)
                assert opens.get(('stall', 'traffic')) == 2, opens
                assert opens.get(('stall', 'connections')) == 1 and opens.get(('stall', 'logs')) == 1, opens
                assert opens.get(('timeout', 'traffic')) == 2, opens
                assert process.poll() is None, 'telemetry repair terminated the data plane'
                print(result.stdout.strip())
                print(f'real sing-box forwarding stayed available for {len(forwarded)} requests during telemetry faults')
            finally:
                done.set()
                process.terminate()
                process.wait(timeout=5)
                server.shutdown()
                tls_server.shutdown()


if __name__ == '__main__':
    main()
