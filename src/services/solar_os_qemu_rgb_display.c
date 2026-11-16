#include "solar_os_qemu_rgb_display.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"
#include "u8g2.h"

/* Espressif QEMU's virtual RGB panel (esp32s3 machine, shown with -display sdl).
 * The panel only exists in QEMU; esp_lcd_new_rgb_qemu() refuses real hardware.
 * Each refresh waits until QEMU's display loop copies the frame, so a run
 * without a display backend (-nographic) blocks at the first refresh. */

#define QEMU_RGB_WIDTH 800U  /* the largest panel QEMU's esp_rgb takes */
#define QEMU_RGB_HEIGHT 600U
#define QEMU_RGB_TILE_WIDTH ((QEMU_RGB_WIDTH + 7U) / 8U)
#define QEMU_RGB_TILE_HEIGHT ((QEMU_RGB_HEIGHT + 7U) / 8U)
#define QEMU_RGB_DRAW_BUFFER_SIZE (QEMU_RGB_WIDTH * QEMU_RGB_TILE_HEIGHT)

typedef struct {
    bool active;
    bool primary;
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
    return (uint16_t)((((rgb888 >> 19U) & 0x1FU) << 11U) |
                      (((rgb888 >> 10U) & 0x3FU) << 5U) |
                      ((rgb888 >> 3U) & 0x1FU));
}

/* shortcut: no lock between the u8g2 and the frame path; both redraw the
 * whole frame, so a race shows one mixed frame. Add a mutex if that is seen. */
/* shortcut: every refresh converts all 480,000 pixels to RGB565 on the
 * emulated CPU, however little changed. Convert only the changed tile rows if
 * refresh speed matters. */
static esp_err_t present_draw_buffer(void)
{
    if (device.panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Same pixel meaning as vga32: a set u8g2 bit shows the background colour. */
    for (size_t y = 0; y < QEMU_RGB_HEIGHT; y++) {
        const uint8_t *row = &draw_buffer[(y / 8U) * QEMU_RGB_WIDTH];
        const uint8_t mask = (uint8_t)(1U << (y % 8U));
        uint16_t *out = &device.framebuffer[y * QEMU_RGB_WIDTH];
        for (size_t x = 0; x < QEMU_RGB_WIDTH; x++) {
            out[x] = (row[x] & mask) != 0U ? device.background : device.foreground;
        }
    }
    return esp_lcd_rgb_qemu_refresh(device.panel);
}

static uint8_t u8x8_display_cb(u8x8_t *u8x8,
                               uint8_t message,
                               uint8_t arg_int,
                               void *arg_ptr)
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

static esp_err_t present_mono_xbm(const uint8_t *bitmap,
                                  size_t bitmap_size,
                                  uint16_t x,
                                  uint16_t y,
                                  uint16_t width,
                                  uint16_t height,
                                  uint16_t stride,
                                  bool palette_inverted)
{
    const size_t source_bytes = ((size_t)width + 7U) / 8U;
    if (device.panel == NULL || bitmap == NULL || width == 0U || height == 0U ||
        stride < source_bytes || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > QEMU_RGB_WIDTH ||
        (uint32_t)y + height > QEMU_RGB_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Same rules as vga32: the rest of the screen is cleared, a set bit is ink. */
    const uint16_t clear = palette_inverted ? device.foreground : device.background;
    for (size_t i = 0; i < (size_t)QEMU_RGB_WIDTH * QEMU_RGB_HEIGHT; i++) {
        device.framebuffer[i] = clear;
    }
    for (size_t source_y = 0; source_y < height; source_y++) {
        const uint8_t *source = bitmap + source_y * stride;
        uint16_t *out = &device.framebuffer[((size_t)y + source_y) * QEMU_RGB_WIDTH + x];
        for (size_t source_x = 0; source_x < width; source_x++) {
            const bool black = (source[source_x / 8U] & (1U << (source_x % 8U))) != 0U;
            out[source_x] = black == palette_inverted ? device.background : device.foreground;
        }
    }
    return esp_lcd_rgb_qemu_refresh(device.panel);
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

static esp_err_t set_colors(solar_os_board_display_t *display,
                            uint32_t foreground,
                            uint32_t background)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    device.foreground = rgb565(foreground);
    device.background = rgb565(background);
    return ESP_OK;
}

static esp_err_t display_present_mono(solar_os_board_display_t *display,
                                      const uint8_t *bitmap,
                                      size_t bitmap_size,
                                      uint16_t x,
                                      uint16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t stride,
                                      bool inverted)
{
    return display != NULL ?
        present_mono_xbm(bitmap, bitmap_size, x, y, width, height, stride, inverted) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_raster(const solar_os_display_raster_t *frame)
{
    if (frame == NULL ||
        frame->format != SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
        frame->source_width != frame->width ||
        frame->source_height != frame->height) {
        return ESP_ERR_INVALID_ARG;
    }
    return present_mono_xbm(frame->data, frame->data_size,
                            frame->x, frame->y, frame->width, frame->height,
                            frame->source_stride, frame->palette_inverted);
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    return display != NULL ? present_raster(frame) : ESP_ERR_INVALID_ARG;
}

static esp_err_t target_present_frame(void *context,
                                      const solar_os_display_raster_t *frame)
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
};

static void release_panel(void)
{
    if (device.panel != NULL) {
        esp_lcd_panel_del(device.panel);
    }
    memset(&device, 0, sizeof(device));
}

static esp_err_t attach(const char *name,
                        const solar_os_expansion_binding_t *bindings,
                        size_t binding_count)
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
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_qemu(&config, &device.panel),
                        "qemu_rgb", "QEMU RGB panel unavailable");
    void *framebuffer = NULL;
    esp_err_t ret = esp_lcd_rgb_qemu_get_frame_buffer(device.panel, &framebuffer);
    if (ret != ESP_OK || framebuffer == NULL) {
        release_panel();
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    device.framebuffer = framebuffer;
    device.foreground = rgb565(0x000000U);
    device.background = rgb565(0xFFFFFFU);
    strlcpy(device.name, name, sizeof(device.name));

    u8g2_SetupDisplay(&device.u8g2, u8x8_display_cb, u8x8_cad_empty,
                      u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&device.u8g2, draw_buffer, QEMU_RGB_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&device.u8g2);
    u8g2_ClearBuffer(&device.u8g2);

    device.display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device,
        .driver_name = "qemu-rgb",
        .u8g2 = &device.u8g2,
        .controller = "QEMU-RGB",
        .width = u8g2_GetDisplayWidth(&device.u8g2),
        .height = u8g2_GetDisplayHeight(&device.u8g2),
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT,
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
        strlcpy(target.driver, "qemu-rgb", sizeof(target.driver));
        strlcpy(target.controller, "QEMU-RGB", sizeof(target.controller));
        strlcpy(target.role, "aux", sizeof(target.role));
        target.width = device.display.width;
        target.height = device.display.height;
        target.ready = true;
        target.frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
        target.preferred_stream_fps = 30;
        target.max_stream_pixels_per_second = 2400000U;
        target.u8g2 = &device.u8g2;
        target.frame_context = &device;
        target.present_frame = target_present_frame;
        ret = solar_os_display_register_target(&target);
    }
    if (ret != ESP_OK) {
        release_panel();
        return ret;
    }
    device.active = true;
    return ESP_OK;
}

static esp_err_t detach(const char *name)
{
    if (!device.active || name == NULL || strcmp(name, device.name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device.primary ?
        solar_os_board_display_unregister_primary(&device.display) :
        solar_os_display_unregister_target(name);
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
