from pathlib import Path
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c


ROOT = Path(__file__).resolve().parents[1]


class DisplayFramebufferMemoryTest(unittest.TestCase):
    def test_psram_preference_fallback_and_exhaustion(self):
        # Compile each driver's actual allocator with a capability-aware heap.
        # Cover both panel sizes and the padded TFT geometry used by the Pager.
        drivers = (
            ("rlcd_st7305.c", "rlcd_alloc_framebuffer", (15200, 8064), False),
            ("tft_ili9341.c", "ili9341_alloc_framebuffer",
             (9600, 19200, 13440, 7200), False),
            ("epd_ultrachip.c", "ultrachip_alloc_framebuffer", (48000,), True),
            ("epd_ssd1685.c", "ssd1685_alloc_framebuffer", (8064,), True),
        )
        for filename, allocator, sizes, zeroed in drivers:
            with self.subTest(driver=filename):
                source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "epd_stubs/esp_heap_caps.h"

static bool fail_external, fail_all;
static unsigned attempts;
static size_t requested_size[2];
static uint32_t requested_caps[2];

static void *test_allocate(size_t count, size_t size, uint32_t caps, bool zero)
{
    assert(attempts < 2);
    requested_size[attempts] = count * size;
    requested_caps[attempts++] = caps;
    if (fail_all || (fail_external && (caps & MALLOC_CAP_SPIRAM))) return NULL;
    void *buffer = malloc(count * size);
    assert(buffer);
    memset(buffer, zero ? 0 : 0xa5, count * size);
    return buffer;
}
'''
                if zeroed:
                    source += r'''
static void *test_calloc(size_t count, size_t size, uint32_t caps)
{ return test_allocate(count, size, caps, true); }
#define heap_caps_calloc test_calloc
'''
                else:
                    source += r'''
static void *test_malloc(size_t size, uint32_t caps)
{ return test_allocate(1, size, caps, false); }
#define heap_caps_malloc test_malloc
'''
                source += production_functions(
                    (ROOT / "src/drivers" / filename).read_text(), (allocator,))
                source += "\nstatic const size_t sizes[] = {" + ",".join(map(str, sizes)) + "};\n"
                source += f"#define ALLOC_FRAME {allocator}\n#define ZEROED {int(zeroed)}\n"
                source += r'''
int main(void)
{
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        for (unsigned external_missing = 0; external_missing < 2; ++external_missing) {
            attempts = 0;
            fail_external = external_missing;
            uint8_t *buffer = ALLOC_FRAME(sizes[i]);
            assert(buffer && attempts == 1 + external_missing);
            assert(requested_size[0] == sizes[i]);
            assert(requested_caps[0] == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (external_missing) {
                assert(requested_size[1] == sizes[i]);
                assert(requested_caps[1] == MALLOC_CAP_8BIT);
            }
            /* The calloc drivers must remain zeroed on either heap. */
            if (ZEROED)
                for (size_t byte = 0; byte < sizes[i]; ++byte) assert(buffer[byte] == 0);
            buffer[0] = 0x5a;
            buffer[sizes[i] - 1] = 0xa5;
            assert(buffer[0] == 0x5a && buffer[sizes[i] - 1] == 0xa5);
            heap_caps_free(buffer);
        }
        attempts = 0;
        fail_all = true;
        assert(ALLOC_FRAME(sizes[i]) == NULL && attempts == 2);
        fail_all = false;
    }
}
'''
                run_c(source)
