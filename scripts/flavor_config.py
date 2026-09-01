#!/usr/bin/env python3
"""Configure a SolarOS firmware flavor in a terminal UI."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import curses
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tomllib
from typing import Iterable

from generate_flavor_config import (
    DEFAULT_PACKAGE_CATALOG,
    PackageCatalog,
    load_catalog,
    load_flavor,
    write_if_changed,
)


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "flavors" / "core.toml"
DEFAULT_OUTPUT = ROOT / "flavors" / "custom.toml"
BUILD_ROOT = ROOT / ".pio" / "build"


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
    raw_groups = data.get("package_groups", {})
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

    def __init__(self, catalog: PackageCatalog, requested: Iterable[str] = ()):
        self.catalog = catalog
        self.mandatory = immutable_packages(catalog)
        self.requested = set(requested) - self.mandatory
        self.reverse_dependencies: dict[str, set[str]] = {
            package: set() for package in catalog.packages
        }
        for package, package_def in catalog.package_defs.items():
            for dependency in package_def.depends:
                self.reverse_dependencies[dependency].add(package)

    @property
    def selected(self) -> set[str]:
        return dependency_closure(self.catalog, self.requested | self.mandatory)

    def preview_enable(self, packages: Iterable[str]) -> set[str]:
        return dependency_closure(
            self.catalog,
            self.requested | self.mandatory | set(packages),
        )

    def enable(self, packages: Iterable[str]) -> None:
        self.requested.update(set(packages) - self.mandatory)

    def disable(self, packages: Iterable[str]) -> None:
        """Disable packages and explicit dependants so the result remains valid."""
        disabled = set(packages) - self.mandatory
        pending = list(disabled)
        while pending:
            dependency = pending.pop()
            for dependant in self.reverse_dependencies[dependency]:
                if dependant not in disabled:
                    disabled.add(dependant)
                    pending.append(dependant)
        self.requested.difference_update(disabled)

    def toggle_package(self, package: str) -> None:
        if package in self.mandatory:
            return
        if package in self.selected:
            self.disable({package})
        else:
            self.enable({package})

    def group_state(self, members: Iterable[str]) -> int:
        members_set = set(members)
        if not members_set:
            return 0
        selected_count = len(members_set & self.selected)
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
        self.requested = set(self.catalog.packages) - self.mandatory

    def clear_optional(self) -> None:
        self.requested.clear()


def render_flavor(name: str,
                  description: str,
                  catalog: PackageCatalog,
                  model: SelectionModel) -> str:
    """Render a compact flavor which reproduces the model's explicit choices."""
    complete_groups = [
        group
        for group in catalog.groups
        if not catalog.group_defs[group].immutable
        and catalog.group_defs[group].members
        and (set(catalog.group_defs[group].members) - model.mandatory) <= model.requested
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
        "[packages]",
    ]
    lines.extend(f"{group} = true" for group in complete_groups)
    if complete_groups and individual:
        lines.append("")
    lines.extend(f"{package} = true" for package in individual)
    return "\n".join(lines).rstrip() + "\n"


@dataclass(frozen=True)
class FolderDef:
    name: str
    members: tuple[str, ...]
    selectors: tuple[str, ...]


def folder_members(catalog: PackageCatalog) -> list[FolderDef]:
    folders: list[FolderDef] = []
    for group in catalog.groups:
        selectors = catalog.group_defs[group].members
        reachable = dependency_closure(catalog, selectors)
        dependencies = tuple(
            package
            for package in catalog.packages
            if package in reachable and package not in selectors
        )
        folders.append(FolderDef(group, selectors + dependencies, selectors))
    return folders


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
        f" SolarOS flavor | image ~{format_size(image_size)} | "
        f"{selected_count}/{package_count} components | "
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
    members: tuple[str, ...] = ()
    selectors: tuple[str, ...] = ()
    parent: str = ""


