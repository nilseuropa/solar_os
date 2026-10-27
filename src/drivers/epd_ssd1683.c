#include "epd_ssd1683.h"
#include "epd_ssd1683_waveform.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_log.h"

#define SSD1683_WIDTH 400U
#define SSD1683_HEIGHT 300U
#define SSD1683_TILE_WIDTH ((SSD1683_WIDTH + 7U) / 8U)
#define SSD1683_TILE_HEIGHT ((SSD1683_HEIGHT + 7U) / 8U)
#define SSD1683_BUFFER_ROW_BYTES (SSD1683_TILE_WIDTH * 8U)
#define SSD1683_BUFFER_BYTES (SSD1683_BUFFER_ROW_BYTES * SSD1683_TILE_HEIGHT)
#define SSD1683_PANEL_ROW_BYTES ((SSD1683_WIDTH + 7U) / 8U)
#define SSD1683_DUAL_WIDTH 792U
#define SSD1683_DUAL_HEIGHT 272U
#define SSD1683_DUAL_PADDED_WIDTH 800U
#define SSD1683_DUAL_SEAM_START 396U
#define SSD1683_DUAL_SEAM_WIDTH 8U
#define SSD1683_DUAL_TILE_WIDTH ((SSD1683_DUAL_WIDTH + 7U) / 8U)
#define SSD1683_DUAL_TILE_HEIGHT ((SSD1683_DUAL_HEIGHT + 7U) / 8U)
#define SSD1683_DUAL_BUFFER_ROW_BYTES (SSD1683_DUAL_TILE_WIDTH * 8U)
#define SSD1683_DUAL_BUFFER_BYTES \
    (SSD1683_DUAL_BUFFER_ROW_BYTES * SSD1683_DUAL_TILE_HEIGHT)
#define SSD1683_DUAL_PADDED_ROW_BYTES (SSD1683_DUAL_PADDED_WIDTH / 8U)
#define SSD1683_DUAL_CONTROLLER_ROW_BYTES (SSD1683_DUAL_PADDED_ROW_BYTES / 2U)
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
/* ceil(1000 / 15): limit new Fastwave images, independently of redrives. */
#define SSD1683_FASTWAVE_MIN_FRAME_PERIOD_MS 67U

_Static_assert(SSD1683_DUAL_WIDTH + SSD1683_DUAL_SEAM_WIDTH ==
                   SSD1683_DUAL_PADDED_WIDTH,
               "dual SSD1683 transport width must include the center gap");

static const char *TAG = "epd_ssd1683";

typedef struct {
    uint8_t x_start_byte;
    uint8_t x_end_byte;
    uint16_t y_start;
    uint16_t y_end;
} ssd1683_window_t;

typedef struct {
    unsigned width, height;
    size_t plane_size;
    esp_err_t (*write_target)(epd_ssd1683_t *, const uint8_t *);
    esp_err_t (*write_mask)(epd_ssd1683_t *);
    esp_err_t (*load_waveform)(epd_ssd1683_t *);
    esp_err_t (*activate)(epd_ssd1683_t *);
    esp_err_t (*power_down)(epd_ssd1683_t *);
} ssd1683_pulse_profile_t;

static const ssd1683_pulse_profile_t *ssd1683_pulse_profile(const epd_ssd1683_t *display);
static esp_err_t ssd1683_pulse_activate(epd_ssd1683_t *display);
static esp_err_t ssd1683_pulse_power_off(epd_ssd1683_t *display);

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

static const u8x8_display_info_t ssd1683_dual_display_info = {
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
    .tile_width = SSD1683_DUAL_TILE_WIDTH,
    .tile_height = SSD1683_DUAL_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = SSD1683_DUAL_WIDTH,
    .pixel_height = SSD1683_DUAL_HEIGHT,
};

static bool ssd1683_is_dual(const epd_ssd1683_t *display)
{
    return display != NULL &&
        display->panel_variant == EPD_SSD1683_PANEL_ELECROW_579_DUAL;
}

static bool ssd1683_uses_fastwave(const epd_ssd1683_t *display)
{
    return ssd1683_is_dual(display) || (display != NULL &&
        display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER);
}

static epd_pulse_tuning_t ssd1683_default_pulse_tuning(epd_ssd1683_panel_variant_t panel)
{
    epd_pulse_tuning_t tuning = epd_pulse_default_tuning();
    if (panel == EPD_SSD1683_PANEL_ELECROW_579_DUAL) {
        /* A single short activation can complete without visibly settling ink.
         * Use the conservative preset confirmed on the 5.79-inch panel; keep
         * the common model's defaults independent of this panel's waveform.
         */
        tuning.drive_cycles = 6;
        tuning.pulse_frames = 8;
        tuning.waveform_fr = 4;
        tuning.cleanup_every = 64;
        tuning.cleanup_adaptive = true;
    } else if (panel == EPD_SSD1683_PANEL_GREEN_STICKER) {
        tuning.drive_cycles = 8;
        tuning.pulse_frames = 8;
        tuning.waveform_fr = 12;
        tuning.cleanup_every = 8;
        tuning.cleanup_adaptive = false;
    } else if (panel == EPD_SSD1683_PANEL_WAVESHARE_V2) {
        /* Initial calibration preset for the single-controller 4.2-inch panel. */
        tuning.drive_cycles = 3;
        tuning.pulse_frames = 8;
        tuning.waveform_fr = 4;
        tuning.cleanup_every = 64;
        tuning.cleanup_adaptive = true;
    }
    return tuning;
}

static void ssd1683_pulse_defaults(epd_ssd1683_t *display,
                                    const epd_ssd1683_config_t *config)
{
    const bool dual = config->panel_variant == EPD_SSD1683_PANEL_ELECROW_579_DUAL;
    const bool green = config->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER;
    display->pulse_tuning = ssd1683_default_pulse_tuning(config->panel_variant);
    display->scan_async = dual || config->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 ||
        config->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER;
    display->scan_period_ms = dual || green ? SSD1683_FASTWAVE_MIN_FRAME_PERIOD_MS : 100;
    display->pulse_power_hold = dual;
    display->pulse_ramp_fast = dual || green;
    display->pulse_spi_clock_hz = dual || green ? 20000000 : config->spi_clock_hz;
}

static esp_err_t ssd1683_trigger_update(epd_ssd1683_t *display,
                                        bool full,
                                        bool partial);

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
    const uint32_t clock = display->refresh_mode == EPD_SSD1683_REFRESH_PULSE ?
        display->pulse_spi_clock_hz : display->spi_clock_hz;
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = {value},
        .override_freq_hz = clock == display->spi_applied_hz ? 0 : clock,
    };
    const esp_err_t err = spi_device_polling_transmit(display->spi, &transaction);
    if (err == ESP_OK) display->spi_applied_hz = clock;
    return err;
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

        const uint32_t clock = display->refresh_mode == EPD_SSD1683_REFRESH_PULSE ?
            display->pulse_spi_clock_hz : display->spi_clock_hz;
        spi_transaction_t transaction = {
            .length = chunk * 8U,
            .tx_buffer = display->line_buffer,
            .override_freq_hz = clock == display->spi_applied_hz ? 0 : clock,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display->spi, &transaction),
                            TAG,
                            "SPI transmit failed");
        display->spi_applied_hz = clock;
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
    /* Automatic light sleep must not float the panel's power/reset or select
     * it accidentally while its controller is driving an update. Preserve the
     * active control configuration; the SPI bus owns its clock/data pins. */
    const int control_pins[] = {display->cs_pin, display->dc_pin,
        display->reset_pin, display->busy_pin, display->power_pin};
    for (size_t i = 0; i < sizeof(control_pins) / sizeof(control_pins[0]); ++i) {
        if (control_pins[i] >= 0) {
            ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis((gpio_num_t)control_pins[i]),
                                TAG, "control GPIO sleep retention failed");
        }
    }
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

static esp_err_t ssd1683_dual_init(epd_ssd1683_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display), TAG, "dual panel reset wait failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x12), TAG, "dual software reset failed");
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display),
                        TAG,
                        "dual software reset wait failed");

    const uint8_t internal_temperature[] = {0x80};
    const uint8_t activate_temperature[] = {0xb1};
    const uint8_t temperature[] = {0x64, 0x00};
    const uint8_t load_temperature[] = {0x91};
    const uint8_t border_waveform[] = {0x03};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x18,
                                         internal_temperature,
                                         sizeof(internal_temperature)),
                        TAG,
                        "dual temperature source failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x22,
                                         activate_temperature,
                                         sizeof(activate_temperature)),
                        TAG,
                        "dual temperature activation failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20),
                        TAG,
                        "dual temperature activation command failed");
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display),
                        TAG,
                        "dual temperature activation wait failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x1a,
                                         temperature,
                                         sizeof(temperature)),
                        TAG,
                        "dual temperature setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x22,
                                         load_temperature,
                                         sizeof(load_temperature)),
                        TAG,
                        "dual temperature load failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20),
                        TAG,
                        "dual temperature load command failed");
    ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display),
                        TAG,
                        "dual temperature load wait failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x3c,
                                         border_waveform,
                                         sizeof(border_waveform)),
                        TAG,
                        "dual border waveform failed");
    return ssd1683_wait_ready(display);
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
    if (ssd1683_is_dual(display)) {
        ESP_RETURN_ON_ERROR(ssd1683_dual_init(display), TAG, "dual panel init failed");
        display->pulse_lut_loaded = false;
        display->pulse_window_valid = false;
        display->pulse_mask_dirty = true;
        display->pulse_mask_pending = 0;
        display->pulse_analog_on = false;
        display->pulse_last_drive_start = 0;
        display->pulse_last_frame_start = 0;
        display->pulse_drive_interval = 0;
        display->pulse_needs_cleanup = false;
        display->controller_ready = true;
        display->shadow_valid = false;
        display->partial_refresh_active = false;
        display->fast_refresh_count = 0;
        return ESP_OK;
    }
    if (display->panel_variant == EPD_SSD1683_PANEL_UNKNOWN) {
        display->panel_variant = ssd1683_busy_cleared(display, SSD1683_VARIANT_PROBE_MS) ?
            EPD_SSD1683_PANEL_LEGACY : EPD_SSD1683_PANEL_GREEN_STICKER;
        ESP_LOGI(TAG,
                 "detected %s panel revision",
                 display->panel_variant == EPD_SSD1683_PANEL_LEGACY ?
                     "legacy SSD1683" : "green-sticker");
        const epd_ssd1683_config_t detected_config = {
            .panel_variant = display->panel_variant,
            .spi_clock_hz = (int)display->spi_clock_hz,
        };
        ssd1683_pulse_defaults(display, &detected_config);
    }

    /* UC8276-family BUSY_N is active low; the legacy SSD1683 is active high.
     * Revision detection above intentionally probes with the SSD1683 level. */
    if (display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER)
        display->busy_level = 0;

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
    display->pulse_lut_loaded = false;
    display->pulse_window_valid = false;
    display->pulse_mask_dirty = true;
    display->pulse_mask_pending = 0;
    display->pulse_analog_on = false;
    display->pulse_last_drive_start = 0;
    display->pulse_last_frame_start = 0;
    display->pulse_drive_interval = 0;
    display->pulse_needs_cleanup = false;
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

