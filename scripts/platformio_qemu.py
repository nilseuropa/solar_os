"""Add build-and-run targets to the SolarOS virtual board environments."""

from pathlib import Path
import subprocess
import sys

Import("env")

sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "scripts"))
from solaros_qemu import ensure_qemu, prepare_flash, qemu_command


def run_qemu(source, target, env):
    platform = env.PioPlatform()
    package = Path(platform.get_package_dir("tool-solaros-qemu"))
    framework = Path(platform.get_package_dir("framework-espidf"))
    try:
        executable = ensure_qemu(package, framework)
        build = Path(env.subst("$BUILD_DIR"))
        images = [(int(str(offset), 0), Path(env.subst(str(path))))
                  for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
        images.append((int(env.subst("$ESP32_APP_OFFSET"), 0),
                       build / env.subst("${PROGNAME}.bin")))
        image = prepare_flash(build, images)
        print("Starting SolarOS in QEMU. UART0 console here; Ctrl+A then X exits.", flush=True)
        if env["PIOENV"] == "qemu_esp32s3_rgb":
            print("Display keyboard: connect a VT100 terminal to 127.0.0.1:5555.", flush=True)
        return subprocess.call(qemu_command(executable, env["PIOENV"], image), cwd=build)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"SolarOS QEMU: {error}", file=sys.stderr)
        return 1


env.AddCustomTarget(
    name="qemu",
    dependencies=["$BUILD_DIR/${PROGNAME}.bin"],
    actions=[run_qemu],
    title="Run QEMU",
    description="Build SolarOS, install Espressif QEMU, and run the virtual board",
)
