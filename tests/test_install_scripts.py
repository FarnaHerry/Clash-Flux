"""Regression checks for install.sh, install.ps1, Linux repo signing, and release packaging."""
import os
import pathlib
import platform
import re
import shutil
import stat
import subprocess
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
    repo_script = root / "scripts/build-linux-repo.sh"
    pub_key = root / "platform/linux/clash-flux-repo.asc"
    workflow = (root / ".github/workflows/build.yml").read_text(encoding="utf-8")

    assert sh_script.is_file(), "install.sh must exist at repository root"
    assert ps_script.is_file(), "install.ps1 must exist at repository root"
    assert repo_script.is_file(), "scripts/build-linux-repo.sh must exist"
    assert pub_key.is_file(), "platform/linux/clash-flux-repo.asc must exist"

    sh_text = sh_script.read_text(encoding="utf-8")
    ps_text = ps_script.read_text(encoding="utf-8")
    repo_text = repo_script.read_text(encoding="utf-8")

    # Never disable TLS or package signature verification.
    assert "--insecure" not in sh_text and " -k " not in sh_text
    assert "--allow-unsigned-rpm" not in sh_text and "trusted=yes" not in sh_text
    assert "SkipCertificateCheck" not in ps_text

    # Pinned GPG fingerprint must match across install.sh and CI workflow.
    wf_fpr = re.search(r"LINUX_GPG_FINGERPRINT:\s*'([0-9A-F]{40})'", workflow)
    sh_fpr = re.search(r'expected_gpg_fingerprint="([0-9A-F]{40})"', sh_text)
    assert wf_fpr and sh_fpr, "both workflow and install.sh must pin the 40-char GPG fingerprint"
    assert wf_fpr.group(1) == sh_fpr.group(1), "workflow and install.sh GPG fingerprints must match"

    # Enforce signed APT/RPM configuration in install.sh.
    for token in ("Signed-By: /usr/share/keyrings/clash-flux.gpg", "gpgcheck=1", "repo_gpgcheck=1"):
        assert token in sh_text, f"install.sh missing required signature directive: {token}"

    # CI publishes install scripts, SHA256SUMS.txt, clean Linux tar.gz, and GitHub Pages APT/RPM repos.
    for token in (
        "install.sh",
        "install.ps1",
        "SHA256SUMS.txt",
        "--exclude='*.deb'",
        "--exclude='*.rpm'",
        "scripts/build-linux-repo.sh",
        "CLASHFLUX_LINUX_GPG_PRIVATE_KEY",
        "actions/upload-pages-artifact",
        "actions/deploy-pages",
    ):
        assert token in workflow, f"workflow missing expected release step/token: {token}"

    for token in ("rpmsign --addsign", "createrepo_c", "dpkg-scanpackages", "apt-ftparchive", "--clearsign"):
        assert token in repo_text, f"scripts/build-linux-repo.sh missing expected step: {token}"

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
    if shutil.which("bash") and os.name != "nt":
        res = run(["bash", "-n", str(repo_script)])
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


def make_stub(path: pathlib.Path, body: str) -> None:
    path.write_text("#!/bin/sh\nset -eu\n" + body + "\n", encoding="utf-8")
    path.chmod(0o755)


def populate_base_tools(tools_bin: pathlib.Path) -> None:
    tools_bin.mkdir()
    for tool in (
        "sh", "uname", "id", "awk", "tr", "sha256sum", "shasum", "openssl",
        "dirname", "basename", "mkdir", "rm", "mv", "cp", "ln", "chmod",
        "mktemp", "readlink", "cat", "tar", "gzip", "rmdir", "gpg",
    ):
        resolved = shutil.which(tool)
        if resolved:
            (tools_bin / tool).symlink_to(resolved)

    # Fake curl fetches files directly from CLASHFLUX_TEST_REPO_DIR by URL path suffix.
    make_stub(
        tools_bin / "curl",
        """
url=""
out=""
while [ "$#" -gt 0 ]; do
  case "$1" in
  -o) out="$2"; shift 2 ;;
  -w) shift 2 ;;
  -*) shift ;;
  *) url="$1"; shift ;;
  esac
done
rel="${url##*/}"
src="$CLASHFLUX_TEST_REPO_DIR/$rel"
[ -f "$src" ] || { echo "mock curl 404: $url" >&2; exit 22; }
cp "$src" "$out"
""",
    )
    make_stub(
        tools_bin / "sudo",
        """
if [ "${1:-}" = "-n" ] && [ "${2:-}" = "true" ]; then
  exit 0
fi
exec "$@"
""",
    )


