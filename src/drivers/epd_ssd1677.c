#include "epd_ssd1677.h"
#include "epd_ssd1677_waveform.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define SSD1677_WIDTH 800U
#define SSD1677_HEIGHT 480U
#define SSD1677_TILE_WIDTH (SSD1677_WIDTH / 8U)
#define SSD1677_TILE_HEIGHT (SSD1677_HEIGHT / 8U)
#define SSD1677_BUFFER_ROW_BYTES (SSD1677_TILE_WIDTH * 8U)
#define SSD1677_BUFFER_BYTES (SSD1677_BUFFER_ROW_BYTES * SSD1677_TILE_HEIGHT)
#define SSD1677_PANEL_ROW_BYTES (SSD1677_WIDTH / 8U)
#define SSD1677_BUSY_TIMEOUT_MS 30000U
#define SSD1677_AUTO_FULL_INTERVAL 20U

static const char *TAG = "epd_ssd1677";
#define TRY(call) do { esp_err_t result_ = (call); if (result_ != ESP_OK) return result_; } while (0)

static esp_err_t scan_process(void *context, const uint8_t *source,
                              bool fresh, bool *pending, bool *scanned);

static void scan_accepting(epd_ssd1677_t *d)
{
    const bool enabled = d->refresh_mode == EPD_SSD1677_REFRESH_PULSE &&
        d->scan_async && !d->suspended && !d->power_state_present &&
        !d->relax.storage && d->powered && d->controller_ready;
    epd_scan_worker_enable(&d->scan_worker, enabled);
    __atomic_store_n(&d->scan_submit_ready, enabled, __ATOMIC_RELEASE);
}

static void scan_disable(epd_ssd1677_t *d)
{
    __atomic_store_n(&d->scan_submit_ready, false, __ATOMIC_RELEASE);
    epd_scan_worker_enable(&d->scan_worker, false);
}

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}



typedef struct {
    uint16_t x_start_byte;
    uint16_t x_end_byte;
    uint16_t y_start;
    uint16_t y_end;
} ssd1677_window_t;

static const u8x8_display_info_t ssd1677_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 2,
    .post_reset_wait_ms = 50,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 20000000,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = SSD1677_TILE_WIDTH,
    .tile_height = SSD1677_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = SSD1677_WIDTH,
    .pixel_height = SSD1677_HEIGHT,
};

static epd_ssd1677_t *ssd1677_from_u8x8(u8x8_t *u8x8)
{
    if (u8x8 == NULL) {
        return NULL;
    }
    return (epd_ssd1677_t *)((uint8_t *)u8x8 -
                             offsetof(epd_ssd1677_t, u8g2) -
                             offsetof(u8g2_t, u8x8));
}

static esp_err_t ssd1677_tx_byte(epd_ssd1677_t *display, uint8_t value)
{
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .override_freq_hz = display->refresh_mode == EPD_SSD1677_REFRESH_PULSE ? display->pulse_spi_clock_hz : 0,
        .length = 8,
        .tx_data = {value},
    };
    return spi_device_polling_transmit(display->spi, &transaction);
}

static esp_err_t ssd1677_tx_bytes(epd_ssd1677_t *display,
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
            memmove(display->line_buffer, data, chunk);
        }
        spi_transaction_t transaction = {
            .length = chunk * 8U,
            .tx_buffer = display->line_buffer,
            .override_freq_hz = display->refresh_mode == EPD_SSD1677_REFRESH_PULSE ? display->pulse_spi_clock_hz : 0,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display->spi, &transaction),
                            TAG,
                            "SPI transmit failed");
        data += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

static esp_err_t ssd1677_cmd_data(epd_ssd1677_t *display,
                                  uint8_t command,
                                  const uint8_t *data,
                                  size_t length)
{
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 0),
                        TAG,
                        "D/C command failed");
    ESP_RETURN_ON_ERROR(ssd1677_tx_byte(display, command), TAG, "command transmit failed");
    if (length == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                        TAG,
                        "D/C data failed");
    return ssd1677_tx_bytes(display, data, length);
}

static esp_err_t ssd1677_cmd(epd_ssd1677_t *display, uint8_t command)
{
    return ssd1677_cmd_data(display, command, NULL, 0);
}

static esp_err_t ssd1677_wait_ready(const epd_ssd1677_t *display)
{
    const int64_t deadline = esp_timer_get_time() +
        (int64_t)SSD1677_BUSY_TIMEOUT_MS * 1000LL;
    const bool pulse = display->fastwave_loaded;
    delay_ms(pulse ? 1 : 100);
    while (gpio_get_level((gpio_num_t)display->busy_pin) == display->busy_level) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(pulse ? 1 : 20);
    }
    return ESP_OK;
}

static esp_err_t ssd1677_set_power(epd_ssd1677_t *display, bool on)
{
    if (display->set_power != NULL) {
        ESP_RETURN_ON_ERROR(display->set_power(display->power_context, on),
                            TAG,
                            "external panel power failed");
    }
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

static esp_err_t ssd1677_configure_pins(epd_ssd1677_t *display)
{
    uint64_t output_mask = (1ULL << (uint32_t)display->dc_pin) |
        (1ULL << (uint32_t)display->reset_pin);
    if (display->power_pin >= 0) {
        output_mask |= 1ULL << (uint32_t)display->power_pin;
    }
    const gpio_config_t outputs = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "output GPIO config failed");
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << (uint32_t)display->busy_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy), TAG, "BUSY GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1), TAG, "D/C idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->reset_pin, 1), TAG, "reset idle failed");
    display->powered = true;
    return display->power_pin >= 0 || display->set_power != NULL ?
        ssd1677_set_power(display, false) : ESP_OK;
}

static void ssd1677_hardware_reset(const epd_ssd1677_t *display)
{
    gpio_set_level((gpio_num_t)display->reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)display->reset_pin, 0);
    /* Match the working board sequence, including with a 100 Hz RTOS tick. */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)display->reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t ssd1677_set_full_address(epd_ssd1677_t *display)
{
    static const uint8_t data_entry[] = {0x01};
    static const uint8_t x_bounds[] = {0x00, 0x00, 0x1f, 0x03};
    static const uint8_t y_bounds[] = {0xdf, 0x01, 0x00, 0x00};
    static const uint8_t x_cursor[] = {0x00, 0x00};
    static const uint8_t y_cursor[] = {0xdf, 0x01};
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x11, data_entry, sizeof(data_entry)),
                        TAG, "data entry mode failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x44, x_bounds, sizeof(x_bounds)),
                        TAG, "X bounds failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x45, y_bounds, sizeof(y_bounds)),
                        TAG, "Y bounds failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x4e, x_cursor, sizeof(x_cursor)),
                        TAG, "X cursor failed");
    return ssd1677_cmd_data(display, 0x4f, y_cursor, sizeof(y_cursor));
}

