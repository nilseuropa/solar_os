"""Shared desktop/build model for SolarOS source keymaps (standard library only)."""
from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import json
import math
from pathlib import Path

PROFILE_DIR = Path(__file__).resolve().parents[1] / "keymaps" / "profiles"
PROFILE_IDS = ("tca8418", "inputronic-keyboard", "lilygo-pager-keyboard")
FILE_MAX = 16384
OUTPUT_FIELDS = {"usage", "key", "shift_key", "raw", "layer_tap", "alt_block"}
ENTRY_FIELDS = OUTPUT_FIELDS | {"physical", "row", "col"}


class KeymapError(ValueError):
    """Invalid profile, selection, or mapping."""


def integer(value, maximum: int, field: str) -> int:
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            not 0 <= value <= maximum or not math.isfinite(value) or int(value) != value):
        raise KeymapError(f"{field}: expected an integer from 0 to {maximum}")
    return int(value)


def fields(value, allowed: set[str], context: str) -> None:
    if not isinstance(value, dict):
        raise KeymapError(f"{context}: expected an object")
    unknown = value.keys() - allowed
    if unknown:
        raise KeymapError(f"{context}: unknown fields: {', '.join(sorted(unknown))}")


def _pairs(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise KeymapError(f"duplicate JSON field: {name}")
        result[name] = value
    return result


def read_json(path: Path, *, mapping: bool = True):
    raw = path.read_bytes()
    if mapping and len(raw) > FILE_MAX:
        raise KeymapError(f"{path}: mapping exceeds 16 KiB")
    if b"\0" in raw:
        raise KeymapError(f"{path}: embedded NUL")
    text = raw.decode("utf-8")
    # Match the firmware's pre-parser nesting limit, before json.loads recurses.
    depth, string, escape = 0, False, False
    for char in text:
        if string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                string = False
        elif char == '"':
            string = True
        elif char in "[{":
            depth += 1
            if mapping and depth > 4:
                raise KeymapError(f"{path}: JSON nesting exceeds four levels")
        elif char in "]}":
            depth -= 1
    def reject_constant(value):
        raise KeymapError(f"invalid JSON number: {value}")
    return json.loads(text, object_pairs_hook=_pairs, parse_constant=reject_constant)


def output(entry: dict) -> dict:
    result = {}
    for name, maximum in (("usage", 0xe7), ("key", 255), ("shift_key", 255)):
        value = integer(entry.get(name, 0), maximum, name)
        if value:
            result[name] = value
    usage = result.get("usage", 0)
    if usage and not (4 <= usage <= 0xa4 or 0xe0 <= usage <= 0xe7):
        raise KeymapError(f"usage: unsupported keyboard HID usage {usage}")
    for flag in ("raw", "layer_tap", "alt_block"):
        if flag in entry and not isinstance(entry[flag], bool):
            raise KeymapError(f"{flag}: expected a boolean")
        if entry.get(flag):
            result[flag] = True
    if sum(bool(result.get(flag)) for flag in ("raw", "layer_tap", "alt_block")) > 1:
        raise KeymapError("raw, layer_tap and alt_block are mutually exclusive")
    if result.get("raw") and any(result.get(name) for name in ("usage", "key", "shift_key")):
        raise KeymapError("raw entries cannot specify logical output")
    if result.get("layer_tap") and (usage >= 0xe0 or usage == 0x39):
        raise KeymapError("layer_tap cannot use a modifier or Caps Lock usage")
    return result


@dataclass
class Profile:
    id: str
    label: str
    rows: int
    cols: int
    first: int
    stride: int
    names: dict[int, str]
    layers: list[dict[int, dict]]

    def physical(self, entry: dict) -> int:
        if "physical" in entry:
            if "row" in entry or "col" in entry:
                raise KeymapError("use physical or row/col, not both")
            physical = integer(entry["physical"], 65535, "physical")
        else:
            row = integer(entry.get("row"), self.rows - 1, "row")
            col = integer(entry.get("col"), self.cols - 1, "col")
            physical = self.first + row * self.stride + col
        if physical not in self.names:
            raise KeymapError(f"unknown physical key: {physical}")
        return physical

    def select(self, name: str) -> int:
        for physical, legend in self.names.items():
            if name.casefold() == legend.casefold():
                return physical
        if name.casefold().startswith("physical:"):
            try:
                return self.physical({"physical": int(name.split(":", 1)[1], 0)})
            except ValueError as exc:
                raise KeymapError(f"invalid physical selector: {name}") from exc
        raise KeymapError(f"unknown key name {name!r}; use show to list this profile")


def validate_layers(layers: list[dict[int, dict]]) -> None:
    selectors = 0
    for layer, entries in enumerate(layers):
        for entry in entries.values():
            output(entry)
            if entry.get("layer_tap"):
                if layer:
                    raise KeymapError("layer_tap is only allowed in the base layer")
                selectors += 1
    if selectors > 1:
        raise KeymapError("only one base-layer selector is allowed")


def merge(profile: Profile, document: dict, *, base=None) -> list[dict[int, dict]]:
    fields(document, {"schema", "keys", "symbols"}, "mapping")
    if integer(document.get("schema"), 1, "schema") != 1:
        raise KeymapError("schema must be 1")
    if "keys" not in document and "symbols" not in document:
        raise KeymapError("mapping requires keys or symbols")
    layers = deepcopy(profile.layers if base is None else base)
    for layer, name in enumerate(("keys", "symbols")):
        if name not in document:
            continue
        entries = document[name]
        if not isinstance(entries, list) or len(entries) > 256:
            raise KeymapError(f"{name}: expected an array of at most 256 entries")
        seen = set()
        for entry in entries:
            fields(entry, ENTRY_FIELDS, name)
            physical = profile.physical(entry)
            if physical in seen:
                raise KeymapError(f"duplicate {name} entry for physical {physical}")
            seen.add(physical)
            layers[layer][physical] = output(entry)
    validate_layers(layers)
    return layers


def load_profile(name: str, directory: Path = PROFILE_DIR, *, rows=None, cols=None) -> Profile:
    requested_rows, requested_cols = rows, cols
    if name not in PROFILE_IDS:
        raise KeymapError(f"unknown keyboard profile: {name}")
    document = read_json(directory / f"{name}.json", mapping=False)
    fields(document, {"schema", "profile", "label", "matrix", "keys", "symbols"}, "profile")
    if integer(document.get("schema"), 1, "schema") != 1 or document.get("profile") != name:
        raise KeymapError("profile identity/schema mismatch")
    matrix = document.get("matrix")
    fields(matrix, {"rows", "cols", "first", "stride"}, "matrix")
    rows = integer(matrix.get("rows"), 8, "rows")
    cols = integer(matrix.get("cols"), 10, "cols")
    first = integer(matrix.get("first"), 65535, "first")
    stride = integer(matrix.get("stride"), 65535, "stride")
    if not rows or not cols or first != 1 or stride != 10:
        raise KeymapError("TCA8418 profiles require first=1, stride=10 and nonzero geometry")
    if not isinstance(document.get("label"), str) or not document["label"]:
        raise KeymapError("profile requires a nonempty label")
    if not isinstance(document.get("keys"), list) or len(document["keys"]) > 256:
        raise KeymapError("profile keys must be an array of at most 256 entries")
    profile = Profile(name, document["label"], rows, cols, first, stride, {}, [{}, {}])
    entries = []
    legends = set()
    for entry in document["keys"]:
        fields(entry, ENTRY_FIELDS | {"name"}, "profile key")
        physical = integer(entry.get("physical"), 80, "physical")
        legend = entry.get("name")
        if (not isinstance(legend, str) or not legend or legend.casefold() in legends or
                physical in profile.names or not physical or
                (physical - 1) // stride >= rows or (physical - 1) % stride >= cols):
            raise KeymapError("profile key identity/name is invalid or duplicated")
        legends.add(legend.casefold())
        profile.names[physical] = legend
        entries.append({key: value for key, value in entry.items() if key != "name"})
    expected = {1 + row * stride + col for row in range(rows) for col in range(cols)}
    if profile.names.keys() != expected:
        raise KeymapError("profile must describe every matrix position")
    profile.layers = merge(profile, {"schema": 1, "keys": entries,
                                    "symbols": document.get("symbols", [])})
    if requested_rows is not None or requested_cols is not None:
        if name != "tca8418":
            raise KeymapError("custom geometry is only supported by the generic tca8418 profile")
        profile.rows = integer(profile.rows if requested_rows is None else requested_rows, profile.rows, "rows")
        profile.cols = integer(profile.cols if requested_cols is None else requested_cols, profile.cols, "cols")
        if not profile.rows or not profile.cols:
            raise KeymapError("rows and cols must be nonzero")
        selected = {1 + row * stride + col for row in range(profile.rows) for col in range(profile.cols)}
        profile.names = {physical: legend for physical, legend in profile.names.items() if physical in selected}
        profile.layers = [{physical: entry for physical, entry in layer.items() if physical in selected}
                          for layer in profile.layers]
    return profile


def sparse(profile: Profile, layers, *, include=None) -> dict:
    result = {"schema": 1, "keys": []}
    for layer, name in enumerate(("keys", "symbols")):
        entries = [{"physical": physical, **layers[layer].get(physical, {})}
                   for physical in sorted(profile.names)
                   if (layers[layer].get(physical, {}) != profile.layers[layer].get(physical, {}) or
                       include is not None and physical in include[layer])]
        if entries or name == "keys":
            result[name] = entries
    return result
