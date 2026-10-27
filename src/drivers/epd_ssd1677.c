#include "epd_ssd1677.h"

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
    vTaskDelay(pdMS_TO_TICKS(100));
    while (gpio_get_level((gpio_num_t)display->busy_pin) == display->busy_level) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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
                                uint16_t y)
{
    const uint8_t row_bit = (uint8_t)(1U << (y & 7U));
    const uint8_t *tile_row = source +
        (size_t)(y >> 3) * SSD1677_BUFFER_ROW_BYTES;
    for (size_t byte = 0; byte < SSD1677_PANEL_ROW_BYTES; byte++) {
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
        ssd1677_convert_row(display, source, y);
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

static esp_err_t ssd1677_activate(epd_ssd1677_t *display)
{
    /* SSD1677 differential update: 0x24 is new, 0x26 is the baseline. */
    static const uint8_t control[] = {0x00};
    static const uint8_t border[] = {0xc0};
    static const uint8_t update_mode[] = {0xfc};
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x21, control, sizeof(control)),
                        TAG, "update control failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x3c, border, sizeof(border)),
                        TAG, "update border setup failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd_data(display, 0x22,
                                         update_mode,
                                         sizeof(update_mode)),
                        TAG, "update mode failed");
    ESP_RETURN_ON_ERROR(ssd1677_cmd(display, 0x20), TAG, "update activation failed");
    return ssd1677_wait_ready(display);
}

static esp_err_t ssd1677_refresh_image(epd_ssd1677_t *display, const uint8_t *source, bool force_full)
{
    if (!display->controller_ready) {
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
    /* A complemented baseline forces every pixel to transition on full updates. */
    ESP_RETURN_ON_ERROR(ssd1677_write_plane(display, 0x26,
                                           partial ? display->shadow : source,
                                           &window, !partial),
                        TAG, "previous frame write failed");
    esp_err_t ret = ssd1677_activate(display);
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

static esp_err_t ssd1677_refresh(epd_ssd1677_t *display)
{
    if (display->relax.storage) return ESP_OK;
    return ssd1677_refresh_image(display, display->buffer, false);
}

static esp_err_t ssd1677_sleep(epd_ssd1677_t *display)
{
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
    esp_err_t err = ESP_OK;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = ssd1677_controller_init(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        err = arg_int != 0 ? ssd1677_sleep(display) :
            (display->controller_ready ? ESP_OK : ssd1677_controller_init(display));
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        err = ssd1677_refresh(display);
        break;
    default:
        return 0;
    }
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
        config->rotation == NULL || (config->busy_level != 0 && config->busy_level != 1) ||
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
    }
    return ret;
}

esp_err_t epd_ssd1677_resume(epd_ssd1677_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ssd1677_configure_pins(display), TAG, "resume pin config failed");
    display->last_error = ESP_OK;
    display->shadow_valid = false;
    return ssd1677_controller_init(display);
}

void epd_ssd1677_deinit(epd_ssd1677_t *display)
{
    if (display == NULL) return;
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

const char *epd_ssd1677_controller_mode(const epd_ssd1677_t *display)
{
    if (display == NULL) return NULL;
    switch (display->refresh_mode) {
    case EPD_SSD1677_REFRESH_PARTIAL: return "refresh=partial";
    case EPD_SSD1677_REFRESH_FULL: return "refresh=full";
    case EPD_SSD1677_REFRESH_AUTO:
    default: return "refresh=auto";
    }
}

const char *epd_ssd1677_controller_mode_values(const epd_ssd1677_t *display)
{
    (void)display;
    return "refresh=<auto,partial,full>";
}

esp_err_t epd_ssd1677_set_controller_mode(epd_ssd1677_t *display,
                                          const char *mode)
{
    if (display == NULL || mode == NULL) return ESP_ERR_INVALID_ARG;
    if (display->relax.storage) return ESP_ERR_INVALID_STATE;
    if (strcmp(mode, "refresh=auto") == 0) {
        display->refresh_mode = EPD_SSD1677_REFRESH_AUTO;
    } else if (strcmp(mode, "refresh=partial") == 0) {
        display->refresh_mode = EPD_SSD1677_REFRESH_PARTIAL;
    } else if (strcmp(mode, "refresh=full") == 0) {
        display->refresh_mode = EPD_SSD1677_REFRESH_FULL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t relax_native(void *context, solar_os_display_relax_phase_t phase,
                               const uint8_t *image)
{
    epd_ssd1677_t *d = context;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        d->controller_ready = d->shadow_valid = false;
        return ESP_OK;
    }
    const esp_err_t err = d->powered ? ssd1677_refresh_image(d, image, true) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    return err;
}

esp_err_t epd_ssd1677_relax(epd_ssd1677_t *d, solar_os_display_relax_phase_t phase)
{
    if (!d || !d->buffer || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END) return ESP_ERR_INVALID_ARG;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN && !d->powered) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = epd_relax_apply(&d->relax, d->buffer, d->buffer_size,
                                         phase, relax_native, d);
    d->last_error = err;
    return err;
}