static esp_err_t ssd1677_controller_init(epd_ssd1677_t *display)
{
    display->controller_ready = display->analog_on = display->fastwave_loaded = false;
    display->pulse_needs_cleanup = display->pulse_mask_dirty = true;
    display->pulse_window_valid = false;
    ESP_RETURN_ON_ERROR(ssd1677_set_power(display, true), TAG, "panel power-on failed");
    ssd1677_hardware_reset(display);
    ESP_RETURN_ON_ERROR(ssd1677_wait_ready(display), TAG, "reset wait failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd(display, 0x12), TAG, "software reset failed");
    ESP_RETURN_ON_ERROR(ssd1677_wait_ready(display), TAG, "software reset wait failed");

    static const uint8_t temperature_sensor[] = {0x80};
    static const uint8_t booster[] = {0xae, 0xc7, 0xc3, 0xc0, 0x80};
    static const uint8_t driver_output[] = {0xdf, 0x01, 0x02};
    static const uint8_t border[] = {0x80};
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x18,
                                         temperature_sensor,
                                         sizeof(temperature_sensor)),
                        TAG, "temperature sensor setup failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x0c, booster, sizeof(booster)),
                        TAG, "booster setup failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x01,
                                         driver_output,
                                         sizeof(driver_output)),
                        TAG, "driver output setup failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x3c, border, sizeof(border)),
                        TAG, "border waveform failed");
    ESP_RETURN_ON_ERROR(ssd1677_set_full_address(display), TAG, "address setup failed");
    ESP_RETURN_ON_ERROR(ssd1677_wait_ready(display), TAG, "address setup wait failed");
    static const uint8_t auto_write[] = {0xf7};
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x46, auto_write, sizeof(auto_write)),
                        TAG, "BW RAM initialization failed");
    ESP_RETURN_ON_ERROR(ssd1677_wait_ready(display), TAG, "BW RAM initialization wait failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x47, auto_write, sizeof(auto_write)),
                        TAG, "previous RAM initialization failed");
    ESP_RETURN_ON_ERROR(ssd1677_wait_ready(display), TAG, "previous RAM initialization wait failed");
    display->controller_ready = true;
    display->partial_refresh_active = false;
    return ESP_OK;
}

static void ssd1677_convert_row(epd_ssd1677_t *display,
                                const uint8_t *source,
                                uint16_t y, size_t first_byte, size_t end_byte)
{
    const uint8_t row_bit = (uint8_t)(1U << (y & 7U));
    const uint8_t *tile_row = source +
        (size_t)(y >> 3) * SSD1677_BUFFER_ROW_BYTES;
    for (size_t byte = first_byte; byte <= end_byte; byte++) {
        uint8_t panel_pixels = 0;
        const uint8_t *columns = tile_row + byte * 8U;
        for (unsigned bit = 0; bit < 8U; bit++) {
            if ((columns[bit] & row_bit) != 0) {
                panel_pixels |= (uint8_t)(0x80U >> bit);
            }
        }
        /* SolarOS display targets use set bits for white by default. */
        display->line_buffer[byte] = panel_pixels;
    }
}

static bool ssd1677_find_change_window(const epd_ssd1677_t *display,
                                       ssd1677_window_t *window)
{
    if (display == NULL || window == NULL || display->shadow == NULL) {
        return false;
    }
    bool changed = false;
    window->x_start_byte = SSD1677_PANEL_ROW_BYTES;
    window->x_end_byte = 0;
    window->y_start = SSD1677_HEIGHT;
    window->y_end = 0;
    for (uint16_t y = 0; y < SSD1677_HEIGHT; y++) {
        const uint8_t row_bit = (uint8_t)(1U << (y & 7U));
        const size_t tile_row = (size_t)(y >> 3) * SSD1677_BUFFER_ROW_BYTES;
        for (uint16_t x = 0; x < SSD1677_WIDTH; x++) {
            const size_t index = tile_row + x;
            if (((display->buffer[index] ^ display->shadow[index]) & row_bit) == 0) {
                continue;
            }
            const uint16_t x_byte = x >> 3;
            if (!changed || x_byte < window->x_start_byte) window->x_start_byte = x_byte;
            if (!changed || x_byte > window->x_end_byte) window->x_end_byte = x_byte;
            if (!changed || y < window->y_start) window->y_start = y;
            if (!changed || y > window->y_end) window->y_end = y;
            changed = true;
        }
    }
    return changed;
}

static esp_err_t ssd1677_set_partial_window(epd_ssd1677_t *display,
                                            const ssd1677_window_t *window)
{
    const uint16_t x_start = window->x_start_byte * 8U;
    const uint16_t x_end = (window->x_end_byte + 1U) * 8U - 1U;
    /* Logical rows run top to bottom; the controller decrements gate Y. */
    const uint16_t y_start = SSD1677_HEIGHT - 1U - window->y_start;
    const uint16_t y_end = SSD1677_HEIGHT - 1U - window->y_end;
    const uint8_t x_bounds[] = {
        (uint8_t)x_start, (uint8_t)(x_start >> 8),
        (uint8_t)x_end, (uint8_t)(x_end >> 8),
    };
    const uint8_t y_bounds[] = {
        (uint8_t)y_start, (uint8_t)(y_start >> 8),
        (uint8_t)y_end, (uint8_t)(y_end >> 8),
    };
    const uint8_t x_cursor[] = {(uint8_t)x_start, (uint8_t)(x_start >> 8)};
    const uint8_t y_cursor[] = {
        (uint8_t)y_start, (uint8_t)(y_start >> 8),
    };
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x44, x_bounds, sizeof(x_bounds)),
                        TAG, "partial X bounds failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x45, y_bounds, sizeof(y_bounds)),
                        TAG, "partial Y bounds failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x4e, x_cursor, sizeof(x_cursor)),
                        TAG, "partial X cursor failed");
    return ssd1677_cmd_data(display, 0x4f, y_cursor, sizeof(y_cursor));
}

static esp_err_t ssd1677_write_plane(epd_ssd1677_t *display,
                                    uint8_t command,
                                    const uint8_t *source,
                                    const ssd1677_window_t *window,
                                    bool invert)
{
    ESP_RETURN_ON_ERROR(ssd1677_set_partial_window(display, window), TAG, "RAM address failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd(display, command), TAG, "RAM write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                        TAG, "D/C frame data failed");
    const size_t row_bytes = window->x_end_byte - window->x_start_byte + 1U;
    for (uint16_t y = window->y_start; y <= window->y_end; y++) {
        ssd1677_convert_row(display, source, y, window->x_start_byte, window->x_end_byte);
        if (invert) {
            for (size_t byte = window->x_start_byte; byte <= window->x_end_byte; byte++) {
                display->line_buffer[byte] = (uint8_t)~display->line_buffer[byte];
            }
        }
        ESP_RETURN_ON_ERROR(ssd1677_tx_bytes(display,
                                             display->line_buffer + window->x_start_byte,
                                             row_bytes),
                            TAG, "frame transmit failed");
    }
    return ESP_OK;
}

