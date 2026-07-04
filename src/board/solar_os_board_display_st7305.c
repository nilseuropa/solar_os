#include "solar_os_board_display.h"

#include <string.h>

#include "rlcd_st7305.h"
#include "solar_os_board.h"

static rlcd_st7305_t st7305_display;

static void display_bind_st7305(solar_os_board_display_t *display)
{
    display->driver = &st7305_display;
    display->driver_name = "st7305";
    display->u8g2 = rlcd_st7305_get_u8g2(&st7305_display);
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
    const esp_err_t err = rlcd_st7305_init(&st7305_display);
    if (err != ESP_OK) {
        return err;
    }

    display_bind_st7305(display);
    return ESP_OK;
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = rlcd_st7305_resume((rlcd_st7305_t *)display->driver);
    if (err != ESP_OK) {
        display->ready = false;
        return err;
    }

    display_bind_st7305(display);
    return ESP_OK;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        rlcd_st7305_deinit((rlcd_st7305_t *)display->driver);
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

const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return NULL;
    }
    return rlcd_st7305_controller_mode((const rlcd_st7305_t *)display->driver);
}

const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display)
{
    (void)display;
    return rlcd_st7305_controller_mode_values();
}

esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return rlcd_st7305_set_controller_mode((rlcd_st7305_t *)display->driver, mode);
}
