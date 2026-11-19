"""Managed Espressif QEMU installation, virtual flash, and launch arguments."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


FLASH_BYTES = 16 * 1024 * 1024
QEMU_BOARDS = ("qemu_esp32s3", "qemu_esp32s3_rgb")


def ensure_qemu(package: Path, framework: Path) -> Path:
    manifest = package / "tools.json"
    tool = json.loads(manifest.read_text())["tools"][0]
    version = next(v["name"] for v in tool["versions"] if v["status"] == "recommended")
    cache = package / "cache"
    executable = cache / "tools" / tool["name"] / version / "qemu" / "bin" / (
        "qemu-system-xtensa.exe" if os.name == "nt" else "qemu-system-xtensa")
    if not executable.is_file():
        print(f"Installing Espressif QEMU {version} in PlatformIO's tool cache", flush=True)
        subprocess.run([
            sys.executable, str(framework / "tools" / "idf_tools.py"),
            "--non-interactive", "--tools-json", str(manifest), "install", tool["name"],
        ], env=dict(os.environ, IDF_TOOLS_PATH=str(cache), IDF_PATH=str(framework)), check=True)
    if not executable.is_file():
        raise RuntimeError(f"QEMU installation did not produce {executable}")
    return executable


def prepare_flash(build: Path, images: list[tuple[int, Path]]) -> Path:
    """Reuse guest-written flash until the build's flash inputs change."""
    digest = hashlib.sha256()
    previous_end = 0
    for offset, path in sorted(images):
        data = path.read_bytes()
        if offset < previous_end or offset + len(data) > FLASH_BYTES:
            raise ValueError(f"Invalid or overlapping QEMU flash image: {path} at {offset:#x}")
        digest.update(f"{offset}:{len(data)}:".encode())
        digest.update(data)
        previous_end = offset + len(data)

    image = build / "flash_image.bin"
    stamp = build / "flash_image.sha256"
    fingerprint = digest.hexdigest()
    if (image.is_file() and image.stat().st_size == FLASH_BYTES and
            stamp.is_file() and stamp.read_text().strip() == fingerprint):
        return image

    temporary = image.with_suffix(".bin.tmp")
    with temporary.open("wb") as output:
        erased = b"\xff" * (64 * 1024)
        for _ in range(FLASH_BYTES // len(erased)):
            output.write(erased)
        for offset, path in images:
            output.seek(offset)
            output.write(path.read_bytes())
    temporary.replace(image)
    stamp.write_text(fingerprint + "\n")
    print(f"Prepared fresh virtual flash: {image}", flush=True)
    return image


def qemu_command(executable: Path, board: str, image: Path) -> list[str]:
    if board not in QEMU_BOARDS:
        raise ValueError(f"Not a QEMU board: {board}")
    graphical = board == "qemu_esp32s3_rgb"
    # QEMU's option parser escapes literal commas by doubling them.
    flash = str(image).replace(",", ",,")
    log = str(image.parent / "uart0.log").replace(",", ",,")
    command = [
        str(executable), "-machine", "esp32s3", "-m", "8M",
        "-global", "driver=ssi_psram,property=is_octal,value=true",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-display", "sdl" if graphical else "none",
        "-monitor", "none",
        "-chardev", f"stdio,id=console,mux=on,signal=off,logfile={log}",
        "-serial", "chardev:console", "-mon", "chardev=console,mode=readline",
    ]
    if graphical:
        command += ["-serial", "tcp:127.0.0.1:5555,server=on,wait=off",
                    "-nic", "user,model=open_eth"]
    else:
        command += ["-nic", "none"]
    return command
