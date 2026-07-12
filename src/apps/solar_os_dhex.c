#include "solar_os_dhex.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs.h"
#include "solar_os_board.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_uart.h"

/* Same physical UART the aqm app parses PM1.0/2.5/10 frames from --
 * dhex just shows the raw bytes. Port/pins/baud/framing all used to be
 * fixed at compile time (SOLAR_OS_BOARD_PM_UART_*, 9600 8E1); now
 * they're runtime-configurable via launch args and persisted in NVS
 * (see dhex_uart_config_t below), with the board macros only used as
 * the very first, never-configured-yet default. */
#define DHEX_NVS_NAMESPACE "dhex"
#define DHEX_UART_DRIVER_RX_BYTES 1024U
#define DHEX_POLL_CHUNK 64U

/* Exactly 16 bytes, no more, no less: 2 rows of 8 in the hex/ascii
 * ("wireshark-style") view up top, 16 columns in the big-ascii strip
 * at the bottom. */
#define DHEX_BUFFER_SIZE 16U
#define DHEX_ROW_BYTES 8U

/* Wireshark section stays on a MONO font -- it needs consistent
 * character width for the hex/address column grid to line up, and
 * SolarOS has no "bold mono" variant. The single-char-per-column big
 * ascii row isn't grid-sensitive the same way, so it gets the bolder,
 * heavier BOLD font instead of MONO for better legibility. */
#define DHEX_SMALL_FONT SOLAR_OS_GFX_FONT_MONO_14
#define DHEX_BIG_FONT SOLAR_OS_GFX_FONT_BOLD_20
#define DHEX_TOP_MARGIN 4
#define DHEX_ROW_HEIGHT 18
#define DHEX_ADDR_GAP 4
#define DHEX_HEX_ASCII_GAP 10
#define DHEX_SECTION_GAP 12
#define DHEX_BOTTOM_LINE_HEIGHT 14

/* Header: "dhex" title on the left, serial params + RX/TX pins on the
 * right (two small lines), a divider line under the whole thing. */
#define DHEX_TITLE_FONT SOLAR_OS_GFX_FONT_BOLD_18
#define DHEX_HEADER_MARGIN 4
#define DHEX_HEADER_TITLE_BASELINE 22
#define DHEX_HEADER_LINE1_BASELINE 14
#define DHEX_HEADER_LINE2_BASELINE 30
#define DHEX_HEADER_HEIGHT 34

static const char *TAG = "solar_os_dhex";

/* Arduino-style "8E1" framing spec: data bits (5-8), parity (N/E/O),
 * stop bits (1-2) -- the exact notation the reference project this was
 * ported from already used (SERIAL_8E1), so launch args mirror
 * Serial2.begin(baud, config, rx, tx)'s argument order instead of
 * inventing new flag names. */
typedef struct {
    int port;
    int tx_pin;
    int rx_pin;
    uint32_t baud;
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
} dhex_uart_config_t;

typedef struct {
    bool uart_ready;
    dhex_uart_config_t active_config;
    uint8_t buffer[DHEX_BUFFER_SIZE];
    size_t total_received;
    size_t last_rendered_total;
} dhex_state_t;

static dhex_state_t dhex_state;

static void dhex_config_defaults(dhex_uart_config_t *cfg)
{
    cfg->port = (int)SOLAR_OS_BOARD_PM_UART_PORT;
    cfg->tx_pin = (int)SOLAR_OS_BOARD_PIN_PM_UART_TX;
    cfg->rx_pin = (int)SOLAR_OS_BOARD_PIN_PM_UART_RX;
    cfg->baud = 9600U;
    cfg->data_bits = 8U;
    cfg->parity = 'E';
    cfg->stop_bits = 1U;
}

