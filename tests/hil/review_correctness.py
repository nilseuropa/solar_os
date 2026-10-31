#!/usr/bin/env python3
"""Exercise HTTP redirect policy and app/parameter retirement on an owned device.

Requires the configured Telnet shell, Python, Synth and Funcgen. Runs two local
HTTP origins. --serial adds a temporary port shell for concurrent parameter
access; only sessions created by this harness are closed. No device files or
persistent settings are changed.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import socket
import threading
import time

from model_bundle_stream import Console


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.handle_request()

    def do_POST(self):
        self.handle_request()

    def handle_request(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path in ("/same", "/cross", "/post303", "/post307"):
            status = 303 if self.path == "/post303" else 307
            location = "/echo" if self.path in ("/same", "/post303") else self.server.peer + "/echo"
            self.send_response(status)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/304":
            self.send_response(304)
            self.end_headers()
            return
        record = dict(method=self.command, authorization=bool(self.headers.get("Authorization")),
                      cookie=bool(self.headers.get("Cookie")), bytes=len(body))
        self.server.records.append(record)
        data = json.dumps(record).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class SerialConsole:
    def __init__(self, path):
        import serial
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=.05, write_timeout=2)
        self.serial.dtr = self.serial.rts = False
        self.serial.port = path
        self.serial.open()
        self.socket = self

    def sendall(self, data):
        self.serial.write(data)
        self.serial.flush()

    def read(self, seconds, prompt=False):
        del prompt
        data = bytearray()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data.extend(self.serial.read(4096))
        return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', data.decode(errors='replace'))

    def close(self):
        self.serial.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=8)
    parser.add_argument("--serial", help="USB/UART device for the parameter concurrency test")
    parser.add_argument("--port", default="cdc0", help="SolarOS port mapped to --serial")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100:
        parser.error("cycles must be 1..100")
    args.output.mkdir(parents=True, exist_ok=False)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((socket.gethostbyname(args.host), 23))
        address = route.getsockname()[0]
    servers = [ThreadingHTTPServer(("0.0.0.0", 0), Handler) for _ in range(2)]
    for server in servers:
        server.records = []
        threading.Thread(target=server.serve_forever, daemon=True).start()
    servers[0].peer = f"http://{address}:{servers[1].server_port}"
    base = f"http://{address}:{servers[0].server_port}"
    console = observer = None
    serial_console = None
    opened = []
    port_session = None
    in_python = False
    log = (args.output / "console.log").open("w")

    def note(text):
        log.write(text + "\n")
        log.flush()

    def wait(marker, timeout=30):
        output = ""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            output += console.read(.2)
            if marker in output:
                note(output)
                if "Traceback" in output or "Guru Meditation" in output:
                    raise AssertionError(output)
                return output
        note(output)
        raise TimeoutError(f"missing {marker}: {output}")

    def python(text):
        # The interactive editor has a 192-byte line bound. Pace input too:
        # every typed byte can redraw the line over a bounded port queue.
        assert len(text.encode()) < 190
        data = (text + "\r\n").encode()
        for offset in range(0, len(data), 16):
            console.socket.sendall(data[offset:offset + 16])
            time.sleep(.02)
        return wait(">>> ")

    def create(app):
        output = observer.command(f"session create {app} display0", 15)
        note(output)
        match = re.search(r"session (\d+) created", output)
        assert match, output
        opened.append(match[1])
        return match[1]

    def close(sid):
        output = observer.command(f"session close {sid}", 15)
        note(output)
        assert "closed session" in output, output
        opened.remove(sid)

    try:
        if args.serial:
            # Opening a USB/UART bridge can reset its target even when DTR/RTS
            # were configured before open. Open once, before creating sessions.
            serial_console = SerialConsole(args.serial)
            note(serial_console.read(12))
        for attempt in range(10):
            try:
                console = Console(args.host)
                break
            except (OSError, RuntimeError):
                if attempt == 9:
                    raise
                time.sleep(1)
        for command in ("version", "board", "display list", "mem policy"):
            note(command + "\n" + console.command(command))
        console.socket.sendall(b"python\r\n")
        in_python = True
        wait(">>> ")
        python("import solaros")
        python(f"u={base!r}")
        python("h={'Authorization':'Bearer review-fixture','Cookie':'review=1'}")
        for path, method, status, auth, length in (
            ("/same", "GET", 200, True, 0),
            ("/cross", "GET", 200, False, 0),
            ("/304", "GET", 304, None, 0),
            ("/post303", "POST", 200, True, 0),
            ("/post307", "POST", 200, False, 11),
        ):
            for server in servers:
                server.records.clear()
            payload = "review-body" if method == "POST" else None
            python(f"r=solaros.http.request({method!r},u+{path!r},{payload!r},h)")
            output = python(f"assert r['status_code']=={status}; print('HTTP_CASE_OK')")
            assert "HTTP_CASE_OK" in output, output
            if auth is not None:
                records = servers[0].records + servers[1].records
                assert len(records) == 1, records
                record = records[0]
                expected_method = "POST" if path == "/post307" else "GET"
                assert record == dict(method=expected_method, authorization=auth, cookie=auth, bytes=length), record
        console.socket.sendall(b"exit()\r\n")
        in_python = False
        note(console.read(2))
        if args.serial:
            observer = console
            output = observer.command("job start shell " + args.port)
            note(output)
            match = re.search(r"session (\d+) created", output)
            assert match, output
            port_session = match[1]
            console = serial_console
            serial_console = None
            console.sendall(b"\r\n")
            note(console.read(1))
            console.sendall(b"python\r\n")
            in_python = True
            wait(">>> ")
        for app in (("synth", "funcgen") if args.serial else ()):
            sid = create(app)
            program = ("import solaros\nseen=0\nmissing=0\nfor i in range(10000):\n"
                       f" try:\n  solaros.parameters.get('{app}." + ("volume" if app == "synth" else "frequency") +
                       "')\n  seen+=1\n except OSError:\n  missing+=1\n solaros.time.sleep_ms(1)\n"
                       "assert seen>0 and missing>0\nprint('PARAMETERS_OK',seen,missing)")
            python("s=''")
            for offset in range(0, len(program), 60):
                python("s+=" + repr(program[offset:offset + 60]))
            console.socket.sendall(b"exec(s)\r\n")
            for _ in range(args.cycles):
                close(sid)
                sid = create(app)
            wait(">>> ", timeout=45)
            close(sid)
        if in_python:
            console.socket.sendall(b"exit()\r\n")
            in_python = False
            note(console.read(2))
        if observer:
            console.socket.close()
            console = observer
            observer = None
            note(console.command("session close " + port_session))
            port_session = None
        for command in ("session list", "mem policy", "log show"):
            note(command + "\n" + console.command(command))
        (args.output / "result.json").write_text(json.dumps({"host": args.host, "http": "pass",
            "parameter_cycles_per_app": args.cycles if args.serial else 0}, indent=2) + "\n")
        print("CORRECTNESS_STAGE_OK", args.host)
    finally:
        if console and in_python:
            console.socket.sendall(b"\x03\x03\r\nexit()\r\n")
            note(console.read(1))
            in_python = False
        if observer:
            for sid in reversed(opened):
                note(observer.command(f"session close {sid}"))
            if port_session:
                note(observer.command("session close " + port_session))
            observer.socket.close()
        if console:
            console.socket.close()
        if serial_console:
            serial_console.close()
        for server in servers:
            server.shutdown()
            server.server_close()
        log.close()


if __name__ == "__main__":
    main()
