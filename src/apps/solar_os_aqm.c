#include "solar_os_aqm.h"

#include <stdio.h>
#include <string.h>

#include "co2_scd41.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "solar_os_board.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"

/* Same physical UART/protocol as dhex (see solar_os_dhex.c) -- this
 * app just parses the particle sensor's framed packets instead of
 * dumping raw bytes. Owns its own instance of the UART driver while
 * running, same as dhex; the two apps aren't meant to run at once. */
#define AQM_UART_BAUD 9600U
#define AQM_UART_DRIVER_RX_BYTES 1024U
#define AQM_POLL_CHUNK 64U

/* Particle sensor packet: 32 bytes, STX(0x02) ... ETX(0x03), PM1.0/2.5/
 * 10 as little-endian 16-bit values at fixed offsets -- matches the
 * reference firmware's frame sync (a plain 32-byte sliding window is
 * equivalent to that code's oversized linear buffer + modulo indexing,
 * just without the unnecessary 2047-byte buffer). */
#define PM_FRAME_LEN 32U
#define PM_FRAME_STX 0x02U
#define PM_FRAME_ETX 0x03U

#define CO2_POLL_INTERVAL_MS 3000U

#define AQM_SMALL_FONT SOLAR_OS_GFX_FONT_MONO_14
#define AQM_LABEL_FONT SOLAR_OS_GFX_FONT_BOLD_14
#define AQM_BIG_FONT SOLAR_OS_GFX_FONT_BOLD_20
#define AQM_TITLE_FONT SOLAR_OS_GFX_FONT_BOLD_18
#define AQM_HEADER_MARGIN 4
#define AQM_HEADER_TITLE_BASELINE 30
#define AQM_HEADER_LINE1_BASELINE 12
#define AQM_HEADER_LINE2_BASELINE 26
#define AQM_HEADER_LINE3_BASELINE 40
#define AQM_HEADER_HEIGHT 46
#define AQM_ROW_LABEL_X 8
#define AQM_ROW_VALUE_MARGIN 8
#define AQM_ROW_COUNT 6U

static const char *TAG = "solar_os_aqm";

typedef struct {
    bool uart_ready;
    uint8_t pm_window[PM_FRAME_LEN];
    uint32_t pm_bytes_seen;
    bool pm_valid;
    uint16_t pm01;
    uint16_t pm25;
    uint16_t pm10;

    bool co2_ready;
    bool co2_valid;
    float co2_ppm;
    float co2_temperature_c;
    float co2_humidity_pct;
    uint32_t co2_last_poll_ms;

    bool dirty;
} aqm_state_t;

static aqm_state_t aqm_state;

