#include "epd_ssd1683.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ
#error "SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ must be defined by the board profile"
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_HOST
#define SOLAR_OS_BOARD_DISPLAY_SPI_HOST SPI2_HOST
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R0
#endif

#ifndef SOLAR_OS_BOARD_LCD_BUSY_LEVEL
#define SOLAR_OS_BOARD_LCD_BUSY_LEVEL 1
#endif

#ifndef SOLAR_OS_BOARD_LCD_POWER_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_LCD_POWER_ACTIVE_LEVEL 1
#endif

#define SSD1683_WIDTH SOLAR_OS_BOARD_DISPLAY_WIDTH
#define SSD1683_HEIGHT SOLAR_OS_BOARD_DISPLAY_HEIGHT
#define SSD1683_TILE_WIDTH ((SSD1683_WIDTH + 7U) / 8U)
#define SSD1683_TILE_HEIGHT ((SSD1683_HEIGHT + 7U) / 8U)
#define SSD1683_BUFFER_ROW_BYTES (SSD1683_TILE_WIDTH * 8U)
#define SSD1683_BUFFER_BYTES (SSD1683_BUFFER_ROW_BYTES * SSD1683_TILE_HEIGHT)
#define SSD1683_PANEL_ROW_BYTES ((SSD1683_WIDTH + 7U) / 8U)
#define SSD1683_BUSY_TIMEOUT_MS 30000U
#define SSD1683_VARIANT_PROBE_MS 500U
#define SSD1683_AUTO_FULL_INTERVAL 20U

static const char *TAG = "epd_ssd1683";
static epd_ssd1683_t *active_display;

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
    .sck_clock_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
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
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 0),
                        TAG,
                        "D/C command failed");
    ESP_RETURN_ON_ERROR(ssd1683_tx_byte(display, command), TAG, "command transmit failed");
    if (length == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1),
                        TAG,
                        "D/C data failed");
    return ssd1683_tx_bytes(display, data, length);
}

static esp_err_t ssd1683_cmd(epd_ssd1683_t *display, uint8_t command)
{
    return ssd1683_cmd_data(display, command, NULL, 0);
}

static bool ssd1683_busy_cleared(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    while (gpio_get_level(SOLAR_OS_BOARD_PIN_LCD_BUSY) == SOLAR_OS_BOARD_LCD_BUSY_LEVEL) {
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static esp_err_t ssd1683_wait_ready(void)
{
    if (!ssd1683_busy_cleared(SSD1683_BUSY_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "BUSY timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t ssd1683_set_power(epd_ssd1683_t *display, bool on)
{
    const int active = SOLAR_OS_BOARD_LCD_POWER_ACTIVE_LEVEL ? 1 : 0;
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_POWER,
                                       on ? active : !active),
                        TAG,
                        "panel power failed");
    display->powered = on;
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t ssd1683_configure_pins(epd_ssd1683_t *display)
{
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << SOLAR_OS_BOARD_PIN_LCD_DC) |
                        (1ULL << SOLAR_OS_BOARD_PIN_LCD_RST) |
                        (1ULL << SOLAR_OS_BOARD_PIN_LCD_POWER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "output GPIO config failed");

    const gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << SOLAR_OS_BOARD_PIN_LCD_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_config), TAG, "BUSY GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "D/C idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1), TAG, "reset idle failed");
    display->powered = true;
    return ssd1683_set_power(display, false);
}

static void ssd1683_hardware_reset(void)
{
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t ssd1683_set_address(epd_ssd1683_t *display)
{
    const uint8_t x_bounds[] = {0, (uint8_t)((SSD1683_WIDTH - 1U) >> 3)};
    const uint8_t y_bounds[] = {
        0,
        0,
        (uint8_t)((SSD1683_HEIGHT - 1U) & 0xffU),
        (uint8_t)((SSD1683_HEIGHT - 1U) >> 8),
    };
    const uint8_t x_cursor[] = {0};
    const uint8_t y_cursor[] = {0, 0};

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

static esp_err_t ssd1683_legacy_init(epd_ssd1683_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x12), TAG, "software reset failed");
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(), TAG, "software reset wait failed");

    const uint8_t update_control[] = {0x40, 0x00};
    const uint8_t border_waveform[] = {0x05};
    const uint8_t temperature[] = {0x6e};
    const uint8_t load_temperature[] = {0x91};
    const uint8_t data_entry_mode[] = {0x03};
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
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x1a, temperature, sizeof(temperature)),
                        TAG,
                        "temperature setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x22,
                                         load_temperature,
                                         sizeof(load_temperature)),
                        TAG,
                        "temperature load failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20), TAG, "temperature activate failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(), TAG, "temperature load wait failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x11,
                                         data_entry_mode,
                                         sizeof(data_entry_mode)),
                        TAG,
                        "data entry mode failed");
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
    if (!display->powered) {
        ESP_RETURN_ON_ERROR(ssd1683_set_power(display, true), TAG, "panel power on failed");
    }

    ssd1683_hardware_reset();
    if (display->panel_variant == EPD_SSD1683_PANEL_UNKNOWN) {
        display->panel_variant = ssd1683_busy_cleared(SSD1683_VARIANT_PROBE_MS) ?
            EPD_SSD1683_PANEL_LEGACY : EPD_SSD1683_PANEL_GREEN_STICKER;
        ESP_LOGI(TAG,
                 "detected %s panel revision",
                 display->panel_variant == EPD_SSD1683_PANEL_LEGACY ?
                     "legacy SSD1683" : "green-sticker");
    }

    if (display->panel_variant == EPD_SSD1683_PANEL_LEGACY) {
        ESP_RETURN_ON_ERROR(ssd1683_wait_ready(), TAG, "panel reset wait failed");
        ESP_RETURN_ON_ERROR(ssd1683_legacy_init(display), TAG, "legacy panel init failed");
    } else {
        ESP_RETURN_ON_ERROR(ssd1683_green_init(display), TAG, "green panel init failed");
    }

    display->controller_ready = true;
    display->shadow_valid = false;
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

