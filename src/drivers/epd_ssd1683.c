#include "epd_ssd1683.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define SSD1683_WIDTH 400U
#define SSD1683_HEIGHT 300U
#define SSD1683_TILE_WIDTH ((SSD1683_WIDTH + 7U) / 8U)
#define SSD1683_TILE_HEIGHT ((SSD1683_HEIGHT + 7U) / 8U)
#define SSD1683_BUFFER_ROW_BYTES (SSD1683_TILE_WIDTH * 8U)
#define SSD1683_BUFFER_BYTES (SSD1683_BUFFER_ROW_BYTES * SSD1683_TILE_HEIGHT)
#define SSD1683_PANEL_ROW_BYTES ((SSD1683_WIDTH + 7U) / 8U)
#define SSD1683_BUSY_TIMEOUT_MS 30000U
#define SSD1683_BUSY_ASSERT_TIMEOUT_MS 100U
#define SSD1683_VARIANT_PROBE_MS 500U
#define SSD1683_RESET_READY_PROBE_MS 1000U
#define SSD1683_RESET_PRE_HIGH_MS 10U
#define SSD1683_RESET_LOW_MS 100U
#define SSD1683_WAVESHARE_RESET_PRE_HIGH_MS 100U
#define SSD1683_WAVESHARE_RESET_LOW_MS 2U
#define SSD1683_RESET_RECOVERY_LOW_MS 1000U
#define SSD1683_AUTO_FULL_INTERVAL 20U

static const char *TAG = "epd_ssd1683";

typedef struct {
    uint8_t x_start_byte;
    uint8_t x_end_byte;
    uint16_t y_start;
    uint16_t y_end;
} ssd1683_window_t;

/*
 * Elecrow ships two electrically compatible panel revisions. The current
 * green-sticker revision uses a different controller command set and waveform
 * tables even though the product documentation still calls the panel SSD1683.
 * The unused bytes in each 42-byte waveform row are intentionally zero.
 */
static const uint8_t green_full_lut[5][42] = {
    {0x01, 0x14, 0x0a, 0x14, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0a, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0a, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0a, 0x54, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0a, 0x54, 0x00, 0x01, 0x01},
};

static const u8x8_display_info_t ssd1683_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 10,
    .post_reset_wait_ms = 10,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 10000000,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = SSD1683_TILE_WIDTH,
    .tile_height = SSD1683_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = SSD1683_WIDTH,
    .pixel_height = SSD1683_HEIGHT,
};

static epd_ssd1683_t *ssd1683_from_u8x8(u8x8_t *u8x8)
{
    if (u8x8 == NULL) {
        return NULL;
    }
    return (epd_ssd1683_t *)((uint8_t *)u8x8 -
                             offsetof(epd_ssd1683_t, u8g2) -
                             offsetof(u8g2_t, u8x8));
}

static esp_err_t ssd1683_tx_byte(epd_ssd1683_t *display, uint8_t value)
{
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = {value},
    };
    return spi_device_polling_transmit(display->spi, &transaction);
}

static esp_err_t ssd1683_tx_bytes(epd_ssd1683_t *display,
                                  const uint8_t *data,
                                  size_t length)
{
    if (length > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (length > 0) {
        const size_t chunk = length > display->line_buffer_size ?
            display->line_buffer_size : length;
        if (data != display->line_buffer) {
            memcpy(display->line_buffer, data, chunk);
        }

        spi_transaction_t transaction = {
            .length = chunk * 8U,
            .tx_buffer = display->line_buffer,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display->spi, &transaction),
                            TAG,
                            "SPI transmit failed");
        data += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

static esp_err_t ssd1683_cmd_data(epd_ssd1683_t *display,
                                  uint8_t command,
                                  const uint8_t *data,
                                  size_t length)
{
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 0),
                        TAG,
                        "D/C command failed");
    ESP_RETURN_ON_ERROR(ssd1683_tx_byte(display, command), TAG, "command transmit failed");
    if (length == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                        TAG,
                        "D/C data failed");
    return ssd1683_tx_bytes(display, data, length);
}

static esp_err_t ssd1683_cmd(epd_ssd1683_t *display, uint8_t command)
{
    return ssd1683_cmd_data(display, command, NULL, 0);
}

