#include "solar_os_board_display.h"

#include <string.h>

#include "lcd_rgb_panel.h"
#include "solar_os_board.h"

static lcd_rgb_panel_t rgb_display;

static void display_bind_rgb_panel(solar_os_board_display_t *display)
{
    display->driver = &rgb_display;
    display->driver_name = "rgb_panel";
    display->u8g2 = lcd_rgb_panel_get_u8g2(&rgb_display);
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

    const esp_err_t err = lcd_rgb_panel_init(&rgb_display);
    if (err != ESP_OK) {
        return err;
    }

    display_bind_rgb_panel(display);
    return ESP_OK;
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = lcd_rgb_panel_resume((lcd_rgb_panel_t *)display->driver);
    if (err != ESP_OK) {
        display->ready = false;
        return err;
    }

    display_bind_rgb_panel(display);
    return ESP_OK;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        lcd_rgb_panel_deinit((lcd_rgb_panel_t *)display->driver);
        memset(display, 0, sizeof(*display));
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
    (void)display;
    return lcd_rgb_panel_backlight_supported();
}

esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return lcd_rgb_panel_get_backlight((const lcd_rgb_panel_t *)display->driver, percent);
}

esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return lcd_rgb_panel_set_backlight((lcd_rgb_panel_t *)display->driver, percent);
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
