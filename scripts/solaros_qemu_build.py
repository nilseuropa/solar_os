"""Build the pinned QEMU extensions for native input and host storage."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess


QEMU_COMMIT = "7411393a2f61e6f0b05af00276d30780769e0839"
QEMU_TAG = "esp-develop-9.2.2-20261109"


def ensure_input_qemu(package: Path, project: Path, ninja: Path) -> Path:
    overlay = project / "tools/qemu-xtensa"
    protocols = sorted((project / "include").glob("solar_os_qemu_*_protocol.h"))
    cache = package / "cache/solar-input"
    source, build, installed = (cache / name for name in ("source", "build", "installed"))
    executable = installed / "bin/qemu-system-xtensa"
    stamp = cache / "source.sha256"
    inputs = [overlay / "solar-input.patch", overlay / "xtensa-softmmu.mak", *protocols,
              Path(__file__), *sorted((overlay / "overlay").rglob("*.c"))]
    digest = hashlib.sha256((QEMU_COMMIT + platform.system() + platform.machine()).encode())
    for path in inputs:
        digest.update(path.read_bytes())
    fingerprint = digest.hexdigest()
    if executable.is_file() and stamp.is_file() and stamp.read_text().strip() == fingerprint:
        return executable
    if os.name == "nt":
        raise RuntimeError("Native QEMU input currently builds on Linux/macOS; use WSL on Windows")
    environment = dict(os.environ)
    environment["PATH"] = str(ninja.parent) + os.pathsep + environment.get("PATH", "")
    # An optional private prefix also permits validation without modifying the host OS.
    dependencies = package / "cache/build-deps/usr"
    if dependencies.is_dir():
        paths = [*dependencies.glob("lib/*/pkgconfig"), dependencies / "lib/pkgconfig",
                 dependencies / "share/pkgconfig"]
        environment["PKG_CONFIG_PATH"] = os.pathsep.join(map(str, paths)) + os.pathsep + environment.get("PKG_CONFIG_PATH", "")
        includes = [dependencies / "include", *sorted((dependencies / "include").glob("*-linux-gnu"))]
        environment["C_INCLUDE_PATH"] = os.pathsep.join(map(str, includes)) + os.pathsep + environment.get("C_INCLUDE_PATH", "")
    required = ("glib-2.0", "pixman-1", "sdl2", "slirp", "libgcrypt")
    missing = [name for name in required if subprocess.run(
        ["pkg-config", "--exists", name], env=environment, check=False).returncode]
    if missing:
        raise RuntimeError("QEMU build dependencies missing: " + ", ".join(missing) +
                           ". Install the development libraries listed in doc/manual/qemu.md")
    cache.mkdir(parents=True, exist_ok=True)
    if not (source / ".git").is_dir():
        subprocess.run(["git", "clone", "--depth", "1", "--branch", QEMU_TAG,
                        "https://github.com/espressif/qemu.git", str(source)], check=True)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if commit != QEMU_COMMIT:
        raise RuntimeError(f"Unexpected QEMU source revision: {commit}")
    # Only these managed source files are reset before reapplying the overlay.
    patched = ["hw/input/meson.build", "hw/misc/meson.build", "hw/xtensa/esp32s3.c", "include/ui/input.h",
               "ui/input.c", "ui/sdl2.c"]
    subprocess.run(["git", "restore", "--source=" + QEMU_COMMIT, "--", *patched], cwd=source, check=True)
    subprocess.run(["git", "apply", str(overlay / "solar-input.patch")], cwd=source, check=True)
    for path in (overlay / "overlay").rglob("*.c"):
        target = source / path.relative_to(overlay / "overlay")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    for protocol in protocols:
        shutil.copyfile(protocol, source / "include" / protocol.name)
    shutil.copyfile(overlay / "xtensa-softmmu.mak", source / "configs/devices/xtensa-softmmu/default.mak")
    build.mkdir(parents=True, exist_ok=True)
    print("Building Espressif QEMU with SolarOS input and host storage (cached after the first build)", flush=True)
    subprocess.run([
        str(source / "configure"), "--target-list=xtensa-softmmu", "--without-default-features",
        "--enable-tcg", "--enable-sdl", "--enable-pixman", "--enable-slirp", "--enable-gcrypt",
        "--disable-docs", "--disable-werror", "--disable-debug-info", "--disable-tools",
        "--with-pkgversion=esp_develop_9.2.2_20261109-solar-os2",
        "--prefix=" + str(installed), "--ninja=" + str(ninja),
    ], cwd=build, env=environment, check=True)
    subprocess.run([str(ninja), "-j", str(min(os.cpu_count() or 2, 8)), "install"],
                   cwd=build, env=environment, check=True)
    if not executable.is_file():
        raise RuntimeError(f"QEMU build did not produce {executable}")
    stamp.write_text(fingerprint + "\n")
    return executable