static bool ssd1683_busy_cleared(const epd_ssd1683_t *display, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    while (gpio_get_level((gpio_num_t)display->busy_pin) == display->busy_level) {
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static bool ssd1683_busy_asserted(const epd_ssd1683_t *display, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    while (gpio_get_level((gpio_num_t)display->busy_pin) != display->busy_level) {
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

static esp_err_t ssd1683_wait_ready(const epd_ssd1683_t *display)
{
    if (!ssd1683_busy_cleared(display, SSD1683_BUSY_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "BUSY timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t ssd1683_set_power(epd_ssd1683_t *display, bool on)
{
    if (display->power_pin >= 0) {
        const int active = display->power_active_level ? 1 : 0;
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->power_pin,
                                           on ? active : !active),
                            TAG,
                            "panel power failed");
    }
    display->powered = on;
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t ssd1683_configure_pins(epd_ssd1683_t *display)
{
    uint64_t output_pin_mask = (1ULL << (uint32_t)display->dc_pin) |
        (1ULL << (uint32_t)display->reset_pin);
    if (display->power_pin >= 0) {
        output_pin_mask |= 1ULL << (uint32_t)display->power_pin;
    }
    const gpio_config_t output_config = {
        .pin_bit_mask = output_pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "output GPIO config failed");

    const gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << (uint32_t)display->busy_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_config), TAG, "BUSY GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1), TAG, "D/C idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->reset_pin, 1), TAG, "reset idle failed");
    display->powered = true;
    return display->power_pin >= 0 ? ssd1683_set_power(display, false) : ESP_OK;
}

static void ssd1683_hardware_reset(const epd_ssd1683_t *display,
                                   uint32_t pre_reset_high_ms,
                                   uint32_t reset_low_ms)
{
    gpio_set_level((gpio_num_t)display->reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(pre_reset_high_ms));
    gpio_set_level((gpio_num_t)display->reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(reset_low_ms));
    gpio_set_level((gpio_num_t)display->reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t ssd1683_set_window(epd_ssd1683_t *display,
                                    const ssd1683_window_t *window)
{
    if (window == NULL ||
        window->x_start_byte > window->x_end_byte ||
        window->x_end_byte >= SSD1683_PANEL_ROW_BYTES ||
        window->y_start > window->y_end ||
        window->y_end >= SSD1683_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t data_entry_mode[] = {0x03};
    const uint8_t x_bounds[] = {window->x_start_byte, window->x_end_byte};
    const uint8_t y_bounds[] = {
        (uint8_t)(window->y_start & 0xffU),
        (uint8_t)(window->y_start >> 8),
        (uint8_t)(window->y_end & 0xffU),
        (uint8_t)(window->y_end >> 8),
    };
    const uint8_t x_cursor[] = {window->x_start_byte};
    const uint8_t y_cursor[] = {
        (uint8_t)(window->y_start & 0xffU),
        (uint8_t)(window->y_start >> 8),
    };

    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x11,
                                         data_entry_mode,
                                         sizeof(data_entry_mode)),
                        TAG,
                        "data entry mode failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x44, x_bounds, sizeof(x_bounds)),
                        TAG,
                        "X bounds failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x45, y_bounds, sizeof(y_bounds)),
                        TAG,
                        "Y bounds failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x4e, x_cursor, sizeof(x_cursor)),
                        TAG,
                        "X cursor failed");
    return ssd1683_cmd_data(display, 0x4f, y_cursor, sizeof(y_cursor));
}

static esp_err_t ssd1683_set_address(epd_ssd1683_t *display)
{
    const ssd1683_window_t full_window = {
        .x_start_byte = 0,
        .x_end_byte = SSD1683_PANEL_ROW_BYTES - 1U,
        .y_start = 0,
        .y_end = SSD1683_HEIGHT - 1U,
    };
    return ssd1683_set_window(display, &full_window);
}

static esp_err_t ssd1683_legacy_init(epd_ssd1683_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x12), TAG, "software reset failed");
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display), TAG, "software reset wait failed");

    const uint8_t update_control[] = {0x40, 0x00};
    const uint8_t border_waveform[] = {0x05};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x21,
                                         update_control,
                                         sizeof(update_control)),
                        TAG,
                        "update control failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x3c,
                                         border_waveform,
                                         sizeof(border_waveform)),
                        TAG,
                        "border waveform failed");
    if (display->panel_variant != EPD_SSD1683_PANEL_WAVESHARE_V2) {
        const uint8_t temperature[] = {0x6e};
        const uint8_t load_temperature[] = {0x91};
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                             0x1a,
                                             temperature,
                                             sizeof(temperature)),
                            TAG,
                            "temperature setup failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                             0x22,
                                             load_temperature,
                                             sizeof(load_temperature)),
                            TAG,
                            "temperature load failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20),
                            TAG,
                            "temperature activate failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display),
                            TAG,
                            "temperature load wait failed");
    }
    ESP_RETURN_ON_ERROR(ssd1683_set_address(display), TAG, "address setup failed");
    return ESP_OK;
}

