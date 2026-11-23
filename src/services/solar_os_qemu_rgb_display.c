#include "solar_os_qemu_rgb_display.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "solar_os_board_display.h"
#include "solar_os_board.h"
#include "solar_os_display.h"
#include "solar_os_indexed_raster.h"
#include "u8g2.h"

/* Espressif QEMU's virtual RGB panel (esp32s3 machine, shown with -display sdl).
 * The panel only exists in QEMU; esp_lcd_new_rgb_qemu() refuses real hardware.
 * Each refresh waits until QEMU's display loop copies the frame, so a run
 * without a display backend (-nographic) blocks at the first refresh. */

#if SOLAR_OS_BOARD_HAS_DISPLAY
#define QEMU_RGB_WIDTH SOLAR_OS_BOARD_DISPLAY_WIDTH
#define QEMU_RGB_HEIGHT SOLAR_OS_BOARD_DISPLAY_HEIGHT
#else
#define QEMU_RGB_WIDTH 480U
#define QEMU_RGB_HEIGHT 320U
#endif
#define QEMU_RGB_TILE_WIDTH ((QEMU_RGB_WIDTH + 7U) / 8U)
#define QEMU_RGB_TILE_HEIGHT ((QEMU_RGB_HEIGHT + 7U) / 8U)
#define QEMU_RGB_DRAW_STRIDE (QEMU_RGB_TILE_WIDTH * 8U)
#define QEMU_RGB_DRAW_BUFFER_SIZE (QEMU_RGB_DRAW_STRIDE * QEMU_RGB_TILE_HEIGHT)

typedef struct {
    bool active;
    bool primary;
    bool mono;
    bool mono_valid;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    esp_lcd_panel_handle_t panel;
    uint16_t *framebuffer;
    uint16_t foreground;
    uint16_t background;
    u8g2_t u8g2;
    solar_os_board_display_t display;
} qemu_rgb_device_t;

static qemu_rgb_device_t device;
static EXT_RAM_BSS_ATTR uint8_t draw_buffer[QEMU_RGB_DRAW_BUFFER_SIZE];
static EXT_RAM_BSS_ATTR uint8_t draw_shadow[QEMU_RGB_DRAW_BUFFER_SIZE];

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .tile_width = QEMU_RGB_TILE_WIDTH,
    .tile_height = QEMU_RGB_TILE_HEIGHT,
    .pixel_width = QEMU_RGB_WIDTH,
    .pixel_height = QEMU_RGB_HEIGHT,
};

static uint16_t rgb565(uint32_t rgb888)
{
    return (uint16_t)((((rgb888 >> 19U) & 0x1FU) << 11U) | (((rgb888 >> 10U) & 0x3FU) << 5U) |
                      ((rgb888 >> 3U) & 0x1FU));
}

static esp_err_t present_draw_buffer(void)
{
    if (device.panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bool changed = false;
    for (size_t tile_y = 0; tile_y < QEMU_RGB_TILE_HEIGHT; tile_y++) {
        const uint8_t *row = draw_buffer + tile_y * QEMU_RGB_DRAW_STRIDE;
        const uint8_t *previous = draw_shadow + tile_y * QEMU_RGB_DRAW_STRIDE;
        for (size_t x = 0; x < QEMU_RGB_WIDTH; x++) {
            if (device.mono_valid && row[x] == previous[x])
                continue;
            changed = true;
            for (size_t bit = 0; bit < 8U && tile_y * 8U + bit < QEMU_RGB_HEIGHT; bit++) {
                device.framebuffer[(tile_y * 8U + bit) * QEMU_RGB_WIDTH + x] =
                    row[x] & (1U << bit) ? device.background : device.foreground;
            }
        }
    }
    if (!changed)
        return ESP_OK;
    esp_err_t err = esp_lcd_rgb_qemu_refresh(device.panel);
    if (err == ESP_OK) {
        memcpy(draw_shadow, draw_buffer, sizeof(draw_shadow));
        device.mono_valid = true;
    } else
        device.mono_valid = false;
    return err;
}

static uint8_t u8x8_display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    (void)arg_int;
    (void)arg_ptr;
    switch (message) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    case U8X8_MSG_DISPLAY_INIT:
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        return present_draw_buffer() == ESP_OK ? 1 : 0;
    default:
        return 0;
    }
}

static esp_err_t present_raster(const solar_os_display_raster_t *frame);

