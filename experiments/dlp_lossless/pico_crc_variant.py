#!/usr/bin/env python3
"""Prepare an isolated nibble-table CRC variant of the Pico RAM trial."""
import argparse
import difflib
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    shutil.copytree(args.source, args.output)
    path = args.output / "protocol/src/sdlp.c"
    original = path.read_text()
    start = original.index("uint32_t sdlp_crc32(")
    end = original.index("bool sdlp_encode(", start)
    table = []
    for value in range(16):
        for _ in range(4):
            value = (value >> 1) ^ (0xedb88320 if value & 1 else 0)
        table.append(f"UINT32_C(0x{value:08x})")
    initializer = ",\n      ".join(", ".join(table[i:i + 4]) for i in range(0, 16, 4))
    function = '''uint32_t sdlp_crc32(const void *data, size_t n) {
  /* Same reflected IEEE CRC32, two nibble lookups per byte. 64 B of rodata. */
  static const uint32_t table[16] = {
      INITIALIZER};
  const uint8_t *p = data;
  uint32_t crc = UINT32_MAX;
  while (n--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ table[crc & 15];
    crc = (crc >> 4) ^ table[crc & 15];
  }
  return ~crc;
}
'''.replace("INITIALIZER", initializer)
    modified = original[:start] + function + original[end:]
    path.write_text(modified)
    patch = "".join(difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
        fromfile="a/protocol/src/sdlp.c", tofile="b/protocol/src/sdlp.c"))
    (args.output.parent / "fast-crc.patch").write_text(patch)


if __name__ == "__main__":
    main()
