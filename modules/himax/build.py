#!/usr/bin/env python3
"""Build the external Himax provider against a configured ESP32-S3 SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def build(root, sdk, output):
    sys.path.insert(0, str(root / "scripts"))
    from solaros_build_lock import acquire_project_build_lock
    acquire_project_build_lock(root, "himax-library")
    commands = json.loads((sdk / "compile_commands.json").read_text())
    template = next(c for c in commands if c["file"].endswith("/src/services/solar_os_native_media.c"))
    base = shlex.split(template["command"])
    compiler = base[0]
    if "xtensa-esp32s3-elf-gcc" not in compiler:
        raise ValueError("Himax native library requires an ESP32-S3 SDK")
    prefix = compiler[:-3]
    output.mkdir(parents=True, exist_ok=True)

    def compile_source(source):
        target = output / (source.stem + ".o")
        args = list(base)
        args[args.index("-o") + 1] = str(target)
        args[-1] = str(source)
        args += ["-O2", "-fPIC", "-fvisibility=hidden", "-ffunction-sections", "-Dmain=app_main",
                 "-I" + str(root / "include"), "-I" + str(root / "src/services"),
                 "-I" + str(HERE / "compat")]
        subprocess.run(args, cwd=template["directory"], check=True)
        return target
    objects = [compile_source(source) for source in (HERE / "provider.c", HERE / "manager.c", HERE / "library.c",
               HERE / "port.c", root / "src/services/solar_os_json_scan.c")]
    libgcc = subprocess.check_output([compiler, "-print-libgcc-file-name"], text=True).strip()
    subprocess.run([compiler, "-r", "-nostdlib", *map(str, objects), libgcc,
                    "-o", str(output / "bundle.o")], check=True)
    subprocess.run([prefix + "objcopy", "--keep-global-symbol=app_main", str(output / "bundle.o"),
                    str(output / "local.o")], check=True)
    artifact = output / "processor-himax.library.elf"
    subprocess.run([compiler, "-nostartfiles", "-nostdlib", "-shared", "-Wl,-Bsymbolic",
                    "-Wl,--gc-sections", "-Wl,-T," + str(root / "modules/espdl/layout.ld"),
                    "-Wl,-e,app_main", str(output / "local.o"), "-o", str(artifact)], check=True)
    sys.path.insert(0, str(root / "modules/espdl"))
    from verify import remove_noop_relocations, verify
    remove_noop_relocations(artifact)
    imports = verify(artifact, {"solar_os_native_library_host_v1", "memcpy", "memmove", "memset", "memcmp", "strlen"})
    subprocess.run([prefix + "strip", "--strip-unneeded", str(artifact)], check=True)
    probe = output / "himax-check.app.elf"
    probe_object = compile_source(HERE / "probe.c")
    subprocess.run([compiler, "-nostartfiles", "-nostdlib", "-shared", "-Wl,-Bsymbolic",
                    "-Wl,--gc-sections", "-Wl,-T," + str(root / "modules/espdl/layout.ld"),
                    "-Wl,-e,app_main", str(probe_object), "-o", str(probe)], check=True)
    remove_noop_relocations(probe)
    verify(probe, {"solar_os_native_host_v1", "memcpy", "memmove", "memset", "memcmp", "strlen"})
    subprocess.run([prefix + "strip", "--strip-unneeded", str(probe)], check=True)
    report = {"artifact_bytes": artifact.stat().st_size,
              "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(), "imports": imports,
              "probe_bytes": probe.stat().st_size, "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest()}
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", default="seeed_sensecap_watcher")
    parser.add_argument("--firmware-project", type=Path, default=ROOT)
    parser.add_argument("--sdk-build-dir", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / ".pio/modules/himax")
    args = parser.parse_args()
    root = args.firmware_project.resolve()
    sdk = (args.sdk_build_dir or root / ".pio/build" / args.environment).resolve()
    if not (sdk / "compile_commands.json").is_file():
        pio = os.environ.get("SOLAROS_PIO", str(Path.home() / ".platformio/penv/bin/pio"))
        subprocess.run([pio, "run", "--project-dir", str(root), "-e", args.environment, "-t", "envdump"], check=True)
    build(root, sdk, args.output.resolve())