def verify_unix_end_to_end(root: pathlib.Path) -> None:
    if os.name == "nt" or platform.system() != "Linux":
        return

    sh_script = root / "install.sh"
    res = run(["sh", str(sh_script), "--help"])
    assert res.returncode == 0 and "--uninstall" in res.stdout
    assert "--format" not in res.stdout and "--portable" not in res.stdout

    pub_asc = (root / "platform/linux/clash-flux-repo.asc").read_bytes()
    if not shutil.which("gpg"):
        return

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        home = tmp_path / "home"
        home.mkdir()
        repo_dir = tmp_path / "pages"
        repo_dir.mkdir()
        (repo_dir / "gpg.key").write_bytes(pub_asc)

        # Dearmor public key into binary keyring matching CI output.
        dearmor = subprocess.run(
            ["gpg", "--dearmor"],
            input=pub_asc,
            capture_output=True,
            check=True,
        )
        (repo_dir / "clash-flux.gpg").write_bytes(dearmor.stdout)

        base_tools = tmp_path / "base-tools"
        populate_base_tools(base_tools)

        # 1. Tampered GPG key must be rejected before writing any APT sources or keyring.
        apt_tools = tmp_path / "apt-tools"
        apt_tools.mkdir()
        log_file = tmp_path / "commands.log"
        make_stub(apt_tools / "dpkg", "exit 0")
        make_stub(apt_tools / "apt-get", 'printf "apt-get %s\\n" "$*" >>"$CLASHFLUX_CMD_LOG"')

        bad_repo = tmp_path / "bad-pages"
        bad_repo.mkdir()
        (bad_repo / "clash-flux.gpg").write_bytes(b"tampered-key")
        apt_root = tmp_path / "apt-root"
        apt_root.mkdir()
        (apt_root / "usr/bin").mkdir(parents=True)
        make_stub(apt_root / "usr/bin/clash-flux", 'echo "Clash-Flux apt"')

        bad_env = {
            "HOME": str(home),
            "PATH": f"{apt_tools}:{apt_root / 'usr/bin'}:{base_tools}",
            "CLASHFLUX_REPO_URL": "https://repo.invalid",
            "CLASHFLUX_SYSTEM_ROOT": str(apt_root),
            "CLASHFLUX_TEST_REPO_DIR": str(bad_repo),
            "CLASHFLUX_CMD_LOG": str(log_file),
        }
        bad = run(["sh", str(sh_script), "--yes"], env=bad_env, check=False)
        assert bad.returncode != 0
        assert not (apt_root / "etc/apt/sources.list.d/clash-flux.sources").exists()
        assert not (apt_root / "usr/share/keyrings/clash-flux.gpg").exists()

        # 2. Signed APT setup, install, and uninstall preserving user DB.
        user_db = home / ".local/share/clash-flux/clash-flux.db"
        user_db.parent.mkdir(parents=True)
        user_db.write_bytes(b"sqlite-user-profiles")

        apt_env = dict(bad_env, CLASHFLUX_TEST_REPO_DIR=str(repo_dir))
        ok_apt = run(["sh", str(sh_script), "--yes"], env=apt_env, check=False)
        assert ok_apt.returncode == 0, ok_apt.stdout + "\n" + ok_apt.stderr
        sources_file = apt_root / "etc/apt/sources.list.d/clash-flux.sources"
        keyring_file = apt_root / "usr/share/keyrings/clash-flux.gpg"
        assert sources_file.is_file() and keyring_file.is_file()
        sources_text = sources_file.read_text(encoding="utf-8")
        assert "URIs: https://repo.invalid/apt" in sources_text
        assert "Signed-By: /usr/share/keyrings/clash-flux.gpg" in sources_text
        cmds = log_file.read_text(encoding="utf-8")
        assert "apt-get update" in cmds and "apt-get install -y clash-flux" in cmds

        un_apt = run(["sh", str(sh_script), "--uninstall", "--yes"], env=apt_env, check=False)
        assert un_apt.returncode == 0, un_apt.stderr
        assert "apt-get remove -y clash-flux" in log_file.read_text(encoding="utf-8")
        assert not sources_file.exists() and not keyring_file.exists()
        assert user_db.read_bytes() == b"sqlite-user-profiles"

        # 3. Signed DNF/RPM setup + leftover user-local binary shadowing detection.
        rpm_tools = tmp_path / "rpm-tools"
        rpm_tools.mkdir()
        rpm_log = tmp_path / "rpm.log"
        make_stub(rpm_tools / "rpm", 'printf "rpm %s\\n" "$*" >>"$CLASHFLUX_CMD_LOG"')
        make_stub(rpm_tools / "dnf", 'printf "dnf %s\\n" "$*" >>"$CLASHFLUX_CMD_LOG"')

        rpm_root = tmp_path / "rpm-root"
        (rpm_root / "usr/bin").mkdir(parents=True)
        make_stub(rpm_root / "usr/bin/clash-flux", 'echo "Clash-Flux rpm"')

        shadow_dir = home / ".local/bin"
        shadow_dir.mkdir(parents=True)
        shadow_bin = shadow_dir / "clash-flux"
        make_stub(shadow_bin, 'echo "old local portable"')

        rpm_env = {
            "HOME": str(home),
            "PATH": f"{shadow_dir}:{rpm_tools}:{rpm_root / 'usr/bin'}:{base_tools}",
            "CLASHFLUX_REPO_URL": "https://repo.invalid",
            "CLASHFLUX_SYSTEM_ROOT": str(rpm_root),
            "CLASHFLUX_TEST_REPO_DIR": str(repo_dir),
            "CLASHFLUX_CMD_LOG": str(rpm_log),
        }
        ok_rpm = run(["sh", str(sh_script), "--yes"], env=rpm_env, check=False)
        assert ok_rpm.returncode == 0, ok_rpm.stdout + "\n" + ok_rpm.stderr
        repo_file = rpm_root / "etc/yum.repos.d/clash-flux.repo"
        assert repo_file.is_file()
        repo_text = repo_file.read_text(encoding="utf-8")
        assert "baseurl=https://repo.invalid/rpm/$basearch" in repo_text
        assert "gpgcheck=1" in repo_text and "repo_gpgcheck=1" in repo_text
        rpm_cmds = rpm_log.read_text(encoding="utf-8")
        assert "rpm --import " in rpm_cmds and "dnf -y --refresh install clash-flux" in rpm_cmds
        assert "shadowed" in ok_rpm.stdout and str(shadow_bin) in ok_rpm.stdout

        un_rpm = run(["sh", str(sh_script), "--uninstall", "--yes"], env=rpm_env, check=False)
        assert un_rpm.returncode == 0, un_rpm.stderr
        assert not repo_file.exists()
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