static esp_err_t ssd1683_green_init(epd_ssd1683_t *display)
{
    static const uint8_t panel_setting[] = {0x3f, 0x4d};
    static const uint8_t power_setting[] = {0x03, 0x10, 0x3f, 0x3f, 0x03};
    static const uint8_t booster_soft_start[] = {0x96, 0x96, 0x29};
    static const uint8_t pll[] = {0x09};
    static const uint8_t resolution[] = {0x01, 0x90, 0x01, 0x2c};
    static const uint8_t vcom[] = {0x05};
    static const uint8_t data_interval[] = {0x97};
    static const uint8_t tcon[] = {0x22};
    static const uint8_t cascade[] = {0x88};

    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x00, panel_setting, sizeof(panel_setting)),
                        TAG, "green panel setting failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x01, power_setting, sizeof(power_setting)),
                        TAG, "green power setting failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x06,
                                         booster_soft_start,
                                         sizeof(booster_soft_start)),
                        TAG, "green booster setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x30, pll, sizeof(pll)),
                        TAG, "green PLL setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x61, resolution, sizeof(resolution)),
                        TAG, "green resolution setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x82, vcom, sizeof(vcom)),
                        TAG, "green VCOM setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x50,
                                         data_interval,
                                         sizeof(data_interval)),
                        TAG, "green data interval setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x60, tcon, sizeof(tcon)),
                        TAG, "green TCON setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0xe3, cascade, sizeof(cascade)),
                        TAG, "green cascade setup failed");
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

static esp_err_t ssd1683_controller_init(epd_ssd1683_t *display)
{
    const bool fixed_legacy_variant =
        display->panel_variant == EPD_SSD1683_PANEL_LEGACY ||
        display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2;
    if (!display->powered) {
        ESP_RETURN_ON_ERROR(ssd1683_set_power(display, true), TAG, "panel power on failed");
    }

    const bool waveshare_v2 =
        display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2;
    ssd1683_hardware_reset(display,
                           waveshare_v2 ? SSD1683_WAVESHARE_RESET_PRE_HIGH_MS :
                               SSD1683_RESET_PRE_HIGH_MS,
                           waveshare_v2 ? SSD1683_WAVESHARE_RESET_LOW_MS :
                               SSD1683_RESET_LOW_MS);
    if (display->panel_variant == EPD_SSD1683_PANEL_UNKNOWN) {
        display->panel_variant = ssd1683_busy_cleared(display, SSD1683_VARIANT_PROBE_MS) ?
            EPD_SSD1683_PANEL_LEGACY : EPD_SSD1683_PANEL_GREEN_STICKER;
        ESP_LOGI(TAG,
                 "detected %s panel revision",
                 display->panel_variant == EPD_SSD1683_PANEL_LEGACY ?
                     "legacy SSD1683" : "green-sticker");
    }

    if (display->panel_variant == EPD_SSD1683_PANEL_LEGACY ||
        display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2) {
        if (fixed_legacy_variant &&
            !ssd1683_busy_cleared(display, SSD1683_RESET_READY_PROBE_MS)) {
            ESP_LOGW(TAG, "BUSY remained active after reset; applying long reset recovery");
            ssd1683_hardware_reset(display,
                                   SSD1683_WAVESHARE_RESET_PRE_HIGH_MS,
                                   SSD1683_RESET_RECOVERY_LOW_MS);
        }
        ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display), TAG, "panel reset wait failed");
        ESP_RETURN_ON_ERROR(ssd1683_legacy_init(display), TAG, "legacy panel init failed");
    } else {
        ESP_RETURN_ON_ERROR(ssd1683_green_init(display), TAG, "green panel init failed");
    }

    display->controller_ready = true;
    display->shadow_valid = false;
    display->partial_refresh_active = false;
    display->fast_refresh_count = 0;
    return ESP_OK;
}