static uint8_t ssd1683_dual_convert_byte(const uint8_t *source,
                                         uint16_t padded_x_byte,
                                         uint16_t y)
{
    const uint8_t row_bit = (uint8_t)(1U << (y & 7U));
    const uint8_t *tile_row = source +
        (size_t)(y >> 3) * SSD1683_DUAL_BUFFER_ROW_BYTES;
    uint8_t panel_pixels = 0;

    for (uint16_t bit = 0; bit < 8U; bit++) {
        const uint16_t padded_x = padded_x_byte * 8U + bit;
        bool white = padded_x >= SSD1683_DUAL_SEAM_START &&
            padded_x < SSD1683_DUAL_SEAM_START + SSD1683_DUAL_SEAM_WIDTH;
        uint16_t x = padded_x;
        if (padded_x >= SSD1683_DUAL_SEAM_START + SSD1683_DUAL_SEAM_WIDTH) {
            x -= SSD1683_DUAL_SEAM_WIDTH;
        }
        if (!white && x < SSD1683_DUAL_WIDTH) {
            white = (tile_row[x] & row_bit) != 0;
        }
        if (white) {
            panel_pixels |= (uint8_t)(0x80U >> bit);
        }
    }
    return panel_pixels;
}

static esp_err_t ssd1683_dual_set_master_address(epd_ssd1683_t *display)
{
    static const uint8_t data_entry_mode[] = {0x05};
    static const uint8_t x_bounds[] = {0x00, 0x31};
    static const uint8_t y_bounds[] = {0x0f, 0x01, 0x00, 0x00};
    static const uint8_t x_cursor[] = {0x00};
    static const uint8_t y_cursor[] = {0x0f, 0x01};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x11,
                                         data_entry_mode,
                                         sizeof(data_entry_mode)),
                        TAG,
                        "dual master data entry mode failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x44, x_bounds, sizeof(x_bounds)),
                        TAG, "dual master X bounds failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x45, y_bounds, sizeof(y_bounds)),
                        TAG, "dual master Y bounds failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x4e, x_cursor, sizeof(x_cursor)),
                        TAG, "dual master X cursor failed");
    return ssd1683_cmd_data(display, 0x4f, y_cursor, sizeof(y_cursor));
}

static esp_err_t ssd1683_dual_set_slave_address(epd_ssd1683_t *display)
{
    static const uint8_t select_slave[] = {0x04};
    static const uint8_t x_bounds[] = {0x31, 0x00};
    static const uint8_t y_bounds[] = {0x0f, 0x01, 0x00, 0x00};
    static const uint8_t x_cursor[] = {0x31};
    static const uint8_t y_cursor[] = {0x0f, 0x01};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display,
                                         0x91,
                                         select_slave,
                                         sizeof(select_slave)),
                        TAG,
                        "dual slave select failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0xc4, x_bounds, sizeof(x_bounds)),
                        TAG, "dual slave X bounds failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0xc5, y_bounds, sizeof(y_bounds)),
                        TAG, "dual slave Y bounds failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0xce, x_cursor, sizeof(x_cursor)),
                        TAG, "dual slave X cursor failed");
    return ssd1683_cmd_data(display, 0xcf, y_cursor, sizeof(y_cursor));
}

static esp_err_t ssd1683_dual_write_half(epd_ssd1683_t *display,
                                         uint8_t command,
                                         uint16_t first_x_byte,
                                         const uint8_t *source)
{
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, command), TAG, "dual RAM write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                        TAG,
                        "dual D/C data failed");
    for (uint16_t x_byte = first_x_byte;
         x_byte < first_x_byte + SSD1683_DUAL_CONTROLLER_ROW_BYTES;
         x_byte++) {
        for (uint16_t y = 0; y < SSD1683_DUAL_HEIGHT; y++) {
            display->line_buffer[y] =
                ssd1683_dual_convert_byte(source, x_byte, y);
        }
        ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                             display->line_buffer,
                                             SSD1683_DUAL_HEIGHT),
                            TAG,
                            "dual frame transmit failed");
    }
    return ESP_OK;
}

static esp_err_t ssd1683_dual_write_constant_half(epd_ssd1683_t *display,
                                                  uint8_t command,
                                                  uint8_t value)
{
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, command),
                        TAG,
                        "dual auxiliary RAM write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                        TAG,
                        "dual auxiliary D/C data failed");
    memset(display->line_buffer, value, SSD1683_DUAL_HEIGHT);
    for (uint16_t x_byte = 0;
         x_byte < SSD1683_DUAL_CONTROLLER_ROW_BYTES;
         x_byte++) {
        ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display,
                                             display->line_buffer,
                                             SSD1683_DUAL_HEIGHT),
                            TAG,
                            "dual auxiliary frame transmit failed");
    }
    return ESP_OK;
}

static esp_err_t ssd1683_dual_write_frame_source(epd_ssd1683_t *display,
                                          const uint8_t *source, bool write_auxiliary_planes)
{
    ESP_RETURN_ON_ERROR(ssd1683_dual_set_master_address(display),
                        TAG,
                        "dual master address failed");
    ESP_RETURN_ON_ERROR(ssd1683_dual_write_half(display, 0x24, 0, source),
                        TAG,
                        "dual master frame failed");
    if (write_auxiliary_planes) {
        ESP_RETURN_ON_ERROR(ssd1683_dual_set_master_address(display),
                            TAG,
                            "dual master auxiliary address failed");
        ESP_RETURN_ON_ERROR(ssd1683_dual_write_constant_half(display, 0x26, 0x00),
                            TAG,
                            "dual master auxiliary frame failed");
    }
    ESP_RETURN_ON_ERROR(ssd1683_dual_set_slave_address(display),
                        TAG,
                        "dual slave address failed");
    ESP_RETURN_ON_ERROR(ssd1683_dual_write_half(
                            display,
                            0xa4,
                            SSD1683_DUAL_CONTROLLER_ROW_BYTES,
                            source),
                        TAG,
                        "dual slave frame failed");
    if (!write_auxiliary_planes) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ssd1683_dual_set_slave_address(display),
                        TAG,
                        "dual slave auxiliary address failed");
    return ssd1683_dual_write_constant_half(display, 0xa6, 0x00);
}

static esp_err_t ssd1683_dual_write_frame(epd_ssd1683_t *display, bool auxiliary)
{
    return ssd1683_dual_write_frame_source(display, display->buffer, auxiliary);
}

/* CrowPanel Fastwave adapter: phase A is the optional opposite kick (off),
 * B drives, C settles at VSS. Only the two transition blocks are driven.
 * Each cascade controller needs the generated LUT and selected companion
 * power registers; earlier native OTP state is not a custom-waveform setup.
 */
static esp_err_t ssd1683_dual_load_pulse_lut(epd_ssd1683_t *display)
{
    solar_os_ssd1683_waveform_params_t params = solar_os_ssd1683_waveform_defaults();
    params.drive_tp = display->pulse_tuning.pulse_frames;
    params.fr = display->pulse_tuning.waveform_fr;
    /* Keep the tested nominal rails and scan duration. VCOM uses Fastwave's
     * legal 0x08 setting; EOPT is deliberately excluded from register loading.
     * The pulse-mask adapter maps block 2 to black and block 3 to white, so
     * upstream's default rail selections preserve our established polarity.
     */
    params.volt_mask |= SOLAR_OS_SSD1683_WAVEFORM_V_GATE | SOLAR_OS_SSD1683_WAVEFORM_V_SOURCE;
    uint8_t waveform[SOLAR_OS_SSD1683_WAVEFORM_BYTES];
    if (!solar_os_ssd1683_waveform_build(waveform, &params)) return ESP_ERR_INVALID_ARG;

    const uint8_t clock[] = {0x80};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x22, clock, sizeof(clock)),
                        TAG, "waveform clock setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20), TAG, "waveform clock enable failed");
    const TickType_t clock_ticks = pdMS_TO_TICKS(2);
    vTaskDelay(clock_ticks != 0 ? clock_ticks : 1);
    const uint8_t booster[] = {0x8b, 0x9c, 0xa4, display->pulse_ramp_fast ? 0x00 : 0x0f};
    const uint8_t border[] = {0x80};
    const uint8_t ram_options[10] = {0};
    const uint8_t cascade[] = {0x00, 0x10};
    for (unsigned chip = 0; chip < 2; ++chip) {
        const uint8_t select = chip ? 0x80 : 0;
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x0c | select, booster, sizeof(booster)),
                            TAG, "pulse booster failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x32 | select, waveform,
                                              SOLAR_OS_SSD1683_LUT_BYTES), TAG, "Fastwave LUT failed");
        if (params.volt_mask & SOLAR_OS_SSD1683_WAVEFORM_V_EOPT)
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x3f | select, waveform + 227, 1),
                                TAG, "waveform EOPT failed");
        if (params.volt_mask & SOLAR_OS_SSD1683_WAVEFORM_V_GATE)
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x03 | select, waveform + 228, 1),
                                TAG, "waveform gate voltage failed");
        if (params.volt_mask & SOLAR_OS_SSD1683_WAVEFORM_V_SOURCE)
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x04 | select, waveform + 229, 3),
                                TAG, "waveform source voltage failed");
        if (params.volt_mask & SOLAR_OS_SSD1683_WAVEFORM_V_VCOM)
            ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x2c | select, waveform + 232, 1),
                                TAG, "waveform VCOM failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x3c | select, border, sizeof(border)),
                            TAG, "pulse border failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x37 | select, ram_options, sizeof(ram_options)),
                            TAG, "pulse RAM options failed");
    }
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x21, cascade, sizeof(cascade)),
                        TAG, "pulse cascade failed");
    display->pulse_lut_loaded = true;
    return ESP_OK;
}

