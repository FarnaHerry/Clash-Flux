"""Exercise production selection with isolated persistence and a stopped core."""
import argparse
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    args = parser.parse_args()
    app = args.app.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="clash-flux-selection-") as temporary:
        root = Path(temporary).resolve()
        assert root.parent == Path(tempfile.gettempdir()).resolve()
        env = {**os.environ, "APPDATA": str(root), "XDG_DATA_HOME": str(root)}
        data = root / "clash-flux"

        def run(*command, expected=0):
            result = subprocess.run([str(app), *command], env=env, capture_output=True, timeout=60)
            assert result.returncode == expected, (command, result.returncode, result.stderr.decode(errors="replace"))

        run("profile", "list")
        database = data / "clash-flux.db"
        assert database.is_file(), "runtime must initialize its isolated database"
        profiles = data / "profiles"
        profiles.mkdir(exist_ok=True)
        sources = {
            1: b"rules: ['MATCH,DIRECT']\n",
            2: b"proxies:\n  - {name: local, type: socks5, server: 127.0.0.1, port: 9}\n"
               b"proxy-groups:\n  - {name: select, type: select, proxies: [local, DIRECT]}\n"
               b"rules: ['MATCH,select']\n",
            3: b"proxies: [unterminated\n",
        }
        with closing(sqlite3.connect(database)) as db, db:
            columns = db.execute("PRAGMA table_info(profiles)").fetchall()
            for identity, content in sources.items():
                filename = f"selection-{identity}.yaml"
                (profiles / filename).write_bytes(content)
                row = {name: (0 if "INT" in kind.upper() else "")
                       for _, name, kind, required, default, primary in columns
                       if required and default is None}
                row.update(id=identity, name=f"selection-{identity}", file=filename,
                           type="local", selected=int(identity == 1))
                names = list(row)
                db.execute(f"INSERT INTO profiles ({','.join(names)}) VALUES ({','.join('?' for _ in names)})",
                           [row[name] for name in names])
        core = data / "core"
        core.mkdir(exist_ok=True)
        config = core / "config.json"
        original_config = b'{"outbounds":[{"type":"direct","tag":"preserve"}]}'
        config.write_bytes(original_config)
        log = core / "logs.log"

        def prepare_count():
            text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
            return text.count("profile-prepare total_ms=")

        before = prepare_count()
        run("profile", "use", "2")
        assert prepare_count() - before == 1, "one selection must prepare one complete candidate"
        with closing(sqlite3.connect(database)) as db, db:
            assert db.execute("SELECT id FROM profiles WHERE selected=1").fetchall() == [(2,)]
            assert db.execute("SELECT COUNT(*) FROM profiles").fetchone()[0] == len(sources)
        run("profile", "use", "3", expected=1)
        with closing(sqlite3.connect(database)) as db, db:
            assert db.execute("SELECT id FROM profiles WHERE selected=1").fetchall() == [(2,)]
            assert db.execute("SELECT COUNT(*) FROM profiles").fetchone()[0] == len(sources)
        assert config.read_bytes() == original_config, "stopped-core selection must preserve runtime config"
        for identity, content in sources.items():
            assert (profiles / f"selection-{identity}.yaml").read_bytes() == content
        latest = [b"# source-v2\nrules: ['MATCH,DIRECT']\n"]
        hold_response = threading.Event()
        request_started = threading.Event()
        release_response = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                body = latest[0]
                if hold_response.is_set():
                    request_started.set()
                    assert release_response.wait(30), "slow subscription fixture timed out"
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with closing(sqlite3.connect(database)) as db, db:
                db.execute("UPDATE profiles SET url=?, type='remote' WHERE id=2",
                           (f"http://127.0.0.1:{server.server_port}/profile",))
            # Update first, then select: queued selection resolves the ID's
            # current file instead of replaying its earlier content snapshot.
            run("profile", "use", "1")
            run("profile", "update", "2")
            run("profile", "use", "2")
            with closing(sqlite3.connect(database)) as db, db:
                filename, selected = db.execute("SELECT file,selected FROM profiles WHERE id=2").fetchone()
                assert filename == "selection-2.yaml" and selected == 1
                assert (profiles / filename).read_bytes() == latest[0]
            # Selection first, then update: the refreshed source keeps the
            # current selection and commits the newer bytes afterwards.
            run("profile", "use", "1")
            run("profile", "use", "2")
            latest[0] = b"# source-v3\nrules: ['MATCH,DIRECT']\n"
            run("profile", "update", "2")
            with closing(sqlite3.connect(database)) as db, db:
                filename, selected = db.execute("SELECT file,selected FROM profiles WHERE id=2").fetchone()
                assert filename == "selection-2.yaml" and selected == 1
                assert (profiles / filename).read_bytes() == latest[0]
                assert db.execute("SELECT COUNT(*) FROM profiles").fetchone()[0] == len(sources)
            assert config.read_bytes() == original_config
            previous_source = latest[0]
            latest[0] = b"proxies: [unterminated\n"
            run("profile", "update", "2", expected=1)
            assert (profiles / "selection-2.yaml").read_bytes() == previous_source
            with closing(sqlite3.connect(database)) as db:
                assert db.execute("SELECT id FROM profiles WHERE selected=1").fetchall() == [(2,)]
                assert db.execute("SELECT COUNT(*) FROM profiles").fetchone()[0] == len(sources)
            # A slow download must leave the lifecycle available to read/check
            # another source. Its late row save must preserve a newer selection.
            hold_response.set()
            latest[0] = b"# source-v4\nrules: ['MATCH,DIRECT']\n"
            updating = subprocess.Popen([str(app), "profile", "update", "2"], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                assert request_started.wait(10), "refresh never reached the slow HTTP response"
                selection = subprocess.run([str(app), "profile", "use", "1"], env=env,
                                           capture_output=True, timeout=10)
                assert selection.returncode == 0, selection.stderr.decode(errors="replace")
                assert updating.poll() is None, "selection must finish before the download is released"
            finally:
                release_response.set()
                output, errors = updating.communicate(timeout=60)
            assert updating.returncode == 0, errors.decode(errors="replace")
            with closing(sqlite3.connect(database)) as db:
                assert db.execute("SELECT id FROM profiles WHERE selected=1").fetchall() == [(1,)]
                assert db.execute("SELECT COUNT(*) FROM profiles").fetchone()[0] == len(sources)
                assert db.execute("SELECT file FROM profiles WHERE id=2").fetchone()[0] == "selection-2.yaml"
            assert (profiles / "selection-2.yaml").read_bytes() == latest[0]
            assert (profiles / "selection-1.yaml").read_bytes() == sources[1]
            assert config.read_bytes() == original_config
            for change in ("edit", "remove"):
                request_started.clear()
                release_response.clear()
                updating = subprocess.Popen([str(app), "profile", "update", "2"], env=env,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                edited = b"# edited while fetching\nrules: ['MATCH,DIRECT']\n"
                try:
                    assert request_started.wait(10)
                    if change == "edit":
                        temporary_source = profiles / "explicit-edit.yaml"
                        temporary_source.write_bytes(edited)
                        temporary_source.replace(profiles / "selection-2.yaml")
                    else:
                        removed = subprocess.run([str(app), "profile", "remove", "2"], env=env,
                                                 capture_output=True, timeout=10)
                        assert removed.returncode == 0, removed.stderr.decode(errors="replace")
                finally:
                    release_response.set()
                    output, errors = updating.communicate(timeout=60)
                assert updating.returncode == 1, (change, output, errors)
                with closing(sqlite3.connect(database)) as db:
                    assert db.execute("SELECT id FROM profiles WHERE selected=1").fetchall() == [(1,)]
                    expected_ids = [(1,), (2,), (3,)] if change == "edit" else [(1,), (3,)]
                    assert db.execute("SELECT id FROM profiles ORDER BY id").fetchall() == expected_ids
                if change == "edit":
                    assert (profiles / "selection-2.yaml").read_bytes() == edited
                assert (profiles / "selection-1.yaml").read_bytes() == sources[1]
                assert (profiles / "selection-3.yaml").read_bytes() == sources[3]
                assert config.read_bytes() == original_config
        finally:
            release_response.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        assert not list(core.glob("prepared-run-*")), "candidate staging must retire after success/failure"
        print("profile_selection_runtime: ok (one candidate; failures preserve data; slow refresh permits another selection)")


if __name__ == "__main__":
    main()
