#include "solar_os_display_scale.h"

#include <string.h>
#include "solar_os_memory.h"
#include "solar_os_indexed_raster.h"

static solar_os_display_scale_t *scales[6];
static uint8_t scale_callback(u8x8_t *, uint8_t, uint8_t, void *);

static void use_view(solar_os_display_scale_t *state, bool native)
{
    u8g2_t *canvas = state->canvas;
    const bool scaled = !native && state->factor > 1U;
    canvas->u8x8.display_info = scaled ? &state->logical_info : state->physical_info;
    canvas->u8x8.display_cb = scaled ? scale_callback : state->physical_callback;
    canvas->tile_buf_ptr = scaled ? state->logical_buffer : state->physical_buffer;
    canvas->tile_buf_height = scaled ? state->logical_info.tile_height : state->physical_rows;
    canvas->tile_curr_row = 0;
    u8g2_SetDisplayRotation(canvas, canvas->cb);
}

void solar_os_display_scale_init(solar_os_display_scale_t *state, u8g2_t *canvas)
{
    memset(state, 0, sizeof(*state));
    state->canvas = canvas;
    state->physical_info = canvas->u8x8.display_info;
    state->physical_callback = canvas->u8x8.display_cb;
    state->physical_buffer = u8g2_GetBufferPtr(canvas);
    state->physical_rows = u8g2_GetBufferTileHeight(canvas);
    state->factor = 1;
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (scales[i] == NULL) { scales[i] = state; break; }
    }
}

void solar_os_display_scale_begin_native(solar_os_display_scale_t *state)
{
    if (state->factor > 1U && state->native_depth++ == 0U) use_view(state, true);
}

void solar_os_display_scale_end_native(solar_os_display_scale_t *state)
{
    if (state->factor > 1U && state->native_depth > 0U && --state->native_depth == 0U)
        use_view(state, false);
}

void solar_os_display_scale_destroy(solar_os_display_scale_t *state)
{
    if (state->canvas != NULL) use_view(state, true);
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (scales[i] == state) scales[i] = NULL;
    }
    solar_os_memory_free(state->logical_buffer);
    solar_os_memory_free(state->surface_buffer);
    solar_os_memory_free(state->raster_buffer);
    memset(state, 0, sizeof(*state));
}

