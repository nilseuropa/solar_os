#!/usr/bin/env python3
"""Keep ELF virtual bounds exact and route resident images through host policy."""
import argparse
from pathlib import Path

OLD = "elf->sec[ELF_SEC_TEXT].size    = ELF_ALIGN(shdr[i].size, 4);"
NEW = "elf->sec[ELF_SEC_TEXT].size    = shdr[i].size; /* Virtual bounds exclude padding. */"


def patch(component: Path):
    source = component / "src/esp_elf.c"
    text = source.read_text()
    if NEW not in text:
        if text.count(OLD) != 1:
            raise ValueError("ELF loader source changed; review the text-bound overlay")
        source.write_text(text.replace(OLD, NEW))
    adapter = component / "src/esp_elf_adapter.c"
    if adapter.exists():
        text = adapter.read_text()
        old = "    return heap_caps_malloc(n, caps);"
        new = """#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    extern void *solar_os_memory_elf_alloc(size_t bytes);
    (void)caps;
    return solar_os_memory_elf_alloc(n);
#else
    return heap_caps_malloc(n, caps);
#endif"""
        if new not in text:
            if text.count(old) != 1:
                raise ValueError("ELF loader source changed; review the resident allocation overlay")
            adapter.write_text(text.replace(old, new))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("component", type=Path)
    patch(parser.parse_args().component)