static void ssd1683_convert_row(epd_ssd1683_t *display,
                                const uint8_t *source,
                                uint16_t y)
{
    const uint8_t row_bit = (uint8_t)(1U << (y & 7U));
    const uint8_t *tile_row = source +
        (size_t)(y >> 3) * SSD1683_BUFFER_ROW_BYTES;

    for (size_t byte = 0; byte < SSD1683_PANEL_ROW_BYTES; byte++) {
        uint8_t panel_pixels = 0;
        const uint8_t *columns = tile_row + byte * 8U;
        for (unsigned bit = 0; bit < 8U; bit++) {
            if ((columns[bit] & row_bit) != 0) {
                panel_pixels |= (uint8_t)(0x80U >> bit);
            }
        }
        display->line_buffer[byte] = panel_pixels;
    }
}

static bool ssd1683_find_change_window(const epd_ssd1683_t *display,
                                       ssd1683_window_t *window)
{
    if (display == NULL || window == NULL || display->shadow == NULL) {
        return false;
    }

    bool changed = false;
    window->x_start_byte = SSD1683_PANEL_ROW_BYTES;
    window->x_end_byte = 0;
    window->y_start = SSD1683_HEIGHT;
    window->y_end = 0;

    for (uint16_t y = 0; y < SSD1683_HEIGHT; y++) {
        const uint8_t row_bit = (uint8_t)(1U << (y & 7U));
        const size_t tile_row = (size_t)(y >> 3) * SSD1683_BUFFER_ROW_BYTES;
        for (uint16_t x = 0; x < SSD1683_WIDTH; x++) {
            const size_t index = tile_row + x;
            if (((display->buffer[index] ^ display->shadow[index]) & row_bit) == 0) {
                continue;
            }
            const uint8_t x_byte = (uint8_t)(x >> 3);
            if (!changed || x_byte < window->x_start_byte) {
                window->x_start_byte = x_byte;
            }
            if (!changed || x_byte > window->x_end_byte) {
                window->x_end_byte = x_byte;
            }
            if (!changed || y < window->y_start) {
                window->y_start = y;
            }
            if (!changed || y > window->y_end) {
                window->y_end = y;
            }
            changed = true;
        }
    }
    return changed;
}

static esp_err_t ssd1683_write_window_data(epd_ssd1683_t *display,
                                           const ssd1683_window_t *window,
                                           uint8_t command,
                                           const uint8_t *source)
{
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ssd1683_set_window(display, window),
                        TAG,
                        "partial window failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, command), TAG, "partial RAM write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                        TAG,
                        "D/C partial data failed");

    const size_t row_bytes =
        (size_t)window->x_end_byte - window->x_start_byte + 1U;
    for (uint16_t y = window->y_start; y <= window->y_end; y++) {
        ssd1683_convert_row(display, source, y);
        if (window->x_start_byte != 0) {
            memmove(display->line_buffer,
                    display->line_buffer + window->x_start_byte,
                    row_bytes);
        }
        ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                             display->line_buffer,
                                             row_bytes),
                            TAG,
                            "partial frame transmit failed");
    }
    return ESP_OK;
}

static esp_err_t ssd1683_write_partial_frame(epd_ssd1683_t *display,
                                             const ssd1683_window_t *window)
{
    const uint8_t border_waveform[] = {0x80};
    const uint8_t update_control[] = {0x00, 0x00};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x3c,
                                         border_waveform,
                                         sizeof(border_waveform)),
                        TAG,
                        "partial border waveform failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x21,
                                         update_control,
                                         sizeof(update_control)),
                        TAG,
                        "partial update control failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x3c,
                                         border_waveform,
                                         sizeof(border_waveform)),
                        TAG,
                        "partial border waveform failed");
    return ssd1683_write_window_data(display, window, 0x24, display->buffer);
}