esp_err_t solar_os_display_scale_prepare(solar_os_display_scale_t *state, uint8_t factor)
{
    if (factor < 1U || factor > SOLAR_OS_DISPLAY_SCALE_MAX ||
        state->physical_info->pixel_width / factor == 0U ||
        state->physical_info->pixel_height / factor == 0U) return ESP_ERR_INVALID_ARG;
    if (factor == 1U) return ESP_OK;
    if (state->canvas->ll_hvline != u8g2_ll_hvline_vertical_top_lsb ||
        state->physical_rows != state->physical_info->tile_height) return ESP_ERR_NOT_SUPPORTED;
    const size_t columns = (state->physical_info->pixel_width / factor + 7U) / 8U;
    const size_t rows = (state->physical_info->pixel_height / factor + 7U) / 8U;
    const size_t bytes = columns * rows * 8U;
    if (bytes > state->logical_capacity) {
        uint8_t *buffer = solar_os_memory_alloc(bytes,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.scale");
        if (buffer == NULL) return ESP_ERR_NO_MEM;
        if (state->logical_buffer != NULL) memcpy(buffer, state->logical_buffer, state->logical_capacity);
        uint8_t *old = state->logical_buffer;
        state->logical_buffer = buffer;
        state->logical_capacity = bytes;
        if (state->factor > 1U) state->canvas->tile_buf_ptr = buffer;
        solar_os_memory_free(old);
    }
    return ESP_OK;
}

void solar_os_display_scale_apply(solar_os_display_scale_t *state, uint8_t factor)
{
    state->factor = factor;
    state->logical_info = *state->physical_info;
    state->logical_info.pixel_width /= factor;
    state->logical_info.pixel_height /= factor;
    state->logical_info.tile_width = (state->logical_info.pixel_width + 7U) / 8U;
    state->logical_info.tile_height = (state->logical_info.pixel_height + 7U) / 8U;
    use_view(state, false);
    u8g2_ClearBuffer(state->canvas);
    state->dirty = false;
    if (factor == 1U) {
        solar_os_memory_free(state->logical_buffer);
        solar_os_memory_free(state->surface_buffer);
        solar_os_memory_free(state->raster_buffer);
        state->logical_buffer = NULL; state->logical_capacity = 0;
        state->surface_buffer = NULL; state->surface_capacity = 0;
        state->raster_buffer = NULL; state->raster_capacity = 0;
    }
}

static uint32_t expand_byte(uint8_t value, unsigned factor)
{
    uint32_t bits = value;
    if (factor == 2U) {
        bits = (bits | (bits << 4U)) & 0x0f0fU;
        bits = (bits | (bits << 2U)) & 0x3333U;
        bits = (bits | (bits << 1U)) & 0x5555U;
        return bits * 3U;
    }
    if (factor == 3U) {
        bits = (bits | (bits << 8U)) & 0x00f00fU;
        bits = (bits | (bits << 4U)) & 0x0c30c3U;
        bits = (bits | (bits << 2U)) & 0x249249U;
        return bits * 7U;
    }
    bits = (bits | (bits << 12U)) & 0x000f000fU;
    bits = (bits | (bits << 6U)) & 0x03030303U;
    bits = (bits | (bits << 3U)) & 0x11111111U;
    return bits * 15U;
}

static void expand_rectangle(solar_os_display_scale_t *state,
    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    const unsigned stride = state->physical_info->tile_width * 8U;
    const unsigned source_stride = state->logical_info.tile_width * 8U;
    const unsigned factor = state->factor;
    for (unsigned row = y0 / 8U; row < (y1 + 7U) / 8U; row++) {
        const unsigned source_y = row / factor * 8U;
        const unsigned shift = row % factor * 8U;
        const unsigned available = source_y < state->logical_info.pixel_height ?
            state->logical_info.pixel_height - source_y : 0U;
        const uint8_t source_mask = available >= 8U ? 0xffU : (1U << available) - 1U;
        const unsigned top = row * 8U < y0 ? y0 - row * 8U : 0U;
        const unsigned bottom = (row + 1U) * 8U > y1 ? (row + 1U) * 8U - y1 : 0U;
        const uint8_t mask = (0xffU << top) & (0xffU >> bottom);
        uint8_t *destination = state->physical_buffer + row * stride;
        for (unsigned x = x0; x < x1;) {
            const unsigned sx = x / factor;
            unsigned end = (sx + 1U) * factor;
            if (end > x1) end = x1;
            const uint8_t source = available && sx < state->logical_info.pixel_width ?
                state->logical_buffer[(row / factor) * source_stride + sx] & source_mask : 0U;
            const uint8_t value = expand_byte(source, factor) >> shift;
            if (mask == 0xffU) {
                memset(destination + x, value, end - x);
                x = end;
            } else while (x < end) {
                destination[x] = (destination[x] & (uint8_t)~mask) | (value & mask);
                x++;
            }
        }
    }
}

void solar_os_display_scale_sync_native(solar_os_display_scale_t *state)
{
    if (state->factor > 1U)
        expand_rectangle(state, 0, 0, state->physical_info->pixel_width, state->physical_info->pixel_height);
}

static uint8_t scale_callback(u8x8_t *display, uint8_t message, uint8_t count, void *argument)
{
    solar_os_display_scale_t *state = NULL;
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (scales[i] != NULL && &scales[i]->canvas->u8x8 == display) { state = scales[i]; break; }
    }
    if (state == NULL) return 0;
    if (message == U8X8_MSG_DISPLAY_DRAW_TILE) {
        const u8x8_tile_t *tile = argument;
        for (uint8_t i = 0; i < count; i++) {
            if (tile[i].y_pos < state->logical_info.tile_height &&
                tile[i].x_pos + tile[i].cnt <= state->logical_info.tile_width) {
                size_t offset = ((size_t)tile[i].y_pos * state->logical_info.tile_width + tile[i].x_pos) * 8U;
                memmove(state->logical_buffer + offset, tile[i].tile_ptr, (size_t)tile[i].cnt * 8U);
                const uint8_t x1 = tile[i].x_pos + tile[i].cnt;
                const uint8_t y1 = tile[i].y_pos + 1U;
                if (!state->dirty) {
                    state->dirty_x0 = tile[i].x_pos; state->dirty_y0 = tile[i].y_pos;
                    state->dirty_x1 = x1; state->dirty_y1 = y1;
                } else {
                    if (tile[i].x_pos < state->dirty_x0) state->dirty_x0 = tile[i].x_pos;
                    if (tile[i].y_pos < state->dirty_y0) state->dirty_y0 = tile[i].y_pos;
                    if (x1 > state->dirty_x1) state->dirty_x1 = x1;
                    if (y1 > state->dirty_y1) state->dirty_y1 = y1;
                }
                state->dirty = true;
            }
        }
        return 1;
    }
    if (message == U8X8_MSG_DISPLAY_REFRESH && state->dirty) {
        const unsigned width = state->physical_info->pixel_width;
        const unsigned height = state->physical_info->pixel_height;
        const unsigned stride = state->physical_info->tile_width * 8U;
        unsigned x0 = state->dirty_x0 * state->factor * 8U;
        unsigned y0 = state->dirty_y0 * state->factor * 8U;
        unsigned x1 = state->dirty_x1 * state->factor * 8U;
        unsigned y1 = state->dirty_y1 * state->factor * 8U;
        if (x1 > width) x1 = width;
        if (y1 > height) y1 = height;
        expand_rectangle(state, x0, y0, x1, y1);
        solar_os_display_scale_begin_native(state);
        for (unsigned row = y0 / 8U; row < (y1 + 7U) / 8U; row++) {
            u8x8_tile_t tile = {.x_pos=x0/8U, .y_pos=row, .cnt=(x1+7U)/8U-x0/8U,
                .tile_ptr=state->physical_buffer + (size_t)row * stride + x0};
            state->physical_callback(display, U8X8_MSG_DISPLAY_DRAW_TILE, 1, &tile);
        }
        state->physical_callback(display, message, count, argument);
        solar_os_display_scale_end_native(state);
        state->dirty = false;
        return 1;
    }
    solar_os_display_scale_begin_native(state);
    uint8_t result = state->physical_callback(display, message, count, argument);
    solar_os_display_scale_end_native(state);
    return result;
}

