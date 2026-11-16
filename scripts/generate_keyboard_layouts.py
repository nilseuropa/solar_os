#!/usr/bin/env python3
"""Generate keymaps/layouts/<name>.json from xkeyboard-config.

Host tool, not part of the firmware build. It needs the exact libxkbcommon,
xkeyboard-config and libX11 versions in REQUIRED_VERSIONS, found through
their pkg-config files. See keymaps/layouts/README.md.

    scripts/generate_keyboard_layouts.py de                 # layout "de"
    scripts/generate_keyboard_layouts.py fr --xkb fr        # explicit XKB name
    scripts/generate_keyboard_layouts.py ch-fr --xkb ch --variant fr
    scripts/generate_keyboard_layouts.py --all              # regenerate all
"""
from __future__ import annotations

import argparse
import ctypes
import glob
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
LAYOUT_DIR = ROOT / "keymaps" / "layouts"

# HID keyboard usages whose meaning depends on the language layout, and the
# Linux evdev key code the kernel's HID driver reports for each one. XKB key
# codes are evdev codes + 8. Enter, Escape, Backspace and Tab (0x28-0x2b) are
# the same in every layout and stay in C.
HID_TO_EVDEV = {
    0x04: 30, 0x05: 48, 0x06: 46, 0x07: 32, 0x08: 18, 0x09: 33, 0x0A: 34,
    0x0B: 35, 0x0C: 23, 0x0D: 36, 0x0E: 37, 0x0F: 38, 0x10: 50, 0x11: 49,
    0x12: 24, 0x13: 25, 0x14: 16, 0x15: 19, 0x16: 31, 0x17: 20, 0x18: 22,
    0x19: 47, 0x1A: 17, 0x1B: 45, 0x1C: 21, 0x1D: 44,
    0x1E: 2, 0x1F: 3, 0x20: 4, 0x21: 5, 0x22: 6, 0x23: 7, 0x24: 8, 0x25: 9,
    0x26: 10, 0x27: 11,
    0x2C: 57, 0x2D: 12, 0x2E: 13, 0x2F: 26, 0x30: 27, 0x31: 43,
    0x32: 43,   # Non-US # and ~: the kernel reports it as KEY_BACKSLASH
    0x33: 39, 0x34: 40, 0x35: 41, 0x36: 51, 0x37: 52, 0x38: 53,
    0x64: 86,   # Non-US \ and | (ISO key left of Z)
    0x87: 89,   # International1 (JIS Ro)
    0x89: 124,  # International3 (JIS Yen)
}
EVDEV_LEFTSHIFT, EVDEV_RIGHTALT, EVDEV_CAPSLOCK = 42, 100, 58
XKB_KEY_UP, XKB_KEY_DOWN = 0, 1


class RuleNames(ctypes.Structure):
    _fields_ = [(name, ctypes.c_char_p)
                for name in ("rules", "model", "layout", "variant", "options")]


# The generated files depend on three packages: xkeyboard-config supplies the
# layouts, libxkbcommon's Caps Lock case mappings follow the Unicode version
# it was built with, and libX11's en_US.UTF-8 Compose table supplies the dead
# key compositions. Regenerating with other versions is refused, so every
# file comes from the same toolchain. To move to newer versions, change the
# pins and regenerate all layouts in one commit.
REQUIRED_VERSIONS = {"xkeyboard-config": "2.48", "libxkbcommon": "1.13.2",
                     "libx11": "1.8.14"}
COMPOSE_LOCALE = "en_US.UTF-8"

PKG_CONFIG_DIRS = ("/opt/homebrew/lib/pkgconfig", "/opt/homebrew/share/pkgconfig",
                   "/usr/local/lib/pkgconfig", "/usr/local/share/pkgconfig",
                   "/usr/lib/pkgconfig", "/usr/lib64/pkgconfig",
                   "/usr/lib/*/pkgconfig", "/usr/share/pkgconfig")


class ToolchainError(RuntimeError):
    pass


