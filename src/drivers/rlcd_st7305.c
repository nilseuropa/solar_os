#include "rlcd_st7305.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_board.h"

#define RLCD_SPI_HOST SPI2_HOST
#define RLCD_SPI_CLOCK_HZ 24000000
#define RLCD_TILE_WIDTH 38
#define RLCD_TILE_HEIGHT 50
#define RLCD_BUFFER_ROW_BYTES (RLCD_TILE_WIDTH * 8)
#define RLCD_NATIVE_WIDTH 300
#define RLCD_NATIVE_HEIGHT 400
#define RLCD_ADDR_START 0x12
#define RLCD_COLUMN_GROUPS ((RLCD_NATIVE_WIDTH + 11) / 12)
#define RLCD_CONTROLLER_ROW_BYTES (RLCD_COLUMN_GROUPS * 3)
#define RLCD_CONTROLLER_ROWS_PER_TILE 4
#define RLCD_SHADOW_BYTES (RLCD_CONTROLLER_ROW_BYTES * RLCD_CONTROLLER_ROWS_PER_TILE * RLCD_TILE_HEIGHT)
#define RLCD_MAX_TRANSFER_BYTES 4092
#define RLCD_CONTROLLER_MODE_VALUES "default inv-off lpm lpm-inv-off u8g2 u8g2-lpm"

static const char *TAG = "rlcd_st7305";
static rlcd_st7305_t *active_display;

static const u8x8_display_info_t st7305_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 20,
    .post_reset_wait_ms = 50,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = RLCD_SPI_CLOCK_HZ,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = RLCD_TILE_WIDTH,
    .tile_height = RLCD_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = RLCD_NATIVE_WIDTH,
    .pixel_height = RLCD_NATIVE_HEIGHT,
};

typedef struct {
    uint8_t d6[2];
    uint8_t d1[1];
    uint8_t c0[2];
    uint8_t c1[4];
    uint8_t c2[4];
    uint8_t c4[4];
    uint8_t c5[4];
    uint8_t d8[2];
    uint8_t b2[1];
    uint8_t b3[10];
    uint8_t b4[8];
    uint8_t gate_timing[3];
    uint8_t b7[1];
    uint8_t b0[1];
    uint8_t c9[1];
    uint8_t m36[1];
    uint8_t m3a[1];
    uint8_t b9[1];
    uint8_t b8[1];
    uint8_t m35[1];
    uint8_t d0[1];
    uint8_t bb[1];
} rlcd_controller_settings_t;

typedef struct {
    const char *name;
    const rlcd_controller_settings_t *settings;
    uint8_t power_mode_cmd;
    uint8_t inversion_cmd;
    bool inversion_after_display_on;
    bool send_bb;
} rlcd_controller_profile_t;

static const rlcd_controller_settings_t rlcd_waveshare_settings = {
    .d6 = {0x17, 0x02},
    .d1 = {0x01},
    .c0 = {0x11, 0x04},
    .c1 = {0x69, 0x69, 0x69, 0x69},
    .c2 = {0x19, 0x19, 0x19, 0x19},
    .c4 = {0x4B, 0x4B, 0x4B, 0x4B},
    .c5 = {0x19, 0x19, 0x19, 0x19},
    .d8 = {0x80, 0xE9},
    .b2 = {0x02},
    .b3 = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45},
    .b4 = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45},
    .gate_timing = {0x32, 0x03, 0x1F},
    .b7 = {0x13},
    .b0 = {0x64},
    .c9 = {0x00},
    .m36 = {0x48},
    .m3a = {0x11},
    .b9 = {0x20},
    .b8 = {0x29},
    .m35 = {0x00},
    .d0 = {0xFF},
    .bb = {0x4F},
};

