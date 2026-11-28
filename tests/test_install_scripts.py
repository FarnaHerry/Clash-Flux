"""Regression checks for install.sh, install.ps1, and release packaging."""
import hashlib
import os
import pathlib
import platform
import shutil
import stat
import subprocess
import tarfile
import tempfile


def run(cmd, env=None, check=True):
    return subprocess.run(
        cmd,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=check,
    )


def verify_static_contracts(root: pathlib.Path) -> None:
    sh_script = root / "install.sh"
    ps_script = root / "install.ps1"
    workflow = (root / ".github/workflows/build.yml").read_text(encoding="utf-8")

    assert sh_script.is_file(), "install.sh must exist at repository root"
    assert ps_script.is_file(), "install.ps1 must exist at repository root"

    sh_text = sh_script.read_text(encoding="utf-8")
    ps_text = ps_script.read_text(encoding="utf-8")

    # Never disable TLS verification.
    assert "--insecure" not in sh_text and " -k " not in sh_text
    assert "SkipCertificateCheck" not in ps_text

    # CI publishes both scripts and SHA256SUMS.txt alongside release binaries.
    for token in ("install.sh", "install.ps1", "SHA256SUMS.txt"):
        assert token in workflow, f"workflow must package {token}"

    # Verify PowerShell installer enforces SHA-256, Zip Slip protection, and Burn flags.
    for token in (
        "Verify-PackageSha256",
        "Get-FileHash -LiteralPath",
        "0xA000",
        "InstallFolder=",
        "CreateDesktopShortcut=1",
        "Assert-SafePrefix",
    ):
        assert token in ps_text, f"install.ps1 missing expected safeguard: {token}"

    if shutil.which("sh") and os.name != "nt":
        res = run(["sh", "-n", str(sh_script)])
        assert res.returncode == 0, res.stderr

    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if pwsh:
        parse_cmd = (
            "$errs = $null; [System.Management.Automation.Language.Parser]::ParseFile("
            f"'{ps_script.as_posix()}', [ref]$null, [ref]$errs); "
            "if ($errs.Count -gt 0) { $errs | ForEach-Object { Write-Error $_.ToString() }; exit 1 }"
        )
        res = run([pwsh, "-NoProfile", "-Command", parse_cmd])
        assert res.returncode == 0, res.stderr


def build_synthetic_tar(archive_path: pathlib.Path, version_label: str, top_dir: str) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        stage = pathlib.Path(tmp) / top_dir
        (stage / "assets").mkdir(parents=True)
        (stage / "engines").mkdir(parents=True)
        binary = stage / "clash-flux"
        binary.write_text(f"#!/bin/sh\necho 'Clash-Flux v{version_label}'\n", encoding="utf-8")
        binary.chmod(0o755)
        updater = stage / "clash-flux-updater"
        updater.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        updater.chmod(0o755)
        (stage / "engines/sing-box").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        (stage / "assets/icon.png").write_bytes(b"PNGDATA")
        # CI staging dir includes stray .deb/.rpm artifacts that portable installs strip.
        (stage / f"clash-flux_{version_label}_amd64.deb").write_bytes(b"deb")
        (stage / f"clash-flux-{version_label}-1.x86_64.rpm").write_bytes(b"rpm")

        with tarfile.open(archive_path, "w:gz") as tf:
            tf.add(stage, arcname=top_dir)
    return hashlib.sha256(archive_path.read_bytes()).hexdigest()


