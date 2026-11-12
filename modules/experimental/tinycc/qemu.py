#!/usr/bin/env python3
"""Build and execute the isolated ESP32-S3 loader/ABI emulator checks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from build import HERE, CACHE, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--native", action="store_true", help="run the compiler itself in the ESP32-S3 emulator")
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    project = HERE / ("native-qemu" if args.native else "qemu")
    output = CACHE / project.name; output.mkdir(exist_ok=True)
    pio = Path.home() / ".platformio/penv/bin"
    build = project / ".pio/build/probe"
    with (output / "build.log").open("w") as log:
        subprocess.run([str(pio / "pio"), "run", "-d", str(project), "-e", "probe"],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    tool = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
    flash = output / "flash.bin"
    run(pio / "python", tool, "--chip", "esp32s3", "merge-bin", "--pad-to-size", "8MB",
        "-o", flash, "0x0", "bootloader.bin", "0x8000", "partitions.bin",
        "0x10000", "firmware.bin", cwd=build)
    marker = "SOLAR_CC_NATIVE_PASS:" if args.native else "SOLAR_TCC_PASS:"
    console = output / "console.log"
    with console.open("wb") as log:
        process = subprocess.Popen([str(qemu), "-machine", "esp32s3", "-nographic", *(["-m", "8M"] if args.native else []),
                                    "-drive", f"file={flash},if=mtd,format=raw"],
                                   stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + (60 if args.native else 15)
        try:
            while time.monotonic() < deadline and process.poll() is None:
                time.sleep(.1)
                text = console.read_text(errors="replace")
                if marker in text or "assert failed:" in text or "Guru Meditation" in text: break
        finally:
            if process.poll() is None: process.terminate()
            process.wait(timeout=5)
    text = console.read_text(errors="replace")
    if marker not in text or (not args.native and "SCALAR result=0" not in text):
        raise RuntimeError("emulator checks failed: " + str(output / "console.log"))
    paths = ["native/tinycc.library.elf", "native/cc.app.elf"] if args.native else [
        "scalar_probe.elf", "library_probe.elf", "hello-library.elf", "library-user.elf"]
    report = {"qemu": subprocess.check_output([str(qemu), "--version"], text=True).splitlines()[0],
              "target": "esp32s3", "compiler_ran_on_target": args.native,
              "artifacts": {name: hashlib.sha256((CACHE / name).read_bytes()).hexdigest() for name in paths}}
    if not args.native: report.update(abi_cycles=100, generated_provider_and_job_callbacks=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(next(line for line in text.splitlines() if marker in line))


if __name__ == "__main__":
    main()