static esp_err_t ssd1683_trigger_update(epd_ssd1683_t *display,
                                        bool full,
                                        bool partial)
{
    if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER) {
        /* Elecrow's current panel only supports its full-screen GC path here. */
        const uint8_t (*lut)[42] = green_full_lut;
        for (uint8_t index = 0; index < 5; index++) {
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                                 (uint8_t)(0x20U + index),
                                                 lut[index],
                                                 sizeof(lut[index])),
                                TAG,
                                "green waveform load failed");
        }
        const uint8_t update[] = {0xa5};
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x17, update, sizeof(update)),
                            TAG,
                            "green display update failed");
        return ssd1683_wait_ready(display);
    }

    const uint8_t update_mode[] = {partial ? 0xff : (full ? 0xf7 : 0xc7)};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x22, update_mode, sizeof(update_mode)),
                        TAG,
                        "display update mode failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20), TAG, "display update failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    if (display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 &&
        !ssd1683_busy_asserted(display, SSD1683_BUSY_ASSERT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "BUSY did not assert after display update command");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ssd1683_wait_ready(display);
}

static esp_err_t ssd1683_refresh(epd_ssd1683_t *display)
{
    if (!display->controller_ready) {
        ESP_RETURN_ON_ERROR(ssd1683_controller_init(display), TAG, "controller resume failed");
    }
    if (display->shadow_valid &&
        display->shadow != NULL &&
        memcmp(display->buffer, display->shadow, display->buffer_size) == 0) {
        return ESP_OK;
    }
    const bool first_refresh = !display->shadow_valid;
    const bool full = display->refresh_mode == EPD_SSD1683_REFRESH_FULL ||
        (display->refresh_mode == EPD_SSD1683_REFRESH_AUTO &&
         (!display->shadow_valid ||
          display->fast_refresh_count >= SSD1683_AUTO_FULL_INTERVAL - 1U));
    const bool partial =
        display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 &&
        display->refresh_mode == EPD_SSD1683_REFRESH_AUTO &&
        display->shadow != NULL &&
        !full;
    ssd1683_window_t partial_window = {0};
    if (partial && !ssd1683_find_change_window(display, &partial_window)) {
        /* Keep the padded U8g2 tile rows synchronized even when no visible pixel changed. */
        memcpy(display->shadow, display->buffer, display->buffer_size);
        return ESP_OK;
    }

    if (display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 &&
        !partial && display->partial_refresh_active) {
        /* Waveshare requires full initialization when leaving partial mode. */
        ESP_RETURN_ON_ERROR(ssd1683_controller_init(display),
                            TAG,
                            "full refresh reinitialization failed");
    }

    const bool log_refresh = display->refresh_log_count < 4U;
    if (log_refresh) {
        if (partial) {
            ESP_LOGI(TAG,
                     "panel refresh %u partial x=%u..%u y=%u..%u starting",
                     (unsigned)(display->refresh_log_count + 1U),
                     (unsigned)partial_window.x_start_byte * 8U,
                     ((unsigned)partial_window.x_end_byte + 1U) * 8U - 1U,
                     (unsigned)partial_window.y_start,
                     (unsigned)partial_window.y_end);
        } else {
            ESP_LOGI(TAG,
                     "panel refresh %u %s starting",
                     (unsigned)(display->refresh_log_count + 1U),
                     full ? "full" : "fast");
        }
    }

    if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER && !first_refresh) {
        /*
         * Elecrow's revised panel requires a reset/init before every changed
         * frame.  Keeping the controller live works for the initial clear but
         * later 0x13 updates can be ignored, leaving the previous image on the
         * bistable panel.
         */
        ssd1683_hardware_reset(display,
                               SSD1683_RESET_PRE_HIGH_MS,
                               SSD1683_RESET_LOW_MS);
        ESP_RETURN_ON_ERROR(ssd1683_green_init(display),
                            TAG,
                            "green panel refresh init failed");
    }

    if (partial) {
        ESP_RETURN_ON_ERROR(ssd1683_write_partial_frame(display, &partial_window),
                            TAG,
                            "partial frame write failed");
    } else if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER) {
        if (first_refresh) {
            ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x10),
                                TAG,
                                "old frame RAM write failed");
            ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                                TAG,
                                "D/C old frame data failed");
            for (uint16_t y = 0; y < SSD1683_HEIGHT; y++) {
                memset(display->line_buffer, 0xff, SSD1683_PANEL_ROW_BYTES);
                ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                                     display->line_buffer,
                                                     SSD1683_PANEL_ROW_BYTES),
                                    TAG,
                                    "old frame transmit failed");
            }
        } else {
            const uint8_t data_interval[] = {0xd7};
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                                 0x50,
                                                 data_interval,
                                                 sizeof(data_interval)),
                                TAG,
                                "green refresh data interval failed");
        }
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x13), TAG, "new frame RAM write failed");
    } else {
        ESP_RETURN_ON_ERROR(ssd1683_set_address(display), TAG, "refresh address failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x24), TAG, "RAM write failed");
    }

    if (!partial) {
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                            TAG,
                            "D/C data failed");
        for (uint16_t y = 0; y < SSD1683_HEIGHT; y++) {
            ssd1683_convert_row(display, display->buffer, y);
            ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                                 display->line_buffer,
                                                 SSD1683_PANEL_ROW_BYTES),
                                TAG,
                                "frame transmit failed");
        }
    }

    if (display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 && !partial) {
        /* Waveshare's V2/UC8176 full-frame path mirrors the image into both RAM planes. */
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x26), TAG, "second RAM write failed");
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                            TAG,
                            "D/C second RAM data failed");
        for (uint16_t y = 0; y < SSD1683_HEIGHT; y++) {
            ssd1683_convert_row(display, display->buffer, y);
            ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                                 display->line_buffer,
                                                 SSD1683_PANEL_ROW_BYTES),
                                TAG,
                                "second frame transmit failed");
        }
    }

    ESP_RETURN_ON_ERROR(ssd1683_trigger_update(display, full, partial),
                        TAG,
                        "panel refresh failed");
    if (partial) {
        /*
         * Differential refresh compares the current and previous RAM planes.
         * Synchronize both to the displayed frame after every update so stale
         * full-screen content cannot reappear on alternating partial refreshes.
         */
        ESP_RETURN_ON_ERROR(ssd1683_write_window_data(display,
                                                      &partial_window,
                                                      0x26,
                                                      display->buffer),
                            TAG,
                            "previous partial frame sync failed");
        ESP_RETURN_ON_ERROR(ssd1683_write_window_data(display,
                                                      &partial_window,
                                                      0x24,
                                                      display->buffer),
                            TAG,
                            "current partial frame sync failed");
    }
    if (log_refresh) {
        display->refresh_log_count++;
        ESP_LOGI(TAG,
                 "panel refresh %u complete",
                 (unsigned)display->refresh_log_count);
    }

    if (display->shadow != NULL) {
        memcpy(display->shadow, display->buffer, display->buffer_size);
    }
    display->shadow_valid = true;
    display->partial_refresh_active = partial;
    display->fast_refresh_count = full ? 0 : (uint8_t)(display->fast_refresh_count + 1U);
    return ESP_OK;
}

