#!/usr/bin/env python3
"""Build and execute the isolated ESP32-S3 loader/ABI emulator checks."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import time

from build import HERE, CACHE, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument("--native", action="store_true", help="run the compiler itself in the ESP32-S3 emulator")
    mode.add_argument("--builtin", action="store_true", help="run the firmware-linked compiler and command")
    mode.add_argument("--differential", action="store_true", help="run the prepared GCC comparison corpus")
    args = parser.parse_args()
    args.native=args.native or args.differential or args.builtin
    qemu = args.qemu.resolve()
    project = HERE / ("native-qemu" if args.native else "qemu")
    output = CACHE / ("builtin-qemu" if args.builtin else "differential-qemu" if args.differential else project.name); output.mkdir(exist_ok=True)
    report_file=output / "report.json"
    if report_file.exists(): report_file.unlink()
    console = output / "console.log"
    if console.exists(): console.unlink()
    pio = Path.home() / ".platformio/penv/bin"
    environment="builtin" if args.builtin else "differential" if args.differential else "probe"
    build = project / ".pio/build" / environment
    with (output / "build.log").open("w") as log:
        subprocess.run([str(pio / "pio"), "run", "-d", str(project), "-e", environment],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    tool = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
    flash = output / "flash.bin"
    run(pio / "python", tool, "--chip", "esp32s3", "merge-bin", "--pad-to-size", "8MB",
        "-o", flash, "0x0", "bootloader.bin", "0x8000", "partitions.bin",
        "0x10000", "firmware.bin", cwd=build)
    marker = "SOLAR_CC_DIFFERENTIAL_PASS:" if args.differential else "SOLAR_CC_FILE_ADAPTER_PASS:" if args.native else "SOLAR_TCC_PASS:"
    with console.open("wb") as log:
        process = subprocess.Popen([str(qemu), "-machine", "esp32s3", "-nographic", *(["-m", "8M"] if args.native else []),
                                    "-drive", f"file={flash},if=mtd,format=raw"],
                                   stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + (180 if args.differential else 60 if args.native else 15)
        try:
            while time.monotonic() < deadline and process.poll() is None:
                time.sleep(.1)
                text = console.read_text(errors="replace")
                if marker in text or "assert failed:" in text or "Guru Meditation" in text: break
        finally:
            if process.poll() is None: process.terminate()
            process.wait(timeout=5)
    text = console.read_text(errors="replace")
    if (marker not in text or
        (args.native and any(required not in text for required in
                             ("SOLAR_CC_NATIVE_PASS:", "SOLAR_CC_LIBC_EXTENSION_PASS:", "SOLAR_CC_SDK_PASS:", "SOLAR_CC_PROJECT_PASS:"))) or
        (not args.native and "SCALAR result=0" not in text)):
        raise RuntimeError("emulator checks failed: " + str(output / "console.log"))
    paths = ["native/tinycc.library.elf", "native/cc.app.elf"] if args.native else [
        "scalar_probe.elf", "library_probe.elf", "hello-library.elf", "library-user.elf"]
    report = {"qemu": subprocess.check_output([str(qemu), "--version"], text=True).splitlines()[0],
              "target": "esp32s3", "compiler_ran_on_target": args.native,
              "firmware_sha256": hashlib.sha256((build / "firmware.bin").read_bytes()).hexdigest(),
              "firmware_bytes": (build / "firmware.bin").stat().st_size,
              "artifacts": {name: hashlib.sha256((CACHE / name).read_bytes()).hexdigest() for name in paths}}
    if args.builtin:
        if any(marker not in text for marker in ('SOLAR_CC_BUILTIN_PASS:', 'SOLAR_CC_PACKED_SDK_PASS:')):
            raise RuntimeError('missing built-in compiler or packed SDK checks')
        report['builtin_archive_sha256'] = hashlib.sha256(
            (build/'tinycc/libsolar_os_cc.a').read_bytes()).hexdigest()
        report['compiler_built_in'] = True
        report['legacy_sdk_view_matches'] = True
    if args.native:
        loaded = re.search(r'CC_MEMORY deployment=(\w+) before_psram=(\d+) loaded_psram=(\d+) before_internal=(\d+) loaded_internal=(\d+)', text)
        released = re.search(r'CC_MEMORY_AFTER psram=(\d+) internal=(\d+)', text)
        if not loaded or not released: raise RuntimeError('missing compiler memory measurements')
        report['memory'] = dict(zip(('deployment','before_psram','loaded_psram','before_internal','loaded_internal'),
                                   [loaded[1], *map(int, loaded.groups()[1:])]))
        report['memory'].update(after_psram=int(released[1]), after_internal=int(released[2]))
    if not args.native: report.update(abi_cycles=100, generated_provider_and_job_callbacks=True)
    else: report.update(legacy_request_abi=True, standalone_main=True, nested_file_headers=True,
                        unsupported_import_rejected=True, optional_libc_extension=True, project_build=True)
    if args.differential:
        rows=re.findall(r"DIFF_CASE name=(\w+) mode=(\w+) vectors=(\d+) mismatches=(\d+) bytes=(\d+) peak=(\d+) ms=(\d+)",text)
        report["comparisons"]=[dict(zip(("name","mode","vectors","mismatches","bytes","peak","ms"),
                                      [*row[:2],*map(int,row[2:])])) for row in rows]
        manifest=json.loads((CACHE / "differential/manifest.json").read_text())
        expected={(name,mode) for name in manifest["cases"] for mode in ("host","native")}
        if (len(rows)!=len(expected) or {(row[0],row[1]) for row in rows}!=expected or "DIFF_FAIL" in text or
            any(int(row[3]) or int(row[2])!=manifest["vectors"] for row in rows)):
            raise RuntimeError("incomplete or failing differential corpus: "+str(console))
        report["comparison_count"]=sum(row["vectors"] for row in report["comparisons"])
    report_file.write_text(json.dumps(report, indent=2) + "\n")
    print(next(line for line in text.splitlines() if marker in line))


if __name__ == "__main__":
    main()