static const rlcd_controller_settings_t rlcd_u8g2_settings = {
    .d6 = {0x13, 0x02},
    .d1 = {0x01},
    .c0 = {0x12, 0x0A},
    .c1 = {0x3C, 0x3E, 0x3C, 0x3C},
    .c2 = {0x23, 0x21, 0x23, 0x23},
    .c4 = {0x5A, 0x5C, 0x5A, 0x5A},
    .c5 = {0x37, 0x35, 0x37, 0x37},
    .d8 = {0xA6, 0xE9},
    .b2 = {0x12},
    .b3 = {0xE5, 0xF6, 0x17, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x71},
    .b4 = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45},
    .gate_timing = {0x32, 0x03, 0x1F},
    .b7 = {0x13},
    .b0 = {0x64},
    .c9 = {0x00},
    .m36 = {0x48},
    .m3a = {0x11},
    .b9 = {0x20},
    .b8 = {0x29},
    .m35 = {0x00},
    .d0 = {0xFF},
    .bb = {0x4F},
};

static const rlcd_controller_profile_t rlcd_controller_profiles[] = {
    {
        .name = "default",
        .settings = &rlcd_waveshare_settings,
        .power_mode_cmd = 0x38,
        .inversion_cmd = 0x21,
    },
    {
        .name = "inv-off",
        .settings = &rlcd_waveshare_settings,
        .power_mode_cmd = 0x38,
        .inversion_cmd = 0x20,
    },
    {
        .name = "lpm",
        .settings = &rlcd_waveshare_settings,
        .power_mode_cmd = 0x39,
        .inversion_cmd = 0x21,
    },
    {
        .name = "lpm-inv-off",
        .settings = &rlcd_waveshare_settings,
        .power_mode_cmd = 0x39,
        .inversion_cmd = 0x20,
    },
    {
        .name = "u8g2",
        .settings = &rlcd_u8g2_settings,
        .power_mode_cmd = 0x38,
        .inversion_cmd = 0x20,
        .inversion_after_display_on = true,
        .send_bb = true,
    },
    {
        .name = "u8g2-lpm",
        .settings = &rlcd_u8g2_settings,
        .power_mode_cmd = 0x39,
        .inversion_cmd = 0x20,
        .inversion_after_display_on = true,
        .send_bb = true,
    },
};

static bool rlcd_checked_cmd(rlcd_st7305_t *display, uint8_t command);

static esp_err_t rlcd_write_bytes(rlcd_st7305_t *display, const uint8_t *data, size_t length)
{
    while (length > 0) {
        const size_t chunk = length > RLCD_MAX_TRANSFER_BYTES ? RLCD_MAX_TRANSFER_BYTES : length;
        spi_transaction_t transaction = {
            .length = chunk * 8,
            .tx_buffer = data,
        };

        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display->spi, &transaction), TAG,
                            "spi transmit failed");
        data += chunk;
        length -= chunk;
    }

    return ESP_OK;
}

static esp_err_t rlcd_cmd_data(rlcd_st7305_t *display, uint8_t command, const uint8_t *data, size_t length)
{
    esp_err_t err = gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 0);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_CS, 0);
    if (err != ESP_OK) {
        return err;
    }

    err = rlcd_write_bytes(display, &command, sizeof(command));
    if (err == ESP_OK && length > 0) {
        err = gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1);
        if (err == ESP_OK) {
            err = rlcd_write_bytes(display, data, length);
        }
    }

    const esp_err_t cs_err = gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_CS, 1);
    return err == ESP_OK ? cs_err : err;
}

static esp_err_t rlcd_cmd(rlcd_st7305_t *display, uint8_t command)
{
    return rlcd_cmd_data(display, command, NULL, 0);
}

static void rlcd_invalidate_shadow(rlcd_st7305_t *display)
{
    if (display != NULL) {
        display->shadow_valid_rows = 0;
    }
}

static bool rlcd_shadow_row_valid(const rlcd_st7305_t *display, uint8_t y_pos)
{
    return display != NULL &&
        display->shadow != NULL &&
        display->shadow_size == RLCD_SHADOW_BYTES &&
        y_pos < RLCD_TILE_HEIGHT &&
        (display->shadow_valid_rows & (1ULL << y_pos)) != 0;
}

static uint8_t *rlcd_shadow_tile_row(rlcd_st7305_t *display, uint8_t y_pos)
{
    return display->shadow +
        ((size_t)y_pos * RLCD_CONTROLLER_ROWS_PER_TILE * RLCD_CONTROLLER_ROW_BYTES);
}

