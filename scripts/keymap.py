#!/usr/bin/env python3
"""Create, edit, preview and validate SolarOS keymap JSON on a desktop."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile

from solaros_keymap import (FILE_MAX, PROFILE_IDS, KeymapError, load_profile,
                           merge, output, read_json, sparse)

# Canonical HID usages. Physical key names come from the shared profiles.
USAGES = {chr(65 + i): 4 + i for i in range(26)}
USAGES.update({str(i): 29 + i for i in range(1, 10)})
USAGES["0"] = 39
USAGES.update({f"F{i}": 57 + i for i in range(1, 13)})
USAGES.update({f"F{i}": 91 + i for i in range(13, 25)})
USAGES.update(dict(Enter=40, Escape=41, Backspace=42, Tab=43, Space=44,
    Minus=45, Equal=46, LeftBracket=47, RightBracket=48, Backslash=49,
    Semicolon=51, Apostrophe=52, Grave=53, Comma=54, Period=55, Slash=56,
    CapsLock=57, PrintScreen=70, ScrollLock=71, Pause=72, Insert=73,
    Home=74, PageUp=75, Delete=76, End=77, PageDown=78, Right=79,
    Left=80, Down=81, Up=82, NumLock=83, Ctrl=224, Shift=225, Alt=226,
    Super=227, RightCtrl=228, RightShift=229, RightAlt=230, RightSuper=231))
USAGE_NAMES = {name.casefold(): value for name, value in USAGES.items()}


def action(text: str) -> dict:
    """Friendly assignments compile to the existing schema's output fields."""
    if text.casefold() == "raw":
        return {"raw": True}
    if text.casefold() == "none":
        return {}
    if text.casefold().startswith("tap:"):
        result = action(text[4:])
        result["layer_tap"] = True
        return output(result)
    if text.casefold().startswith("char:"):
        value = text[5:]
        if len(value) != 1 or not 1 <= ord(value) <= 255:
            raise KeymapError("char: requires one character with a byte value from 1 to 255")
        return {"key": ord(value)}
    for prefix, field in (("usage:", "usage"), ("key:", "key")):
        if text.casefold().startswith(prefix):
            try:
                return output({field: int(text[len(prefix):], 0)})
            except ValueError as exc:
                raise KeymapError(f"invalid {prefix} value: {text}") from exc
    usage = USAGE_NAMES.get(text.casefold())
    if usage is None:
        raise KeymapError(f"unknown action {text!r}; use an HID name, char:X, tap:Space, raw or none")
    return {"usage": usage}


def describe(entry: dict) -> str:
    if not entry:
        return "none"
    if entry.get("raw"):
        return "raw"
    parts = []
    if entry.get("usage"):
        parts.append(next((name for name, usage in USAGES.items()
                           if usage == entry["usage"]), f"usage:{entry['usage']}"))
    if entry.get("key"):
        value = entry["key"]
        parts.append(f"char:{chr(value)}" if 32 <= value < 127 else f"key:{value}")
    if entry.get("shift_key"):
        parts.append(f"shift_key:{entry['shift_key']}")
    if entry.get("layer_tap"):
        parts.append("layer_tap")
    if entry.get("alt_block"):
        parts.append("alt_block")
    return " + ".join(parts)


def preview(profile, layers) -> None:
    print(f"{profile.label} ({profile.id}), {profile.rows}x{profile.cols}")
    print(f"{'PHYSICAL':>8} {'KEY':<16} {'BASE':<35} SYMBOL")
    for physical, name in sorted(profile.names.items()):
        print(f"{physical:8} {name:<16} {describe(layers[0].get(physical, {})):<35} "
              f"{describe(layers[1].get(physical, {}))}")


def assign(profile, layers, assignments, layer=0) -> set[int]:
    seen = set()
    for assignment in assignments:
        if "=" not in assignment:
            raise KeymapError(f"assignment must be KEY=ACTION: {assignment}")
        name, text = assignment.split("=", 1)
        physical = profile.select(name)
        if physical in seen:
            raise KeymapError(f"duplicate assignment for physical {physical}")
        seen.add(physical)
        layers[layer][physical] = action(text)
    return seen


