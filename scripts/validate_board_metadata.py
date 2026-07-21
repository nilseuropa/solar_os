#!/usr/bin/env python3
"""Validate the consistency of SolarOS board metadata.

CMake board profiles are authoritative for capability flags and driver selection.
Board headers are authoritative for identity, pins, and static buses.  This tool
checks the deliberately shared boundary between those sources and the user-facing
board documentation.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


CAPABILITY_DEPENDENCIES = {
    "DISPLAY": ("GFX",),
    "DISPLAY_BRIGHTNESS": ("DISPLAY",),
    "AUDIO_INPUT": ("AUDIO",),
    "EXPANSION_GPIO": ("GPIO",),
    "EXPANSION_I2C": ("I2C",),
    "EXPANSION_SPI": ("SPI",),
    "EXPANSION_UART": ("UART",),
    "EXPANSION_ADC": ("GPIO", "ADC"),
    "EXPANSION_PWM": ("GPIO", "PWM"),
}

PIN_SURFACES = (
    ("EXPANSION_GPIO", "SOLAR_OS_BOARD_EXPANSION_GPIO_MASK"),
    ("EXPANSION_ADC", "SOLAR_OS_BOARD_EXPANSION_ADC_MASK"),
    ("EXPANSION_PWM", "SOLAR_OS_BOARD_EXPANSION_PWM_MASK"),
)


@dataclass(frozen=True)
class BoardMetadata:
    board_id: str
    name: str
    vendor: str
    define: str
    capabilities: frozenset[str]
    macros: dict[str, str]
    static_buses: tuple[tuple[str, str], ...]


def _logical_lines(text: str) -> list[str]:
    result: list[str] = []
    pending = ""
    for line in text.splitlines():
        stripped = line.rstrip()
        pending += stripped[:-1] + " " if stripped.endswith("\\") else stripped
        if not stripped.endswith("\\"):
            result.append(pending)
            pending = ""
    if pending:
        result.append(pending)
    return result


def parse_macros(text: str) -> dict[str, str]:
    macros: dict[str, str] = {}
    for line in _logical_lines(text):
        match = re.match(r"^\s*#define\s+([A-Z0-9_]+)(?:\s+(.*))?$", line)
        if match:
            macros[match.group(1)] = (match.group(2) or "").strip()
    return macros


def _quoted_macro(macros: dict[str, str], name: str) -> str | None:
    value = macros.get(name)
    if value is None:
        return None
    match = re.fullmatch(r'"([^"]*)"', value)
    return match.group(1) if match else None


def _cmake_value(text: str, name: str) -> str | None:
    match = re.search(rf"set\(\s*{re.escape(name)}\s+(?:\"([^\"]*)\"|([^\s\)]+))\s*\)", text)
    if not match:
        return None
    return match.group(1) if match.group(1) is not None else match.group(2)


def _mask_pins(macros: dict[str, str], name: str, seen: frozenset[str] = frozenset()) -> set[int]:
    if name in seen:
        raise ValueError(f"cyclic mask alias involving {name}")
    value = macros.get(name, "0")
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", value):
        return _mask_pins(macros, value, seen | {name})
    pins = {int(pin) for pin in re.findall(r"GPIO_NUM_(\d+)", value)}
    if pins:
        return pins
    if re.fullmatch(r"(?:\(?\s*)?(?:0|0U|0UL|0ULL)(?:\s*\)?)", value):
        return set()
    raise ValueError(f"cannot parse {name}: {value}")


def _pin_list(macros: dict[str, str], name: str) -> set[int]:
    value = _quoted_macro(macros, name)
    if value is None:
        raise ValueError(f"missing or invalid {name}")
    return {int(pin) for pin in value.split()} if value.strip() else set()


def _gpio_slots(macros: dict[str, str]) -> dict[int, str]:
    value = macros.get("SOLAR_OS_BOARD_GPIO_SLOTS", "")
    return {
        int(pin): policy
        for pin, policy in re.findall(
            r"\.pin\s*=\s*(?:GPIO_NUM_)?(\d+).*?"
            r"\.policy\s*=\s*SOLAR_OS_PIN_POLICY_(FREE|RELEASABLE|FIXED)",
            value,
        )
    }


def _static_buses(macros: dict[str, str]) -> tuple[tuple[str, str], ...]:
    value = macros.get("SOLAR_OS_BOARD_BUSES", "")
    return tuple(
        (name, protocol)
        for name, protocol in re.findall(
            r'\.name\s*=\s*"([^"]+)"\s*,\s*'
            r"\.protocol\s*=\s*SOLAR_OS_BUS_PROTOCOL_([A-Z0-9_]+)",
            value,
        )
    )


def _gpio_value(
    macros: dict[str, str],
    value: str,
    seen: frozenset[str] = frozenset(),
) -> int | None:
    direct = re.fullmatch(r"GPIO_NUM_(\d+)", value)
    if direct:
        return int(direct.group(1))
    if value == "GPIO_NUM_NC":
        return None
    if value in seen or value not in macros:
        raise ValueError(f"cannot resolve GPIO value {value}")
    return _gpio_value(macros, macros[value], seen | {value})


def _static_bus_pins(macros: dict[str, str]) -> dict[str, set[int]]:
    value = macros.get("SOLAR_OS_BOARD_BUSES", "")
    matches = list(re.finditer(
        r'\.name\s*=\s*"([^"]+)"\s*,\s*'
        r"\.protocol\s*=\s*SOLAR_OS_BUS_PROTOCOL_([A-Z0-9_]+)",
        value,
    ))
    result: dict[str, set[int]] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(value)
        body = value[match.end():end]
        pins: set[int] = set()
        for token in re.findall(
            r"\.(?:sda_pin|scl_pin|sclk_pin|miso_pin|mosi_pin|tx_pin|rx_pin|pin)\s*=\s*"
            r"([A-Z][A-Z0-9_]*|\d+)",
            body,
        ):
            if token.isdigit():
                pins.add(int(token))
                continue
            pin = _gpio_value(macros, token)
            if pin is not None:
                pins.add(pin)
        result[match.group(1)] = pins
    return result


def _parse_gpio_text(value: str) -> set[int]:
    pins: set[int] = set()
    for first, last in re.findall(r"GPIO(\d+)(?:-GPIO(\d+))?", value):
        start = int(first)
        end = int(last) if last else start
        pins.update(range(start, end + 1))
    return pins


def _table_row(section: str, labels: tuple[str, ...]) -> list[str] | None:
    for line in section.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if cells and cells[0] in labels:
            return cells
    return None


def _section(text: str, start: str, end: str | None) -> str:
    start_at = text.find(start)
    if start_at < 0:
        return ""
    end_at = text.find(end, start_at + len(start)) if end else -1
    return text[start_at:] if end_at < 0 else text[start_at:end_at]


def _format_pins(pins: set[int]) -> str:
    return " ".join(str(pin) for pin in sorted(pins)) or "none"


def _validate_global_metadata(root: Path) -> tuple[list[str], set[str]]:
    errors: list[str] = []
    cmake_text = (root / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    caps_header = (root / "src" / "services" / "solar_os_board_caps.h").read_text(encoding="utf-8")
    caps_source = (root / "src" / "services" / "solar_os_board_caps.c").read_text(encoding="utf-8")
    board_doc = (root / "doc" / "solar_os_boards.md").read_text(encoding="utf-8")

    block = re.search(r"set\(SOLAR_OS_BOARD_CAPABILITY_NAMES(.*?)\n\)", cmake_text, re.DOTALL)
    cmake_caps = (
        set(re.findall(r"^\s+([A-Z][A-Z0-9_]*)\s*$", block.group(1), re.MULTILINE))
        if block
        else set()
    )
    enum_caps = set(re.findall(
        r"^#define SOLAR_OS_BOARD_CAP_([A-Z0-9_]+)\s",
        caps_header,
        re.MULTILINE,
    ))
    runtime_caps = set(re.findall(
        r"SOLAR_OS_BOARD_HAS_([A-Z0-9_]+)\s*\?\s*SOLAR_OS_BOARD_CAP_\1",
        caps_source,
    ))
    capability_doc = _section(board_doc, "## Capability Flags", "## Board Header")
    doc_caps = set(re.findall(r"^\| `([A-Z0-9_]+)` \|", capability_doc, re.MULTILINE))

    for label, values in (("runtime capability bits", enum_caps),
                          ("runtime capability aggregation", runtime_caps),
                          ("documented capability flags", doc_caps)):
        missing = sorted(cmake_caps - values)
        extra = sorted(values - cmake_caps)
        if missing:
            errors.append(f"{label} missing: {', '.join(missing)}")
        if extra:
            errors.append(f"{label} has unknown entries: {', '.join(extra)}")
    if not cmake_caps:
        errors.append("could not parse SOLAR_OS_BOARD_CAPABILITY_NAMES")
    return errors, cmake_caps


def _load_board(
    root: Path,
    path: Path,
    known_caps: set[str],
) -> tuple[BoardMetadata | None, list[str]]:
    errors: list[str] = []
    board_id = path.stem
    header_path = root / "include" / "boards" / f"{board_id}.h"
    if not header_path.is_file():
        return None, [f"{board_id}: missing {header_path.relative_to(root)}"]

    cmake_text = path.read_text(encoding="utf-8")
    header_text = header_path.read_text(encoding="utf-8")
    macros = parse_macros(header_text)
    cmake_id = _cmake_value(cmake_text, "SOLAR_OS_BOARD_ID")
    cmake_name = _cmake_value(cmake_text, "SOLAR_OS_BOARD_NAME")
    define = _cmake_value(cmake_text, "SOLAR_OS_BOARD_DEFINE")
    header_id = _quoted_macro(macros, "SOLAR_OS_BOARD_ID")
    header_name = _quoted_macro(macros, "SOLAR_OS_BOARD_NAME")
    vendor = _quoted_macro(macros, "SOLAR_OS_BOARD_VENDOR") or ""

    if cmake_id != board_id:
        errors.append(f"{board_id}: CMake SOLAR_OS_BOARD_ID is {cmake_id!r}")
    if header_id != board_id:
        errors.append(f"{board_id}: header SOLAR_OS_BOARD_ID is {header_id!r}")
    if not cmake_name or cmake_name != header_name:
        errors.append(
            f"{board_id}: CMake/header board names differ "
            f"({cmake_name!r} != {header_name!r})"
        )
    if not define:
        errors.append(f"{board_id}: missing SOLAR_OS_BOARD_DEFINE")

    capabilities = {
        name
        for name, state in re.findall(
            r"set\(SOLAR_OS_BOARD_HAS_([A-Z0-9_]+)\s+(ON|OFF)\s*\)",
            cmake_text,
        )
        if state == "ON"
    }
    mentioned_caps = set(re.findall(r"set\(SOLAR_OS_BOARD_HAS_([A-Z0-9_]+)\s+", cmake_text))
    unknown = sorted(mentioned_caps - known_caps)
    if unknown:
        errors.append(f"{board_id}: unknown capabilities: {', '.join(unknown)}")
    for capability, dependencies in CAPABILITY_DEPENDENCIES.items():
        if capability not in capabilities:
            continue
        for dependency in dependencies:
            if dependency not in capabilities:
                errors.append(f"{board_id}: {capability} requires {dependency}")

    return BoardMetadata(
        board_id=board_id,
        name=cmake_name or header_name or board_id,
        vendor=vendor,
        define=define or "",
        capabilities=frozenset(capabilities),
        macros=macros,
        static_buses=_static_buses(macros),
    ), errors


def _validate_pin_metadata(board: BoardMetadata) -> list[str]:
    errors: list[str] = []
    macros = board.macros
    if "SOLAR_OS_BOARD_CAPABILITIES" in macros:
        errors.append(
            f"{board.board_id}: legacy SOLAR_OS_BOARD_CAPABILITIES duplicates "
            "CMake capability flags"
        )
    try:
        expansion = _mask_pins(macros, "SOLAR_OS_BOARD_EXPANSION_GPIO_MASK")
        users = _mask_pins(macros, "SOLAR_OS_BOARD_USER_GPIO_MASK")
        expansion_list = _pin_list(macros, "SOLAR_OS_BOARD_EXPANSION_GPIO_LIST")
        user_list = _pin_list(macros, "SOLAR_OS_BOARD_USER_GPIO_LIST")
    except ValueError as exc:
        return [f"{board.board_id}: {exc}"]

    if expansion != expansion_list:
        errors.append(
            f"{board.board_id}: expansion GPIO mask/list differ "
            f"({_format_pins(expansion)} != {_format_pins(expansion_list)})"
        )
    if users != user_list:
        errors.append(
            f"{board.board_id}: user GPIO mask/list differ "
            f"({_format_pins(users)} != {_format_pins(user_list)})"
        )
    if not users <= expansion:
        errors.append(f"{board.board_id}: user GPIOs are not a subset of expansion GPIOs")

    slots = _gpio_slots(macros)
    free_pins = {pin for pin, policy in slots.items() if policy == "FREE"}
    if free_pins != users:
        errors.append(
            f"{board.board_id}: free GPIO slots/user mask differ "
            f"({_format_pins(free_pins)} != {_format_pins(users)})"
        )
    missing_slots = expansion - set(slots)
    if missing_slots:
        errors.append(
            f"{board.board_id}: expansion GPIOs missing from GPIO slots: "
            f"{_format_pins(missing_slots)}"
        )

    for capability, mask_name in PIN_SURFACES:
        try:
            pins = _mask_pins(macros, mask_name)
        except ValueError as exc:
            errors.append(f"{board.board_id}: {exc}")
            continue
        enabled = capability in board.capabilities
        if enabled != bool(pins):
            errors.append(
                f"{board.board_id}: {capability} is {'enabled' if enabled else 'disabled'} "
                f"but {mask_name} is {'non-empty' if pins else 'empty'}"
            )
        if capability != "EXPANSION_GPIO" and not pins <= users:
            errors.append(f"{board.board_id}: {mask_name} contains non-user GPIOs")

    bus_names = [name for name, _ in board.static_buses]
    if len(bus_names) != len(set(bus_names)):
        errors.append(f"{board.board_id}: static bus names are not unique")
    for name, protocol in board.static_buses:
        capability = "GPIO" if protocol == "ONEWIRE" else protocol
        if capability not in board.capabilities:
            errors.append(
                f"{board.board_id}: static bus {name} uses {protocol} without "
                f"{capability} capability"
            )

    for mask_name, required in (
        ("SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK", ("SPI", "EXPANSION_SPI")),
        ("SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK", ("UART", "EXPANSION_UART")),
    ):
        value = macros.get(mask_name, "0")
        nonempty = not bool(re.fullmatch(r"(?:0|0U|0UL|0ULL)", value))
        if nonempty:
            for capability in required:
                if capability not in board.capabilities:
                    errors.append(f"{board.board_id}: {mask_name} requires {capability}")
    return errors


def _validate_registration(root: Path, board: BoardMetadata) -> list[str]:
    errors: list[str] = []
    selector = (root / "include" / "solar_os_board.h").read_text(encoding="utf-8")
    expected_include = f'#include "boards/{board.board_id}.h"'
    selector_pair = re.search(
        rf"defined\({re.escape(board.define)}\)\s*\n{re.escape(expected_include)}",
        selector,
    )
    if not selector_pair:
        errors.append(
            f"{board.board_id}: missing or inconsistent include/solar_os_board.h selector"
        )

    platformio = (root / "platformio.ini").read_text(encoding="utf-8")
    env_match = re.search(
        rf"^\[env:{re.escape(board.board_id)}\](.*?)(?=^\[|\Z)",
        platformio,
        re.MULTILINE | re.DOTALL,
    )
    if not env_match or f"-DSOLAR_OS_BOARD={board.board_id}" not in env_match.group(1):
        errors.append(f"{board.board_id}: missing or inconsistent PlatformIO environment")
    return errors


def _validate_documentation(root: Path, board: BoardMetadata) -> list[str]:
    errors: list[str] = []
    boards_doc = (root / "doc" / "solar_os_boards.md").read_text(encoding="utf-8")
    expansion_doc = (root / "doc" / "expansion.md").read_text(encoding="utf-8")
    if f"`{board.board_id}`" not in boards_doc:
        errors.append(f"{board.board_id}: missing from doc/solar_os_boards.md target table")

    short_name = board.name
    if board.vendor and short_name.startswith(board.vendor + " "):
        short_name = short_name[len(board.vendor) + 1:]
    labels = (board.name, short_name)
    gpio_section = _section(expansion_doc, "### GPIO, ADC, and PWM", "### Named and Runtime Buses")
    gpio_row = _table_row(gpio_section, labels)
    if not gpio_row or len(gpio_row) < 4:
        errors.append(f"{board.board_id}: missing GPIO row in doc/expansion.md")
    else:
        try:
            expected = (
                _mask_pins(board.macros, "SOLAR_OS_BOARD_EXPANSION_GPIO_MASK"),
                _mask_pins(board.macros, "SOLAR_OS_BOARD_USER_GPIO_MASK"),
                _mask_pins(board.macros, "SOLAR_OS_BOARD_EXPANSION_ADC_MASK"),
            )
            documented = tuple(_parse_gpio_text(cell) for cell in gpio_row[1:4])
            # A generic physical description is allowed, but an enumerated one must be exact.
            if documented[0] and documented[0] != expected[0]:
                errors.append(
                    f"{board.board_id}: documented physical GPIOs differ from expansion mask"
                )
            if documented[1] != expected[1]:
                errors.append(f"{board.board_id}: documented runtime GPIOs differ from user mask")
            if documented[2] != expected[2]:
                errors.append(f"{board.board_id}: documented runtime ADC pins differ from ADC mask")
        except ValueError as exc:
            errors.append(f"{board.board_id}: {exc}")

    bus_section = _section(expansion_doc, "### Named and Runtime Buses", "## Typical Workflow")
    bus_row = _table_row(bus_section, labels)
    if not bus_row or len(bus_row) < 2:
        errors.append(f"{board.board_id}: missing bus row in doc/expansion.md")
    else:
        documented_names = set(re.findall(r"`([a-z][a-z0-9]*\d+)`", bus_row[1]))
        expected_names = {name for name, _ in board.static_buses}
        if documented_names != expected_names:
            errors.append(
                f"{board.board_id}: documented/static bus names differ "
                f"({', '.join(sorted(documented_names)) or 'none'} != "
                f"{', '.join(sorted(expected_names)) or 'none'})"
            )
        else:
            try:
                expected_pins = _static_bus_pins(board.macros)
                documented_pins: dict[str, set[int]] = {}
                for item in bus_row[1].split(";"):
                    name_match = re.search(r"`([a-z][a-z0-9]*\d+)`", item)
                    if name_match:
                        documented_pins[name_match.group(1)] = _parse_gpio_text(item)
                for name in sorted(expected_names):
                    if documented_pins.get(name, set()) != expected_pins.get(name, set()):
                        errors.append(
                            f"{board.board_id}: documented/static pins for {name} differ "
                            f"({_format_pins(documented_pins.get(name, set()))} != "
                            f"{_format_pins(expected_pins.get(name, set()))})"
                        )
            except ValueError as exc:
                errors.append(f"{board.board_id}: {exc}")
    return errors


def validate(root: Path, board_ids: tuple[str, ...] = ()) -> list[str]:
    root = root.resolve()
    errors, known_caps = _validate_global_metadata(root)
    paths = sorted((root / "boards").glob("*.cmake"))
    if board_ids:
        requested = set(board_ids)
        paths = [path for path in paths if path.stem in requested]
        missing = requested - {path.stem for path in paths}
        errors.extend(f"unknown board: {board_id}" for board_id in sorted(missing))

    for path in paths:
        board, board_errors = _load_board(root, path, known_caps)
        errors.extend(board_errors)
        if board is None:
            continue
        errors.extend(_validate_pin_metadata(board))
        errors.extend(_validate_registration(root, board))
        errors.extend(_validate_documentation(root, board))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="SolarOS source root",
    )
    parser.add_argument("--board", action="append", default=[], help="validate only this board ID")
    args = parser.parse_args()

    try:
        errors = validate(args.root, tuple(args.board))
    except (OSError, ValueError) as exc:
        errors = [str(exc)]
    if errors:
        for error in errors:
            print(f"board metadata: {error}", file=sys.stderr)
        return 1
    print(f"Board metadata valid ({len(args.board) if args.board else 'all'} board profiles).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