static esp_err_t present_mono_xbm(const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
                                  uint16_t width, uint16_t height, uint16_t stride,
                                  bool palette_inverted)
{
    const solar_os_display_raster_t frame = {
        .data = bitmap,
        .data_size = bitmap_size,
        .source_width = width,
        .source_height = height,
        .source_stride = stride,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .format = SOLAR_OS_DISPLAY_FORMAT_MONO1,
        .palette_inverted = palette_inverted,
        .clear_background = true,
    };
    return present_raster(&frame);
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    display->ready = true;
    return ESP_OK;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL) {
        display->ready = false;
    }
}

static esp_err_t set_colors(solar_os_board_display_t *display, uint32_t foreground,
                            uint32_t background)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (foreground > 0xffffffU || background > 0xffffffU)
        return ESP_ERR_INVALID_ARG;
    const uint16_t fg = device.mono ? 0U : rgb565(foreground);
    const uint16_t bg = device.mono ? 0xffffU : rgb565(background);
    if (device.foreground != fg || device.background != bg)
        device.mono_valid = false;
    device.foreground = fg;
    device.background = bg;
    return ESP_OK;
}

static esp_err_t display_present_mono(solar_os_board_display_t *display, const uint8_t *bitmap,
                                      size_t bitmap_size, uint16_t x, uint16_t y, uint16_t width,
                                      uint16_t height, uint16_t stride, bool inverted)
{
    return display != NULL
               ? present_mono_xbm(bitmap, bitmap_size, x, y, width, height, stride, inverted)
               : ESP_ERR_INVALID_STATE;
}

