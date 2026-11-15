"""Keep registered URI formats, independent mappings, ledgers and pinned cores coherent."""
import copy
import json
import pathlib
import re
import subprocess
import sys
import tempfile


def capture(pattern, body, label):
    found = re.search(pattern, body, re.MULTILINE)
    assert found, "missing identity: " + label
    return found.group(1)


def identities(repository, engine):
    read = lambda name: (repository / name).read_text(encoding="utf-8")
    cmake = read("cmake/singbox_bundle.cmake")
    gradle = read("platform/android/app/build.gradle")
    android = read("platform/android/build-singbox-libbox.sh")
    ios = read("platform/ios/build-singbox-libbox.sh")
    ci = read(".github/workflows/build.yml")
    version = subprocess.run([engine, "version"], check=True, capture_output=True, text=True, timeout=10).stdout
    return {
        "versions": [
            capture(r'set\(CLASHFLUX_SINGBOX_VERSION "([^"]+)"\)', cmake, "desktop"),
            capture(r'def singboxVersion = "([^"]+)"', gradle, "Android app"),
            capture(r'SINGBOX_VERSION:-([^}]+)', android, "Android script default"),
            capture(r'^sing-box version (\S+)', version, "actual bundled executable"),
        ],
        "revisions": [
            capture(r'def singboxRevision = "([^"]+)"', gradle, "Android app"),
            capture(r'SINGBOX_COMMIT:-([^}]+)', android, "Android script default"),
            capture(r'SINGBOX_COMMIT:-([^}]+)', ios, "iOS script default"),
            capture(r"SINGBOX_COMMIT: '([^']+)'", ci, "CI source"),
            capture(r'^Revision: (\S+)', version, "actual bundled executable"),
        ],
    }


def validate(catalog, cases, pins):
    assert catalog["schema_version"] == 1
    assert set(pins["versions"]) == {catalog["audited_core_version"]}, "audited core version differs from pinned/actual cores"
    assert set(pins["revisions"]) == {catalog["audited_core_revision"]}, "audited core revision differs from pinned/actual cores"
    formats = catalog["formats"]
    actual = {item["id"]: item for item in formats}
    assert len(actual) == len(formats), "duplicate format identity"
    assert set(actual) == {case["format"] for case in cases}, "registered formats and independent executable cases differ"
    for case in cases:
        item = actual[case["format"]]
        for key in ("protocol", "native_type", "schemes", "case_sensitive", "legacy"):
            assert item[key] == case[key], (case["id"], key)
        assert case["expected_node"]["type"] == item["native_type"], "manifest differs from independent native schema"
        assert item["schemes"] and len(set(item["schemes"])) == len(item["schemes"]), "invalid scheme aliases"


def main():
    compiler, engine, repository = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
    repository = pathlib.Path(repository)
    fixture = json.loads((repository / "tests/fixtures/node-input-catalog.json").read_text(encoding="utf-8"))
    assert fixture["schema_version"] == 1
    cases = fixture["cases"]
    pins = identities(repository, engine)
    with tempfile.TemporaryDirectory(prefix="clash-flux-node-input-") as temp:
        root = pathlib.Path(temp)
        result = subprocess.run([compiler, "--node-input-fixtures", str(root)], check=True, capture_output=True, text=True, timeout=30)
        catalog = json.loads((root / "catalog.json").read_text(encoding="utf-8"))
        validate(catalog, cases, pins)
        for fixture_case in cases:
            config = root / ("input-" + fixture_case["id"] + ".json")
            assert config.is_file(), "missing executable mapping fixture"
        native = sorted(root.glob("input-*.json")) + sorted(root.glob("legacy-ss-*.json"))
        assert native, "no fixed-core checks exported"
        for config in native:
            directory = root / ("check-" + config.stem)
            directory.mkdir()
            subprocess.run([engine, "check", "-c", str(config), "-D", str(directory)], check=True, capture_output=True, timeout=10)
        mutations = [
            lambda data: data.update(audited_core_version="0.0.0"),
            lambda data: data.update(audited_core_revision="0" * 40),
            lambda data: data["formats"].pop(),
            lambda data: data["formats"].append(copy.deepcopy(data["formats"][0])),
            lambda data: data["formats"][0].update(native_type="unsupported"),
            lambda data: data["formats"][0].update(schemes=[]),
            lambda data: data["formats"][0].update(case_sensitive=True),
            lambda data: data["formats"][1].update(legacy=False),
        ]
        for mutate in mutations:
            changed = copy.deepcopy(catalog)
            mutate(changed)
            try:
                validate(changed, cases, pins)
            except AssertionError:
                continue
            raise AssertionError("catalog drift was not rejected")
        print(result.stdout.strip())
        print(f"node_input_catalog: pinned/actual core identities, {len(catalog['formats'])} formats, {len(native)} native checks, {len(mutations)} drift rejections passed")


if __name__ == "__main__":
    main()
