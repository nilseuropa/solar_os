#!/usr/bin/env python3
"""Build, configure, and flash SolarOS firmware in a terminal UI."""

from __future__ import annotations

import argparse
from collections import deque
import configparser
from dataclasses import dataclass
import curses
import hashlib
import os
from pathlib import Path
import re
import select
import signal
import statistics
import subprocess
import sys
import tomllib
from typing import Iterable

from generate_flavor_config import (
    apply_board_capability_pruning,
    apply_target_pruning,
    capabilities_supported,
    DEFAULT_PACKAGE_CATALOG,
    PackageCatalog,
    load_catalog,
    load_flavor,
    write_if_changed,
)
from solaros_board_manifest import (
    load_board_manifest,
    load_driver_catalog,
    required_packages as manifest_required_packages,
)
from solaros_update_layout import UpdateLayout as LayoutContext, layouts_for_flash


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "flavors" / "core.toml"
DEFAULT_OUTPUT = ROOT / "flavors" / "custom.toml"
BUILD_ROOT = ROOT / ".pio" / "build"
MANIFEST_DIR = ROOT / "boards" / "manifests"
DRIVER_CATALOG = ROOT / "boards" / "expansion_drivers.toml"
PLATFORMIO_INI = ROOT / "platformio.ini"
BUILDER_DIR = ROOT / ".pio" / "os_builder"
BUILDER_FLAVOR = BUILDER_DIR / "current.toml"
PIO = Path("/home/nils/.platformio/penv/bin/pio")
LAYOUT_PACKAGE = "service_ota"


