"""Exercise standby, URL testing and manual proxy listening with the fixed core.

All traffic stays on loopback. No system proxy, TUN, user settings or services.
"""
import http.client
import http.server
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.dont_write_bytecode = True
from test_native_control_runtime import native


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def request(port, path, *, control=False):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request("GET", path, headers={"Authorization": "Bearer test-native-secret"} if control else {})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


class Target(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(204)
        self.end_headers()

    def log_message(self, *_args):
        pass


def run_stage(binary, root, config, controller, proxy, native_port, target):
    document = json.loads(config.read_text(encoding="utf-8"))
    listening = bool(document["inbounds"])
    assert not any(item["type"] == "tun" for item in document["inbounds"])
    with (root / "core.log").open("wb") as log:
        process = subprocess.Popen(
            [binary, "run", "-c", str(config), "-D", str(root)],
            stdout=log, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise AssertionError((root / "core.log").read_text(errors="replace"))
                try:
                    if request(controller, "/version", control=True)[0] == 200 and native(native_port, "GetVersion"):
                        break
                except (OSError, http.client.HTTPException):
                    pass
                assert time.monotonic() < deadline, "controller never became ready"
                time.sleep(0.05)
            status, body = request(controller, "/proxies/DIRECT/delay?" + urllib.parse.urlencode(
                {"url": target, "timeout": 2000}), control=True)
            assert status == 200 and isinstance(json.loads(body).get("delay"), int), body
            if listening:
                status, _ = request(proxy, target)
                assert status == 204, "manual proxy failed with system proxy and TUN both off"
            else:
                try:
                    connection = socket.create_connection(("127.0.0.1", proxy), timeout=0.3)
                except OSError:
                    pass
                else:
                    connection.close()
                    raise AssertionError("standby unexpectedly accepted application traffic")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def main():
    generator, binary = sys.argv[1:]
    ports = set()
    while len(ports) < 3:
        ports.add(free_port())
    controller, proxy, native_port = ports
    with tempfile.TemporaryDirectory(prefix="clash-flux-proxy-runtime-") as directory:
        root = pathlib.Path(directory)
        subprocess.run([generator, "--proxy-runtime-fixtures", str(root), str(controller), str(proxy),
                        str(native_port)], check=True)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Target) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                target = f"http://127.0.0.1:{server.server_port}/generate_204"
                for prefix in ("", "native-"):
                    for state in ("standby", "connected", "standby"):
                        run_stage(binary, root, root / f"{prefix}{state}.json", controller, proxy, native_port, target)
            finally:
                server.shutdown()
                worker.join(timeout=5)
    print("YAML/native: standby URLTest, closed proxy ports, manual proxy and return to standby passed")


if __name__ == "__main__":
    main()
