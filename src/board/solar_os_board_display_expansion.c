#include "solar_os_board_display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static solar_os_board_display_t *primary_display;
static portMUX_TYPE primary_display_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t solar_os_board_display_register_primary(solar_os_board_display_t *display)
{
    if (display == NULL || display->ops == NULL || display->driver == NULL ||
        display->u8g2 == NULL || display->driver_name == NULL ||
        display->controller == NULL || display->width == 0 || display->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&primary_display_lock);
    if (primary_display != NULL) {
        portEXIT_CRITICAL(&primary_display_lock);
        return ESP_ERR_INVALID_STATE;
    }
    primary_display = display;
    portEXIT_CRITICAL(&primary_display_lock);
    return ESP_OK;
}

esp_err_t solar_os_board_display_unregister_primary(solar_os_board_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&primary_display_lock);
    if (primary_display != display) {
        portEXIT_CRITICAL(&primary_display_lock);
        return ESP_ERR_NOT_FOUND;
    }
    primary_display = NULL;
    portEXIT_CRITICAL(&primary_display_lock);
    return ESP_OK;
}

esp_err_t solar_os_board_display_init(solar_os_board_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&primary_display_lock);
    solar_os_board_display_t *registered = primary_display;
    if (registered != NULL) {
        *display = *registered;
    }
    portEXIT_CRITICAL(&primary_display_lock);
    return registered != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_board_display_runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL && display->ops->runtime_ready != NULL ?
        display->ops->runtime_ready(display) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL && display->ops->resume != NULL ?
        display->ops->resume(display) : ESP_ERR_INVALID_STATE;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->ops != NULL && display->ops->deinit != NULL) {
        display->ops->deinit(display);
    }
}

u8g2_t *solar_os_board_display_u8g2(solar_os_board_display_t *display)
{
    return display != NULL ? display->u8g2 : NULL;
}

const char *solar_os_board_display_driver_name(const solar_os_board_display_t *display)
{
    return display != NULL && display->driver_name != NULL ? display->driver_name : "unknown";
}

const char *solar_os_board_display_controller(const solar_os_board_display_t *display)
{
    return display != NULL && display->controller != NULL ? display->controller : "unknown";
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
    return display != NULL && display->ops != NULL &&
        display->ops->brightness_supported != NULL &&
        display->ops->brightness_supported(display);
}

esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return display != NULL && display->ops != NULL && display->ops->get_brightness != NULL ?
        display->ops->get_brightness(display, percent) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent)
{
    return display != NULL && display->ops != NULL && display->ops->set_brightness != NULL ?
        display->ops->set_brightness(display, percent) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_colors(solar_os_board_display_t *display,
                                            uint32_t foreground_rgb888,
                                            uint32_t background_rgb888)
{
    return display != NULL && display->ops != NULL && display->ops->set_colors != NULL ?
        display->ops->set_colors(display, foreground_rgb888, background_rgb888) : ESP_OK;
}

const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL && display->ops->controller_mode != NULL ?
        display->ops->controller_mode(display) : NULL;
}

const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display)
{
    return display != NULL && display->ops != NULL &&
        display->ops->controller_mode_values != NULL ?
        display->ops->controller_mode_values(display) : NULL;
}

esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode)
{
    return display != NULL && display->ops != NULL &&
        display->ops->set_controller_mode != NULL ?
        display->ops->set_controller_mode(display, mode) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_board_display_set_high_refresh_override(
    solar_os_board_display_t *display,
    bool enabled,
    uint16_t hz_tenths)
{
    return display != NULL && display->ops != NULL &&
        display->ops->set_high_refresh_override != NULL ?
        display->ops->set_high_refresh_override(display, enabled, hz_tenths) :
        ESP_ERR_NOT_SUPPORTED;
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
    return display != NULL && display->ops != NULL && display->ops->present_mono_xbm != NULL ?
        display->ops->present_mono_xbm(display,
                                       bitmap,
                                       bitmap_size,
                                       x,
                                       y,
                                       width,
                                       height,
                                       stride,
                                       palette_inverted) : ESP_ERR_NOT_SUPPORTED;
}
