from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("native_elf_patch", ROOT / "scripts/patch_native_elf.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class NativeElfOverlayTest(unittest.TestCase):
    def test_patch_is_idempotent_and_refuses_unknown_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            component = Path(tmp)
            source = component / "src/esp_elf.c"
            source.parent.mkdir()
            source.write_text(PATCH.OLD)
            adapter = source.parent / "esp_elf_adapter.c"
            adapter.write_text("void *allocate(size_t n, unsigned caps) {\n    return heap_caps_malloc(n, caps);\n}\n")
            PATCH.patch(component)
            self.assertEqual(source.read_text(), PATCH.NEW)
            PATCH.patch(component)
            self.assertEqual(source.read_text(), PATCH.NEW)
            overlaid_adapter = adapter.read_text()
            self.assertEqual(overlaid_adapter.count("solar_os_memory_elf_alloc(n)"), 1)
            self.assertIn("#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM", overlaid_adapter)
            self.assertIn("#else\n    return heap_caps_malloc(n, caps);", overlaid_adapter)
            adapter.write_text("changed allocation contract")
            with self.assertRaisesRegex(ValueError, "resident allocation overlay"):
                PATCH.patch(component)
            source.write_text("new loader implementation")
            with self.assertRaisesRegex(ValueError, "source changed"):
                PATCH.patch(component)

    def test_adjacent_byte_data_is_not_mapped_to_padded_instruction_space(self):
        vendor = ROOT / "managed_components/espressif__elf_loader/src/esp_elf.c"
        if not vendor.exists():
            self.skipTest("ELF loader dependency is not installed")
        source = vendor.read_text()
        mapper = production_functions(source, ("esp_elf_map_sym",))
        code = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { uintptr_t v_addr, addr, size; } section_t;
typedef struct { section_t sec[5]; } esp_elf_t;
#define ELF_SECS 5
#define ELF_SEC_TEXT 0
''' + mapper + r'''
int main(void) {
    esp_elf_t object = {0}, *elf = &object;
    struct { uintptr_t size; } shdr[] = {{6}};
    unsigned i = 0;
    elf->sec[0].v_addr = 0x1000; elf->sec[0].addr = 0x42001000;
''' + PATCH.NEW + r'''
    elf->sec[2] = (section_t){0x1006, 0x3d001000, 4};
    assert(esp_elf_map_sym(elf, 0x1005) == 0x42001005);
    assert(esp_elf_map_sym(elf, 0x1006) == 0x3d001000);
    assert(esp_elf_map_sym(elf, 0x1007) == 0x3d001001);
    assert(esp_elf_map_sym(elf, 0x100a) == 0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "bounds")
            subprocess.run(["cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-o", str(binary), "-"], input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