def interactive(profile, layers) -> set[int]:
    print("Actions: F1, Escape, Ctrl, char:!, tap:Space, raw, none.")
    print("Press Enter to keep the current assignment.")
    selected = set()
    fn_keys = sorted(((physical, name) for physical, name in profile.names.items()
                      if name.startswith("FN")), key=lambda pair: pair[1])
    if fn_keys:
        for physical, name in fn_keys:
            while True:
                text = input(f"{name} (physical {physical}) [{describe(layers[0].get(physical, {}))}]: ")
                if not text:
                    break
                try:
                    layers[0][physical] = action(text)
                    selected.add(physical)
                    break
                except KeymapError as exc:
                    print(exc)
    else:
        print("Enter KEY=ACTION assignments; an empty line finishes. Use show to list keys.")
        while text := input("Assignment: "):
            try:
                selected.update(assign(profile, layers, [text]))
            except KeymapError as exc:
                print(exc)
    return selected


def write_mapping(path: Path, document: dict, *, overwrite: bool) -> None:
    text = json.dumps(document, indent=2, ensure_ascii=True) + "\n"
    if len(text.encode()) > FILE_MAX:
        raise KeymapError("generated mapping exceeds 16 KiB")
    if not overwrite:
        # Exclusive creation also works on FAT/exFAT output media without hard links.
        with path.open("x", encoding="utf-8") as stream:
            try:
                stream.write(text)
                stream.flush()
            except BaseException:
                stream.close()
                path.unlink(missing_ok=True)
                raise
        return
    # Stage beside the destination. A failed write never truncates an existing map.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
            stream.flush()
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("profiles", help="list supported keyboard profiles")
    for command in ("create", "edit", "show", "validate"):
        sub = commands.add_parser(command)
        sub.add_argument("--profile", required=True, choices=PROFILE_IDS)
        sub.add_argument("--rows", type=int, help="generic TCA8418 matrix rows")
        sub.add_argument("--cols", type=int, help="generic TCA8418 matrix columns")
        sub.add_argument("--input", type=Path, required=command in ("edit", "validate"))
        if command in ("create", "edit"):
            sub.add_argument("--output", type=Path, required=command == "create")
            sub.add_argument("--set", action="append", default=[], metavar="KEY=ACTION")
            sub.add_argument("--symbol", action="append", default=[], metavar="KEY=ACTION")
            sub.add_argument("--interactive", action="store_true")
            sub.add_argument("--force", action="store_true", help="allow replacing an output file")
    args = parser.parse_args(argv)
    try:
        if args.command == "profiles":
            for name in PROFILE_IDS:
                profile = load_profile(name)
                print(f"{name:<24} {profile.rows}x{profile.cols} {profile.label}")
            return 0
        profile = load_profile(args.profile, rows=args.rows, cols=args.cols)
        original = read_json(args.input) if args.input else {"schema": 1, "keys": []}
        layers = merge(profile, original)
        if args.command == "show":
            preview(profile, layers)
        elif args.command == "validate":
            print(f"Valid {profile.id} mapping: {args.input}")
        else:
            # Keep explicit assignments, even if they match the standard profile.
            # Such entries can override a different default baked into firmware.
            include = [{profile.physical(entry) for entry in original.get(name, [])}
                       for name in ("keys", "symbols")]
            include[0].update(assign(profile, layers, args.set))
            include[1].update(assign(profile, layers, args.symbol, 1))
            if args.interactive or (not args.set and not args.symbol and sys.stdin.isatty()):
                include[0].update(interactive(profile, layers))
            document = sparse(profile, layers, include=include)
            merge(profile, document)  # Validate the combined map, including layer selectors.
            destination = args.output or args.input
            same_file = args.input is not None and args.input.resolve() == destination.resolve()
            write_mapping(destination, document, overwrite=args.force or same_file)
            print(f"Saved {destination}; unlisted keys retain the firmware default.")
        return 0
    except (OSError, ValueError, UnicodeError, EOFError, KeyboardInterrupt) as exc:
        print(f"keymap: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
