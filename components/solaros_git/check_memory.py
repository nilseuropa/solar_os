#!/usr/bin/env python3
"""Verify Git's RAM placement and initialized PSRAM state in a linked firmware.

Run with the PlatformIO Python environment (which provides pyelftools).
"""
import argparse
from pathlib import Path
import re
import struct

from elftools.elf.elffile import ELFFile


def verify(path):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols()}
        sections = list(elf.iter_sections())

        def region(address, size=1):
            return next((s.name for s in sections if s["sh_flags"] & 2 and
                         s["sh_addr"] <= address and
                         address + size <= s["sh_addr"] + s["sh_size"]), None)

        assert elf.elfclass == 32 and elf.little_endian
        for name, expected in (("sha1_dvs", ".flash.rodata"),
                               ("git__allocator", ".ext_ram.bss"),
                               ("git_mwindow__mem_ctl", ".ext_ram.bss"),
                               ("git_busy", ".dram0.bss")):
            symbol = symbols[name]
            assert region(symbol["st_value"], symbol["st_size"]) == expected, name

        start = symbols["_solar_git_initializers_start"]["st_value"]
        end = symbols["_solar_git_initializers_end"]["st_value"]
        assert end > start and (end - start) % 12 == 0, "missing or misaligned Git initializers"
        assert region(start, end - start) == ".flash.rodata"
        rodata = elf.get_section_by_name(".flash.rodata")
        data = rodata.data()[start - rodata["sh_addr"]:end - rodata["sh_addr"]]
        initialized_bytes = 0
        for offset in range(0, len(data), 12):
            destination, initial_value, size = struct.unpack_from("<III", data, offset)
            assert size and region(destination, size) == ".ext_ram.bss", hex(destination)
            assert region(initial_value, size) == ".flash.rodata", hex(initial_value)
            initialized_bytes += size

        internal = []
        external_bytes = 0
        for line in path.with_suffix(".map").read_text().splitlines():
            match = re.match(r"\s*(?:\S+\s+)?0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+"
                             r".*libsolaros_git\.a\(([^)]+)\)", line)
            if not match:
                continue
            address, size = int(match[1], 16), int(match[2], 16)
            if not size:
                continue
            placement = region(address, size)
            if placement in (".dram0.data", ".dram0.bss"):
                internal.append((match[3], size, hex(address)))
            if placement == ".ext_ram.bss":
                external_bytes += size
        assert not internal, f"Git library globals still in internal SRAM: {internal}"
        assert external_bytes > initialized_bytes
        print(f"Git memory: {external_bytes} bytes in PSRAM, 0 library bytes in internal SRAM; "
              f"{len(data) // 12} flash initializers validated ({initialized_bytes} bytes)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    verify(parser.parse_args().elf)