static esp_err_t ssd1677_activate(epd_ssd1677_t *display, bool full)
{
    const bool mode1 = full && display->panel == EPD_SSD1677_PANEL_STICKY;
    /* Sticky's full OTP waveform uses matching monochrome bitplanes. */
    static const uint8_t control[] = {0x00};
    const uint8_t border[] = {mode1 ? 0x01 : 0xc0};
    const uint8_t update_mode[] = {mode1 ? 0xf7 : 0xfc};
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x21, control, sizeof(control)),
                        TAG, "update control failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x3c, border, sizeof(border)),
                        TAG, "update border setup failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x22,
                                         update_mode,
                                         sizeof(update_mode)),
                        TAG, "update mode failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd(display, 0x20), TAG, "update activation failed");
    display->analog_on = !mode1;
    return ssd1677_wait_ready(display);
}

static esp_err_t ssd1677_refresh_image(epd_ssd1677_t *display, const uint8_t *source, bool force_full)
{
    if (!display->controller_ready || display->fastwave_loaded) {
        display->shadow_valid = false;
        ESP_RETURN_ON_ERROR(ssd1677_controller_init(display), TAG, "controller resume failed");
    }
    if (!force_full && display->shadow_valid && display->shadow != NULL &&
        memcmp(source, display->shadow, display->buffer_size) == 0) {
        return ESP_OK;
    }
    const bool full = force_full || !display->shadow_valid || display->shadow == NULL ||
        display->refresh_mode == EPD_SSD1677_REFRESH_FULL ||
        (display->refresh_mode == EPD_SSD1677_REFRESH_AUTO &&
         display->partial_refresh_count >= SSD1677_AUTO_FULL_INTERVAL - 1U);
    ssd1677_window_t window = {0};
    const bool partial = !full && display->shadow != NULL &&
        (display->refresh_mode == EPD_SSD1677_REFRESH_AUTO ||
         display->refresh_mode == EPD_SSD1677_REFRESH_PARTIAL);
    const bool mode1 = full && display->panel == EPD_SSD1677_PANEL_STICKY;
    if (partial && !ssd1677_find_change_window(display, &window)) {
        memcpy(display->shadow, source, display->buffer_size);
        return ESP_OK;
    }

    const bool log_refresh = display->refresh_log_count < 4U;
    if (log_refresh) {
        if (partial) {
            ESP_LOGI(TAG,
                     "panel refresh %u partial x=%u..%u y=%u..%u starting",
                     (unsigned)(display->refresh_log_count + 1U),
                     (unsigned)window.x_start_byte * 8U,
                     ((unsigned)window.x_end_byte + 1U) * 8U - 1U,
                     (unsigned)window.y_start,
                     (unsigned)window.y_end);
        } else {
            ESP_LOGI(TAG, "panel refresh %u full starting",
                     (unsigned)(display->refresh_log_count + 1U));
        }
    }

    if (!partial) {
        window = (ssd1677_window_t) {
            .x_start_byte = 0, .x_end_byte = SSD1677_PANEL_ROW_BYTES - 1U,
            .y_start = 0, .y_end = SSD1677_HEIGHT - 1U,
        };
    }
    ESP_RETURN_ON_ERROR(ssd1677_write_plane(display, 0x24, source, &window, false),
                        TAG, "current frame write failed");
    ESP_RETURN_ON_ERROR(ssd1677_write_plane(display, 0x26,
                                           partial ? display->shadow : source,
                                           &window, !partial && !mode1),
                        TAG, "previous frame write failed");
    esp_err_t ret = ssd1677_activate(display, full);
    if (ret == ESP_OK) {
        /* Keep both controller planes aligned with the displayed frame. */
        ret = ssd1677_write_plane(display, 0x24, source, &window, false);
    }
    if (ret == ESP_OK) {
        ret = ssd1677_write_plane(display, 0x26, source, &window, false);
    }
    if (ret != ESP_OK) {
        display->shadow_valid = false;
        display->controller_ready = false;
        return ret;
    }

    if (log_refresh) {
        display->refresh_log_count++;
        ESP_LOGI(TAG, "panel refresh %u complete", (unsigned)display->refresh_log_count);
    }
    if (display->shadow != NULL) {
        memcpy(display->shadow, source, display->buffer_size);
    }
    display->shadow_valid = true;
    display->partial_refresh_active = partial;
    display->partial_refresh_count = full ? 0 :
        (uint8_t)(display->partial_refresh_count + 1U);
    return ESP_OK;
}


static esp_err_t pulse_powerdown(epd_ssd1677_t *d)
{
    if (!d->analog_on) return ESP_OK;
    TRY(ssd1677_cmd_data(d, 0x22, (uint8_t[]){0x03}, 1));
    TRY(ssd1677_cmd(d, 0x20));
    TRY(ssd1677_wait_ready(d));
    d->analog_on = false;
    return ESP_OK;
}

static esp_err_t load_fastwave(epd_ssd1677_t *d)
{
    if (d->fastwave_loaded) return ESP_OK;
    uint8_t lut[SOLAR_OS_SSD1677_LUT_BYTES];
    if (!solar_os_ssd1677_waveform_build(lut, d->pulse_tuning.pulse_frames,
                                         d->pulse_tuning.waveform_fr, d->invert))
        return ESP_ERR_INVALID_ARG;
    TRY(ssd1677_cmd_data(d, 0x22, (uint8_t[]){0x80}, 1));
    TRY(ssd1677_cmd(d, 0x20));
    TRY(ssd1677_wait_ready(d));
    TRY(ssd1677_cmd_data(d, 0x32, lut, sizeof(lut)));
    /* Nominal vendor rails and VCOM; no OTP writes or source overdrive. */
    TRY(ssd1677_cmd_data(d, 0x03, (uint8_t[]){0x17}, 1));
    TRY(ssd1677_cmd_data(d, 0x04, (uint8_t[]){0x41, 0xa8, 0x32}, 3));
    TRY(ssd1677_cmd_data(d, 0x2c, (uint8_t[]){0x30}, 1));
    TRY(ssd1677_cmd_data(d, 0x3c, (uint8_t[]){0x80}, 1));
    TRY(ssd1677_cmd_data(d, 0x37, (uint8_t[10]){0}, 10));
    d->fastwave_loaded = true;
    return ESP_OK;
}