static esp_err_t ssd1683_single_load_pulse_lut(epd_ssd1683_t *display)
{
    /* Preserve Waveshare V2's established table and voltage setup. */
    uint8_t lut[227] = {0};
    const uint8_t frames = display->pulse_tuning.pulse_frames;
    for (unsigned group = 0; group < 5; ++group) {
        const size_t offset = group * 42U;
        lut[offset] = 1;
        lut[offset + 1] = frames;
        lut[offset + 2] = 1;
        lut[offset + 5] = 1;
    }
    lut[85] |= 0x40;  /* old white -> new black: VSH1 */
    lut[127] |= 0x80; /* old black -> new white: VSL */
    lut[224] = display->pulse_tuning.waveform_fr;
    const uint8_t end[] = {0x22}; /* Documented two-frame discharge. */
    const uint8_t border[] = {0x80};
    const uint8_t cascade[] = {0x00, 0x00};
    /* Waveshare EPD_4in2_V2's custom-LUT setup publishes VCOM 0x30. */
    const uint8_t vcom[] = {0x30};
    const uint8_t gate[] = {0x17};
    const uint8_t source[] = {0x41, 0xa8, 0x32};
    /* D[5:0] holds three two-bit duration selections, each 10..40 ms.
     * Normal 0x0f is 40+40+10 ms; fast 0x00 is 10+10+10 ms.
     * Current/strength settings and voltage targets remain unchanged.
     */
    const uint8_t booster[] = {0x8b, 0x9c, 0xa4, display->pulse_ramp_fast ? 0x00 : 0x0f};
    const uint8_t ram_options[10] = {0}; /* Disable RAM ping-pong: explicit planes. */
    for (unsigned chip = 0; chip < 1U; ++chip) {
        const uint8_t select = chip == 0 ? 0x00 : 0x80;
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x0c | select, booster,
                                              sizeof(booster)), TAG, "pulse booster failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x32 | select, lut,
                                              sizeof(lut)), TAG, "pulse LUT failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x3f | select, end,
                                              sizeof(end)), TAG, "pulse end failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x03 | select, gate,
                                              sizeof(gate)), TAG, "pulse gate voltage failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x04 | select, source,
                                              sizeof(source)), TAG, "pulse source voltage failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x3c | select, border,
                                              sizeof(border)), TAG, "pulse border failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x2c | select, vcom,
                                              sizeof(vcom)), TAG, "pulse VCOM failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x37 | select, ram_options,
                                              sizeof(ram_options)), TAG, "pulse RAM options failed");
    }
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x21, cascade, sizeof(cascade)),
                        TAG, "pulse cascade failed");
    display->pulse_lut_loaded = true;
    return ESP_OK;
}

static bool ssd1683_pulse_pixel_white(const epd_ssd1683_t *display,
                                      const uint8_t *source, size_t pixel)
{
    const unsigned width = ssd1683_pulse_profile(display)->width;
    const size_t x = pixel % width;
    const size_t y = pixel / width;
    return (source[(y / 8U) * width + x] &
             (1U << (y % 8U))) != 0;
}

static void ssd1683_pulse_seed_budget(epd_ssd1683_t *display, const uint8_t *source)
{
    epd_pixel_budget_reset(&display->pixel_budget);
    display->pulse_window_valid = false;
    display->pulse_mask_dirty = true;
    display->pulse_mask_pending = 0;
    for (size_t p = 0; p < display->pixel_budget.count; ++p)
        epd_pixel_budget_seed(&display->pixel_budget, p,
                              ssd1683_pulse_pixel_white(display, source, p));
}

static esp_err_t ssd1683_dual_pulse_window(epd_ssd1683_t *display, unsigned chip,
                                          unsigned first, unsigned last,
                                          unsigned top, unsigned bottom)
{
    const uint8_t select = chip ? 0x80 : 0;
    const uint8_t mode[] = {chip ? 0x04 : 0x05};
    const uint8_t x[] = {chip ? 99U - first : first, chip ? 99U - last : last};
    const unsigned ys = SSD1683_DUAL_HEIGHT - 1U - top;
    const unsigned ye = SSD1683_DUAL_HEIGHT - 1U - bottom;
    const uint8_t y[] = {ys & 0xffU, ys >> 8, ye & 0xffU, ye >> 8};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x11 | select, mode, 1), TAG, "pulse entry failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x44 | select, x, 2), TAG, "pulse X failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x45 | select, y, 4), TAG, "pulse Y failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x4e | select, x, 1), TAG, "pulse X cursor failed");
    return ssd1683_cmd_data(display, 0x4f | select, y, 2);
}

static esp_err_t ssd1683_dual_write_pulse(epd_ssd1683_t *display)
{
    /* Black drive is (old,new)=(1,0), white=(0,1), hold=(0,0).
     * Always replace both planes of both controllers when the mask changes.
     */
    const size_t plane_size = SSD1683_DUAL_PADDED_ROW_BYTES * SSD1683_DUAL_HEIGHT;
    memset(display->pulse_planes, 0, plane_size * 2U);
    unsigned first = 99, last = 0, top = 271, bottom = 0;
    for (size_t i = 0; i < display->pixel_budget.active_count; ++i) {
        const size_t p = display->pixel_budget.active[i];
        const epd_pixel_drive_t drive = epd_pixel_budget_drive(&display->pixel_budget, p);
        if (drive == EPD_PIXEL_HOLD)
            continue;
        unsigned x = p % SSD1683_DUAL_WIDTH;
        const unsigned y = p / SSD1683_DUAL_WIDTH;
        if (x >= SSD1683_DUAL_SEAM_START)
            x += SSD1683_DUAL_SEAM_WIDTH;
        const unsigned column = x / 8U;
        if (column < first) first = column;
        if (column > last) last = column;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
        const unsigned plane = drive == EPD_PIXEL_DRIVE_BLACK;
        display->pulse_planes[plane * plane_size + column * SSD1683_DUAL_HEIGHT + y] |=
            (uint8_t)(0x80U >> (x % 8U));
    }
    const unsigned active_first = first, active_last = last;
    const unsigned active_top = top, active_bottom = bottom;
    /* Include the preceding mask so its now-settled pixels become neutral.
     * Remember the active bounds, not this union, so the window can shrink.
     */
    if (display->pulse_window_valid) {
        if (display->pulse_x_start < first) first = display->pulse_x_start;
        if (display->pulse_x_end > last) last = display->pulse_x_end;
        if (display->pulse_y_start < top) top = display->pulse_y_start;
        if (display->pulse_y_end > bottom) bottom = display->pulse_y_end;
    } else {
        /* Native image bits select active entries in the custom LUT. Replace
         * all four planes once before restricting writes to smaller windows.
         */
        first = 0; last = SSD1683_DUAL_PADDED_ROW_BYTES - 1U;
        top = 0; bottom = SSD1683_DUAL_HEIGHT - 1U;
    }
    for (unsigned chip = 0; chip < 2; ++chip) {
        const uint8_t select = chip == 0 ? 0x00 : 0x80;
        const unsigned chip_first = chip * SSD1683_DUAL_CONTROLLER_ROW_BYTES;
        const unsigned chip_last = chip_first + SSD1683_DUAL_CONTROLLER_ROW_BYTES - 1U;
        if (last < chip_first || first > chip_last)
            continue;
        const unsigned start = first > chip_first ? first : chip_first;
        const unsigned end = last < chip_last ? last : chip_last;
        for (unsigned plane = 0; plane < 2; ++plane) {
            ESP_RETURN_ON_ERROR(ssd1683_dual_pulse_window(display, chip, start, end, top, bottom),
                                TAG, "pulse window failed");
            ESP_RETURN_ON_ERROR(ssd1683_cmd(display, (plane ? 0x26 : 0x24) | select),
                                TAG, "pulse RAM failed");
            ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1), TAG, "pulse D/C failed");
            for (unsigned column = start; column <= end; ++column) {
                const size_t rows = bottom - top + 1U;
                memcpy(display->line_buffer, display->pulse_planes + plane * plane_size +
                           column * SSD1683_DUAL_HEIGHT + top, rows);
                ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display, display->line_buffer, rows),
                                    TAG, "pulse plane failed");
            }
        }
    }
    display->pulse_window_valid = true;
    display->pulse_x_start = active_first;
    display->pulse_x_end = active_last;
    display->pulse_y_start = active_top;
    display->pulse_y_end = active_bottom;
    return ESP_OK;
}

static esp_err_t ssd1683_dual_write_target(epd_ssd1683_t *display, const uint8_t *source)
{
    return ssd1683_dual_write_frame_source(display, source, true);
}

static esp_err_t ssd1683_single_write_target(epd_ssd1683_t *display, const uint8_t *source)
{
    for (unsigned plane = 0; plane < 2; ++plane) {
        ESP_RETURN_ON_ERROR(ssd1683_set_address(display), TAG, "native address failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, plane ? 0x26 : 0x24), TAG, "native RAM failed");
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1), TAG, "native D/C failed");
        for (unsigned y = 0; y < SSD1683_HEIGHT; ++y) {
            ssd1683_convert_row(display, source, y);
            ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display, display->line_buffer,
                                                SSD1683_PANEL_ROW_BYTES), TAG, "native row failed");
        }
    }
    return ESP_OK;
}

static esp_err_t ssd1683_legacy_write_target(epd_ssd1683_t *display, const uint8_t *source)
{
    ESP_RETURN_ON_ERROR(ssd1683_set_address(display), TAG, "native address failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x24), TAG, "native RAM failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1), TAG, "native D/C failed");
    for (unsigned y = 0; y < SSD1683_HEIGHT; ++y) {
        ssd1683_convert_row(display, source, y);
        ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display, display->line_buffer,
                                            SSD1683_PANEL_ROW_BYTES), TAG, "native row failed");
    }
    return ESP_OK;
}

