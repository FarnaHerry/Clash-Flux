"""Missing or disabled safety tests must fail the production CMake gate."""
import pathlib
import subprocess
import sys
import tempfile

REQUIRED = ["smoke", "profile_links", "page_transition", "singbox",
            "rule_provider_download", "dns_hosts_runtime", "dns_policy_runtime",
            "dns_tls_runtime", "vpn", "routing", "compensation", "sqlite_orm",
            "persistence", "project_test_gate", "group_expansion_runtime", "service_protocol"]


def main():
    cmake, module, download = sys.argv[1:]
    module = pathlib.Path(module).resolve().as_posix()
    cases = 0
    with tempfile.TemporaryDirectory(prefix="clash-flux-test-gate-") as temp:
        root = pathlib.Path(temp)

        def configure(names, *, missing=None, disabled=None, msvc=False, windows=False):
            nonlocal cases
            folder = root / str(cases)
            folder.mkdir()
            lines = ["cmake_minimum_required(VERSION 3.30)",
                     "project(GateRegression LANGUAGES NONE)", "enable_testing()",
                     f'set(CLASHFLUX_MSVC_C4737_HARD_ERROR {"TRUE" if msvc else "FALSE"})']
            lines.append(f'set(WIN32 {"TRUE" if windows else "FALSE"})')
            # Upstream tests cannot substitute for a missing project regression.
            lines.append('add_test(NAME upstream_only COMMAND "${CMAKE_COMMAND}" -E true)')
            lines.extend(f'add_test(NAME {name} COMMAND "${{CMAKE_COMMAND}}" -E true)'
                         for name in names)
            if disabled:
                lines.append(f"set_tests_properties({disabled} PROPERTIES DISABLED TRUE)")
            lines += [f'include("{module}")', "clashflux_check_required_tests()"]
            (folder / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8")
            result = subprocess.run([cmake, "-S", str(folder), "-B", str(folder / "build"),
                                     "-G", "Ninja"], capture_output=True, text=True, timeout=10)
            output = " ".join((result.stdout + result.stderr).split())
            if missing or disabled:
                expected = f"{missing or disabled} ({'disabled' if disabled else 'not registered'})"
                assert result.returncode != 0 and expected in output, output
            else:
                assert result.returncode == 0, output
                assert f"all {len(names)} required project tests registered and enabled" in output, output
            cases += 1

        configure(REQUIRED)
        for name in REQUIRED:
            configure([other for other in REQUIRED if other != name], missing=name)
        for name in ("persistence", "dns_tls_runtime"):
            configure(REQUIRED, disabled=name)
        without_orm = [name for name in REQUIRED if name != "sqlite_orm"]
        configure(without_orm, msvc=True)
        configure([name for name in without_orm if name != "persistence"],
                  missing="persistence", msvc=True)
        windows_required = REQUIRED + ["windows_service"]
        configure(windows_required, windows=True)
        configure(REQUIRED, missing="windows_service", windows=True)
        configure(windows_required, disabled="windows_service", windows=True)
        configure([name for name in windows_required if name != "sqlite_orm"], msvc=True, windows=True)
        for openssl in ("", str(root / "missing-openssl")):
            result = subprocess.run([sys.executable, download, "must-not-run",
                                     "--require-tls", "--openssl", openssl],
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 2 and "OpenSSL CLI" in result.stderr, result.stderr
            cases += 1
    print(f"project_test_gate: {cases} registration/disabled/MSVC/TLS cases passed")


if __name__ == "__main__":
    main()
