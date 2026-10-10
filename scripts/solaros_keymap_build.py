"""Resolve optional compiled keymaps for PlatformIO and CMake."""
from pathlib import Path

from solaros_keymap import PROFILE_IDS, KeymapError, fields, read_json

OPTIONS = ("SOLAR_OS_KEYMAP_PROFILE", "SOLAR_OS_KEYMAP_FILE", "SOLAR_OS_KEYMAPS_FILE")


def resolve_selection(root: Path, profile="", file="", index="") -> dict[str, Path]:
    def resolve(value, base):
        path = Path(value)
        path = (base / path if not path.is_absolute() else path).resolve()
        if not path.is_file():
            raise KeymapError(f"keymap file not found: {path}")
        return path
    if index and (profile or file):
        raise KeymapError("KEYMAPS_FILE cannot be combined with KEYMAP_PROFILE/KEYMAP_FILE")
    if bool(profile) != bool(file):
        raise KeymapError("set both SOLAR_OS_KEYMAP_PROFILE and SOLAR_OS_KEYMAP_FILE")
    if index:
        path = resolve(index, root)
        document = read_json(path)
        fields(document, {"schema", "profiles"}, "keymap index")
        if document.get("schema") != 1 or isinstance(document.get("schema"), bool):
            raise KeymapError("keymap index schema must be 1")
        entries = document.get("profiles")
        fields(entries, set(PROFILE_IDS), "keymap index profiles")
        if not entries:
            raise KeymapError("keymap index must select at least one profile")
        if any(not isinstance(value, str) or not value for value in entries.values()):
            raise KeymapError("keymap index paths must be nonempty strings")
        return {name: resolve(value, path.parent) for name, value in entries.items()}
    if profile:
        if profile not in PROFILE_IDS:
            raise KeymapError(f"unknown keyboard profile: {profile}")
        return {profile: resolve(file, root)}
    return {}


def dependencies(root: Path, profile="", file="", index="") -> tuple[Path, ...]:
    selected = resolve_selection(root, profile, file, index)
    inputs = list(selected.values())
    if index:
        path = Path(index)
        inputs.append((root / path if not path.is_absolute() else path).resolve())
    return tuple(inputs)
