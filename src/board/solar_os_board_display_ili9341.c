#include "solar_os_board_display.h"

#include <string.h>

#include "solar_os_board.h"
#include "tft_ili9341.h"

#if defined(SOLAR_OS_BOARD_M5STACK_CORE2)
#include "pmic_axp192.h"
#elif defined(SOLAR_OS_BOARD_M5STACK_CORES3)
#include "io_expander_aw9523b.h"
#include "pmic_axp2101.h"
#endif

static tft_ili9341_t ili9341_display;

#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
static uint8_t cores3_backlight_percent = 100;
#endif

static void display_bind_ili9341(solar_os_board_display_t *display)
{
    display->driver = &ili9341_display;
    display->driver_name = "ili9341";
    display->u8g2 = tft_ili9341_get_u8g2(&ili9341_display);
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

#if defined(SOLAR_OS_BOARD_M5STACK_CORE2)
    /* Core2's LCD backlight and reset lines live behind the AXP192 PMIC,
     * not ESP32 GPIOs, so they must be powered up before the panel init
     * sequence talks to it over SPI. */
    const esp_err_t pmic_err = pmic_axp192_core2_bringup();
    if (pmic_err != ESP_OK) {
        return pmic_err;
    }
#elif defined(SOLAR_OS_BOARD_M5STACK_CORES3)
    /*
     * CoreS3's LCD digital VDD (BLDO1) and backlight boost (DLDO1) live
     * behind the AXP2101 PMIC, and LCD reset lives behind the AW9523B
     * IO expander -- both on the internal I2C bus, neither on a raw
     * ESP32 GPIO. Power must come up before reset is released (the
     * expander bring-up needs BLDO1 already up), and both must run
     * before the panel init sequence talks to it over SPI.
     */
    const esp_err_t pmic_err = pmic_axp2101_cores3_bringup();
    if (pmic_err != ESP_OK) {
        return pmic_err;
    }
    const esp_err_t expander_err = io_expander_aw9523b_init();
    if (expander_err != ESP_OK) {
        return expander_err;
    }
#endif

    const esp_err_t err = tft_ili9341_init(&ili9341_display);
    if (err != ESP_OK) {
        return err;
    }

#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
    /*
     * tft_ili9341's own backlight path is a no-op here (PIN_LCD_BL is
     * undefined -- the backlight boost converter is on the PMIC, not a
     * plain ESP32 GPIO), so drive it directly instead.
     */
    (void)pmic_axp2101_set_backlight(cores3_backlight_percent);
#endif

    display_bind_ili9341(display);
    return ESP_OK;
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = tft_ili9341_resume((tft_ili9341_t *)display->driver);
    if (err != ESP_OK) {
        display->ready = false;
        return err;
    }

    display_bind_ili9341(display);
    return ESP_OK;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        tft_ili9341_deinit((tft_ili9341_t *)display->driver);
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
#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
    return true;
#else
    return tft_ili9341_backlight_supported();
#endif
}

esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *percent = cores3_backlight_percent;
    return ESP_OK;
#else
    return tft_ili9341_get_backlight((const tft_ili9341_t *)display->driver, percent);
#endif
}

esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(SOLAR_OS_BOARD_M5STACK_CORES3)
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = pmic_axp2101_set_backlight(percent);
    if (err == ESP_OK) {
        cores3_backlight_percent = percent;
    }
    return err;
#else
    return tft_ili9341_set_backlight((tft_ili9341_t *)display->driver, percent);
#endif
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