def _quoted(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def dependency_closure(catalog: PackageCatalog, packages: Iterable[str]) -> set[str]:
    result = set(packages)
    pending = list(result)
    while pending:
        package = pending.pop()
        for dependency in catalog.package_defs[package].depends:
            if dependency not in result:
                result.add(dependency)
                pending.append(dependency)
    return result


def immutable_packages(catalog: PackageCatalog) -> set[str]:
    members = {
        package
        for group in catalog.group_defs.values()
        if group.immutable
        for package in group.members
    }
    return dependency_closure(catalog, members)


def load_requested_packages(path: Path, catalog: PackageCatalog) -> set[str]:
    """Load explicit group/member choices without confusing dependencies with choices."""
    with path.open("rb") as file:
        data = tomllib.load(file)
    raw_groups = data.get("groups", data.get("package_groups", {}))
    raw_packages = data.get("packages", {})
    requested: set[str] = set()

    for table in (raw_groups, raw_packages):
        if not isinstance(table, dict):
            raise ValueError("flavor package selections must be TOML tables")
        for name, enabled in table.items():
            if name in catalog.group_defs and bool(enabled):
                requested.update(catalog.group_defs[name].members)

    if isinstance(raw_packages, dict):
        for name, enabled in raw_packages.items():
            if name not in catalog.package_defs:
                continue
            if bool(enabled):
                requested.add(name)
            else:
                requested.discard(name)
    return requested - immutable_packages(catalog)


class SelectionModel:
    """Track user choices separately from automatically selected dependencies."""

    def __init__(self,
                 catalog: PackageCatalog,
                 requested: Iterable[str] = (),
                 board_required: Iterable[str] = (),
                 target: str = "",
                 capabilities: Iterable[str] | None = None,
                 layout: str | None = None):
        self.catalog = catalog
        self.target = target
        self.capabilities = set(capabilities) if capabilities is not None else None
        if layout not in (None, "ota", "single"):
            raise ValueError(f"unknown update layout: {layout}")
        self.layout = layout
        self.board_required = set(board_required)
        layout_required = {LAYOUT_PACKAGE} if layout == "ota" else set()
        self._mandatory_unpruned = dependency_closure(
            catalog,
            immutable_packages(catalog) | self.board_required | layout_required,
        )
        self.mandatory = self._resolve(self._mandatory_unpruned)
        unavailable_required = self.board_required - self.mandatory
        if unavailable_required:
            raise ValueError(
                "board-required package(s) unavailable: "
                + ", ".join(sorted(unavailable_required))
            )
        # The physical update layout owns OTA selection. Normalize legacy flavor
        # files which exposed it as a group or package.
        self.requested = set(requested) - {LAYOUT_PACKAGE}

    def _resolve(self, packages: Iterable[str]) -> set[str]:
        result = dependency_closure(self.catalog, packages)
        if not self.target or self.capabilities is None:
            return result
        enabled = {package: package in result for package in self.catalog.packages}
        enabled = apply_target_pruning(self.catalog, enabled, self.target)
        groups = {group: False for group in self.catalog.groups}
        _, enabled = apply_board_capability_pruning(
            self.catalog,
            groups,
            enabled,
            self.capabilities,
        )
        return {package for package, value in enabled.items() if value}

    @property
    def selected(self) -> set[str]:
        return self._resolve(self.requested | self._mandatory_unpruned)

    def preview_enable(self, packages: Iterable[str]) -> set[str]:
        return self._resolve(self.requested | self._mandatory_unpruned | set(packages))

    def preview_disable(self, packages: Iterable[str]) -> set[str]:
        return self._resolve(
            (self.requested - set(packages)) | self._mandatory_unpruned
        )

    def enable(self, packages: Iterable[str]) -> None:
        self.requested.update(packages)

    def disable(self, packages: Iterable[str]) -> None:
        self.requested.difference_update(packages)

    def toggle_package(self, package: str) -> None:
        if package in self.mandatory:
            return
        if package in self.requested:
            self.disable({package})
        else:
            self.enable({package})

    def group_state(self, members: Iterable[str]) -> int:
        members_set = set(members)
        if not members_set:
            return 0
        selected_count = len(members_set & self.requested)
        if selected_count == 0:
            return 0
        if selected_count == len(members_set):
            return 2
        return 1

    def toggle_group(self, members: Iterable[str]) -> None:
        members_set = set(members)
        if self.group_state(members_set) == 2:
            self.disable(members_set)
        else:
            self.enable(members_set)

    def select_all(self) -> None:
        for group in visible_groups(self.catalog):
            if self.group_supported(group):
                self.enable(group.members)

    def clear_optional(self) -> None:
        self.requested.clear()

    def group_supported(self, group) -> bool:
        if not self.target or self.capabilities is None:
            return True
        if not capabilities_supported(
            group.capabilities,
            group.any_capabilities,
            self.capabilities,
        ):
            return False
        preview = self.preview_enable(group.members)
        return set(group.members) <= preview

    def group_locked(self, group) -> bool:
        return bool(group.members) and set(group.members) <= self.mandatory

    def unsupported_reason(self, group) -> str:
        if self.capabilities is None:
            return ""
        closure = dependency_closure(self.catalog, group.members)
        if self.target:
            wrong_target = sorted(
                package
                for package in closure
                if self.catalog.package_defs[package].targets
                and self.target not in self.catalog.package_defs[package].targets
            )
            if wrong_target:
                return f"requires another MCU target ({wrong_target[0]})"
        missing = sorted({
            capability
            for package in closure
            for capability in self.catalog.package_defs[package].capabilities
            if capability not in self.capabilities
        } | {
            capability
            for capability in group.capabilities
            if capability not in self.capabilities
        })
        if missing:
            return "requires " + ", ".join(missing)
        alternatives = [
            self.catalog.package_defs[package].any_capabilities
            for package in closure
            if self.catalog.package_defs[package].any_capabilities
            and not any(
                capability in self.capabilities
                for capability in self.catalog.package_defs[package].any_capabilities
            )
        ]
        if alternatives:
            return "requires one of " + ", ".join(alternatives[0])
        return "not supported by this board"


def render_flavor(name: str,
                  description: str,
                  catalog: PackageCatalog,
                  model: SelectionModel) -> str:
    """Render a compact flavor which reproduces the model's explicit choices."""
    complete_groups = [
        group
        for group in catalog.groups
        if not catalog.group_defs[group].immutable
        and not catalog.group_defs[group].hidden
        and catalog.group_defs[group].members
        and set(catalog.group_defs[group].members) <= model.requested
    ]
    covered = {
        package
        for group in complete_groups
        for package in catalog.group_defs[group].members
    }
    individual = [
        package
        for package in catalog.packages
        if package in model.requested and package not in covered
    ]

    lines = [
        "[flavor]",
        f"name = {_quoted(name)}",
        f"description = {_quoted(description)}",
        "",
        "[groups]",
    ]
    lines.extend(f"{group} = true" for group in complete_groups)
    if individual:
        lines.extend(["", "[packages]"])
        lines.extend(f"{package} = true" for package in individual)
    return "\n".join(lines).rstrip() + "\n"


def visible_groups(catalog: PackageCatalog):
    return [
        catalog.group_defs[name]
        for name in catalog.groups
        if not catalog.group_defs[name].hidden
    ]


def group_names_by_category(catalog: PackageCatalog) -> list[tuple[str, tuple[str, ...]]]:
    categories: dict[str, list[str]] = {}
    for name in catalog.groups:
        group = catalog.group_defs[name]
        if group.hidden:
            continue
        categories.setdefault(group.category, []).append(name)
    return [(category, tuple(names)) for category, names in categories.items()]


def available_layouts(flash_bytes: int) -> tuple[LayoutContext, ...]:
    return layouts_for_flash(flash_bytes)


@dataclass(frozen=True)
class BoardContext:
    board_id: str
    name: str
    target: str
    environment: str
    capabilities: frozenset[str]
    psram_bytes: int
    flash_bytes: int
    layouts: tuple[LayoutContext, ...]
    required_packages: frozenset[str]

    @property
    def app_bytes(self) -> int:
        """Compatibility alias for the default update layout's application slot."""
        return self.layouts[0].app_bytes

    @property
    def build_dir(self) -> Path:
        return BUILD_ROOT / self.environment


def _base_manifest_id(path: Path, manifest_dir: Path) -> str:
    current = path
    seen: set[Path] = set()
    while True:
        current = current.resolve()
        if current in seen:
            raise ValueError(f"board manifest inheritance cycle at {current}")
        seen.add(current)
        with current.open("rb") as file:
            raw = tomllib.load(file)
        parent = raw.get("extends")
        if not parent:
            return str(raw["board"]["id"])
        current = manifest_dir / f"{parent}.toml"


def _partition_limits(path: Path) -> tuple[int, int]:
    flash_bytes = 0
    app_bytes = 0
    size_pattern = re.compile(r"#\s*(\d+)\s*MB\s+flash", re.IGNORECASE)
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        match = size_pattern.search(raw_line)
        if match:
            flash_bytes = int(match.group(1)) * 1024 * 1024
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[1] == "app":
            app_bytes = max(app_bytes, int(fields[4], 0))
    if not flash_bytes or not app_bytes:
        raise ValueError(f"could not determine flash/application size from {path}")
    return flash_bytes, app_bytes


def _fragment_required_packages(fragments: Iterable[str]) -> set[str]:
    result: set[str] = set()
    declaration = re.compile(
        r"list\s*\(\s*APPEND\s+SOLAR_OS_BOARD_REQUIRED_PACKAGES(.*?)\)",
        re.DOTALL,
    )
    for fragment in fragments:
        path = ROOT / "boards" / "drivers" / f"{fragment}.cmake"
        if not path.is_file():
            continue
        for body in declaration.findall(path.read_text(encoding="utf-8")):
            result.update(re.findall(r"\b(?:driver|expansion)_[a-z0-9_]+\b", body))
    return result


def load_board_contexts(
    manifest_dir: Path = MANIFEST_DIR,
    platformio_ini: Path = PLATFORMIO_INI,
) -> list[BoardContext]:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(platformio_ini, encoding="utf-8")
    drivers = load_driver_catalog(DRIVER_CATALOG)
    contexts: list[BoardContext] = []
    for path in sorted(manifest_dir.glob("*.toml")):
        board = load_board_manifest(path, manifest_dir)
        base_id = _base_manifest_id(path, manifest_dir)
        section_name = f"env:{base_id}"
        if section_name not in parser:
            continue
        partition_name = parser[section_name].get(
            "board_build.partitions",
            parser["env"].get("board_build.partitions", "partitions.csv"),
        )
        flash_bytes, _ = _partition_limits(ROOT / partition_name)
        build = board["build"]
        required = set(manifest_required_packages(board, drivers))
        required.update(_fragment_required_packages(build.get("drivers", [])))
        contexts.append(BoardContext(
            board_id=str(board["board"]["id"]),
            name=str(board["board"]["name"]),
            target=str(board["target"]["mcu"]),
            environment=base_id,
            capabilities=frozenset(build.get("capabilities", [])),
            psram_bytes=int(build.get("psram_bytes", 0)),
            flash_bytes=flash_bytes,
            layouts=available_layouts(flash_bytes),
            required_packages=frozenset(required),
        ))
    return contexts


def format_size(value: int) -> str:
    if value >= 1024 * 1024:
        return f"{value / (1024 * 1024):.2f} MiB"
    if value >= 1024:
        return f"{value / 1024:.1f} KiB"
    return f"{value} B"


def top_bar_text(selected_count: int,
                 package_count: int,
                 image_size: int,
                 bootstrap_size: int) -> str:
    optional_size = max(0, image_size - bootstrap_size)
    return (
        f" SolarOS Builder | image ~{format_size(image_size)} | "
        f"{selected_count}/{package_count} choices | "
        f"+{format_size(optional_size)} optional "
    )


def _best_build_dir(build_root: Path, catalog: PackageCatalog) -> Path | None:
    if not build_root.is_dir():
        return None
    sources = {
        source
        for package_def in catalog.package_defs.values()
        for source in package_def.sources
    }
    candidates: list[tuple[int, float, Path]] = []
    for path in build_root.iterdir():
        if not path.is_dir():
            continue
        coverage = sum((path / "src" / f"{source}.o").is_file() for source in sources)
        firmware = path / "firmware.elf"
        if coverage == 0 and not firmware.is_file():
            continue
        modified = firmware.stat().st_mtime if firmware.is_file() else path.stat().st_mtime
        candidates.append((coverage, modified, path))
    return max(candidates)[2] if candidates else None


def _size_tool(build_dir: Path) -> Path | None:
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_OBJDUMP:FILEPATH="):
                objdump = Path(line.split("=", 1)[1])
                candidate = objdump.with_name(objdump.name.removesuffix("-objdump") + "-size")
                if candidate.is_file():
                    return candidate
    return None


_SIZE_LINE = re.compile(
    r"^\s*(\d+)\s+(\d+)\s+\d+\s+\d+\s+[0-9a-fA-F]+\s+(.+?)\s*$"
)
_ARCHIVE_MEMBER = re.compile(r"^.+ \(ex (.+\.a)\)$")


def _measure_artifacts(tool: Path, artifacts: Iterable[Path]) -> dict[Path, int]:
    artifact_list = sorted({path.resolve() for path in artifacts if path.is_file()})
    if not artifact_list:
        return {}
    result = subprocess.run(
        [str(tool), *map(str, artifact_list)],
        check=True,
        capture_output=True,
        text=True,
    )
    sizes: dict[Path, int] = {}
    for line in result.stdout.splitlines():
        match = _SIZE_LINE.match(line)
        if match is None:
            continue
        flash_bytes = int(match.group(1)) + int(match.group(2))
        filename = match.group(3)
        archive_match = _ARCHIVE_MEMBER.match(filename)
        path = Path(archive_match.group(1) if archive_match else filename).resolve()
        sizes[path] = sizes.get(path, 0) + flash_bytes
    return sizes


@dataclass(frozen=True)
class FlashEstimator:
    catalog: PackageCatalog
    source_sizes: dict[str, int]
    requirement_sizes: dict[str, int]
    provenance: str

    @classmethod
    def create(cls,
               catalog: PackageCatalog,
               build_dir: Path | None = None,
               root: Path = ROOT) -> "FlashEstimator":
        selected_build = build_dir or _best_build_dir(BUILD_ROOT, catalog)
        object_paths: dict[str, Path] = {}
        requirement_paths: dict[str, tuple[Path, ...]] = {}
        measured: dict[Path, int] = {}
        tool: Path | None = None

        if selected_build is not None:
            selected_build = selected_build.resolve()
            for package_def in catalog.package_defs.values():
                for source in package_def.sources:
                    path = selected_build / "src" / f"{source}.o"
                    if path.is_file():
                        object_paths[source] = path
                for requirement in package_def.requires:
                    directory = selected_build / "esp-idf" / requirement
                    archives = tuple(sorted(directory.glob("*.a"))) if directory.is_dir() else ()
                    if archives:
                        requirement_paths[requirement] = archives
            tool = _size_tool(selected_build)
            if tool is not None:
                try:
                    measured = _measure_artifacts(
                        tool,
                        list(object_paths.values())
                        + [
                            archive
                            for archives in requirement_paths.values()
                            for archive in archives
                        ],
                    )
                except (OSError, subprocess.CalledProcessError):
                    measured = {}

        known_ratios: list[float] = []
        for source, object_path in object_paths.items():
            source_path = root / "src" / source
            object_size = measured.get(object_path.resolve())
            if object_size is not None and source_path.is_file() and source_path.stat().st_size:
                known_ratios.append(object_size / source_path.stat().st_size)
        ratio = min(2.0, max(0.10, statistics.median(known_ratios))) if known_ratios else 0.35

        all_sources = {
            source
            for package_def in catalog.package_defs.values()
            for source in package_def.sources
        }
        source_sizes: dict[str, int] = {}
        measured_source_count = 0
        for source in all_sources:
            object_path = object_paths.get(source)
            measured_size = measured.get(object_path.resolve()) if object_path else None
            if measured_size is not None:
                source_sizes[source] = measured_size
                measured_source_count += 1
                continue
            source_path = root / "src" / source
            source_sizes[source] = (
                max(1, round(source_path.stat().st_size * ratio))
                if source_path.is_file() else 0
            )

        all_requirements = {
            requirement
            for package_def in catalog.package_defs.values()
            for requirement in package_def.requires
        }
        requirement_sizes: dict[str, int] = {}
        measured_requirement_count = 0
        for requirement in all_requirements:
            archives = requirement_paths.get(requirement, ())
            values = [measured.get(archive.resolve()) for archive in archives]
            known = [value for value in values if value is not None]
            requirement_sizes[requirement] = sum(known)
            if known:
                measured_requirement_count += 1

        if selected_build is None or tool is None or not measured:
            provenance = f"source fallback ({ratio:.2f}x source bytes)"
        else:
            provenance = (
                f"{selected_build.name}: {measured_source_count}/{len(all_sources)} objects, "
                f"{measured_requirement_count}/{len(all_requirements)} components"
            )
        return cls(catalog, source_sizes, requirement_sizes, provenance)

    def estimate(self, packages: Iterable[str]) -> int:
        package_set = set(packages)
        sources = {
            source
            for package in package_set
            for source in self.catalog.package_defs[package].sources
        }
        requirements = {
            requirement
            for package in package_set
            for requirement in self.catalog.package_defs[package].requires
        }
        return (
            sum(self.source_sizes.get(source, 0) for source in sources)
            + sum(self.requirement_sizes.get(requirement, 0) for requirement in requirements)
        )

    def package_size(self, package: str) -> int:
        package_def = self.catalog.package_defs[package]
        return (
            sum(self.source_sizes.get(source, 0) for source in set(package_def.sources))
            + sum(self.requirement_sizes.get(requirement, 0)
                  for requirement in set(package_def.requires))
        )


@dataclass(frozen=True)
class TreeRow:
    kind: str
    name: str


_ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
_FLASH_PERCENT = re.compile(
    r"Writing at .*?(\d+(?:\.\d+)?)\s*%",
    re.IGNORECASE,
)


def progress_for_line(action: str, line: str, current: int) -> tuple[int, str]:
    """Translate noisy PlatformIO output into a stable progress stage."""
    lower = line.lower()
    if action == "build":
        if "configuring" in lower or "cmake" in lower:
            return max(current, 8), "Configuring"
        if "compiling" in lower:
            return min(78, max(15, current + 1)), "Compiling"
        if "linking" in lower:
            return max(current, 84), "Linking firmware"
        if "firmware.bin" in lower:
            return max(current, 92), "Creating image"
        if "success" in lower:
            return 100, "Build complete"
    else:
        match = _FLASH_PERCENT.search(line)
        if match:
            percent = float(match.group(1))
            return max(current, 20 + round(percent * 0.70)), "Writing flash"
        if "uploading" in lower:
            return max(current, 4), "Preparing upload"
        if "connecting" in lower:
            return max(current, 8), "Connecting"
        if "erase" in lower:
            return max(current, 15), "Erasing flash"
        if "verif" in lower or "hash of data verified" in lower:
            return max(current, 94), "Verifying"
        if "reset" in lower:
            return max(current, 98), "Resetting device"
        if "success" in lower:
            return 100, "Flash complete"
    return current, "Building" if action == "build" else "Flashing"


def build_fingerprint(board: BoardContext, layout: LayoutContext, content: str) -> str:
    payload = f"{board.board_id}\0{layout.key}\0{content}".encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def builder_environment(board: BoardContext,
                        layout: LayoutContext,
                        flavor_file: Path) -> dict[str, str]:
    result = dict(os.environ)
    result.update({
        "SOLAR_OS_BOARD": board.board_id,
        "SOLAR_OS_FLAVOR": "os_builder",
        "SOLAR_OS_FLAVOR_FILE": str(flavor_file.resolve()),
        "SOLAR_OS_LAYOUT": layout.key,
    })
    return result


def builder_command(board: BoardContext,
                    flash: bool = False,
                    upload_port: str | None = None) -> list[str]:
    executable = str(PIO) if PIO.is_file() else "pio"
    command = [executable, "run", "-e", board.environment]
    if flash:
        command.extend(["-t", "upload"])
        if upload_port:
            command.extend(["--upload-port", upload_port])
    return command


class FlavorScreen:
    def __init__(self,
                 window: curses.window,
                 catalog: PackageCatalog,
                 model: SelectionModel,
                 estimator: FlashEstimator,
                 board: BoardContext | None,
                 input_path: Path,
                 output_path: Path,
                 layout: LayoutContext | None = None,
                 flavor_name: str = "custom",
                 description: str = "Custom SolarOS flavor created with os_builder.",
                 upload_port: str | None = None):
        self.window = window
        self.catalog = catalog
        self.model = model
        self.estimator = estimator
        self.board = board
        self.input_path = input_path
        self.output_path = output_path
        self.layout = layout
        self.flavor_name = flavor_name
        self.description = description
        self.upload_port = upload_port
        self.categories = group_names_by_category(catalog)
        self.expanded: set[str] = set()
        self.selected_index = 0
        self.offset = 0
        self.measured_size: int | None = None
        self.built_fingerprint: str | None = None
        self.status_message = ""
        self.saved = False
        self.window.keypad(True)
        try:
            curses.curs_set(0)
        except curses.error:
            pass

    def _flavor_content(self) -> str:
        return render_flavor(
            self.flavor_name,
            self.description,
            self.catalog,
            self.model,
        )

    def _fingerprint(self) -> str | None:
        if self.board is None or self.layout is None:
            return None
        return build_fingerprint(self.board, self.layout, self._flavor_content())

    @property
    def build_is_current(self) -> bool:
        return self.built_fingerprint is not None and self.built_fingerprint == self._fingerprint()

    def rows(self) -> list[TreeRow]:
        result: list[TreeRow] = []
        for category, groups in self.categories:
            result.append(TreeRow("category", category))
            if category not in self.expanded:
                continue
            result.extend(TreeRow("group", name) for name in groups)
        return result

    @staticmethod
    def _add(window: curses.window, row: int, column: int, text: str, limit: int,
             attr: int = curses.A_NORMAL) -> None:
        if limit <= 0:
            return
        try:
            window.addnstr(row, column, text, limit, attr)
        except curses.error:
            pass

    def _group_cost(self, name: str, current_size: int) -> tuple[str, int, int]:
        group = self.catalog.group_defs[name]
        explicit = self.model.group_state(group.members) == 2
        if explicit:
            without = self.model.preview_disable(group.members)
            total = max(0, current_size - self.estimator.estimate(without))
            base = without
            symbol = "~"
        else:
            base = self.model.selected
            with_group = self.model.preview_enable(group.members)
            total = max(0, self.estimator.estimate(with_group) - current_size)
            symbol = "+"
        direct_packages = set(base) | set(group.members)
        direct = max(
            0,
            self.estimator.estimate(direct_packages) - self.estimator.estimate(base),
        )
        return symbol, total, min(total, direct)

    def _draw(self) -> list[TreeRow]:
        self.window.erase()
        height, width = self.window.getmaxyx()
        if height < 9 or width < 48:
            warning = "Terminal too small; resize to at least 48x9 (q: exit)"
            self._add(self.window, 0, 0, warning, width, curses.A_REVERSE | curses.A_BOLD)
            self.window.refresh()
            return []
        rows = self.rows()
        self.selected_index = min(self.selected_index, max(0, len(rows) - 1))
        selected = self.model.selected
        estimated_size = self.estimator.estimate(selected)
        current_size = (
            self.measured_size
            if self.build_is_current and self.measured_size is not None
            else estimated_size
        )
        size_mark = "" if self.build_is_current else "~"
        if self.board is None:
            title = top_bar_text(
                sum(
                    self.model.group_state(group.members) == 2
                    for group in visible_groups(self.catalog)
                ),
                len(visible_groups(self.catalog)),
                current_size,
                self.estimator.estimate(self.model.mandatory),
            )
        else:
            app_bytes = self.layout.app_bytes if self.layout else self.board.app_bytes
            free = app_bytes - current_size
            fit = (
                f"{size_mark}{format_size(free)} free"
                if free >= 0 else f"OVER by {size_mark}{format_size(-free)}"
            )
            title = (
                f" {self.board.name} | image {size_mark}{format_size(current_size)} / "
                f"{format_size(app_bytes)} | {fit} "
            )
        self._add(self.window, 0, 0, title.ljust(width), width, curses.A_REVERSE | curses.A_BOLD)
        board_text = ""
        if self.board is not None:
            layout_text = f"{self.layout.label} | " if self.layout else ""
            board_text = (
                f"{format_size(self.board.flash_bytes)} flash, "
                f"{format_size(self.board.psram_bytes)} PSRAM | {layout_text}"
            )
        context = f"{board_text}From {self.input_path.name} -> {self.output_path.name} | {self.estimator.provenance}"
        self._add(self.window, 1, 1, context, width - 2, curses.A_DIM)

        visible = max(1, height - 5)
        self.offset = min(self.offset, max(0, len(rows) - visible))
        if self.selected_index < self.offset:
            self.offset = self.selected_index
        elif self.selected_index >= self.offset + visible:
            self.offset = self.selected_index - visible + 1

        for screen_row, tree_row in enumerate(rows[self.offset:self.offset + visible], 2):
            index = self.offset + screen_row - 2
            attr = curses.A_REVERSE if index == self.selected_index else curses.A_NORMAL
            if tree_row.kind == "category":
                arrow = "v" if tree_row.name in self.expanded else ">"
                count = next(
                    len(groups) for category, groups in self.categories
                    if category == tree_row.name
                )
                prefix = f"{arrow} {tree_row.name} ({count})"
                size_text = ""
            else:
                group = self.catalog.group_defs[tree_row.name]
                explicit = self.model.group_state(group.members) == 2
                locked = self.model.group_locked(group)
                supported = self.model.group_supported(group)
                automatic = not explicit and bool(set(group.members) & selected)
                if locked:
                    mark = "[!]"
                elif explicit:
                    mark = "[x]"
                elif automatic:
                    mark = "[+]"
                elif not supported:
                    mark = "[-]"
                else:
                    mark = "[ ]"
                prefix = f"    {mark} {group.label}"
                symbol, total, _ = self._group_cost(tree_row.name, estimated_size)
                size_text = f"{symbol}{format_size(total)}" if supported or explicit else "unavailable"
            available = max(1, width - 2)
            if len(prefix) + len(size_text) + 1 <= available:
                line = prefix + " " * (available - len(prefix) - len(size_text)) + size_text
            else:
                line = prefix
            self._add(self.window, screen_row, 1, line, available, attr)

        current = rows[self.selected_index] if rows else None
        detail = ""
        if current is not None and current.kind == "group":
            group = self.catalog.group_defs[current.name]
            supported = self.model.group_supported(group)
            if not supported:
                detail = self.model.unsupported_reason(group)
            elif self.model.group_locked(group):
                detail = "included by the selected board"
            else:
                _, total, direct = self._group_cost(current.name, estimated_size)
                detail = (
                    f"group {format_size(direct)} | "
                    f"automatic support {format_size(max(0, total - direct))}"
                )
        self._add(self.window, height - 3, 1, detail, width - 2, curses.A_DIM)
        state = self.status_message
        if self.built_fingerprint and not self.build_is_current:
            state = "Configuration changed: rebuild before flashing"
        elif not state and self.build_is_current:
            state = "Build is current; ready to flash"
        self._add(self.window, height - 2, 1, state, width - 2, curses.A_DIM)
        help_text = (
            "Arrows/hjkl: navigate/open  Space: toggle group  a: all supported  "
            "n: board only  b: build  f: flash  s: save  q: exit"
        )
        self._add(self.window, height - 1, 1, help_text, width - 2, curses.A_BOLD)
        self.window.refresh()
        return rows

    def _confirm_overwrite(self) -> bool:
        if not self.output_path.exists():
            return True
        return self._confirm(f"Overwrite {self.output_path}? (y/N)")

    def _confirm(self, prompt: str) -> bool:
        height, width = self.window.getmaxyx()
        self._add(self.window, height - 2, 1, prompt.ljust(max(1, width - 2)), width - 2,
                  curses.A_REVERSE)
        self.window.refresh()
        return self.window.getch() in (ord("y"), ord("Y"))

    def _draw_progress(self,
                       action: str,
                       percent: int,
                       stage: str,
                       tail: Iterable[str]) -> None:
        self.window.erase()
        height, width = self.window.getmaxyx()
        title = f" SolarOS Builder | {action.title()} "
        self._add(self.window, 0, 0, title.ljust(width), width,
                  curses.A_REVERSE | curses.A_BOLD)
        bar_width = max(10, width - 16)
        filled = min(bar_width, round(bar_width * percent / 100))
        bar = "[" + "#" * filled + "-" * (bar_width - filled) + "]"
        self._add(self.window, 2, 1, f"{bar} {percent:3d}%", width - 2, curses.A_BOLD)
        self._add(self.window, 3, 1, stage, width - 2)
        log_height = max(0, height - 6)
        for row, line in enumerate(list(tail)[-log_height:], 4):
            self._add(self.window, row, 1, line, width - 2, curses.A_DIM)
        self._add(self.window, height - 1, 1, "Esc: cancel", width - 2, curses.A_BOLD)
        self.window.refresh()

    def _show_failure(self, action: str, lines: list[str], log_path: Path) -> None:
        important = [
            line for line in lines
            if re.search(r"\b(error|fatal|failed|traceback)\b", line, re.IGNORECASE)
        ]
        concise = (important[-12:] + lines[-12:]) if important else lines[-24:]
        concise = list(dict.fromkeys(line for line in concise if line.strip()))
        if not concise:
            concise = ["The command failed without diagnostic output."]
        showing_all = False
        offset = 0
        while True:
            content = lines if showing_all else concise
            self.window.erase()
            height, width = self.window.getmaxyx()
            title = f" {action.title()} failed | {'full log' if showing_all else 'error summary'} "
            self._add(self.window, 0, 0, title.ljust(width), width,
                      curses.A_REVERSE | curses.A_BOLD)
            self._add(self.window, 1, 1, f"Log: {log_path}", width - 2, curses.A_DIM)
            visible = max(1, height - 4)
            offset = min(offset, max(0, len(content) - visible))
            for row, line in enumerate(content[offset:offset + visible], 2):
                self._add(self.window, row, 1, line, width - 2)
            self._add(
                self.window,
                height - 1,
                1,
                "a: full/summary  Arrows/PgUp/PgDn: scroll  Enter/Esc: return",
                width - 2,
                curses.A_BOLD,
            )
            self.window.refresh()
            key = self.window.getch()
            if key in (10, 13, curses.KEY_ENTER, 27, ord("q")):
                return
            if key == ord("a"):
                showing_all = not showing_all
                offset = max(0, len(lines if showing_all else concise) - visible)
            elif key in (curses.KEY_UP, ord("k")):
                offset = max(0, offset - 1)
            elif key in (curses.KEY_DOWN, ord("j")):
                offset = min(max(0, len(content) - visible), offset + 1)
            elif key == curses.KEY_PPAGE:
                offset = max(0, offset - visible)
            elif key == curses.KEY_NPAGE:
                offset = min(max(0, len(content) - visible), offset + visible)

    def _run_process(self,
                     action: str,
                     command: list[str],
                     environment: dict[str, str],
                     log_path: Path) -> bool:
        lines: list[str] = []
        tail: deque[str] = deque(maxlen=12)
        percent = 1
        stage = "Starting"
        cancelled = False
        log_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            process = subprocess.Popen(
                command,
                cwd=ROOT,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                start_new_session=True,
            )
        except OSError as exc:
            lines = [f"Could not start {' '.join(command)}: {exc}"]
            log_path.write_text(lines[0] + "\n", encoding="utf-8")
            self._show_failure(action, lines, log_path)
            return False

        assert process.stdout is not None
        self.window.timeout(100)
        try:
            while process.poll() is None:
                ready, _, _ = select.select([process.stdout], [], [], 0)
                if ready:
                    raw = process.stdout.readline()
                    for part in raw.replace("\r", "\n").splitlines():
                        clean = _ANSI_ESCAPE.sub("", part).strip()
                        if clean:
                            lines.append(clean)
                            tail.append(clean)
                            percent, stage = progress_for_line(action, clean, percent)
                self._draw_progress(action, percent, stage, tail)
                if self.window.getch() == 27:
                    cancelled = True
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    break
            remainder = process.stdout.read()
            for part in remainder.replace("\r", "\n").splitlines():
                clean = _ANSI_ESCAPE.sub("", part).strip()
                if clean:
                    lines.append(clean)
                    tail.append(clean)
                    percent, stage = progress_for_line(action, clean, percent)
        finally:
            self.window.timeout(-1)

        return_code = process.wait()
        log_path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
        if cancelled:
            self.status_message = f"{action.title()} cancelled"
            return False
        if return_code != 0:
            self._show_failure(action, lines, log_path)
            self.status_message = f"{action.title()} failed; see {log_path.name}"
            return False
        self._draw_progress(action, 100, f"{action.title()} complete", tail)
        return True

    def _build(self) -> None:
        if self.board is None or self.layout is None:
            self.status_message = "Select a board and update layout first"
            return
        content = self._flavor_content()
        write_if_changed(BUILDER_FLAVOR, content)
        fingerprint = build_fingerprint(self.board, self.layout, content)
        succeeded = self._run_process(
            "build",
            builder_command(self.board),
            builder_environment(self.board, self.layout, BUILDER_FLAVOR),
            BUILDER_DIR / "build.log",
        )
        if not succeeded:
            self.built_fingerprint = None
            self.measured_size = None
            return
        firmware = self.board.build_dir / "firmware.bin"
        if not firmware.is_file():
            lines = [f"Build succeeded but the firmware image is missing: {firmware}"]
            log_path = BUILDER_DIR / "build.log"
            with log_path.open("a", encoding="utf-8") as file:
                file.write(lines[0] + "\n")
            self._show_failure("build", lines, log_path)
            self.status_message = "Build output is missing"
            self.built_fingerprint = None
            self.measured_size = None
            return
        self.measured_size = firmware.stat().st_size
        self.built_fingerprint = fingerprint
        self.estimator = FlashEstimator.create(self.catalog, self.board.build_dir)
        self.status_message = f"Built {format_size(self.measured_size)} image"

    def _flash(self) -> None:
        if self.board is None or self.layout is None:
            self.status_message = "Select a board and update layout first"
            return
        if not self.build_is_current:
            self.status_message = "Build this configuration before flashing"
            return
        if not self._confirm(
                f"Flash {self.board.name} ({self.layout.label}) now? (y/N)"):
            self.status_message = "Flash cancelled"
            return
        write_if_changed(BUILDER_FLAVOR, self._flavor_content())
        succeeded = self._run_process(
            "flash",
            builder_command(self.board, True, self.upload_port),
            builder_environment(self.board, self.layout, BUILDER_FLAVOR),
            BUILDER_DIR / "flash.log",
        )
        self.status_message = "Flash complete" if succeeded else self.status_message

    def _save(self) -> None:
        if not self._confirm_overwrite():
            self.status_message = "Save cancelled"
            return
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        write_if_changed(self.output_path, self._flavor_content())
        self.saved = True
        self.status_message = f"Saved {self.output_path}"

    def run(self) -> bool:
        while True:
            rows = self._draw()
            key = self.window.getch()
            if key in (ord("q"), 27):
                return self.saved
            if not rows:
                continue
            if key in (curses.KEY_UP, ord("k")):
                self.selected_index = max(0, self.selected_index - 1)
            elif key in (curses.KEY_DOWN, ord("j")):
                self.selected_index = min(len(rows) - 1, self.selected_index + 1)
            elif key == curses.KEY_HOME:
                self.selected_index = 0
            elif key == curses.KEY_END:
                self.selected_index = len(rows) - 1
            elif key == curses.KEY_PPAGE:
                page = max(1, self.window.getmaxyx()[0] - 4)
                self.selected_index = max(0, self.selected_index - page)
            elif key == curses.KEY_NPAGE:
                self.selected_index = min(
                    len(rows) - 1,
                    self.selected_index + max(1, self.window.getmaxyx()[0] - 4),
                )
            elif key in (curses.KEY_RIGHT, ord("l")):
                row = rows[self.selected_index]
                if row.kind == "category":
                    if row.name not in self.expanded:
                        self.expanded.add(row.name)
                    else:
                        self.selected_index += 1
            elif key in (curses.KEY_LEFT, ord("h")):
                row = rows[self.selected_index]
                if row.kind == "category":
                    self.expanded.discard(row.name)
                else:
                    category = self.catalog.group_defs[row.name].category
                    self.selected_index = next(
                        index for index, candidate in enumerate(rows)
                        if candidate.kind == "category" and candidate.name == category
                    )
            elif key in (10, 13, curses.KEY_ENTER):
                row = rows[self.selected_index]
                if row.kind == "category":
                    if row.name in self.expanded:
                        self.expanded.remove(row.name)
                    else:
                        self.expanded.add(row.name)
            elif key == ord(" "):
                row = rows[self.selected_index]
                if row.kind == "group":
                    group = self.catalog.group_defs[row.name]
                    if (self.model.group_state(group.members) == 2
                            or (self.model.group_supported(group)
                                and not self.model.group_locked(group))):
                        self.model.toggle_group(group.members)
            elif key == ord("a"):
                self.model.select_all()
            elif key == ord("n"):
                self.model.clear_optional()
            elif key == ord("b"):
                self._build()
            elif key == ord("f"):
                self._flash()
            elif key == ord("s"):
                self._save()


def _choose_board(window: curses.window, boards: list[BoardContext]) -> BoardContext:
    selected = 0
    window.keypad(True)
    while True:
        window.erase()
        height, width = window.getmaxyx()
        FlavorScreen._add(window, 0, 0, " SolarOS Builder | Select board ".ljust(width), width,
                          curses.A_REVERSE | curses.A_BOLD)
        visible = max(1, height - 2)
        offset = min(max(0, selected - visible + 1), max(0, len(boards) - visible))
        for row, board in enumerate(boards[offset:offset + visible], 1):
            index = offset + row - 1
            text = (
                f"{board.name} | {format_size(board.flash_bytes)} flash | "
                f"{format_size(board.psram_bytes)} PSRAM"
            )
            attr = curses.A_REVERSE if index == selected else curses.A_NORMAL
            FlavorScreen._add(window, row, 1, text, width - 2, attr)
        FlavorScreen._add(window, height - 1, 1, "Enter: select  q: cancel", width - 2,
                          curses.A_BOLD)
        window.refresh()
        key = window.getch()
        if key in (curses.KEY_UP, ord("k")):
            selected = (selected - 1) % len(boards)
        elif key in (curses.KEY_DOWN, ord("j")):
            selected = (selected + 1) % len(boards)
        elif key in (10, 13, curses.KEY_ENTER):
            return boards[selected]
        elif key in (ord("q"), 27):
            raise KeyboardInterrupt


def _choose_layout(window: curses.window,
                   board: BoardContext,
                   preferred: str | None = None) -> LayoutContext:
    selected = next(
        (index for index, layout in enumerate(board.layouts) if layout.key == preferred),
        0,
    )
    while True:
        window.erase()
        height, width = window.getmaxyx()
        FlavorScreen._add(
            window, 0, 0,
            f" SolarOS Builder | {board.name} | Update layout ".ljust(width),
            width, curses.A_REVERSE | curses.A_BOLD,
        )
        for row, layout in enumerate(board.layouts, 2):
            text = (
                f"{layout.label} | {layout.description} | "
                f"{format_size(layout.app_bytes)} image limit"
            )
            attr = curses.A_REVERSE if row - 2 == selected else curses.A_NORMAL
            FlavorScreen._add(window, row, 2, text, width - 4, attr)
        note = (
            "This board has one fixed serial-update layout."
            if len(board.layouts) == 1 else
            "OTA keeps two images; serial updates give one image more space."
        )
        FlavorScreen._add(window, height - 2, 1, note, width - 2, curses.A_DIM)
        FlavorScreen._add(window, height - 1, 1, "Enter: select  q: back", width - 2,
                          curses.A_BOLD)
        window.refresh()
        key = window.getch()
        if key in (curses.KEY_UP, ord("k")):
            selected = (selected - 1) % len(board.layouts)
        elif key in (curses.KEY_DOWN, ord("j")):
            selected = (selected + 1) % len(board.layouts)
        elif key in (10, 13, curses.KEY_ENTER):
            return board.layouts[selected]
        elif key in (ord("q"), 27):
            raise KeyboardInterrupt


def _run_tui(window: curses.window,
             catalog: PackageCatalog,
             requested: set[str],
             boards: list[BoardContext],
             selected_board: BoardContext | None,
             selected_layout: str | None,
             build_dir: Path | None,
             input_path: Path,
             output_path: Path,
             flavor_name: str,
             description: str,
             upload_port: str | None) -> tuple[bool, SelectionModel, BoardContext, LayoutContext]:
    board = selected_board or _choose_board(window, boards)
    if selected_layout is not None and not any(
            layout.key == selected_layout for layout in board.layouts):
        raise ValueError(
            f"{selected_layout} update layout is not available for {board.name}"
        )
    layout = _choose_layout(window, board, selected_layout)
    model = SelectionModel(
        catalog,
        requested,
        board.required_packages,
        board.target,
        board.capabilities,
        layout.key,
    )
    estimator = FlashEstimator.create(catalog, build_dir or board.build_dir)
    return FlavorScreen(
        window,
        catalog,
        model,
        estimator,
        board,
        input_path,
        output_path,
        layout,
        flavor_name,
        description,
        upload_port,
    ).run(), model, board, layout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT,
                        help="flavor to use as the initial selection")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="custom flavor TOML to write")
    parser.add_argument("--packages", type=Path, default=DEFAULT_PACKAGE_CATALOG)
    board_group = parser.add_mutually_exclusive_group()
    board_group.add_argument("--board", help="SolarOS board profile to configure for")
    board_group.add_argument(
        "--environment",
        help="legacy alias selecting the board with this PlatformIO environment",
    )
    parser.add_argument("--build-dir", type=Path,
                        help="explicit PlatformIO build directory for size estimates")
    parser.add_argument("--layout", choices=("ota", "single"),
                        help="preselect the update layout")
    parser.add_argument("--upload-port", help="serial port passed to PlatformIO upload")
    parser.add_argument("--name", help="flavor name (defaults to the output filename)")
    parser.add_argument(
        "--description",
        default="Custom SolarOS flavor created with os_builder.",
    )
    parser.add_argument("--list", action="store_true",
                        help="list boards and board-filtered groups without starting curses")
    args = parser.parse_args()

    try:
        catalog = load_catalog(args.packages)
        load_flavor(args.input, catalog)
        requested = load_requested_packages(args.input, catalog)
        boards = load_board_contexts()
        if not boards:
            raise ValueError("no buildable SolarOS board profiles found")
        selected_board = None
        if args.board:
            selected_board = next(
                (board for board in boards if board.board_id == args.board), None
            )
            if selected_board is None:
                raise ValueError(f"unknown board profile: {args.board}")
        elif args.environment:
            selected_board = next(
                (board for board in boards if board.environment == args.environment
                 and board.board_id == args.environment),
                None,
            )
            if selected_board is None:
                raise ValueError(f"unknown PlatformIO environment: {args.environment}")

        if args.list:
            if selected_board is None:
                print("Boards:")
                for board in boards:
                    layouts = ", ".join(layout.key for layout in board.layouts)
                    print(
                        f"  {board.board_id:<42} {format_size(board.flash_bytes):>9} flash  "
                        f"{format_size(board.psram_bytes):>9} PSRAM  {layouts}"
                    )
                print("\nUse --board <id> to show compatibility and estimates.")
                return 0
            layout = next(
                (layout for layout in selected_board.layouts if layout.key == args.layout),
                selected_board.layouts[0] if args.layout is None else None,
            )
            if layout is None:
                raise ValueError(
                    f"{args.layout} update layout is not available for {selected_board.name}"
                )
            model = SelectionModel(
                catalog,
                requested,
                selected_board.required_packages,
                selected_board.target,
                selected_board.capabilities,
                layout.key,
            )
            estimator = FlashEstimator.create(
                catalog,
                args.build_dir or selected_board.build_dir,
            )
            selected = model.selected
            current_size = estimator.estimate(selected)
            print(
                f"Board: {selected_board.name} ({selected_board.board_id})\n"
                f"Flash: {format_size(selected_board.flash_bytes)}, "
                f"layout: {layout.label}, "
                f"application slot: {format_size(layout.app_bytes)}, "
                f"PSRAM: {format_size(selected_board.psram_bytes)}\n"
            )
            for category, names in group_names_by_category(catalog):
                print(f"{category}:")
                for name in names:
                    group = catalog.group_defs[name]
                    explicit = model.group_state(group.members) == 2
                    supported = model.group_supported(group)
                    locked = model.group_locked(group)
                    mark = (
                        "!" if locked else
                        "x" if explicit else
                        "+" if set(group.members) & selected else
                        " "
                    )
                    if not supported:
                        print(f"  [-] {group.label:<38} {model.unsupported_reason(group)}")
                        continue
                    if explicit:
                        preview = model.preview_disable(group.members)
                        contribution = max(
                            0,
                            current_size - estimator.estimate(preview),
                        )
                        size_text = f"~{format_size(contribution)}"
                    else:
                        preview = model.preview_enable(group.members)
                        contribution = max(
                            0,
                            estimator.estimate(preview) - current_size,
                        )
                        size_text = f"+{format_size(contribution)}"
                    print(f"  [{mark}] {group.label:<38} {size_text}")
            print(f"size model: {estimator.provenance}")
            return 0

        flavor_name = args.name or args.output.stem
        saved, model, selected_board, layout = curses.wrapper(
            _run_tui,
            catalog,
            requested,
            boards,
            selected_board,
            args.layout,
            args.build_dir,
            args.input,
            args.output,
            flavor_name,
            args.description,
            args.upload_port,
        )
        if not saved:
            print("Exited SolarOS Builder without saving a flavor.")
            return 0
    except KeyboardInterrupt:
        print("Flavor configuration cancelled.", file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.SubprocessError, curses.error) as exc:
        print(f"os_builder: {exc}", file=sys.stderr)
        return 1

    print(f"Wrote {args.output}")
    print(f"Board: {selected_board.name}; layout: {layout.label}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