static esp_err_t ssd1683_single_write_pulse(epd_ssd1683_t *display)
{
    const size_t plane_size = SSD1683_PANEL_ROW_BYTES * SSD1683_HEIGHT;
    memset(display->pulse_planes, 0, plane_size * 2U);
    unsigned first = SSD1683_PANEL_ROW_BYTES - 1U, last = 0;
    unsigned top = SSD1683_HEIGHT - 1U, bottom = 0;
    for (size_t i = 0; i < display->pixel_budget.active_count; ++i) {
        const size_t p = display->pixel_budget.active[i];
        const epd_pixel_drive_t drive = epd_pixel_budget_drive(&display->pixel_budget, p);
        if (drive == EPD_PIXEL_HOLD) continue;
        const unsigned x = p % SSD1683_WIDTH, y = p / SSD1683_WIDTH;
        const unsigned column = x / 8U;
        if (column < first) first = column;
        if (column > last) last = column;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
        const unsigned plane = drive == EPD_PIXEL_DRIVE_BLACK;
        display->pulse_planes[plane * plane_size + y * SSD1683_PANEL_ROW_BYTES + column] |=
            (uint8_t)(0x80U >> (x % 8U));
    }
    const unsigned active_first = first, active_last = last;
    const unsigned active_top = top, active_bottom = bottom;
    if (display->pulse_window_valid) {
        if (display->pulse_x_start < first) first = display->pulse_x_start;
        if (display->pulse_x_end > last) last = display->pulse_x_end;
        if (display->pulse_y_start < top) top = display->pulse_y_start;
        if (display->pulse_y_end > bottom) bottom = display->pulse_y_end;
    } else {
        /* Remove native image bits before enabling differential drive entries. */
        first = 0; last = SSD1683_PANEL_ROW_BYTES - 1U;
        top = 0; bottom = SSD1683_HEIGHT - 1U;
    }
    const ssd1683_window_t window = {first, last, top, bottom};
    const size_t row_bytes = last - first + 1U;
    for (unsigned plane = 0; plane < 2; ++plane) {
        ESP_RETURN_ON_ERROR(ssd1683_set_window(display, &window), TAG, "pulse window failed");
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, plane ? 0x26 : 0x24), TAG, "pulse RAM failed");
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1), TAG, "pulse D/C failed");
        for (unsigned y = top; y <= bottom; ++y) {
            memcpy(display->line_buffer, display->pulse_planes + plane * plane_size +
                y * SSD1683_PANEL_ROW_BYTES + first, row_bytes);
            ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display, display->line_buffer, row_bytes),
                                TAG, "pulse row failed");
        }
    }
    display->pulse_window_valid = true;
    display->pulse_x_start = active_first; display->pulse_x_end = active_last;
    display->pulse_y_start = active_top; display->pulse_y_end = active_bottom;
    return ESP_OK;
}

/* The green-sticker controller uses UC8276-family seven-byte LUT groups and
 * OLD/NEW transmission commands, rather than SSD1683's RAM/window commands.
 * DDX=01 keeps the vendor's white=1 convention: NEW/OLD=01 drives black,
 * 10 drives white, and both equal combinations remain neutral. */
static esp_err_t ssd1683_green_write_planes(epd_ssd1683_t *display,
                                           const uint8_t *source)
{
    const size_t plane_size = SSD1683_PANEL_ROW_BYTES * SSD1683_HEIGHT;
    for (unsigned plane = 0; plane < 2; ++plane) {
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, plane ? 0x10 : 0x13),
                            TAG, "green RAM command failed");
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 1),
                            TAG, "green RAM D/C failed");
        for (unsigned y = 0; y < SSD1683_HEIGHT; ++y) {
            if (source != NULL)
                ssd1683_convert_row(display, source, y);
            else
                memcpy(display->line_buffer, display->pulse_planes +
                    plane * plane_size + y * SSD1683_PANEL_ROW_BYTES,
                    SSD1683_PANEL_ROW_BYTES);
            ESP_RETURN_ON_ERROR(ssd1683_tx_bytes(display, display->line_buffer,
                SSD1683_PANEL_ROW_BYTES), TAG, "green RAM row failed");
        }
    }
    return ESP_OK;
}

static esp_err_t ssd1683_green_write_target(epd_ssd1683_t *display,
                                           const uint8_t *source)
{
    return ssd1683_green_write_planes(display, source);
}

static esp_err_t ssd1683_green_write_pulse(epd_ssd1683_t *display)
{
    const size_t plane_size = SSD1683_PANEL_ROW_BYTES * SSD1683_HEIGHT;
    memset(display->pulse_planes, 0, 2U * plane_size);
    for (size_t i = 0; i < display->pixel_budget.active_count; ++i) {
        const size_t p = display->pixel_budget.active[i];
        const epd_pixel_drive_t drive = epd_pixel_budget_drive(&display->pixel_budget, p);
        if (drive == EPD_PIXEL_HOLD) continue;
        const unsigned x = p % SSD1683_WIDTH, y = p / SSD1683_WIDTH;
        const unsigned plane = drive == EPD_PIXEL_DRIVE_BLACK;
        display->pulse_planes[plane * plane_size + y * SSD1683_PANEL_ROW_BYTES + x / 8U] |=
            (uint8_t)(0x80U >> (x % 8U));
    }
    /* Full RAM uploads avoid depending on the panel's partial-window support.
     * Neutral entries still ensure only budgeted pixels receive a drive. */
    return ssd1683_green_write_planes(display, NULL);
}

static esp_err_t ssd1683_green_load_pulse_lut(epd_ssd1683_t *display)
{
    /* UC8276c A0.6 sections 14-18: KW mode has the same six 7-byte groups
     * per LUT as SSD1683. Reuse phases only; PLL and voltage registers have
     * different encodings. DDX=01 maps block 2 to white, block 3 to black,
     * opposite to the dual-panel adapter's rail selections.
     */
    solar_os_ssd1683_waveform_params_t params = solar_os_ssd1683_waveform_defaults();
    params.drive_tp = display->pulse_tuning.pulse_frames;
    params.invert = true;
    params.volt_mask = 0;
    uint8_t waveform[SOLAR_OS_SSD1683_WAVEFORM_BYTES];
    if (!solar_os_ssd1683_waveform_build(waveform, &params)) return ESP_ERR_INVALID_ARG;
    /* The native automatic GC sequence leaves this revision needing reset
     * before a changed update. Reinitialize when entering a pulse waveform;
     * preserve the software budgets and replace both hardware RAM masks. */
    ssd1683_hardware_reset(display, SSD1683_RESET_PRE_HIGH_MS, SSD1683_RESET_LOW_MS);
    ESP_RETURN_ON_ERROR(ssd1683_green_init(display), TAG, "green pulse initialization failed");
    display->pulse_mask_dirty = true;
    const uint8_t interval[] = {0xd7}; /* Floating border, DDX=01. */
    const uint8_t pll[] = {(uint8_t)display->pulse_tuning.waveform_fr};
    const uint8_t booster[] = {
        display->pulse_ramp_fast ? 0x16 : 0x96,
        display->pulse_ramp_fast ? 0x16 : 0x96, 0x29,
    };
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x50, interval, sizeof(interval)),
                        TAG, "green pulse data mapping failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x30, pll, sizeof(pll)),
                        TAG, "green pulse PLL failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x06, booster, sizeof(booster)),
                        TAG, "green pulse booster failed");
    for (unsigned entry = 0; entry < 5; ++entry) {
        ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x20U + entry,
                            waveform + entry * 42U, 42),
                            TAG, "green pulse LUT failed");
    }
    display->pulse_lut_loaded = true;
    return ESP_OK;
}

static esp_err_t ssd1683_green_activate_pulse(epd_ssd1683_t *display)
{
    if (!display->pulse_power_hold) {
        const uint8_t automatic[] = {0xa5};
        display->pulse_analog_on = false;
        return ssd1683_cmd_data(display, 0x17, automatic, sizeof(automatic));
    }
    if (!display->pulse_analog_on) {
        const int idle = gpio_get_level((gpio_num_t)display->busy_pin);
        ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x04), TAG, "green pulse power-on failed");
        if (!ssd1683_busy_asserted(display, SSD1683_BUSY_ASSERT_TIMEOUT_MS) ||
            !ssd1683_busy_cleared(display, 2000)) {
            SOLAR_OS_LOGE(TAG, "green pulse power-on BUSY timeout idle=%d now=%d expected=%d",
                idle, gpio_get_level((gpio_num_t)display->busy_pin), display->busy_level);
            return ESP_ERR_TIMEOUT;
        }
        display->pulse_analog_on = true;
    }
    return ssd1683_cmd(display, 0x12);
}

static esp_err_t ssd1683_green_pulse_power_off(epd_ssd1683_t *display)
{
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x02), TAG, "green pulse power-off failed");
    return ssd1683_busy_cleared(display, 2000) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static const ssd1683_pulse_profile_t *ssd1683_pulse_profile(const epd_ssd1683_t *display)
{
    static const ssd1683_pulse_profile_t dual = {
        .width = SSD1683_DUAL_WIDTH, .height = SSD1683_DUAL_HEIGHT,
        .plane_size = SSD1683_DUAL_PADDED_ROW_BYTES * SSD1683_DUAL_HEIGHT,
        .write_target = ssd1683_dual_write_target,
        .write_mask = ssd1683_dual_write_pulse,
        .load_waveform = ssd1683_dual_load_pulse_lut,
        .activate = ssd1683_pulse_activate,
        .power_down = ssd1683_pulse_power_off,
    };
    static const ssd1683_pulse_profile_t single = {
        .width = SSD1683_WIDTH, .height = SSD1683_HEIGHT,
        .plane_size = SSD1683_PANEL_ROW_BYTES * SSD1683_HEIGHT,
        .write_target = ssd1683_single_write_target,
        .write_mask = ssd1683_single_write_pulse,
        .load_waveform = ssd1683_single_load_pulse_lut,
        .activate = ssd1683_pulse_activate,
        .power_down = ssd1683_pulse_power_off,
    };
    static const ssd1683_pulse_profile_t green = {
        .width = SSD1683_WIDTH, .height = SSD1683_HEIGHT,
        .plane_size = SSD1683_PANEL_ROW_BYTES * SSD1683_HEIGHT,
        .write_target = ssd1683_green_write_target,
        .write_mask = ssd1683_green_write_pulse,
        .load_waveform = ssd1683_green_load_pulse_lut,
        .activate = ssd1683_green_activate_pulse,
        .power_down = ssd1683_green_pulse_power_off,
    };
    if (ssd1683_is_dual(display)) return &dual;
    if (display != NULL && display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER)
        return &green;
    return display != NULL && display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 ? &single : NULL;
}