esp_err_t solar_os_display_scale_raster(solar_os_display_scale_t *state,
    const solar_os_display_raster_t *source, solar_os_display_raster_t *output)
{
    *output = *source;
    const unsigned scale = state->factor;
    if (scale > 1U && (source->source_width != source->width || source->source_height != source->height)) {
        if (source->data == NULL || source->source_width == 0U || source->source_height == 0U ||
            source->width == 0U || source->height == 0U) return ESP_ERR_INVALID_ARG;
        size_t stride;
        switch (source->format) {
        case SOLAR_OS_DISPLAY_FORMAT_MONO1: stride = (source->width + 7U) / 8U; break;
        case SOLAR_OS_DISPLAY_FORMAT_INDEX2: stride = (source->width + 3U) / 4U; break;
        case SOLAR_OS_DISPLAY_FORMAT_INDEX8: stride = source->width; break;
        case SOLAR_OS_DISPLAY_FORMAT_RGB565: stride = (size_t)source->width * 2U; break;
        default: return ESP_ERR_INVALID_ARG;
        }
        const size_t bytes = stride * source->height;
        if (bytes > state->raster_capacity) {
            uint8_t *buffer = solar_os_memory_alloc(bytes,
                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.scale.raster");
            if (buffer == NULL) return ESP_ERR_NO_MEM;
            solar_os_memory_free(state->raster_buffer);
            state->raster_buffer = buffer; state->raster_capacity = bytes;
        }
        memset(state->raster_buffer, 0, bytes);
        for (unsigned y = 0; y < source->height; y++) {
            const uint8_t *row = source->data + ((size_t)y * source->source_height / source->height) * source->source_stride;
            uint8_t *out = state->raster_buffer + (size_t)y * stride;
            for (unsigned x = 0; x < source->width; x++) {
                unsigned sx = (uint32_t)x * source->source_width / source->width;
                if (source->format == SOLAR_OS_DISPLAY_FORMAT_MONO1) {
                    if (row[sx / 8U] & (1U << (sx & 7U))) out[x / 8U] |= (uint8_t)(1U << (x & 7U));
                } else if (source->format == SOLAR_OS_DISPLAY_FORMAT_RGB565) {
                    out[x * 2U] = row[sx * 2U]; out[x * 2U + 1U] = row[sx * 2U + 1U];
                } else solar_os_indexed_set(out, x, solar_os_indexed_pixel(row, sx, source->format), source->format);
            }
        }
        output->data = state->raster_buffer; output->data_size = bytes;
        output->source_width = source->width; output->source_height = source->height;
        output->source_stride = stride;
    }
    output->x *= scale; output->y *= scale;
    output->width *= scale; output->height *= scale;
    const unsigned remainder_x = state->physical_info->pixel_width % scale;
    const unsigned remainder_y = state->physical_info->pixel_height % scale;
    if (state->canvas->cb == U8G2_R1) output->y += remainder_x;
    else if (state->canvas->cb == U8G2_R2) { output->x += remainder_x; output->y += remainder_y; }
    else if (state->canvas->cb == U8G2_R3) output->x += remainder_y;
    return ESP_OK;
}

esp_err_t solar_os_display_scale_bitmap(solar_os_display_scale_t *state,
    const solar_os_display_raster_t *source, solar_os_display_raster_t *output)
{
    if (source->data == NULL || source->format != SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
        source->source_width != source->width || source->source_height != source->height ||
        source->width == 0U || source->height == 0U ||
        source->source_stride < (source->width + 7U) / 8U ||
        source->data_size < (size_t)source->source_stride * source->height ||
        (uint32_t)source->x + source->width > u8g2_GetDisplayWidth(state->canvas) ||
        (uint32_t)source->y + source->height > u8g2_GetDisplayHeight(state->canvas))
        return ESP_ERR_INVALID_ARG;
    esp_err_t ret = solar_os_display_scale_raster(state, source, output);
    if (ret != ESP_OK || state->factor == 1U) return ret;
    const size_t stride = (output->width + 7U) / 8U;
    const size_t bytes = stride * output->height;
    if (bytes > state->raster_capacity) {
        uint8_t *buffer = solar_os_memory_alloc(bytes,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.scale.bitmap");
        if (buffer == NULL) return ESP_ERR_NO_MEM;
        solar_os_memory_free(state->raster_buffer);
        state->raster_buffer = buffer; state->raster_capacity = bytes;
    }
    memset(state->raster_buffer, 0, bytes);
    for (unsigned y = 0; y < output->height; y++) {
        const uint8_t *row = source->data + (y / state->factor) * source->source_stride;
        uint8_t *out = state->raster_buffer + (size_t)y * stride;
        for (unsigned x = 0; x < output->width; x++) {
            const unsigned sx = x / state->factor;
            if (row[sx / 8U] & (1U << (sx & 7U))) out[x / 8U] |= (uint8_t)(1U << (x & 7U));
        }
    }
    output->data = state->raster_buffer; output->data_size = bytes;
    output->source_width = output->width; output->source_height = output->height;
    output->source_stride = stride;
    return ESP_OK;
}

esp_err_t solar_os_display_scale_surface(solar_os_display_scale_t *state,
    const solar_os_display_surface_t *source, solar_os_display_surface_t *output)
{
    *output = *source;
    if (state->factor == 1U) return ESP_OK;
    const unsigned width = state->physical_info->pixel_width;
    const unsigned height = state->physical_info->pixel_height;
    const unsigned columns = (width + 7U) / 8U, rows = (height + 7U) / 8U;
    const size_t pixels = (size_t)width * height;
    const size_t dirty_bytes = ((columns + 7U) / 8U) * rows;
    const size_t hash_offset = (pixels + dirty_bytes + 3U) & ~(size_t)3U;
    const size_t bytes = hash_offset + (size_t)columns * rows * sizeof(uint32_t);
    if (source->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 || source->data == NULL ||
        source->native_width != state->logical_info.pixel_width ||
        source->native_height != state->logical_info.pixel_height ||
        source->stride < source->width ||
        source->data_size < (size_t)source->stride * source->height) return ESP_ERR_INVALID_SIZE;
    const bool swap = source->rotation == SOLAR_OS_DISPLAY_ROTATION_90 || source->rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    if (source->width != (swap ? source->native_height : source->native_width) ||
        source->height != (swap ? source->native_width : source->native_height)) return ESP_ERR_INVALID_SIZE;
    if (bytes > state->surface_capacity) {
        uint8_t *buffer = solar_os_memory_calloc(1, bytes,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.scale.surface");
        if (buffer == NULL) return ESP_ERR_NO_MEM;
        solar_os_memory_free(state->surface_buffer);
        state->surface_buffer = buffer;
        state->surface_capacity = bytes;
    }
    for (unsigned y = 0; y < height; y++) {
        unsigned sy = y / state->factor;
        if (sy >= source->native_height) sy = source->native_height - 1U;
        for (unsigned x = 0; x < width; x++) {
            unsigned sx = x / state->factor;
            if (sx >= source->native_width) sx = source->native_width - 1U;
            unsigned logical_x = sx, logical_y = sy;
            switch (source->rotation) {
            case SOLAR_OS_DISPLAY_ROTATION_90:
                logical_x = sy; logical_y = source->native_width - 1U - sx; break;
            case SOLAR_OS_DISPLAY_ROTATION_180:
                logical_x = source->native_width - 1U - sx;
                logical_y = source->native_height - 1U - sy; break;
            case SOLAR_OS_DISPLAY_ROTATION_270:
                logical_x = source->native_height - 1U - sy; logical_y = sx; break;
            default: break;
            }
            state->surface_buffer[(size_t)y * width + x] = source->data[(size_t)logical_y * source->stride + logical_x];
        }
    }
    memset(state->surface_buffer + pixels, 0xff, dirty_bytes);
    output->data = state->surface_buffer; output->data_size = pixels;
    output->native_width = width; output->native_height = height;
    output->rotation = SOLAR_OS_DISPLAY_ROTATION_0;
    output->width = width; output->height = height;
    output->stride = width; output->tile_size = 8;
    output->dirty_tiles = state->surface_buffer + pixels; output->dirty_size = dirty_bytes;
    output->dirty_stride = (columns + 7U) / 8U;
    output->presented_hashes = (uint32_t *)(state->surface_buffer + hash_offset);
    output->presented_hash_count = (size_t)columns * rows; output->hash_stride = columns;
    return ESP_OK;
}
