#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_display_layer.h"
#include "solar_os_display_scale.h"
#include "solar_os_memory.h"

typedef void *SemaphoreHandle_t;
typedef int solar_os_display_present_mode_t;
typedef struct {
    struct { u8g2_t *u8g2; } target;
    solar_os_display_scale_t scale;
    solar_os_display_layer_t *layer;
    SemaphoreHandle_t present_mutex;
    uint32_t generation;
    unsigned refs;
    bool overlay_active, power_state_active, relax_active;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
static int display_targets_lock;
static uint8_t *physical, *panel;
static size_t physical_size;
static unsigned refreshes;
static u8x8_display_info_t info;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
static int xSemaphoreTake(SemaphoreHandle_t m, unsigned wait) {assert(m); return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t m) {assert(m);}
static int display_find_slot_by_u8g2_locked(u8g2_t *canvas)
{ return canvas == display_targets[0].target.u8g2 ? 0 : -1; }
static void display_release_slot_ref(size_t index, uint32_t generation)
{ assert(index == 0 && display_targets[0].refs == 1); display_targets[0].refs--; }
static bool display_overlay_active(size_t index, uint32_t generation) {return false;}
static void display_activate_pending_overlay_locked(display_target_slot_t *slot) {}
static void display_layer_update_colors(display_target_slot_t *slot) {}
static void display_publish_layer_locked(display_target_slot_t *slot) {}
static void display_publish_frame(u8g2_t *canvas) {}

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t kind, const char *tag)
{return malloc(bytes);}
void *solar_os_memory_calloc(size_t n, size_t bytes, solar_os_memory_class_t kind, const char *tag)
{return calloc(n, bytes);}
void solar_os_memory_free(void *pointer) {free(pointer);}

/* Like SSD1677, this driver refreshes its own buffer and ignores DRAW_TILE. */
static uint8_t driver(u8x8_t *display, uint8_t message, uint8_t count, void *argument)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY)
        u8x8_d_helper_display_setup_memory(display, &info);
    else if (message == U8X8_MSG_DISPLAY_REFRESH) {
        memcpy(panel, physical, physical_size);
        refreshes++;
    }
    return 1;
}

#include "display_layer_present.inc"

static void check_panel(display_target_slot_t *slot, const uint8_t *composed)
{
    if (slot->scale.factor == 1U) assert(!memcmp(panel, composed, physical_size));
    else {
        /* Independently expand the logical canvas, including rotated layouts. */
        unsigned f = slot->scale.factor, stride = slot->scale.logical_info.tile_width * 8U;
        for (unsigned y = 0; y < info.pixel_height; y++)
            for (unsigned x = 0; x < info.pixel_width; x++) {
                bool expected = (composed[(y/f/8U)*stride+x/f] >> (y/f & 7U)) & 1U;
                assert(!!(panel[(y/8U)*info.tile_width*8U+x] & (1U << (y & 7U))) == expected);
            }
    }
}

static void check_frame(display_target_slot_t *slot)
{
    check_panel(slot, solar_os_display_layer_canvas(slot->layer, false));
    assert(slot->target.u8g2->tile_buf_ptr == solar_os_display_layer_base(slot->layer));
}

int main(void)
{
    info = (u8x8_display_info_t){.pixel_width=800, .pixel_height=480,
        .tile_width=100, .tile_height=60};
    physical_size = 800U * 480U / 8U;
    physical = calloc(1, physical_size); panel = calloc(1, physical_size);
    u8g2_t canvas = {0};
    u8g2_SetupDisplay(&canvas, driver, NULL, NULL, NULL);
    u8g2_SetupBuffer(&canvas, physical, info.tile_height, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    display_target_slot_t *slot = &display_targets[0];
    slot->target.u8g2 = &canvas; slot->present_mutex = slot;
    solar_os_display_scale_init(&slot->scale, &canvas);
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    for (unsigned factor = 1; factor <= 2; factor++) {
        assert(solar_os_display_scale_prepare(&slot->scale, factor) == ESP_OK);
        solar_os_display_scale_apply(&slot->scale, factor);
        for (unsigned r = 0; r < 4; r++) {
            u8g2_SetDisplayRotation(&canvas, rotations[r]);
            u8g2_ClearBuffer(&canvas); u8g2_DrawPixel(&canvas, 1, 2);
            solar_os_display_present_regions(&canvas, 0, NULL, 0);
            uint8_t *logical = slot->scale.logical_buffer;
            slot->layer = solar_os_display_layer_create(&canvas, true, false, 0, 0xffff);
            assert(slot->layer);
            if (factor > 1) slot->scale.logical_buffer = solar_os_display_layer_base(slot->layer);
            uint8_t ink[] = {0x81, 0x42, 0x24, 0x18};
            solar_os_display_layer_update(slot->layer, ink, 24, 32, 8, 4);
            unsigned before = refreshes;
            display_draw_layer_locked(slot); check_frame(slot);
            assert(refreshes == before+1);
            /* Updating selection and repainting the application preserve the overlay. */
            ink[0] = 0x7e;
            solar_os_display_layer_update(slot->layer, ink, 24, 32, 8, 4);
            display_draw_layer_locked(slot); check_frame(slot);
            u8g2_DrawPixel(&canvas, 7, 9);
            const solar_os_display_rect_t area = {7, 9, 1, 1};
            solar_os_display_present_regions(&canvas, 0, &area, 1); check_frame(slot);
            assert(refreshes == before+3 && !slot->refs);
            /* The clean backing canvas never contains keyboard pixels. */
            uint8_t packed[4];
            solar_os_display_canvas_pack(&canvas, packed, 24, 32, 8, 4);
            assert(!memcmp(packed, (uint8_t[4]){0}, 4));
            solar_os_display_layer_destroy(slot->layer); slot->layer = NULL;
            slot->scale.logical_buffer = logical;
            solar_os_display_present_regions(&canvas, 0, NULL, 0);
            check_panel(slot, u8g2_GetBufferPtr(&canvas));
        }
    }
    solar_os_display_scale_destroy(&slot->scale);
    free(physical); free(panel);
    puts("layer presentation reaches fixed driver framebuffer at scales 1 and 2: ok");
}
