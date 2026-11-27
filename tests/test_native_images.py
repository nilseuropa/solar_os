from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeImagesTest(unittest.TestCase):
    def test_elf_backend_ownership_and_failure_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "esp_elf.h").write_text('''#pragma once
#include <stdbool.h>
typedef struct { bool initialized; } esp_elf_t;
struct esp_elfsym { const char *name; void (*address)(void); };
#define ESP_ELFSYM_EXPORT(name) {#name, (void (*)(void))name}
#define ESP_ELFSYM_END {0, 0}
int esp_elf_register_symbol(const struct esp_elfsym *symbols);
int esp_elf_init(esp_elf_t *elf);
int esp_elf_relocate(esp_elf_t *elf, const void *data);
int esp_elf_request(esp_elf_t *elf, int unused, int argc, char **argv);
void esp_elf_deinit(esp_elf_t *elf);
''')
            binary = stage / "images"
            subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-pthread",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-I", str(stage), "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/native_images_test.c"),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