static esp_err_t ssd1683_trigger_update(epd_ssd1683_t *display, bool full)
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
        return ssd1683_wait_ready();
    }

    const uint8_t update_mode[] = {full ? 0xf7 : 0xc7};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x22, update_mode, sizeof(update_mode)),
                        TAG,
                        "display update mode failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20), TAG, "display update failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ssd1683_wait_ready();
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
    const bool log_refresh = display->refresh_log_count < 4U;
    if (log_refresh) {
        ESP_LOGI(TAG,
                 "panel refresh %u starting",
                 (unsigned)(display->refresh_log_count + 1U));
    }

    if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER && !first_refresh) {
        /*
         * Elecrow's revised panel requires a reset/init before every changed
         * frame.  Keeping the controller live works for the initial clear but
         * later 0x13 updates can be ignored, leaving the previous image on the
         * bistable panel.
         */
        ssd1683_hardware_reset();
        ESP_RETURN_ON_ERROR(ssd1683_green_init(display),
                            TAG,
                            "green panel refresh init failed");
    }

    const bool full = display->refresh_mode == EPD_SSD1683_REFRESH_FULL ||
        (display->refresh_mode == EPD_SSD1683_REFRESH_AUTO &&
         (!display->shadow_valid ||
          display->fast_refresh_count >= SSD1683_AUTO_FULL_INTERVAL - 1U));

    if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER) {
        if (first_refresh) {
            ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x10),
                                TAG,
                                "old frame RAM write failed");
            ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1),
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

    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "D/C data failed");
    for (uint16_t y = 0; y < SSD1683_HEIGHT; y++) {
        ssd1683_convert_row(display, display->buffer, y);
        ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                             display->line_buffer,
                                             SSD1683_PANEL_ROW_BYTES),
                            TAG,
                            "frame transmit failed");
    }

    ESP_RETURN_ON_ERROR(ssd1683_trigger_update(display, full), TAG, "panel refresh failed");
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

    epd_ssd1683_t *display = active_display;
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

esp_err_t epd_ssd1683_init(epd_ssd1683_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->last_error = ESP_OK;
    display->refresh_mode = EPD_SSD1683_REFRESH_AUTO;
    ESP_RETURN_ON_ERROR(ssd1683_configure_pins(display), TAG, "control pin config failed");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = SOLAR_OS_BOARD_PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = SOLAR_OS_BOARD_PIN_LCD_SCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SSD1683_PANEL_ROW_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SOLAR_OS_BOARD_DISPLAY_SPI_HOST,
                                           &bus_config,
                                           SPI_DMA_CH_AUTO),
                        TAG,
                        "SPI bus init failed");
    display->bus_initialized = true;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = SOLAR_OS_BOARD_PIN_LCD_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SOLAR_OS_BOARD_DISPLAY_SPI_HOST,
                                           &device_config,
                                           &display->spi),
                        TAG,
                        "SPI device add failed");

    display->line_buffer_size = SSD1683_PANEL_ROW_BYTES;
    display->line_buffer = heap_caps_malloc(display->line_buffer_size,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->line_buffer == NULL) {
        epd_ssd1683_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->buffer_size = SSD1683_BUFFER_BYTES;
    display->buffer = heap_caps_calloc(1, display->buffer_size, MALLOC_CAP_8BIT);
    if (display->buffer == NULL) {
        epd_ssd1683_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->shadow_size = SSD1683_BUFFER_BYTES;
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
                     SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    return display->last_error;
}

esp_err_t epd_ssd1683_resume(epd_ssd1683_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ssd1683_configure_pins(display), TAG, "resume pin config failed");
    active_display = display;
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
        (void)spi_bus_free(SOLAR_OS_BOARD_DISPLAY_SPI_HOST);
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
    if (active_display == display) {
        active_display = NULL;
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
