#!/usr/bin/env python3
"""Compile catalog panel choices for the runtime expansion form."""

import argparse
import json
from pathlib import Path

from solaros_board_manifest import load_driver_catalog, write_if_changed


def generate_header(catalog: Path, packages: set[str]) -> str:
    lines = ["/* Generated from boards/expansion_drivers.toml. Do not edit. */",
             "#pragma once", "", "#define SOLAR_OS_EXPANSION_PANEL_CHOICES(X) \\"]
    for driver in load_driver_catalog(catalog).values():
        if driver.package not in packages:
            continue
        for profile in driver.display_profiles:
            if set(profile.bindings) != {"panel"}:
                continue
            size = f"{profile.width}x{profile.height}"
            label = f"{size} {profile.label.replace(size, '').strip()}".strip()
            lines.append(f"    X({json.dumps(driver.name)}, {profile.bindings['panel']}, "
                         f"{json.dumps(label, ensure_ascii=False)}, {int(profile.default)}) \\")
    lines.append("    /* end */")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--packages", default="")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    write_if_changed(args.output, generate_header(args.catalog, set(args.packages.split())))


if __name__ == "__main__":
    main()
