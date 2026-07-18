#include "tft_ili9341.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_board.h"
#include "solar_os_vector.h"
#include "spi_bus.h"

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ
#error "SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ must be defined by the board profile"
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH SOLAR_OS_BOARD_DISPLAY_WIDTH
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT SOLAR_OS_BOARD_DISPLAY_HEIGHT
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_MADCTL
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0x28
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R0
#endif

#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_ACTIVE_LEVEL 1
#endif
#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM 0
#endif
#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ 20000U
#endif

#if SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM
#include "pwm_port.h"
#endif

#define ILI9341_SPI_CLOCK_HZ SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ
#define ILI9341_WIDTH SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH
#define ILI9341_HEIGHT SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT
#define ILI9341_TILE_WIDTH ((ILI9341_WIDTH + 7) / 8)
#define ILI9341_TILE_HEIGHT ((ILI9341_HEIGHT + 7) / 8)
#define ILI9341_BUFFER_ROW_BYTES (ILI9341_TILE_WIDTH * 8)
#define ILI9341_BUFFER_BYTES (ILI9341_BUFFER_ROW_BYTES * ILI9341_TILE_HEIGHT)
#define ILI9341_LINE_BYTES (ILI9341_WIDTH * 2)
#define ILI9341_RGB565_BLACK 0x0000
#define ILI9341_RGB565_WHITE 0xffff

static const char *TAG = "tft_ili9341";
static tft_ili9341_t *active_display;

static const u8x8_display_info_t ili9341_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 20,
    .post_reset_wait_ms = 120,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = ILI9341_SPI_CLOCK_HZ,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = ILI9341_TILE_WIDTH,
    .tile_height = ILI9341_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = ILI9341_WIDTH,
    .pixel_height = ILI9341_HEIGHT,
};

static bool gpio_valid(gpio_num_t pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

static uint64_t gpio_pin_mask(gpio_num_t pin)
{
    return gpio_valid(pin) ? (1ULL << (unsigned)pin) : 0ULL;
}

static esp_err_t ili9341_tx_byte(tft_ili9341_t *display, uint8_t value)
{
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = {value},
    };

    return spi_device_polling_transmit(display->spi, &transaction);
}

static esp_err_t ili9341_tx_bytes(tft_ili9341_t *display, const uint8_t *data, size_t length)
{
    while (length > 0) {
        const size_t chunk = length > display->line_buffer_size ?
            display->line_buffer_size :
            length;
        if (data != display->line_buffer) {
            memcpy(display->line_buffer, data, chunk);
        }

        spi_transaction_t transaction = {
            .length = chunk * 8U,
            .tx_buffer = display->line_buffer,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display->spi, &transaction),
                            TAG,
                            "spi transmit failed");
        data += chunk;
        length -= chunk;
    }

    return ESP_OK;
}

static esp_err_t ili9341_cmd_data(tft_ili9341_t *display,
                                  uint8_t command,
                                  const uint8_t *data,
                                  size_t length)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 0), TAG, "dc command failed");
    ESP_RETURN_ON_ERROR(ili9341_tx_byte(display, command), TAG, "command transmit failed");
    if (length == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "dc data failed");
    return ili9341_tx_bytes(display, data, length);
}

static esp_err_t ili9341_cmd(tft_ili9341_t *display, uint8_t command)
{
    return ili9341_cmd_data(display, command, NULL, 0);
}

