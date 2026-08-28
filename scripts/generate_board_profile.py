#!/usr/bin/env python3
"""Compile a SolarOS board TOML manifest into CMake and C metadata."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import tomllib

from solaros_board_manifest import (
    ManifestError,
    generate_cmake,
    generate_header,
    load_board_manifest,
    load_driver_catalog,
    validate_board,
    write_if_changed,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--manifest-dir", type=Path)
    parser.add_argument("--drivers", type=Path, required=True)
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    try:
        drivers = load_driver_catalog(args.drivers)
        board = load_board_manifest(args.manifest, args.manifest_dir)
        validate_board(board, drivers)
        if not args.validate_only:
            if args.cmake is None or args.header is None:
                parser.error("--cmake and --header are required unless --validate-only is used")
            write_if_changed(args.cmake, generate_cmake(board, drivers))
            write_if_changed(args.header, generate_header(board, drivers))
    except (OSError, ManifestError, tomllib.TOMLDecodeError) as exc:
        print(f"board manifest: {exc}", file=sys.stderr)
        return 1
    print(f"Board manifest valid: {board['board']['id']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
