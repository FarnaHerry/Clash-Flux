"""Compare source rebuild and fixture artifact loads in fresh Linux processes."""
import argparse
import csv
import json
import pathlib
import subprocess
import sys
import tempfile


def write_fixture(path, family, count):
    if family == "clash":
        with path.open("w", encoding="utf-8") as stream:
            stream.write(
                "rule-providers:\n"
                "  sample: {type: inline, behavior: domain, payload: [provider.test]}\n"
                "rules:\n"
                "  - RULE-SET,sample,DIRECT\n"
                "  - IP-CIDR,192.0.2.0/24,DIRECT,no-resolve\n"
                "  - UNKNOWN,omitted.test,DIRECT\n"
            )
            for i in range(count):
                stream.write(f"  - DOMAIN-SUFFIX,rule{i}.test,DIRECT\n")
            stream.write("  - MATCH,DIRECT\n  - DOMAIN,after-final.test,REJECT\n")
        return
    rules = [
        {"domain_suffix": [f"rule{i}.test"], "action": "route", "outbound": "DIRECT"}
        for i in range(count)
    ]
    rules.append({
        "future_match": {"uint": 18446744073709551615, "negative": -9223372036854775808, "float": 0.125},
        "action": "route",
        "outbound": "DIRECT",
    })
    document = {"outbounds": [{"type": "direct", "tag": "DIRECT"}],
                "route": {"rules": rules, "final": "DIRECT"}}
    path.write_text(json.dumps(document, separators=(",", ":")), encoding="utf-8")


def run_probe(exe, source, artifact, mode, rss_file):
    completed = subprocess.run(
        ["/usr/bin/time", "-f", "%M", "-o", rss_file, exe,
         "--target-restart-probe", mode, source, artifact],
        capture_output=True, text=True, check=False,
    )
    if completed.returncode:
        raise RuntimeError(f"{mode} exited {completed.returncode}\n{completed.stderr}")
    return {**json.loads(completed.stdout), "peak_rss_kib": int(rss_file.read_text())}


def verify_rejection(exe, root, source, artifact):
    original = artifact.read_bytes()
    invalid = root / "invalid"
    changed_source = root / "changed-source"
    changed_source.write_bytes(source.read_bytes() + b"\n")
    for label, data, candidate in (
        ("unknown version", original.replace(b"target-r5-probe-2", b"target-r5-probe-3", 1), source),
        ("mismatched source", original, changed_source),
        ("truncated body", original[:-1], source),
    ):
        invalid.write_bytes(data)
        result = subprocess.run(
            [exe, "--target-restart-probe", "load-cbor", candidate, invalid],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 1 or result.stdout:
            raise RuntimeError(f"Probe did not reject {label}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_singbox", type=pathlib.Path)
    parser.add_argument("--temp-root", type=pathlib.Path, help="filesystem for temporary fixtures")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--rows", type=int, nargs="+", default=(1000, 10000, 50000))
    args = parser.parse_args()
    if args.samples < 1 or any(not 1 <= count <= 100000 for count in args.rows):
        parser.error("use at least one sample and row counts between 1 and 100000")
    exe = args.test_singbox.resolve(strict=True)
    fields = ["family", "input_rows", "sample", "mode", "source_bytes", "artifact_bytes",
              "source_read_ms", "artifact_read_ms", "hash_check_ms", "compile_capture_ms",
              "codec_decode_ms", "model_restore_ms", "project_ms", "ready_ms",
              "encode_hash_ms", "write_ms", "peak_rss_kib", "view_rows", "model_billed_bytes"]
    writer = csv.DictWriter(sys.stdout, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    with tempfile.TemporaryDirectory(prefix="clashflux-r5-", dir=args.temp_root) as temporary:
        root = pathlib.Path(temporary)
        source, rss_file = root / "source", root / "rss"
        artifacts = {codec: root / codec for codec in ("json", "cbor")}
        for family in ("clash", "native"):
            for count in args.rows:
                write_fixture(source, family, count)
                for codec, artifact in artifacts.items():
                    run_probe(exe, source, artifact, f"write-{codec}", rss_file)
                    run_probe(exe, source, artifact, f"verify-{codec}", rss_file)
                if count == args.rows[0]:
                    verify_rejection(exe, root, source, artifacts["cbor"])
                modes = ["rebuild", "load-json", "load-cbor"]
                for sample in range(args.samples):
                    offset = sample % len(modes)
                    for mode in modes[offset:] + modes[:offset] + ["write-json", "write-cbor"]:
                        codec = mode.rpartition("-")[2]
                        artifact = artifacts.get(codec, root / "unused")
                        stats = run_probe(exe, source, artifact, mode, rss_file)
                        writer.writerow(dict(family=family, input_rows=count, sample=sample + 1, mode=mode, **stats))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