static esp_err_t ssd1683_sleep(epd_ssd1683_t *display)
{
    if (display->controller_ready) {
        if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER) {
            const uint8_t sleep_mode[] = {0xa5};
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                                 0x07,
                                                 sleep_mode,
                                                 sizeof(sleep_mode)),
                                TAG,
                                "green deep sleep failed");
        } else {
            const uint8_t sleep_mode[] = {0x01};
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                                 0x10,
                                                 sleep_mode,
                                                 sizeof(sleep_mode)),
                                TAG,
                                "deep sleep failed");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    display->controller_ready = false;
    display->shadow_valid = false;
    display->partial_refresh_active = false;
    return ssd1683_set_power(display, false);
}

static uint8_t ssd1683_u8x8_byte_cb(u8x8_t *u8x8,
                                    uint8_t message,
                                    uint8_t arg_int,
                                    void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

static uint8_t ssd1683_u8x8_display_cb(u8x8_t *u8x8,
                                       uint8_t message,
                                       uint8_t arg_int,
                                       void *arg_ptr)
{
    (void)arg_ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &ssd1683_display_info);
        return 1;
    }

    epd_ssd1683_t *display = ssd1683_from_u8x8(u8x8);
    if (display == NULL) {
        return 0;
    }

    esp_err_t err = ESP_OK;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = ssd1683_controller_init(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int != 0) {
            err = ssd1683_sleep(display);
        } else if (!display->controller_ready) {
            err = ssd1683_controller_init(display);
        }
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        err = ssd1683_refresh(display);
        break;
    default:
        return 0;
    }

    display->last_error = err;
    return err == ESP_OK ? 1 : 0;
}