static void free_budget(epd_ssd1677_t *d)
{
    d->pulse_window_valid = false;
    heap_caps_free(d->pixel_budget.active);
    heap_caps_free(d->pixel_budget.drive_history);
    heap_caps_free(d->pixel_budget.pixels);
    memset(&d->pixel_budget, 0, sizeof(d->pixel_budget));
    heap_caps_free(d->pulse_planes);
    d->pulse_planes = NULL;
}

static esp_err_t allocate_budget(epd_ssd1677_t *d)
{
    if (d->pixel_budget.pixels) return ESP_OK;
    if (!d->shadow) return ESP_ERR_NO_MEM;
    const size_t count = SSD1677_WIDTH * SSD1677_HEIGHT;
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *pixels = heap_caps_malloc(count, caps);
    uint32_t *active = heap_caps_malloc(count * sizeof(*active), caps);
    uint16_t *history = heap_caps_malloc(count * sizeof(*history), caps);
    d->pulse_planes = heap_caps_malloc(SSD1677_BUFFER_BYTES * 2U, caps);
    if (!pixels || !active || !history || !d->pulse_planes) {
        heap_caps_free(pixels); heap_caps_free(active); heap_caps_free(history);
        heap_caps_free(d->pulse_planes); d->pulse_planes = NULL;
        return ESP_ERR_NO_MEM;
    }
    TRY(epd_pixel_budget_init(&d->pixel_budget, pixels, count));
    TRY(epd_pixel_budget_set_active_storage(&d->pixel_budget, active, count));
    TRY(epd_pixel_budget_set_drive_history(&d->pixel_budget, history, count));
    d->pulse_needs_cleanup = true;
    return ESP_OK;
}

static bool pixel_white(const uint8_t *source, size_t p)
{
    const size_t x = p % SSD1677_WIDTH, y = p / SSD1677_WIDTH;
    return (source[(y / 8U) * SSD1677_WIDTH + x] & (1U << (y % 8U))) != 0;
}

static bool cleanup_due(const epd_ssd1677_t *d)
{
    const epd_pulse_tuning_t *t = &d->pulse_tuning;
    return d->pulse_updates >= t->cleanup_every * (t->cleanup_adaptive ? 4U : 1U) ||
        (t->cleanup_adaptive && d->pixel_budget.peak_drive_cycles >= t->cleanup_every * t->drive_cycles);
}

static esp_err_t pulse_prepare(epd_ssd1677_t *d, const uint8_t *source,
                                bool fresh, bool *baseline)
{
    *baseline = false;
    TRY(allocate_budget(d));
    if (!d->controller_ready || !d->shadow_valid || d->pulse_needs_cleanup || cleanup_due(d)) {
        TRY(ssd1677_refresh_image(d, source, true));
        epd_pixel_budget_reset(&d->pixel_budget);
        for (size_t p = 0; p < d->pixel_budget.count; ++p)
            epd_pixel_budget_seed(&d->pixel_budget, p, pixel_white(source, p));
        d->pulse_needs_cleanup = false;
        d->pulse_mask_dirty = true;
        d->pulse_window_valid = false;
        d->pulse_updates = 0;
        d->pulse_last_frame_start = 0;
        *baseline = true;
        return ESP_OK;
    }
    if (fresh && memcmp(source, d->shadow, SSD1677_BUFFER_BYTES) != 0) {
        if (!d->scan_async && d->pulse_last_frame_start) {
            const int64_t due = d->pulse_last_frame_start + (int64_t)d->scan_period_ms * 1000;
            while (esp_timer_get_time() < due) delay_ms(1);
        }
        d->pulse_last_frame_start = esp_timer_get_time();
        for (size_t i = 0; i < SSD1677_BUFFER_BYTES; ++i) {
            const uint8_t changed = source[i] ^ d->shadow[i];
            if (!changed) continue;
            const size_t x = i % SSD1677_WIDTH, y = i / SSD1677_WIDTH * 8U;
            for (unsigned bit = 0; bit < 8U; bit++) {
                if (changed & (1U << bit)) {
                    const size_t p = (y + bit) * SSD1677_WIDTH + x;
                    epd_pixel_budget_target(&d->pixel_budget, p,
                        (source[i] & (1U << bit)) != 0, d->pulse_tuning.drive_cycles);
                }
            }
        }
        memcpy(d->shadow, source, SSD1677_BUFFER_BYTES);
        ++d->pulse_updates; /* changed targets, never physical settling scans */
        d->pulse_mask_dirty = true;
    }
    return ESP_OK;
}

static esp_err_t pulse_cycle(epd_ssd1677_t *d)
{
    TRY(load_fastwave(d));
    if (d->pulse_mask_dirty || d->pulse_mask_pending != d->pixel_budget.pending) {
        ssd1677_window_t active = {SSD1677_PANEL_ROW_BYTES - 1U, 0, SSD1677_HEIGHT - 1U, 0};
        bool changed = false;
        uint8_t *black = d->pulse_planes, *white = black + SSD1677_BUFFER_BYTES;
        memset(d->pulse_planes, 0, SSD1677_BUFFER_BYTES * 2U);
        for (size_t i = 0; i < d->pixel_budget.active_count; ++i) {
            const size_t p = d->pixel_budget.active[i];
            const epd_pixel_drive_t drive = epd_pixel_budget_drive(&d->pixel_budget, p);
            if (drive == EPD_PIXEL_HOLD) continue;
            const size_t x = p % SSD1677_WIDTH, y = p / SSD1677_WIDTH;
            const unsigned byte = x / 8U;
            if (byte < active.x_start_byte) active.x_start_byte = byte;
            if (byte > active.x_end_byte) active.x_end_byte = byte;
            if (y < active.y_start) active.y_start = y;
            if (y > active.y_end) active.y_end = y;
            changed = true;
            const size_t offset = (y / 8U) * SSD1677_WIDTH + x;
            (drive == EPD_PIXEL_DRIVE_BLACK ? black : white)[offset] |= (uint8_t)(1U << (y % 8U));
        }
        if (!changed) return ESP_ERR_INVALID_STATE;
        ssd1677_window_t window = {0, SSD1677_PANEL_ROW_BYTES - 1U, 0, SSD1677_HEIGHT - 1U};
        if (d->pulse_window_valid) {
            window = active;
            /* Clear pixels from the preceding mask as they finish settling. */
            if (d->pulse_x_start < window.x_start_byte) window.x_start_byte = d->pulse_x_start;
            if (d->pulse_x_end > window.x_end_byte) window.x_end_byte = d->pulse_x_end;
            if (d->pulse_y_start < window.y_start) window.y_start = d->pulse_y_start;
            if (d->pulse_y_end > window.y_end) window.y_end = d->pulse_y_end;
        }
        TRY(ssd1677_cmd_data(d, 0x21, (uint8_t[]){0}, 1));
        TRY(ssd1677_write_plane(d, 0x26, black, &window, false));
        TRY(ssd1677_write_plane(d, 0x24, white, &window, false));
        d->pulse_x_start = active.x_start_byte;
        d->pulse_x_end = active.x_end_byte;
        d->pulse_y_start = active.y_start;
        d->pulse_y_end = active.y_end;
        d->pulse_window_valid = true;
        d->pulse_mask_dirty = false;
        d->pulse_mask_pending = d->pixel_budget.pending;
    }
    /* Mode 2 with the MCU LUT: omit temperature/LUT reload stages. */
    const uint8_t update = d->pulse_power_hold ? (d->analog_on ? 0x0c : 0xcc) : 0xcf;
    TRY(ssd1677_cmd_data(d, 0x22, &update, 1));
    TRY(ssd1677_cmd(d, 0x20));
    d->analog_on = d->pulse_power_hold;
    const int64_t deadline = esp_timer_get_time() + 50000;
    while (gpio_get_level(d->busy_pin) != d->busy_level) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(1);
    }
    TRY(ssd1677_wait_ready(d));
    epd_pixel_budget_advance(&d->pixel_budget);
    return ESP_OK;
}

