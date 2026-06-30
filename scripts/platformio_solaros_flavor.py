#!/usr/bin/env python3
"""Keep PlatformIO ESP-IDF builds aligned with the selected SolarOS flavor."""

from __future__ import annotations

import os
from pathlib import Path
import shutil

Import("env")


def _selected_flavor() -> str:
    return os.environ.get("SOLAR_OS_FLAVOR") or "full"


def _selected_board() -> str:
    return os.environ.get("SOLAR_OS_BOARD") or env["PIOENV"]


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


project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
flavor = _selected_flavor()
board = _selected_board()

flavor_file = project_dir / "flavors" / f"{flavor}.toml"
if not flavor_file.exists():
    raise SystemExit(f"SolarOS flavor not found: {flavor_file}")

_append_cmake_arg(f"-DSOLAR_OS_FLAVOR={flavor}")
if os.environ.get("SOLAR_OS_BOARD"):
    _append_cmake_arg(f"-DSOLAR_OS_BOARD={board}")

stamp_dir = build_dir / "generated" / "solar_os"
stamp_path = stamp_dir / "platformio_build_selection.txt"
board_files = tuple(sorted((project_dir / "boards").rglob("*.cmake")))
tracked_files = (
    flavor_file,
    project_dir / "packages" / "solar_os_packages.toml",
    project_dir / "scripts" / "generate_flavor_config.py",
    project_dir / "src" / "CMakeLists.txt",
) + board_files
stamp = f"board={board}\nflavor={flavor}\n"
for tracked_file in tracked_files:
    stat = tracked_file.stat()
    stamp += (
        f"{tracked_file.relative_to(project_dir)}:"
        f"{stat.st_mtime_ns}:"
        f"{stat.st_size}\n"
    )

previous = stamp_path.read_text(encoding="utf-8") if stamp_path.exists() else ""
if previous != stamp and (previous or (build_dir / "CMakeCache.txt").exists()):
    print(f"SolarOS build selection changed to {board}/{flavor}; reconfiguring CMake")
    for entry in (
        build_dir / "CMakeCache.txt",
        build_dir / "build.ninja",
        build_dir / "cmake_install.cmake",
        build_dir / "CMakeFiles",
        build_dir / ".cmake",
    ):
        _remove_path(entry)

stamp_dir.mkdir(parents=True, exist_ok=True)
stamp_path.write_text(stamp, encoding="utf-8")
