"""Optional Linux TUN regression in a disposable user/network namespace.

Requires bwrap, ip, nft, /dev/net/tun and unprivileged user namespaces. Never
runs a host service or writes host routing/DNS/proxy settings. No root prompt.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


def output(*args):
    return subprocess.check_output(args, text=True)


def inside(root, engine, host_namespace):
    assert os.getuid() == 0 and os.stat('/proc/self/ns/net').st_ino != host_namespace
    output('ip', 'link', 'set', 'lo', 'up')
    output('ip', 'link', 'add', 'flux-physical', 'type', 'dummy')
    output('ip', 'addr', 'add', '10.100.1.2/24', 'dev', 'flux-physical')
    output('ip', 'link', 'set', 'flux-physical', 'up')
    output('ip', 'route', 'add', 'default', 'via', '10.100.1.1')
    output('ip', '-6', 'addr', 'add', 'fd01::2/64', 'dev', 'flux-physical')
    output('ip', '-6', 'route', 'add', 'default', 'via', 'fd01::1')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for mode in ('policy', 'redirect', 'empty-policy', 'empty-redirect'):
        redirect = mode.endswith('redirect')
        source = json.loads((root / ('network-sets-' + ('redirect' if redirect else 'policy') + '.json')).read_text())
        tun = next(row for row in source['inbounds'] if row['type'] == 'tun')
        tun['interface_name'] = 'flux-test'
        tun['dns_mode'] = 'disabled'  # Native DNS service is outside this regression.
        empty = mode.startswith('empty-')
        if empty:
            resource = next(row for row in source['route']['rule_set'] if row['tag'] == 'cf_personal_set_capture')
            resource['rules'] = [{'domain': ['fixture.invalid']}]
        config = root / (mode + '-actual.json')
        config.write_text(json.dumps(source))
        with (root / (mode + '-actual.log')).open('w') as log:
            process = subprocess.Popen([str(engine), 'run', '-c', str(config), '-D', str(root)], stdout=log, stderr=log)
            try:
                ready = False
                for _ in range(80):
                    if process.poll() is not None:
                        break
                    try:
                        with opener.open('http://127.0.0.1:29097/version', timeout=.2) as response:
                            ready = response.status == 200
                        if ready:
                            break
                    except OSError:
                        pass
                    time.sleep(.1)
                assert ready, (root / (mode + '-actual.log')).read_text()
                for target, capture in [('192.0.2.10', True), ('192.0.2.200', False),
                                        ('198.51.100.10', False), ('2001:db8:1::10', True), ('2001:db9::10', False)]:
                    route = json.loads(output('ip', '-j', 'route', 'get', target))[0]
                    if not redirect:
                        expected = 'flux-test' if capture and not empty else 'flux-physical'
                        assert route.get('dev') == expected, (mode, target, route)
                if redirect:
                    rules = output('nft', 'list', 'ruleset')
                    assert 'table inet sing-box' in rules
                    # Empty-set capture must retain an explicit IP restriction;
                    # the core must not fall back to capturing every destination.
                    if empty:
                        assert '0.0.0.0' in rules and '::' in rules, rules
                print(mode + ': started, route/table checks passed', flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        assert not any(row['ifname'] == 'flux-test' for row in json.loads(output('ip', '-j', 'link')))
        assert 'sing-box' not in output('nft', 'list', 'tables')
        print(mode + ': TUN and nftables cleanup passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-binary', type=pathlib.Path, default=pathlib.Path('build/test_singbox'))
    parser.add_argument('--singbox', type=pathlib.Path, default=pathlib.Path('build/engines/sing-box'))
    parser.add_argument('--inside', type=pathlib.Path, help=argparse.SUPPRESS)
    parser.add_argument('--host-namespace', type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inside:
        inside(args.inside, args.singbox, args.host_namespace)
        return
    for name in ('bwrap', 'ip', 'nft'):
        assert shutil.which(name), 'Missing prerequisite: ' + name
    assert pathlib.Path('/dev/net/tun').exists(), 'Missing /dev/net/tun'
    with tempfile.TemporaryDirectory(prefix='clashflux-network-capture-') as temporary:
        root = pathlib.Path(temporary)
        subprocess.run([str(args.fixture_binary.resolve()), '--network-settings-fixtures', str(root)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(['bwrap', '--die-with-parent', '--unshare-user', '--unshare-net', '--uid', '0', '--gid', '0',
                        '--cap-add', 'CAP_NET_ADMIN', '--cap-add', 'CAP_NET_RAW', '--ro-bind', '/', '/',
                        '--bind', str(root), str(root), '--dev-bind', '/dev', '/dev', '--tmpfs', '/run',
                        '--proc', '/proc', '--', sys.executable, str(pathlib.Path(__file__).resolve()),
                        '--inside', str(root), '--host-namespace', str(os.stat('/proc/self/ns/net').st_ino),
                        '--singbox', str(args.singbox.resolve())], check=True, timeout=75)
    print('Disposable namespace removed; host network untouched')


if __name__ == '__main__':
    main()