static void pulse_failed(epd_ssd1677_t *d)
{
    scan_disable(d);
    d->refresh_mode = EPD_SSD1677_REFRESH_AUTO;
    d->controller_ready = d->shadow_valid = false;
    d->pulse_needs_cleanup = true;
}

static esp_err_t scan_process(void *context, const uint8_t *source,
                              bool fresh, bool *pending, bool *scanned)
{
    epd_ssd1677_t *d = context;
    *pending = *scanned = false;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (__atomic_load_n(&d->scan_submit_ready, __ATOMIC_ACQUIRE) && !d->suspended &&
        d->refresh_mode == EPD_SSD1677_REFRESH_PULSE && d->scan_async) {
        bool baseline;
        err = pulse_prepare(d, source, fresh, &baseline);
        if (err == ESP_OK && !baseline && d->pixel_budget.pending) {
            err = pulse_cycle(d);
            *scanned = err == ESP_OK;
        }
        if (err == ESP_OK && !d->pixel_budget.pending) err = pulse_powerdown(d);
        if (err != ESP_OK) {
            pulse_failed(d);
            /* Recover the newest accepted image even if the app stops painting. */
            xSemaphoreTake(d->scan_worker.lock, portMAX_DELAY);
            memcpy(d->scan_worker.snapshot, d->scan_worker.latest, SSD1677_BUFFER_BYTES);
            xSemaphoreGive(d->scan_worker.lock);
            if (ssd1677_refresh_image(d, d->scan_worker.snapshot, true) != ESP_OK)
                d->controller_ready = d->shadow_valid = false;
        } else *pending = d->pixel_budget.pending != 0 || cleanup_due(d);
        d->last_error = err;
    }
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t refresh_frame(epd_ssd1677_t *d)
{
    if (d->refresh_mode != EPD_SSD1677_REFRESH_PULSE)
        return ssd1677_refresh_image(d, d->buffer, false);
    bool baseline;
    TRY(pulse_prepare(d, d->buffer, true, &baseline));
    if (!baseline) while (d->pixel_budget.pending) TRY(pulse_cycle(d));
    TRY(pulse_powerdown(d));
    if (cleanup_due(d)) TRY(pulse_prepare(d, d->buffer, false, &baseline));
    return ESP_OK;
}

esp_err_t epd_ssd1677_present_power_state(epd_ssd1677_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer || !d->lock)
        return ESP_ERR_INVALID_STATE;
    scan_disable(d);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->power_state_present = true;
    /* Join the current scan, then discard any target it re-enabled. A full
     * native refresh must finish before suspend or system power-off. */
    scan_disable(d);
    const esp_err_t err = d->suspended ? ESP_ERR_INVALID_STATE :
        ssd1677_refresh_image(d, d->buffer, true);
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    d->last_error = err;
    xSemaphoreGive(d->lock);
    return err;
}

