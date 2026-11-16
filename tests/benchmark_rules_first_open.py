"""Measure Linux Rules first open against the production app in an isolated Xvfb session."""
import ctypes as C
import argparse
import csv
import hashlib
import http.server
import os
import pathlib
import re
import socket
import subprocess as sp
import sys
import tempfile
import threading
import time


def wait_for(fn, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.008)
    raise RuntimeError("timeout " + repr(fn))

def config(rows):
    result = ["proxies:", "  - {name: Demo, type: socks5, server: 127.0.0.1, port: 1080}",
              "proxy-groups:", "  - {name: Choice, type: select, proxies: [Demo, DIRECT]}",
              "rule-providers:",
              "  sample: {type: inline, behavior: domain, payload: [provider.test]}",
              "rules:", "  - RULE-SET,sample,Choice"]
    for i in range(rows):
        kind = ("DOMAIN-SUFFIX", "DOMAIN", "DOMAIN-KEYWORD")[i % 3]
        value = f"rule{i}" if kind == "DOMAIN-KEYWORD" else f"rule{i}.test"
        result.append(f"  - {kind},{value},{('DIRECT', 'Choice', 'REJECT')[i % 3]}")
    result.extend(("  - IP-CIDR,192.0.2.0/24,DIRECT,no-resolve", "  - MATCH,Choice"))
    return ("\n".join(result) + "\n").encode()

def env_for(directory):
    runtime = directory / "runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    shared = str(directory)
    return dict(os.environ, HOME=shared, XDG_DATA_HOME=shared, XDG_CONFIG_HOME=shared,
                XDG_CACHE_HOME=shared, XDG_RUNTIME_DIR=str(runtime), LANGUAGE="en", LC_ALL="C.UTF-8",
                NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")

def seed(exe, env, rows):
    body = config(rows)
    class Source(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *_):
            pass
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Source) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            uri = f"http://127.0.0.1:{server.server_port}/fixture"
            imported = sp.run([exe, "profile", "import", uri, f"Fixture-{rows}"], env=env,
                              capture_output=True, timeout=30, text=True)
            if imported.returncode:
                raise RuntimeError(imported.stdout + imported.stderr)
        finally:
            server.shutdown()
            thread.join()

def window():
    win = sp.check_output(["xwininfo", "-root", "-tree"], text=True)
    for line in win.splitlines():
        if '("clash-flux" "clash-flux")' in line or '"Clash Flux"' in line or '"Clash-Flux"' in line:
            result = re.match(r"\s*(0x\w+).*?\s(\d+)x(\d+)\+\d+\+\d+\s+\+(\d+)\+(\d+)", line)
            if result and int(result[2]) > 500 and int(result[3]) > 450:
                return result.groups()
    return None

class X11:
    def __init__(self):
        self.x = C.CDLL("libX11.so.6")
        self.xt = C.CDLL("libXtst.so.6")
        self.x.XOpenDisplay.argtypes = [C.c_char_p]
        self.x.XOpenDisplay.restype = C.c_void_p
        self.d = self.x.XOpenDisplay(None)
        if not self.d:
            raise RuntimeError("Cannot open the isolated X11 display")
        self.xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.x.XSync.argtypes = [C.c_void_p, C.c_int]
        self.x.XDefaultRootWindow.argtypes = [C.c_void_p]
        self.x.XDefaultRootWindow.restype = C.c_ulong
        self.x.XGetImage.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_ulong, C.c_int]
        self.x.XGetImage.restype = C.POINTER(ImageHeader)
        self.x.XDestroyImage.argtypes = [C.POINTER(ImageHeader)]
        self.root = self.x.XDefaultRootWindow(self.d)

    def frame(self, box):
        shot = self.x.XGetImage(self.d, self.root, *box, C.c_ulong(-1), 2)
        if not shot:
            raise RuntimeError("Cannot read X11 framebuffer")
        try:
            header = shot.contents
            return hashlib.sha256(C.string_at(header.data, header.bytes_per_line * header.height)).digest()
        finally:
            self.x.XDestroyImage(shot)

    def close(self):
        self.x.XCloseDisplay.argtypes = [C.c_void_p]
        self.x.XCloseDisplay(self.d)

    def click(self, x, y):
        self.xt.XTestFakeMotionEvent(self.d, -1, x, y, 0)
        self.xt.XTestFakeButtonEvent(self.d, 1, True, 0)
        self.xt.XTestFakeButtonEvent(self.d, 1, False, 0)
        self.xt.XTestFakeMotionEvent(self.d, -1, 1200, 800, 0)
        self.x.XSync(self.d, False)


