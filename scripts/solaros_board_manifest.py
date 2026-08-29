#!/usr/bin/env python3
"""Load, validate, and compile declarative SolarOS board manifests."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path
import re
import tomllib
from typing import Any


SCHEMA_VERSION = 1
BOARD_ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")
DEVICE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
DEFINE_NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
HEADER_INCLUDE_RE = re.compile(r"^[A-Za-z0-9_./-]+\.h$")
TARGET_GPIO_MAX = {"esp32": 39, "esp32s3": 48}
POLICIES = {"free", "releasable", "fixed"}
BUS_PROTOCOLS = {"i2c", "spi", "uart", "onewire", "ps2"}
BINDING_KINDS = {
    "gpio",
    "adc",
    "pwm",
    "i2s_port",
    "i2c_bus",
    "i2c_address",
    "spi_bus",
    "spi_cs",
    "uart_port",
    "ps2_bus",
    "scalar_stream",
    "parameter",
}
PIN_BINDING_KINDS = {"gpio", "adc", "pwm", "spi_cs"}
TARGET_BINDING_KINDS = {
    "i2c_bus",
    "spi_bus",
    "uart_port",
    "ps2_bus",
    "scalar_stream",
}
KEYED_LISTS = {
    ("buses",): "name",
    ("devices",): "name",
    ("pins",): "gpio",
    ("connectors",): ("connector", "position"),
}
UNION_LISTS = {
    ("build", "drivers"),
    ("build", "capabilities"),
    ("build", "required_packages"),
}


class ManifestError(ValueError):
    """A board or expansion manifest is invalid."""


@dataclass(frozen=True)
class DriverBinding:
    key: str
    kind: str
    hint: str
    role: str
    required: bool
    allowed: tuple[int, ...]
    minimum: int | None
    maximum: int | None


@dataclass(frozen=True)
class DriverDef:
    name: str
    summary: str
    package: str
    targets: tuple[str, ...]
    capabilities: tuple[str, ...]
    board_capabilities: tuple[str, ...]
    board_driver: str
    board_defines: dict[str, str]
    early: bool
    default_name: str
    bindings: tuple[DriverBinding, ...]


def _string_list(value: Any, path: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ManifestError(f"{path} must be a list of strings")
    return list(value)


def _integer_list(value: Any, path: str) -> list[int]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(item, int) for item in value):
        raise ManifestError(f"{path} must be a list of integers")
    return list(value)


def load_driver_catalog(path: Path) -> dict[str, DriverDef]:
    with path.open("rb") as file:
        data = tomllib.load(file)
    if data.get("schema") != SCHEMA_VERSION:
        raise ManifestError(
            f"{path}: unsupported expansion schema {data.get('schema')!r}; "
            f"expected {SCHEMA_VERSION}"
        )
    raw_drivers = data.get("drivers")
    if not isinstance(raw_drivers, dict) or not raw_drivers:
        raise ManifestError(f"{path}: [drivers] is empty")

    result: dict[str, DriverDef] = {}
    for name, raw in raw_drivers.items():
        prefix = f"drivers.{name}"
        if not isinstance(raw, dict):
            raise ManifestError(f"{prefix} must be a table")
        if not DEVICE_NAME_RE.fullmatch(name):
            raise ManifestError(f"{prefix} has an invalid driver name")
        package = raw.get("package")
        if not isinstance(package, str) or not package:
            raise ManifestError(f"{prefix}.package is required")
        targets = tuple(_string_list(raw.get("targets"), f"{prefix}.targets"))
        if not targets or any(target not in TARGET_GPIO_MAX for target in targets):
            raise ManifestError(f"{prefix}.targets must select esp32 and/or esp32s3")
        raw_bindings = raw.get("bindings", [])
        if not isinstance(raw_bindings, list):
            raise ManifestError(f"{prefix}.bindings must be an array of tables")
        bindings: list[DriverBinding] = []
        seen_keys: set[str] = set()
        for index, binding in enumerate(raw_bindings):
            binding_path = f"{prefix}.bindings[{index}]"
            if not isinstance(binding, dict):
                raise ManifestError(f"{binding_path} must be a table")
            key = binding.get("key")
            kind = binding.get("kind")
            if not isinstance(key, str) or not key or key in seen_keys:
                raise ManifestError(f"{binding_path}.key is missing or duplicated")
            if kind not in BINDING_KINDS:
                raise ManifestError(f"{binding_path}.kind is invalid")
            seen_keys.add(key)
            allowed = tuple(_integer_list(binding.get("allowed"), f"{binding_path}.allowed"))
            minimum = binding.get("min")
            maximum = binding.get("max")
            if minimum is not None and not isinstance(minimum, int):
                raise ManifestError(f"{binding_path}.min must be an integer")
            if maximum is not None and not isinstance(maximum, int):
                raise ManifestError(f"{binding_path}.max must be an integer")
            if minimum is not None and maximum is not None and minimum > maximum:
                raise ManifestError(f"{binding_path} has an inverted value range")
            default_role = key if kind in {
                "gpio", "adc", "pwm", "parameter", "scalar_stream"
            } else ""
            bindings.append(DriverBinding(
                key=key,
                kind=kind,
                hint=str(binding.get("hint") or "value"),
                role=str(binding.get("role") or default_role),
                required=bool(binding.get("required", False)),
                allowed=allowed,
                minimum=minimum,
                maximum=maximum,
            ))
        raw_board_defines = raw.get("board_defines", {})
        if not isinstance(raw_board_defines, dict):
            raise ManifestError(f"{prefix}.board_defines must be a table")
        if any(not isinstance(key, str) or not DEFINE_NAME_RE.fullmatch(key)
               for key in raw_board_defines):
            raise ManifestError(f"{prefix}.board_defines contains an invalid macro name")
        result[name] = DriverDef(
            name=name,
            summary=str(raw.get("summary") or name),
            package=package,
            targets=targets,
            capabilities=tuple(_string_list(raw.get("capabilities"), f"{prefix}.capabilities")),
            board_capabilities=tuple(_string_list(
                raw.get("board_capabilities"),
                f"{prefix}.board_capabilities",
            )),
            board_driver=str(raw.get("board_driver") or ""),
            board_defines={
                str(key): str(value)
                for key, value in raw_board_defines.items()
            },
            early=bool(raw.get("early", False)),
            default_name=str(raw.get("default_name") or name.replace("-", "")),
            bindings=tuple(bindings),
        )
    return result


def _item_key(item: dict[str, Any], key_spec: str | tuple[str, ...], path: str) -> Any:
    keys = (key_spec,) if isinstance(key_spec, str) else key_spec
    try:
        return tuple(item[key] for key in keys) if len(keys) > 1 else item[keys[0]]
    except KeyError as exc:
        raise ManifestError(f"{path} item is missing {exc.args[0]}") from exc


def merge_board_overlay(base: Any, overlay: Any, path: tuple[str, ...] = ()) -> Any:
    """Merge an inherited board overlay into its resolved base manifest."""
    if isinstance(base, dict) and isinstance(overlay, dict):
        result = deepcopy(base)
        for key, value in overlay.items():
            if key == "extends":
                continue
            result[key] = (
                merge_board_overlay(result[key], value, path + (key,))
                if key in result else deepcopy(value)
            )
        return result
    if isinstance(base, list) and isinstance(overlay, list):
        if path in UNION_LISTS:
            return list(dict.fromkeys([*base, *overlay]))
        key_spec = KEYED_LISTS.get(path)
        if key_spec is None:
            return deepcopy(overlay)
        result = deepcopy(base)
        indexes = {
            _item_key(item, key_spec, ".".join(path)): index
            for index, item in enumerate(result)
        }
        for item in overlay:
            if not isinstance(item, dict):
                raise ManifestError(f"{'.'.join(path)} must contain tables")
            key = _item_key(item, key_spec, ".".join(path))
            if item.get("remove") is True:
                if key in indexes:
                    result.pop(indexes[key])
                    indexes = {
                        _item_key(existing, key_spec, ".".join(path)): index
                        for index, existing in enumerate(result)
                    }
                continue
            if key in indexes:
                result[indexes[key]] = merge_board_overlay(
                    result[indexes[key]], item, path + (str(key),)
                )
            else:
                result.append(deepcopy(item))
                indexes[key] = len(result) - 1
        return result
    return deepcopy(overlay)


def load_board_manifest(path: Path, manifest_dir: Path | None = None) -> dict[str, Any]:
    manifest_dir = manifest_dir or path.parent
    seen: set[Path] = set()

    def load(current: Path) -> dict[str, Any]:
        current = current.resolve()
        if current in seen:
            raise ManifestError(f"board manifest inheritance cycle at {current}")
        seen.add(current)
        with current.open("rb") as file:
            raw = tomllib.load(file)
        if raw.get("schema") != SCHEMA_VERSION:
            raise ManifestError(
                f"{current}: unsupported board schema {raw.get('schema')!r}; "
                f"expected {SCHEMA_VERSION}"
            )
        parent = raw.get("extends")
        if parent is None:
            resolved = deepcopy(raw)
        else:
            if not isinstance(parent, str) or not BOARD_ID_RE.fullmatch(parent):
                raise ManifestError(f"{current}: extends must be a board ID")
            parent_path = manifest_dir / f"{parent}.toml"
            if not parent_path.is_file():
                raise ManifestError(f"{current}: parent board does not exist: {parent_path}")
            resolved = merge_board_overlay(load(parent_path), raw)
        seen.remove(current)
        resolved["_source"] = str(current)
        return resolved

    return load(path)


def _require_table(data: dict[str, Any], name: str) -> dict[str, Any]:
    value = data.get(name)
    if not isinstance(value, dict):
        raise ManifestError(f"[{name}] is required")
    return value


def _pin_map(board: dict[str, Any]) -> dict[int, dict[str, Any]]:
    raw_pins = board.get("pins", [])
    if not isinstance(raw_pins, list):
        raise ManifestError("pins must be an array of tables")
    result: dict[int, dict[str, Any]] = {}
    for index, pin in enumerate(raw_pins):
        if not isinstance(pin, dict) or not isinstance(pin.get("gpio"), int):
            raise ManifestError(f"pins[{index}].gpio must be an integer")
        gpio = pin["gpio"]
        if gpio in result:
            raise ManifestError(f"GPIO{gpio} is declared more than once")
        result[gpio] = pin
    return result


def _bus_gpio_roles(bus: dict[str, Any]) -> list[tuple[int, str]]:
    protocol = bus.get("protocol")
    roles: list[tuple[int, str]] = []
    role_names = {
        "i2c": ("sda", "scl"),
        "spi": ("sclk", "miso", "mosi"),
        "uart": ("tx", "rx"),
        "onewire": ("pin",),
        "ps2": ("clock", "data"),
    }.get(protocol, ())
    for role in role_names:
        value = bus.get(role)
        if value is not None:
            if not isinstance(value, int):
                raise ManifestError(f"bus {bus.get('name')}: {role} must be a GPIO number")
            roles.append((value, f"{bus.get('name')} {role}"))
    if protocol == "spi":
        for value in _integer_list(bus.get("cs"), f"bus {bus.get('name')}.cs"):
            roles.append((value, f"{bus.get('name')} cs"))
    return roles


def validate_board(board: dict[str, Any], drivers: dict[str, DriverDef]) -> None:
    identity = _require_table(board, "board")
    target = _require_table(board, "target")
    build = _require_table(board, "build")
    board_id = identity.get("id")
    if not isinstance(board_id, str) or not BOARD_ID_RE.fullmatch(board_id):
        raise ManifestError("board.id must use lowercase letters, numbers, and underscores")
    for key in ("name", "vendor", "module"):
        if not isinstance(identity.get(key), str) or not identity[key]:
            raise ManifestError(f"board.{key} is required")
    mcu = target.get("mcu")
    if mcu not in TARGET_GPIO_MAX:
        raise ManifestError("target.mcu must be esp32 or esp32s3")
    if not isinstance(target.get("platformio_board"), str) or not target["platformio_board"]:
        raise ManifestError("target.platformio_board is required")
    capabilities = set(_string_list(build.get("capabilities"), "build.capabilities"))
    _string_list(build.get("drivers"), "build.drivers")
    _string_list(build.get("required_packages"), "build.required_packages")
    psram_bytes = build.get("psram_bytes", 0)
    if not isinstance(psram_bytes, int) or psram_bytes < 0:
        raise ManifestError("build.psram_bytes must be a non-negative integer")
    if ("psram" in capabilities) != (psram_bytes > 0):
        raise ManifestError("PSRAM capability and build.psram_bytes disagree")

    header = board.get("header", {})
    if not isinstance(header, dict):
        raise ManifestError("header must be a table")
    includes = _string_list(header.get("includes"), "header.includes")
    if any(not HEADER_INCLUDE_RE.fullmatch(include) for include in includes):
        raise ManifestError("header.includes contains an invalid header path")

    pins = _pin_map(board)
    max_gpio = TARGET_GPIO_MAX[mcu]
    for gpio, pin in pins.items():
        if gpio < 0 or gpio > max_gpio:
            raise ManifestError(f"GPIO{gpio} is invalid for {mcu}")
        policy = pin.get("policy")
        if policy not in POLICIES:
            raise ManifestError(f"GPIO{gpio} has invalid policy {policy!r}")
        if not isinstance(pin.get("role"), str) or not pin["role"]:
            raise ManifestError(f"GPIO{gpio} needs a role")
        expansion = bool(pin.get("expansion", False))
        user = bool(pin.get("user", policy == "free"))
        if user and (not expansion or policy != "free"):
            raise ManifestError(f"GPIO{gpio}: user pins must be free expansion pins")
        if (pin.get("adc") or pin.get("pwm")) and not user:
            raise ManifestError(f"GPIO{gpio}: ADC/PWM expansion requires a user pin")

    raw_buses = board.get("buses", [])
    if not isinstance(raw_buses, list):
        raise ManifestError("buses must be an array of tables")
    buses: dict[str, dict[str, Any]] = {}
    bus_signal_owners: dict[int, str] = {}
    spi_cs: dict[str, set[int]] = {}
    for index, bus in enumerate(raw_buses):
        if not isinstance(bus, dict):
            raise ManifestError(f"buses[{index}] must be a table")
        name = bus.get("name")
        protocol = bus.get("protocol")
        if not isinstance(name, str) or not DEVICE_NAME_RE.fullmatch(name):
            raise ManifestError(f"buses[{index}].name is invalid")
        if name in buses:
            raise ManifestError(f"bus {name} is declared more than once")
        if protocol not in BUS_PROTOCOLS:
            raise ManifestError(f"bus {name} has invalid protocol {protocol!r}")
        buses[name] = bus
        if protocol == "spi":
            cs_values = _integer_list(bus.get("cs"), f"bus {name}.cs")
            if len(cs_values) > 5:
                raise ManifestError(f"bus {name} has more than 5 SPI CS slots")
            spi_cs[name] = set(cs_values)
        for gpio, role in _bus_gpio_roles(bus):
            if gpio not in pins:
                raise ManifestError(f"{role} GPIO{gpio} is absent from pins")
            if role.endswith(" cs"):
                continue
            previous = bus_signal_owners.get(gpio)
            if previous is not None:
                raise ManifestError(f"GPIO{gpio} is shared by {previous} and {role}")
            bus_signal_owners[gpio] = role
            if pins[gpio].get("policy") == "free":
                raise ManifestError(f"{role} GPIO{gpio} must not be a free pin")

    raw_devices = board.get("devices", [])
    if not isinstance(raw_devices, list):
        raise ManifestError("devices must be an array of tables")
    device_names: set[str] = set()
    direct_pin_owners: dict[int, str] = {}
    cs_owners: dict[tuple[str, int], str] = {}
    for index, device in enumerate(raw_devices):
        if not isinstance(device, dict):
            raise ManifestError(f"devices[{index}] must be a table")
        name = device.get("name")
        driver_name = device.get("driver")
        if not isinstance(name, str) or not DEVICE_NAME_RE.fullmatch(name):
            raise ManifestError(f"devices[{index}].name is invalid")
        if name in device_names:
            raise ManifestError(f"device {name} is declared more than once")
        device_names.add(name)
        driver = drivers.get(driver_name)
        if driver is None:
            raise ManifestError(f"device {name} uses unknown driver {driver_name!r}")
        if mcu not in driver.targets:
            raise ManifestError(f"driver {driver.name} does not support {mcu}")
        missing_caps = set(driver.capabilities) - capabilities
        if missing_caps:
            raise ManifestError(
                f"device {name} requires capabilities: {', '.join(sorted(missing_caps))}"
            )
        bindings = device.get("bindings")
        if not isinstance(bindings, dict):
            raise ManifestError(f"device {name}.bindings must be a table")
        specs = {binding.key: binding for binding in driver.bindings}
        missing = [binding.key for binding in driver.bindings
                   if binding.required and binding.key not in bindings]
        unknown = sorted(set(bindings) - set(specs))
        if missing:
            raise ManifestError(f"device {name} is missing bindings: {', '.join(missing)}")
        if unknown:
            raise ManifestError(f"device {name} has unknown bindings: {', '.join(unknown)}")
        for key, value in bindings.items():
            spec = specs[key]
            if spec.kind in TARGET_BINDING_KINDS:
                if not isinstance(value, str) or not value:
                    raise ManifestError(f"device {name}.{key} must name a resource")
                expected_protocol = {
                    "i2c_bus": "i2c",
                    "spi_bus": "spi",
                    "uart_port": "uart",
                    "ps2_bus": "ps2",
                }.get(spec.kind)
                if expected_protocol is not None and (
                    value not in buses or buses[value].get("protocol") != expected_protocol
                ):
                    raise ManifestError(
                        f"device {name}.{key} must name a {expected_protocol} bus"
                    )
                continue
            if not isinstance(value, int):
                raise ManifestError(f"device {name}.{key} must be an integer")
            if spec.allowed and value not in spec.allowed:
                raise ManifestError(f"device {name}.{key} is not an allowed value")
            if spec.minimum is not None and value < spec.minimum:
                raise ManifestError(f"device {name}.{key} is below {spec.minimum}")
            if spec.maximum is not None and value > spec.maximum:
                raise ManifestError(f"device {name}.{key} is above {spec.maximum}")
            if spec.kind in PIN_BINDING_KINDS:
                if value not in pins:
                    raise ManifestError(f"device {name}.{key} GPIO{value} is absent from pins")
                if pins[value].get("policy") == "free":
                    raise ManifestError(f"device {name}.{key} GPIO{value} must not be free")
            if spec.kind == "spi_cs":
                bus_name = bindings.get("spi")
                if not isinstance(bus_name, str) or value not in spi_cs.get(bus_name, set()):
                    raise ManifestError(
                        f"device {name}.{key} GPIO{value} is not a CS slot on its SPI bus"
                    )
                owner_key = (bus_name, value)
                if owner_key in cs_owners:
                    raise ManifestError(
                        f"{bus_name} CS GPIO{value} is shared by {cs_owners[owner_key]} and {name}"
                    )
                cs_owners[owner_key] = name
            elif spec.kind in PIN_BINDING_KINDS:
                previous = direct_pin_owners.get(value) or bus_signal_owners.get(value)
                if previous is not None:
                    raise ManifestError(f"GPIO{value} is shared by {previous} and {name}.{key}")
                direct_pin_owners[value] = f"{name}.{key}"


def required_packages(board: dict[str, Any], drivers: dict[str, DriverDef]) -> list[str]:
    build = _require_table(board, "build")
    packages = _string_list(build.get("required_packages"), "build.required_packages")
    for device in board.get("devices", []):
        driver = drivers[device["driver"]]
        if driver.package not in packages:
            packages.append(driver.package)
    return packages


def _c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _macro_lines(name: str, entries: list[str], empty: str = "{{0}}") -> list[str]:
    if not entries:
        return [f"#define {name} {empty}"]
    lines = [f"#define {name} {{ \\"]
    lines.extend(f"    {entry} \\" for entry in entries)
    lines.append("}")
    return lines


def _mask_expression(gpios: list[int]) -> str:
    if not gpios:
        return "0ULL"
    return "(" + " | ".join(f"(1ULL << GPIO_NUM_{gpio})" for gpio in gpios) + ")"


def _bus_initializer(bus: dict[str, Any]) -> str:
    name = _c_string(bus["name"])
    protocol = bus["protocol"]
    sharing = str(bus.get("sharing", "shared")).upper()
    common = (
        f"{{.name = {name}, .protocol = SOLAR_OS_BUS_PROTOCOL_{protocol.upper()}, "
        f".origin = SOLAR_OS_BUS_ORIGIN_BOARD, .sharing = SOLAR_OS_BUS_{sharing}, "
    )
    if protocol == "i2c":
        config = (
            ".config.i2c = {"
            f".port = {bus['port']}, .sda_pin = GPIO_NUM_{bus['sda']}, "
            f".scl_pin = GPIO_NUM_{bus['scl']}, "
            f".speed_hz = {bus.get('speed_hz', 'SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ')}"
            "}"
        )
    elif protocol == "spi":
        cs = _integer_list(bus.get("cs"), f"bus {bus['name']}.cs")
        miso = bus.get("miso")
        mosi = bus.get("mosi")
        miso_value = "GPIO_NUM_NC" if miso is None else f"GPIO_NUM_{miso}"
        mosi_value = "GPIO_NUM_NC" if mosi is None else f"GPIO_NUM_{mosi}"
        cs_entries = ", ".join(
            f"{{.name = \"gpio{gpio}\", .pin = GPIO_NUM_{gpio}}}" for gpio in cs
        )
        config = (
            ".config.spi = {"
            f".host = {bus['host']}, .sclk_pin = GPIO_NUM_{bus['sclk']}, "
            f".miso_pin = {miso_value}, .mosi_pin = {mosi_value}, "
            f".max_transfer_size = {bus.get('max_transfer_size', 4096)}, "
            f".cs_count = {len(cs)}, .cs = {{{cs_entries}}}"
            "}"
        )
    elif protocol == "uart":
        config = (
            ".config.uart = {"
            f".port = {bus['port']}, .tx_pin = GPIO_NUM_{bus['tx']}, "
            f".rx_pin = GPIO_NUM_{bus['rx']}, "
            f".baud_rate = {bus.get('baud_rate', 'SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE')}"
            "}"
        )
    elif protocol == "onewire":
        config = f".config.onewire = {{.pin = GPIO_NUM_{bus['pin']}}}"
    elif protocol == "ps2":
        config = (
            ".config.ps2 = {"
            f".clock_pin = GPIO_NUM_{bus['clock']}, .data_pin = GPIO_NUM_{bus['data']}"
            "}"
        )
    else:
        raise ManifestError(f"cannot generate bus protocol {protocol}")
    return common + config + "}"


def _binding_initializer(spec: DriverBinding, value: Any, bindings: dict[str, Any]) -> str:
    kind = f"SOLAR_OS_EXPANSION_BINDING_{spec.kind.upper()}"
    fields = [f".kind = {kind}"]
    if spec.role:
        fields.append(f".role = {_c_string(spec.role)}")
    if spec.kind in TARGET_BINDING_KINDS:
        fields.append(f".target = {_c_string(value)}")
    elif spec.kind == "spi_cs":
        fields.append(f".target = {_c_string(str(bindings['spi']))}")
        fields.append(f".value = {value}")
    else:
        fields.append(f".value = {value}")
    return "{" + ", ".join(fields) + "}"


def _device_initializer(device: dict[str, Any], drivers: dict[str, DriverDef]) -> str:
    driver = drivers[device["driver"]]
    bindings = device["bindings"]
    entries = [
        _binding_initializer(spec, bindings[spec.key], bindings)
        for spec in driver.bindings
        if spec.key in bindings
    ]
    return (
        "{.driver = " + _c_string(driver.name) + ", .name = " + _c_string(device["name"]) +
        f", .binding_count = {len(entries)}, .bindings = {{" + ", ".join(entries) + "}}"
    )


def generate_header(board: dict[str, Any], drivers: dict[str, DriverDef]) -> str:
    validate_board(board, drivers)
    identity = board["board"]
    pins = _pin_map(board)
    includes = [
        "driver/gpio.h",
        "driver/i2c_types.h",
        "driver/i2s_types.h",
        "driver/spi_master.h",
        "driver/uart.h",
        "solar_os_bus_types.h",
        "solar_os_pin_types.h",
        *_string_list(board.get("header", {}).get("includes"), "header.includes"),
    ]
    lines = [
        "/* Generated by scripts/generate_board_profile.py. Do not edit. */",
        "#pragma once",
        "",
        *[f'#include "{include}"' for include in dict.fromkeys(includes)],
        "",
        f"#define SOLAR_OS_BOARD_ID {_c_string(identity['id'])}",
        f"#define SOLAR_OS_BOARD_NAME {_c_string(identity['name'])}",
        f"#define SOLAR_OS_BOARD_VENDOR {_c_string(identity['vendor'])}",
        f"#define SOLAR_OS_BOARD_MODULE_NAME {_c_string(identity['module'])}",
    ]
    for name, value in board.get("defines", {}).items():
        lines.append(f"#define {name} {value}")

    runtime = board.get("runtime", {})
    for key, macro, prefix in (
        ("spi_hosts", "SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK", "SPI"),
        ("uart_ports", "SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK", "UART"),
        ("i2s_ports", "SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK", "I2S"),
    ):
        values = _string_list(runtime.get(key), f"runtime.{key}")
        expression = " | ".join(f"(1U << {value})" for value in values)
        expression = f"({expression})" if expression else "0U"
        lines.append(f"#define {macro} {expression}")

    buses = board.get("buses", [])
    spi_cs_values = sorted({gpio for bus in buses if bus.get("protocol") == "spi"
                            for gpio in _integer_list(bus.get("cs"), f"bus {bus.get('name')}.cs")})
    lines.extend(_macro_lines(
        "SOLAR_OS_BOARD_SPI_CS_SLOTS",
        [f'{{.pin = GPIO_NUM_{gpio}, .name = "gpio{gpio}"}},' for gpio in spi_cs_values],
    ))
    lines.extend(_macro_lines(
        "SOLAR_OS_BOARD_BUSES",
        [_bus_initializer(bus) + "," for bus in buses],
    ))

    devices = board.get("devices", [])
    lines.append(f"#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT {len(devices)}")
    lines.extend(_macro_lines(
        "SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES",
        [_device_initializer(device, drivers) + "," for device in devices],
    ))

    connector = board.get("connector", {})
    connectors = board.get("connectors", [])
    if connector or connectors:
        lines.extend([
            f"#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE {_c_string(str(connector.get('title', 'Expansion connectors')))}",
            f"#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW {_c_string(str(connector.get('view', 'component-side view')))}",
            f"#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS {int(connector.get('rows', 0))}",
            f"#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS {int(connector.get('columns', 0))}",
            f"#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT {len(connectors)}",
        ])
        connector_entries = []
        for entry in connectors:
            pin = entry.get("gpio", -1)
            pin_value = f"GPIO_NUM_{pin}" if isinstance(pin, int) and pin >= 0 else "-1"
            connector_entries.append(
                "{.connector = " + _c_string(str(entry["connector"])) +
                f", .position = {entry['position']}, .row = {entry['row']}, "
                f".column = {entry['column']}, .pin = {pin_value}, "
                f".kind = SOLAR_OS_CONNECTOR_PIN_{str(entry['kind']).upper()}, "
                f".label = {_c_string(str(entry['label']))}}},"
            )
        lines.extend(_macro_lines("SOLAR_OS_BOARD_CONNECTOR_PINS", connector_entries))

    expansion = sorted(gpio for gpio, pin in pins.items() if pin.get("expansion", False))
    users = sorted(gpio for gpio, pin in pins.items()
                   if pin.get("user", pin.get("policy") == "free"))
    adc = sorted(gpio for gpio, pin in pins.items() if pin.get("adc", False))
    pwm = sorted(gpio for gpio, pin in pins.items() if pin.get("pwm", False))
    lines.extend([
        f"#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK {_mask_expression(expansion)}",
        f"#define SOLAR_OS_BOARD_USER_GPIO_MASK {_mask_expression(users)}",
        f"#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST {_c_string(' '.join(map(str, expansion)))}",
        f"#define SOLAR_OS_BOARD_USER_GPIO_LIST {_c_string(' '.join(map(str, users)))}",
        f"#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK {_mask_expression(adc)}",
        f"#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK {_mask_expression(pwm)}",
    ])
    pin_entries = [
        f'{{.pin = {gpio}, .policy = SOLAR_OS_PIN_POLICY_{str(pin["policy"]).upper()}, '
        f'.role = {_c_string(str(pin["role"]))}}},'
        for gpio, pin in sorted(pins.items())
    ]
    lines.extend(_macro_lines("SOLAR_OS_BOARD_GPIO_SLOTS", pin_entries))
    lines.append("")
    return "\n".join(lines)


def generate_cmake(board: dict[str, Any], drivers: dict[str, DriverDef]) -> str:
    validate_board(board, drivers)
    identity = board["board"]
    build = board["build"]
    define = "SOLAR_OS_BOARD_" + re.sub(r"[^A-Z0-9]", "_", identity["id"].upper())
    lines = [
        "# Generated by scripts/generate_board_profile.py. Do not edit.",
        f'set(SOLAR_OS_BOARD_ID "{identity["id"]}")',
        f'set(SOLAR_OS_BOARD_NAME "{identity["name"]}")',
        f'set(SOLAR_OS_BOARD_DEFINE "{define}")',
        "set(SOLAR_OS_BOARD_GENERATED ON)",
    ]
    for fragment in _string_list(build.get("drivers"), "build.drivers"):
        if not BOARD_ID_RE.fullmatch(fragment):
            raise ManifestError(f"invalid board driver fragment {fragment!r}")
        lines.append(
            f'include("${{SOLAR_OS_PROJECT_DIR}}/boards/drivers/{fragment}.cmake")'
        )
    packages = required_packages(board, drivers)
    if packages:
        lines.append("list(APPEND SOLAR_OS_BOARD_REQUIRED_PACKAGES")
        lines.extend(f"    {package}" for package in packages)
        lines.append(")")
    capabilities = set(_string_list(build.get("capabilities"), "build.capabilities"))
    for capability in sorted(capabilities):
        lines.append(f"set(SOLAR_OS_BOARD_HAS_{capability.upper()} ON)")
    lines.append(f"set(SOLAR_OS_BOARD_PSRAM_BYTES {int(build.get('psram_bytes', 0))})")
    lines.append("")
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
