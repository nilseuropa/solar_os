#include "solar_os_remote_screen.h"

#include <string.h>

#include "solar_os_gfx_internal.h"

static u8g2_t *screen_u8g2;
static const solar_os_gfx_t *screen_gfx;

void solar_os_remote_screen_attach(u8g2_t *u8g2, const solar_os_gfx_t *gfx)
{
    screen_u8g2 = u8g2;
    screen_gfx = gfx;
}

bool solar_os_remote_screen_available(void)
{
    return screen_u8g2 != NULL;
}

static uint16_t screen_native_width(void)
{
    return (uint16_t)u8g2_GetU8x8(screen_u8g2)->display_info->tile_width * 8U;
}

static uint16_t screen_native_height(void)
{
    return (uint16_t)u8g2_GetU8x8(screen_u8g2)->display_info->tile_height * 8U;
}

static bool screen_rotation_swaps_axes(void)
{
    return screen_u8g2->cb == U8G2_R1 || screen_u8g2->cb == U8G2_R3;
}

esp_err_t solar_os_remote_screen_get_size(uint16_t *width, uint16_t *height)
{
    if (screen_u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t nw = screen_native_width();
    const uint16_t nh = screen_native_height();
    if (width != NULL) {
        *width = screen_rotation_swaps_axes() ? nh : nw;
    }
    if (height != NULL) {
        *height = screen_rotation_swaps_axes() ? nw : nh;
    }
    return ESP_OK;
}

/*
 * Logical (x, y) -> native buffer (nx, ny), inverting the transforms
 * u8g2's draw_l90_r0..r3 callbacks apply at draw time (see vendored
 * u8g2_setup.c) -- the full framebuffer is always stored in the
 * panel's native orientation, rotation happens per drawing call.
 */
static void screen_map_pixel(const u8g2_cb_t *cb,
                             uint16_t nw,
                             uint16_t nh,
                             uint16_t x,
                             uint16_t y,
                             uint16_t *nx,
                             uint16_t *ny)
{
    if (cb == U8G2_R1) {
        *nx = (uint16_t)(nw - 1U - y);
        *ny = x;
    } else if (cb == U8G2_R2) {
        *nx = (uint16_t)(nw - 1U - x);
        *ny = (uint16_t)(nh - 1U - y);
    } else if (cb == U8G2_R3) {
        *nx = y;
        *ny = (uint16_t)(nh - 1U - x);
    } else {
        *nx = x;
        *ny = y;
    }
}

esp_err_t solar_os_remote_screen_snapshot(uint8_t *out,
                                          size_t out_size,
                                          uint16_t *width,
                                          uint16_t *height)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (screen_u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *buffer = screen_u8g2->tile_buf_ptr;
    if (buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t nw = screen_native_width();
    const uint16_t nh = screen_native_height();
    const bool swap = screen_rotation_swaps_axes();
    const uint16_t w = swap ? nh : nw;
    const uint16_t h = swap ? nw : nh;
    const size_t stride = ((size_t)w + 7U) / 8U;
    if (out_size < stride * h) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* bit set = white in the output; the u8g2 buffer's set bit means
     * "drawn with color 1", whose meaning the gfx layer decides via
     * black_is_one (reflective LCDs invert it). */
    const bool set_bit_is_white = screen_gfx == NULL || !screen_gfx->black_is_one;
    const u8g2_cb_t *cb = screen_u8g2->cb;

    memset(out, 0, stride * h);
    for (uint16_t y = 0; y < h; y++) {
        uint8_t *row = &out[(size_t)y * stride];
        for (uint16_t x = 0; x < w; x++) {
            uint16_t nx = 0;
            uint16_t ny = 0;
            screen_map_pixel(cb, nw, nh, x, y, &nx, &ny);

            const uint8_t native = buffer[((size_t)(ny >> 3) * nw) + nx];
            const bool bit_set = (native & (uint8_t)(1U << (ny & 7U))) != 0;
            const bool white = bit_set == set_bit_is_white;
            if (white) {
                row[x >> 3] |= (uint8_t)(0x80U >> (x & 7U));
            }
        }
    }

    if (width != NULL) {
        *width = w;
    }
    if (height != NULL) {
        *height = h;
    }
    return ESP_OK;
}
