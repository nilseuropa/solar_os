#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_indexed_raster.h"
#include "solar_os_rgb565.h"
#include "solar_os_memory.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_SIZE 2
enum { U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3 };
typedef struct {
    struct { unsigned cb; } u8g2;
    uint8_t *frame_scratch;
    size_t frame_scratch_size;
} tft_ili9341_t;

static bool fail_allocation;
static unsigned allocations;
static solar_os_memory_class_t allocation_class;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(!strcmp(tag, "display.rotate"));
    allocation_class = kind;
    allocations++;
    return fail_allocation ? NULL : malloc(size);
}
void solar_os_memory_free(void *pointer) { free(pointer); }

/* The SDK heap has no contiguous PSRAM left, while the managed pool above
 * still has space. This is the live failure that used to blank every frame. */
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void *heap_caps_malloc(size_t size, unsigned caps)
{ (void)size; (void)caps; return NULL; }
void *heap_caps_malloc_prefer(size_t size, size_t count, ...)
{ (void)size; (void)count; return NULL; }
void heap_caps_free(void *pointer) { free(pointer); }

#include "display_rotation.inc"

int main(void)
{
    /* Exercise Doom's full-size frame and both packed and RGB consumers. */
    uint8_t source[320U * 240U * 2U];
    memset(source, 0x55, sizeof(source));
    for (unsigned format = SOLAR_OS_DISPLAY_FORMAT_INDEX2;
         format <= SOLAR_OS_DISPLAY_FORMAT_RGB565; format++) {
        const unsigned stride = format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ? 80U :
            format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 320U : 640U;
        solar_os_display_raster_t frame = {
            .data = source, .data_size = stride * 240U,
            .source_width = 320, .source_height = 240,
            .source_stride = stride, .width = 320, .height = 240,
            .format = (solar_os_display_format_t)format,
        }, native;
        tft_ili9341_t display = { .u8g2.cb = U8G2_R0 };
        unsigned before = allocations;
        assert(ili9341_prepare_native_frame(&display, &frame, &native) == ESP_OK);
        assert(native.data == source && allocations == before);
        for (unsigned rotation = U8G2_R1; rotation <= U8G2_R3; rotation++) {
            display.u8g2.cb = rotation;
            assert(ili9341_prepare_native_frame(&display, &frame, &native) == ESP_OK);
            assert(native.data == display.frame_scratch);
            assert(native.data_size == frame.data_size);
            assert(native.source_width == (rotation == U8G2_R2 ? 320 : 240));
            assert(native.source_height == (rotation == U8G2_R2 ? 240 : 320));
            assert(native.data[0] == 0x55);
        }
        assert(allocations == before + 1); /* Reuse across frames/rotations. */
        assert(allocation_class == (format == SOLAR_OS_DISPLAY_FORMAT_RGB565 ?
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED : SOLAR_OS_MEMORY_EXTERNAL_PREFERRED));

        /* Failed growth must preserve the existing allocation for retry. */
        uint8_t *saved = display.frame_scratch;
        size_t saved_size = display.frame_scratch_size;
        frame.source_height = 480;
        fail_allocation = true;
        assert(ili9341_prepare_native_frame(&display, &frame, &native) == ESP_ERR_NO_MEM);
        assert(display.frame_scratch == saved && display.frame_scratch_size == saved_size);
        fail_allocation = false;
        solar_os_memory_free(display.frame_scratch);
    }
    return 0;
}
