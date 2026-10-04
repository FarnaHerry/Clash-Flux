"""Production expansion compiler, pinned kernel API and actual SOCKS forwarding."""
import json
import pathlib
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

sys.dont_write_bytecode = True
from test_dns_hosts_runtime import Node, port, request


class A(Node):
    requests = 0


class Z(Node):
    requests = 0


def main():
    compiler, engine, sample = sys.argv[1:]
    services = [socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler) for handler in (A, Z)]
    for service in services:
        service.daemon_threads = True
        threading.Thread(target=service.serve_forever, daemon=True).start()
    client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with tempfile.TemporaryDirectory(prefix="clash-flux-group-expansion-") as temp:
            folder = pathlib.Path(temp)
            source, generated = folder / "source.yaml", folder / "config.json"
            yaml = pathlib.Path(sample).read_text(encoding="utf-8")
            yaml = yaml.replace("port: 11081", f"port: {services[0].server_address[1]}")
            yaml = yaml.replace("port: 11082", f"port: {services[1].server_address[1]}")
            source.write_text(yaml, encoding="utf-8")
            subprocess.run([compiler, "--compile-config", str(source), str(generated)],
                           check=True, capture_output=True, timeout=10)
            config = json.loads(generated.read_text())
            tags = {out["server_port"]: out["tag"] for out in config["outbounds"] if out["type"] == "socks"}
            a, z = [tags[service.server_address[1]] for service in services]
            selected = config["route"]["final"]
            groups = {out["tag"]: out for out in config["outbounds"] if out["type"] == "selector"}
            all_tag = next(tag for tag, group in groups.items() if group["outbounds"] == [a, z])
            assert groups[selected]["outbounds"] == [z, all_tag, a, z]
            assert groups[selected]["default"] == a
            mixed, controller = port(), port()
            config["inbounds"] = [{"type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": mixed}]
            config["experimental"] = {"clash_api": {"external_controller": f"127.0.0.1:{controller}"}}
            config["log"] = {"level": "error"}
            generated.write_text(json.dumps(config))
            subprocess.run([engine, "check", "-c", str(generated)], check=True, capture_output=True, timeout=10)

            def api(tag, choice=None):
                url = f"http://127.0.0.1:{controller}/proxies/" + urllib.parse.quote(tag, safe="")
                data = None if choice is None else json.dumps({"name": choice}).encode()
                req = urllib.request.Request(url, data=data, method="GET" if data is None else "PUT",
                                             headers={"Content-Type": "application/json"})
                with client.open(req, timeout=0.5) as reply:
                    return json.load(reply) if choice is None else reply.status

            with (folder / "kernel.log").open("wb") as log:
                process = subprocess.Popen([engine, "run", "-c", str(generated)], cwd=folder, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 5
                    while True:
                        try:
                            state = api(selected)
                            break
                        except (OSError, TimeoutError):
                            if process.poll() is not None or time.monotonic() >= deadline:
                                raise RuntimeError((folder / "kernel.log").read_text())
                            time.sleep(0.02)
                    assert state["all"] == [z, all_tag, a, z] and state["now"] == a, state
                    assert api(all_tag)["all"] == [a, z]
                    count = 2

                    def forward(handler):
                        before = (A.requests, Z.requests)
                        assert request(mixed, "127.0.0.1", 80) == b"node-ok"
                        expected = (before[0] + int(handler is A), before[1] + int(handler is Z))
                        assert (A.requests, Z.requests) == expected

                    forward(A)  # The generated selector default applies on first launch.
                    count += 1
                    for choice, handler in [(z, Z), (all_tag, A), (a, A)]:
                        assert api(selected, choice) == 204
                        assert api(selected)["now"] == choice
                        forward(handler)
                        count += 1
                    assert api(selected, all_tag) == 204 and api(all_tag, z) == 204
                    forward(Z)  # Nested expanded selector selection reaches the actual node.
                    count += 1
                    before = generated.read_bytes()
                    source.write_text(yaml.replace("include-all-proxies: true}",
                                      "include-all-proxies: true, filter: unsupported}"), encoding="utf-8")
                    failed = subprocess.run([compiler, "--compile-config", str(source), str(generated)],
                                            capture_output=True, timeout=10)
                    assert failed.returncode != 0 and generated.read_bytes() == before
                    assert process.poll() is None
                    forward(Z)
                    count += 1
                    print(f"group_expansion_runtime: {count} cases passed (API membership/default, real node switching and rejected candidate)")
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
    finally:
        threads = [threading.Thread(target=service.shutdown) for service in services]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        for service in services:
            service.server_close()


if __name__ == "__main__":
    main()
