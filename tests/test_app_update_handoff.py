"""Run the production Windows handoff with a harmless fake installer."""
import hashlib
import pathlib
import subprocess
import sys
import tempfile
import time


def main():
    test, helper, fake = map(pathlib.Path, sys.argv[1:])
    content = fake.read_bytes()
    digest = hashlib.sha256(content).hexdigest()
    with tempfile.TemporaryDirectory(prefix="clash-flux-update-handoff-") as temp:
        parent = subprocess.Popen([str(test), "--handoff", temp, str(helper.parent),
                                   str(fake), str(len(content)), digest],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = parent.stdout.readline().strip()
            assert line, parent.stderr.read()
            stage = pathlib.Path(line)
            assert parent.poll() is None and not (stage / "installed.txt").exists(), \
                "installer must wait until the GUI process exits"
            try:
                with (stage / "package.exe").open("r+b") as output:
                    output.write(b"tampering")
            except PermissionError:
                pass
            else:
                raise AssertionError("helper must retain a read-only package lock through handoff")
            assert parent.wait(timeout=15) == 0, parent.stderr.read()
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                result = stage / "result.txt"
                if result.exists() and result.read_text().strip() == "Installation completed" \
                        and not (stage / "ready.txt").exists():
                    break
                time.sleep(0.05)
            assert (stage / "installed.txt").read_text() == "parent-exited"
            assert not (stage / "package.exe").exists(), "successful handoff cleans its package"
            # The statically linked helper can take a moment to release its own image.
            for attempt in range(100):
                try:
                    (stage / "updater.exe").unlink()
                    break
                except PermissionError:
                    time.sleep(0.05)
        finally:
            if parent.poll() is None:
                parent.terminate()
                parent.wait(timeout=10)
    print("app_update_handoff: parent-exit barrier, immutable package and native launch passed")


if __name__ == "__main__":
    main()
