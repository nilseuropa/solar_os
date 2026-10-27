from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MemoryPolicyTest(unittest.TestCase):
    def test_mixed_heaps_reallocation_and_reclamation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "freertos").mkdir()
            (root / "freertos/portmacro.h").write_text(
                '#include "freertos/FreeRTOS.h"\nextern int test_isr;\n#define xPortInIsrContext() test_isr\n')
            (root / "esp_attr.h").write_text("#define IRAM_ATTR\n")
            (root / "esp_memory_utils.h").write_text(
                "#include <stdbool.h>\nbool esp_ptr_external_ram(const void *);\n")
            (root / "solar_os_log.h").write_text(
                "void test_log(const char *, const char *, ...);\n"
                "#define SOLAR_OS_LOGW(tag, ...) test_log(tag, __VA_ARGS__)\n")
            (root / "esp_heap_caps.h").write_text("""
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
#define MALLOC_CAP_DMA 8U
void *heap_caps_malloc(size_t, uint32_t);
void *heap_caps_calloc(size_t, size_t, uint32_t);
void *heap_caps_aligned_alloc(size_t, size_t, uint32_t);
void *heap_caps_realloc(void *, size_t, uint32_t);
void heap_caps_free(void *);
size_t heap_caps_get_allocated_size(const void *);
size_t heap_caps_get_total_size(uint32_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
size_t strlcpy(char *, const char *, size_t);
""")
            # Compile the production service against the mock SDK headers.
            # Relocating its text lets quoted SDK/log includes resolve here.
            (root / "memory.c").write_text(
                (ROOT / "src/services/solar_os_memory.c").read_text())
            binary = root / "policy"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + tmp, "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/memory_policy_test.c"),
                str(root / "memory.c"),
                str(ROOT / "src/services/solar_os_memory_arena.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
