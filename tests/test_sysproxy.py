"""Exercise production desktop adapters without changing the host proxy."""
import json
import os
import shutil
import socket
import time
import urllib.request
import pathlib
import subprocess
import sys
import tempfile

FAKE = r'''#!/usr/bin/env python3
import ast, configparser, json, os, pathlib, sys
tool = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if tool == 'dbus-send': sys.exit(0)
state = pathlib.Path(os.environ['CLASHFLUX_PROXY_TEST_STATE'])
if tool == 'gsettings':
    if args == ['list-schemas']:
        print('org.gnome.system.proxy'); sys.exit(0)
    action, scope, key = args[:3]
    scope = scope.removeprefix('org.gnome.system.proxy').lstrip('.')
    field = scope + '.' + key if scope else key
    values = json.loads(state.read_text())
    if action == 'get': print(values[field]); sys.exit(0)
    if field == os.environ.get('CLASHFLUX_PROXY_FAIL_KEY'): sys.exit(1)
    value = args[3]
    if key == 'port': value = str(int(value))
    elif key == 'ignore-hosts': value = repr(ast.literal_eval(value))
    else: value = repr(value[1:-1] if value.startswith("'") and value.endswith("'") else value)
    values[field] = value
    state.write_text(json.dumps(values)); sys.exit(0)
if tool == 'kwriteconfig6':
    key = args[args.index('--key') + 1]
    if key == os.environ.get('CLASHFLUX_PROXY_FAIL_KEY'): sys.exit(1)
    path = pathlib.Path(os.environ['HOME']) / '.config/kioslaverc'
    cfg = configparser.ConfigParser(interpolation=None); cfg.optionxform = str
    cfg.read(path)
    if '--delete' in args: cfg.remove_option('Proxy Settings', key)
    else: cfg['Proxy Settings'][key] = args[-1]
    with path.open('w') as output: cfg.write(output, space_around_delimiters=False)
    sys.exit(0)
sys.exit(2)
'''


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='clashflux-sysproxy-') as temporary:
        root = pathlib.Path(temporary)
        tools = root / 'tools'
        tools.mkdir()
        for name in ('gsettings', 'kwriteconfig6', 'dbus-send'):
            path = tools / name
            path.write_text(FAKE)
            path.chmod(0o755)
        for desktop in ('gnome', 'kde'):
            home = root / desktop
            (home / '.config').mkdir(parents=True)
            (home / '.config/kioslaverc').write_text(
                '[Proxy Settings]\nProxyType=2\nhttpProxy=http://old.test:8080\n'
                'NoProxyFor=old.test\nProxy Config Script=https://old.test/proxy.pac\nUnrelated=keep\n[Other]\nValue=keep\n')
            values = {'mode': "'auto'", 'ignore-hosts': "['localhost', 'old.test']",
                      'autoconfig-url': "'https://old.test/proxy.pac'"}
            for protocol in ('http', 'https', 'socks'):
                values[protocol + '.host'] = "'old.test'"
                values[protocol + '.port'] = '8080'
            state = home / 'state.json'
            state.write_text(json.dumps(values))
            env = dict(os.environ, HOME=str(home), XDG_CURRENT_DESKTOP='GNOME' if desktop == 'gnome' else 'KDE',
                       PATH=str(tools) + os.pathsep + os.environ['PATH'], CLASHFLUX_PROXY_TEST_STATE=str(state))
            subprocess.run([str(binary), desktop], env=env, check=True, timeout=30)
            assert 'Unrelated=keep' in (home / '.config/kioslaverc').read_text()
    # Real bounded PAC socket, never writes host proxy settings.
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    server = subprocess.Popen([str(binary), '--pac-server', str(port)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True)
    try:
        url = server.stdout.readline().strip()
        assert url == f'http://127.0.0.1:{port}/fixture/proxy.pac', url
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(url, timeout=5) as response:
            assert response.headers['Content-Type'] == 'application/x-ns-proxy-autoconfig'
            assert response.headers['Cache-Control'] == 'no-store'
            script = response.read().decode()
        def request(raw):
            with socket.create_connection(('127.0.0.1', port), timeout=5) as connection:
                connection.sendall(raw)
                return connection.recv(4096)
        host = f'Host: 127.0.0.1:{port}\r\n'
        for line in ['POST /fixture/proxy.pac HTTP/1.1', 'GET /unknown HTTP/1.1']:
            assert b'404' in request((line + '\r\n' + host + '\r\n').encode())
        for headers in ['Host: attacker.test', host + 'Upgrade: websocket', host + 'Content-Length: 999999999']:
            assert b'404' in request(('GET /fixture/proxy.pac HTTP/1.1\r\n' + headers + '\r\n\r\n').encode())
        assert b'404' in request(('GET /fixture/proxy.pac HTTP/1.1\r\n' + host + 'X-Fill: ' + 'a' * 8300).encode())
        node = shutil.which('node')
        if node:
            checks = """
const vm = require('vm');
const context = {
 dnsDomainIs: (host, domain) => host.endsWith(domain),
 shExpMatch: (host, pattern) => new RegExp('^' + pattern.split('*').map(x => x.replace(/[.]/g, '[.]')).join('.*') + '$').test(host)
};
vm.createContext(context); vm.runInContext(SCRIPT, context);
for (const host of ['localhost','127.0.0.1','::1','example.test','x.example.test','x.internal.test','exact.test']) {
 if (context.FindProxyForURL('https://' + host,host) !== 'DIRECT') throw new Error(host);
}
for (const host of ['other.test','notexample.test','exact.test.evil']) {
 if (context.FindProxyForURL('https://' + host,host) !== 'PROXY 127.0.0.1:19099') throw new Error(host);
}
""".replace('SCRIPT', json.dumps(script))
            subprocess.run([node, '-e', checks], check=True, timeout=10)
        else:
            print('PAC JavaScript evaluation skipped: Node unavailable')
        with socket.create_connection(('127.0.0.1', port), timeout=5) as stalled:
            stalled.sendall(b'GET ')
            time.sleep(2.2)
            assert stalled.recv(1) == b''
    finally:
        server.communicate('stop\n', timeout=8)
        assert server.returncode == 0
    with socket.socket() as closed:
        assert closed.connect_ex(('127.0.0.1', port)) != 0
    print('sysproxy: isolated GNOME/KDE PAC round trips, bounded PAC HTTP passed')


if __name__ == '__main__':
    main()
