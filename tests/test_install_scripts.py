"""Regression checks for install.sh, install.ps1, and release packaging."""
import hashlib
import os
import pathlib
import platform
import re
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

    # install.sh must stay sudo-free and not invoke system package managers.
    assert not re.search(
        r"(?:^|[;&|\s(])(sudo|doas|apt-get|dpkg|dnf|rpm)(?:\s|$)",
        sh_text,
        re.MULTILINE,
    ), "install.sh must not invoke privilege escalation or system package managers"

    # CI publishes both scripts and SHA256SUMS.txt alongside release binaries.
    for token in ("install.sh", "install.ps1", "SHA256SUMS.txt"):
        assert token in workflow, f"workflow must package {token}"
    assert "--exclude='*.deb'" in workflow and "--exclude='*.rpm'" in workflow, (
        "workflow must exclude .deb and .rpm packages from the Linux .tar.gz release archive"
    )

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
    assert res.returncode == 0
    assert "--prefix" in res.stdout and "--uninstall" in res.stdout
    assert "--format" not in res.stdout and "--portable" not in res.stdout

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        home = tmp_path / "home"
        home.mkdir()
        tools_bin = tmp_path / "tools-bin"
        tools_bin.mkdir()
        for tool in (
            "sh", "uname", "id", "awk", "tr", "sha256sum", "shasum", "openssl",
            "dirname", "basename", "mkdir", "rm", "mv", "cp",
            "ln", "chmod", "mktemp", "readlink", "cat", "tar",
            "gzip", "rmdir", "curl",
        ):
            resolved = shutil.which(tool)
            if resolved:
                (tools_bin / tool).symlink_to(resolved)
        fake_sys_bin = tmp_path / "sys-bin"
        fake_sys_bin.mkdir()
        fake_sys_clash = fake_sys_bin / "clash-flux"
        fake_sys_clash.write_text("#!/bin/sh\necho 'Clash-Flux system'\n", encoding="utf-8")
        fake_sys_clash.chmod(0o755)
        base_env = dict(
            os.environ,
            HOME=str(home),
            XDG_DATA_HOME=str(home / ".local/share"),
            PATH=str(tools_bin),
        )

        # Reject unsafe prefixes.
        for bad_prefix in ("/", str(home), "/tmp/../etc/clash"):
            bad = run(["sh", str(sh_script), "--prefix", bad_prefix, "--uninstall", "--yes"], env=base_env, check=False)
            assert bad.returncode != 0, f"expected unsafe prefix {bad_prefix} to be rejected"

        archive_1 = tmp_path / f"clash-flux-0.3.139-linux-{arch}.tar.gz"
        digest_1 = build_synthetic_tar(archive_1, "0.3.139", f"build-linux-{arch}")
        sums_file = tmp_path / "SHA256SUMS.txt"
        sums_file.write_text(f"{digest_1}  {archive_1.name}\n", encoding="utf-8")

        # Reject mismatched SHA-256 without creating prefix.
        wrong_hash = "0" * 64
        bad_sha = run(
            ["sh", str(sh_script), "--archive", str(archive_1), "--sha256", wrong_hash, "--yes"],
            env=base_env,
            check=False,
        )
        assert bad_sha.returncode != 0
        assert not (home / ".local/share/Clash-Flux").exists()

        # 1. Missing from PATH -> emits clear PATH hint without failing.
        ok_1 = run(
            ["sh", str(sh_script), "--archive", str(archive_1), "--yes"],
            env=base_env,
            check=False,
        )
        assert ok_1.returncode == 0, ok_1.stdout + "\n" + ok_1.stderr
        assert "is not in your PATH" in ok_1.stdout

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
        assert run([str(symlink)], env=base_env).stdout.strip() == "Clash-Flux v0.3.139"

        # Place user database sentinel and verify upgrade + uninstall never touch user data.
        user_data_dir = home / ".local/share/clash-flux"
        user_data_dir.mkdir(parents=True)
        user_db = user_data_dir / "clash-flux.db"
        user_db.write_bytes(b"sqlite-user-profiles")

        archive_2 = tmp_path / f"clash-flux-0.3.140-linux-{arch}.tar.gz"
        digest_2 = build_synthetic_tar(archive_2, "0.3.140", f"build-linux-{arch}")
        sums_file.write_text(f"{digest_2}  {archive_2.name}\n", encoding="utf-8")

        # 2. Shadowed by an earlier system binary in PATH -> emits shadowing warning.
        shadowed_env = dict(base_env, PATH=f"{fake_sys_bin}:{home / '.local/bin'}:{tools_bin}")
        ok_2 = run(
            ["sh", str(sh_script), "--archive", str(archive_2), "--yes"],
            env=shadowed_env,
            check=False,
        )
        assert ok_2.returncode == 0, ok_2.stderr
        assert "shadowed" in ok_2.stdout and str(fake_sys_clash) in ok_2.stdout
        assert run([str(symlink)], env=shadowed_env).stdout.strip() == "Clash-Flux v0.3.140"
        assert user_db.read_bytes() == b"sqlite-user-profiles"

        # 3. User bin precedes system bin in PATH -> no missing-PATH hint and no shadow warning.
        clean_env = dict(base_env, PATH=f"{home / '.local/bin'}:{fake_sys_bin}:{tools_bin}")
        ok_3 = run(
            ["sh", str(sh_script), "--archive", str(archive_2), "--yes"],
            env=clean_env,
            check=False,
        )
        assert ok_3.returncode == 0, ok_3.stderr
        assert "is not in your PATH" not in ok_3.stdout
        assert "shadowed" not in ok_3.stdout

        un = run(
            ["sh", str(sh_script), "--uninstall", "--yes"],
            env=clean_env,
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
