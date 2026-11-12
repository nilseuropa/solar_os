#!/usr/bin/env python3
"""Build and execute the isolated ESP32-S3 loader/ABI emulator checks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from build import HERE, CACHE, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    output = CACHE / "qemu"; output.mkdir(exist_ok=True)
    pio = Path.home() / ".platformio/penv/bin"
    build = HERE / "qemu/.pio/build/probe"
    with (output / "build.log").open("w") as log:
        subprocess.run([str(pio / "pio"), "run", "-d", str(HERE / "qemu"), "-e", "probe"],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    tool = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
    flash = output / "flash.bin"
    run(pio / "python", tool, "--chip", "esp32s3", "merge-bin", "--pad-to-size", "8MB",
        "-o", flash, "0x0", "bootloader.bin", "0x8000", "partitions.bin",
        "0x10000", "firmware.bin", cwd=build)
    process = subprocess.Popen([str(qemu), "-machine", "esp32s3", "-nographic",
                                "-drive", f"file={flash},if=mtd,format=raw"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        text, _ = process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate(); text, _ = process.communicate(timeout=5)
    text = text.decode(errors="replace")
    (output / "console.log").write_text(text)
    if "SOLAR_TCC_PASS:" not in text or "SCALAR result=0" not in text:
        raise RuntimeError("emulator checks failed: " + str(output / "console.log"))
    report = {"qemu": subprocess.check_output([str(qemu), "--version"], text=True).splitlines()[0],
              "target": "esp32s3", "abi_cycles": 100, "generated_provider_and_job_callbacks": True,
              "artifacts": {name: hashlib.sha256((CACHE / name).read_bytes()).hexdigest()
                            for name in ("scalar_probe.elf", "library_probe.elf", "hello-library.elf", "library-user.elf")}}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(next(line for line in text.splitlines() if "SOLAR_TCC_PASS:" in line))


if __name__ == "__main__":
    main()
