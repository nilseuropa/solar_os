"""Add build-and-run targets to the SolarOS virtual board environments."""

from pathlib import Path
import subprocess
import sys

Import("env")

sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "scripts"))
from solaros_qemu import ensure_qemu, prepare_flash, qemu_command
from solaros_qemu_build import ensure_input_qemu


def run_qemu(source, target, env):
    platform = env.PioPlatform()
    package = Path(platform.get_package_dir("tool-solaros-qemu"))
    framework = Path(platform.get_package_dir("framework-espidf"))
    try:
        project = Path(env.subst("$PROJECT_DIR"))
        configured = env.GetProjectOption("custom_qemu_executable", "")
        if configured:
            executable = Path(configured).expanduser()
            if not executable.is_absolute(): executable = project / executable
            if not executable.is_file(): raise RuntimeError(f"QEMU executable not found: {executable}")
        elif env["PIOENV"] == "qemu_esp32s3_rgb":
            ninja_name = "ninja.exe" if sys.platform == "win32" else "ninja"
            ninja = Path(platform.get_package_dir("tool-ninja")) / ninja_name
            executable = ensure_input_qemu(package, project, ninja)
        else:
            executable = ensure_qemu(package, framework)
        build = Path(env.subst("$BUILD_DIR"))
        images = [(int(str(offset), 0), Path(env.subst(str(path))))
                  for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
        images.append((int(env.subst("$ESP32_APP_OFFSET"), 0),
                       build / env.subst("${PROGNAME}.bin")))
        image = prepare_flash(build, images)
        print("Starting SolarOS in QEMU. UART0 console here; Ctrl+A then X exits.", flush=True)
        pointer_mode = env.GetProjectOption("custom_qemu_pointer", "touch")
        return subprocess.call(qemu_command(executable, env["PIOENV"], image, pointer_mode), cwd=build)
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