static bool ssd1683_pulse_retarget(epd_ssd1683_t *display, const uint8_t *source)
{
    const ssd1683_pulse_profile_t *profile = ssd1683_pulse_profile(display);
    bool changed = false;
    size_t index = 0;
    for (unsigned tile = 0; tile < (profile->height + 7U) / 8U; ++tile) {
        const unsigned rows = profile->height - tile * 8U;
        const uint8_t visible = rows >= 8 ? 0xffU : (1U << rows) - 1U;
        for (unsigned x = 0; x < profile->width; ++x, ++index) {
            const uint8_t bits = (source[index] ^ display->shadow[index]) & visible;
            if (bits == 0) continue;
            changed = true;
            for (unsigned bit = 0; bit < 8; ++bit) {
                if (bits & (1U << bit))
                    epd_pixel_budget_target(&display->pixel_budget,
                        (tile * 8U + bit) * profile->width + x,
                        (source[index] & (1U << bit)) != 0, display->pulse_tuning.drive_cycles);
            }
        }
    }
    memcpy(display->shadow, source, display->buffer_size);
    if (changed) display->pulse_mask_dirty = true;
    return changed;
}

static bool ssd1683_pulse_cleanup_due(const epd_ssd1683_t *display)
{
    const unsigned age_limit = display->pulse_tuning.cleanup_every *
        (display->pulse_tuning.cleanup_adaptive ? 4U : 1U);
    const unsigned drive_limit = display->pulse_tuning.cleanup_every * display->pulse_tuning.drive_cycles;
    return display->pulse_updates >= age_limit || (display->pulse_tuning.cleanup_adaptive &&
        display->pixel_budget.peak_drive_cycles >= drive_limit);
}