def read_pkg_config(name: str) -> dict:
    """Variables and fields of <name>.pc, found like pkg-config would."""
    directories = [d for d in os.environ.get("PKG_CONFIG_PATH", "").split(os.pathsep) if d]
    for pattern in PKG_CONFIG_DIRS:
        directories += sorted(glob.glob(pattern))
    for directory in directories:
        path = Path(directory, f"{name}.pc")
        if not path.is_file():
            continue
        values = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.match(r"\s*([A-Za-z0-9_.]+)\s*([=:])\s*(.*?)\s*$", line)
            if not match:
                continue
            key, _, value = match.groups()
            values[key] = re.sub(r"\$\{([^}]+)\}", lambda m: values.get(m.group(1), ""), value)
        values["path"] = str(path)
        return values
    raise ToolchainError(f"{name}.pc not found; install the {name} development files "
                         "or set PKG_CONFIG_PATH")


def check_version(name: str, found: str) -> None:
    required = REQUIRED_VERSIONS[name]
    if found != required:
        raise ToolchainError(f"{name} {found} found, but the layouts are generated with "
                             f"{name} {required} (see keymaps/layouts/README.md)")


def find_toolchain() -> tuple[Path, Path, Path]:
    """(libxkbcommon library, XKB data root, Compose file) at the pinned versions."""
    xkbcommon = read_pkg_config("xkbcommon")
    check_version("libxkbcommon", xkbcommon.get("Version", "unknown"))
    libdir = Path(xkbcommon.get("libdir", ""))
    library = next((libdir / name for name in
                    ("libxkbcommon.dylib", "libxkbcommon.so", "libxkbcommon.so.0")
                    if (libdir / name).exists()), None)
    if library is None:
        raise ToolchainError(f"libxkbcommon not found in {libdir}")
    xkb_data = read_pkg_config("xkeyboard-config")
    check_version("xkeyboard-config", xkb_data.get("Version", "unknown"))
    root = Path(xkb_data.get("xkb_base", ""))
    if not (root / "symbols").is_dir():
        raise ToolchainError(f"XKB data not found in {root}")
    x11 = read_pkg_config("x11")
    check_version("libx11", x11.get("Version", "unknown"))
    datadir = Path(x11.get("datarootdir") or x11.get("datadir") or
                   Path(x11.get("prefix", "/usr"), "share"))
    compose = datadir / "X11" / "locale" / COMPOSE_LOCALE / "Compose"
    if not compose.is_file():
        raise ToolchainError(f"Compose table not found at {compose}")
    return library, root, compose


def load_xkbcommon(library: Path):
    lib = ctypes.CDLL(str(library))
    lib.xkb_context_new.restype = ctypes.c_void_p
    lib.xkb_context_new.argtypes = [ctypes.c_int]
    lib.xkb_context_include_path_append.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.xkb_context_unref.argtypes = [ctypes.c_void_p]
    lib.xkb_keymap_new_from_names.restype = ctypes.c_void_p
    lib.xkb_keymap_new_from_names.argtypes = [ctypes.c_void_p,
                                              ctypes.POINTER(RuleNames), ctypes.c_int]
    lib.xkb_keymap_unref.argtypes = [ctypes.c_void_p]
    lib.xkb_state_new.restype = ctypes.c_void_p
    lib.xkb_state_new.argtypes = [ctypes.c_void_p]
    lib.xkb_state_unref.argtypes = [ctypes.c_void_p]
    lib.xkb_state_update_key.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int]
    lib.xkb_state_key_get_one_sym.restype = ctypes.c_uint32
    lib.xkb_state_key_get_one_sym.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.xkb_keysym_to_utf32.restype = ctypes.c_uint32
    lib.xkb_keysym_to_utf32.argtypes = [ctypes.c_uint32]
    lib.xkb_keysym_get_name.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
    lib.xkb_keysym_from_name.restype = ctypes.c_uint32
    lib.xkb_keysym_from_name.argtypes = [ctypes.c_char_p, ctypes.c_int]
    return lib


COMPOSE_LINE = re.compile(r'^<dead_([a-z_]+)>\s+<([A-Za-z0-9_]+)>\s*:\s*"((?:[^"\\]|\\.)*)"')


