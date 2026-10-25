#!/usr/bin/env python3
"""Linux supplemental integration test, automatically isolated with unshare -Urn.

Usage: python3 tests/test_network_conflict_namespace.py <test_runtime_evidence>
Creates a live TUN in the private namespace; no external packet or host route change.
"""
import fcntl
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    driver = str(Path(sys.argv[1]).resolve())
    if len(sys.argv) == 2:
        parent_namespace = os.readlink('/proc/self/ns/net')
        subprocess.run(['unshare', '--user', '--map-root-user', '--net', sys.executable,
                        str(Path(__file__).resolve()), driver, parent_namespace], check=True)
        return
    # Refuse the host's network namespace even if accidentally run as root.
    if len(sys.argv) != 3 or os.readlink('/proc/self/ns/net') == sys.argv[2]:
        raise RuntimeError('Run in a private network namespace with unshare -Urn')

    def run(*args):
        subprocess.run(args, check=True)

    def probe(mode):
        run(driver, '--conflict-namespace', mode)
        print('network conflict namespace:', mode, 'passed', flush=True)

    with open('/dev/net/tun', 'rb+', buffering=0) as tun:
        fcntl.ioctl(tun, 0x400454ca, struct.pack('16sH', b'other-tun', 0x1001))
        run('ip', 'link', 'set', 'lo', 'up')
        run('ip', 'addr', 'add', '172.19.0.1/30', 'dev', 'other-tun')
        run('ip', 'link', 'set', 'other-tun', 'up')
        run('ip', 'route', 'add', 'default', 'dev', 'other-tun')
        probe('global')
        run('ip', 'route', 'del', 'default', 'dev', 'other-tun')
        run('ip', 'route', 'add', '0.0.0.0/1', 'dev', 'other-tun')
        run('ip', 'route', 'add', '128.0.0.0/1', 'dev', 'other-tun')
        probe('halves')
        run('ip', 'route', 'del', '0.0.0.0/1', 'dev', 'other-tun')
        run('ip', 'route', 'del', '128.0.0.0/1', 'dev', 'other-tun')
        run('ip', '-6', 'addr', 'add', 'fd99:1234::1/126', 'dev', 'other-tun', 'nodad')
        run('ip', '-6', 'route', 'add', 'default', 'dev', 'other-tun')
        probe('global6')
        run('ip', '-6', 'route', 'del', 'default', 'dev', 'other-tun')
        run('ip', '-6', 'route', 'add', '::/1', 'dev', 'other-tun')
        run('ip', '-6', 'route', 'add', '8000::/1', 'dev', 'other-tun')
        probe('halves6')
        run('ip', '-6', 'route', 'del', '::/1', 'dev', 'other-tun')
        run('ip', '-6', 'route', 'del', '8000::/1', 'dev', 'other-tun')
        probe('address')
        run('ip', 'link', 'set', 'other-tun', 'down')
        probe('down')


if __name__ == '__main__':
    main()
