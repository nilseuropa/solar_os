"""Add build-and-run targets to the SolarOS virtual board environments."""

from pathlib import Path
import subprocess
import sys

Import("env")

sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "scripts"))
from solaros_qemu import (DEFAULT_UART0_MODE, QEMU_GRAPHICAL_BOARDS, ensure_qemu, launch_qemu,
                          prepare_audio_backend, prepare_flash, prepare_host_storage, prepare_uart0, qemu_command)
from solaros_qemu_build import ensure_input_qemu


def run_qemu(source, target, env):
    platform = env.PioPlatform()
    package = Path(platform.get_package_dir("tool-solaros-qemu"))
    framework = Path(platform.get_package_dir("framework-espidf"))
    try:
        project = Path(env.subst("$PROJECT_DIR"))
        storage = prepare_host_storage(project, env.GetProjectOption(
            "custom_qemu_storage", "none" if sys.platform == "win32" else "qemu-storage"))
        audio = prepare_audio_backend(project, env.GetProjectOption(
            "custom_qemu_audio", "off" if sys.platform == "win32" else "sdl"))
        configured = env.GetProjectOption("custom_qemu_executable", "")
        if configured:
            executable = Path(configured).expanduser()
            if not executable.is_absolute(): executable = project / executable
            if not executable.is_file(): raise RuntimeError(f"QEMU executable not found: {executable}")
        elif env["PIOENV"] in QEMU_GRAPHICAL_BOARDS or storage is not None or audio != "off":
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
        pointer_mode = env.GetProjectOption("custom_qemu_pointer", "touch")
        uart0_mode = env.GetProjectOption("custom_qemu_uart0", DEFAULT_UART0_MODE)
        zoom = int(env.GetProjectOption("custom_qemu_zoom", "2"))
        command = qemu_command(executable, env["PIOENV"], image, pointer_mode, uart0_mode, storage, audio, zoom)
        prepare_uart0(build, uart0_mode)
        if storage is not None:
            print(f"SolarOS /host directory: {storage}", flush=True)
        if audio != "off": print(f"SolarOS audio output: {audio}", flush=True)
        if uart0_mode == "pty":
            print(f"SolarOS UART0 serial device: {build / 'uart0'}", flush=True)
            print("Open UART0 in a serial terminal. QEMU monitor here: 'quit' exits, 'system_reset' reboots.", flush=True)
        else:
            print("Starting SolarOS in QEMU. UART0 console here; Ctrl+A then X exits.", flush=True)
        return launch_qemu(command, build)
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