static bool ssd1683_pin_valid(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

static bool ssd1683_config_valid(const epd_ssd1683_config_t *config)
{
    if (config == NULL ||
        !ssd1683_pin_valid(config->cs_pin) ||
        !ssd1683_pin_valid(config->dc_pin) ||
        !ssd1683_pin_valid(config->reset_pin) ||
        !ssd1683_pin_valid(config->busy_pin) ||
        config->spi_clock_hz <= 0 ||
        config->rotation == NULL ||
        (config->busy_level != 0 && config->busy_level != 1) ||
        (config->power_active_level != 0 && config->power_active_level != 1) ||
        config->panel_variant > EPD_SSD1683_PANEL_WAVESHARE_V2) {
        return false;
    }
    if (config->spi_bus == NULL || config->spi_bus[0] == '\0') {
        if (!ssd1683_pin_valid(config->sclk_pin) ||
            !ssd1683_pin_valid(config->mosi_pin) ||
            (config->spi_host != SPI2_HOST && config->spi_host != SPI3_HOST)) {
            return false;
        }
    }
    const int pins[] = {
        config->cs_pin,
        config->dc_pin,
        config->reset_pin,
        config->busy_pin,
        config->power_pin,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] < 0) {
            continue;
        }
        if (!ssd1683_pin_valid(pins[i])) {
            return false;
        }
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); j++) {
            if (pins[i] == pins[j]) {
                return false;
            }
        }
    }
    return true;
}

