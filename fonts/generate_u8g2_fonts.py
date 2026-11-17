#!/usr/bin/env python3
"""Generate monochrome SolarOS U8g2 fonts from the default TTF faces.

The pipeline is deliberately offline:

    TTF -> monochrome fixed-cell BDF -> optional bdfconv -> U8g2 C array

Generated files are written to fonts/build/ by default and are not part of the
normal firmware build until explicitly copied into the U8g2 component.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


FONT_DIR = Path(__file__).resolve().parent
DEFAULT_SIZES = [10, 12, 14, 16, 18, 20]
DEFAULT_RANGES = [(0x20, 0x7E), (0xA0, 0xFF)]


@dataclass(frozen=True)
class FontStyle:
    key: str
    label: str
    filename: str
    weight: str
    slant: str


STYLES = [
    FontStyle("r", "regular", "Regular.ttf", "Medium", "R"),
    FontStyle("b", "bold", "Bold.ttf", "Bold", "R"),
    FontStyle("i", "italic", "Italic.ttf", "Medium", "I"),
    FontStyle("bi", "bold_italic", "BoldItalic.ttf", "Bold", "I"),
]


@dataclass
class GeneratedFont:
    style: FontStyle
    size: int
    cell_width: int
    cell_height: int
    ascent: int
    descent: int
    bdf_path: Path
    c_path: Path | None
    symbol: str


@dataclass(frozen=True)
class RasterizedGlyph:
    rows: list[int]
    width: int
    x_offset: int


def parse_sizes(value: str) -> list[int]:
    sizes: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        size = int(item, 10)
        if size < 4 or size > 96:
            raise argparse.ArgumentTypeError(f"unreasonable font size: {size}")
        sizes.append(size)
    if not sizes:
        raise argparse.ArgumentTypeError("at least one size is required")
    return sizes


def parse_int(value: str) -> int:
    return int(value, 0)


def parse_ranges(value: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            start_s, end_s = item.split("-", 1)
            start = parse_int(start_s)
            end = parse_int(end_s)
        else:
            start = end = parse_int(item)
        if start < 0 or end > 0x10FFFF or end < start:
            raise argparse.ArgumentTypeError(f"invalid glyph range: {item}")
        ranges.append((start, end))
    if not ranges:
        raise argparse.ArgumentTypeError("at least one glyph range is required")
    return ranges


def iter_codepoints(ranges: Iterable[tuple[int, int]]) -> list[int]:
    codepoints: list[int] = []
    seen = set()
    for start, end in ranges:
        for codepoint in range(start, end + 1):
            if codepoint not in seen:
                codepoints.append(codepoint)
                seen.add(codepoint)
    return codepoints


def bdfconv_map_expr(ranges: Iterable[tuple[int, int]]) -> str:
    parts = []
    for start, end in ranges:
        if start == end:
            parts.append(str(start))
        else:
            parts.append(f"{start}-{end}")
    return ",".join(parts)


def glyph_safe_char(codepoint: int) -> str:
    if 0x20 <= codepoint <= 0x7E:
        ch = chr(codepoint)
        if ch.isalnum():
            return ch
    return f"uni{codepoint:04X}"


def ceil_length(value: float) -> int:
    return int(math.ceil(value - 1e-6))


def font_cell_width(font: ImageFont.FreeTypeFont, codepoints: Iterable[int], strict_cell: bool) -> int:
    advance = max(ceil_length(font.getlength(chr(codepoint))) for codepoint in codepoints)
    if strict_cell:
        return advance

    max_right = advance
    min_left = 0
    for codepoint in codepoints:
        left, _top, right, _bottom = font.getbbox(chr(codepoint), anchor="ls")
        min_left = min(min_left, left)
        max_right = max(max_right, right)
    return max_right - min_left


def rasterize_glyph(
    font: ImageFont.FreeTypeFont,
    codepoint: int,
    cell_width: int,
    height: int,
    ascent: int,
    threshold: int,
    antialias: bool,
) -> RasterizedGlyph:
    left, _top, right, _bottom = font.getbbox(chr(codepoint), anchor="ls")
    x_offset = min(0, left)
    width = max(cell_width, right) - x_offset
    image = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(image)
    if not antialias:
        draw.fontmode = "1"
    draw.text((-x_offset, ascent), chr(codepoint), fill=255, font=font, anchor="ls")

    rows: list[int] = []
    for y in range(height):
        bits = 0
        for x in range(width):
            bits <<= 1
            if image.getpixel((x, y)) >= threshold:
                bits |= 1
        rows.append(bits)
    return RasterizedGlyph(rows=rows, width=width, x_offset=x_offset)


def rows_to_hex(rows: list[int], width: int) -> list[str]:
    byte_count = (width + 7) // 8
    pad_bits = byte_count * 8 - width
    lines: list[str] = []
    for row in rows:
        value = row << pad_bits
        lines.append(f"{value:0{byte_count * 2}X}")
    return lines


def write_bdf(
    path: Path,
    style: FontStyle,
    size: int,
    font: ImageFont.FreeTypeFont,
    codepoints: list[int],
    cell_width: int,
    cell_height: int,
    ascent: int,
    descent: int,
    threshold: int,
    antialias: bool,
) -> None:
    point_size = size * 10
    avg_width = cell_width * 10
    font_name = (
        f"-SolarOS-Default-{style.weight}-{style.slant}-Normal--"
        f"{size}-{point_size}-75-75-M-{avg_width}-ISO10646-1"
    )

    with path.open("w", encoding="ascii", newline="\n") as out:
        out.write("STARTFONT 2.1\n")
        out.write(f"FONT {font_name}\n")
        out.write(f"SIZE {size} 75 75\n")
        out.write(f"FONTBOUNDINGBOX {cell_width} {cell_height} 0 -{descent}\n")
        out.write("STARTPROPERTIES 20\n")
        out.write('FOUNDRY "SolarOS"\n')
        out.write('FAMILY_NAME "SolarOS Default"\n')
        out.write(f'WEIGHT_NAME "{style.weight}"\n')
        out.write(f'SLANT "{style.slant}"\n')
        out.write('SETWIDTH_NAME "Normal"\n')
        out.write('ADD_STYLE_NAME ""\n')
        out.write(f"PIXEL_SIZE {size}\n")
        out.write(f"POINT_SIZE {point_size}\n")
        out.write("RESOLUTION_X 75\n")
        out.write("RESOLUTION_Y 75\n")
        out.write("SPACING \"M\"\n")
        out.write(f"AVERAGE_WIDTH {avg_width}\n")
        out.write("CHARSET_REGISTRY \"ISO10646\"\n")
        out.write("CHARSET_ENCODING \"1\"\n")
        out.write(f"FONT_ASCENT {ascent}\n")
        out.write(f"FONT_DESCENT {descent}\n")
        out.write("DEFAULT_CHAR 32\n")
        out.write("COPYRIGHT \"Generated from SolarOS default TTF faces.\"\n")
        out.write("_GBDFED_INFO \"Generated by fonts/generate_u8g2_fonts.py\"\n")
        out.write("_GBDFED_SKIP 0\n")
        out.write("ENDPROPERTIES\n")
        out.write(f"CHARS {len(codepoints)}\n")

        for codepoint in codepoints:
            glyph = rasterize_glyph(
                font,
                codepoint,
                cell_width,
                cell_height,
                ascent,
                threshold,
                antialias,
            )
            out.write(f"STARTCHAR {glyph_safe_char(codepoint)}\n")
            out.write(f"ENCODING {codepoint}\n")
            out.write(f"SWIDTH {cell_width * 1000 // max(size, 1)} 0\n")
            out.write(f"DWIDTH {cell_width} 0\n")
            out.write(f"BBX {glyph.width} {cell_height} {glyph.x_offset} -{descent}\n")
            out.write("BITMAP\n")
            for line in rows_to_hex(glyph.rows, glyph.width):
                out.write(f"{line}\n")
            out.write("ENDCHAR\n")

        out.write("ENDFONT\n")


def find_bdfconv(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    vendored = FONT_DIR / "tools" / "bdfconv" / "bdfconv"
    if vendored.exists():
        return str(vendored)
    return shutil.which("bdfconv")


def run_bdfconv(
    bdfconv: str,
    bdf_path: Path,
    c_path: Path,
    symbol: str,
    map_expr: str,
) -> None:
    cmd = [
        bdfconv,
        "-b",
        "0",
        "-f",
        "1",
        "-m",
        map_expr,
        "-n",
        symbol,
        "-o",
        str(c_path),
        str(bdf_path),
    ]
    subprocess.run(cmd, check=True)
    contents = c_path.read_text(encoding="utf-8")
    if '#include "u8g2.h"' not in contents[:200]:
        contents = '#include "u8g2.h"\n\n' + contents
    contents = "\n".join(line.rstrip() for line in contents.splitlines()) + "\n"
    c_path.write_text(contents, encoding="utf-8")


def draw_preview(
    path: Path,
    generated: list[GeneratedFont],
    codepoints: list[int],
    threshold: int,
    antialias: bool,
    scale: int,
) -> None:
    samples = [
        "SolarOS default bitmap preview",
        "The quick brown fox jumps over the lazy dog.",
        "0123456789 []{} <> ~/ .ssh shell> setterm",
        "äöü ÄÖÜ ß éèê ñ ø å",
    ]
    rows = []
    max_width = 1
    total_height = 0
    for item in generated:
        font = ImageFont.truetype(str(FONT_DIR / item.style.filename), item.size)
        label = f"{item.size}px {item.style.label} ({item.cell_width}x{item.cell_height})"
        block_lines = [label] + samples
        block_height = (len(block_lines) + 1) * item.cell_height
        block_width = max(len(line) for line in block_lines) * item.cell_width
        rows.append((item, font, block_lines, block_width, block_height))
        max_width = max(max_width, block_width)
        total_height += block_height

    image = Image.new("L", (max_width + 12, total_height + 12), 255)
    y = 6
    for item, font, block_lines, _block_width, block_height in rows:
        for line in block_lines:
            x = 6
            for ch in line:
                codepoint = ord(ch)
                if codepoint not in codepoints:
                    codepoint = ord("?")
                glyph_rows = rasterize_glyph(
                    font,
                    codepoint,
                    item.cell_width,
                    item.cell_height,
                    item.ascent,
                    threshold,
                    antialias,
                )
                for gy, row in enumerate(glyph_rows.rows):
                    for gx in range(glyph_rows.width):
                        bit = (row >> (glyph_rows.width - gx - 1)) & 1
                        if bit:
                            pixel_x = x + glyph_rows.x_offset + gx
                            if 0 <= pixel_x < image.width:
                                image.putpixel((pixel_x, y + gy), 0)
                x += item.cell_width
            y += item.cell_height
        y += item.cell_height

    if scale > 1:
        image = image.resize((image.width * scale, image.height * scale), Image.Resampling.NEAREST)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


PACK_MAGIC = b"SFP1"
PACK_NAME_MAX = 16


def parse_u8g2_c_array(path: Path) -> bytes:
    """Return the bytes of the U8g2 font array bdfconv wrote to a C file."""
    text = path.read_text(encoding="utf-8")
    declared = re.search(r"\[(\d+)\]", text)
    body = text[text.index("=") + 1:]
    data = bytearray()
    escapes = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
               "\\": 92, "\"": 34, "'": 39, "?": 63}
    for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        i = 0
        while i < len(literal):
            ch = literal[i]
            if ch != "\\":
                data.append(ord(ch))
                i += 1
                continue
            i += 1
            esc = literal[i]
            if esc in "01234567":
                j = i
                while j < len(literal) and j < i + 3 and literal[j] in "01234567":
                    j += 1
                data.append(int(literal[i:j], 8) & 0xFF)
                i = j
            elif esc == "x":
                j = i + 1
                while j < len(literal) and literal[j] in "0123456789abcdefABCDEF":
                    j += 1
                data.append(int(literal[i + 1:j], 16) & 0xFF)
                i = j
            else:
                data.append(escapes[esc])
                i += 1
    data.append(0)  # the C string's own terminator is part of the array
    if declared and int(declared.group(1)) != len(data):
        raise SystemExit(f"{path}: declared {declared.group(1)} bytes, parsed {len(data)}")
    return bytes(data)


def write_pack(path: Path, name: str, ranges: list[tuple[int, int]], faces: list[GeneratedFont]) -> None:
    """Write a font pack: the glyphs of `ranges` for every style and size.

    Layout, little endian:
      "SFP1", u16 range count, u16 face count, name[16],
      ranges (u32 first, u32 last), faces (u8 style, u8 size, u16 0,
      u32 offset, u32 length), then the U8g2 font data of each face.
    """
    if len(name) >= PACK_NAME_MAX:
        raise SystemExit(f"pack name too long: {name}")
    styles = [style.key for style in STYLES]
    blobs = [parse_u8g2_c_array(item.c_path) for item in faces]
    header_size = 4 + 2 + 2 + PACK_NAME_MAX + 8 * len(ranges) + 12 * len(faces)
    out = bytearray(PACK_MAGIC)
    out += struct.pack("<HH", len(ranges), len(faces))
    out += name.encode("ascii").ljust(PACK_NAME_MAX, b"\0")
    for first, last in ranges:
        out += struct.pack("<II", first, last)
    offset = header_size
    for item, blob in zip(faces, blobs):
        out += struct.pack("<BBHII", styles.index(item.style.key), item.size, 0, offset, len(blob))
        offset += len(blob)
    for blob in blobs:
        out += blob
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))


def generate(args: argparse.Namespace) -> list[GeneratedFont]:
    out_dir = Path(args.out).resolve()
    if args.pack:
        # A pack's intermediate files stay apart from the default fonts.
        args.stem = "pack"
        args.no_preview = True
        out_dir = out_dir / f"pack-{args.pack}"
    bdf_dir = out_dir / "bdf"
    u8g2_dir = out_dir / "u8g2"
    bdf_dir.mkdir(parents=True, exist_ok=True)
    if not args.no_u8g2:
        u8g2_dir.mkdir(parents=True, exist_ok=True)

    codepoints = iter_codepoints(args.glyph_ranges)
    map_expr = bdfconv_map_expr(args.glyph_ranges)
    bdfconv = None if args.no_u8g2 else find_bdfconv(args.bdfconv)
    generated: list[GeneratedFont] = []

    for size in args.sizes:
        fonts = {
            style.key: ImageFont.truetype(str(FONT_DIR / style.filename), size)
            for style in STYLES
        }
        widths = {
            style.key: font_cell_width(fonts[style.key], codepoints, args.strict_cell)
            for style in STYLES
        }
        # Keep all styles at a size on one terminal grid.
        cell_width = max(widths.values())

        for style in STYLES:
            font = fonts[style.key]
            ascent, descent = font.getmetrics()
            cell_height = ascent + descent
            stem = f"{args.stem}_{style.key}_{size}"
            symbol = f"u8g2_font_solar_os_{stem}_tf"
            bdf_path = bdf_dir / f"{stem}.bdf"
            c_path = u8g2_dir / f"{symbol}.c"
            write_bdf(
                bdf_path,
                style,
                size,
                font,
                codepoints,
                cell_width,
                cell_height,
                ascent,
                descent,
                args.threshold,
                args.antialias,
            )
            actual_c_path: Path | None = None
            if not args.no_u8g2:
                if bdfconv:
                    run_bdfconv(bdfconv, bdf_path, c_path, symbol, map_expr)
                    actual_c_path = c_path
                else:
                    print("warning: bdfconv not found; generated BDF only")
                    print("         build it with: make -C fonts/tools/bdfconv")
                    print("         or pass: --bdfconv /path/to/bdfconv")
                    args.no_u8g2 = True
            generated.append(
                GeneratedFont(
                    style=style,
                    size=size,
                    cell_width=cell_width,
                    cell_height=cell_height,
                    ascent=ascent,
                    descent=descent,
                    bdf_path=bdf_path,
                    c_path=actual_c_path,
                    symbol=symbol,
                )
            )

    manifest = {
        "source": "SolarOS default TTF faces",
        "sizes": args.sizes,
        "glyph_ranges": [f"0x{start:x}-0x{end:x}" for start, end in args.glyph_ranges],
        "antialias": args.antialias,
        "threshold": args.threshold,
        "fonts": [
            {
                "style": item.style.label,
                "size": item.size,
                "cell_width": item.cell_width,
                "cell_height": item.cell_height,
                "ascent": item.ascent,
                "descent": item.descent,
                "bdf": str(item.bdf_path.relative_to(out_dir)),
                "c": str(item.c_path.relative_to(out_dir)) if item.c_path else None,
                "symbol": item.symbol,
            }
            for item in generated
        ],
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    if not args.no_preview:
        draw_preview(
            out_dir / "preview" / "default_preview.png",
            generated,
            codepoints,
            args.threshold,
            args.antialias,
            args.preview_scale,
        )

    if args.pack:
        if args.no_u8g2 or any(item.c_path is None for item in generated):
            raise SystemExit("--pack needs bdfconv: make -C fonts/tools/bdfconv")
        pack_path = Path(args.pack_dir).resolve() / f"{args.pack}.sfp"
        write_pack(pack_path, args.pack, args.glyph_ranges, generated)
        print(f"wrote {pack_path} ({pack_path.stat().st_size} bytes)")

    return generated


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        default=str(FONT_DIR / "build"),
        help="output directory, default: fonts/build",
    )
    parser.add_argument(
        "--sizes",
        type=parse_sizes,
        default=DEFAULT_SIZES,
        help="comma-separated pixel sizes, default: 10,12,14,16,18,20",
    )
    parser.add_argument(
        "--glyph-ranges",
        type=parse_ranges,
        default=DEFAULT_RANGES,
        help="comma-separated codepoint ranges, default: 0x20-0x7e,0xa0-0xff",
    )
    parser.add_argument(
        "--stem",
        default="default",
        help="font set name used in file and symbol names, default: default "
        "(u8g2_font_solar_os_<stem>_<style>_<size>_tf); a fallback pack uses "
        "its own name and only the glyph ranges the built-in fonts lack",
    )
    parser.add_argument(
        "--pack",
        metavar="NAME",
        help="write a font pack NAME.sfp holding only --glyph-ranges for every "
        "style and size (loaded on the device with `setterm font load NAME`)",
    )
    parser.add_argument(
        "--pack-dir",
        default=str(FONT_DIR / "packs"),
        help="where --pack writes the pack file, default: fonts/packs",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        default=128,
        help="threshold for antialiased rasterization, default: 128",
    )
    parser.add_argument(
        "--antialias",
        action="store_true",
        help="render antialiased grayscale first, then threshold to 1-bit",
    )
    parser.add_argument(
        "--loose-cell",
        dest="strict_cell",
        action="store_false",
        help="allow italic overhangs to widen the whole size grid",
    )
    parser.set_defaults(strict_cell=True)
    parser.add_argument(
        "--bdfconv",
        help="path to bdfconv; if omitted, fonts/tools/bdfconv/bdfconv and PATH are searched",
    )
    parser.add_argument(
        "--no-u8g2",
        action="store_true",
        help="skip bdfconv and generate BDF/preview only",
    )
    parser.add_argument(
        "--no-preview",
        action="store_true",
        help="skip preview PNG generation",
    )
    parser.add_argument(
        "--preview-scale",
        type=int,
        default=3,
        help="nearest-neighbor preview enlargement, default: 3",
    )
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    generated = generate(args)
    print(f"generated {len(generated)} BDF font(s) in {Path(args.out).resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