static bool rlcd_shadow_window_matches(rlcd_st7305_t *display,
                                       const uint8_t *rows,
                                       uint8_t y_pos,
                                       int send_start,
                                       int send_count)
{
    if (rows == NULL ||
        send_start < 0 ||
        send_count <= 0 ||
        send_start + send_count > RLCD_CONTROLLER_ROW_BYTES ||
        !rlcd_shadow_row_valid(display, y_pos)) {
        return false;
    }

    const uint8_t *shadow = rlcd_shadow_tile_row(display, y_pos);
    for (int source_row = 0; source_row < RLCD_CONTROLLER_ROWS_PER_TILE; source_row++) {
        const uint8_t *source = rows + ((size_t)source_row * (size_t)send_count);
        const uint8_t *previous =
            shadow + ((size_t)source_row * RLCD_CONTROLLER_ROW_BYTES) + send_start;
        if (memcmp(previous, source, (size_t)send_count) != 0) {
            return false;
        }
    }

    return true;
}

static void rlcd_shadow_update_window(rlcd_st7305_t *display,
                                      const uint8_t *rows,
                                      uint8_t y_pos,
                                      int send_start,
                                      int send_count)
{
    if (display == NULL ||
        display->shadow == NULL ||
        display->shadow_size != RLCD_SHADOW_BYTES ||
        rows == NULL ||
        y_pos >= RLCD_TILE_HEIGHT ||
        send_start < 0 ||
        send_count <= 0 ||
        send_start + send_count > RLCD_CONTROLLER_ROW_BYTES) {
        return;
    }

    uint8_t *shadow = rlcd_shadow_tile_row(display, y_pos);
    for (int source_row = 0; source_row < RLCD_CONTROLLER_ROWS_PER_TILE; source_row++) {
        const uint8_t *source = rows + ((size_t)source_row * (size_t)send_count);
        uint8_t *dest = shadow + ((size_t)source_row * RLCD_CONTROLLER_ROW_BYTES) + send_start;
        memcpy(dest, source, (size_t)send_count);
    }

    if (send_start == 0 && send_count == RLCD_CONTROLLER_ROW_BYTES) {
        display->shadow_valid_rows |= (1ULL << y_pos);
    }
}

static bool rlcd_checked_cmd_data(rlcd_st7305_t *display, uint8_t command, const uint8_t *data, size_t length)
{
    const esp_err_t err = rlcd_cmd_data(display, command, data, length);
    if (err != ESP_OK) {
        display->last_error = err;
        ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
        return false;
    }

    return true;
}

static const rlcd_controller_profile_t *rlcd_find_controller_profile(const char *mode)
{
    if (mode == NULL || mode[0] == '\0') {
        mode = "default";
    }

    for (size_t i = 0; i < sizeof(rlcd_controller_profiles) / sizeof(rlcd_controller_profiles[0]); i++) {
        if (strcmp(rlcd_controller_profiles[i].name, mode) == 0) {
            return &rlcd_controller_profiles[i];
        }
    }

    return NULL;
}

static const rlcd_controller_profile_t *rlcd_current_controller_profile(const rlcd_st7305_t *display)
{
    const rlcd_controller_profile_t *profile =
        rlcd_find_controller_profile(display != NULL ? display->controller_mode : NULL);
    return profile != NULL ? profile : &rlcd_controller_profiles[0];
}

static bool rlcd_profiles_differ_only_by_power(const rlcd_controller_profile_t *current,
                                               const rlcd_controller_profile_t *next)
{
    return current != NULL &&
        next != NULL &&
        current->settings == next->settings &&
        current->inversion_cmd == next->inversion_cmd &&
        current->inversion_after_display_on == next->inversion_after_display_on &&
        current->send_bb == next->send_bb;
}

static esp_err_t rlcd_apply_controller_power_mode(rlcd_st7305_t *display,
                                                  const rlcd_controller_profile_t *profile)
{
    if (display == NULL || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rlcd_checked_cmd(display, profile->power_mode_cmd)) {
        return display->last_error;
    }

    display->controller_mode = profile->name;
    display->last_error = ESP_OK;
    return ESP_OK;
}