def read_compose(lib, path: Path) -> dict:
    """{dead key name: {base character: result}} for two-key dead key sequences."""
    table: dict = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = COMPOSE_LINE.match(line)
        if not match:
            continue
        dead, base_name, result = match.groups()
        result = re.sub(r"\\(.)", r"\1", result)
        sym = lib.xkb_keysym_from_name(base_name.encode(), 0)
        base = lib.xkb_keysym_to_utf32(sym) if sym else 0
        if base < 0x20 or base > 0xFFFF or len(result) != 1 or ord(result) > 0xFFFF:
            continue
        table.setdefault(dead, {}).setdefault(chr(base), result)
    return table


class Keymap:
    def __init__(self, lib, root: Path, layout: str, variant: str):
        self.lib = lib
        self.context = lib.xkb_context_new(1)  # XKB_CONTEXT_NO_DEFAULT_INCLUDES
        if not self.context:
            sys.exit("xkb_context_new failed")
        lib.xkb_context_include_path_append(self.context, str(root).encode())
        names = RuleNames(b"evdev", b"pc105", layout.encode(), variant.encode(), b"")
        self.keymap = lib.xkb_keymap_new_from_names(self.context, ctypes.byref(names), 0)
        if not self.keymap:
            sys.exit(f"XKB layout {layout!r} variant {variant!r} not found in {root}")

    def close(self):
        self.lib.xkb_keymap_unref(self.keymap)
        self.lib.xkb_context_unref(self.context)

    def sym(self, evdev: int, *, shift=False, altgr=False, caps=False) -> int:
        state = self.lib.xkb_state_new(self.keymap)
        try:
            if caps:
                for direction in (XKB_KEY_DOWN, XKB_KEY_UP):
                    self.lib.xkb_state_update_key(state, EVDEV_CAPSLOCK + 8, direction)
            if shift:
                self.lib.xkb_state_update_key(state, EVDEV_LEFTSHIFT + 8, XKB_KEY_DOWN)
            if altgr:
                self.lib.xkb_state_update_key(state, EVDEV_RIGHTALT + 8, XKB_KEY_DOWN)
            return self.lib.xkb_state_key_get_one_sym(state, evdev + 8)
        finally:
            self.lib.xkb_state_unref(state)

    def value(self, sym: int):
        """JSON value for a keysym: one character, a dead key, or None."""
        if sym == 0:
            return None
        buffer = ctypes.create_string_buffer(64)
        self.lib.xkb_keysym_get_name(sym, buffer, len(buffer))
        name = buffer.value.decode()
        if name.startswith("dead_"):
            return {"dead": name[5:]}
        codepoint = self.lib.xkb_keysym_to_utf32(sym)
        if codepoint < 0x20 or codepoint == 0x7F or 0xD800 <= codepoint <= 0xDFFF:
            return None
        return chr(codepoint)


def key_entry(keymap: Keymap, usage: int):
    evdev = HID_TO_EVDEV[usage]
    levels = [keymap.value(keymap.sym(evdev, shift=i & 1, altgr=i & 2))
              for i in range(4)]
    caps = [keymap.value(keymap.sym(evdev, shift=i & 1, altgr=i & 2, caps=True))
            for i in range(4)]
    for value in levels + caps:
        if isinstance(value, str) and ord(value) > 0xFFFF:
            raise ValueError(f"usage 0x{usage:02x}: {value!r} is outside the BMP")
    if not any(levels + caps):
        return None
    entry = {"levels": trim(levels)}
    if caps != levels:
        entry["caps"] = trim(caps)
    return entry


def trim(values: list) -> list:
    values = list(values)
    while values and values[-1] is None:
        values.pop()
    return values


def dead_keys(keys: dict, compose: dict) -> dict:
    """Compositions of the layout's dead keys with the characters it types.

    The space entry is the spacing character a dead key types on its own."""
    values = [value for entry in keys.values()
              for value in entry["levels"] + entry.get("caps", [])]
    typed = {value for value in values if isinstance(value, str)} | {" "}
    result = {}
    for dead in sorted({value["dead"] for value in values if isinstance(value, dict)}):
        table = compose.get(dead, {})
        result[dead] = {base: table[base] for base in sorted(table) if base in typed}
    return result


