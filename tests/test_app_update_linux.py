"""Exercise the production Linux installation flow with a build-only fake pkexec."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time


def wait_for(path, process):
    deadline = time.monotonic() + 10
    while not path.exists() and time.monotonic() < deadline:
        assert process.poll() is None, process.communicate()
        time.sleep(0.02)
    assert path.exists(), path


def main():
    production, fixture, driver = map(lambda p: str(pathlib.Path(p).resolve()), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="clash-flux-update-linux-") as temp:
        root = pathlib.Path(temp)
        package = root / "package.deb"
        package.write_bytes(b"abc")
        bad = subprocess.run([production, "--apply", str(os.getpid()), str(package), "0" * 64,
                              "3", "deb", sys.executable], capture_output=True, timeout=10)
        assert bad.returncode == 1 and not (root / "ready.txt").exists()
        assert "SHA-256 mismatch" in (root / "result.txt").read_text()
        helper_dir = root / "helper"
        helper_dir.mkdir()
        (helper_dir / "clash-flux-updater").symlink_to(fixture)
        for kind in ("deb", "rpm"):
            for code in (0, 126, 127, 1):
                case = root / f"{kind}-{code}"
                case.mkdir()
                env = dict(os.environ, CLASHFLUX_UPDATE_TEST_ROOT=str(case),
                           CLASHFLUX_UPDATE_TEST_RESULT=str(code))
                parent = subprocess.Popen([driver, "--linux-install", str(case), str(helper_dir), kind],
                                          env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    stage = pathlib.Path(parent.stdout.readline().strip())
                    wait_for(case / "started", parent)
                    assert parent.poll() is None, "GUI must remain alive through authorization and installation"
                    assert (case / "started").read_text() == (
                        "/usr/bin/dpkg --install" if kind == "deb" else "/usr/bin/rpm --upgrade")
                    if code == 0:
                        # The privileged installer must consume the retained sealed snapshot.
                        (stage / f"package.{kind}").write_bytes(b"untrusted replacement")
                    (case / "allow").touch()
                    output, error = parent.communicate(timeout=10)
                    assert parent.returncode == (1 if code else 0), (output, error)
                    if code:
                        assert "RETAINED:" in output and f"fixture authorization/package failure {code}" in output
                    else:
                        assert "INSTALLED" in output and (case / "installed").read_bytes() == b"abc"
                finally:
                    if parent.poll() is None:
                        parent.terminate()
                        parent.wait(timeout=5)
    print("app_update_linux: live parent, DEB/RPM, immutable snapshot, success/cancel/no-agent/package-error passed")


if __name__ == "__main__":
    main()