static esp_err_t ssd1683_pulse_prepare(epd_ssd1683_t *display,
                                           const uint8_t *source, bool new_target, bool *baseline)
{
    *baseline = false;
    const ssd1683_pulse_profile_t *profile = ssd1683_pulse_profile(display);
    if (profile == NULL) return ESP_ERR_NOT_SUPPORTED;
    if (!display->controller_ready)
        ESP_RETURN_ON_ERROR(ssd1683_controller_init(display), TAG, "pulse controller init failed");
    if (display->shadow == NULL)
        return ESP_ERR_NO_MEM;
    if (display->pixel_budget.pixels == NULL) {
        const size_t count = profile->width * profile->height;
        uint8_t *storage = heap_caps_malloc(count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage == NULL)
            return ESP_ERR_NO_MEM;
        ESP_RETURN_ON_ERROR(epd_pixel_budget_init(&display->pixel_budget,
                                                   storage, count), TAG,
                            "pixel budget init failed");
        uint32_t *active = heap_caps_malloc(count * sizeof(*active), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *history = heap_caps_malloc(count * sizeof(*history), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        display->pulse_planes = heap_caps_malloc(profile->plane_size * 2U,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (active == NULL || history == NULL || display->pulse_planes == NULL) {
            heap_caps_free(active);
            heap_caps_free(history);
            heap_caps_free(display->pixel_budget.pixels);
            memset(&display->pixel_budget, 0, sizeof(display->pixel_budget));
            heap_caps_free(display->pulse_planes);
            display->pulse_planes = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_RETURN_ON_ERROR(epd_pixel_budget_set_active_storage(&display->pixel_budget, active, count),
                            TAG, "active storage failed");
        ESP_RETURN_ON_ERROR(epd_pixel_budget_set_drive_history(&display->pixel_budget, history, count),
                            TAG, "drive history failed");
        display->pulse_needs_cleanup = true;
    }
    const unsigned drive_limit = display->pulse_tuning.cleanup_every * display->pulse_tuning.drive_cycles;
    const bool drive_cleanup = display->pulse_tuning.cleanup_adaptive &&
        display->pixel_budget.peak_drive_cycles >= drive_limit;
    if (!display->shadow_valid || display->pulse_needs_cleanup || ssd1683_pulse_cleanup_due(display)) {
        const char *reason = !display->shadow_valid || display->pulse_needs_cleanup ? "baseline" :
            (drive_cleanup ? "pixel-drive" : "age");
        const int64_t cleanup_started = esp_timer_get_time();
        if (display->pulse_lut_loaded)
            ESP_RETURN_ON_ERROR(ssd1683_controller_init(display), TAG,
                                "pulse cleanup reset failed");
        ESP_RETURN_ON_ERROR(profile->write_target(display, source), TAG,
                            "pulse cleanup frame failed");
        ESP_RETURN_ON_ERROR(ssd1683_trigger_update(display, true, false), TAG,
                            "pulse cleanup failed");
        memcpy(display->shadow, source, display->buffer_size);
        ssd1683_pulse_seed_budget(display, source);
        display->shadow_valid = true;
        display->pulse_lut_loaded = false;
        display->partial_refresh_active = false;
        display->pulse_needs_cleanup = false;
        display->pulse_updates = 0;
        SOLAR_OS_LOGI(TAG, "pulse baseline native total=%lldus reason=%s",
                 (long long)(esp_timer_get_time() - cleanup_started), reason);
        *baseline = true;
        return ESP_OK;
    }
    if (new_target && ssd1683_pulse_retarget(display, source)) {
        /* The worker paces new snapshots. Synchronous presentations need the
         * same image interval here; redrives never wait for an image slot.
         */
        if (!display->scan_async && display->pulse_last_frame_start != 0) {
            const int64_t due = display->pulse_last_frame_start +
                (int64_t)display->scan_period_ms * 1000;
            int64_t remaining;
            while ((remaining = due - esp_timer_get_time()) > 0) {
                TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
                vTaskDelay(ticks != 0 ? ticks : 1);
            }
        }
        display->pulse_last_frame_start = esp_timer_get_time();
        if (display->scan_async) {
            /* Continuous retargeting can keep some pixels pending forever.
             * Age cleanup by changed targets, independently of completion.
             */
            ++display->pulse_updates;
        }
    }
    return ESP_OK;
}

static esp_err_t ssd1683_pulse_power_off(epd_ssd1683_t *display)
{
    const uint8_t off[] = {0x03};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x22, off, 1), TAG, "pulse power-down setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20), TAG, "pulse power-down failed");
    if (!ssd1683_busy_cleared(display, 2000)) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

static esp_err_t ssd1683_pulse_powerdown(epd_ssd1683_t *display)
{
    if (!display->pulse_analog_on) return ESP_OK;
    const ssd1683_pulse_profile_t *profile = ssd1683_pulse_profile(display);
    if (profile == NULL) return ESP_ERR_NOT_SUPPORTED;
    ESP_RETURN_ON_ERROR(profile->power_down(display), TAG, "pulse power-off failed");
    display->pulse_analog_on = false;
    return ESP_OK;
}

static esp_err_t ssd1683_pulse_activate(epd_ssd1683_t *display)
{
    /* The dual panel only needs the analog-enable sequence once per burst.
     * DISPLAY alone keeps using the custom LUT and the already enabled rails.
     * Reset and idle power-down invalidate pulse_analog_on before reuse.
     */
    const bool rails_ready = ssd1683_is_dual(display) && display->pulse_analog_on;
    const uint8_t update[] = {display->pulse_power_hold ? (rails_ready ? 0x0c : 0xcc) : 0xcf};
    ESP_RETURN_ON_ERROR(ssd1683_cmd_data(display, 0x22, update, sizeof(update)),
                        TAG, "pulse activation setup failed");
    ESP_RETURN_ON_ERROR(ssd1683_cmd(display, 0x20), TAG, "pulse activation failed");
    display->pulse_analog_on = display->pulse_power_hold;
    return ESP_OK;
}

static esp_err_t ssd1683_pulse_cycle(epd_ssd1683_t *display,
                                         int64_t *transfer_us, int64_t *drive_us)
{
    const ssd1683_pulse_profile_t *profile = ssd1683_pulse_profile(display);
    if (profile == NULL) return ESP_ERR_NOT_SUPPORTED;
    if (!display->pulse_lut_loaded)
        ESP_RETURN_ON_ERROR(profile->load_waveform(display), TAG,
                            "pulse waveform setup failed");
    if (display->pulse_mask_dirty || display->pulse_mask_pending != display->pixel_budget.pending) {
        const int64_t transfer_started = esp_timer_get_time();
        ESP_RETURN_ON_ERROR(profile->write_mask(display), TAG, "pulse transfer failed");
        *transfer_us += esp_timer_get_time() - transfer_started;
        display->pulse_mask_pending = display->pixel_budget.pending;
        display->pulse_mask_dirty = false;
    }
    const int64_t drive_started = esp_timer_get_time();
    const int busy_idle = gpio_get_level((gpio_num_t)display->busy_pin);
    ESP_RETURN_ON_ERROR(profile->activate(display), TAG, "pulse activation failed");
    /* Measure physical drives separately from the new-image cadence. */
    const int64_t activated = esp_timer_get_time();
    display->pulse_drive_interval = display->pulse_last_drive_start == 0 ? 0 :
        activated - display->pulse_last_drive_start;
    display->pulse_last_drive_start = activated;
    if (!ssd1683_busy_asserted(display, SSD1683_BUSY_ASSERT_TIMEOUT_MS)) {
        SOLAR_OS_LOGE(TAG, "pulse activation did not assert BUSY idle=%d now=%d expected=%d",
            busy_idle, gpio_get_level((gpio_num_t)display->busy_pin), display->busy_level);
        return ESP_ERR_TIMEOUT;
    }
    if (!ssd1683_busy_cleared(display, 2000)) {
        SOLAR_OS_LOGE(TAG, "pulse BUSY did not clear within 2000ms");
        return ESP_ERR_TIMEOUT;
    }
    *drive_us += esp_timer_get_time() - drive_started;
    epd_pixel_budget_advance(&display->pixel_budget);
    return ESP_OK;
}

static esp_err_t ssd1683_pulse_refresh(epd_ssd1683_t *display)
{
    bool baseline;
    ESP_RETURN_ON_ERROR(ssd1683_pulse_prepare(display, display->buffer, true, &baseline),
                        TAG, "pulse prepare failed");
    if (baseline) return ESP_OK;
    if (display->pixel_budget.pending == 0) return ssd1683_pulse_powerdown(display);
    const int64_t started = esp_timer_get_time();
    const size_t changed = display->pixel_budget.pending;
    int64_t transfer_us = 0, drive_us = 0;
    unsigned cycles = 0;
    SOLAR_OS_LOGI(TAG, "pulse start pixels=%u cycles=%u frames=%u fr=%u",
        (unsigned)changed, display->pulse_tuning.drive_cycles,
        display->pulse_tuning.pulse_frames, display->pulse_tuning.waveform_fr);
    while (display->pixel_budget.pending != 0) {
        ESP_RETURN_ON_ERROR(ssd1683_pulse_cycle(display, &transfer_us, &drive_us), TAG, "pulse cycle failed");
        ++cycles;
    }
    ESP_RETURN_ON_ERROR(ssd1683_pulse_powerdown(display), TAG, "pulse idle power-down failed");
    ++display->pulse_updates;
    SOLAR_OS_LOGI(TAG, "pulse pixels=%u cycles=%u transfer=%lldus drive=%lldus total=%lldus",
        (unsigned)changed, cycles, (long long)transfer_us, (long long)drive_us,
        (long long)(esp_timer_get_time() - started));
    return ESP_OK;
}

static void ssd1683_scan_update_accepting(epd_ssd1683_t *display)
{
    const bool enabled = !display->power_state_present && display->relax_storage == NULL && display->scan_async &&
        display->refresh_mode == EPD_SSD1683_REFRESH_PULSE &&
        display->powered && display->controller_ready;
    epd_scan_worker_enable(&display->scan_worker, enabled);
    __atomic_store_n(&display->scan_submit_ready, enabled, __ATOMIC_RELEASE);
}

static void ssd1683_pulse_failed(epd_ssd1683_t *display, esp_err_t err)
{
    __atomic_store_n(&display->scan_submit_ready, false, __ATOMIC_RELEASE);
    epd_scan_worker_enable(&display->scan_worker, false);
    display->refresh_mode = EPD_SSD1683_REFRESH_AUTO;
    display->controller_ready = false;
    display->shadow_valid = false;
    display->pulse_needs_cleanup = true;
    SOLAR_OS_LOGW(TAG, "pulse failed err=0x%x, returning to refresh=auto", (unsigned)err);
}

static esp_err_t ssd1683_scan_process(void *context, const uint8_t *source,
                                      bool fresh, bool *pending, bool *scanned)
{
    epd_ssd1683_t *display = context;
    *pending = false; *scanned = false;
    if (display->lock != NULL) xSemaphoreTake(display->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (__atomic_load_n(&display->scan_submit_ready, __ATOMIC_ACQUIRE) &&
        display->scan_async && display->powered &&
        display->refresh_mode == EPD_SSD1683_REFRESH_PULSE) {
        const int64_t started = esp_timer_get_time();
        bool baseline;
        err = ssd1683_pulse_prepare(display, source, fresh, &baseline);
        if (err == ESP_OK && !baseline && display->pixel_budget.pending != 0) {
            const size_t pixels = display->pixel_budget.pending;
            int64_t transfer_us = 0, drive_us = 0;
            err = ssd1683_pulse_cycle(display, &transfer_us, &drive_us);
            if (err == ESP_OK) {
                *scanned = true;
                SOLAR_OS_LOGI(TAG, "pulse scan pixels=%u pending=%u transfer=%lldus drive=%lldus total=%lldus interval=%lldus",
                    (unsigned)pixels, (unsigned)display->pixel_budget.pending,
                    (long long)transfer_us, (long long)drive_us,
                    (long long)(esp_timer_get_time() - started), (long long)display->pulse_drive_interval);
            }
        }
        if (err == ESP_OK && display->pixel_budget.pending == 0)
            err = ssd1683_pulse_powerdown(display);
        if (err != ESP_OK) {
            ssd1683_pulse_failed(display, err);
            /* Recover immediately, including the newest accepted target, so a
             * static app does not remain frozen waiting for another paint.
             */
            xSemaphoreTake(display->scan_worker.lock, portMAX_DELAY);
            memcpy(display->scan_worker.snapshot, display->scan_worker.latest, display->buffer_size);
            xSemaphoreGive(display->scan_worker.lock);
            esp_err_t recovery = ssd1683_controller_init(display);
            if (recovery == ESP_OK)
                recovery = ssd1683_pulse_profile(display)->write_target(display, source);
            if (recovery == ESP_OK) recovery = ssd1683_trigger_update(display, true, false);
            if (recovery == ESP_OK) {
                memcpy(display->shadow, source, display->buffer_size);
                display->shadow_valid = true;
                display->pulse_needs_cleanup = false;
            } else {
                display->controller_ready = false;
                display->shadow_valid = false;
                SOLAR_OS_LOGE(TAG, "pulse native recovery failed err=0x%x", (unsigned)recovery);
            }
        } else {
            *pending = display->pixel_budget.pending != 0 || ssd1683_pulse_cleanup_due(display);
        }
    }
    display->last_error = err;
    if (display->lock != NULL) xSemaphoreGive(display->lock);
    return err;
}

static esp_err_t ssd1683_restore_native(epd_ssd1683_t *display)
{
    if (display->pulse_lut_loaded || display->pulse_needs_cleanup) {
        ESP_RETURN_ON_ERROR(ssd1683_controller_init(display), TAG,
                            "native waveform restore failed");
        display->pulse_needs_cleanup = false;
    }
    if (display->pixel_budget.pixels != NULL) {
        heap_caps_free(display->pixel_budget.active);
        heap_caps_free(display->pixel_budget.drive_history);
        heap_caps_free(display->pixel_budget.pixels);
        memset(&display->pixel_budget, 0, sizeof(display->pixel_budget));
    }
    heap_caps_free(display->pulse_planes);
    display->pulse_planes = NULL;
    return ESP_OK;
}

static esp_err_t ssd1683_dual_refresh(epd_ssd1683_t *display)
{
    if (display->shadow_valid && display->shadow != NULL &&
        memcmp(display->buffer, display->shadow, display->buffer_size) == 0) {
        return ESP_OK;
    }

    /*
     * Elecrow's dual-controller partial-refresh path transfers both primary
     * RAM halves and triggers update mode 0xDC.  Do not apply the generic
     * periodic full-refresh cadence here: a terminal/status update would then
     * flash the complete 5.79-inch panel every few seconds.
     */
    const bool full = display->refresh_mode == EPD_SSD1683_REFRESH_FULL ||
        !display->shadow_valid;
    const bool partial = display->refresh_mode == EPD_SSD1683_REFRESH_AUTO &&
        !full;
    const bool log_refresh = display->refresh_log_count < 4U;
    if (log_refresh) {
        ESP_LOGI(TAG,
                 "dual panel refresh %u %s starting",
                 (unsigned)(display->refresh_log_count + 1U),
                 full ? "full" : (partial ? "partial" : "fast"));
    }

    if (partial && !display->partial_refresh_active) {
        /* Elecrow resets once between the full cleanup and 0xDC partial mode. */
        ssd1683_hardware_reset(display,
                               SSD1683_RESET_PRE_HIGH_MS,
                               SSD1683_RESET_LOW_MS);
        ESP_RETURN_ON_ERROR(ssd1683_wait_ready(display),
                            TAG,
                            "dual partial reset wait failed");
    }

    ESP_RETURN_ON_ERROR(ssd1683_dual_write_frame(display, full),
                        TAG,
                        "dual frame write failed");
    ESP_RETURN_ON_ERROR(ssd1683_trigger_update(display, full, partial),
                        TAG,
                        "dual panel refresh failed");

    if (log_refresh) {
        display->refresh_log_count++;
        ESP_LOGI(TAG,
                 "dual panel refresh %u complete",
                 (unsigned)display->refresh_log_count);
    }
    if (display->shadow != NULL) {
        memcpy(display->shadow, display->buffer, display->buffer_size);
    }
    display->shadow_valid = true;
    display->partial_refresh_active = partial;
    display->fast_refresh_count = 0;
    return ESP_OK;
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
        if (!ssd1683_busy_asserted(display, SSD1683_BUSY_ASSERT_TIMEOUT_MS)) {
            SOLAR_OS_LOGE(TAG, "green native activation did not assert BUSY");
            return ESP_ERR_INVALID_RESPONSE;
        }
        return ssd1683_wait_ready(display);
    }

    const uint8_t update_mode[] = {
        partial ? (ssd1683_is_dual(display) ? 0xdc : 0xff) :
            (full ? 0xf7 : 0xc7)
    };
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
    if (display->relax_storage != NULL) return ESP_OK;
    const int64_t native_started = esp_timer_get_time();
    if (display->refresh_mode == EPD_SSD1683_REFRESH_PULSE) {
        const esp_err_t err = ssd1683_pulse_refresh(display);
        if (err != ESP_OK) ssd1683_pulse_failed(display, err);
        return err;
    }
    ESP_RETURN_ON_ERROR(ssd1683_restore_native(display), TAG, "native restore failed");
    if (!display->controller_ready) {
        ESP_RETURN_ON_ERROR(ssd1683_controller_init(display), TAG, "controller resume failed");
    }
    if (ssd1683_is_dual(display)) {
        return ssd1683_dual_refresh(display);
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

    const int64_t transfer_started = esp_timer_get_time();
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

    const int64_t drive_started = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(ssd1683_trigger_update(display, full, partial),
                        TAG,
                        "panel refresh failed");
    const int64_t drive_finished = esp_timer_get_time();
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
    if (display->panel_variant == EPD_SSD1683_PANEL_WAVESHARE_V2 ||
        display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER) {
        const int64_t finished = esp_timer_get_time();
        SOLAR_OS_LOGI(TAG, "native %s transfer=%lldus drive=%lldus total=%lldus",
            full ? "full" : (partial ? "partial" : "fast"),
            (long long)(drive_started - transfer_started + finished - drive_finished),
            (long long)(drive_finished - drive_started), (long long)(finished - native_started));
    }
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
    display->pulse_analog_on = false;
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
    epd_ssd1683_t *display = ssd1683_from_u8x8(u8x8);
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(
            u8x8,
            ssd1683_is_dual(display) ? &ssd1683_dual_display_info :
                &ssd1683_display_info);
        return 1;
    }

    if (display == NULL) {
        return 0;
    }

    if (message == U8X8_MSG_DISPLAY_DRAW_TILE) return 1;
    if (message == U8X8_MSG_DISPLAY_REFRESH &&
        __atomic_load_n(&display->scan_submit_ready, __ATOMIC_ACQUIRE)) {
        const esp_err_t submitted = epd_scan_worker_submit(&display->scan_worker, display->buffer);
        if (submitted == ESP_OK) return 1;
    }

    esp_err_t err = ESP_OK;
    if (display->lock != NULL)
        xSemaphoreTake(display->lock, portMAX_DELAY);
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        err = ssd1683_controller_init(display);
        break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        if (arg_int != 0) {
            __atomic_store_n(&display->scan_submit_ready, false, __ATOMIC_RELEASE);
            epd_scan_worker_enable(&display->scan_worker, false);
            err = ssd1683_sleep(display);
            if (err != ESP_OK) {
                display->controller_ready = false;
                display->shadow_valid = false;
            }
        } else {
            display->power_state_present = false;
            if (!display->controller_ready) err = ssd1683_controller_init(display);
        }
        break;
    case U8X8_MSG_DISPLAY_DRAW_TILE:
        if (display->lock != NULL)
            xSemaphoreGive(display->lock);
        return 1;
    case U8X8_MSG_DISPLAY_REFRESH:
        if (__atomic_load_n(&display->scan_submit_ready, __ATOMIC_ACQUIRE))
            err = epd_scan_worker_submit(&display->scan_worker, display->buffer);
        else
            err = ssd1683_refresh(display);
        break;
    default:
        if (display->lock != NULL)
            xSemaphoreGive(display->lock);
        return 0;
    }

    display->last_error = err;
    ssd1683_scan_update_accepting(display);
    if (display->lock != NULL)
        xSemaphoreGive(display->lock);
    return err == ESP_OK ? 1 : 0;
}

esp_err_t epd_ssd1683_present_mono_xbm(epd_ssd1683_t *display,
                                      const uint8_t *bitmap, size_t bitmap_size,
                                      uint16_t x, uint16_t y,
                                      uint16_t width, uint16_t height,
                                      uint16_t stride, bool palette_inverted)
{
    if (display == NULL || display->buffer == NULL || bitmap == NULL ||
        width == 0U || height == 0U ||
        stride < ((size_t)width + 7U) / 8U ||
        height > SIZE_MAX / stride || bitmap_size < (size_t)height * stride ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(&display->u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(&display->u8g2)) {
        return ESP_ERR_INVALID_ARG;
    }
    const u8g2_cb_t *rotation = display->u8g2.cb;
    if (rotation != U8G2_R0 && rotation != U8G2_R1 &&
        rotation != U8G2_R2 && rotation != U8G2_R3) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const unsigned native_width = ssd1683_is_dual(display) ?
        SSD1683_DUAL_WIDTH : SSD1683_WIDTH;
    const unsigned native_height = ssd1683_is_dual(display) ?
        SSD1683_DUAL_HEIGHT : SSD1683_HEIGHT;
    if (display->buffer_size < native_width * ((native_height + 7U) / 8U) ||
        !display->powered || display->power_state_present || display->relax_storage != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Like ordinary u8g2 drawing, compose before submitting the immutable
     * snapshot. The pulse worker reads its own snapshot, never this buffer. */
    memset(display->buffer, palette_inverted ? 0x00 : 0xff, display->buffer_size);
    for (unsigned row = 0; row < height; ++row) {
        const uint8_t *source = bitmap + (size_t)row * stride;
        for (unsigned column = 0; column < width; ++column) {
            const unsigned lx = x + column, ly = y + row;
            unsigned nx = lx, ny = ly;
            if (rotation == U8G2_R1) { nx = native_width - 1U - ly; ny = lx; }
            else if (rotation == U8G2_R2) {
                nx = native_width - 1U - lx; ny = native_height - 1U - ly;
            } else if (rotation == U8G2_R3) { nx = ly; ny = native_height - 1U - lx; }
            uint8_t *destination = display->buffer + (ny / 8U) * native_width + nx;
            const uint8_t mask = (uint8_t)(1U << (ny & 7U));
            const bool black = (source[column / 8U] & (1U << (column & 7U))) != 0U;
            if (black == palette_inverted) *destination |= mask;
            else *destination &= (uint8_t)~mask;
        }
    }
    if (__atomic_load_n(&display->scan_submit_ready, __ATOMIC_ACQUIRE)) {
        const esp_err_t err = epd_scan_worker_submit(&display->scan_worker, display->buffer);
        if (err == ESP_OK) return ESP_OK;
    }
    if (display->lock != NULL) xSemaphoreTake(display->lock, portMAX_DELAY);
    const esp_err_t err = ssd1683_refresh(display);
    display->last_error = err;
    ssd1683_scan_update_accepting(display);
    if (display->lock != NULL) xSemaphoreGive(display->lock);
    return err;
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
        config->panel_variant > EPD_SSD1683_PANEL_ELECROW_579_DUAL) {
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

static void ssd1683_start_default_refresh(epd_ssd1683_t *display,
                                         const epd_ssd1683_config_t *config)
{
    /* Resolve auto-detected panels before applying a board's startup policy.
     * Unsupported panels and allocation failures retain a usable native mode. */
    if (config->startup_pulse && ssd1683_pulse_profile(display) != NULL) {
        const esp_err_t err = epd_ssd1683_set_controller_mode(display, "refresh=pulse");
        if (err != ESP_OK)
            ESP_LOGW(TAG, "startup pulse unavailable err=0x%x; retaining refresh=auto", (unsigned)err);
    }
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
    ssd1683_pulse_defaults(display, config);
    display->spi_clock_hz = config->spi_clock_hz;
    display->spi_applied_hz = config->spi_clock_hz;
    display->lock = xSemaphoreCreateMutexStatic(&display->lock_storage);
    display->panel_variant = config->panel_variant;
    display->cs_pin = config->cs_pin;
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
            .max_transfer_sz = ssd1683_is_dual(display) ? SSD1683_DUAL_HEIGHT :
                SSD1683_PANEL_ROW_BYTES,
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

    display->line_buffer_size = ssd1683_is_dual(display) ? SSD1683_DUAL_HEIGHT :
        SSD1683_PANEL_ROW_BYTES;
    /* SPI transmits directly from this line buffer, so it must be internal DMA memory. */
    display->line_buffer = heap_caps_malloc(display->line_buffer_size,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (display->line_buffer == NULL) {
        epd_ssd1683_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->buffer_size = ssd1683_is_dual(display) ? SSD1683_DUAL_BUFFER_BYTES :
        SSD1683_BUFFER_BYTES;
    /* Driver framebuffer only requires byte-addressable memory. */
    display->buffer = heap_caps_calloc(1, display->buffer_size, MALLOC_CAP_8BIT);
    if (display->buffer == NULL) {
        epd_ssd1683_deinit(display);
        return ESP_ERR_NO_MEM;
    }

    display->shadow_size = display->buffer_size;
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
                     ssd1683_is_dual(display) ? SSD1683_DUAL_TILE_HEIGHT :
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
    } else {
        ssd1683_start_default_refresh(display, config);
    }
    return ret;
}

esp_err_t epd_ssd1683_relax(epd_ssd1683_t *display,
                            solar_os_display_relax_phase_t phase)
{
    if (display == NULL || display->buffer == NULL || (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END)
        return ESP_ERR_INVALID_ARG;
    const ssd1683_pulse_profile_t *profile = ssd1683_pulse_profile(display);
    if (display->lock != NULL) xSemaphoreTake(display->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        if (display->relax_storage != NULL || !display->powered || display->power_state_present) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            uint8_t *storage = heap_caps_malloc(display->buffer_size * 2U,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (storage == NULL) storage = heap_caps_malloc(display->buffer_size * 2U, MALLOC_CAP_8BIT);
            if (storage == NULL) err = ESP_ERR_NO_MEM;
            else {
                memcpy(storage, display->buffer, display->buffer_size);
                display->relax_storage = storage;
                ssd1683_scan_update_accepting(display);
                err = ssd1683_pulse_powerdown(display);
                if (err == ESP_OK) err = ssd1683_controller_init(display);
                if (err != ESP_OK) {
                    heap_caps_free(display->relax_storage);
                    display->relax_storage = NULL;
                    display->controller_ready = false;
                    display->shadow_valid = false;
                }
            }
        }
    } else if (display->relax_storage == NULL) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        const bool ending = phase == SOLAR_OS_DISPLAY_RELAX_END;
        uint8_t *image = display->relax_storage;
        if (!ending) {
            image += display->buffer_size;
            memset(image, phase == SOLAR_OS_DISPLAY_RELAX_WHITE ? 0xff : 0,
                   display->buffer_size);
        }
        if (!display->controller_ready) err = ssd1683_controller_init(display);
        if (err == ESP_OK) err = profile ? profile->write_target(display, image) :
            ssd1683_legacy_write_target(display, image);
        if (err == ESP_OK) err = ssd1683_trigger_update(display, true, false);
        if (err != ESP_OK) {
            display->controller_ready = false;
            display->shadow_valid = false;
        }
        if (ending) {
            if (err == ESP_OK && display->shadow != NULL) {
                memcpy(display->shadow, image, display->buffer_size);
                display->shadow_valid = true;
                if (profile && display->pixel_budget.pixels != NULL)
                    ssd1683_pulse_seed_budget(display, image);
            }
            display->pulse_updates = 0;
            display->pulse_needs_cleanup = err != ESP_OK;
            heap_caps_free(display->relax_storage);
            display->relax_storage = NULL;
        }
    }
    display->last_error = err;
    ssd1683_scan_update_accepting(display);
    if (display->lock != NULL) xSemaphoreGive(display->lock);
    return err;
}

esp_err_t epd_ssd1683_present_power_state(epd_ssd1683_t *display)
{
    if (display == NULL || display->buffer == NULL || display->lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&display->scan_submit_ready, false, __ATOMIC_RELEASE);
    epd_scan_worker_enable(&display->scan_worker, false);
    xSemaphoreTake(display->lock, portMAX_DELAY);
    display->power_state_present = true;
    /* An in-flight scan may have re-enabled submission before releasing the
     * panel lock. Discard its queued target again after joining it. */
    ssd1683_scan_update_accepting(display);
    const epd_ssd1683_refresh_mode_t previous = display->refresh_mode;
    display->refresh_mode = EPD_SSD1683_REFRESH_FULL;
    display->last_error = ssd1683_refresh(display);
    display->refresh_mode = previous;
    const esp_err_t err = display->last_error;
    xSemaphoreGive(display->lock);
    return err;
}

esp_err_t epd_ssd1683_resume(epd_ssd1683_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (display->lock != NULL) xSemaphoreTake(display->lock, portMAX_DELAY);
    __atomic_store_n(&display->scan_submit_ready, false, __ATOMIC_RELEASE);
    epd_scan_worker_enable(&display->scan_worker, false);
    display->last_error = ESP_OK;
    esp_err_t err = ssd1683_configure_pins(display);
    if (err == ESP_OK) err = ssd1683_controller_init(display);
    display->last_error = err;
    display->power_state_present = false;
    ssd1683_scan_update_accepting(display);
    if (display->lock != NULL) xSemaphoreGive(display->lock);
    return err;
}

void epd_ssd1683_deinit(epd_ssd1683_t *display)
{
    if (display == NULL) {
        return;
    }

    __atomic_store_n(&display->scan_submit_ready, false, __ATOMIC_RELEASE);
    epd_scan_worker_deinit(&display->scan_worker);

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
    if (display->pixel_budget.pixels != NULL) {
        heap_caps_free(display->pixel_budget.active);
        heap_caps_free(display->pixel_budget.drive_history);
        heap_caps_free(display->pixel_budget.pixels);
        memset(&display->pixel_budget, 0, sizeof(display->pixel_budget));
    }
    heap_caps_free(display->pulse_planes);
    display->pulse_planes = NULL;
    heap_caps_free(display->relax_storage);
    display->relax_storage = NULL;
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

const char *epd_ssd1683_controller_name(const epd_ssd1683_t *display)
{
    return ssd1683_is_dual(display) ? "SSD1683x2" : "SSD1683";
}

const char *epd_ssd1683_controller_mode(const epd_ssd1683_t *display)
{
    if (display == NULL) {
        return NULL;
    }
    if (ssd1683_pulse_profile(display) != NULL) {
        epd_ssd1683_t *d = (epd_ssd1683_t *)display;
        if (d->lock != NULL)
            xSemaphoreTake(d->lock, portMAX_DELAY);
        const char *refresh = d->refresh_mode == EPD_SSD1683_REFRESH_PULSE ? "pulse" :
            (d->refresh_mode == EPD_SSD1683_REFRESH_FULL ? "full" :
             (d->refresh_mode == EPD_SSD1683_REFRESH_FAST ? "fast" : "auto"));
        const epd_scan_stats_t stats = epd_scan_worker_stats(&d->scan_worker);
        snprintf(d->mode_description, sizeof(d->mode_description),
            "refresh=%s%s drive-cycles=%u pulse-frames=%u waveform-fr=%u cleanup-every=%u "
            "cleanup-policy=%s cleanup-peak-cycles=%u cleanup-limit-cycles=%u cleanup-age=%u "
            "pulse-scan=%s pulse-period=%u pulse-power=%s pulse-ramp=%s pulse-spi-khz=%u "
            "scan-last-us=%lld scan-interval-us=%lld scan-count=%u "
            "frame-count=%u frame-interval-us=%lld "
            "scan-submitted=%u scan-coalesced=%u pending=%u",
            refresh, ssd1683_is_dual(d) ? " waveform=fastwave" :
                (d->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER ? " waveform=fastwave-uc8276" : ""),
            d->pulse_tuning.drive_cycles, d->pulse_tuning.pulse_frames,
            d->pulse_tuning.waveform_fr, d->pulse_tuning.cleanup_every,
            d->pulse_tuning.cleanup_adaptive ? "adaptive" : "fixed",
            (unsigned)d->pixel_budget.peak_drive_cycles,
            d->pulse_tuning.cleanup_every * d->pulse_tuning.drive_cycles,
            (unsigned)d->pulse_updates,
            d->scan_async ? "async" : "sync", d->scan_period_ms,
            d->pulse_power_hold ? "hold" : "cycle", d->pulse_ramp_fast ? "fast" : "normal",
            (unsigned)(d->pulse_spi_clock_hz / 1000U),
            (long long)stats.last_scan_us, (long long)stats.last_interval_us,
            (unsigned)stats.scans, (unsigned)stats.frames, (long long)stats.last_frame_interval_us,
            (unsigned)stats.submitted, (unsigned)stats.coalesced,
            (unsigned)d->pixel_budget.pending);
        if (d->lock != NULL)
            xSemaphoreGive(d->lock);
        return d->mode_description;
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
    if (ssd1683_is_dual(display))
        return "refresh=<auto,fast,full,pulse> drive-cycles=<1..32> "
               "pulse-frames=<1..32> waveform-fr=<1..5,9..12> cleanup-every=<1..128> "
               "cleanup-policy=<adaptive,fixed> "
               "pulse-scan=<sync,async> pulse-period=<67..1000> "
               "pulse-power=<cycle,hold> pulse-ramp=<normal,fast> pulse-spi-khz=<100..20000>";
    if (display != NULL && display->panel_variant == EPD_SSD1683_PANEL_GREEN_STICKER)
        return "refresh=<auto,fast,full,pulse> drive-cycles=<1..32> "
               "pulse-frames=<1..32> waveform-fr=<1..12> cleanup-every=<1..128> "
               "cleanup-policy=<adaptive,fixed> "
               "pulse-scan=<sync,async> pulse-period=<67..1000> "
               "pulse-power=<cycle,hold> pulse-ramp=<normal,fast> pulse-spi-khz=<100..20000>";
    if (ssd1683_pulse_profile(display) != NULL)
        return "refresh=<auto,fast,full,pulse> drive-cycles=<1..32> "
               "pulse-frames=<1..32> waveform-fr=<1..12> cleanup-every=<1..128> "
               "cleanup-policy=<adaptive,fixed> "
               "pulse-scan=<sync,async> pulse-period=<10..1000> "
               "pulse-power=<cycle,hold> pulse-ramp=<normal,fast> pulse-spi-khz=<100..20000>";
    return "refresh=<auto,fast,full>";
}

esp_err_t epd_ssd1683_set_controller_mode(epd_ssd1683_t *display, const char *mode)
{
    if (display == NULL || mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display->lock != NULL)
        xSemaphoreTake(display->lock, portMAX_DELAY);
    if (display->relax_storage != NULL) {
        if (display->lock != NULL) xSemaphoreGive(display->lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    const epd_ssd1683_refresh_mode_t before = display->refresh_mode;
    if (strcmp(mode, "refresh=auto") == 0) {
        display->refresh_mode = EPD_SSD1683_REFRESH_AUTO;
    } else if (strcmp(mode, "refresh=fast") == 0) {
        display->refresh_mode = EPD_SSD1683_REFRESH_FAST;
    } else if (strcmp(mode, "refresh=full") == 0) {
        display->refresh_mode = EPD_SSD1683_REFRESH_FULL;
    } else if (strcmp(mode, "refresh=pulse") == 0 && ssd1683_pulse_profile(display) != NULL) {
        /* The default async preset creates its worker only when selected. */
        if (display->scan_async && display->scan_worker.task == NULL) {
            err = epd_scan_worker_init(&display->scan_worker, display->buffer_size,
                                        ssd1683_scan_process, display);
            if (err == ESP_OK)
                err = epd_scan_worker_set_period(&display->scan_worker, display->scan_period_ms);
        }
        if (err == ESP_OK) {
            display->refresh_mode = EPD_SSD1683_REFRESH_PULSE;
            if (before != display->refresh_mode)
                display->pulse_needs_cleanup = true;
        }
    } else if (ssd1683_pulse_profile(display) != NULL) {
        const epd_pulse_tuning_t previous = display->pulse_tuning;
        const bool was_async = display->scan_async;
        const bool was_hold = display->pulse_power_hold, was_fast = display->pulse_ramp_fast;
        if (strcmp(mode, "pulse-scan=async") == 0) {
            if (display->scan_worker.task == NULL)
                err = epd_scan_worker_init(&display->scan_worker, display->buffer_size, ssd1683_scan_process, display);
            if (err == ESP_OK) {
                err = epd_scan_worker_set_period(&display->scan_worker, display->scan_period_ms);
                if (err == ESP_OK) display->scan_async = true;
            }
        } else if (strcmp(mode, "pulse-scan=sync") == 0) {
            display->scan_async = false;
        } else if (strcmp(mode, "pulse-power=hold") == 0) {
            display->pulse_power_hold = true;
        } else if (strcmp(mode, "pulse-power=cycle") == 0) {
            display->pulse_power_hold = false;
        } else if (strcmp(mode, "pulse-ramp=fast") == 0) {
            display->pulse_ramp_fast = true;
        } else if (strcmp(mode, "pulse-ramp=normal") == 0) {
            display->pulse_ramp_fast = false;
        } else if (strncmp(mode, "pulse-period=", 13) == 0 ||
                   strncmp(mode, "pulse-spi-khz=", 14) == 0) {
            const bool spi_option = mode[6] == 's';
            const unsigned maximum = spi_option ? 20000 : 1000;
            unsigned value = 0;
            for (const char *p = mode + (spi_option ? 14 : 13); *p; ++p) {
                if (*p < '0' || *p > '9' || value > maximum) { err = ESP_ERR_INVALID_ARG; break; }
                value = value * 10U + (unsigned)(*p - '0');
            }
            const unsigned minimum = spi_option ? 100U :
                (ssd1683_uses_fastwave(display) ? SSD1683_FASTWAVE_MIN_FRAME_PERIOD_MS : 10U);
            if (value < minimum || value > maximum) err = ESP_ERR_INVALID_ARG;
            if (err == ESP_OK && !spi_option && display->scan_worker.task != NULL)
                err = epd_scan_worker_set_period(&display->scan_worker, value);
            if (err == ESP_OK) {
                if (spi_option) display->pulse_spi_clock_hz = value * 1000U;
                else display->scan_period_ms = value;
            }
        } else {
            err = epd_pulse_tuning_set(&display->pulse_tuning, mode);
            if (err == ESP_OK && ssd1683_is_dual(display) &&
                !solar_os_ssd1683_waveform_fr_valid(display->pulse_tuning.waveform_fr)) {
                display->pulse_tuning = previous;
                err = ESP_ERR_INVALID_ARG;
            }
        }
        if (err == ESP_OK && display->refresh_mode == EPD_SSD1683_REFRESH_PULSE &&
            (was_async != display->scan_async || was_hold != display->pulse_power_hold ||
            was_fast != display->pulse_ramp_fast || previous.pulse_frames != display->pulse_tuning.pulse_frames ||
            previous.waveform_fr != display->pulse_tuning.waveform_fr))
            display->pulse_needs_cleanup = true;
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    if (before == EPD_SSD1683_REFRESH_PULSE &&
        display->refresh_mode != EPD_SSD1683_REFRESH_PULSE)
        display->pulse_needs_cleanup = true;
    ssd1683_scan_update_accepting(display);
    if (err == ESP_OK && (display->refresh_mode != EPD_SSD1683_REFRESH_PULSE || !display->scan_async))
        err = ssd1683_pulse_powerdown(display);
    if (display->lock != NULL)
        xSemaphoreGive(display->lock);
    return err;
}
