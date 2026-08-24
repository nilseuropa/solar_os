#include "solar_os_board_display.h"

#include <string.h>

#include "solar_os_board.h"
#include "vga32.h"

static vga32_t vga_display;

static void display_bind_vga32(solar_os_board_display_t *display)
{
    display->driver = &vga_display;
    display->driver_name = "vga32";
    display->u8g2 = vga32_get_u8g2(&vga_display);
    display->controller = SOLAR_OS_BOARD_DISPLAY_CONTROLLER;
    display->width = SOLAR_OS_BOARD_DISPLAY_WIDTH;
    display->height = SOLAR_OS_BOARD_DISPLAY_HEIGHT;
    display->ready = true;
}

esp_err_t solar_os_board_display_init(solar_os_board_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(display, 0, sizeof(*display));
    const esp_err_t err = vga32_init(&vga_display);
    if (err != ESP_OK) {
        return err;
    }
    display_bind_vga32(display);
    return ESP_OK;
}

esp_err_t solar_os_board_display_runtime_ready(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return vga32_start_async_present((vga32_t *)display->driver);
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = vga32_resume((vga32_t *)display->driver);
    if (err != ESP_OK) {
        display->ready = false;
        return err;
    }
    display_bind_vga32(display);
    return ESP_OK;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        vga32_deinit((vga32_t *)display->driver);
        memset(display, 0, sizeof(*display));
    }
}

u8g2_t *solar_os_board_display_u8g2(solar_os_board_display_t *display)
{
    return display != NULL ? display->u8g2 : NULL;
}

const char *solar_os_board_display_driver_name(const solar_os_board_display_t *display)
{
    return display != NULL && display->driver_name != NULL
               ? display->driver_name
               : "unknown";
}

const char *solar_os_board_display_controller(const solar_os_board_display_t *display)
{
    return display != NULL && display->controller != NULL
               ? display->controller
               : "unknown";
}

uint16_t solar_os_board_display_width(const solar_os_board_display_t *display)
{
    return display != NULL ? display->width : 0;
}

uint16_t solar_os_board_display_height(const solar_os_board_display_t *display)
{
    return display != NULL ? display->height : 0;
}

bool solar_os_board_display_ready(const solar_os_board_display_t *display)
{
    return display != NULL && display->ready;
}

bool solar_os_board_display_brightness_supported(const solar_os_board_display_t *display)
{
    (void)display;
    return false;
}

esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent)
{
    (void)display;
    if (percent != NULL) {
        *percent = 100;
    }
    return percent != NULL ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent)
{
    (void)display;
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_colors(solar_os_board_display_t *display,
                                            uint32_t foreground_rgb888,
                                            uint32_t background_rgb888)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return vga32_set_colors((vga32_t *)display->driver,
                            foreground_rgb888,
                            background_rgb888);
}

const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *display)
{
    (void)display;
    return NULL;
}

const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display)
{
    (void)display;
    return NULL;
}

esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode)
{
    (void)display;
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_high_refresh_override(
    solar_os_board_display_t *display,
    bool enabled,
    uint16_t hz_tenths)
{
    (void)display;
    (void)enabled;
    (void)hz_tenths;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_present_mono_xbm(solar_os_board_display_t *display,
                                                  const uint8_t *bitmap,
                                                  size_t bitmap_size,
                                                  uint16_t x,
                                                  uint16_t y,
                                                  uint16_t width,
                                                  uint16_t height,
                                                  uint16_t stride,
                                                  bool palette_inverted)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return vga32_present_mono_xbm((vga32_t *)display->driver,
                                  bitmap,
                                  bitmap_size,
                                  x,
                                  y,
                                  width,
                                  height,
                                  stride,
                                  palette_inverted);
}