HOME_BOX = (90, 9, 70, 25)
ROWS_BOX = (90, 152, 320, 93)
LOG_FIELDS = ("wait_us", "read_us", "target_us", "target_reused", "bytes", "rows")


class ImageHeader(C.Structure):
    _fields_ = [
        ("width", C.c_int), ("height", C.c_int), ("xoffset", C.c_int), ("format", C.c_int),
        ("data", C.c_void_p), ("byte_order", C.c_int), ("bitmap_unit", C.c_int),
        ("bitmap_bit_order", C.c_int), ("bitmap_pad", C.c_int), ("depth", C.c_int),
        ("bytes_per_line", C.c_int),
    ]


def stop(app):
    if app.poll() is None:
        app.terminate()
    try:
        app.wait(timeout=5)
    except sp.TimeoutExpired:
        app.kill()
        app.wait()


def calibration(exe, env, directory, x, log_path):
    with (directory / "app.txt").open("w") as output:
        app = sp.Popen([exe], env=env, cwd=directory, stdout=output, stderr=output)
        try:
            win = wait_for(window)
            if win[1:] != ("1080", "720", "0", "0"):
                raise RuntimeError(f"Unexpected window geometry: {win}")
            def prewarm_decided():
                text = log_path.read_text(errors="replace")
                return "rules-prewarm finish" in text or "rules-prewarm defer" in text

            wait_for(prewarm_decided)
            time.sleep(.25)
            home = x.frame(HOME_BOX)
            x.click(34, 212)
            wait_for(lambda: "rules-load " in log_path.read_text(errors="replace"))
            time.sleep(.55)
            rows = x.frame(ROWS_BOX)
            x.click(34, 68)
            wait_for(lambda: x.frame(HOME_BOX) == home)
            if rows == x.frame(ROWS_BOX):
                raise RuntimeError("Click did not switch pages")
            return home, rows
        finally:
            stop(app)