static esp_err_t rlcd_apply_controller_profile(rlcd_st7305_t *display,
                                               const rlcd_controller_profile_t *profile,
                                               bool display_was_reset)
{
    if (display == NULL || profile == NULL || profile->settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const rlcd_controller_settings_t *settings = profile->settings;
    const uint8_t win_a[] = {0x12, 0x2A};
    const uint8_t win_b[] = {0x00, 0xC7};

    if (!rlcd_checked_cmd_data(display, 0xD6, settings->d6, sizeof(settings->d6)) ||
        !rlcd_checked_cmd_data(display, 0xD1, settings->d1, sizeof(settings->d1)) ||
        !rlcd_checked_cmd_data(display, 0xC0, settings->c0, sizeof(settings->c0)) ||
        !rlcd_checked_cmd_data(display, 0xC1, settings->c1, sizeof(settings->c1)) ||
        !rlcd_checked_cmd_data(display, 0xC2, settings->c2, sizeof(settings->c2)) ||
        !rlcd_checked_cmd_data(display, 0xC4, settings->c4, sizeof(settings->c4)) ||
        !rlcd_checked_cmd_data(display, 0xC5, settings->c5, sizeof(settings->c5)) ||
        !rlcd_checked_cmd_data(display, 0xD8, settings->d8, sizeof(settings->d8)) ||
        !rlcd_checked_cmd_data(display, 0xB2, settings->b2, sizeof(settings->b2)) ||
        !rlcd_checked_cmd_data(display, 0xB3, settings->b3, sizeof(settings->b3)) ||
        !rlcd_checked_cmd_data(display, 0xB4, settings->b4, sizeof(settings->b4)) ||
        !rlcd_checked_cmd_data(display, 0x62, settings->gate_timing, sizeof(settings->gate_timing)) ||
        !rlcd_checked_cmd_data(display, 0xB7, settings->b7, sizeof(settings->b7)) ||
        !rlcd_checked_cmd_data(display, 0xB0, settings->b0, sizeof(settings->b0)) ||
        !rlcd_checked_cmd(display, 0x11)) {
        return display->last_error;
    }

    if (display_was_reset) {
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!rlcd_checked_cmd_data(display, 0xC9, settings->c9, sizeof(settings->c9)) ||
        !rlcd_checked_cmd_data(display, 0x36, settings->m36, sizeof(settings->m36)) ||
        !rlcd_checked_cmd_data(display, 0x3A, settings->m3a, sizeof(settings->m3a)) ||
        !rlcd_checked_cmd_data(display, 0xB9, settings->b9, sizeof(settings->b9)) ||
        !rlcd_checked_cmd_data(display, 0xB8, settings->b8, sizeof(settings->b8))) {
        return display->last_error;
    }

    if (!profile->inversion_after_display_on && !rlcd_checked_cmd(display, profile->inversion_cmd)) {
        return display->last_error;
    }

    if (!rlcd_checked_cmd_data(display, 0x2A, win_a, sizeof(win_a)) ||
        !rlcd_checked_cmd_data(display, 0x2B, win_b, sizeof(win_b)) ||
        !rlcd_checked_cmd_data(display, 0x35, settings->m35, sizeof(settings->m35)) ||
        !rlcd_checked_cmd_data(display, 0xD0, settings->d0, sizeof(settings->d0)) ||
        !rlcd_checked_cmd(display, profile->power_mode_cmd) ||
        !rlcd_checked_cmd(display, 0x29)) {
        return display->last_error;
    }

    if (profile->inversion_after_display_on && !rlcd_checked_cmd(display, profile->inversion_cmd)) {
        return display->last_error;
    }
    if (profile->send_bb &&
        !rlcd_checked_cmd_data(display, 0xBB, settings->bb, sizeof(settings->bb))) {
        return display->last_error;
    }

    rlcd_invalidate_shadow(display);
    display->controller_mode = profile->name;
    display->last_error = ESP_OK;
    return ESP_OK;
}

static bool rlcd_checked_cmd(rlcd_st7305_t *display, uint8_t command)
{
    const esp_err_t err = rlcd_cmd(display, command);
    if (err != ESP_OK) {
        display->last_error = err;
        ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
        return false;
    }

    return true;
}

static void rlcd_reset(void)
{
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t rlcd_configure_control_pins(void)
{
    const gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << SOLAR_OS_BOARD_PIN_LCD_DC) |
                        (1ULL << SOLAR_OS_BOARD_PIN_LCD_CS) |
                        (1ULL << SOLAR_OS_BOARD_PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "gpio config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_CS, 1), TAG, "cs high failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_DC, 1), TAG, "dc high failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(SOLAR_OS_BOARD_PIN_LCD_RST, 1), TAG, "rst high failed");
    return ESP_OK;
}

static esp_err_t rlcd_full_init(rlcd_st7305_t *display)
{
    rlcd_reset();

    return rlcd_apply_controller_profile(display, rlcd_current_controller_profile(display), true);
}

static uint8_t rlcd_u8x8_byte_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

static uint8_t rlcd_u8x8_display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    (void)arg_int;

    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &st7305_display_info);
        return 1;
    }

    rlcd_st7305_t *display = active_display;
    if (display == NULL) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
        return rlcd_full_init(display) == ESP_OK ? 1 : 0;

    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        rlcd_invalidate_shadow(display);
        return 1;

    case U8X8_MSG_DISPLAY_DRAW_TILE: {
        const u8x8_tile_t *tile = (const u8x8_tile_t *)arg_ptr;
        const uint8_t count = tile->cnt;
        const uint8_t y_pos = tile->y_pos;
        const uint8_t x_pos = tile->x_pos;

        const int first_col = x_pos * 8;
        int last_col = (x_pos + count) * 8 - 1;
        if (last_col >= RLCD_NATIVE_WIDTH) {
            last_col = RLCD_NATIVE_WIDTH - 1;
        }

        const int addr_start = RLCD_ADDR_START + first_col / 12;
        const int addr_end = RLCD_ADDR_START + last_col / 12;
        const int send_start = (addr_start - RLCD_ADDR_START) * 3;
        const int send_count = (addr_end - addr_start + 1) * 3;

        const int addr_first_col = (addr_start - RLCD_ADDR_START) * 12;
        int addr_last_col = (addr_end - RLCD_ADDR_START) * 12 + 11;
        if (addr_last_col >= RLCD_NATIVE_WIDTH) {
            addr_last_col = RLCD_NATIVE_WIDTH - 1;
        }

        const uint8_t *row_base = tile->tile_ptr - ((uint16_t)x_pos * 8U);

        static const uint8_t st_lut[4][4] = {
            {0x00, 0x80, 0x40, 0xC0},
            {0x00, 0x20, 0x10, 0x30},
            {0x00, 0x08, 0x04, 0x0C},
            {0x00, 0x02, 0x01, 0x03},
        };

        uint8_t rows[RLCD_CONTROLLER_ROW_BYTES * RLCD_CONTROLLER_ROWS_PER_TILE] = {0};
        for (int source_row = 0; source_row < RLCD_CONTROLLER_ROWS_PER_TILE; source_row++) {
            const int shift = source_row * 2;
            const int base_offset = source_row * send_count;
            int index = base_offset + (addr_first_col >> 2) - send_start;

            for (int col = addr_first_col; col <= addr_last_col; col += 4, index++) {
                rows[index] = st_lut[0][(row_base[col] >> shift) & 3] |
                              st_lut[1][(row_base[col + 1] >> shift) & 3] |
                              st_lut[2][(row_base[col + 2] >> shift) & 3] |
                              st_lut[3][(row_base[col + 3] >> shift) & 3];
            }
        }

        if (rlcd_shadow_window_matches(display, rows, y_pos, send_start, send_count)) {
            return 1;
        }

        const uint8_t col_bounds[] = {
            (uint8_t)(0x3C - addr_end),
            (uint8_t)(0x3C - addr_start),
        };
        if (!rlcd_checked_cmd_data(display, 0x2A, col_bounds, sizeof(col_bounds))) {
            return 0;
        }

        const uint8_t row_bounds[] = {
            (uint8_t)(y_pos * RLCD_CONTROLLER_ROWS_PER_TILE),
            (uint8_t)(y_pos * RLCD_CONTROLLER_ROWS_PER_TILE + RLCD_CONTROLLER_ROWS_PER_TILE - 1),
        };
        if (!rlcd_checked_cmd_data(display, 0x2B, row_bounds, sizeof(row_bounds))) {
            return 0;
        }

        if (!rlcd_checked_cmd_data(display,
                                   0x2C,
                                   rows,
                                   (size_t)send_count * RLCD_CONTROLLER_ROWS_PER_TILE)) {
            return 0;
        }

        rlcd_shadow_update_window(display, rows, y_pos, send_start, send_count);
        return 1;
    }

    default:
        return 0;
    }
}