static uint32_t aqm_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t aqm_uart_start(void)
{
    esp_err_t ret = uart_driver_install(SOLAR_OS_BOARD_PM_UART_PORT,
                                        AQM_UART_DRIVER_RX_BYTES,
                                        0,
                                        0,
                                        NULL,
                                        0);
    if (ret != ESP_OK) {
        return ret;
    }

    const uart_config_t config = {
        .baud_rate = AQM_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ret = uart_param_config(SOLAR_OS_BOARD_PM_UART_PORT, &config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(SOLAR_OS_BOARD_PM_UART_PORT,
                           SOLAR_OS_BOARD_PIN_PM_UART_TX,
                           SOLAR_OS_BOARD_PIN_PM_UART_RX,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(SOLAR_OS_BOARD_PM_UART_PORT);
        return ret;
    }

    return ESP_OK;
}

static void aqm_uart_stop(void)
{
    if (aqm_state.uart_ready) {
        uart_driver_delete(SOLAR_OS_BOARD_PM_UART_PORT);
    }
}

static void aqm_pm_feed_byte(uint8_t b)
{
    memmove(aqm_state.pm_window, aqm_state.pm_window + 1, PM_FRAME_LEN - 1);
    aqm_state.pm_window[PM_FRAME_LEN - 1] = b;
    if (aqm_state.pm_bytes_seen < PM_FRAME_LEN) {
        aqm_state.pm_bytes_seen++;
        return;
    }

    if (aqm_state.pm_window[0] == PM_FRAME_STX && aqm_state.pm_window[PM_FRAME_LEN - 1] == PM_FRAME_ETX) {
        aqm_state.pm01 = (uint16_t)(aqm_state.pm_window[2] * 256U + aqm_state.pm_window[1]);
        aqm_state.pm25 = (uint16_t)(aqm_state.pm_window[6] * 256U + aqm_state.pm_window[5]);
        aqm_state.pm10 = (uint16_t)(aqm_state.pm_window[10] * 256U + aqm_state.pm_window[9]);
        aqm_state.pm_valid = true;
        aqm_state.dirty = true;
    }
}

static void aqm_poll_pm(void)
{
    if (!aqm_state.uart_ready) {
        return;
    }

    uint8_t chunk[AQM_POLL_CHUNK];
    const int len = uart_read_bytes(SOLAR_OS_BOARD_PM_UART_PORT, chunk, sizeof(chunk), 0);
    for (int i = 0; i < len; i++) {
        aqm_pm_feed_byte(chunk[i]);
    }
}

static void aqm_poll_co2(void)
{
    if (!aqm_state.co2_ready) {
        return;
    }

    const uint32_t now_ms = aqm_now_ms();
    if (aqm_state.co2_last_poll_ms != 0 && (now_ms - aqm_state.co2_last_poll_ms) < CO2_POLL_INTERVAL_MS) {
        return;
    }
    aqm_state.co2_last_poll_ms = now_ms;

    co2_scd41_reading_t reading;
    const esp_err_t ret = co2_scd41_read(&reading);
    if (ret != ESP_OK || !reading.valid) {
        return;
    }

    aqm_state.co2_ppm = reading.co2_ppm;
    aqm_state.co2_temperature_c = reading.temperature_c;
    aqm_state.co2_humidity_pct = reading.humidity_pct;
    aqm_state.co2_valid = true;
    aqm_state.dirty = true;
}

static void aqm_draw_header(solar_os_gfx_t *gfx)
{
    const int screen_width = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_set_font(gfx, AQM_TITLE_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, AQM_HEADER_MARGIN, AQM_HEADER_TITLE_BASELINE, "aqm");

    char params[24];
    snprintf(params, sizeof(params), "%u,8,E,1", AQM_UART_BAUD);
    char uart_pins[24];
    snprintf(uart_pins,
            sizeof(uart_pins),
            "RX%d TX%d",
            (int)SOLAR_OS_BOARD_PIN_PM_UART_RX,
            (int)SOLAR_OS_BOARD_PIN_PM_UART_TX);
    char i2c_pins[24];
    snprintf(i2c_pins,
            sizeof(i2c_pins),
            "SDA%d SCL%d",
            (int)SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA,
            (int)SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL);

    solar_os_gfx_set_font(gfx, AQM_SMALL_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    const int params_w = (int)solar_os_gfx_text_width(gfx, params);
    const int uart_pins_w = (int)solar_os_gfx_text_width(gfx, uart_pins);
    const int i2c_pins_w = (int)solar_os_gfx_text_width(gfx, i2c_pins);
    solar_os_gfx_text(gfx, screen_width - AQM_HEADER_MARGIN - params_w, AQM_HEADER_LINE1_BASELINE, params);
    solar_os_gfx_text(gfx, screen_width - AQM_HEADER_MARGIN - uart_pins_w, AQM_HEADER_LINE2_BASELINE, uart_pins);
    solar_os_gfx_text(gfx, screen_width - AQM_HEADER_MARGIN - i2c_pins_w, AQM_HEADER_LINE3_BASELINE, i2c_pins);

    solar_os_gfx_line(gfx, 0, AQM_HEADER_HEIGHT, screen_width - 1, AQM_HEADER_HEIGHT);
}

static void aqm_draw_row(solar_os_gfx_t *gfx, int y, int screen_width, const char *label, const char *value)
{
    solar_os_gfx_set_font(gfx, AQM_LABEL_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_text(gfx, AQM_ROW_LABEL_X, y, label);

    solar_os_gfx_set_font(gfx, AQM_BIG_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const int value_w = (int)solar_os_gfx_text_width(gfx, value);
    solar_os_gfx_text(gfx, screen_width - AQM_ROW_VALUE_MARGIN - value_w, y, value);
}

static void aqm_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    aqm_draw_header(gfx);

    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);
    const int body_top = AQM_HEADER_HEIGHT + 8;
    const int body_height = screen_height - body_top;
    const int row_height = body_height / (int)AQM_ROW_COUNT;

    char value[24];
    int y = body_top + row_height - 8;

    if (aqm_state.co2_valid) {
        snprintf(value, sizeof(value), "%u ppm", (unsigned)aqm_state.co2_ppm);
    } else {
        strlcpy(value, "--", sizeof(value));
    }
    aqm_draw_row(gfx, y, screen_width, "CO2", value);
    y += row_height;

    if (aqm_state.co2_valid) {
        snprintf(value, sizeof(value), "%.1f C", (double)aqm_state.co2_temperature_c);
    } else {
        strlcpy(value, "--", sizeof(value));
    }
    aqm_draw_row(gfx, y, screen_width, "Temp", value);
    y += row_height;

    if (aqm_state.co2_valid) {
        snprintf(value, sizeof(value), "%.0f %%", (double)aqm_state.co2_humidity_pct);
    } else {
        strlcpy(value, "--", sizeof(value));
    }
    aqm_draw_row(gfx, y, screen_width, "RH", value);
    y += row_height;

    if (aqm_state.pm_valid) {
        snprintf(value, sizeof(value), "%u ug/m3", (unsigned)aqm_state.pm01);
    } else {
        strlcpy(value, "--", sizeof(value));
    }
    aqm_draw_row(gfx, y, screen_width, "PM1.0", value);
    y += row_height;

    if (aqm_state.pm_valid) {
        snprintf(value, sizeof(value), "%u ug/m3", (unsigned)aqm_state.pm25);
    } else {
        strlcpy(value, "--", sizeof(value));
    }
    aqm_draw_row(gfx, y, screen_width, "PM2.5", value);
    y += row_height;

    if (aqm_state.pm_valid) {
        snprintf(value, sizeof(value), "%u ug/m3", (unsigned)aqm_state.pm10);
    } else {
        strlcpy(value, "--", sizeof(value));
    }
    aqm_draw_row(gfx, y, screen_width, "PM10", value);

    solar_os_gfx_present(gfx);
    aqm_state.dirty = false;
}

static esp_err_t aqm_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&aqm_state, 0, sizeof(aqm_state));

    esp_err_t ret = aqm_uart_start();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "PM sensor UART start failed: %s", esp_err_to_name(ret));
    } else {
        aqm_state.uart_ready = true;
    }

    ret = co2_scd41_init();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "SCD41 init failed: %s", esp_err_to_name(ret));
    } else {
        aqm_state.co2_ready = true;
    }

    solar_os_context_set_graphics_active(ctx, true);
    aqm_render(ctx);
    return ESP_OK;
}

static void aqm_stop(solar_os_context_t *ctx)
{
    aqm_uart_stop();
    solar_os_context_set_graphics_active(ctx, false);
    memset(&aqm_state, 0, sizeof(aqm_state));
}

static void aqm_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void aqm_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    aqm_render(ctx);
}

static void aqm_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    strlcpy(buffer, "aqm", buffer_len);
}

static bool aqm_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        aqm_poll_pm();
        aqm_poll_co2();
        if (aqm_state.dirty) {
            aqm_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        aqm_resume(ctx);
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_aqm_app = {
    .name = "aqm",
    .summary = "air quality monitor: particle sensor + SCD41 CO2/temp/RH",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = aqm_start,
    .suspend = aqm_suspend,
    .resume = aqm_resume,
    .stop = aqm_stop,
    .event = aqm_event,
    .title = aqm_title,
};
