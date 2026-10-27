#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_display_surface.h"

typedef int u8g2_t;
typedef struct { unsigned pixel_width, pixel_height; } u8x8_display_info_t;
typedef struct { const u8x8_display_info_t *display_info; } u8x8_t;
static const u8x8_display_info_t display_info = {8, 8};
static const u8x8_t u8x8 = {&display_info};
static const u8x8_t *u8g2_GetU8x8(u8g2_t *display) { (void)display; return &u8x8; }
static unsigned u8g2_GetBufferTileWidth(u8g2_t *display) { (void)display; return 1; }
static uint32_t display_foreground_rgb888 = 0xff00ff, display_background_rgb888 = 0x00ff00;
typedef struct {
    bool export_enabled, export_publishing;
    unsigned export_readers, refs;
    uint32_t generation, export_frame_id;
    uint8_t *export_buffer;
    size_t export_buffer_size, export_rgb565_size;
    uint16_t export_native_stride;
    struct { bool black_is_one; } target;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static int display_find_slot_by_u8g2_locked(u8g2_t *display) { return display ? 0 : -1; }
static uint8_t *display_detach_export_buffer_locked(display_target_slot_t *slot)
{ (void)slot; return NULL; }
static void solar_os_memory_free(void *p) { assert(p == NULL); }
#include "export.inc"

int main(void)
{
    uint8_t rgb[8 * 20], indices[64], actual[8 + 128], indexed[8 + 128];
    uint16_t palette[256] = {0};
    u8g2_t display = 1;
    /* Pad RGB rows to check stride; use distinct R/G/B values to detect
     * accidental host-endian reads. The old indexed path is the reference. */
    memset(rgb, 0xcc, sizeof(rgb));
    for (unsigned i = 0; i < 64; ++i) {
        uint16_t color = (uint16_t)(i * 1040U + 7U);
        palette[i] = color;
        indices[i] = i;
        rgb[(i / 8) * 20 + (i % 8) * 2] = color >> 8;
        rgb[(i / 8) * 20 + (i % 8) * 2 + 1] = color & 255;
    }
    solar_os_display_surface_t surface = {
        .data = rgb, .data_size = sizeof(rgb), .width = 8, .height = 8,
        .stride = 20, .native_width = 8, .native_height = 8,
        .format = SOLAR_OS_DISPLAY_FORMAT_RGB565,
    };
    display_target_slot_t *slot = display_targets;
    *slot = (display_target_slot_t){
        .export_enabled = true, .generation = 7, .export_buffer = actual,
        .export_buffer_size = 8, .export_rgb565_size = 128, .export_native_stride = 8,
    };
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        surface.rotation = rotation;
        for (unsigned inverted = 0; inverted < 2; ++inverted) {
            slot->target.black_is_one = inverted;
            surface.data = rgb; surface.stride = 20;
            surface.format = SOLAR_OS_DISPLAY_FORMAT_RGB565;
            surface.palette_rgb565 = NULL;
            display_publish_surface(&display, &surface);
            /* Exact source colors survive export, including near-black and
             * near-white shades; palette polarity must not invert RGB. */
            for (unsigned i = 0; i < 64; ++i) {
                unsigned x = i % 8, y = i / 8, nx, ny;
                switch (rotation) {
                case 1: nx = 7-y; ny = x; break;
                case 2: nx = 7-x; ny = 7-y; break;
                case 3: nx = y; ny = 7-x; break;
                default: nx = x; ny = y; break;
                }
                size_t offset = 8 + (ny * 8 + nx) * 2;
                assert(actual[offset] == palette[i] >> 8);
                assert(actual[offset+1] == (palette[i] & 255));
            }
            memcpy(indexed, actual, sizeof(actual));
            surface.data = indices; surface.stride = 8;
            surface.format = SOLAR_OS_DISPLAY_FORMAT_INDEX8;
            surface.palette_rgb565 = palette;
            display_publish_surface(&display, &surface);
            assert(memcmp(actual, indexed, sizeof(actual)) == 0);
            assert(slot->refs == 0 && !slot->export_publishing);
        }
    }
    uint32_t frame = slot->export_frame_id;
    slot->export_readers = 1;
    memset(actual, 0xa5, sizeof(actual));
    display_publish_surface(&display, &surface);
    assert(slot->export_frame_id == frame && actual[0] == 0xa5);
    slot->export_readers = 0;
    slot->export_enabled = false;
    display_publish_surface(&display, &surface);
    assert(slot->export_frame_id == frame && actual[0] == 0xa5);
    for (size_t i = 0; i < sizeof(actual); ++i) assert(actual[i] == 0xa5);

    /* Non-square native geometry catches accidental logical row addressing.
     * Each input has six distinct primary/gray pixels, rotated into a fixed
     * native image. These explicit permutations do not use production mapping. */
    static const unsigned order[4][6] = {
        {0, 1, 2, 3, 4, 5}, {2, 5, 1, 4, 0, 3},
        {5, 4, 3, 2, 1, 0}, {3, 0, 4, 1, 5, 2},
    };
    static const uint16_t colors[6] = {0xf800, 0x07e0, 0x001f, 0, 0xffff, 0x18c3};
    uint8_t pixels[12];
    slot->export_enabled = true;
    slot->export_rgb565_size = sizeof(pixels);
    surface.native_width = 3; surface.native_height = 2;
    surface.format = SOLAR_OS_DISPLAY_FORMAT_RGB565;
    surface.data = pixels; surface.data_size = sizeof(pixels);
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        surface.rotation = rotation;
        surface.width = rotation & 1 ? 2 : 3;
        surface.height = rotation & 1 ? 3 : 2;
        surface.stride = surface.width * 2;
        for (unsigned i = 0; i < 6; ++i) {
            uint16_t color = colors[order[rotation][i]];
            pixels[i*2] = color >> 8; pixels[i*2+1] = color & 255;
        }
        display_publish_surface(&display, &surface);
        for (unsigned i = 0; i < 6; ++i) {
            assert(actual[8+i*2] == colors[i] >> 8);
            assert(actual[8+i*2+1] == (colors[i] & 255));
        }
    }
    /* Monochrome targets allocate no color tail. Never touch that memory. */
    slot->export_rgb565_size = 0;
    memset(actual + 8, 0xa5, 128);
    display_publish_surface(&display, &surface);
    for (unsigned i = 8; i < sizeof(actual); ++i) assert(actual[i] == 0xa5);
    /* U8g2 presentations and the initial snapshot also retain theme colors. */
    for (unsigned inverted = 0; inverted < 2; ++inverted) {
        memset(actual, 0x55, 8);
        display_export_mono_rgb565(&display, actual, 8, 128, inverted);
        for (unsigned i = 0; i < 64; ++i) {
            const bool foreground = ((i / 8) % 2 == 0) == (bool)inverted;
            const uint16_t color = foreground ? 0xf81f : 0x07e0;
            assert(actual[8+i*2] == color >> 8);
            assert(actual[8+i*2+1] == (color & 255));
        }
    }
    puts("RGB565/indexed export preserves exact colors, wire order and rotation; monochrome and immutable readers remain supported");
}