class FlavorScreen:
    def __init__(self,
                 window: curses.window,
                 catalog: PackageCatalog,
                 model: SelectionModel,
                 estimator: FlashEstimator,
                 input_path: Path,
                 output_path: Path):
        self.window = window
        self.catalog = catalog
        self.model = model
        self.estimator = estimator
        self.input_path = input_path
        self.output_path = output_path
        self.folders = folder_members(catalog)
        self.expanded: set[str] = set()
        self.selected_index = 0
        self.offset = 0
        self.window.keypad(True)
        try:
            curses.curs_set(0)
        except curses.error:
            pass

    def rows(self) -> list[TreeRow]:
        result: list[TreeRow] = []
        for folder in self.folders:
            result.append(
                TreeRow("folder", folder.name, folder.members, folder.selectors)
            )
            if folder.name in self.expanded:
                result.extend(
                    TreeRow("package", package, parent=folder.name)
                    for package in folder.members
                )
        return result

    def _folder_label(self, name: str) -> str:
        return name.replace("_", " ")

    @staticmethod
    def _add(window: curses.window, row: int, column: int, text: str, limit: int,
             attr: int = curses.A_NORMAL) -> None:
        if limit <= 0:
            return
        try:
            window.addnstr(row, column, text, limit, attr)
        except curses.error:
            pass

    def _row_size(self, row: TreeRow, selected: set[str], current_size: int) -> tuple[str, int]:
        if row.kind == "folder":
            if self.model.group_state(row.members) == 2:
                return "~", max(0, self.estimator.estimate(set(row.members))
                                - self.estimator.estimate(set()))
            preview = self.model.preview_enable(row.selectors)
            return "+", max(0, self.estimator.estimate(preview) - current_size)
        if row.name in selected:
            return "~", self.estimator.package_size(row.name)
        preview = self.model.preview_enable({row.name})
        return "+", max(0, self.estimator.estimate(preview) - current_size)

    def _draw(self) -> list[TreeRow]:
        self.window.erase()
        height, width = self.window.getmaxyx()
        if height < 8 or width < 48:
            warning = "Terminal too small; resize to at least 48x8 (q: cancel)"
            self._add(self.window, 0, 0, warning, width, curses.A_REVERSE | curses.A_BOLD)
            self.window.refresh()
            return []
        rows = self.rows()
        self.selected_index = min(self.selected_index, max(0, len(rows) - 1))
        selected = self.model.selected
        baseline = self.estimator.estimate(self.model.mandatory)
        current_size = self.estimator.estimate(selected)

        title = top_bar_text(
            len(selected),
            len(self.catalog.packages),
            current_size,
            baseline,
        )
        self._add(self.window, 0, 0, title.ljust(width), width, curses.A_REVERSE | curses.A_BOLD)
        context = (
            f"From {self.input_path.name}  ->  {self.output_path.name}  |  "
            f"size model: {self.estimator.provenance}"
        )
        self._add(self.window, 1, 1, context, width - 2, curses.A_DIM)

        visible = max(1, height - 4)
        self.offset = min(self.offset, max(0, len(rows) - visible))
        if self.selected_index < self.offset:
            self.offset = self.selected_index
        elif self.selected_index >= self.offset + visible:
            self.offset = self.selected_index - visible + 1

        for screen_row, tree_row in enumerate(rows[self.offset:self.offset + visible], 2):
            index = self.offset + screen_row - 2
            attr = curses.A_REVERSE if index == self.selected_index else curses.A_NORMAL
            prefix = ""
            if tree_row.kind == "folder":
                state = self.model.group_state(tree_row.members)
                mark = "[x]" if state == 2 else "[-]" if state == 1 else "[ ]"
                immutable = self.catalog.group_defs[tree_row.name].immutable
                if immutable:
                    mark = "[!]"
                arrow = "v" if tree_row.name in self.expanded else ">"
                prefix = (
                    f"{arrow} {mark} {self._folder_label(tree_row.name)} "
                    f"({len(tree_row.members)})"
                )
            else:
                package = tree_row.name
                if package in self.model.mandatory:
                    mark = "[!]"
                elif package in self.model.requested:
                    mark = "[x]"
                elif package in selected:
                    mark = "[+]"
                else:
                    mark = "[ ]"
                label = self.catalog.package_defs[package].label
                suffix = f" ({package})" if width >= 100 and label != package else ""
                prefix = f"    {mark} {label}{suffix}"
            size_kind, size_value = self._row_size(tree_row, selected, current_size)
            size_text = f"{size_kind}{format_size(size_value)}"
            available = max(1, width - 2)
            if len(prefix) + len(size_text) + 1 <= available:
                line = prefix + " " * (available - len(prefix) - len(size_text)) + size_text
            else:
                line = prefix
            self._add(self.window, screen_row, 1, line, available, attr)

        current = rows[self.selected_index] if rows else None
        detail = ""
        if current is not None and current.kind == "package":
            package_def = self.catalog.package_defs[current.name]
            parts = []
            if current.name in selected and current.name not in self.model.requested:
                parts.append("automatic dependency")
            if package_def.depends:
                parts.append("depends: " + ", ".join(package_def.depends))
            capabilities = package_def.capabilities or package_def.any_capabilities
            if capabilities:
                parts.append("capabilities: " + ", ".join(capabilities))
            detail = " | ".join(parts)
        elif current is not None:
            count = len(set(current.members) & selected)
            detail = f"{count}/{len(current.members)} children selected"
        self._add(self.window, height - 2, 1, detail, width - 2, curses.A_DIM)
        help_text = (
            "Arrows/hjkl: navigate/open  Space: toggle  a: all  n: bootstrap only  "
            "s: save  q: cancel  [!]=locked [+]=dependency"
        )
        self._add(self.window, height - 1, 1, help_text, width - 2, curses.A_BOLD)
        self.window.refresh()
        return rows

    def _confirm_overwrite(self) -> bool:
        if not self.output_path.exists():
            return True
        height, width = self.window.getmaxyx()
        prompt = f"Overwrite {self.output_path}? (y/N)"
        self._add(self.window, height - 2, 1, prompt.ljust(max(1, width - 2)), width - 2,
                  curses.A_REVERSE)
        self.window.refresh()
        return self.window.getch() in (ord("y"), ord("Y"))

    def run(self) -> bool:
        while True:
            rows = self._draw()
            key = self.window.getch()
            if key in (ord("q"), 27):
                return False
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
                if row.kind == "folder" and row.name not in self.expanded:
                    self.expanded.add(row.name)
                elif row.kind == "folder" and row.members:
                    self.selected_index += 1
            elif key in (curses.KEY_LEFT, ord("h")):
                row = rows[self.selected_index]
                if row.kind == "folder":
                    self.expanded.discard(row.name)
                else:
                    parent_index = next(
                        index for index, candidate in enumerate(rows)
                        if candidate.kind == "folder" and candidate.name == row.parent
                    )
                    self.selected_index = parent_index
            elif key in (10, 13, curses.KEY_ENTER):
                row = rows[self.selected_index]
                if row.kind == "folder":
                    if row.name in self.expanded:
                        self.expanded.remove(row.name)
                    else:
                        self.expanded.add(row.name)
                else:
                    self.model.toggle_package(row.name)
            elif key == ord(" "):
                row = rows[self.selected_index]
                if row.kind == "folder":
                    self.model.toggle_group(row.selectors)
                else:
                    self.model.toggle_package(row.name)
            elif key == ord("a"):
                self.model.select_all()
            elif key == ord("n"):
                self.model.clear_optional()
            elif key == ord("s") and self._confirm_overwrite():
                return True


