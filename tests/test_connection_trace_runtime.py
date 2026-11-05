"""Compiler metadata + native catalogs + live old/new connections on loopback.

No app database, service, TUN, system proxy or external network is used.
"""
import http.client
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
sys.dont_write_bytecode = True
from test_native_control_runtime import Server, Echo, socks, port, native


def request(controller, path):
    connection = http.client.HTTPConnection('127.0.0.1', controller, timeout=1)
    try:
        connection.request('GET', path, headers={'Authorization': 'Bearer test-native-secret'})
        response = connection.getresponse()
        assert response.status == 200, response.status
        return response.read()
    finally:
        connection.close()


def run(fixture, helper, engine):
    subprocess.run([helper], check=True, timeout=20)
    with tempfile.TemporaryDirectory(prefix='clashflux-trace-') as directory:
        root = Path(directory)
        subprocess.run([fixture, '--trace-fixtures', directory], check=True, timeout=20)
        with Server(('127.0.0.1', 0), Echo) as target:
            thread = threading.Thread(target=target.serve_forever, daemon=True)
            thread.start()
            try:
                for mode in ('unique', 'duplicate'):
                    controller, proxy, native_port = port(), port(), port()
                    assert len({controller, proxy, native_port}) == 3, 'ephemeral ports collided'
                    config = json.loads((root / (mode + '.json')).read_text())
                    for inbound in config['inbounds']:
                        assert inbound['type'] != 'tun'
                        inbound['listen_port'] = proxy
                    config['experimental']['clash_api']['external_controller'] = f'127.0.0.1:{controller}'
                    for service in config['services']:
                        if service['type'] == 'api':
                            service['listen_port'] = native_port
                    path = root / (mode + '.actual.json')
                    path.write_text(json.dumps(config))
                    with (root / (mode + '.log')).open('wb') as log:
                        process = subprocess.Popen([engine, 'run', '-c', str(path), '-D', directory],
                                                   cwd=root, stdout=log, stderr=log)
                    probe = None
                    old = new = None
                    try:
                        deadline = time.monotonic() + 10
                        while True:
                            assert process.poll() is None, (root / (mode + '.log')).read_text(errors='replace')
                            try:
                                request(controller, '/version')
                                native(native_port, 'GetStartedAt')
                                break
                            except (OSError, http.client.HTTPException):
                                assert time.monotonic() < deadline, 'native core not ready'
                                time.sleep(.03)
                        old = socks(proxy, target.server_address[1])
                        marker = root / mode
                        probe = subprocess.Popen([helper, f'http://127.0.0.1:{controller}',
                                                  f'http://127.0.0.1:{native_port}', str(root / (mode + '.plan')),
                                                  str(marker), mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        deadline = time.monotonic() + 10
                        while not Path(str(marker) + '.ready').exists():
                            if probe.poll() is not None:
                                raise AssertionError(probe.communicate())
                            assert time.monotonic() < deadline, 'trace baseline not ready'
                            time.sleep(.01)
                        new = socks(proxy, target.server_address[1])
                        Path(str(marker) + '.go').touch()
                        out, err = probe.communicate(timeout=15)
                        assert probe.returncode == 0, (probe.returncode, out, err)
                        print(out, end='')
                    finally:
                        if probe and probe.poll() is None:
                            probe.terminate(); probe.wait(timeout=5)
                        for connection in (old, new):
                            if connection: connection.close()
                        process.terminate()
                        try: process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait()
            finally:
                target.shutdown()
                thread.join()


if __name__ == '__main__':
    run(*(str(Path(argument).resolve(strict=True)) for argument in sys.argv[1:4]))
