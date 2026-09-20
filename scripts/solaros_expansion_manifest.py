"""Load the portable runtime-expansion snapshot written by SolarOS."""

from __future__ import annotations

from pathlib import Path
import re
import tomllib
from typing import Any

from solaros_board_manifest import BOARD_ID_RE, BUS_PROTOCOLS, DEVICE_NAME_RE, ManifestError


SCHEMA_VERSION = 1
KIND = "solaros-expansion"
_TOP_LEVEL_KEYS = {"schema", "kind", "base", "buses", "devices"}
_BUS_KEYS = {
    "i2c": ({"name", "protocol", "sharing", "port", "sda", "scl", "speed_hz"}, set()),
    "spi": (
        {"name", "protocol", "sharing", "host", "sclk", "cs", "max_transfer_size"},
        {"miso", "mosi"},
    ),
    "uart": ({"name", "protocol", "sharing", "port", "tx", "rx", "baud_rate"}, set()),
    "midi": ({"name", "protocol", "sharing", "port", "tx", "rx", "baud_rate"}, set()),
    "onewire": ({"name", "protocol", "sharing", "pin"}, set()),
    "ps2": ({"name", "protocol", "sharing", "clock", "data"}, set()),
}
_PORT_PATTERNS = {
    "i2c": re.compile(r"^I2C_NUM_[01]$"),
    "spi": re.compile(r"^SPI[23]_HOST$"),
    "uart": re.compile(r"^UART_NUM_[0-2]$"),
    "midi": re.compile(r"^UART_NUM_[0-2]$"),
}


def _require_keys(table: dict[str, Any], required: set[str], optional: set[str], path: str) -> None:
    missing = sorted(required - set(table))
    unknown = sorted(set(table) - required - optional)
    if missing:
        raise ManifestError(f"{path} is missing: {', '.join(missing)}")
    if unknown:
        raise ManifestError(f"{path} has unsupported fields: {', '.join(unknown)}")


def load_expansion_manifest(path: Path) -> dict[str, Any]:
    """Load and validate a device-exported expansion manifest."""
    with path.open("rb") as file:
        data = tomllib.load(file)
    if data.get("schema") != SCHEMA_VERSION:
        raise ManifestError(
            f"{path}: unsupported expansion snapshot schema {data.get('schema')!r}; "
            f"expected {SCHEMA_VERSION}"
        )
    if data.get("kind") != KIND:
        raise ManifestError(f"{path}: kind must be {KIND!r}")
    unknown = sorted(set(data) - _TOP_LEVEL_KEYS)
    if unknown:
        raise ManifestError(f"{path}: unsupported top-level fields: {', '.join(unknown)}")

    base = data.get("base")
    if not isinstance(base, dict):
        raise ManifestError(f"{path}: [base] is required")
    _require_keys(base, {"board", "firmware"}, set(), "base")
    if not isinstance(base["board"], str) or not BOARD_ID_RE.fullmatch(base["board"]):
        raise ManifestError("base.board must be a SolarOS board ID")
    if not isinstance(base["firmware"], str) or not base["firmware"]:
        raise ManifestError("base.firmware is required")

    buses = data.get("buses", [])
    if not isinstance(buses, list):
        raise ManifestError("buses must be an array of tables")
    bus_names: set[str] = set()
    for index, bus in enumerate(buses):
        item_path = f"buses[{index}]"
        if not isinstance(bus, dict):
            raise ManifestError(f"{item_path} must be a table")
        protocol = bus.get("protocol")
        if protocol not in BUS_PROTOCOLS or protocol not in _BUS_KEYS:
            raise ManifestError(f"{item_path}.protocol is unsupported")
        required, optional = _BUS_KEYS[protocol]
        _require_keys(bus, required, optional, item_path)
        name = bus["name"]
        if not isinstance(name, str) or not DEVICE_NAME_RE.fullmatch(name):
            raise ManifestError(f"{item_path}.name is invalid")
        if name in bus_names:
            raise ManifestError(f"bus {name} is declared more than once")
        bus_names.add(name)
        if bus["sharing"] not in {"shared", "exclusive"}:
            raise ManifestError(f"bus {name}.sharing must be shared or exclusive")
        endpoint_key = "host" if protocol == "spi" else "port"
        pattern = _PORT_PATTERNS.get(protocol)
        if pattern is not None:
            endpoint = bus[endpoint_key]
            if not isinstance(endpoint, str) or not pattern.fullmatch(endpoint):
                raise ManifestError(f"bus {name}.{endpoint_key} is invalid")
        integer_fields = (required | optional) - {
            "name", "protocol", "sharing", "port", "host", "cs"
        }
        for key in integer_fields:
            if key in bus and not isinstance(bus[key], int):
                raise ManifestError(f"bus {name}.{key} must be an integer")
        if protocol == "spi" and (
            not isinstance(bus["cs"], list) or
            not all(isinstance(value, int) for value in bus["cs"])
        ):
            raise ManifestError(f"bus {name}.cs must be a list of integers")

    devices = data.get("devices", [])
    if not isinstance(devices, list):
        raise ManifestError("devices must be an array of tables")
    device_names: set[str] = set()
    for index, device in enumerate(devices):
        item_path = f"devices[{index}]"
        if not isinstance(device, dict):
            raise ManifestError(f"{item_path} must be a table")
        _require_keys(device, {"driver", "name", "bindings"}, set(), item_path)
        driver = device["driver"]
        name = device["name"]
        if not isinstance(driver, str) or not DEVICE_NAME_RE.fullmatch(driver):
            raise ManifestError(f"{item_path}.driver is invalid")
        if driver == "manual":
            raise ManifestError(f"device {name} uses manual bindings and cannot become board-owned")
        if not isinstance(name, str) or not DEVICE_NAME_RE.fullmatch(name):
            raise ManifestError(f"{item_path}.name is invalid")
        if name in device_names:
            raise ManifestError(f"device {name} is declared more than once")
        device_names.add(name)
        bindings = device["bindings"]
        if not isinstance(bindings, dict) or any(
            not isinstance(key, str) or not key or not isinstance(value, (str, int))
            for key, value in bindings.items()
        ):
            raise ManifestError(f"device {name}.bindings must contain string or integer values")

    data["_source"] = str(path.resolve())
    return data