def trial(exe, env, directory, x, log_path, reference, delay):
    log_path.write_text("")
    home, rows = reference
    marks = {}

    def observe():
        now = time.monotonic_ns()
        text = log_path.read_text(errors="replace")
        for key in ("rules-prewarm begin", "rules-load ", "rules-prewarm finish", "rules-prewarm defer"):
            if key in text:
                marks.setdefault(key, now)
        return text

    with (directory / "app.txt").open("w") as output:
        started = time.monotonic_ns()
        app = sp.Popen([exe], env=env, cwd=directory, stdout=output, stderr=output)
        try:
            def observe_home():
                observe()
                if x.frame(HOME_BOX) == home:
                    return time.monotonic_ns()
                return None

            home_at = wait_for(observe_home)
            until = home_at + delay * 1_000_000
            while time.monotonic_ns() < until:
                observe()
                time.sleep(.004)
            observe()
            clicked = time.monotonic_ns()
            x.click(34, 212)

            def observe_rows():
                observe()
                if x.frame(ROWS_BOX) == rows:
                    return time.monotonic_ns()
                return None

            rows_at = wait_for(observe_rows)
            time.sleep(.20)
            text = observe()
            matches = re.findall(r'rules-load ([^"]+)', text)
            loads = [dict(re.findall(r"(\w+)=(\d+)", match)) for match in matches]
            identities = {(load["rows"], load["source_id"], load["source_generation"]) for load in loads}
            if not 1 <= len(loads) <= 2 or len(identities) != 1:
                raise RuntimeError(f"Unexpected source loads: {loads}")
            fields = loads[-1]
            ios = {field: int(fields[field]) for field in LOG_FIELDS}
            rss = int(re.search(r"^VmHWM:\s+(\d+) kB", pathlib.Path(f"/proc/{app.pid}/status").read_text(), re.M)[1])

            def ms(at, since=started):
                return None if at is None else round((at - since) / 1_000_000, 3)

            finished = marks.get("rules-prewarm finish")
            return dict(home_ms=ms(home_at), click_ms=ms(clicked), first_rows_ms=ms(rows_at),
                        click_to_rows_ms=ms(rows_at, clicked), prewarm_started_ms=ms(marks.get("rules-prewarm begin")),
                        model_logged_ms=ms(marks.get("rules-load ")), prewarm_finished_ms=ms(finished),
                        prewarm_finished_by_click=int(finished is not None and finished < clicked),
                        prewarm_deferred_ms=ms(marks.get("rules-prewarm defer")),
                        loads=len(loads), prewarm_published=int("ready=1 reads_started=" in text),
                        first_load_target_us=int(loads[0]["target_us"]), peak_rss_kib=rss, **ios)
        finally:
            stop(app)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=pathlib.Path)
    parser.add_argument("--work-dir", required=True, type=pathlib.Path)
    parser.add_argument("--rows", nargs="+", type=int, default=(1000, 10000, 50000))
    parser.add_argument("--delays", nargs="+", type=int, default=(0, 250, 1000))
    parser.add_argument("--samples", type=int, default=5)
    args = parser.parse_args()
    if {name for _, name in socket.if_nameindex()} != {"lo"} or os.environ.get("DISPLAY") != ":91":
        parser.error("run in the loopback-only sandbox with DISPLAY=:91 described in the evidence")
    if any(not 1 <= count <= 100000 for count in args.rows) or args.samples < 1 or min(args.delays) < 0:
        parser.error("use 1–100000 rows, at least one sample and nonnegative delays")
    exe = args.exe.resolve(strict=True)
    work_dir = args.work_dir.resolve(strict=True)
    fields = ("input_rows", "delay_ms", "sample", "home_ms", "click_ms", "first_rows_ms", "click_to_rows_ms",
              "prewarm_started_ms", "model_logged_ms", "prewarm_finished_ms", "prewarm_finished_by_click", "prewarm_deferred_ms",
              "loads", "prewarm_published", "first_load_target_us", "peak_rss_kib", *LOG_FIELDS)
    writer = csv.DictWriter(sys.stdout, fields, lineterminator="\n")
    writer.writeheader()
    with tempfile.TemporaryFile(dir=work_dir) as xlog:
        xvfb = sp.Popen(["Xvfb", ":91", "-screen", "0", "1280x850x24", "-nolisten", "tcp"],
                        stdout=xlog, stderr=xlog)
        try:
            wait_for(lambda: sp.run(["xwininfo", "-root"], capture_output=True).returncode == 0)
            x = X11()
            try:
                for rows in args.rows:
                    with tempfile.TemporaryDirectory(dir=work_dir) as tmp:
                        directory = pathlib.Path(tmp)
                        env = env_for(directory)
                        seed(exe, env, rows)
                        log_path = directory / "clash-flux" / "core" / "logs.log"
                        log_path.touch()
                        log_path.write_text("")
                        reference = calibration(exe, env, directory, x, log_path)
                        for sample in range(args.samples):
                            offset = sample % len(args.delays)
                            for delay in args.delays[offset:] + args.delays[:offset]:
                                result = trial(exe, env, directory, x, log_path, reference, delay)
                                if result["rows"] != rows + 3:
                                    raise RuntimeError(f"Unexpected view size: {result}")
                                writer.writerow(dict(input_rows=rows, delay_ms=delay, sample=sample + 1, **result))
                                sys.stdout.flush()
            finally:
                x.close()
        finally:
            stop(xvfb)


if __name__ == "__main__":
    main()