static esp_err_t ssd1677_refresh(epd_ssd1677_t *d)
{
    if (!d) return ESP_ERR_INVALID_STATE;
    if (__atomic_load_n(&d->scan_submit_ready, __ATOMIC_ACQUIRE) &&
        epd_scan_worker_submit(&d->scan_worker, d->buffer) == ESP_OK) return ESP_OK;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    if (d->suspended || !d->spi || !d->buffer || !d->line_buffer) {
        if (d->lock) xSemaphoreGive(d->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (d->relax.storage) {
        if (d->lock) xSemaphoreGive(d->lock);
        return ESP_OK;
    }
    esp_err_t err = refresh_frame(d);
    if (err != ESP_OK) {
        if (d->refresh_mode == EPD_SSD1677_REFRESH_PULSE) pulse_failed(d);
        else d->controller_ready = d->shadow_valid = false;
    }
    d->last_error = err;
    if (err == ESP_OK) scan_accepting(d);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

esp_err_t epd_ssd1677_present_mono_xbm(epd_ssd1677_t *d,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool palette_inverted)
{
    if (!d || !d->buffer || !bitmap || !width || !height ||
        stride < ((size_t)width + 7U) / 8U || height > SIZE_MAX / stride ||
        bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(&d->u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(&d->u8g2)) return ESP_ERR_INVALID_ARG;
    const u8g2_cb_t *rotation = d->u8g2.cb;
    if (rotation != U8G2_R0 && rotation != U8G2_R1 &&
        rotation != U8G2_R2 && rotation != U8G2_R3) return ESP_ERR_NOT_SUPPORTED;
    if (!d->powered || d->suspended || d->power_state_present || d->relax.storage ||
        !d->spi || !d->line_buffer || d->buffer_size < SSD1677_BUFFER_BYTES)
        return ESP_ERR_INVALID_STATE;
    /* Compose without the panel lock; async scanning uses an immutable snapshot. */
    memset(d->buffer, palette_inverted ? 0 : 0xff, SSD1677_BUFFER_BYTES);
    for (unsigned row = 0; row < height; row++) {
        const uint8_t *source = bitmap + (size_t)row * stride;
        for (unsigned column = 0; column < width; column++) {
            const unsigned lx = x + column, ly = y + row;
            unsigned nx = lx, ny = ly;
            if (rotation == U8G2_R1) { nx = SSD1677_WIDTH - 1U - ly; ny = lx; }
            else if (rotation == U8G2_R2) {
                nx = SSD1677_WIDTH - 1U - lx; ny = SSD1677_HEIGHT - 1U - ly;
            } else if (rotation == U8G2_R3) { nx = ly; ny = SSD1677_HEIGHT - 1U - lx; }
            uint8_t *destination = d->buffer + (ny / 8U) * SSD1677_WIDTH + nx;
            const uint8_t mask = (uint8_t)(1U << (ny & 7U));
            const bool black = (source[column / 8U] & (1U << (column & 7U))) != 0;
            if (black == palette_inverted) *destination |= mask;
            else *destination &= (uint8_t)~mask;
        }
    }
    return ssd1677_refresh(d);
}

static esp_err_t ssd1677_sleep(epd_ssd1677_t *display)
{
    display->suspended = true;
    scan_disable(display);
    TRY(pulse_powerdown(display));
    if (display->controller_ready) {
        static const uint8_t sleep_mode[] = {0x01};
        ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x10,
                                             sleep_mode,
                                             sizeof(sleep_mode)),
                            TAG, "deep sleep failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        (void)gpio_set_level((gpio_num_t)display->reset_pin, 0);
        (void)gpio_set_level((gpio_num_t)display->dc_pin, 0);
    }
    display->controller_ready = false;
    display->shadow_valid = false;
    display->partial_refresh_active = false;
    display->partial_refresh_count = 0;
    return ssd1677_set_power(display, false);
}

static uint8_t ssd1677_u8x8_byte_cb(u8x8_t *u8x8,
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

static uint8_t ssd1677_u8x8_display_cb(u8x8_t *u8x8,
                                       uint8_t message,
                                       uint8_t arg_int,
                                       void *arg_ptr)
{
    (void)arg_ptr;
    epd_ssd1677_t *display = ssd1677_from_u8x8(u8x8);
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &ssd1677_display_info);
        return 1;
    }
    if (display == NULL) {
        return 0;
    }
    if (message == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    esp_err_t err = ESP_OK;
    if (message == U8X8_MSG_DISPLAY_REFRESH) {
        err = ssd1677_refresh(display);
        display->last_error = err;
        return err == ESP_OK;
    }
    if (display->lock) xSemaphoreTake(display->lock, portMAX_DELAY);
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = ssd1677_controller_init(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int) err = ssd1677_sleep(display);
        else {
            display->suspended = display->power_state_present = false;
            err = display->controller_ready ? ESP_OK : ssd1677_controller_init(display);
            if (err == ESP_OK) scan_accepting(display);
        }
        break;
    default:
        if (display->lock) xSemaphoreGive(display->lock);
        return 0;
    }
    if (display->lock) xSemaphoreGive(display->lock);
    display->last_error = err;
    return err == ESP_OK ? 1 : 0;
}

static bool ssd1677_pin_valid(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

static bool ssd1677_config_valid(const epd_ssd1677_config_t *config)
{
    if (config == NULL || !ssd1677_pin_valid(config->cs_pin) ||
        !ssd1677_pin_valid(config->dc_pin) || !ssd1677_pin_valid(config->reset_pin) ||
        !ssd1677_pin_valid(config->busy_pin) || config->spi_clock_hz <= 0 ||
        config->rotation == NULL || config->panel > EPD_SSD1677_PANEL_STICKY ||
        (config->busy_level != 0 && config->busy_level != 1) ||
        (config->power_active_level != 0 && config->power_active_level != 1)) {
        return false;
    }
    if ((config->spi_bus == NULL || config->spi_bus[0] == '\0') &&
        (!ssd1677_pin_valid(config->sclk_pin) || !ssd1677_pin_valid(config->mosi_pin) ||
         (config->spi_host != SPI2_HOST && config->spi_host != SPI3_HOST))) {
        return false;
    }
    const int pins[] = {
        config->cs_pin, config->dc_pin, config->reset_pin,
        config->busy_pin, config->power_pin,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] < 0) continue;
        if (!ssd1677_pin_valid(pins[i])) return false;
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); j++) {
            if (pins[i] == pins[j]) return false;
        }
    }
    return true;
}

