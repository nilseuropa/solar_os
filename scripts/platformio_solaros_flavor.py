#!/usr/bin/env python3
"""Keep PlatformIO ESP-IDF builds aligned with the selected SolarOS flavor."""

from __future__ import annotations

import os
import shlex
from pathlib import Path
import shutil
import sys

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
script_dir = project_dir / "scripts"
if str(script_dir) not in sys.path:
    sys.path.insert(0, str(script_dir))

from solaros_build_lock import acquire_project_build_lock
from solaros_update_layout import select_layout
from solaros_keymap import PROFILE_DIR, load_profile, merge, read_json
from solaros_keymap_build import OPTIONS, dependencies, resolve_selection


def _selected_flavor() -> str:
    return (
        os.environ.get("SOLAR_OS_FLAVOR")
        or env.GetProjectOption("custom_solaros_default_flavor", "full")
    )


def _selected_flavor_file(project_dir: Path, flavor: str) -> Path:
    configured = os.environ.get("SOLAR_OS_FLAVOR_FILE")
    path = Path(configured) if configured else Path("flavors") / f"{flavor}.toml"
    if not path.is_absolute():
        path = project_dir / path
    path = path.resolve()
    if not path.is_file():
        raise SystemExit(f"SolarOS flavor not found: {path}")
    return path


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
    args = shlex.split(current)
    if arg not in args:
        args.append(arg)
    board_config.update("build.cmake_extra_args", shlex.join(args))


def _remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


build_dir = Path(env.subst("$BUILD_DIR"))
flavor = _selected_flavor()
flavor_name_override = os.environ.get("SOLAR_OS_FLAVOR_NAME_OVERRIDE", "")
board = _selected_board()
board_config = env.BoardConfig()
# Resolve keymap paths before IDF's component-discovery subprocesses.
cmake_options = {}
for arg in shlex.split(board_config.get("build.cmake_extra_args", "") or ""):
    if arg.startswith("-D") and "=" in arg:
        name, value = arg[2:].split("=", 1)
        cmake_options[name.split(":", 1)[0]] = value
keymap_options = {
    name: (os.environ.get(name) or env.GetProjectOption(
        "custom_solaros_" + name.removeprefix("SOLAR_OS_").lower(), "") or cmake_options.get(name, ""))
    for name in OPTIONS
}
try:
    selected_keymaps = resolve_selection(project_dir, *keymap_options.values())
    keymap_inputs = dependencies(project_dir, *keymap_options.values())
    for name, path in selected_keymaps.items():
        merge(load_profile(name), read_json(path))
except (OSError, ValueError) as exc:
    raise SystemExit(f"SolarOS keymap: {exc}") from exc
for name in ("SOLAR_OS_KEYMAP_FILE", "SOLAR_OS_KEYMAPS_FILE"):
    if keymap_options[name]:
        path = Path(keymap_options[name])
        keymap_options[name] = (project_dir / path if not path.is_absolute() else path).resolve().as_posix()
for name, value in keymap_options.items():
    os.environ[name] = value
    _append_cmake_arg(f"-D{name}={value}")
configured_partition = str(board_config.get("build.partitions", "partitions.csv"))
try:
    update_layout = select_layout(
        configured_partition,
        os.environ.get("SOLAR_OS_LAYOUT"),
    )
except ValueError as exc:
    raise SystemExit(str(exc)) from exc
layout = update_layout.key
partition_file = update_layout.partition_file
maximum_size = update_layout.app_bytes
cvbs_mode = _selected_cvbs_mode()
vga_mode = _selected_vga_mode()

# ESP-IDF runs component discovery in child CMake processes that do not always
# inherit PlatformIO's CMake cache arguments. Export the resolved selection so
# those processes cannot fall back to another board or flavor.
os.environ["SOLAR_OS_BOARD"] = board
os.environ["SOLAR_OS_FLAVOR"] = flavor
os.environ["SOLAR_OS_FLAVOR_NAME_OVERRIDE"] = flavor_name_override
os.environ["SOLAR_OS_LAYOUT"] = layout
os.environ["SOLAR_OS_CVBS_MODE"] = cvbs_mode
os.environ["SOLAR_OS_VGA_MODE"] = vga_mode

acquire_project_build_lock(project_dir, env["PIOENV"])

# PlatformIO hides successful CMake output, including the configuration guard's
# notice. Announce the impending regeneration here as well for normal pio runs.
sdkconfig_path = Path(os.path.expandvars(str(board_config.get(
    "build.esp-idf.sdkconfig_path", project_dir / f"sdkconfig.{env['PIOENV']}"
))))
if not sdkconfig_path.is_absolute():
    sdkconfig_path = project_dir / sdkconfig_path
