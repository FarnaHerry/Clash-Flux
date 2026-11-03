"""Production resource writes and single-provider refresh in an isolated Runtime.

Uses a private database, source files and loopback HTTP; no service, TUN or system proxy.
"""
from collections import Counter
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

from mmdb_fixture import mmdb_file


def geo_varint(number):
    result = bytearray()
    while number > 127:
        result.append((number & 127) | 128)
        number >>= 7
    result.append(number)
    return bytes(result)


def geo_field(number, body):
    return geo_varint(number * 8 + 2) + geo_varint(len(body)) + body


def main():
    app = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="clashflux-resources-") as temporary:
        root = Path(temporary)
        env = {**os.environ, "APPDATA": temporary, "XDG_DATA_HOME": temporary, "HOME": temporary}
        data = root / "clash-flux"
        database = data / "clash-flux.db"

        def run(*args, expected=0):
            result = subprocess.run([app, *args], env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
            return result.stdout

        def setting(key):
            with closing(sqlite3.connect(database)) as db:
                row = db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
                return row[0] if row else ""

        def set_setting(key, value):
            with closing(sqlite3.connect(database)) as db, db:
                db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES (?,?)", (key, value))

        run("resource", "list")
        run("profile", "resources", "99999999", expected=1)
        fixture = root / "source.json"
        good = json.dumps({"version": 5, "rules": [{"domain_suffix": ["ads.test"]}]})
        fixture.write_text(good)
        run("resource", "add", "Ads", str(fixture))
        run("resource", "add", "Office", str(fixture))
        original = json.loads(setting("rules.resources"))
        first, second = original["resources"]
        first_id, second_id = first["id"], second["id"]
        run("resource", "rename", first_id, "Ads renamed")
        renamed = json.loads(setting("rules.resources"))
        assert renamed["resources"][0]["id"] == first_id
        assert renamed["resources"][1] == second
        before_failure = setting("rules.resources")
        fixture.write_text('{"version":5,"rules":[{"unknown_test_field":true}]}')
        run("resource", "add", "Invalid", str(fixture), expected=1)
        assert setting("rules.resources") == before_failure, "native candidate rejection must retain the library"
        assert fixture.read_text().find("unknown_test_field") > 0, "import must not modify its input"
        fixture.write_text(good)
        run("resource", "add", "Office", str(fixture), expected=1)
        run("resource", "remove", "missing-id", expected=1)
        assert setting("rules.resources") == before_failure

        # A disabled personal rule still protects its stable resource reference.
        run("profile", "list")
        profiles = data / "profiles"
        profiles.mkdir(exist_ok=True)
        plain = b"rules: ['MATCH,DIRECT']\n"
        with closing(sqlite3.connect(database)) as db, db:
            columns = db.execute("PRAGMA table_info(profiles)").fetchall()
            for identity in (1, 2):
                (profiles / f"{identity}.yaml").write_bytes(plain)
                row = {name: (0 if "INT" in kind.upper() else "")
                       for _, name, kind, required, default, primary in columns if required and default is None}
                row.update(id=identity, name=f"source-{identity}", file=f"{identity}.yaml", type="local",
                           selected=int(identity == 1))
                names = list(row)
                db.execute(f"INSERT INTO profiles ({','.join(names)}) VALUES ({','.join('?' for _ in names)})",
                           [row[name] for name in names])
        set_setting("profile.rules.2", json.dumps({"format_version": 1, "revision": 1, "rules": [{
            "id": "personal-ref", "match": "rule_set", "pattern": first_id,
            "target": "DIRECT", "enabled": False}]}))
        set_setting("vpn.global_policy", json.dumps({"format_version": 2, "revision": 1, "rules": [{
            "id": "global-ref", "match": "规则集", "pattern": first_id,
            "connection_id": "DIRECT", "enabled": False}]}))
        references = run("resource", "references", first_id)
        assert "personal-ref" in references and "global-ref" in references, references
        run("resource", "remove", first_id, expected=1)
        assert setting("rules.resources") == before_failure
        run("resource", "remove", second_id)
        assert [r["id"] for r in json.loads(setting("rules.resources"))["resources"]] == [first_id]

        requests = Counter()
        hold = threading.Event()
        reached = threading.Event()
        release = threading.Event()
        held_route = ["/rules-a"]
        bodies = {
            "/nodes-a": b"proxies: [{name: A, type: socks5, server: 127.0.0.1, port: 9}]\n",
            "/nodes-b": b"proxies: [{name: B, type: socks5, server: 127.0.0.1, port: 9}]\n",
            "/rules-a": b"payload: [a.test]\n", "/rules-b": b"payload: [b.test]\n",
        }

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests[self.path] += 1
                body = bodies[self.path]
                if hold.is_set() and self.path == held_route[0]:
                    reached.set()
                    assert release.wait(20)
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        address = f"http://127.0.0.1:{server.server_port}"
        content = ("proxy-providers:\n"
                   f"  A: {{type: http, url: '{address}/nodes-a', interval: 3600}}\n"
                   f"  B: {{type: http, url: '{address}/nodes-b', interval: 3600}}\n"
                   "proxy-groups:\n  - {name: Proxy, type: select, use: [A, B]}\n"
                   "rule-providers:\n"
                   f"  a: {{type: http, behavior: domain, url: '{address}/rules-a', interval: 3600}}\n"
                   f"  b: {{type: http, behavior: domain, url: '{address}/rules-b', interval: 3600}}\n"
                   "rules: ['RULE-SET,a,Proxy', 'RULE-SET,b,Proxy', 'MATCH,Proxy']\n").encode()
        bodies["/profile"] = content
        with closing(sqlite3.connect(database)) as db, db:
            db.execute("UPDATE profiles SET type='remote',url=? WHERE id=1", (address + "/profile",))
        try:
            run("profile", "update", "1")
            cache = list((data / "core").glob("*-providers/*.cache"))
            assert len(cache) == 4
            before = requests.copy()
            assert "attempted=0" in run("profile", "resources-update-due")
            assert requests == before
            for file in cache:
                os.utime(file, (time.time() - 7200,) * 2)
            summary = run("profile", "resources-update-due")
            assert "attempted=3 updated=3 applied=0" in summary, summary
            summary = run("profile", "resources-update-due")
            assert "attempted=1 updated=1 applied=0" in summary, summary
            assert requests - before == Counter({"/nodes-a": 1,"/nodes-b": 1,"/rules-a": 1,"/rules-b": 1})
            assert (profiles / "1.yaml").read_bytes() == content
            assert (profiles / "2.yaml").read_bytes() == plain
            for kind, name, route, body in (
                ("rule-provider", "a", "/rules-a", b"payload: [changed.test]\n"),
                ("proxy-provider", "A", "/nodes-a", b"proxies: [{name: A2, type: socks5, server: 127.0.0.1, port: 9}]\n"),
            ):
                for file in cache:
                    os.utime(file, (time.time() - 48 * 3600,) * 2)
                before = requests.copy()
                bodies[route] = body
                run("profile", "resource-refresh", "1", kind, name)
                assert requests - before == Counter({route: 1}), (before, requests)
                assert (profiles / "1.yaml").read_bytes() == content
                assert (profiles / "2.yaml").read_bytes() == plain
            # A declared but unused provider can be refreshed without changing
            # the source's groups or fetching unrelated expired resources.
            unused = content.replace(b"use: [A, B]", b"use: [A]")
            (profiles / "1.yaml").write_bytes(unused)
            before = requests.copy()
            run("profile", "resource-refresh", "1", "proxy-provider", "B")
            assert requests - before == Counter({"/nodes-b": 1})
            assert (profiles / "1.yaml").read_bytes() == unused
            (profiles / "1.yaml").write_bytes(content)
            previous_cache = {file: file.read_bytes() for file in cache}
            bodies["/rules-a"] = b"payload: [unterminated\n"
            run("profile", "resource-refresh", "1", "rule-provider", "a", expected=1)
            assert {file: file.read_bytes() for file in cache} == previous_cache
            assert (profiles / "1.yaml").read_bytes() == content
            # Editing the exact source while IO is in flight must reject the
            # late response before any candidate cache is committed.
            bodies["/rules-a"] = b"payload: [late.test]\n"
            hold.set()
            refreshing = subprocess.Popen([app, "profile", "resource-refresh", "1", "rule-provider", "a"],
                                          env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                assert reached.wait(10)
                edited = b"# edited during download\n" + content
                (profiles / "1.yaml").write_bytes(edited)
            finally:
                release.set()
                output, errors = refreshing.communicate(timeout=60)
            assert refreshing.returncode == 1, (output, errors)
            assert (profiles / "1.yaml").read_bytes() == edited
            assert {file: file.read_bytes() for file in cache} == previous_cache
            hold.clear()
            (profiles / "1.yaml").write_bytes(content)
            run("resource", "add-url", "Remote", "source", address + "/rules-native")
            remote = json.loads(setting("rules.resources"))["resources"][-1]
            before = requests.copy()
            receipt = run("resource", "refresh", remote["id"])
            assert "下次引用" in receipt and requests == before, "queued native refresh must not claim a download"
            assert json.loads(setting("rules.resources"))["resources"][-1]["refresh_generation"] == 1
            # Geo DAT uses the same production cache/guard/candidate path. It
            # must never silently substitute built-in categories or MMDB data.
            def site_file(domain, category=b"TEST"):
                return geo_field(1, geo_field(1, category) + geo_field(2, b"\x08\x02" + geo_field(2, domain)))
            # A database can exceed the ordinary provider's 8 MiB while the
            # referenced category stays small. Exercise real binary transport.
            padding = geo_field(2, b"\x08\x02" + geo_field(2, b"padding.example.test")) * 350000
            bodies["/site.dat"] = site_file(b"ads.test") + geo_field(1, geo_field(1, b"UNUSED") + padding)
            assert len(bodies["/site.dat"]) > 8 * 1024 * 1024
            bodies["/ip.dat"] = geo_field(1, geo_field(1, b"CN") +
                geo_field(2, geo_field(1, b"\xc0\x00\x02\x00") + b"\x10\x18"))
            geo = ("geodata-mode: true\ngeo-auto-update: true\ngeo-update-interval: 24\n"
                   f"geox-url: {{geosite: '{address}/site.dat', geoip: '{address}/ip.dat'}}\n"
                   "rules: ['GEOSITE,test,REJECT', 'GEOIP,CN,REJECT,no-resolve', 'MATCH,DIRECT']\n").encode()
            bodies["/profile"] = geo
            run("profile", "update", "1")
            assert (profiles / "1.yaml").read_bytes() == geo
            assert requests["/site.dat"] == 1 and requests["/ip.dat"] == 1
            before = requests.copy()
            statuses = run("profile", "resources", "1")
            assert "geo\tgeosite\tvalid\t" in statuses and "geo\tgeoip\tvalid\t" in statuses
            assert requests == before, "status reads must not fetch resources"
            all_cache = list((data / "core").glob("*-providers/*.cache"))
            assert len(all_cache) == 6
            geo_file = next(file for file in all_cache if b'"geo-dat-v1"' in file.read_bytes()[:1024] and b'"geosite"' in file.read_bytes()[:1024])
            image = geo_file.read_bytes()
            geo_file.write_bytes(b"broken cached image")
            assert "geo\tgeosite\tinvalid\t" in run("profile", "resources", "1")
            geo_file.write_bytes(image)
            for file in all_cache:
                os.utime(file, (time.time() - 48 * 3600,) * 2)
            before = requests.copy()
            bodies["/site.dat"] = site_file(b"fresh.test")
            run("profile", "resource-refresh", "1", "geo", "geosite")
            assert requests - before == Counter({"/site.dat": 1})
            saved_cache = {file: file.read_bytes() for file in all_cache}
            for broken in (b"<html>not geodata</html>", bodies["/site.dat"][:-1],
                           site_file(b"missing.test", b"OTHER")):
                bodies["/site.dat"] = broken
                run("profile", "resource-refresh", "1", "geo", "geosite", expected=1)
                assert {file: file.read_bytes() for file in all_cache} == saved_cache
                assert (profiles / "1.yaml").read_bytes() == geo
            bodies["/site.dat"] = site_file(b"late.test")
            held_route[0] = "/site.dat"
            reached.clear(); release.clear(); hold.set()
            pending = subprocess.Popen([app, "profile", "resource-refresh", "1", "geo", "geosite"],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                assert reached.wait(10)
                (profiles / "1.yaml").write_bytes(b"# changed Geo source\n" + geo)
            finally:
                release.set()
                output, errors = pending.communicate(timeout=60)
            assert pending.returncode == 1, (output, errors)
            assert {file: file.read_bytes() for file in all_cache} == saved_cache
            hold.clear()
            (profiles / "1.yaml").write_bytes(geo)
            bodies["/country.mmdb"] = b"not a database"
            bodies["/profile"] = (f"geox-url: {{mmdb: '{address}/country.mmdb'}}\n"
                "rules: ['GEOIP,CN,REJECT', 'MATCH,DIRECT']\n").encode()
            run("profile", "update", "1", expected=1)
            assert (profiles / "1.yaml").read_bytes() == geo
            assert {file: file.read_bytes() for file in all_cache} == saved_cache
            assert (profiles / "2.yaml").read_bytes() == plain
            unused_geo = (f"geox-url: {{geosite: '{address}/site.dat'}}\nrules: ['MATCH,DIRECT']\n").encode()
            (profiles / "2.yaml").write_bytes(unused_geo)
            assert "geo\tgeosite\tmissing\t" in run("profile", "resources", "2")
            before = requests.copy()
            run("profile", "resource-refresh", "2", "geo", "geosite")
            assert "geo\tgeosite\tvalid\t" in run("profile", "resources", "2")
            assert requests - before == Counter({"/site.dat": 1})
            assert (profiles / "2.yaml").read_bytes() == unused_geo
            assert (profiles / "1.yaml").read_bytes() == geo
            assert len(list((data / "core").glob("*-providers/*.cache"))) == 7, "source IDs must isolate Geo caches"
            (profiles / "2.yaml").write_bytes(plain)
            # Real MMDB downloads, state projection, native check, source isolation
            # and independent expiry with profile auto-update disabled.
            bodies["/country.mmdb"] = mmdb_file()
            bodies["/asn.mmdb"] = mmdb_file(asn=123)
            mmdb_source = ("geo-auto-update: true\ngeo-update-interval: 1\n"
                f"geox-url: {{mmdb: '{address}/country.mmdb', asn: '{address}/asn.mmdb'}}\n"
                "rules: ['GEOIP,CN,REJECT,no-resolve','IP-ASN,123,REJECT,no-resolve','MATCH,DIRECT']\n").encode()
            bodies["/profile"] = mmdb_source
            run("profile", "update", "1")
            assert "geo\tmmdb\tvalid\t" in run("profile", "resources", "1")
            assert "geo\tasn\tvalid\t" in run("profile", "resources", "1")
            mmdb_caches = [file for file in (data / "core").glob("*-providers/*.cache")
                           if b'"geo-mmdb-v1"' in file.read_bytes()[:1024]]
            assert len(mmdb_caches) == 2
            before = requests.copy()
            assert "attempted=0" in run("profile", "resources-update-due")
            assert requests == before, "fresh MMDB and disabled DAT must not download"
            for file in mmdb_caches:
                os.utime(file, (time.time() - 7200,) * 2)
            before = requests.copy()
            summary = run("profile", "resources-update-due")
            assert "attempted=2 updated=2 applied=0" in summary, summary
            assert requests - before == Counter({"/country.mmdb": 1, "/asn.mmdb": 1}), (before, requests)
            assert (profiles / "1.yaml").read_bytes() == mmdb_source
            assert (profiles / "2.yaml").read_bytes() == plain
            assert not (data / "core" / "config.json").exists(), "stopped scheduler must not launch or commit an active configuration"
            # Failed scheduled candidate retains every cache; deletion/retargeting
            # before the next pass removes that declaration from scheduling.
            old = {file: file.read_bytes() for file in mmdb_caches}
            bodies["/country.mmdb"] = mmdb_file("US")
            target = next(file for file in mmdb_caches if b'"mmdb"' in file.read_bytes()[:1024])
            os.utime(target, (time.time() - 7200,) * 2)
            old_stamp = target.stat().st_mtime_ns
            run("profile", "resources-update-due", expected=1)
            assert target.stat().st_mtime_ns == old_stamp, "failed scheduled refresh must retain the expiry timestamp"
            assert {file: file.read_bytes() for file in mmdb_caches} == old
            bodies["/country.mmdb"] = mmdb_file()
            (profiles / "1.yaml").write_bytes(plain)
            before = requests.copy()
            assert "attempted=0" in run("profile", "resources-update-due")
            assert requests == before
            assert not list((data / "core").glob("**/.stage-*"))
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)
        # Reopen once more; only explicitly removed resources/IDs disappeared.
        assert first_id in run("resource", "list") and second_id not in run("resource", "list")
        with closing(sqlite3.connect(database)) as db:
            assert db.execute("SELECT id FROM profiles ORDER BY id").fetchall() == [(1,), (2,)]
        print("resource_management_runtime: stable IDs, references, rejection rollback, scoped refresh and reopen passed")


if __name__ == "__main__":
    main()
