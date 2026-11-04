#!/usr/bin/env python3
"""Build the C robot library against an existing ESP32-S3 firmware SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def prepare(environment):
    build_root = ROOT / ".pio/robot-sdk/build"
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOLAR_OS_")}
    env.update(PLATFORMIO_BUILD_DIR=str(build_root), SOLAR_OS_FLAVOR="full",
               SOLAR_OS_FLAVOR_FILE=str(ROOT / "flavors/full.toml"), SOLAR_OS_LAYOUT="ota")
    pio = os.environ.get("SOLAROS_PIO", str(Path.home() / ".platformio/penv/bin/pio"))
    log = ROOT / ".pio/robot-sdk/configure.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as stream:
        subprocess.run([pio, "run", "--project-dir", str(ROOT), "-e", environment, "-t", "envdump"],
                       env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    return build_root / environment


def build(sdk, output):
    sys.path.insert(0, str(ROOT / "scripts"))
    from solaros_build_lock import acquire_project_build_lock
    acquire_project_build_lock(ROOT, "robot-library")
    commands = json.loads((sdk / "compile_commands.json").read_text())
    template = next(c for c in commands if c["file"].endswith("/src/services/solar_os_motor.c"))
    base = shlex.split(template["command"])
    compiler = base[0]
    if "xtensa-esp32s3-elf-gcc" not in compiler:
        raise ValueError("robot requires an ESP32-S3 SDK")
    prefix = compiler[:-3]
    output.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in (HERE / "main/robot.c", HERE / "main/model.c", HERE / "main/transform.c",
                   HERE / "main/library.c"):
        target = output / (source.stem + ".o")
        args = list(base)
        args[args.index("-o") + 1] = str(target)
        args[-1] = str(source)
        args += ["-O2", "-fPIC", "-fvisibility=hidden", "-Dmain=app_main",
                 "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/services")]
        subprocess.run(args, cwd=template["directory"], check=True)
        objects.append(target)
    archives = [subprocess.check_output([compiler, "-print-file-name=" + a], text=True).strip()
                for a in ("libm.a", "libgcc.a")]
    subprocess.run([compiler, "-r", "-nostdlib", *map(str, objects), "-Wl,--start-group", *archives,
                    "-Wl,--end-group", "-o", str(output / "bundle.o")], check=True)
    subprocess.run([prefix + "objcopy", "--keep-global-symbol=app_main", str(output / "bundle.o"), str(output / "local.o")], check=True)
    layout = ROOT / "modules/espdl/layout.ld"
    debug = output / "robot.debug.elf"
    subprocess.run([compiler, "-nostartfiles", "-nostdlib", "-shared", "-Wl,-Bsymbolic",
                    "-Wl,-T," + str(layout), "-Wl,-e,app_main", str(output / "local.o"), "-o", str(debug)], check=True)
    sys.path.insert(0, str(ROOT / "modules/espdl"))
    from verify import remove_noop_relocations, verify
    artifact = output / "robot.library.elf"
    shutil.copyfile(debug, artifact)
    remove_noop_relocations(artifact)
    imports = verify(artifact, {"solar_os_native_library_host_v1", "memcpy", "memmove", "memset", "memcmp", "strlen"})
    subprocess.run([prefix + "strip", "--strip-unneeded", str(artifact)], check=True)
    report = {"artifact_bytes": artifact.stat().st_size, "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(), "imports": imports}
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", default="esp32_s3_devkitc1_n16r8")
    parser.add_argument("--firmware-project", type=Path, default=ROOT)
    parser.add_argument("--sdk-build-dir", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / ".pio/modules/robot")
    args = parser.parse_args()
    ROOT = args.firmware_project.resolve()
    build((args.sdk_build_dir or prepare(args.environment)).resolve(), args.output.resolve())