if sdkconfig_path.is_file():
    sdkconfig_lines = sdkconfig_path.read_text(encoding="utf-8").splitlines()
    required_config = (project_dir / "patches/nimble/required_config.txt").read_text(
        encoding="utf-8"
    ).splitlines()
    missing_config = [key for key in required_config if f"{key}=y" not in sdkconfig_lines]
    if "CONFIG_BT_ENABLED=y" in sdkconfig_lines and missing_config:
        print(
            f"SolarOS BLE configuration missing {', '.join(missing_config)}; "
            f"CMake will regenerate {sdkconfig_path} "
            "from SDK configuration defaults without a backup"
        )

flavor_file = _selected_flavor_file(project_dir, flavor)
# PlatformIO parses build.cmake_extra_args with Click's POSIX-style argument
# splitter, including on Windows. Native Windows backslashes would therefore be
# consumed as escapes before CMake sees the path. CMake accepts forward slashes
# on every supported host.
flavor_file_cmake = flavor_file.as_posix()
os.environ["SOLAR_OS_FLAVOR_FILE"] = flavor_file_cmake
board_config.update("build.partitions", partition_file)
board_config.update("upload.maximum_size", maximum_size)

_append_cmake_arg(f"-DSOLAR_OS_FLAVOR={flavor}")
_append_cmake_arg(f"-DSOLAR_OS_FLAVOR_FILE={flavor_file_cmake}")
_append_cmake_arg(f"-DSOLAR_OS_FLAVOR_NAME_OVERRIDE={flavor_name_override}")
_append_cmake_arg(f"-DSOLAR_OS_LAYOUT={layout}")
_append_cmake_arg(f"-DSOLAR_OS_CVBS_MODE={cvbs_mode}")
_append_cmake_arg(f"-DSOLAR_OS_VGA_MODE={vga_mode}")
if os.environ.get("SOLAR_OS_BOARD"):
    _append_cmake_arg(f"-DSOLAR_OS_BOARD={board}")

stamp_dir = build_dir / "generated" / "solar_os"
stamp_path = stamp_dir / "platformio_build_selection.txt"
board_files = tuple(sorted((project_dir / "boards").rglob("*.cmake")))
board_manifest_files = tuple(sorted((project_dir / "boards").rglob("*.toml")))
board_headers = tuple(sorted((project_dir / "include" / "boards").glob("*.h")))
sdkconfig_default_files = tuple(sorted(project_dir.glob("sdkconfig.defaults*")))
partition_files = tuple(sorted(project_dir.glob("partitions*.csv")))
tracked_files = (
    flavor_file,
    project_dir / "version.txt",
    project_dir / "packages" / "solar_os_packages.toml",
    project_dir / "scripts" / "generate_flavor_config.py",
    project_dir / "scripts" / "platformio_solaros_flavor.py",
    project_dir / "scripts" / "cxx_sdkconfig.cmake",
    project_dir / "scripts" / "solaros_update_layout.py",
    project_dir / "scripts" / "solaros_build_lock.py",
    project_dir / "scripts" / "generate_keymaps.py",
    project_dir / "scripts" / "solaros_keymap.py",
    project_dir / "scripts" / "solaros_keymap_build.py",
    project_dir / "patches" / "nimble" / "required_config.txt",
    project_dir / "patches" / "nimble" / "config_guard.cmake",
    project_dir / "scripts" / "validate_board_metadata.py",
    project_dir / "scripts" / "generate_board_profile.py",
    project_dir / "scripts" / "solaros_board_manifest.py",
    project_dir / "src" / "CMakeLists.txt",
    project_dir / "src" / "services" / "solar_os_board_caps.h",
    project_dir / "src" / "services" / "solar_os_board_caps.c",
    project_dir / "include" / "solar_os_board.h",
    project_dir / "doc" / "manual" / "boards.md",
    project_dir / "doc" / "manual" / "expansion.reference.md",
) + board_files + board_manifest_files + board_headers + sdkconfig_default_files + partition_files \
    + tuple(sorted(PROFILE_DIR.glob("*.json"))) + keymap_inputs
stamp = (
    f"board={board}\n"
    f"flavor={flavor}\n"
    f"flavor_name_override={flavor_name_override}\n"
    f"flavor_file={flavor_file}\n"
    f"layout={layout}\n"
    f"partition={partition_file}\n"
    f"cvbs={cvbs_mode}\n"
    f"vga={vga_mode}\n"
)
for name, value in keymap_options.items():
    stamp += f"{name}={value}\n"
for tracked_file in tracked_files:
    stat = tracked_file.stat()
    try:
        display_path = tracked_file.relative_to(project_dir)
    except ValueError:
        display_path = tracked_file
    stamp += (
        f"{display_path}:"
        f"{stat.st_mtime_ns}:"
        f"{stat.st_size}\n"
    )

previous = stamp_path.read_text(encoding="utf-8") if stamp_path.exists() else ""
if previous != stamp and (previous or (build_dir / "CMakeCache.txt").exists()):
    print(
        f"SolarOS build selection changed to {board}/{flavor}/{layout} "
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