static esp_err_t present_raster(const solar_os_display_raster_t *frame)
{
    if (!device.panel)
        return ESP_ERR_INVALID_STATE;
    if (!frame || !frame->data || !frame->source_width || !frame->source_height || !frame->width ||
        !frame->height)
        return ESP_ERR_INVALID_ARG;
    size_t row_bytes, colors = 0;
    switch (frame->format) {
    case SOLAR_OS_DISPLAY_FORMAT_MONO1:
        row_bytes = (frame->source_width + 7U) / 8U;
        break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2:
        row_bytes = (frame->source_width + 3U) / 4U;
        colors = 4;
        break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX8:
        row_bytes = frame->source_width;
        colors = 256;
        break;
    case SOLAR_OS_DISPLAY_FORMAT_RGB565:
        row_bytes = (size_t)frame->source_width * 2U;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (device.mono && frame->format != SOLAR_OS_DISPLAY_FORMAT_MONO1)
        return ESP_ERR_NOT_SUPPORTED;
    if (frame->source_stride < row_bytes ||
        frame->data_size < (size_t)frame->source_stride * frame->source_height ||
        (colors && (!frame->palette_rgb565 || frame->palette_size < colors)) ||
        (frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 && frame->palette_inverted) ||
        (frame->clear_background && colors && frame->background_index >= colors) ||
        (frame->clear_background && frame->format == SOLAR_OS_DISPLAY_FORMAT_MONO1 &&
         frame->background_index > 1U))
        return ESP_ERR_INVALID_ARG;
    const u8g2_cb_t *rotation = device.u8g2.cb;
    const bool quarter_turn = rotation == U8G2_R1 || rotation == U8G2_R3;
    const uint16_t width = quarter_turn ? QEMU_RGB_HEIGHT : QEMU_RGB_WIDTH;
    const uint16_t height = quarter_turn ? QEMU_RGB_WIDTH : QEMU_RGB_HEIGHT;
    if ((uint32_t)frame->x + frame->width > width || (uint32_t)frame->y + frame->height > height)
        return ESP_ERR_INVALID_SIZE;
    uint16_t palette[256];
    if (colors)
        solar_os_indexed_palette(frame->palette_rgb565, frame->format, frame->palette_inverted,
                                 palette);
    else {
        palette[0] = frame->palette_inverted ? device.foreground : device.background;
        palette[1] = frame->palette_inverted ? device.background : device.foreground;
    }
    if (frame->clear_background) {
        uint16_t background = frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565
                                  ? frame->background_rgb565
                                  : palette[frame->background_index];
        for (size_t i = 0; i < (size_t)QEMU_RGB_WIDTH * QEMU_RGB_HEIGHT; i++)
            device.framebuffer[i] = background;
    }
    for (uint16_t y = 0; y < frame->height; y++) {
        const uint8_t *row =
            frame->data +
            (size_t)((uint32_t)y * frame->source_height / frame->height) * frame->source_stride;
        for (uint16_t x = 0; x < frame->width; x++) {
            const uint16_t sx = (uint32_t)x * frame->source_width / frame->width;
            uint16_t color;
            if (frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565)
                color = (uint16_t)((row[sx * 2U] << 8U) | row[sx * 2U + 1U]);
            else if (frame->format == SOLAR_OS_DISPLAY_FORMAT_MONO1)
                color = palette[(row[sx / 8U] >> (sx % 8U)) & 1U];
            else
                color = palette[solar_os_indexed_pixel(row, sx, frame->format)];
            uint16_t px = frame->x + x, py = frame->y + y, nx = px, ny = py;
            if (rotation == U8G2_R1) {
                nx = QEMU_RGB_WIDTH - 1U - py;
                ny = px;
            } else if (rotation == U8G2_R2) {
                nx = QEMU_RGB_WIDTH - 1U - px;
                ny = QEMU_RGB_HEIGHT - 1U - py;
            } else if (rotation == U8G2_R3) {
                nx = py;
                ny = QEMU_RGB_HEIGHT - 1U - px;
            }
            device.framebuffer[(size_t)ny * QEMU_RGB_WIDTH + nx] = color;
        }
    }
    device.mono_valid = false;
    return esp_lcd_rgb_qemu_refresh(device.panel);
}

static esp_err_t present_surface(const solar_os_display_surface_t *surface)
{
    if (!device.panel)
        return ESP_ERR_INVALID_STATE;
    if (device.mono)
        return ESP_ERR_NOT_SUPPORTED;
    if (!surface || !surface->data || surface->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 ||
        !surface->palette_rgb565 || surface->palette_size < 256U ||
        surface->rotation > SOLAR_OS_DISPLAY_ROTATION_270 ||
        surface->native_width != QEMU_RGB_WIDTH || surface->native_height != QEMU_RGB_HEIGHT)
        return ESP_ERR_INVALID_ARG;
    const bool rotated = surface->rotation == SOLAR_OS_DISPLAY_ROTATION_90 ||
                         surface->rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    if (surface->width != (rotated ? QEMU_RGB_HEIGHT : QEMU_RGB_WIDTH) ||
        surface->height != (rotated ? QEMU_RGB_WIDTH : QEMU_RGB_HEIGHT) ||
        surface->stride < surface->width ||
        surface->data_size < (size_t)surface->stride * surface->height)
        return ESP_ERR_INVALID_ARG;
    for (uint16_t ny = 0; ny < QEMU_RGB_HEIGHT; ny++)
        for (uint16_t nx = 0; nx < QEMU_RGB_WIDTH; nx++) {
            uint16_t x = nx, y = ny;
            if (surface->rotation == SOLAR_OS_DISPLAY_ROTATION_90) {
                x = ny;
                y = QEMU_RGB_WIDTH - 1U - nx;
            } else if (surface->rotation == SOLAR_OS_DISPLAY_ROTATION_180) {
                x = QEMU_RGB_WIDTH - 1U - nx;
                y = QEMU_RGB_HEIGHT - 1U - ny;
            } else if (surface->rotation == SOLAR_OS_DISPLAY_ROTATION_270) {
                x = QEMU_RGB_HEIGHT - 1U - ny;
                y = nx;
            }
            device.framebuffer[(size_t)ny * QEMU_RGB_WIDTH + nx] =
                surface->palette_rgb565[surface->data[(size_t)y * surface->stride + x]];
        }
    device.mono_valid = false;
    return esp_lcd_rgb_qemu_refresh(device.panel);
}

static esp_err_t display_present_surface(solar_os_board_display_t *display,
                                         const solar_os_display_surface_t *surface)
{
    return display ? present_surface(surface) : ESP_ERR_INVALID_ARG;
}

static esp_err_t target_present_surface(void *context, const solar_os_display_surface_t *surface)
{
    return context ? present_surface(surface) : ESP_ERR_INVALID_ARG;
}

static bool prefers_mono(const solar_os_board_display_t *display)
{
    (void)display;
    return device.mono;
}

static esp_err_t target_set_colors(void *context, uint32_t foreground, uint32_t background)
{
    return set_colors(context, foreground, background);
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    return display != NULL ? present_raster(frame) : ESP_ERR_INVALID_ARG;
}

static esp_err_t target_present_frame(void *context, const solar_os_display_raster_t *frame)
{
    return context != NULL ? present_raster(frame) : ESP_ERR_INVALID_ARG;
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .set_colors = set_colors,
    .present_mono_xbm = display_present_mono,
    .present_frame = present_frame,
    .present_surface = display_present_surface,
    .prefers_mono = prefers_mono,
};

static void release_panel(void)
{
    if (device.panel != NULL) {
        esp_lcd_panel_del(device.panel);
    }
    memset(&device, 0, sizeof(device));
}

static esp_err_t attach_display(const char *name, const solar_os_expansion_binding_t *bindings,
                                size_t binding_count, bool mono)
{
    (void)bindings;
    if (device.active || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (binding_count != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_lcd_rgb_qemu_config_t config = {
        .width = QEMU_RGB_WIDTH,
        .height = QEMU_RGB_HEIGHT,
        .bpp = RGB_QEMU_BPP_16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_qemu(&config, &device.panel), "qemu_rgb",
                        "QEMU RGB panel unavailable");
    void *framebuffer = NULL;
    esp_err_t ret = esp_lcd_rgb_qemu_get_frame_buffer(device.panel, &framebuffer);
    if (ret != ESP_OK || framebuffer == NULL) {
        release_panel();
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    device.framebuffer = framebuffer;
    device.mono = mono;
    device.foreground = rgb565(0x000000U);
    device.background = rgb565(0xFFFFFFU);
    strlcpy(device.name, name, sizeof(device.name));

    u8g2_SetupDisplay(&device.u8g2, u8x8_display_cb, u8x8_cad_empty, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&device.u8g2, draw_buffer, QEMU_RGB_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&device.u8g2);
    u8g2_ClearBuffer(&device.u8g2);

    device.display = (solar_os_board_display_t){
        .ops = &display_ops,
        .driver = &device,
        .driver_name = mono ? "qemu-mono" : "qemu-rgb",
        .u8g2 = &device.u8g2,
        .controller = mono ? "QEMU-MONO" : "QEMU-RGB",
        .width = u8g2_GetDisplayWidth(&device.u8g2),
        .height = u8g2_GetDisplayHeight(&device.u8g2),
        .frame_formats =
            SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT |
            (mono ? 0U
                  : SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT |
                        SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT),
        .surface_formats = mono ? 0U : SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
        .preferred_stream_fps = 30,
        .max_stream_pixels_per_second = 2400000U,
        .ready = true,
    };
    device.primary = solar_os_display_target_is_board_primary(name);
    if (device.primary) {
        ret = solar_os_board_display_register_primary(&device.display);
    } else {
        solar_os_display_target_t target = {0};
        strlcpy(target.name, name, sizeof(target.name));
        strlcpy(target.source, "expansion", sizeof(target.source));
        strlcpy(target.driver, device.display.driver_name, sizeof(target.driver));
        strlcpy(target.controller, device.display.controller, sizeof(target.controller));
        strlcpy(target.role, "aux", sizeof(target.role));
        target.width = device.display.width;
        target.height = device.display.height;
        target.ready = true;
        target.frame_formats = device.display.frame_formats;
        target.surface_formats = device.display.surface_formats;
        target.prefer_mono = mono;
        target.preferred_stream_fps = 30;
        target.max_stream_pixels_per_second = 2400000U;
        target.u8g2 = &device.u8g2;
        target.frame_context = &device;
        target.present_frame = target_present_frame;
        if (!mono) {
            target.surface_context = &device;
            target.present_surface = target_present_surface;
        }
        target.set_colors = target_set_colors;
        target.color_context = &device.display;
        ret = solar_os_display_register_target(&target);
    }
    if (ret != ESP_OK) {
        release_panel();
        return ret;
    }
    device.active = true;
    return ESP_OK;
}

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings,
                        size_t count)
{
    return attach_display(name, bindings, count, false);
}

static esp_err_t attach_mono(const char *name, const solar_os_expansion_binding_t *bindings,
                             size_t count)
{
    return attach_display(name, bindings, count, true);
}

static esp_err_t detach(const char *name)
{
    if (!device.active || name == NULL || strcmp(name, device.name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device.primary
                              ? solar_os_board_display_unregister_primary(&device.display)
                              : solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    release_panel();
    return ESP_OK;
}

const solar_os_expansion_driver_t solar_os_qemu_rgb_expansion_driver = {
    .name = "qemu-rgb",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "QEMU virtual RGB panel",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX,
    .early = true,
    .attach = attach,
    .detach = detach,
};

const solar_os_expansion_driver_t solar_os_qemu_mono_expansion_driver = {
    .name = "qemu-mono",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "QEMU virtual monochrome panel",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX,
    .early = true,
    .attach = attach_mono,
    .detach = detach,
};
