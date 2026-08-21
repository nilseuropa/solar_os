#include "solar_os_board_display.h"

#include <string.h>

#include "epd_ssd1683.h"
#include "solar_os_board.h"

static epd_ssd1683_t ssd1683_display;

static void display_bind_ssd1683(solar_os_board_display_t *display)
{
    display->driver = &ssd1683_display;
    display->driver_name = "ssd1683";
    display->u8g2 = epd_ssd1683_get_u8g2(&ssd1683_display);
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
    const epd_ssd1683_config_t config = {
        .spi_host = SOLAR_OS_BOARD_DISPLAY_SPI_HOST,
        .sclk_pin = SOLAR_OS_BOARD_PIN_LCD_SCK,
        .mosi_pin = SOLAR_OS_BOARD_PIN_LCD_MOSI,
        .cs_pin = SOLAR_OS_BOARD_PIN_LCD_CS,
        .dc_pin = SOLAR_OS_BOARD_PIN_LCD_DC,
        .reset_pin = SOLAR_OS_BOARD_PIN_LCD_RST,
        .busy_pin = SOLAR_OS_BOARD_PIN_LCD_BUSY,
        .power_pin = SOLAR_OS_BOARD_PIN_LCD_POWER,
        .spi_clock_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
        .busy_level = SOLAR_OS_BOARD_LCD_BUSY_LEVEL,
        .power_active_level = SOLAR_OS_BOARD_LCD_POWER_ACTIVE_LEVEL,
        .rotation = SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION,
        .panel_variant = EPD_SSD1683_PANEL_UNKNOWN,
    };
    const esp_err_t err = epd_ssd1683_init(&ssd1683_display, &config);
    if (err != ESP_OK) {
        return err;
    }
    display_bind_ssd1683(display);
    return ESP_OK;
}

esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = epd_ssd1683_resume((epd_ssd1683_t *)display->driver);
    if (err != ESP_OK) {
        display->ready = false;
        return err;
    }
    display_bind_ssd1683(display);
    return ESP_OK;
}

void solar_os_board_display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        epd_ssd1683_deinit((epd_ssd1683_t *)display->driver);
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
    return epd_ssd1683_controller_mode((const epd_ssd1683_t *)display->driver);
}

const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display)
{
    if (display == NULL || display->driver == NULL) {
        return NULL;
    }
    return epd_ssd1683_controller_mode_values((const epd_ssd1683_t *)display->driver);
}

esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode)
{
    if (display == NULL || display->driver == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return epd_ssd1683_set_controller_mode((epd_ssd1683_t *)display->driver, mode);
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
    (void)display;
    (void)bitmap;
    (void)bitmap_size;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)stride;
    (void)palette_inverted;
    return ESP_ERR_NOT_SUPPORTED;
}