def verify_unix_end_to_end(root: pathlib.Path) -> None:
    if os.name == "nt" or platform.system() != "Linux":
        return

    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch = "x86_64"
    elif machine in ("aarch64", "arm64"):
        arch = "aarch64"
    else:
        return

    sh_script = root / "install.sh"
    res = run(["sh", str(sh_script), "--help"])
    assert res.returncode == 0 and "--portable" in res.stdout

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        home = tmp_path / "home"
        home.mkdir()
        env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / ".local/share"))

        # Reject unsafe prefixes.
        for bad_prefix in ("/", str(home), "/tmp/../etc/clash"):
            bad = run(["sh", str(sh_script), "--format", "tar", "--prefix", bad_prefix, "--uninstall", "--yes"], env=env, check=False)
            assert bad.returncode != 0, f"expected unsafe prefix {bad_prefix} to be rejected"

        archive_1 = tmp_path / f"clash-flux-0.3.139-linux-{arch}.tar.gz"
        digest_1 = build_synthetic_tar(archive_1, "0.3.139", f"build-linux-{arch}")
        sums_file = tmp_path / "SHA256SUMS.txt"
        sums_file.write_text(f"{digest_1}  {archive_1.name}\n", encoding="utf-8")

        # Reject mismatched SHA-256 without creating prefix.
        wrong_hash = "0" * 64
        bad_sha = run(
            ["sh", str(sh_script), "--format", "tar", "--archive", str(archive_1), "--sha256", wrong_hash, "--yes"],
            env=env,
            check=False,
        )
        assert bad_sha.returncode != 0
        assert not (home / ".local/share/Clash-Flux").exists()

        # Successful offline install using SHA256SUMS.txt.
        ok_1 = run(
            ["sh", str(sh_script), "--format", "tar", "--archive", str(archive_1), "--yes"],
            env=env,
            check=False,
        )
        assert ok_1.returncode == 0, ok_1.stdout + "\n" + ok_1.stderr

        install_dir = home / ".local/share/Clash-Flux"
        installed_bin = install_dir / "clash-flux"
        assert installed_bin.is_file() and (installed_bin.stat().st_mode & stat.S_IXUSR)
        assert not list(install_dir.glob("*.deb")) and not list(install_dir.glob("*.rpm"))

        symlink = home / ".local/bin/clash-flux"
        assert symlink.is_symlink() and pathlib.Path(os.readlink(symlink)) == installed_bin
        desktop_file = home / ".local/share/applications/clash-flux.desktop"
        assert desktop_file.is_file() and str(installed_bin) in desktop_file.read_text(encoding="utf-8")
        icon_file = home / ".local/share/icons/hicolor/256x256/apps/clash-flux.png"
        assert icon_file.read_bytes() == b"PNGDATA"
        assert run([str(symlink)], env=env).stdout.strip() == "Clash-Flux v0.3.139"

        # Place user database sentinel and verify upgrade + uninstall never touch user data.
        user_data_dir = home / ".local/share/clash-flux"
        user_data_dir.mkdir(parents=True)
        user_db = user_data_dir / "clash-flux.db"
        user_db.write_bytes(b"sqlite-user-profiles")

        archive_2 = tmp_path / f"clash-flux-0.3.140-linux-{arch}.tar.gz"
        digest_2 = build_synthetic_tar(archive_2, "0.3.140", f"build-linux-{arch}")
        sums_file.write_text(f"{digest_2}  {archive_2.name}\n", encoding="utf-8")

        ok_2 = run(
            ["sh", str(sh_script), "--format", "tar", "--archive", str(archive_2), "--yes"],
            env=env,
            check=False,
        )
        assert ok_2.returncode == 0, ok_2.stderr
        assert run([str(symlink)], env=env).stdout.strip() == "Clash-Flux v0.3.140"
        assert user_db.read_bytes() == b"sqlite-user-profiles"

        un = run(
            ["sh", str(sh_script), "--format", "tar", "--uninstall", "--yes"],
            env=env,
            check=False,
        )
        assert un.returncode == 0, un.stderr
        assert not install_dir.exists()
        assert not symlink.exists()
        assert not desktop_file.exists()
        assert user_db.read_bytes() == b"sqlite-user-profiles"


def main(argv: list[str]) -> None:
    import sys

    root = pathlib.Path(argv[1]).resolve() if len(argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
    verify_static_contracts(root)
    verify_unix_end_to_end(root)
    print("install_scripts passed")


if __name__ == "__main__":
    import sys

    main(sys.argv)
