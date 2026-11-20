#!/usr/bin/env python3
"""Run probes over telnet using temporary files and a local HTTP server."""
import argparse
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import socket
import sys
import tempfile
import threading

from build import ROOT, CACHE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--telnet", required=True)
    parser.add_argument("--runs", type=int, default=20)
    args = parser.parse_args()
    if not 1 <= args.runs <= 100: parser.error("--runs must be 1..100")
    sys.path.insert(0, str(ROOT / "tests/hil"))
    from model_bundle_stream import Console
    files = ("scalar_probe.elf", "library_probe.elf", "hello-library.library.elf")
    for name in files:
        if not (CACHE / name).exists(): raise SystemExit("missing " + name)
    target = socket.gethostbyname(args.telnet)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((target, 23)); address = route.getsockname()[0]
    report_dir = CACHE / "hil"; report_dir.mkdir(exist_ok=True)
    log = (report_dir / "console.log").open("w")
    console = Console(args.telnet)
    owned_folder = owned_library = False
    server = None

    def command(text):
        output = console.command(text, 30)
        log.write(output); log.flush()
        return output

    def check(text, expected):
        output = command(text)
        if expected not in output: raise RuntimeError(output)
        return output

    try:
        check("ls /dev/tinycc-prototype", "No such file or directory")
        check("ls /modules/libraries/hello-library.elf", "No such file or di")
        board, version = command("board"), command("version")
        if "ESP32-S3" not in board: raise RuntimeError("prototype requires ESP32-S3: " + board)
        with tempfile.TemporaryDirectory(prefix="solar-tcc-http-") as temporary:
            for name in files: shutil.copyfile(CACHE / name, Path(temporary) / name)
            hashes = {name: hashlib.sha256((Path(temporary) / name).read_bytes()).hexdigest() for name in files}
            handler = partial(SimpleHTTPRequestHandler, directory=temporary)
            server = ThreadingHTTPServer((address, 0), handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            command("mkdir /dev/tinycc-prototype"); owned_folder = True
            for name in files:
                if name == "hello-library.library.elf":
                    remote = "/modules/libraries/hello-library.elf"; owned_library = True
                else: remote = "/dev/tinycc-prototype/" + name
                check(f"curl -o {remote} http://{address}:{server.server_port}/{name}", "curl: wrote")
            # Warm the loader and provider before comparing reported free memory.
            check("load /dev/tinycc-prototype/scalar_probe.elf", "TinyCC: scalar checks passed")
            check("load /dev/tinycc-prototype/library_probe.elf", "TinyCC: native library returned 42")
            check("pkg unload hello-library", "pkg: unloaded library hello-library")
            before = command("mem")
            for i in range(args.runs):
                check("load /dev/tinycc-prototype/scalar_probe.elf", "TinyCC: scalar checks passed")
                check("load /dev/tinycc-prototype/library_probe.elf", "TinyCC: native library returned 42")
                check("pkg unload hello-library", "pkg: unloaded library hello-library")
                print(f"cycle {i+1}/{args.runs}", flush=True)
            after = command("mem")
            report = {"host": args.telnet, "board": board, "firmware": version,
                      "cycles": args.runs, "scalar_pass": True, "real_library_abi_pass": True,
                      "unload_each_cycle": True, "before": before, "after": after, "artifacts": hashes}
            (report_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            print("SOLAR_TCC_HIL_PASS", flush=True)
    finally:
        try:
            if owned_library: check("pkg remove hello-library", "pkg: removed hello-library")
            if owned_folder:
                command("rm /dev/tinycc-prototype/scalar_probe.elf")
                command("rm /dev/tinycc-prototype/library_probe.elf")
                command("rm -f /dev/tinycc-prototype")
        finally:
            if server: server.shutdown(); server.server_close()
            console.socket.close(); log.close()


if __name__ == "__main__":
    main()
