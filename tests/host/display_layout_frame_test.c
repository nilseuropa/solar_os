#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_display_layout.h"
#include "solar_os_memory.h"

typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int StaticSemaphore_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
static bool refuse_lock;
static unsigned requests;
static int xSemaphoreTake(void *mutex, unsigned wait)
{ assert(mutex && wait == portMAX_DELAY); return !refuse_lock; }
static void xSemaphoreGive(void *mutex) { assert(mutex); requests++; }
static size_t strlcpy(char *out, const char *in, size_t size)
{ size_t length = strlen(in); if (size) snprintf(out, size, "%s", in); return length; }
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t kind, const char *tag)
{ (void)kind; (void)tag; return calloc(count, bytes); }
static solar_os_display_target_t registered;
esp_err_t solar_os_display_register_target(const solar_os_display_target_t *target)
{
    assert(target->frame_formats == SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT);
    assert(target->preferred_stream_fps && target->present_frame && target->frame_context);
    registered = *target;
    return ESP_OK;
}

#include "layout_frames.inc"

static void check_frames(solar_os_display_layout_axis_t axis, unsigned side)
{
    solar_os_display_layout_rect_t regions[2];
    assert(solar_os_display_layout_split_geometry(axis, 101, 55, regions));
    display_layout_members_t members = {0};
    uint8_t queued[104 * 7], expected_queue[sizeof(queued)];
    solar_os_display_layout_runtime_t layout = {
        .width = 101, .height = 55, .queued = queued, .members = &members,
        .mutex = &layout, .requested = &members,
    };
    assert(layout_prepare_logical(&layout, side, "pane", regions[side], "viewport") == ESP_OK);
    display_layout_logical_t *logical = &members.logical[side];
    assert(logical->registered && registered.frame_context == logical && !registered.black_is_one);
    uint8_t *expected = malloc(logical->buffer_size);
    assert(expected);
    u8g2_t reference = logical->u8g2;
    reference.tile_buf_ptr = expected;
    uint8_t source[3 * 11];
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = i * 71U + 43U;
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        u8g2_SetDisplayRotation(&logical->u8g2, rotations[rotation]);
        u8g2_SetDisplayRotation(&reference, rotations[rotation]);
        for (unsigned offset = 0; offset < 8; offset++) {
            for (unsigned flags = 0; flags < 8; flags++) {
                const solar_os_display_raster_t frame = {
                    .data = source, .data_size = sizeof(source), .source_stride = 3,
                    .source_width = 13, .source_height = 11,
                    .width = 13, .height = 11, .x = 2, .y = offset + 3,
                    .format = SOLAR_OS_DISPLAY_FORMAT_MONO1,
                    .palette_inverted = (flags & 1) != 0,
                    .clear_background = (flags & 2) != 0,
                    .background_index = (flags >> 2) & 1,
                };
                memset(logical->buffer, 0xa5, logical->buffer_size);
                memset(expected, 0xa5, logical->buffer_size);
                memset(queued, 0x69, sizeof(queued));
                memcpy(expected_queue, queued, sizeof(queued));
                if (frame.clear_background) {
                    u8g2_SetDrawColor(&reference,
                        frame.background_index == frame.palette_inverted);
                    u8g2_DrawBox(&reference, 0, 0,
                        u8g2_GetDisplayWidth(&reference), u8g2_GetDisplayHeight(&reference));
                }
                for (unsigned y = 0; y < frame.height; y++)
                    for (unsigned x = 0; x < frame.width; x++) {
                        bool black = (source[y * frame.source_stride + x / 8] >> (x & 7)) & 1;
                        u8g2_SetDrawColor(&reference, black == frame.palette_inverted);
                        u8g2_DrawPixel(&reference, frame.x + x, frame.y + y);
                    }
                /* Clear-background also initializes padding in the native buffer. */
                if (frame.clear_background) {
                    for (unsigned y = 0; y < (regions[side].height + 7U) / 8U * 8U; y++)
                        for (unsigned x = 0; x < (regions[side].width + 7U) / 8U * 8U; x++)
                            if (x >= regions[side].width || y >= regions[side].height) {
                                size_t index = (size_t)(y / 8) * ((regions[side].width + 7U) & ~7U) + x;
                                unsigned mask = 1U << (y & 7);
                                if (frame.background_index == frame.palette_inverted) expected[index] |= mask;
                                else expected[index] &= ~mask;
                            }
                }
                assert(solar_os_display_layout_mono_blit(expected_queue, 101, 55,
                    regions[side].x, regions[side].y, expected, regions[side].width, regions[side].height));
                layout.queued_ready = false;
                requests = 0;
                assert(registered.present_frame(registered.frame_context, &frame) == ESP_OK);
                assert(layout.queued_ready && requests == 2);
                assert(!memcmp(expected, logical->buffer, logical->buffer_size));
                /* The entire canvas comparison includes the untouched sibling. */
                assert(!memcmp(expected_queue, queued, sizeof(queued)));
            }
        }
    }
    solar_os_display_raster_t invalid = {
        .data = source, .data_size = sizeof(source), .source_stride = 3,
        .source_width = 13, .source_height = 11, .width = 13, .height = 11,
        .format = SOLAR_OS_DISPLAY_FORMAT_MONO1,
    };
    layout.stop_requested = true;
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_STATE);
    layout.stop_requested = false;
    refuse_lock = true;
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_STATE);
    refuse_lock = false;
    invalid.source_stride = 1;
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_ARG);
    invalid.source_stride = 3; invalid.data_size--;
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_ARG);
    invalid.data_size++; invalid.x = u8g2_GetDisplayWidth(&logical->u8g2);
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_ARG);
    invalid.x = 0; invalid.source_width--;
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_ARG);
    invalid.source_width++; invalid.format = SOLAR_OS_DISPLAY_FORMAT_INDEX2;
    assert(layout_present_frame(logical, &invalid) == ESP_ERR_INVALID_ARG);
    assert(layout_present_frame(NULL, &invalid) == ESP_ERR_INVALID_ARG);
    assert(layout_present_frame(logical, NULL) == ESP_ERR_INVALID_ARG);
    free(expected);
    free(logical->buffer);
}

int main(void)
{
    for (unsigned side = 0; side < 2; side++) {
        check_frames(SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL, side);
        check_frames(SOLAR_OS_DISPLAY_LAYOUT_VERTICAL, side);
    }
    puts("display layout streaming frames passed");
}
