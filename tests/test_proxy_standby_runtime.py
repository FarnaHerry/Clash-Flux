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


def request(port, path, *, control=False, method="GET", body=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        headers = {"Authorization": "Bearer test-native-secret"} if control else {}
        connection.request(method, path, body=body, headers=headers)
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


def run_stage(binary, root, config, controller, proxy, native_port, target, after_ready=None):
    document = json.loads(config.read_text(encoding="utf-8"))
    listening = bool(document["inbounds"])
    assert not any(item["type"] == "tun" for item in document["inbounds"])
    document["outbounds"] = [item for item in document["outbounds"] if item.get("tag") != "LOOPBACK"]
    document["outbounds"].append(
        {"type": "urltest", "tag": "LOOPBACK", "outbounds": ["DIRECT"], "url": target})
    config.write_text(json.dumps(document), encoding="utf-8")
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
            status, body = request(controller, "/group/LOOPBACK/delay?" + urllib.parse.urlencode(
                {"timeout": 2000}), control=True)
            assert status == 200 and isinstance(json.loads(body).get("DIRECT"), int), (
                config.name, body, (root / "core.log").read_text(errors="replace"))
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
            if after_ready:
                after_ready()
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def main():
    generator, binary, driver = sys.argv[1:]
    ports = set()
    while len(ports) < 3:
        ports.add(free_port())
    controller, proxy, native_port = ports

    def actual_mode():
        status, body = request(controller, "/configs", control=True)
        assert status == 200, body
        return json.loads(body)["mode"]

    def patch_mode(mode):
        status, body = request(controller, "/configs", control=True, method="PATCH",
                               body=json.dumps({"mode": mode}))
        assert 200 <= status < 300, body
        assert actual_mode() == mode

    def reconcile_cached_direct():
        assert actual_mode() == "direct", "sing-box cache did not restore the previous mode"
        result = subprocess.run(
            [driver, "--reconcile-mode", f"http://127.0.0.1:{controller}",
             "test-native-secret", "rule"], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
        assert actual_mode() == "rule", "PATCH was not confirmed through GET /configs"

    with tempfile.TemporaryDirectory(prefix="clash-flux-proxy-runtime-") as directory:
        root = pathlib.Path(directory)
        subprocess.run([generator, "--proxy-runtime-fixtures", str(root), str(controller), str(proxy),
                        str(native_port)], check=True)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Target) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                target = f"http://127.0.0.1:{server.server_port}/generate_204"
                direct_default = root / "direct-default.json"
                document = json.loads((root / "connected.json").read_text(encoding="utf-8"))
                document["experimental"]["clash_api"]["default_mode"] = "direct"
                direct_default.write_text(json.dumps(document), encoding="utf-8")
                for prefix in ("", "native-"):
                    for state in ("standby", "connected", "standby"):
                        run_stage(binary, root, root / f"{prefix}{state}.json", controller, proxy, native_port, target)
                run_stage(binary, root, root / "connected.json", controller, proxy, native_port, target,
                          after_ready=lambda: patch_mode("direct"))
                run_stage(binary, root, root / "connected.json", controller, proxy, native_port, target,
                          after_ready=reconcile_cached_direct)
                run_stage(binary, root, direct_default, controller, proxy, native_port, target,
                          after_ready=lambda: patch_mode("rule"))
            finally:
                server.shutdown()
                worker.join(timeout=5)
    print("YAML/native standby/listening and cached-mode reconciliation with GET verification passed")


if __name__ == "__main__":
    main()
