#!/usr/bin/env python3
"""Generate pixel-doubled ProFont U8g2 fonts for high-resolution boards.

Takes the original ProFont BDF sources (vendored under fonts/profont_bdf/,
from U8g2's tools/font/bdf collection) and doubles every glyph pixel into a
2x2 block. The result keeps ProFont's exact drawing at twice the size --
suitable for 800x480 panels where the UI runs at 2x scale -- rather than a
TTF re-rasterization that would look subtly different.

Output C arrays are written to fonts/build/u8g2/ as
u8g2_font_solar_os_profont_<2*size>_mf.c and are committed to the
repository, like the default generated fonts.

Usage:

    python3 fonts/generate_profont_2x.py [--bdfconv /path/to/bdfconv]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

FONT_DIR = Path(__file__).resolve().parent
BDF_DIR = FONT_DIR / "profont_bdf"
OUT_DIR = FONT_DIR / "build" / "u8g2"
SIZES = [12, 15, 17, 22, 29]
# Full printable ASCII, matching the vendored profont *_mf variants.
MAP_EXPR = "32-126"


def double_bitmap_row(hex_row: str, width: int) -> str:
    row_bytes = len(hex_row) // 2
    bits = int(hex_row, 16)
    out_width = width * 2
    out_bytes = (out_width + 7) // 8
    out = 0
    for x in range(width):
        bit = (bits >> (row_bytes * 8 - 1 - x)) & 1
        if bit:
            out |= 0b11 << (out_width - 2 - (2 * x))
    out <<= out_bytes * 8 - out_width
    return f"{out:0{out_bytes * 2}X}"


def double_bdf(src: Path, dst: Path) -> None:
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    out: list[str] = []
    bbx_width = 0
    in_bitmap = False
    in_properties = False

    for line in lines:
        parts = line.split()
        keyword = parts[0] if parts else ""

        if in_properties:
            # Property values may be quoted free text (profont17 even has
            # a property named FONTBOUNDINGBOX); only the metric
            # properties are doubled, everything else passes through.
            if keyword == "ENDPROPERTIES":
                in_properties = False
                out.append(line)
            elif keyword in ("FONT_ASCENT", "FONT_DESCENT", "PIXEL_SIZE"):
                out.append(f"{keyword} {int(parts[1]) * 2}")
            else:
                out.append(line)
            continue

        if keyword == "STARTPROPERTIES":
            in_properties = True
            out.append(line)
            continue

        if in_bitmap:
            if keyword == "ENDCHAR":
                in_bitmap = False
                out.append(line)
            else:
                doubled = double_bitmap_row(line.strip(), bbx_width)
                out.append(doubled)
                out.append(doubled)
            continue

        if keyword == "FONT" and len(parts) == 2:
            out.append(f"FONT {parts[1]}2x")
        elif keyword == "SIZE":
            out.append(f"SIZE {int(parts[1]) * 2} {parts[2]} {parts[3]}")
        elif keyword in ("FONTBOUNDINGBOX", "BBX"):
            values = [int(v) * 2 for v in parts[1:5]]
            out.append(f"{keyword} {values[0]} {values[1]} {values[2]} {values[3]}")
        elif keyword == "DWIDTH":
            out.append(f"DWIDTH {int(parts[1]) * 2} {int(parts[2]) * 2}")
        elif keyword in ("FONT_ASCENT", "FONT_DESCENT", "PIXEL_SIZE"):
            out.append(f"{keyword} {int(parts[1]) * 2}")
        elif keyword == "BITMAP":
            in_bitmap = True
            out.append(line)
        else:
            out.append(line)

        if keyword == "BBX":
            bbx_width = int(parts[1])

    dst.write_text("\n".join(out) + "\n", encoding="utf-8")


def find_bdfconv(explicit: str | None) -> str:
    if explicit:
        return explicit
    for name in ("bdfconv", "bdfconv.exe"):
        vendored = FONT_DIR / "tools" / "bdfconv" / name
        if vendored.exists():
            return str(vendored)
    found = shutil.which("bdfconv")
    if not found:
        raise SystemExit(
            "bdfconv not found; build it first: make -C fonts/tools/bdfconv")
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bdfconv", default=None)
    args = parser.parse_args()

    bdfconv = find_bdfconv(args.bdfconv)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for size in SIZES:
        src = BDF_DIR / f"profont{size}.bdf"
        doubled = OUT_DIR.parent / f"profont{size}_2x.bdf"
        double_bdf(src, doubled)

        symbol = f"u8g2_font_solar_os_profont_{size * 2}_mf"
        c_path = OUT_DIR / f"{symbol}.c"
        subprocess.run(
            [bdfconv, "-b", "0", "-f", "1", "-m", MAP_EXPR,
             "-n", symbol, "-o", str(c_path), str(doubled)],
            check=True,
        )
        contents = c_path.read_text(encoding="utf-8")
        if '#include "u8g2.h"' not in contents[:200]:
            c_path.write_text('#include "u8g2.h"\n\n' + contents,
                              encoding="utf-8")
        doubled.unlink()
        print(f"profont{size} -> {c_path.name}")


if __name__ == "__main__":
    main()
