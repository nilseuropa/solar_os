"""Embed the pinned C64 ROM set, verifying provenance hashes before generation."""
import hashlib
import json
from pathlib import Path
import sys

assets, output = map(Path, sys.argv[1:])
manifest = json.loads((assets / "sources.json").read_text())
lines = ['#include "roms.h"']
for name, symbol, size in (
    ("basic.rom", "solar_os_c64_basic_rom", 8192),
    ("kernal.rom", "solar_os_c64_kernal_rom", 8192),
    ("chargen.rom", "solar_os_c64_chargen_rom", 4096),
):
    data = (assets / name).read_bytes()
    record = next(entry for entry in manifest if entry["file"] == name)
    if len(data) != size or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise SystemExit(f"{name}: size or SHA-256 differs from the pinned ROM set")
    lines.append(f"const uint8_t {symbol}[{size}] = {{")
    for offset in range(0, size, 16):
        lines.append(",".join(str(byte) for byte in data[offset:offset + 16]) + ",")
    lines.append("};")
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text("\n".join(lines) + "\n")