static void dhex_config_load(dhex_uart_config_t *cfg)
{
    dhex_config_defaults(cfg);

    nvs_handle_t nvs;
    if (nvs_open(DHEX_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    int32_t pin_value = 0;
    if (nvs_get_i32(nvs, "port", &pin_value) == ESP_OK) {
        cfg->port = (int)pin_value;
    }
    if (nvs_get_i32(nvs, "tx", &pin_value) == ESP_OK) {
        cfg->tx_pin = (int)pin_value;
    }
    if (nvs_get_i32(nvs, "rx", &pin_value) == ESP_OK) {
        cfg->rx_pin = (int)pin_value;
    }
    uint32_t baud_value = 0;
    if (nvs_get_u32(nvs, "baud", &baud_value) == ESP_OK) {
        cfg->baud = baud_value;
    }
    uint8_t byte_value = 0;
    if (nvs_get_u8(nvs, "bits", &byte_value) == ESP_OK) {
        cfg->data_bits = byte_value;
    }
    if (nvs_get_u8(nvs, "parity", &byte_value) == ESP_OK) {
        cfg->parity = (char)byte_value;
    }
    if (nvs_get_u8(nvs, "stop", &byte_value) == ESP_OK) {
        cfg->stop_bits = byte_value;
    }

    nvs_close(nvs);
}

static esp_err_t dhex_config_save(const dhex_uart_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(DHEX_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_i32(nvs, "port", cfg->port);
    if (ret == ESP_OK) {
        ret = nvs_set_i32(nvs, "tx", cfg->tx_pin);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_i32(nvs, "rx", cfg->rx_pin);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u32(nvs, "baud", cfg->baud);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "bits", cfg->data_bits);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "parity", (uint8_t)cfg->parity);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "stop", cfg->stop_bits);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static bool dhex_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool dhex_parse_framing(const char *text, uint8_t *data_bits, char *parity, uint8_t *stop_bits)
{
    if (text == NULL || strlen(text) != 3) {
        return false;
    }
    if (text[0] < '5' || text[0] > '8') {
        return false;
    }
    const char p = (char)toupper((unsigned char)text[1]);
    if (p != 'N' && p != 'E' && p != 'O') {
        return false;
    }
    if (text[2] != '1' && text[2] != '2') {
        return false;
    }

    *data_bits = (uint8_t)(text[0] - '0');
    *parity = p;
    *stop_bits = (uint8_t)(text[2] - '0');
    return true;
}

/* dhex                              -- use the saved (or default) config
 * dhex <baud> <framing> <rx> <tx> [port] -- e.g. "dhex 9600 8E1 13 14",
 * mirroring Serial2.begin(baud, config, rxPin, txPin)'s argument order.
 * Applying new args always saves them, so a bare "dhex" next time
 * reuses whatever was last configured. */
static bool dhex_parse_args(solar_os_context_t *ctx, dhex_uart_config_t *cfg)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc == 1) {
        dhex_config_load(cfg);
        return true;
    }

    if (argc != 5 && argc != 6) {
        return false;
    }

    dhex_uart_config_t parsed;
    dhex_config_defaults(&parsed);

    uint32_t baud = 0;
    if (!dhex_parse_u32(solar_os_context_argv(ctx, 1),
                        SOLAR_OS_UART_MIN_BAUD_RATE,
                        SOLAR_OS_UART_MAX_BAUD_RATE,
                        &baud)) {
        return false;
    }
    parsed.baud = baud;

    if (!dhex_parse_framing(solar_os_context_argv(ctx, 2),
                            &parsed.data_bits,
                            &parsed.parity,
                            &parsed.stop_bits)) {
        return false;
    }

    uint32_t rx_pin = 0;
    uint32_t tx_pin = 0;
    if (!dhex_parse_u32(solar_os_context_argv(ctx, 3), 0, GPIO_NUM_MAX - 1, &rx_pin) ||
        !dhex_parse_u32(solar_os_context_argv(ctx, 4), 0, GPIO_NUM_MAX - 1, &tx_pin)) {
        return false;
    }
    parsed.rx_pin = (int)rx_pin;
    parsed.tx_pin = (int)tx_pin;

    if (argc == 6) {
        uint32_t port = 0;
        if (!dhex_parse_u32(solar_os_context_argv(ctx, 5), 0, SOC_UART_NUM - 1, &port)) {
            return false;
        }
        parsed.port = (int)port;
    }

    if (dhex_config_save(&parsed) != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "failed to save config to NVS, using it for this run only");
    }
    *cfg = parsed;
    return true;
}