esp_err_t epd_ssd1677_init(epd_ssd1677_t *display,
                           const epd_ssd1677_config_t *config)
{
    if (display == NULL || !ssd1677_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(display, 0, sizeof(*display));
    display->last_error = ESP_OK;
    display->refresh_mode = EPD_SSD1677_REFRESH_AUTO;
    display->panel = config->panel;
    display->lock = xSemaphoreCreateMutexStatic(&display->lock_storage);
    display->mode_lock = xSemaphoreCreateMutexStatic(&display->mode_lock_storage);
    if (!display->lock || !display->mode_lock) return ESP_ERR_NO_MEM;
    display->pulse_tuning = (epd_pulse_tuning_t){
        .drive_cycles = config->panel == EPD_SSD1677_PANEL_STICKY ? 5 : 3, .pulse_frames=8,
        .waveform_fr=8, .cleanup_every=64, .cleanup_adaptive=true};
    display->scan_async = true;
    display->scan_period_ms = 100;
    display->pulse_power_hold = true;
    display->pulse_spi_clock_hz = config->spi_clock_hz;
    display->dc_pin = config->dc_pin;
    display->reset_pin = config->reset_pin;
    display->busy_pin = config->busy_pin;
    display->power_pin = config->power_pin;
    display->busy_level = config->busy_level;
    display->power_active_level = config->power_active_level;
    display->set_power = config->set_power;
    display->power_context = config->power_context;
    display->spi_host = config->spi_host;
    ESP_RETURN_ON_ERROR(ssd1677_configure_pins(display), TAG, "control pin config failed");

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = config->spi_clock_hz,
        .mode = 0,
        .spics_io_num = config->cs_pin,
        .queue_size = 1,
    };
    esp_err_t ret;
    if (config->spi_bus != NULL && config->spi_bus[0] != '\0') {
        ret = solar_os_bus_spi_add_device(config->spi_bus, &device_config, &display->spi);
    } else {
        const spi_bus_config_t bus_config = {
            .mosi_io_num = config->mosi_pin,
            .miso_io_num = GPIO_NUM_NC,
            .sclk_io_num = config->sclk_pin,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = SSD1677_PANEL_ROW_BYTES,
        };
        ret = spi_bus_initialize(config->spi_host, &bus_config, SPI_DMA_CH_AUTO);
        if (ret == ESP_OK) {
            display->bus_initialized = true;
            ret = spi_bus_add_device(config->spi_host, &device_config, &display->spi);
        }
    }
    if (ret != ESP_OK) {
        epd_ssd1677_deinit(display);
        return ret;
    }

    display->line_buffer_size = SSD1677_PANEL_ROW_BYTES;
    display->line_buffer = heap_caps_malloc(display->line_buffer_size,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    display->buffer_size = SSD1677_BUFFER_BYTES;
    display->buffer = heap_caps_calloc(1, display->buffer_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->buffer == NULL)
        display->buffer = heap_caps_calloc(1, display->buffer_size, MALLOC_CAP_8BIT);
    display->shadow_size = display->buffer_size;
    display->shadow = heap_caps_malloc(display->shadow_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->shadow == NULL) {
        display->shadow = heap_caps_malloc(display->shadow_size, MALLOC_CAP_8BIT);
    }
    if (display->line_buffer == NULL || display->buffer == NULL) {
        epd_ssd1677_deinit(display);
        return ESP_ERR_NO_MEM;
    }
    if (display->shadow == NULL) {
        ESP_LOGW(TAG, "display shadow allocation failed; partial refresh disabled");
        display->shadow_size = 0;
    }

    u8g2_SetupDisplay(&display->u8g2,
                      ssd1677_u8x8_display_cb,
                      u8x8_dummy_cb,
                      ssd1677_u8x8_byte_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     SSD1677_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     config->rotation);
    u8g2_InitDisplay(&display->u8g2);
    ret = display->last_error;
    if (ret == ESP_OK) {
        u8g2_SetPowerSave(&display->u8g2, 0);
        ret = display->last_error;
    }
    if (ret != ESP_OK) {
        epd_ssd1677_deinit(display);
    } else if (display->panel == EPD_SSD1677_PANEL_STICKY) {
        const esp_err_t pulse = epd_ssd1677_set_controller_mode(display, "refresh=pulse");
        if (pulse != ESP_OK)
            ESP_LOGW(TAG, "startup pulse unavailable err=0x%x; retaining refresh=auto", (unsigned)pulse);
    }
    return ret;
}

esp_err_t epd_ssd1677_resume(epd_ssd1677_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    scan_disable(display);
    if (display->lock) xSemaphoreTake(display->lock, portMAX_DELAY);
    display->suspended = display->power_state_present = false;
    esp_err_t err = ssd1677_configure_pins(display);
    display->shadow_valid = false;
    if (err == ESP_OK) err = ssd1677_controller_init(display);
    display->last_error = err;
    if (err == ESP_OK) scan_accepting(display);
    if (display->lock) xSemaphoreGive(display->lock);
    return err;
}

void epd_ssd1677_deinit(epd_ssd1677_t *display)
{
    if (display == NULL) return;
    scan_disable(display);
    epd_scan_worker_deinit(&display->scan_worker);
    free_budget(display);
    epd_relax_dispose(&display->relax);
    if (display->spi != NULL && display->powered) (void)ssd1677_sleep(display);
    if (display->spi != NULL) {
        (void)spi_bus_remove_device(display->spi);
        display->spi = NULL;
    }
    if (display->bus_initialized) {
        (void)spi_bus_free(display->spi_host);
        display->bus_initialized = false;
    }
    heap_caps_free(display->line_buffer);
    heap_caps_free(display->buffer);
    heap_caps_free(display->shadow);
    display->line_buffer = NULL;
    display->buffer = NULL;
    display->shadow = NULL;
    if (display->dc_pin >= 0) (void)gpio_reset_pin((gpio_num_t)display->dc_pin);
    if (display->reset_pin >= 0) (void)gpio_reset_pin((gpio_num_t)display->reset_pin);
    if (display->busy_pin >= 0) (void)gpio_reset_pin((gpio_num_t)display->busy_pin);
    if (display->power_pin >= 0) (void)gpio_reset_pin((gpio_num_t)display->power_pin);
    display->buffer_size = 0;
    display->shadow_size = 0;
    display->line_buffer_size = 0;
    display->controller_ready = false;
    display->shadow_valid = false;
}

u8g2_t *epd_ssd1677_get_u8g2(epd_ssd1677_t *display)
{
    return display == NULL ? NULL : &display->u8g2;
}

const char *epd_ssd1677_controller_mode(const epd_ssd1677_t *d)
{
    if (!d) return NULL;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    const char *refresh = d->refresh_mode == EPD_SSD1677_REFRESH_PARTIAL ? "partial" :
        d->refresh_mode == EPD_SSD1677_REFRESH_FULL ? "full" :
        d->refresh_mode == EPD_SSD1677_REFRESH_PULSE ? "pulse" : "auto";
    /* Same per-instance description pattern as the other e-paper drivers. */
    char *description = ((epd_ssd1677_t *)d)->mode_description;
    const epd_scan_stats_t stats = epd_scan_worker_stats(&((epd_ssd1677_t *)d)->scan_worker);
    snprintf(description, sizeof(d->mode_description),
             "refresh=%s waveform=%s drive-cycles=%u pulse-frames=%u waveform-fr=%u "
             "cleanup-every=%u cleanup-policy=%s cleanup-peak-cycles=%u cleanup-limit-cycles=%u cleanup-age=%u "
             "pulse-scan=%s pulse-period=%u pulse-power=%s pulse-spi-khz=%u invert=%s "
             "scan-last-us=%lld scan-interval-us=%lld scan-count=%u frame-count=%u frame-interval-us=%lld "
             "scan-submitted=%u scan-coalesced=%u pending=%u",
             refresh, d->refresh_mode == EPD_SSD1677_REFRESH_PULSE ? "fastwave" : "otp",
             d->pulse_tuning.drive_cycles, d->pulse_tuning.pulse_frames, d->pulse_tuning.waveform_fr,
             d->pulse_tuning.cleanup_every, d->pulse_tuning.cleanup_adaptive ? "adaptive" : "fixed",
             (unsigned)d->pixel_budget.peak_drive_cycles,
             d->pulse_tuning.cleanup_every * d->pulse_tuning.drive_cycles, (unsigned)d->pulse_updates,
             d->scan_async ? "async" : "sync", d->scan_period_ms,
             d->pulse_power_hold ? "hold" : "cycle",
             (unsigned)(d->pulse_spi_clock_hz / 1000), d->invert ? "true" : "false",
             (long long)stats.last_scan_us, (long long)stats.last_interval_us, (unsigned)stats.scans,
             (unsigned)stats.frames, (long long)stats.last_frame_interval_us,
             (unsigned)stats.submitted, (unsigned)stats.coalesced, (unsigned)d->pixel_budget.pending);
    if (d->lock) xSemaphoreGive(d->lock);
    return description;
}

const char *epd_ssd1677_controller_mode_values(const epd_ssd1677_t *d)
{
    (void)d;
    return "refresh=<auto,fast,partial,full,pulse> waveform=<otp,fastwave> "
           "drive-cycles=<1..32> pulse-frames=<1..32> waveform-fr=<1..8> "
           "cleanup-every=<1..128> cleanup-policy=<adaptive,fixed> "
           "pulse-scan=<sync,async> pulse-period=<67..1000> pulse-power=<cycle,hold> "
           "pulse-spi-khz=<100..20000> invert=<false,true>";
}

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase,
                               const uint8_t *image)
{
    epd_ssd1677_t *d = context;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        scan_disable(d);
        esp_err_t err = pulse_powerdown(d);
        if (err == ESP_OK) err = ssd1677_controller_init(d);
        if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
        return err;
    }
    const esp_err_t err = !d->powered || d->suspended ? ESP_ERR_INVALID_STATE : ssd1677_refresh_image(d, image, true);
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    if (phase == SOLAR_OS_DISPLAY_RELAX_END) {
        if (err == ESP_OK && d->pixel_budget.pixels) {
            epd_pixel_budget_reset(&d->pixel_budget);
            for (size_t p = 0; p < d->pixel_budget.count; ++p)
                epd_pixel_budget_seed(&d->pixel_budget, p, pixel_white(image, p));
        }
        d->pulse_updates = 0;
        d->pulse_needs_cleanup = err != ESP_OK;
        d->pulse_mask_dirty = true;
        d->pulse_last_frame_start = 0;
    }
    return err;
}