esp_err_t rlcd_st7305_init(rlcd_st7305_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->last_error = ESP_OK;
    display->controller_mode = "default";

    ESP_RETURN_ON_ERROR(rlcd_configure_control_pins(), TAG, "control pin config failed");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = SOLAR_OS_BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = SOLAR_OS_BOARD_PIN_LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RLCD_MAX_TRANSFER_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(RLCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init failed");
    display->bus_initialized = true;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = RLCD_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(RLCD_SPI_HOST, &device_config, &display->spi), TAG,
                        "spi add device failed");

    display->buffer_size = RLCD_BUFFER_ROW_BYTES * RLCD_TILE_HEIGHT;
    display->buffer = heap_caps_malloc(display->buffer_size, MALLOC_CAP_8BIT);
    if (display->buffer == NULL) {
        rlcd_st7305_deinit(display);
        return ESP_ERR_NO_MEM;
    }
    memset(display->buffer, 0, display->buffer_size);

    display->shadow_size = RLCD_SHADOW_BYTES;
    display->shadow = heap_caps_malloc(display->shadow_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display->shadow == NULL) {
        display->shadow = heap_caps_malloc(display->shadow_size, MALLOC_CAP_8BIT);
    }
    if (display->shadow == NULL) {
        ESP_LOGW(TAG, "display shadow allocation failed, partial update skipping disabled");
        display->shadow_size = 0;
    } else {
        memset(display->shadow, 0, display->shadow_size);
        rlcd_invalidate_shadow(display);
    }

    u8g2_SetupDisplay(&display->u8g2, rlcd_u8x8_display_cb, u8x8_dummy_cb,
                      rlcd_u8x8_byte_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2, display->buffer, RLCD_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R1);
    active_display = display;
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);

    return display->last_error;
}

