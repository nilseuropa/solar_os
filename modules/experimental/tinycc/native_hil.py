#!/usr/bin/env python3
"""Compile C on SolarOS, validate its returned ELF, then run it on the device."""
import argparse
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import shutil
import socket
import sys
import tempfile
import threading

from build import ROOT, CACHE, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--telnet", required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT / "tests/hil"))
    from model_bundle_stream import Console
    output = CACHE / "native-hil"; output.mkdir(exist_ok=True)
    console = Console(args.telnet)
    log = (output / "console.log").open("w")
    owned_folder = owned_compiler = owned_provider = False
    server = None
    report = None

    def command(text):
        out = console.command(text, 60)
        log.write(out); log.flush(); return out

    def check(text, expected):
        out = command(text)
        if expected not in out or "CC_ERROR" in out: raise RuntimeError(out)
        return out

    try:
        for path in ("/dev/tinycc-compiler", "/modules/libraries/tinycc.elf", "/modules/libraries/hello-library.elf"):
            check("ls " + path, "No such file or di")
        board, version = command("board"), command("version")
        if "ESP32-S3" not in board: raise RuntimeError("requires ESP32-S3: " + board)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect((socket.gethostbyname(args.telnet), 23)); address = route.getsockname()[0]
        with tempfile.TemporaryDirectory(prefix="solar-native-cc-") as temporary:
            staged = {"tinycc.elf": CACHE / "native/tinycc.library.elf",
                      "cc.elf": CACHE / "native/cc.app.elf",
                      "hello-library.elf": CACHE / "hello-library.library.elf"}
            for name, path in staged.items(): shutil.copyfile(path, Path(temporary) / name)
            hashes = {name: hashlib.sha256((Path(temporary) / name).read_bytes()).hexdigest() for name in staged}
            server = ThreadingHTTPServer((address, 0), partial(SimpleHTTPRequestHandler, directory=temporary))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            command("mkdir /dev/tinycc-compiler"); owned_folder = True
            for name in staged:
                if name == "cc.elf": remote = "/dev/tinycc-compiler/cc.elf"
                else:
                    remote = "/modules/libraries/" + name
                    if name == "tinycc.elf": owned_compiler = True
                    else: owned_provider = True
                check(f"curl -o {remote} http://{address}:{server.server_port}/{name}", "curl: wrote")
            warmup = check("load /dev/tinycc-compiler/cc.elf --self-test", "CC_SELF_TEST_OK")
            print(warmup, flush=True)
            before = command("mem")
            repeat = check("load /dev/tinycc-compiler/cc.elf --self-test", "CC_SELF_TEST_OK")
            print(repeat, flush=True)
            after = command("mem")
            encoded = check("load /dev/tinycc-compiler/cc.elf --demo-hex", "CC_ELF_END")
            size = re.search(r"CC_ELF_BEGIN bytes=(\d+)", encoded)
            if not size: raise RuntimeError(encoded)
            data = bytearray()
            for offset, hex_bytes in re.findall(r"CC_ELF ([0-9a-f]{8}) ([0-9a-f]+)", encoded):
                if int(offset, 16) != len(data): raise RuntimeError("ELF stream offset mismatch")
                data.extend(bytes.fromhex(hex_bytes))
            if len(data) != int(size[1]): raise RuntimeError("incomplete ELF stream")
            generated = output / "generated.elf"; generated.write_bytes(data)
            run(CACHE / "elf-check", generated)
            sys.path.insert(0, str(ROOT / "modules/espdl"))
            from verify import verify
            verify(generated, {"solar_os_native_host_v1"})
            shutil.copyfile(generated, Path(temporary) / "generated.elf")
            check(f"curl -o /dev/tinycc-compiler/generated.elf http://{address}:{server.server_port}/generated.elf", "curl: wrote")
            executed = check("load /dev/tinycc-compiler/generated.elf", "TinyCC: native library returned 42")
            print(executed, flush=True)
            encoded_scalar = check("load /dev/tinycc-compiler/cc.elf --scalar-hex", "CC_ELF_END")
            scalar_size = re.search(r"CC_ELF_BEGIN bytes=(\d+)", encoded_scalar)
            scalar = bytearray()
            for offset, hex_bytes in re.findall(r"CC_ELF ([0-9a-f]{8}) ([0-9a-f]+)", encoded_scalar):
                if int(offset, 16) != len(scalar): raise RuntimeError("scalar ELF stream offset mismatch")
                scalar.extend(bytes.fromhex(hex_bytes))
            if not scalar_size or len(scalar) != int(scalar_size[1]): raise RuntimeError("incomplete scalar ELF stream")
            scalar_file = output / "scalar.elf"; scalar_file.write_bytes(scalar)
            run(CACHE / "elf-check", scalar_file); verify(scalar_file, {"solar_os_native_host_v1"})
            shutil.copyfile(scalar_file, Path(temporary) / "scalar.elf")
            check(f"curl -o /dev/tinycc-compiler/scalar.elf http://{address}:{server.server_port}/scalar.elf", "curl: wrote")
            scalar_executed = check("load /dev/tinycc-compiler/scalar.elf", "TinyCC: scalar checks passed")
            print(scalar_executed, flush=True)
            check("pkg unload tinycc", "pkg: unloaded library tinycc")
            check("pkg unload hello-library", "pkg: unloaded library hello-library")
            report = {"host": args.telnet, "board": board, "firmware": version,
                      "compiler_ran_on_device": True, "self_test_runs": 2,
                      "generated_program_ran_on_device": True, "real_library_result": 42,
                      "generated_bytes": len(data), "generated_sha256": hashlib.sha256(data).hexdigest(),
                      "scalar_generated_on_device_and_executed": True,
                      "scalar_sha256": hashlib.sha256(scalar).hexdigest(),
                      "artifacts": hashes, "before": before, "after": after, "warmup": warmup}
    finally:
        try:
            if owned_provider: check("pkg remove hello-library", "pkg: removed hello-library")
            if owned_compiler: check("pkg remove tinycc", "pkg: removed tinycc")
            if owned_folder:
                command("rm /dev/tinycc-compiler/scalar.elf")
                command("rm /dev/tinycc-compiler/generated.elf")
                command("rm /dev/tinycc-compiler/cc.elf")
                command("rm -f /dev/tinycc-compiler")
                check("ls /dev/tinycc-compiler", "No such file or di")
        finally:
            if server: server.shutdown(); server.server_close()
            console.socket.close(); log.close()
    report["temporary_files_removed"] = True
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("SOLAR_CC_NATIVE_HIL_PASS", flush=True)


if __name__ == "__main__": main()