def generate(name: str, layout: str, variant: str, description: str,
             root: Path, lib, compose: dict) -> dict:
    keymap = Keymap(lib, root, layout, variant)
    try:
        keys = {}
        for usage in sorted(HID_TO_EVDEV):
            entry = key_entry(keymap, usage)
            if entry is not None:
                keys[f"0x{usage:02x}"] = entry
    finally:
        keymap.close()
    return {
        "name": name,
        "description": description,
        "source": {**REQUIRED_VERSIONS, "layout": layout, "variant": variant},
        "keys": keys,
        "dead": dead_keys(keys, compose),
    }


def xkb_description(root: Path, layout: str, variant: str) -> str:
    """Human name from rules/evdev.lst, e.g. "German" or "French (AZERTY)"."""
    lst = root / "rules" / "evdev.lst"
    if not lst.is_file():
        return layout
    section, wanted = None, variant or layout
    for line in lst.read_text(encoding="utf-8").splitlines():
        if line.startswith("!"):
            section = line[1:].strip()
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        if not variant and section == "layout" and parts[0] == layout:
            return parts[1].strip()
        if variant and section == "variant" and parts[0] == wanted and \
                parts[1].startswith(f"{layout}:"):
            return parts[1].split(":", 1)[1].strip()
    return layout


def dump(data: dict) -> str:
    lines = ["{"]
    for field in ("name", "description", "source"):
        lines.append(f"  {json.dumps(field)}: {json.dumps(data[field], ensure_ascii=False)},")
    lines.append('  "keys": {')
    items = list(data["keys"].items())
    for index, (usage, entry) in enumerate(items):
        comma = "," if index + 1 < len(items) else ""
        lines.append(f"    {json.dumps(usage)}: {json.dumps(entry, ensure_ascii=False)}{comma}")
    lines.append("  },")
    items = list(data["dead"].items())
    if not items:
        lines += ['  "dead": {}', "}", ""]
        return "\n".join(lines)
    lines.append('  "dead": {')
    for index, (dead, table) in enumerate(items):
        comma = "," if index + 1 < len(items) else ""
        lines.append(f"    {json.dumps(dead)}: {json.dumps(table, ensure_ascii=False)}{comma}")
    lines += ["  }", "}", ""]
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("name", nargs="?", help="SolarOS layout name (file stem)")
    parser.add_argument("--xkb", help="XKB layout (default: same as name)")
    parser.add_argument("--variant", default="", help="XKB variant")
    parser.add_argument("--description", help="override the XKB description")
    parser.add_argument("--all", action="store_true",
                        help="regenerate every keymaps/layouts/*.json from its source")
    parser.add_argument("--output-dir", type=Path, default=LAYOUT_DIR)
    args = parser.parse_args(argv)
    if bool(args.all) == bool(args.name):
        parser.error("give one layout name or --all")

    jobs = []
    if args.all:
        for path in sorted(args.output_dir.glob("*.json")):
            old = json.loads(path.read_text(encoding="utf-8"))
            source = old["source"]
            jobs.append((old["name"], source["layout"], source.get("variant", ""),
                         old.get("description")))
    else:
        if not re.fullmatch(r"[a-z][a-z0-9-]{0,15}", args.name):
            parser.error("name must be 1-16 lowercase letters, digits or '-'")
        jobs.append((args.name, args.xkb or args.name, args.variant, args.description))

    try:
        library, xkb_root, compose_path = find_toolchain()
    except ToolchainError as error:
        print(f"generate_keyboard_layouts: {error}", file=sys.stderr)
        return 1
    lib = load_xkbcommon(library)
    compose = read_compose(lib, compose_path)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, layout, variant, description in jobs:
        description = description or xkb_description(xkb_root, layout, variant)
        data = generate(name, layout, variant, description, xkb_root, lib, compose)
        path = args.output_dir / f"{name}.json"
        path.write_text(dump(data), encoding="utf-8")
        print(f"{path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}: "
              f"{len(data['keys'])} keys")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