static bool ili9341_checked_cmd_data(tft_ili9341_t *display,
                                     uint8_t command,
                                     const uint8_t *data,
                                     size_t length)
{
    const esp_err_t err = ili9341_cmd_data(display, command, data, length);
    if (err != ESP_OK) {
        display->last_error = err;
        ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool ili9341_checked_cmd(tft_ili9341_t *display, uint8_t command)
{
    const esp_err_t err = ili9341_cmd(display, command);
    if (err != ESP_OK) {
        display->last_error = err;
        ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool ili9341_backlight_supported(void)
{
#ifdef SOLAR_OS_BOARD_PIN_LCD_BL
    return gpio_valid(SOLAR_OS_BOARD_PIN_LCD_BL);
#else
    return false;
#endif
}

static uint8_t ili9341_backlight_duty(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return SOLAR_OS_BOARD_LCD_BACKLIGHT_ACTIVE_LEVEL ? percent : (uint8_t)(100U - percent);
}

static esp_err_t ili9341_apply_backlight(uint8_t percent)
{
    if (!ili9341_backlight_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

#if SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM
    return pwm_port_set(SOLAR_OS_BOARD_PIN_LCD_BL,
                        SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ,
                        ili9341_backlight_duty(percent));
#else
    const int active = SOLAR_OS_BOARD_LCD_BACKLIGHT_ACTIVE_LEVEL ? 1 : 0;
    return gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_BL, percent > 0 ? active : !active);
#endif
}

static void ili9341_set_backlight_power(tft_ili9341_t *display, bool on)
{
    if (display == NULL) {
        return;
    }

    display->backlight_power = on;
    const uint8_t percent = on ? display->backlight_percent : 0;
    const esp_err_t err = ili9341_apply_backlight(percent);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        display->last_error = err;
        ESP_LOGW(TAG, "backlight set failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t ili9341_configure_control_pins(void)
{
    uint64_t pin_mask = 0;
    pin_mask |= gpio_pin_mask(SOLAR_OS_BOARD_PIN_LCD_DC);
#ifdef SOLAR_OS_BOARD_PIN_LCD_RST
    pin_mask |= gpio_pin_mask(SOLAR_OS_BOARD_PIN_LCD_RST);
#endif
#ifdef SOLAR_OS_BOARD_PIN_LCD_BL
    pin_mask |= gpio_pin_mask(SOLAR_OS_BOARD_PIN_LCD_BL);
#endif

    if (pin_mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t io_config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "gpio config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "dc high failed");
#ifdef SOLAR_OS_BOARD_PIN_LCD_RST
    if (gpio_valid(SOLAR_OS_BOARD_PIN_LCD_RST)) {
        ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1), TAG, "rst high failed");
    }
#endif
    return ESP_OK;
}

static void ili9341_hardware_reset(void)
{
#ifdef SOLAR_OS_BOARD_PIN_LCD_RST
    if (!gpio_valid(SOLAR_OS_BOARD_PIN_LCD_RST)) {
        return;
    }

    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif
}

static esp_err_t ili9341_set_window(tft_ili9341_t *display,
                                    uint16_t x0,
                                    uint16_t y0,
                                    uint16_t x1,
                                    uint16_t y1)
{
    const uint8_t col[] = {
        (uint8_t)(x0 >> 8),
        (uint8_t)(x0 & 0xff),
        (uint8_t)(x1 >> 8),
        (uint8_t)(x1 & 0xff),
    };
    const uint8_t row[] = {
        (uint8_t)(y0 >> 8),
        (uint8_t)(y0 & 0xff),
        (uint8_t)(y1 >> 8),
        (uint8_t)(y1 & 0xff),
    };

    ESP_RETURN_ON_ERROR(ili9341_cmd_data(display, 0x2a, col, sizeof(col)), TAG, "set col failed");
    return ili9341_cmd_data(display, 0x2b, row, sizeof(row));
}

static void ili9341_fill_line(tft_ili9341_t *display, uint16_t rgb565, size_t pixels)
{
    solar_os_vector_fill_rgb565_be(display->line_buffer, rgb565, pixels);
}

static esp_err_t ili9341_fill_screen(tft_ili9341_t *display, uint16_t rgb565)
{
    ESP_RETURN_ON_ERROR(ili9341_set_window(display, 0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1),
                        TAG,
                        "window failed");
    ESP_RETURN_ON_ERROR(ili9341_cmd(display, 0x2c), TAG, "ram write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "dc data failed");

    ili9341_fill_line(display, rgb565, ILI9341_WIDTH);
    for (uint16_t row = 0; row < ILI9341_HEIGHT; row++) {
        ESP_RETURN_ON_ERROR(ili9341_tx_bytes(display,
                                             display->line_buffer,
                                             ILI9341_WIDTH * 2U),
                            TAG,
                            "fill transmit failed");
    }

    return ESP_OK;
}

static void ili9341_invalidate_shadow(tft_ili9341_t *display)
{
    if (display != NULL) {
        display->shadow_valid_rows = 0;
    }
}

static bool ili9341_shadow_matches(tft_ili9341_t *display,
                                   const uint8_t *tile_data,
                                   uint8_t x_pos,
                                   uint8_t y_pos,
                                   uint8_t count)
{
    if (display == NULL ||
        display->shadow == NULL ||
        display->shadow_size != ILI9341_BUFFER_BYTES ||
        tile_data == NULL ||
        y_pos >= ILI9341_TILE_HEIGHT ||
        x_pos >= ILI9341_TILE_WIDTH ||
        count == 0 ||
        x_pos + count > ILI9341_TILE_WIDTH ||
        (display->shadow_valid_rows & (1ULL << y_pos)) == 0) {
        return false;
    }

    const size_t offset = ((size_t)y_pos * ILI9341_BUFFER_ROW_BYTES) + ((size_t)x_pos * 8U);
    return memcmp(&display->shadow[offset], tile_data, (size_t)count * 8U) == 0;
}

static void ili9341_shadow_update(tft_ili9341_t *display,
                                  const uint8_t *tile_data,
                                  uint8_t x_pos,
                                  uint8_t y_pos,
                                  uint8_t count)
{
    if (display == NULL ||
        display->shadow == NULL ||
        display->shadow_size != ILI9341_BUFFER_BYTES ||
        tile_data == NULL ||
        y_pos >= ILI9341_TILE_HEIGHT ||
        x_pos >= ILI9341_TILE_WIDTH ||
        count == 0 ||
        x_pos + count > ILI9341_TILE_WIDTH) {
        return;
    }

    const size_t offset = ((size_t)y_pos * ILI9341_BUFFER_ROW_BYTES) + ((size_t)x_pos * 8U);
    memcpy(&display->shadow[offset], tile_data, (size_t)count * 8U);
    display->shadow_valid_rows |= (1ULL << y_pos);
}

static void ili9341_line_from_tile(tft_ili9341_t *display,
                                   const uint8_t *tile_data,
                                   int row,
                                   int width)
{
    solar_os_vector_expand_1bpp_to_rgb565_be(display->line_buffer,
                                             tile_data,
                                             (unsigned)row,
                                             ILI9341_RGB565_BLACK,
                                             ILI9341_RGB565_WHITE,
                                             (size_t)width);
}

static esp_err_t ili9341_draw_tile(tft_ili9341_t *display, const u8x8_tile_t *tile)
{
    if (display == NULL || tile == NULL || tile->tile_ptr == NULL || tile->cnt == 0) {
        return ESP_OK;
    }
    if (tile->x_pos >= ILI9341_TILE_WIDTH || tile->y_pos >= ILI9341_TILE_HEIGHT) {
        return ESP_OK;
    }

    uint8_t count = tile->cnt;
    if (tile->x_pos + count > ILI9341_TILE_WIDTH) {
        count = ILI9341_TILE_WIDTH - tile->x_pos;
    }
    if (count == 0) {
        return ESP_OK;
    }

    if (ili9341_shadow_matches(display, tile->tile_ptr, tile->x_pos, tile->y_pos, count)) {
        return ESP_OK;
    }

    const uint16_t x = (uint16_t)tile->x_pos * 8U;
    const uint16_t y = (uint16_t)tile->y_pos * 8U;
    uint16_t width = (uint16_t)count * 8U;
    uint16_t height = 8;
    if (x + width > ILI9341_WIDTH) {
        width = ILI9341_WIDTH - x;
    }
    if (y + height > ILI9341_HEIGHT) {
        height = ILI9341_HEIGHT - y;
    }
    if (width == 0 || height == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ili9341_set_window(display, x, y, x + width - 1, y + height - 1),
                        TAG,
                        "tile window failed");
    ESP_RETURN_ON_ERROR(ili9341_cmd(display, 0x2c), TAG, "tile ram write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "dc data failed");

    for (uint16_t row = 0; row < height; row++) {
        ili9341_line_from_tile(display, tile->tile_ptr, row, width);
        ESP_RETURN_ON_ERROR(ili9341_tx_bytes(display, display->line_buffer, (size_t)width * 2U),
                            TAG,
                            "tile transmit failed");
    }

    ili9341_shadow_update(display, tile->tile_ptr, tile->x_pos, tile->y_pos, count);
    return ESP_OK;
}

static esp_err_t ili9341_full_init(tft_ili9341_t *display)
{
    ili9341_hardware_reset();

    if (!ili9341_checked_cmd(display, 0x01)) {
        return display->last_error;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t ef[] = {0x03, 0x80, 0x02};
    const uint8_t cf[] = {0x00, 0xc1, 0x30};
    const uint8_t ed[] = {0x64, 0x03, 0x12, 0x81};
    const uint8_t e8[] = {0x85, 0x00, 0x78};
    const uint8_t cb[] = {0x39, 0x2c, 0x00, 0x34, 0x02};
    const uint8_t f7[] = {0x20};
    const uint8_t ea[] = {0x00, 0x00};
    const uint8_t c0[] = {0x23};
    const uint8_t c1[] = {0x10};
    const uint8_t c5[] = {0x3e, 0x28};
    const uint8_t c7[] = {0x86};
    const uint8_t madctl[] = {SOLAR_OS_BOARD_DISPLAY_MADCTL};
    const uint8_t colmod[] = {0x55};
    const uint8_t b1[] = {0x00, 0x18};
    const uint8_t b6[] = {0x08, 0x82, 0x27};
    const uint8_t f2[] = {0x00};
    const uint8_t gamma[] = {0x01};
    const uint8_t e0[] = {
        0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1,
        0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00,
    };
    const uint8_t e1[] = {
        0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1,
        0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f,
    };

    if (!ili9341_checked_cmd_data(display, 0xef, ef, sizeof(ef)) ||
        !ili9341_checked_cmd_data(display, 0xcf, cf, sizeof(cf)) ||
        !ili9341_checked_cmd_data(display, 0xed, ed, sizeof(ed)) ||
        !ili9341_checked_cmd_data(display, 0xe8, e8, sizeof(e8)) ||
        !ili9341_checked_cmd_data(display, 0xcb, cb, sizeof(cb)) ||
        !ili9341_checked_cmd_data(display, 0xf7, f7, sizeof(f7)) ||
        !ili9341_checked_cmd_data(display, 0xea, ea, sizeof(ea)) ||
        !ili9341_checked_cmd_data(display, 0xc0, c0, sizeof(c0)) ||
        !ili9341_checked_cmd_data(display, 0xc1, c1, sizeof(c1)) ||
        !ili9341_checked_cmd_data(display, 0xc5, c5, sizeof(c5)) ||
        !ili9341_checked_cmd_data(display, 0xc7, c7, sizeof(c7)) ||
        !ili9341_checked_cmd_data(display, 0x36, madctl, sizeof(madctl)) ||
        !ili9341_checked_cmd_data(display, 0x3a, colmod, sizeof(colmod)) ||
        !ili9341_checked_cmd_data(display, 0xb1, b1, sizeof(b1)) ||
        !ili9341_checked_cmd_data(display, 0xb6, b6, sizeof(b6)) ||
        !ili9341_checked_cmd_data(display, 0xf2, f2, sizeof(f2)) ||
        !ili9341_checked_cmd_data(display, 0x26, gamma, sizeof(gamma)) ||
        !ili9341_checked_cmd_data(display, 0xe0, e0, sizeof(e0)) ||
        !ili9341_checked_cmd_data(display, 0xe1, e1, sizeof(e1)) ||
        !ili9341_checked_cmd(display, 0x11)) {
        return display->last_error;
    }

    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(ili9341_fill_screen(display, ILI9341_RGB565_WHITE),
                        TAG,
                        "screen clear failed");

    if (!ili9341_checked_cmd(display, 0x29)) {
        return display->last_error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    ili9341_set_backlight_power(display, true);

    ili9341_invalidate_shadow(display);
    display->last_error = ESP_OK;
    return ESP_OK;
}

static uint8_t ili9341_u8x8_byte_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

static uint8_t ili9341_u8x8_display_cb(u8x8_t *u8x8,
                                       uint8_t message,
                                       uint8_t arg_int,
                                       void *arg_ptr)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &ili9341_display_info);
        return 1;
    }

    tft_ili9341_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        return ili9341_full_init(display) == ESP_OK ? 1 : 0;

    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        ili9341_invalidate_shadow(display);
        if (arg_int == 0) {
            ili9341_set_backlight_power(display, true);
            return ili9341_cmd(display, 0x29) == ESP_OK ? 1 : 0;
        }
        ili9341_set_backlight_power(display, false);
        return ili9341_cmd(display, 0x28) == ESP_OK ? 1 : 0;

    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return ili9341_draw_tile(display, (const u8x8_tile_t *)arg_ptr) == ESP_OK ? 1 : 0;

    default:
        return 0;
    }
}

esp_err_t tft_ili9341_init(tft_ili9341_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->last_error = ESP_OK;
    display->backlight_percent = 100;

    ESP_RETURN_ON_ERROR(ili9341_configure_control_pins(), TAG, "control pin config failed");
    ili9341_set_backlight_power(display, false);
    ESP_RETURN_ON_ERROR(solar_os_spi_bus_acquire(), TAG, "spi bus acquire failed");
    display->bus_acquired = true;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = ILI9341_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = SOLAR_OS_BOARD_PIN_LCD_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(solar_os_spi_bus_host(), &device_config, &display->spi),
                        TAG,
                        "spi add device failed");

    display->line_buffer_size = ILI9341_LINE_BYTES;
    display->line_buffer = heap_caps_malloc(display->line_buffer_size,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->line_buffer == NULL) {
        tft_ili9341_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->buffer_size = ILI9341_BUFFER_BYTES;
    display->buffer = heap_caps_malloc(display->buffer_size, MALLOC_CAP_8BIT);
    if (display->buffer == NULL) {
        tft_ili9341_deinit(display);
        return ESP_ERR_NO_MEM;
    }
    memset(display->buffer, 0, display->buffer_size);

    display->shadow_size = ILI9341_BUFFER_BYTES;
    display->shadow = heap_caps_malloc(display->shadow_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->shadow == NULL) {
        ESP_LOGW(TAG, "display shadow allocation failed, partial update skipping disabled");
        display->shadow_size = 0;
    } else {
        memset(display->shadow, 0, display->shadow_size);
        ili9341_invalidate_shadow(display);
    }

    u8g2_SetupDisplay(&display->u8g2,
                      ili9341_u8x8_display_cb,
                      u8x8_dummy_cb,
                      ili9341_u8x8_byte_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     ILI9341_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);

    return display->last_error;
}

esp_err_t tft_ili9341_resume(tft_ili9341_t *display)
{
    if (display == NULL ||
        display->spi == NULL ||
        display->buffer == NULL ||
        display->line_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ili9341_configure_control_pins(), TAG, "resume pin config failed");
    active_display = display;
    display->last_error = ESP_OK;
    ili9341_invalidate_shadow(display);
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    return display->last_error;
}

void tft_ili9341_deinit(tft_ili9341_t *display)
{
    if (display == NULL) {
        return;
    }

    ili9341_set_backlight_power(display, false);

    if (display->spi != NULL) {
        spi_bus_remove_device(display->spi);
        display->spi = NULL;
    }

    if (display->bus_acquired) {
        solar_os_spi_bus_release();
        display->bus_acquired = false;
    }

    if (display->line_buffer != NULL) {
        heap_caps_free(display->line_buffer);
        display->line_buffer = NULL;
    }
    if (display->buffer != NULL) {
        heap_caps_free(display->buffer);
        display->buffer = NULL;
    }
    if (display->shadow != NULL) {
        heap_caps_free(display->shadow);
        display->shadow = NULL;
    }

    if (active_display == display) {
        active_display = NULL;
    }

    display->buffer_size = 0;
    display->shadow_size = 0;
    display->line_buffer_size = 0;
    display->shadow_valid_rows = 0;
}

u8g2_t *tft_ili9341_get_u8g2(tft_ili9341_t *display)
{
    return display == NULL ? NULL : &display->u8g2;
}

bool tft_ili9341_backlight_supported(void)
{
    return ili9341_backlight_supported();
}

esp_err_t tft_ili9341_get_backlight(const tft_ili9341_t *display, uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display == NULL) {
        *percent = 0;
        return ESP_ERR_INVALID_STATE;
    }
    if (!ili9341_backlight_supported()) {
        *percent = 100;
        return ESP_ERR_NOT_SUPPORTED;
    }

    *percent = display->backlight_percent;
    return ESP_OK;
}

esp_err_t tft_ili9341_set_backlight(tft_ili9341_t *display, uint8_t percent)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ili9341_backlight_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    display->backlight_percent = percent;
    if (display->backlight_power) {
        return ili9341_apply_backlight(percent);
    }
    return ESP_OK;
}