esp_err_t epd_ssd1677_relax(epd_ssd1677_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END) return ESP_ERR_INVALID_ARG;
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = phase == SOLAR_OS_DISPLAY_RELAX_BEGIN && (!d->powered || d->suspended) ? ESP_ERR_INVALID_STATE :
        epd_relax_apply(&d->relax, d->buffer, SSD1677_BUFFER_BYTES, phase, relax_native, d);
    d->last_error = err;
    scan_accepting(d);
    if (d->lock) xSemaphoreGive(d->lock);
    return err;
}

static bool parse_tuning(const char *text, unsigned maximum, unsigned *value)
{
    if (*text < '0' || *text > '9') return false;
    char *end;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (*end || parsed < 1 || parsed > maximum) return false;
    *value = (unsigned)parsed;
    return true;
}

static esp_err_t set_controller_mode(epd_ssd1677_t *d, const char *mode)
{
    if (d->relax.storage) return ESP_ERR_INVALID_STATE;
    const epd_ssd1677_refresh_mode_t before = d->refresh_mode;
    bool restart = false;
    if (strcmp(mode, "refresh=auto") == 0 || strcmp(mode, "waveform=otp") == 0)
        d->refresh_mode = EPD_SSD1677_REFRESH_AUTO;
    else if (strcmp(mode, "refresh=partial") == 0 || strcmp(mode, "refresh=fast") == 0)
        d->refresh_mode = EPD_SSD1677_REFRESH_PARTIAL;
    else if (strcmp(mode, "refresh=full") == 0) d->refresh_mode = EPD_SSD1677_REFRESH_FULL;
    else if (strcmp(mode, "refresh=pulse") == 0 || strcmp(mode, "waveform=fastwave") == 0) {
        const bool allocated = d->pixel_budget.pixels != NULL;
        TRY(allocate_budget(d));
        if (d->scan_async && !d->scan_worker.task) {
            const esp_err_t err = epd_scan_worker_init(&d->scan_worker, SSD1677_BUFFER_BYTES, scan_process, d);
            if (err != ESP_OK) {
                if (!allocated) free_budget(d);
                return err;
            }
            TRY(epd_scan_worker_set_period(&d->scan_worker, d->scan_period_ms));
        }
        d->refresh_mode = EPD_SSD1677_REFRESH_PULSE;
        restart = before != d->refresh_mode;
    } else if (strcmp(mode, "pulse-scan=async") == 0) {
        if (!d->scan_worker.task) {
            TRY(epd_scan_worker_init(&d->scan_worker, SSD1677_BUFFER_BYTES, scan_process, d));
            TRY(epd_scan_worker_set_period(&d->scan_worker, d->scan_period_ms));
        }
        restart = !d->scan_async;
        d->scan_async = true;
    } else if (strcmp(mode, "pulse-scan=sync") == 0) {
        restart = d->scan_async;
        d->scan_async = false;
    } else if (strcmp(mode, "pulse-power=hold") == 0 || strcmp(mode, "pulse-power=cycle") == 0) {
        d->pulse_power_hold = strcmp(mode, "pulse-power=hold") == 0;
        restart = true;
    } else if (strncmp(mode, "pulse-period=", 13) == 0 || strncmp(mode, "pulse-spi-khz=", 14) == 0) {
        const bool spi = mode[6] == 's';
        unsigned value;
        if (!parse_tuning(mode + (spi ? 14 : 13), spi ? 20000 : 1000, &value) ||
            value < (spi ? 100U : 67U)) return ESP_ERR_INVALID_ARG;
        if (spi) d->pulse_spi_clock_hz = value * 1000U;
        else {
            if (d->scan_worker.task) TRY(epd_scan_worker_set_period(&d->scan_worker, value));
            d->scan_period_ms = (uint16_t)value;
        }
    } else if (strcmp(mode, "invert=true") == 0 || strcmp(mode, "invert=false") == 0) {
        d->invert = strcmp(mode, "invert=true") == 0;
        restart = true;
    } else {
        const epd_pulse_tuning_t previous = d->pulse_tuning;
        TRY(epd_pulse_tuning_set(&d->pulse_tuning, mode));
        if (d->pulse_tuning.waveform_fr > 8) {
            d->pulse_tuning = previous;
            return ESP_ERR_INVALID_ARG;
        }
        restart = previous.pulse_frames != d->pulse_tuning.pulse_frames ||
            previous.waveform_fr != d->pulse_tuning.waveform_fr;
    }
    if (restart || (before == EPD_SSD1677_REFRESH_PULSE && before != d->refresh_mode))
        d->pulse_needs_cleanup = true;
    if (d->refresh_mode != EPD_SSD1677_REFRESH_PULSE && d->pixel_budget.pixels) {
        scan_disable(d);
        TRY(pulse_powerdown(d));
        d->controller_ready = d->shadow_valid = false;
        free_budget(d);
    }
    scan_accepting(d);
    if (d->refresh_mode != EPD_SSD1677_REFRESH_PULSE || !d->scan_async)
        TRY(pulse_powerdown(d));
    return ESP_OK;
}

esp_err_t epd_ssd1677_set_controller_mode(epd_ssd1677_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    if (d->mode_lock) xSemaphoreTake(d->mode_lock, portMAX_DELAY);
    if (d->lock) xSemaphoreTake(d->lock, portMAX_DELAY);
    const esp_err_t err = set_controller_mode(d, mode);
    const bool stop_worker = err == ESP_OK &&
        (d->refresh_mode != EPD_SSD1677_REFRESH_PULSE || !d->scan_async);
    if (stop_worker) scan_disable(d);
    if (d->lock) xSemaphoreGive(d->lock);
    /* Joining under the panel lock would deadlock an in-flight scan. */
    if (stop_worker) epd_scan_worker_deinit(&d->scan_worker);
    if (d->mode_lock) xSemaphoreGive(d->mode_lock);
    return err;
}