def _run_tui(window: curses.window,
             catalog: PackageCatalog,
             model: SelectionModel,
             estimator: FlashEstimator,
             input_path: Path,
             output_path: Path) -> bool:
    return FlavorScreen(
        window,
        catalog,
        model,
        estimator,
        input_path,
        output_path,
    ).run()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT,
                        help="flavor to use as the initial selection")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="custom flavor TOML to write")
    parser.add_argument("--packages", type=Path, default=DEFAULT_PACKAGE_CATALOG)
    build_group = parser.add_mutually_exclusive_group()
    build_group.add_argument(
        "--environment",
        help="PlatformIO environment whose cached objects provide size estimates",
    )
    build_group.add_argument("--build-dir", type=Path,
                             help="explicit PlatformIO build directory for size estimates")
    parser.add_argument("--name", help="flavor name (defaults to the output filename)")
    parser.add_argument(
        "--description",
        default="Custom SolarOS flavor created with the flavor configuration TUI.",
    )
    parser.add_argument("--list", action="store_true",
                        help="list the package tree and estimates without starting curses")
    args = parser.parse_args()

    try:
        catalog = load_catalog(args.packages)
        load_flavor(args.input, catalog)
        requested = load_requested_packages(args.input, catalog)
        model = SelectionModel(catalog, requested)
        build_dir = args.build_dir
        if args.environment:
            build_dir = BUILD_ROOT / args.environment
        if build_dir is not None and not build_dir.is_dir():
            raise ValueError(f"build directory does not exist: {build_dir}")
        estimator = FlashEstimator.create(catalog, build_dir)

        if args.list:
            selected = model.selected
            for folder in folder_members(catalog):
                print(f"{folder.name}:")
                for package in folder.members:
                    mark = "x" if package in selected else " "
                    print(
                        f"  [{mark}] {catalog.package_defs[package].label:<28} "
                        f"~{format_size(estimator.package_size(package))}"
                    )
            print(f"size model: {estimator.provenance}")
            return 0

        saved = curses.wrapper(
            _run_tui,
            catalog,
            model,
            estimator,
            args.input,
            args.output,
        )
        if not saved:
            print("Flavor configuration cancelled.", file=sys.stderr)
            return 130

        name = args.name or args.output.stem
        content = render_flavor(name, args.description, catalog, model)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_if_changed(args.output, content)
    except KeyboardInterrupt:
        print("Flavor configuration cancelled.", file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.SubprocessError, curses.error) as exc:
        print(f"flavor config: {exc}", file=sys.stderr)
        return 1

    print(f"Wrote {args.output}")
    if args.output.parent.resolve() == (ROOT / "flavors").resolve():
        environment = args.environment or (build_dir.name if build_dir else "<environment>")
        print(f"Build with: SOLAR_OS_FLAVOR={args.output.stem} pio run -e {environment}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