static uart_word_length_t dhex_data_bits_enum(uint8_t bits)
{
    switch (bits) {
        case 5: return UART_DATA_5_BITS;
        case 6: return UART_DATA_6_BITS;
        case 7: return UART_DATA_7_BITS;
        default: return UART_DATA_8_BITS;
    }
}

static uart_parity_t dhex_parity_enum(char parity)
{
    switch (parity) {
        case 'E': return UART_PARITY_EVEN;
        case 'O': return UART_PARITY_ODD;
        default: return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t dhex_stop_bits_enum(uint8_t bits)
{
    return bits >= 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static esp_err_t dhex_uart_start(const dhex_uart_config_t *cfg)
{
    const uart_port_t port = (uart_port_t)cfg->port;

    esp_err_t ret = uart_driver_install(port, DHEX_UART_DRIVER_RX_BYTES, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    const uart_config_t config = {
        .baud_rate = (int)cfg->baud,
        .data_bits = dhex_data_bits_enum(cfg->data_bits),
        .parity = dhex_parity_enum(cfg->parity),
        .stop_bits = dhex_stop_bits_enum(cfg->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ret = uart_param_config(port, &config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(port,
                           (gpio_num_t)cfg->tx_pin,
                           (gpio_num_t)cfg->rx_pin,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(port);
        return ret;
    }

    return ESP_OK;
}

static void dhex_uart_stop(void)
{
    if (dhex_state.uart_ready) {
        uart_driver_delete((uart_port_t)dhex_state.active_config.port);
    }
}

static void dhex_poll_uart(void)
{
    if (!dhex_state.uart_ready) {
        return;
    }

    uint8_t chunk[DHEX_POLL_CHUNK];
    int len = uart_read_bytes((uart_port_t)dhex_state.active_config.port, chunk, sizeof(chunk), 0);
    for (int i = 0; i < len; i++) {
        dhex_state.buffer[dhex_state.total_received % DHEX_BUFFER_SIZE] = chunk[i];
        dhex_state.total_received++;
    }
}

/* Oldest-to-newest logical index -> physical ring buffer slot. Index 0
 * is the oldest byte currently held (or the first not-yet-received
 * slot, if fewer than 16 bytes have arrived so far). */
static size_t dhex_ring_index(size_t logical_index)
{
    const size_t total = dhex_state.total_received;
    const size_t base = total >= DHEX_BUFFER_SIZE ? total : DHEX_BUFFER_SIZE;
    return (base - DHEX_BUFFER_SIZE + logical_index) % DHEX_BUFFER_SIZE;
}

static bool dhex_slot_filled(size_t logical_index)
{
    const size_t total = dhex_state.total_received;
    if (total >= DHEX_BUFFER_SIZE) {
        return true;
    }
    return logical_index < total;
}

static void dhex_draw_wireshark_row(solar_os_gfx_t *gfx, size_t row, int y, int hex_x, int ascii_x, int hex_col_w, int ascii_col_w)
{
    char addr[8];
    const size_t base_offset = dhex_state.total_received >= DHEX_BUFFER_SIZE ?
        dhex_state.total_received - DHEX_BUFFER_SIZE :
        0;
    snprintf(addr, sizeof(addr), "%04X:", (unsigned)((base_offset + (row * DHEX_ROW_BYTES)) & 0xFFFFU));
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_text(gfx, 0, y, addr);

    for (size_t col = 0; col < DHEX_ROW_BYTES; col++) {
        const size_t logical = (row * DHEX_ROW_BYTES) + col;
        const int x = hex_x + (int)(col * (size_t)hex_col_w);
        if (!dhex_slot_filled(logical)) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_text(gfx, x, y, "--");
            continue;
        }

        const uint8_t b = dhex_state.buffer[dhex_ring_index(logical)];
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", b);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_text(gfx, x, y, hex);
    }

    for (size_t col = 0; col < DHEX_ROW_BYTES; col++) {
        const size_t logical = (row * DHEX_ROW_BYTES) + col;
        const int x = ascii_x + (int)(col * (size_t)ascii_col_w);
        if (!dhex_slot_filled(logical)) {
            continue;
        }

        const uint8_t b = dhex_state.buffer[dhex_ring_index(logical)];
        char sym[2] = {0, 0};
        if (b == 0x0D) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            sym[0] = 'C';
        } else if (b == 0x0A) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            sym[0] = 'L';
        } else if (b >= 0x20 && b <= 0x7E) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            sym[0] = (char)b;
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            sym[0] = '.';
        }
        solar_os_gfx_text(gfx, x, y, sym);
    }
}

static void dhex_draw_wireshark_section(solar_os_gfx_t *gfx, int top_y, int hex_x, int ascii_x, int hex_col_w, int ascii_col_w)
{
    solar_os_gfx_set_font(gfx, DHEX_SMALL_FONT);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_text(gfx, 0, top_y, "Addr:");
    for (size_t col = 0; col < DHEX_ROW_BYTES; col++) {
        char header[3];
        snprintf(header, sizeof(header), "%02X", (unsigned)col);
        solar_os_gfx_text(gfx, hex_x + (int)(col * (size_t)hex_col_w), top_y, header);
    }

    int y = top_y + DHEX_ROW_HEIGHT;
    for (size_t row = 0; row < (DHEX_BUFFER_SIZE / DHEX_ROW_BYTES); row++) {
        dhex_draw_wireshark_row(gfx, row, y, hex_x, ascii_x, hex_col_w, ascii_col_w);
        y += DHEX_ROW_HEIGHT;
    }
}

static void dhex_draw_big_ascii_section(solar_os_gfx_t *gfx, int y, int col_w)
{
    for (size_t i = 0; i < DHEX_BUFFER_SIZE; i++) {
        const int x = (int)(i * (size_t)col_w);
        if (!dhex_slot_filled(i)) {
            continue;
        }

        const uint8_t b = dhex_state.buffer[dhex_ring_index(i)];
        if (b == 0x0D || b == 0x0A) {
            solar_os_gfx_set_font(gfx, DHEX_SMALL_FONT);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            const char *top = b == 0x0D ? "C" : "L";
            const char *bottom = b == 0x0D ? "R" : "F";
            solar_os_gfx_text(gfx, x, y - DHEX_BOTTOM_LINE_HEIGHT, top);
            solar_os_gfx_text(gfx, x, y, bottom);
        } else if (b >= 0x20 && b <= 0x7E) {
            solar_os_gfx_set_font(gfx, DHEX_BIG_FONT);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            char sym[2] = {(char)b, 0};
            solar_os_gfx_text(gfx, x, y, sym);
        } else {
            solar_os_gfx_set_font(gfx, DHEX_SMALL_FONT);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_text(gfx, x, y, ".");
        }
    }
}

static void dhex_draw_header(solar_os_gfx_t *gfx)
{
    const int screen_width = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_set_font(gfx, DHEX_TITLE_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, DHEX_HEADER_MARGIN, DHEX_HEADER_TITLE_BASELINE, "dhex");

    const dhex_uart_config_t *cfg = &dhex_state.active_config;
    char params[24];
    snprintf(params,
            sizeof(params),
            "UART%d %u,%u%c%u",
            cfg->port,
            (unsigned)cfg->baud,
            (unsigned)cfg->data_bits,
            cfg->parity,
            (unsigned)cfg->stop_bits);
    char pins[24];
    snprintf(pins, sizeof(pins), "RX%d TX%d", cfg->rx_pin, cfg->tx_pin);

    solar_os_gfx_set_font(gfx, DHEX_SMALL_FONT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    const int params_w = (int)solar_os_gfx_text_width(gfx, params);
    const int pins_w = (int)solar_os_gfx_text_width(gfx, pins);
    solar_os_gfx_text(gfx,
                      screen_width - DHEX_HEADER_MARGIN - params_w,
                      DHEX_HEADER_LINE1_BASELINE,
                      params);
    solar_os_gfx_text(gfx,
                      screen_width - DHEX_HEADER_MARGIN - pins_w,
                      DHEX_HEADER_LINE2_BASELINE,
                      pins);

    solar_os_gfx_line(gfx, 0, DHEX_HEADER_HEIGHT, screen_width - 1, DHEX_HEADER_HEIGHT);
}

static void dhex_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    dhex_draw_header(gfx);

    if (!dhex_state.uart_ready) {
        solar_os_gfx_set_font(gfx, DHEX_SMALL_FONT);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_text(gfx, DHEX_HEADER_MARGIN, DHEX_HEADER_HEIGHT + DHEX_ROW_HEIGHT, "uart unavailable");
        solar_os_gfx_present(gfx);
        return;
    }

    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_set_font(gfx, DHEX_SMALL_FONT);
    const int addr_w = (int)solar_os_gfx_text_width(gfx, "0000:") + DHEX_ADDR_GAP;
    const int hex_col_w = (int)solar_os_gfx_text_width(gfx, "00 ");
    const int hex_x = addr_w;
    const int ascii_x = hex_x + (int)(DHEX_ROW_BYTES * (size_t)hex_col_w) + DHEX_HEX_ASCII_GAP;
    const int ascii_col_w = (int)solar_os_gfx_text_width(gfx, "X ");

    const int top_y = DHEX_HEADER_HEIGHT + DHEX_TOP_MARGIN + DHEX_ROW_HEIGHT;
    dhex_draw_wireshark_section(gfx, top_y, hex_x, ascii_x, hex_col_w, ascii_col_w);

    const int big_col_w = screen_width / (int)DHEX_BUFFER_SIZE;
    const int big_y = screen_height - (DHEX_SECTION_GAP / 2);
    dhex_draw_big_ascii_section(gfx, big_y, big_col_w);

    solar_os_gfx_present(gfx);
    dhex_state.last_rendered_total = dhex_state.total_received;
}

static esp_err_t dhex_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    dhex_uart_config_t cfg;
    if (!dhex_parse_args(ctx, &cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&dhex_state, 0, sizeof(dhex_state));
    dhex_state.active_config = cfg;

    const esp_err_t ret = dhex_uart_start(&cfg);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "UART start failed: %s", esp_err_to_name(ret));
    } else {
        dhex_state.uart_ready = true;
        SOLAR_OS_LOGI(TAG,
                     "dhex ready: UART%d TX=%d RX=%d %u,%u%c%u",
                     cfg.port,
                     cfg.tx_pin,
                     cfg.rx_pin,
                     (unsigned)cfg.baud,
                     (unsigned)cfg.data_bits,
                     cfg.parity,
                     (unsigned)cfg.stop_bits);
    }

    solar_os_context_set_graphics_active(ctx, true);
    dhex_render(ctx);
    return ESP_OK;
}

static void dhex_stop(solar_os_context_t *ctx)
{
    dhex_uart_stop();
    solar_os_context_set_graphics_active(ctx, false);
    memset(&dhex_state, 0, sizeof(dhex_state));
}

static void dhex_suspend(solar_os_context_t *ctx)
{
    /* Leave the UART driver running so bytes keep arriving in its own
     * ring buffer while this app isn't the active session. */
    solar_os_context_set_graphics_active(ctx, false);
}

static void dhex_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    dhex_render(ctx);
}

static void dhex_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    strlcpy(buffer, "dhex", buffer_len);
}

static bool dhex_event(solar_os_context_t *ctx, const solar_os_event_t *event)
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
        dhex_poll_uart();
        if (dhex_state.total_received != dhex_state.last_rendered_total) {
            dhex_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        dhex_resume(ctx);
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_dhex_app = {
    .name = "dhex",
    .summary = "hex/ascii UART dump; usage: dhex [baud framing rx tx [port]] e.g. dhex 9600 8E1 13 14",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = dhex_start,
    .suspend = dhex_suspend,
    .resume = dhex_resume,
    .stop = dhex_stop,
    .event = dhex_event,
    .title = dhex_title,
};