esp_err_t epd_ssd1683_init(epd_ssd1683_t *display,
                           const epd_ssd1683_config_t *config)
{
    if (display == NULL || !ssd1683_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->last_error = ESP_OK;
    display->refresh_mode = EPD_SSD1683_REFRESH_AUTO;
    display->panel_variant = config->panel_variant;
    display->dc_pin = config->dc_pin;
    display->reset_pin = config->reset_pin;
    display->busy_pin = config->busy_pin;
    display->power_pin = config->power_pin;
    display->busy_level = config->busy_level;
    display->power_active_level = config->power_active_level;
    display->spi_host = config->spi_host;
    ESP_RETURN_ON_ERROR(ssd1683_configure_pins(display), TAG, "control pin config failed");

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = config->spi_clock_hz,
        .mode = 0,
        .spics_io_num = config->cs_pin,
        .queue_size = 1,
    };
    esp_err_t ret = ESP_OK;
    if (config->spi_bus != NULL && config->spi_bus[0] != '\0') {
        ret = solar_os_bus_spi_add_device(config->spi_bus, &device_config, &display->spi);
    } else {
        const spi_bus_config_t bus_config = {
            .mosi_io_num = config->mosi_pin,
            .miso_io_num = GPIO_NUM_NC,
            .sclk_io_num = config->sclk_pin,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = SSD1683_PANEL_ROW_BYTES,
        };
        ret = spi_bus_initialize(config->spi_host, &bus_config, SPI_DMA_CH_AUTO);
        if (ret == ESP_OK) {
            display->bus_initialized = true;
            ret = spi_bus_add_device(config->spi_host, &device_config, &display->spi);
        }
    }
    if (ret != ESP_OK) {
        epd_ssd1683_deinit(display);
        return ret;
    }

    display->line_buffer_size = SSD1683_PANEL_ROW_BYTES;
    /* SPI transmits directly from this line buffer, so it must be internal DMA memory. */
    display->line_buffer = heap_caps_malloc(display->line_buffer_size,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->line_buffer == NULL) {
        epd_ssd1683_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->buffer_size = SSD1683_BUFFER_BYTES;
    /* Driver framebuffer only requires byte-addressable memory. */
    display->buffer = heap_caps_calloc(1, display->buffer_size, MALLOC_CAP_8BIT);
    if (display->buffer == NULL) {
        epd_ssd1683_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->shadow_size = SSD1683_BUFFER_BYTES;
    /* Full-frame shadow prefers PSRAM but remains optional without it. */
    display->shadow = heap_caps_malloc(display->shadow_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->shadow == NULL) {
        display->shadow = heap_caps_malloc(display->shadow_size, MALLOC_CAP_8BIT);
    }
    if (display->shadow == NULL) {
        ESP_LOGW(TAG, "display shadow allocation failed, unchanged frames cannot be skipped");
        display->shadow_size = 0;
    }

    u8g2_SetupDisplay(&display->u8g2,
                      ssd1683_u8x8_display_cb,
                      u8x8_dummy_cb,
                      ssd1683_u8x8_byte_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     SSD1683_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     config->rotation);
    u8g2_InitDisplay(&display->u8g2);
    ret = display->last_error;
    if (ret == ESP_OK) {
        u8g2_SetPowerSave(&display->u8g2, 0);
        ret = display->last_error;
    }
    if (ret != ESP_OK) {
        epd_ssd1683_deinit(display);
    }
    return ret;
}

esp_err_t epd_ssd1683_resume(epd_ssd1683_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ssd1683_configure_pins(display), TAG, "resume pin config failed");
    display->last_error = ESP_OK;
    return ssd1683_controller_init(display);
}

void epd_ssd1683_deinit(epd_ssd1683_t *display)
{
    if (display == NULL) {
        return;
    }

    if (display->spi != NULL && display->powered) {
        (void)ssd1683_sleep(display);
    }
    if (display->spi != NULL) {
        (void)spi_bus_remove_device(display->spi);
        display->spi = NULL;
    }
    if (display->bus_initialized) {
        (void)spi_bus_free(display->spi_host);
        display->bus_initialized = false;
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
    if (display->dc_pin >= 0) {
        (void)gpio_reset_pin((gpio_num_t)display->dc_pin);
    }
    if (display->reset_pin >= 0) {
        (void)gpio_reset_pin((gpio_num_t)display->reset_pin);
    }
    if (display->busy_pin >= 0) {
        (void)gpio_reset_pin((gpio_num_t)display->busy_pin);
    }
    if (display->power_pin >= 0) {
        (void)gpio_reset_pin((gpio_num_t)display->power_pin);
    }
    display->buffer_size = 0;
    display->shadow_size = 0;
    display->line_buffer_size = 0;
    display->controller_ready = false;
    display->shadow_valid = false;
}

u8g2_t *epd_ssd1683_get_u8g2(epd_ssd1683_t *display)
{
    return display == NULL ? NULL : &display->u8g2;
}

const char *epd_ssd1683_controller_mode(const epd_ssd1683_t *display)
{
    if (display == NULL) {
        return NULL;
    }
    switch (display->refresh_mode) {
    case EPD_SSD1683_REFRESH_FAST:
        return "refresh=fast";
    case EPD_SSD1683_REFRESH_FULL:
        return "refresh=full";
    case EPD_SSD1683_REFRESH_AUTO:
    default:
        return "refresh=auto";
    }
}

const char *epd_ssd1683_controller_mode_values(const epd_ssd1683_t *display)
{
    (void)display;
    return "refresh=<auto,fast,full>";
}

esp_err_t epd_ssd1683_set_controller_mode(epd_ssd1683_t *display, const char *mode)
{
    if (display == NULL || mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(mode, "refresh=auto") == 0) {
        display->refresh_mode = EPD_SSD1683_REFRESH_AUTO;
    } else if (strcmp(mode, "refresh=fast") == 0) {
        display->refresh_mode = EPD_SSD1683_REFRESH_FAST;
    } else if (strcmp(mode, "refresh=full") == 0) {
        display->refresh_mode = EPD_SSD1683_REFRESH_FULL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
