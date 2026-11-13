#!/usr/bin/env python3
"""Test file-based C compilation on an ESP32-S3; keep the installed compiler/examples."""
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
import time

from build import ROOT, HERE, CACHE, run


INSPECTOR = '''import sys, hashlib, binascii
f = open(sys.argv[1], "rb")
h = hashlib.sha256()
offset = 0
while True:
    data = f.read(128)
    if not data: break
    h.update(data)
    if len(sys.argv) > 2: print("CC_FILE", offset, binascii.hexlify(data).decode())
    offset += len(data)
f.close()
print("CC_HASH", offset, binascii.hexlify(h.digest()).decode())
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--telnet", required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT / "tests/hil"))
    from model_bundle_stream import Console
    sys.path.insert(0, str(ROOT / "modules/espdl"))
    from verify import verify
    output = CACHE / "file-hil"; output.mkdir(exist_ok=True)
    report_path = output / "report.json"
    if report_path.exists(): report_path.unlink()
    folder = "/dev/cc-examples"
    console = Console(args.telnet)
    log = (output / "console.log").open("w")
    server = None

    def command(text, expected=None):
        log.write("\n> " + text + "\n"); log.flush()
        out = console.command(text, 60)
        log.write(out); log.flush()
        if expected is not None and expected not in out: raise RuntimeError(out)
        return out

    def inspect(path, save=None):
        remote = path if path.startswith("/") else folder + "/" + path
        out = command(f"python {folder}/inspect.py {remote}" + (" hex" if save else ""), "CC_HASH ")
        match = re.search(r"CC_HASH (\d+) ([0-9a-f]{64})", out)
        if not match: raise RuntimeError(out)
        if save:
            data = bytearray()
            for offset, encoded in re.findall(r"CC_FILE (\d+) ([0-9a-f]+)", out):
                if int(offset) != len(data): raise RuntimeError("incomplete file stream")
                data.extend(bytes.fromhex(encoded))
            if len(data) != int(match[1]) or hashlib.sha256(data).hexdigest() != match[2]:
                raise RuntimeError("file stream/hash mismatch")
            path = output / save; path.write_bytes(data)
            run(CACHE / "elf-check", path)
            verify(path, {"solar_os_native_host_v1"})
        return match[2]

    try:
        # These are persistent test artifacts. Refuse to overwrite existing work.
        for path in (folder, "/modules/apps/cc.elf", "/modules/libraries/tinycc.elf",
                     "/modules/libraries/hello-library.elf"):
            command("ls " + path, "No such file")
        board, version, ota = command("board"), command("version"), command("ota status")
        if "ESP32-S3" not in board: raise RuntimeError("requires ESP32-S3: " + board)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect((socket.gethostbyname(args.telnet), 23)); address = route.getsockname()[0]
        with tempfile.TemporaryDirectory(prefix="solar-file-cc-") as temporary:
            staged = Path(temporary)
            artifacts = {"tinycc.elf": CACHE / "native/tinycc.library.elf",
                         "cc.elf": CACHE / "native/cc.app.elf",
                         "hello-library.elf": CACHE / "hello-library.library.elf"}
            for name, path in artifacts.items(): shutil.copyfile(path, staged / name)
            hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in artifacts.items()}
            sources = {"hello.c": (HERE / "examples/library_probe.c").read_text(),
                       "scalar.c": (HERE / "examples/scalar_probe.c").read_text(),
                       "plain.c": "int main(void) { return 42; }\n",
                       "include/outer.h": '#include "detail/answer.h"\n',
                       "include/detail/answer.h": '#define ANSWER 42\n',
                       "include/user_api.h": (ROOT / "modules/examples/hello-library/main/hello_library_api.h").read_text(),
                       "syntax.c": "int main(void) { return missing + ; }\n",
                       "missing.c": '#include "missing-header.h"\nint main(void) { return 0; }\n',
                       "import.c": 'extern int unknown(void); int main(void) { return unknown(); }\n',
                       "large.c": " " * 65537,
                       "nul.c": "int main(void) { return 0; }\0\n",
                       "stress.c": "int main(void) { volatile int v=0;\n" + "v++;\n" * 10000 + "return v; }\n",
                       "inspect.py": INSPECTOR}
            # Exercise quoted/nested headers, -I, custom library declarations and the SolarOS ABI.
            sources["headers.c"] = sources["hello.c"].replace('#include "hello_library_api.h"',
                '#include <user_api.h>\n#include "include/outer.h"').replace("add(19, 23)", "add(ANSWER, 0)")
            for name, data in sources.items():
                path = staged / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(data)
            server = ThreadingHTTPServer((address, 0), partial(SimpleHTTPRequestHandler, directory=temporary))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            for path in ("/dev", "/modules", "/modules/apps", "/modules/libraries"):
                if "No such file" in command("ls " + path): command("mkdir " + path)
            for path in (folder, folder + "/include", folder + "/include/detail"):
                out=command("mkdir " + path)
                if "cannot create" in out: raise RuntimeError(out)

            def fetch(name, remote):
                out=command(f"curl -o {remote} http://{address}:{server.server_port}/{name}", "curl: wrote")
                count=re.search(r"curl: wrote (\d+) bytes",out)
                if "curl: HTTP 200," not in out or not count or int(count[1])!=(staged/name).stat().st_size:
                    raise RuntimeError("failed or incomplete transfer: "+out)

            for name in artifacts:
                fetch(name, "/modules/" + ("apps/" if name == "cc.elf" else "libraries/") + name)
            for name in sources: fetch(name, folder + "/" + name)
            for name in artifacts:
                remote = "/modules/" + ("apps/" if name == "cc.elf" else "libraries/") + name
                if inspect(remote) != hashes[name]: raise RuntimeError("installed artifact hash mismatch: " + name)
            command("cd " + folder)
            command("cc --help", "usage: cc input.c")
            command("cc --self-test", "CC_SELF_TEST_OK")
            command("cc hello.c -o hello.elf", "cc: hello.elf (")
            command("load hello.elf", "TinyCC: native library returned 42")
            hello_hash = inspect("hello.elf", "hello.elf")
            command("cc headers.c -I include -o headers.elf", "cc: headers.elf (")
            command("load headers.elf", "TinyCC: native library returned 42")
            inspect("headers.elf", "headers.elf")
            command("cc scalar.c -o scalar.elf", "cc: scalar.elf (")
            command("load scalar.elf", "TinyCC: scalar checks passed")
            inspect("scalar.elf", "scalar.elf")
            command("cc plain.c", "cc: a.elf (")
            command("load a.elf", "load: completed")
            inspect("a.elf", "plain.elf")
            # Relative paths are captured from this shell, including parent components.
            command("cd include")
            command("cc ../hello.c -o ../hello.elf", "cc: ../hello.elf (")
            command("cd ..")
            if inspect("hello.elf") != hello_hash: raise RuntimeError("non-deterministic overwrite")
            for text, expected in (
                ("cc syntax.c -o hello.elf", "/syntax.c:1:"),
                ("cc missing.c -o hello.elf", "missing-header.h"),
                ("cc import.c -o hello.elf", "unsupported native import"),
                ("cc large.c -o hello.elf", "source must contain 1..65536"),
                ("cc nul.c -o hello.elf", "exceeds the supported size"),
                ("cc hello.c -o ./hello.c", "output must differ"),
                ("cc hello.c -o hello.elf --invalid", "unknown option"),
            ):
                command(text, expected)
                if inspect("hello.elf") != hello_hash: raise RuntimeError("error changed previous output: " + text)
            # A conflicting staging file is never removed or truncated.
            fetch("plain.c", folder + "/hello.elf.native.tmp")
            staged_hash = inspect("hello.elf.native.tmp")
            command("cc hello.c -o hello.elf", "cc: hello.elf: operation failed")
            if inspect("hello.elf") != hello_hash or inspect("hello.elf.native.tmp") != staged_hash:
                raise RuntimeError("staging conflict changed files")
            command("rm hello.elf.native.tmp")
            # Interrupt a deliberately expensive request and check recovery/output preservation.
            log.write("\n> interrupt cc stress.c -o hello.elf\n"); log.flush()
            console.socket.sendall(b"cc stress.c -o hello.elf\r\n")
            time.sleep(.2); console.socket.sendall(b"\x1d")
            out = console.read(30, True); log.write(out); log.flush()
            compiler_cancelled="compilation failed (-7)" in out and "compilation cancelled" in out
            input_cancelled="cc: stress.c: cancelled or timed out" in out
            if not compiler_cancelled and not input_cancelled:
                raise RuntimeError("request did not cancel: " + out)
            if inspect("hello.elf") != hello_hash: raise RuntimeError("cancellation changed output")
            command("cc hello.c -o hello.elf", "cc: hello.elf (")
            before = command("mem")
            for _ in range(10):
                command("cc headers.c -Iinclude -o headers.elf", "cc: headers.elf (")
                command("load headers.elf", "TinyCC: native library returned 42")
            after = command("mem")
            before_free = re.search(r"Managed PSRAM: total \d+ free (\d+)", before)
            after_free = re.search(r"Managed PSRAM: total \d+ free (\d+)", after)
            if not before_free or not after_free or before_free[1] != after_free[1]:
                raise RuntimeError("managed free memory changed across repeated compilation")
            for name in ("hello.elf", "headers.elf", "scalar.elf", "a.elf"):
                for suffix in (".native.tmp", ".native.bak"):
                    command("ls " + name + suffix, "No such file")
            # Leave the useful examples and installed modules; discard negative-test inputs.
            for name in ("syntax.c", "missing.c", "import.c", "large.c", "nul.c", "stress.c", "inspect.py"):
                command("rm " + name)
            report = {"host": args.telnet, "board": board, "firmware": version, "ota": ota,
                      "artifacts": hashes, "installed_artifact_hashes_verified": True,
                      "file_compilation_on_device": True,
                      "native_library_result": 42, "scalar_program": True, "standalone_main": True,
                      "quoted_nested_and_include_headers": True, "error_preserves_output": True,
                      "cancellation_and_recovery": True, "staging_conflict_preserves_files": True,
                      "cancellation_phase": "compiler" if compiler_cancelled else "source_read",
                      "repeated_compile_and_run": 10, "before": before, "after": after,
                      "compiler_and_examples_left_installed": True, "examples": folder,
                      "hello_sha256": hello_hash}
            report_path.write_text(json.dumps(report, indent=2) + "\n")
    finally:
        if server: server.shutdown(); server.server_close()
        console.socket.close(); log.close()
    print("SOLAR_CC_FILE_HIL_PASS; cc and examples remain installed", flush=True)


if __name__ == "__main__": main()
