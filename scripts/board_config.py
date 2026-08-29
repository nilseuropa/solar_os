#!/usr/bin/env python3
"""Configure a SolarOS board profile in a terminal UI."""

from __future__ import annotations

import argparse
from copy import deepcopy
import curses
from pathlib import Path
import sys
import tomllib
from typing import Any, Iterable

from solaros_board_manifest import (
    DriverBinding,
    DriverDef,
    ManifestError,
    load_board_manifest,
    load_driver_catalog,
    merge_board_overlay,
    validate_board,
    write_if_changed,
)


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_DIR = ROOT / "boards" / "manifests"
DRIVER_CATALOG = ROOT / "boards" / "expansion_drivers.toml"
EXCLUSIVE_BOARD_CAPABILITIES = {"display", "sd", "audio", "pointer", "battery", "rtc"}
BUS_KINDS = {
    "i2c_bus": "i2c",
    "spi_bus": "spi",
    "uart_port": "uart",
    "ps2_bus": "ps2",
}


def _quoted(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _value(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return _quoted(value)
    if isinstance(value, list):
        return "[" + ", ".join(_value(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{key} = {_value(item)}" for key, item in value.items()) + " }"
    raise TypeError(f"cannot write TOML value {value!r}")


def render_overlay(profile: dict[str, Any]) -> str:
    """Render the small inherited manifest produced by the configurator."""
    lines = [f"schema = {profile['schema']}", f"extends = {_quoted(profile['extends'])}", ""]
    for table in ("board", "build", "defines", "runtime"):
        values = profile.get(table)
        if not values:
            continue
        lines.append(f"[{table}]")
        lines.extend(f"{key} = {_value(value)}" for key, value in values.items())
        lines.append("")
    for table in ("buses", "devices", "pins"):
        for item in profile.get(table, []):
            lines.append(f"[[{table}]]")
            lines.extend(f"{key} = {_value(value)}" for key, value in item.items())
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def available_base_profiles(manifest_dir: Path = MANIFEST_DIR) -> dict[str, list[tuple[Path, dict[str, Any]]]]:
    result: dict[str, list[tuple[Path, dict[str, Any]]]] = {"esp32s3": [], "esp32": []}
    for path in sorted(manifest_dir.glob("*.toml")):
        with path.open("rb") as file:
            raw = tomllib.load(file)
        if "extends" in raw:
            continue
        board = load_board_manifest(path, manifest_dir)
        mcu = board.get("target", {}).get("mcu")
        if mcu in result:
            result[mcu].append((path, board))
    return result


def _parse_integer(text: str) -> int:
    value = text.strip().lower()
    if value.startswith("gpio"):
        value = value[4:]
    if value.startswith("i2s"):
        value = value[3:]
    return int(value, 0)


class Screen:
    def __init__(self, window: curses.window):
        self.window = window
        self._set_cursor(False)
        self.window.keypad(True)

    @staticmethod
    def _set_cursor(visible: bool) -> None:
        try:
            curses.curs_set(1 if visible else 0)
        except curses.error:
            pass

    def _frame(self, title: str, help_text: str = "") -> None:
        self.window.erase()
        height, width = self.window.getmaxyx()
        self.window.addnstr(0, 2, f" SolarOS board configuration - {title} ", width - 4, curses.A_BOLD)
        if help_text:
            self.window.addnstr(height - 1, 1, help_text, width - 2, curses.A_DIM)

    def message(self, title: str, text: str) -> None:
        self._frame(title, "Press any key to continue")
        height, width = self.window.getmaxyx()
        for row, line in enumerate(text.splitlines(), 2):
            if row >= height - 2:
                break
            self.window.addnstr(row, 2, line, width - 4)
        self.window.refresh()
        self.window.getch()

    def choose(self, title: str, choices: list[tuple[str, str]], help_text: str = "Enter: select  q: quit") -> str:
        if not choices:
            raise ManifestError(f"no choices available for {title}")
        selected = 0
        while True:
            self._frame(title, help_text)
            height, width = self.window.getmaxyx()
            visible = max(1, height - 4)
            start = min(max(0, selected - visible + 1), max(0, len(choices) - visible))
            for row, (key, label) in enumerate(choices[start:start + visible], 2):
                index = start + row - 2
                attr = curses.A_REVERSE if index == selected else curses.A_NORMAL
                self.window.addnstr(row, 2, label, width - 4, attr)
            self.window.refresh()
            key = self.window.getch()
            if key in (curses.KEY_UP, ord("k")):
                selected = (selected - 1) % len(choices)
            elif key in (curses.KEY_DOWN, ord("j")):
                selected = (selected + 1) % len(choices)
            elif key in (10, 13, curses.KEY_ENTER):
                return choices[selected][0]
            elif key in (ord("q"), 27):
                raise KeyboardInterrupt

    def multi_choose(self, title: str, choices: list[tuple[str, str]]) -> list[str]:
        selected = 0
        enabled: set[str] = set()
        while True:
            self._frame(title, "Space: toggle  Enter: continue  q: quit")
            height, width = self.window.getmaxyx()
            visible = max(1, height - 4)
            start = min(max(0, selected - visible + 1), max(0, len(choices) - visible))
            for row, (key_name, label) in enumerate(choices[start:start + visible], 2):
                index = start + row - 2
                mark = "[*]" if key_name in enabled else "[ ]"
                attr = curses.A_REVERSE if index == selected else curses.A_NORMAL
                self.window.addnstr(row, 2, f"{mark} {label}", width - 4, attr)
            self.window.refresh()
            key = self.window.getch()
            if key in (curses.KEY_UP, ord("k")):
                selected = (selected - 1) % len(choices)
            elif key in (curses.KEY_DOWN, ord("j")):
                selected = (selected + 1) % len(choices)
            elif key == ord(" "):
                name = choices[selected][0]
                enabled.symmetric_difference_update({name})
            elif key in (10, 13, curses.KEY_ENTER):
                return [name for name, _ in choices if name in enabled]
            elif key in (ord("q"), 27):
                raise KeyboardInterrupt

    def prompt(self, title: str, label: str, default: str = "") -> str:
        self._frame(title, "Enter: accept")
        height, width = self.window.getmaxyx()
        self.window.addnstr(2, 2, label, width - 4, curses.A_BOLD)
        if default:
            self.window.addnstr(3, 2, f"Default: {default}", width - 4, curses.A_DIM)
        self.window.addstr(5, 2, "> ")
        self._set_cursor(True)
        curses.echo()
        try:
            raw = self.window.getstr(5, 4, max(1, width - 6)).decode("utf-8").strip()
        finally:
            curses.noecho()
            self._set_cursor(False)
        return raw or default

    def yes_no(self, title: str, label: str, default: bool = False) -> bool:
        choice = self.choose(title, [
            ("yes", "Yes" + (" (default)" if default else "")),
            ("no", "No" + (" (default)" if not default else "")),
        ])
        return choice == "yes"


class Configurator:
    def __init__(self, screen: Screen, base_path: Path, base: dict[str, Any], drivers: dict[str, DriverDef]):
        self.screen = screen
        self.base_path = base_path
        self.base = base
        self.drivers = drivers
        self.working_buses = {bus["name"]: deepcopy(bus) for bus in base.get("buses", [])}
        self.bus_overlays: dict[str, dict[str, Any]] = {}
        self.fixed_pins: dict[int, str] = {}

    def _prompt_int(self, title: str, label: str, default: str = "") -> int:
        while True:
            try:
                return _parse_integer(self.screen.prompt(title, label, default))
            except ValueError:
                self.screen.message("Invalid value", "Enter a decimal or 0x-prefixed integer. GPIO12 and gpio12 are also accepted.")

    def _fix_pin(self, gpio: int, role: str) -> None:
        previous = self.fixed_pins.get(gpio)
        if previous is not None and previous != role:
            raise ManifestError(f"GPIO{gpio} is assigned to both {previous} and {role}")
        self.fixed_pins[gpio] = role

    def _remove_cs_candidate(self, gpio: int, keep_bus: str = "") -> None:
        for name, bus in self.working_buses.items():
            if bus.get("protocol") != "spi" or name == keep_bus or gpio not in bus.get("cs", []):
                continue
            bus["cs"] = [pin for pin in bus["cs"] if pin != gpio]
            self.bus_overlays[name] = {"name": name, "cs": list(bus["cs"])}

    def _new_bus(self, protocol: str) -> str:
        title = f"New {protocol.upper()} bus"
        index = 0
        while f"{protocol}{index}" in self.working_buses:
            index += 1
        name = self.screen.prompt(title, "Bus name", f"{protocol}{index}")
        if name in self.working_buses:
            raise ManifestError(f"bus {name} already exists")
        bus: dict[str, Any] = {"name": name, "protocol": protocol, "sharing": "shared"}
        if protocol == "i2c":
            bus.update({
                "port": self.screen.choose(title, [("I2C_NUM_0", "I2C port 0"), ("I2C_NUM_1", "I2C port 1")]),
                "sda": self._prompt_int(title, "SDA GPIO"),
                "scl": self._prompt_int(title, "SCL GPIO"),
                "speed_hz": "SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ",
            })
        elif protocol == "spi":
            hosts = [("SPI2_HOST", "SPI2 host"), ("SPI3_HOST", "SPI3 host")]
            bus.update({
                "host": self.screen.choose(title, hosts),
                "sclk": self._prompt_int(title, "SCLK GPIO"),
                "mosi": self._prompt_int(title, "MOSI GPIO"),
                "miso": self._prompt_int(title, "MISO GPIO"),
                "cs": [],
                "max_transfer_size": self._prompt_int(title, "Maximum transfer size", "4096"),
            })
        elif protocol == "uart":
            ports = [("UART_NUM_1", "UART port 1"), ("UART_NUM_2", "UART port 2")]
            bus.update({
                "sharing": "exclusive",
                "port": self.screen.choose(title, ports),
                "tx": self._prompt_int(title, "TX GPIO"),
                "rx": self._prompt_int(title, "RX GPIO"),
                "baud_rate": "SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE",
            })
        elif protocol == "ps2":
            bus.update({
                "sharing": "exclusive",
                "clock": self._prompt_int(title, "Clock GPIO"),
                "data": self._prompt_int(title, "Data GPIO"),
            })
        else:
            raise ManifestError(f"the TUI cannot create {protocol} buses")
        for role in ("sda", "scl", "sclk", "mosi", "miso", "tx", "rx", "clock", "data"):
            if role in bus:
                gpio = int(bus[role])
                self._remove_cs_candidate(gpio)
                self._fix_pin(gpio, f"{name} {role}")
        self.working_buses[name] = bus
        self.bus_overlays[name] = deepcopy(bus)
        return name

    def _choose_bus(self, driver: DriverDef, spec: DriverBinding) -> str:
        protocol = BUS_KINDS[spec.kind]
        choices = [
            (name, f"{name} ({protocol})")
            for name, bus in self.working_buses.items()
            if bus.get("protocol") == protocol
        ]
        choices.append(("__new__", f"Create a new {protocol.upper()} bus"))
        selected = self.screen.choose(f"{driver.name}: {spec.key}", choices)
        return self._new_bus(protocol) if selected == "__new__" else selected

    def _binding_value(self, driver: DriverDef, spec: DriverBinding, bindings: dict[str, Any]) -> Any:
        title = f"Wire {driver.name}: {spec.key}"
        if spec.kind in BUS_KINDS:
            return self._choose_bus(driver, spec)
        if spec.kind == "scalar_stream":
            return self.screen.prompt(title, f"Resource name ({spec.hint})")
        default = ""
        if len(spec.allowed) == 1:
            default = str(spec.allowed[0])
        value = self._prompt_int(title, f"Value ({spec.hint})", default)
        if spec.kind in {"gpio", "adc", "pwm", "spi_cs"}:
            keep = str(bindings.get("spi", "")) if spec.kind == "spi_cs" else ""
            self._remove_cs_candidate(value, keep)
            self._fix_pin(value, f"{driver.name} {spec.role or spec.key}")
        if spec.kind == "spi_cs":
            bus_name = str(bindings.get("spi", ""))
            bus = self.working_buses[bus_name]
            if value not in bus.get("cs", []):
                bus.setdefault("cs", []).append(value)
                self.bus_overlays[bus_name] = {"name": bus_name, "cs": list(bus["cs"])}
        return value

    def configure_device(self, driver: DriverDef) -> dict[str, Any]:
        name = self.screen.prompt(driver.name, "Device name", driver.default_name)
        bindings: dict[str, Any] = {}
        for spec in driver.bindings:
            if not spec.required and not self.screen.yes_no(driver.name, f"Configure optional binding '{spec.key}'?", False):
                continue
            bindings[spec.key] = self._binding_value(driver, spec, bindings)
        return {"driver": driver.name, "name": name, "bindings": bindings}

    def create(self, board_id: str, name: str, vendor: str, module: str, driver_names: Iterable[str]) -> dict[str, Any]:
        selected = [self.drivers[item] for item in driver_names]
        for capability in EXCLUSIVE_BOARD_CAPABILITIES:
            owners = [driver.name for driver in selected if capability in driver.board_capabilities]
            if len(owners) > 1:
                raise ManifestError(f"select only one built-in {capability} driver: {', '.join(owners)}")

        devices = [self.configure_device(driver) for driver in selected]
        capabilities: list[str] = []
        fragments: list[str] = []
        defines: dict[str, str] = {}
        for driver in selected:
            for capability in (*driver.capabilities, *driver.board_capabilities):
                if capability not in capabilities:
                    capabilities.append(capability)
            if driver.board_driver and driver.board_driver not in fragments:
                fragments.append(driver.board_driver)
            defines.update(driver.board_defines)

        profile: dict[str, Any] = {
            "schema": 1,
            "extends": self.base["board"]["id"],
            "board": {"id": board_id, "name": name, "vendor": vendor, "module": module},
            "build": {"drivers": fragments, "capabilities": capabilities},
            "devices": devices,
        }
        if defines:
            profile["defines"] = defines
        if self.bus_overlays:
            profile["buses"] = list(self.bus_overlays.values())
        if self.fixed_pins:
            profile["pins"] = [
                {
                    "gpio": gpio,
                    "policy": "fixed",
                    "role": role,
                    "user": False,
                    "adc": False,
                    "pwm": False,
                }
                for gpio, role in sorted(self.fixed_pins.items())
            ]
        return profile


def _run_tui(window: curses.window, args: argparse.Namespace) -> tuple[Path, str]:
    screen = Screen(window)
    catalog = load_driver_catalog(args.drivers)
    bases = available_base_profiles(args.manifest_dir)
    mcu = screen.choose("Target MCU", [
        ("esp32s3", "ESP32-S3"),
        ("esp32", "Classic ESP32"),
    ])
    if not bases[mcu]:
        raise ManifestError(f"no declarative base profile is available for {mcu}")
    base_id = screen.choose("Base board", [
        (board["board"]["id"], f"{board['board']['name']} ({path.stem})")
        for path, board in bases[mcu]
    ])
    base_path, base = next(item for item in bases[mcu] if item[1]["board"]["id"] == base_id)
    board_id = screen.prompt("Identity", "Board ID (lowercase letters, numbers, underscores)", "my_board")
    name = screen.prompt("Identity", "Board name", "My SolarOS Board")
    vendor = screen.prompt("Identity", "Vendor", "Custom")
    module = screen.prompt("Identity", "Module", base["board"]["module"])
    driver_names = screen.multi_choose("Built-in expansion drivers", [
        (driver.name, f"{driver.name:<18} {driver.summary}")
        for driver in catalog.values()
        if mcu in driver.targets
    ])
    configurator = Configurator(screen, base_path, base, catalog)
    profile = configurator.create(board_id, name, vendor, module, driver_names)
    merged = merge_board_overlay(base, profile)
    validate_board(merged, catalog)
    destination = args.manifest_dir / f"{board_id}.toml"
    if destination.exists() and not screen.yes_no("Overwrite", f"Overwrite {destination.name}?", False):
        raise ManifestError("profile was not written")
    content = render_overlay(profile)
    write_if_changed(destination, content)
    command = f"SOLAR_OS_BOARD={board_id} pio run -e {base_id}"
    screen.message("Profile created", f"Wrote {destination}\n\nBuild with:\n{command}")
    return destination, command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest-dir", type=Path, default=MANIFEST_DIR)
    parser.add_argument("--drivers", type=Path, default=DRIVER_CATALOG)
    parser.add_argument("--list", action="store_true", help="list available bases and drivers without starting curses")
    args = parser.parse_args()
    try:
        if args.list:
            drivers = load_driver_catalog(args.drivers)
            for mcu, bases in available_base_profiles(args.manifest_dir).items():
                print(f"{mcu} bases: " + ", ".join(board["board"]["id"] for _, board in bases))
                print(f"{mcu} drivers: " + ", ".join(driver.name for driver in drivers.values() if mcu in driver.targets))
            return 0
        destination, command = curses.wrapper(_run_tui, args)
    except KeyboardInterrupt:
        print("Board configuration cancelled.", file=sys.stderr)
        return 130
    except (OSError, ManifestError, curses.error) as exc:
        print(f"board config: {exc}", file=sys.stderr)
        return 1
    print(f"Wrote {destination}")
    print(f"Build with: {command}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
