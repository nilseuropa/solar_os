#!/usr/bin/env python3
"""Keep PlatformIO ESP-IDF builds aligned with the selected SolarOS flavor."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import sys

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
script_dir = project_dir / "scripts"
if str(script_dir) not in sys.path:
    sys.path.insert(0, str(script_dir))

from solaros_build_lock import acquire_project_build_lock


def _selected_flavor() -> str:
    return (
        os.environ.get("SOLAR_OS_FLAVOR")
        or env.GetProjectOption("custom_solaros_default_flavor", "full")
    )


def _selected_board() -> str:
    return os.environ.get("SOLAR_OS_BOARD") or env["PIOENV"]


def _selected_cvbs_mode() -> str:
    mode = os.environ.get("SOLAR_OS_CVBS_MODE") or "384x288"
    if mode not in ("384x288", "320x200"):
        raise SystemExit(
            "Unsupported SOLAR_OS_CVBS_MODE "
            f"{mode!r}; expected '384x288' or '320x200'"
        )
    return mode


def _selected_vga_mode() -> str:
    mode = os.environ.get("SOLAR_OS_VGA_MODE") or "640x480"
    if mode not in ("320x200", "320x240", "640x400", "640x480"):
        raise SystemExit(
            "Unsupported SOLAR_OS_VGA_MODE "
            f"{mode!r}; expected '320x200', '320x240', '640x400', or '640x480'"
        )
    return mode


def _append_cmake_arg(arg: str) -> None:
    board_config = env.BoardConfig()
    current = board_config.get("build.cmake_extra_args", "") or ""
    args = current.split()
    if arg not in args:
        args.append(arg)
    board_config.update("build.cmake_extra_args", " ".join(args))


def _remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


build_dir = Path(env.subst("$BUILD_DIR"))
flavor = _selected_flavor()
board = _selected_board()
cvbs_mode = _selected_cvbs_mode()
vga_mode = _selected_vga_mode()

acquire_project_build_lock(project_dir, env["PIOENV"])

flavor_file = project_dir / "flavors" / f"{flavor}.toml"
if not flavor_file.exists():
    raise SystemExit(f"SolarOS flavor not found: {flavor_file}")

_append_cmake_arg(f"-DSOLAR_OS_FLAVOR={flavor}")
_append_cmake_arg(f"-DSOLAR_OS_CVBS_MODE={cvbs_mode}")
_append_cmake_arg(f"-DSOLAR_OS_VGA_MODE={vga_mode}")
if os.environ.get("SOLAR_OS_BOARD"):
    _append_cmake_arg(f"-DSOLAR_OS_BOARD={board}")

stamp_dir = build_dir / "generated" / "solar_os"
stamp_path = stamp_dir / "platformio_build_selection.txt"
board_files = tuple(sorted((project_dir / "boards").rglob("*.cmake")))
board_headers = tuple(sorted((project_dir / "include" / "boards").glob("*.h")))
sdkconfig_default_files = tuple(sorted(project_dir.glob("sdkconfig.defaults*")))
tracked_files = (
    flavor_file,
    project_dir / "packages" / "solar_os_packages.toml",
    project_dir / "scripts" / "generate_flavor_config.py",
    project_dir / "scripts" / "platformio_solaros_flavor.py",
    project_dir / "scripts" / "solaros_build_lock.py",
    project_dir / "scripts" / "validate_board_metadata.py",
    project_dir / "src" / "CMakeLists.txt",
    project_dir / "src" / "services" / "solar_os_board_caps.h",
    project_dir / "src" / "services" / "solar_os_board_caps.c",
    project_dir / "include" / "solar_os_board.h",
    project_dir / "doc" / "manual" / "boards.md",
    project_dir / "doc" / "manual" / "expansion.reference.md",
) + board_files + board_headers + sdkconfig_default_files
stamp = (
    f"board={board}\n"
    f"flavor={flavor}\n"
    f"cvbs={cvbs_mode}\n"
    f"vga={vga_mode}\n"
)
for tracked_file in tracked_files:
    stat = tracked_file.stat()
    stamp += (
        f"{tracked_file.relative_to(project_dir)}:"
        f"{stat.st_mtime_ns}:"
        f"{stat.st_size}\n"
    )

previous = stamp_path.read_text(encoding="utf-8") if stamp_path.exists() else ""
if previous != stamp and (previous or (build_dir / "CMakeCache.txt").exists()):
    print(
        f"SolarOS build selection changed to {board}/{flavor} "
        f"(CVBS {cvbs_mode}, VGA {vga_mode}); reconfiguring CMake"
    )
    for entry in (
        build_dir / "CMakeCache.txt",
        build_dir / "build.ninja",
        build_dir / "cmake_install.cmake",
        build_dir / "memory.ld",
        build_dir / "sections.ld",
        build_dir / "esp-idf" / "esp_system" / "ld" / "memory.ld.in",
        build_dir / "esp-idf" / "esp_system" / "ld" / "sections.ld.in",
        build_dir / "CMakeFiles",
        build_dir / ".cmake",
    ):
        _remove_path(entry)

stamp_dir.mkdir(parents=True, exist_ok=True)
stamp_path.write_text(stamp, encoding="utf-8")
