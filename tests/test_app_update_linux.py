"""Verify the real Linux helper without requesting privileges or installing."""
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile
import time


def main():
    helper = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="clash-flux-update-linux-") as temp:
        root = pathlib.Path(temp)
        package = root / "package.deb"
        package.write_bytes(b"abc")
        digest = hashlib.sha256(b"abc").hexdigest()
        command = [helper, "--apply", str(os.getpid()), str(package), digest, "3", "deb", sys.executable]
        bad = command.copy()
        bad[4] = "0" * 64
        rejected = subprocess.run(bad, capture_output=True, text=True, timeout=10)
        assert rejected.returncode == 1 and not (root / "ready.txt").exists()
        assert "SHA-256 mismatch" in (root / "result.txt").read_text()
        child = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10
            while not (root / "ready.txt").exists() and time.monotonic() < deadline:
                assert child.poll() is None, child.stderr.read()
                time.sleep(0.02)
            assert (root / "ready.txt").read_text().strip() == "ready"
            assert child.poll() is None, "helper must wait for its live GUI parent"
            sealed = next(fd for fd in pathlib.Path(f"/proc/{child.pid}/fd").iterdir()
                          if "memfd:clash-flux-update" in os.readlink(fd))
            assert sealed.read_bytes() == b"abc"
            package.write_bytes(b"untrusted replacement")
            assert sealed.read_bytes() == b"abc", "installer must consume the verified immutable snapshot"
            try:
                with sealed.open("r+b") as output:
                    output.write(b"x")
            except PermissionError:
                pass
            else:
                raise AssertionError("snapshot must reject writes even from the owning user")
        finally:
            child.terminate()
            child.wait(timeout=5)
    print("app_update_linux: checksum rejection, parent-exit barrier and sealed package passed")


if __name__ == "__main__":
    main()