esp_err_t rlcd_st7305_resume(rlcd_st7305_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(rlcd_configure_control_pins(), TAG, "resume pin config failed");
    active_display = display;
    display->last_error = ESP_OK;
    rlcd_invalidate_shadow(display);
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    return display->last_error;
}

void rlcd_st7305_deinit(rlcd_st7305_t *display)
{
    if (display == NULL) {
        return;
    }

    if (display->spi != NULL) {
        spi_bus_remove_device(display->spi);
        display->spi = NULL;
    }

    if (display->bus_initialized) {
        spi_bus_free(RLCD_SPI_HOST);
        display->bus_initialized = false;
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
    display->shadow_valid_rows = 0;
}

u8g2_t *rlcd_st7305_get_u8g2(rlcd_st7305_t *display)
{
    return display == NULL ? NULL : &display->u8g2;
}

const char *rlcd_st7305_controller_mode(const rlcd_st7305_t *display)
{
    return display != NULL && display->controller_mode != NULL ?
        display->controller_mode :
        "default";
}

const char *rlcd_st7305_controller_mode_values(void)
{
    return RLCD_CONTROLLER_MODE_VALUES;
}

esp_err_t rlcd_st7305_set_controller_mode(rlcd_st7305_t *display, const char *mode)
{
    if (display == NULL || display->spi == NULL || mode == NULL || mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const rlcd_controller_profile_t *profile = rlcd_find_controller_profile(mode);
    if (profile == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const rlcd_controller_profile_t *current = rlcd_current_controller_profile(display);
    if (current == profile) {
        return ESP_OK;
    }
    if (rlcd_profiles_differ_only_by_power(current, profile)) {
        return rlcd_apply_controller_power_mode(display, profile);
    }

    return rlcd_apply_controller_profile(display, profile, false);
}
