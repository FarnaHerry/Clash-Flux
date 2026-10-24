"""Linux native tray regression using a private watcher and disposable app data.

Run: dbus-run-session -- /usr/bin/python3 tests/test_tray_runtime.py build/clash-flux
Needs a graphical session, PyGObject/Gio and bubblewrap. The sandbox hides the
installed service and makes the host filesystem read-only except the fixture.
This is an opt-in native check, separate from the display-independent CTest gate.
"""
import argparse
import os
import pathlib
import sqlite3
import subprocess
import tempfile
import threading
import time

from gi.repository import Gio, GLib


WATCHER = "org.kde.StatusNotifierWatcher"
WATCHER_PATH = "/StatusNotifierWatcher"
MENU_PATH = "/StatusNotifierItem/Menu"
MENU = "com.canonical.dbusmenu"
XML = """<node><interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
</interface></node>"""


class Fixture:
    def __init__(self, binary, folder):
        self.binary = binary
        self.folder = folder
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.owner = None
        self.host = True
        self.process = None
        self.env = dict(os.environ, XDG_DATA_HOME=str(folder), TMPDIR=str(folder), GDK_BACKEND="x11",
                        INTEL_FORCE_PROBE="1", GTK_A11Y="none", NO_AT_BRIDGE="1")
        # Refuse the real session bus: watcher restarts must never affect Plasma.
        address = self.env.get("DBUS_SESSION_BUS_ADDRESS", "")
        assert address.startswith(("unix:path=/tmp/dbus-", "unix:abstract=/tmp/dbus-")), \
            "run inside dbus-run-session"
        names = self.call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "ListNames").unpack()[0]
        assert WATCHER not in names, "fixture requires a private, unused watcher"
        self.node = Gio.DBusNodeInfo.new_for_xml(XML)
        self.registration = self.bus.register_object(
            WATCHER_PATH, self.node.interfaces[0], self.register_item,
            lambda *_: GLib.Variant("b", self.host), None)
        result = self.call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "RequestName",
                           GLib.Variant("(su)", (WATCHER, 4))).unpack()[0]
        assert result == 1, "could not own fixture watcher"
        # Serve watcher calls while the main thread queries the application's
        # menu: the native adapter synchronously registers after reading a property.
        self.loop = GLib.MainLoop()
        self.thread = threading.Thread(target=self.loop.run, daemon=True)
        self.thread.start()

    def call(self, dest, path, interface, method, args=None):
        return self.bus.call_sync(dest, path, interface, method, args, None,
                                  Gio.DBusCallFlags.NONE, 3000, None)

    def register_item(self, connection, sender, path, interface, method, args, invocation):
        self.owner = sender
        invocation.return_value(None)

    def wait(self, predicate, message, timeout=15):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if predicate():
                return
            if self.process is not None:
                assert self.process.poll() is None, "fixture app exited early"
            time.sleep(.02)
        raise AssertionError(message)

    def menu(self):
        if self.owner is None:
            return []
        return self.call(self.owner, MENU_PATH, MENU, "GetLayout",
                         GLib.Variant("(iias)", (0, -1, []))).unpack()[1][2]

    def sandbox(self):
        return ["bwrap", "--die-with-parent", "--ro-bind", "/", "/",
                "--dev-bind", "/dev", "/dev",
                "--proc", "/proc",
                "--bind", str(self.folder), str(self.folder),
                "--tmpfs", "/run/clash-flux", "--tmpfs", "/etc/clash-flux",
                "--", str(self.binary)]

    def start(self, root_count):
        self.owner = None
        self.log = open(self.folder / "gui.log", "w+")
        self.process = subprocess.Popen(self.sandbox(), env=self.env,
                                        stdout=self.log, stderr=self.log)
        self.wait(lambda: len(self.menu()) == root_count, "initial tray menu missing")
        # Finish hydrate/maintenance before checking an otherwise identical menu.
        self.wait(lambda: self.menu()[4 if root_count == 10 else 2][1].get("enabled", True),
                  "startup maintenance never finished")

    def click(self, item):
        self.call(self.owner, MENU_PATH, MENU, "Event",
                  GLib.Variant("(isvu)", (item[0], "clicked", GLib.Variant("i", 0), 0)))

    def set_host(self, available):
        self.host = available
        self.bus.emit_signal(None, WATCHER_PATH, "org.freedesktop.DBus.Properties",
                             "PropertiesChanged", GLib.Variant("(sa{sv}as)",
                             (WATCHER, {"IsStatusNotifierHostRegistered":
                              GLib.Variant("b", available)}, [])))

    def recover_host(self, root_count):
        before = self.menu()
        self.set_host(False)
        self.wait(lambda: not self.menu(), "unavailable host did not clear tray")
        self.set_host(True)
        self.wait(lambda: len(self.menu()) == root_count, "tray not republished on host return")
        assert self.menu() == before, "host return changed labels, checkmarks or actions"
        print(f"PASS: {root_count}-item menu restored after host loss", flush=True)

    def restart_watcher(self):
        before = self.menu()
        self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                  "ReleaseName", GLib.Variant("(s)", (WATCHER,)))

        def unexported():
            try:
                self.menu()
            except GLib.Error as error:
                if Gio.DBusError.get_remote_error(error) in (
                        "org.freedesktop.DBus.Error.UnknownMethod",
                        "org.freedesktop.DBus.Error.UnknownObject"):
                    return True
                raise
            return False

        self.wait(unexported, "vanished watcher did not unexport its menu")
        # Leave enough time for the application's unavailable-host poll to run.
        time.sleep(.5)
        self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                  "RequestName", GLib.Variant("(su)", (WATCHER, 4)))
        self.wait(lambda: not unexported() and len(self.menu()) == len(before),
                  "watcher restart lost tray menu")
        assert self.menu() == before, "watcher restart changed menu roles or callbacks"
        print("PASS: watcher name loss/reacquisition restores menu", flush=True)

    def finish(self):
        process = self.process
        self.click(self.menu()[-1])
        self.wait(lambda: process.poll() is not None, "tray Quit failed")
        assert process.returncode == 0, "tray Quit returned an error"
        self.process = None
        self.log.close()
        print("PASS: tray Quit completed", flush=True)

    def close(self):
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            self.log.flush()
            self.log.seek(0)
            print(self.log.read()[-4000:])
            app_log = self.folder / "clash-flux" / "core" / "logs.log"
            if app_log.exists():
                print(app_log.read_text()[-4000:])
            self.log.close()
        self.bus.unregister_object(self.registration)
        self.loop.quit()
        self.thread.join(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="clash-flux-tray-") as directory:
        folder = pathlib.Path(directory)
        fixture = Fixture(args.binary.resolve(), folder)
        try:
            fixture.start(10)
            for _ in range(2):
                fixture.recover_host(10)
            fixture.restart_watcher()
            menu = fixture.menu()
            lines = menu[3]
            assert lines[1].get("children-display") == "submenu"
            assert "toggle-type" not in lines[1] and "toggle-state" not in lines[1]
            lightweight = menu[7]
            fixture.click(lightweight)
            fixture.wait(lambda: fixture.menu()[7][1].get("toggle-state") !=
                         lightweight[1].get("toggle-state"), "checkbox did not update")
            print("PASS: full menu retains submenu and checkbox roles", flush=True)
            fixture.finish()

            # Seed only the stopped fixture database, never the user's database.
            with sqlite3.connect(folder / "clash-flux" / "clash-flux.db") as database:
                database.execute("INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)",
                                 ("tray.start_minimized", "true"))
            fixture.start(7)
            fixture.click(fixture.menu()[0])
            fixture.wait(lambda: len(fixture.menu()) == 10,
                         "window activation did not replace the basic menu")
            fixture.recover_host(10)
            fixture.finish()

            fixture.start(7)
            fixture.set_host(False)
            fixture.wait(lambda: not fixture.menu(), "lost host did not hide the basic tray")
            fixture.set_host(True)
            fixture.wait(lambda: len(fixture.menu()) == 10,
                         "lost headless tray did not fall back to a window and full menu")
            print("PASS: headless host loss opens a fallback window", flush=True)
            fixture.finish()
        finally:
            fixture.close()


if __name__ == "__main__":
    main()
