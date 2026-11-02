"""HTTP/file/inline proxy-provider preparation and native selector/urltest IO."""
import json
import pathlib
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Node, port, request


def run_case(compiler, engine, root, service, kind):
    root.mkdir()
    Node.requests = 0
    automatic = kind.endswith("-urltest")
    controller, mixed, native = port(), port(), port()
    subprocess.run([compiler, "--proxy-provider-runtime-fixture", str(root),
                    str(controller), str(mixed), str(native),
                    str(service.server_address[1]), kind], check=True, timeout=15)
    generated = root / "provider.json"
    config = json.loads(generated.read_text())
    assert config["route"]["final"] == "Proxy"
    group = next(item for item in config["outbounds"] if item.get("tag") == "Proxy")
    expected = ["dead-node", "remote-node"] if automatic else ["remote-node"]
    assert group["outbounds"] == expected
    assert group["type"] == ("urltest" if automatic else "selector")
    assert not any(item.get("tag") == "filtered-out" for item in config["outbounds"])
    if automatic:
        assert group["url"] == "http://127.0.0.1/provider-probe"
        assert group["interval"] == "1s" and group["tolerance"] == 1
    if kind.startswith("http"):
        assert list((root / "cache").glob("*.cache"))
    else:
        assert not list((root / "cache").glob("*.cache"))
    original = (root / "nodes.yaml").read_bytes() if kind.startswith("file") else None
    subprocess.run([engine, "check", "-c", str(generated), "-D", str(root)],
                   check=True, capture_output=True, timeout=15)
    with (root / "kernel.log").open("wb") as log:
        process = subprocess.Popen([engine, "run", "-c", str(generated), "-D", str(root)],
                                   cwd=root, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    if automatic:
                        probe = urllib.request.Request(
                            f"http://127.0.0.1:{controller}/proxies/Proxy",
                            headers={"Authorization": "Bearer test-native-secret"})
                        with urllib.request.urlopen(probe, timeout=1) as response:
                            status = json.load(response)
                        assert status["now"] == "remote-node", status
                    assert request(mixed, "127.0.0.1", 80) == b"node-ok"
                    break
                except (OSError, EOFError, AssertionError):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError((root / "kernel.log").read_text())
                    time.sleep(0.05)
            assert Node.requests >= 1
            if not automatic:
                assert Node.requests == 1, Node.requests
            if original is not None:
                assert (root / "nodes.yaml").read_bytes() == original
            print(f"proxy_provider_runtime: {kind} real node forwarding passed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


def main():
    compiler, engine = sys.argv[1:]
    service = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Node)
    service.daemon_threads = True
    threading.Thread(target=service.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-proxy-provider-") as temp:
            for kind in ("http", "file", "inline", "http-urltest", "file-urltest", "inline-urltest"):
                run_case(compiler, engine, pathlib.Path(temp) / kind, service, kind)
    finally:
        service.shutdown()
        service.server_close()


if __name__ == "__main__":
    main()
