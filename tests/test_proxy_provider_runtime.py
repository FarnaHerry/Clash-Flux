"""HTTP proxy-provider preparation and real forwarding through its node."""
import json
import pathlib
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Node, port, request


def main():
    compiler, engine = sys.argv[1:]
    Node.requests = 0
    service = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Node)
    service.daemon_threads = True
    threading.Thread(target=service.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-proxy-provider-") as temp:
            root = pathlib.Path(temp)
            controller, mixed, native = port(), port(), port()
            subprocess.run([compiler, "--proxy-provider-runtime-fixture", str(root),
                            str(controller), str(mixed), str(native),
                            str(service.server_address[1])], check=True, timeout=15)
            generated = root / "provider.json"
            config = json.loads(generated.read_text())
            assert config["route"]["final"] == "Proxy"
            group = next(item for item in config["outbounds"] if item.get("tag") == "Proxy")
            assert group["outbounds"] == ["remote-node"]
            assert list((root / "cache").glob("*.cache"))
            subprocess.run([engine, "check", "-c", str(generated), "-D", str(root)],
                           check=True, capture_output=True, timeout=15)
            with (root / "kernel.log").open("wb") as log:
                process = subprocess.Popen([engine, "run", "-c", str(generated), "-D", str(root)],
                                           cwd=root, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 8
                    while True:
                        try:
                            assert request(mixed, "127.0.0.1", 80) == b"node-ok"
                            break
                        except (OSError, AssertionError):
                            if process.poll() is not None or time.monotonic() >= deadline:
                                raise RuntimeError((root / "kernel.log").read_text())
                            time.sleep(0.05)
                    assert Node.requests == 1, Node.requests
                    print("proxy_provider_runtime: real provider node forwarding passed")
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
    finally:
        service.shutdown()
        service.server_close()


if __name__ == "__main__":
    main()
