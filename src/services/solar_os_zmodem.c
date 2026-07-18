#include "solar_os_zmodem.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_memory.h"

#define ZM_SEND_CHUNK_SIZE 512U
#define ZM_RECV_CHUNK_SIZE 1024U
#define ZM_FILEINFO_MAX 256U
#define ZM_RX_BUFFER_SIZE 2048U
#define ZM_READ_POLL_MS 10U
#define ZM_HEADER_TIMEOUT_MS 1000U
#define ZM_SESSION_TIMEOUT_MS 60000U
#define ZM_PROGRESS_STEP_BYTES (16U * 1024U)

#define ZPAD '*'
#define ZDLE 0x18
#define ZBIN 'A'
#define ZHEX 'B'
#define ZBIN32 'C'

#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define ZRUB0 'l'
#define ZRUB1 'm'

#define ZRQINIT 0
#define ZRINIT 1
#define ZSINIT 2
#define ZACK 3
#define ZFILE 4
#define ZSKIP 5
#define ZNAK 6
#define ZABORT 7
#define ZFIN 8
#define ZRPOS 9
#define ZDATA 10
#define ZEOF 11
#define ZFERR 12
#define ZCRC 13

#define CANFDX 0x01U
#define CANOVIO 0x02U
#define CANFC32 0x20U

typedef enum {
    ZM_CRC16,
    ZM_CRC32,
} zm_crc_mode_t;

typedef struct {
    uint8_t type;
    uint32_t pos;
    zm_crc_mode_t crc_mode;
} zm_header_t;

typedef struct {
    const solar_os_transfer_options_t *options;
    const solar_os_port_handle_t *port;
    uint8_t *rx_buffer;
    size_t rx_capacity;
    size_t rx_pos;
    size_t rx_len;
} zm_session_t;

static esp_err_t zm_session_init(zm_session_t *zm,
                                 const solar_os_transfer_options_t *options,
                                 const solar_os_port_handle_t *port)
{
    if (zm == NULL || options == NULL || port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(zm, 0, sizeof(*zm));
    zm->options = options;
    zm->port = port;
    zm->rx_buffer = solar_os_memory_alloc(ZM_RX_BUFFER_SIZE,
                                           SOLAR_OS_MEMORY_TRANSIENT,
                                           "zmodem.rx");
    if (zm->rx_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    zm->rx_capacity = ZM_RX_BUFFER_SIZE;
    return ESP_OK;
}

static void zm_session_deinit(zm_session_t *zm)
{
    if (zm == NULL) {
        return;
    }

    solar_os_memory_free(zm->rx_buffer);
    zm->rx_buffer = NULL;
    zm->rx_capacity = 0;
    zm->rx_pos = 0;
    zm->rx_len = 0;
}

static bool zm_should_cancel(const zm_session_t *zm)
{
    return zm != NULL &&
        zm->options != NULL &&
        zm->options->should_cancel != NULL &&
        zm->options->should_cancel(zm->options->user);
}

static void zm_report_progress(const zm_session_t *zm, uint64_t bytes, uint64_t *next_progress)
{
    if (zm == NULL ||
        zm->options == NULL ||
        zm->options->progress == NULL ||
        next_progress == NULL ||
        bytes < *next_progress) {
        return;
    }

    zm->options->progress(bytes, zm->options->user);
    *next_progress = bytes + ZM_PROGRESS_STEP_BYTES;
}

static uint16_t zm_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x8000U) ?
            (uint16_t)((crc << 1) ^ 0x1021U) :
            (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t zm_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        crc = zm_crc16_update(crc, data[i]);
    }
    return crc;
}

static uint32_t zm_crc32_update_raw(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 1U) ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
    }
    return crc;
}

static uint32_t zm_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffUL;

    for (size_t i = 0; i < len; i++) {
        crc = zm_crc32_update_raw(crc, data[i]);
    }
    return ~crc;
}

static esp_err_t zm_write_all(const zm_session_t *zm, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        if (zm_should_cancel(zm)) {
            return ESP_ERR_INVALID_STATE;
        }
        size_t written = 0;
        const esp_err_t err = solar_os_port_write(zm->port, &data[offset], len - offset, &written);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_FAIL;
        }
        offset += written;
    }
    return ESP_OK;
}

static esp_err_t zm_write_byte(const zm_session_t *zm, uint8_t byte)
{
    return zm_write_all(zm, &byte, 1);
}

static bool zm_must_escape(uint8_t byte)
{
    return byte == ZDLE ||
        byte == 0x10 ||
        byte == 0x11 ||
        byte == 0x13 ||
        byte == 0x90 ||
        byte == 0x91 ||
        byte == 0x93 ||
        byte < 0x20 ||
        byte == 0x7f ||
        byte == 0xff;
}

static esp_err_t zm_write_escaped_byte(const zm_session_t *zm, uint8_t byte)
{
    if (!zm_must_escape(byte)) {
        return zm_write_byte(zm, byte);
    }
    if (byte == 0x7f) {
        const uint8_t escaped[2] = {ZDLE, ZRUB0};
        return zm_write_all(zm, escaped, sizeof(escaped));
    }
    if (byte == 0xff) {
        const uint8_t escaped[2] = {ZDLE, ZRUB1};
        return zm_write_all(zm, escaped, sizeof(escaped));
    }

    const uint8_t escaped[2] = {ZDLE, (uint8_t)(byte ^ 0x40U)};
    return zm_write_all(zm, escaped, sizeof(escaped));
}

static esp_err_t zm_write_escaped_le32(const zm_session_t *zm, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        const esp_err_t err = zm_write_escaped_byte(zm, (uint8_t)(value >> (i * 8)));
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static int zm_hex_digit(uint8_t byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static esp_err_t zm_read_byte(zm_session_t *zm, uint32_t timeout_ms, uint8_t *byte)
{
    if (zm == NULL || byte == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (zm->rx_buffer == NULL || zm->rx_capacity == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t start_us = esp_timer_get_time();
    while (true) {
        if (zm->rx_pos < zm->rx_len) {
            *byte = zm->rx_buffer[zm->rx_pos++];
            if (zm->rx_pos >= zm->rx_len) {
                zm->rx_pos = 0;
                zm->rx_len = 0;
            }
            return ESP_OK;
        }

        if (zm_should_cancel(zm)) {
            return ESP_ERR_INVALID_STATE;
        }

        uint32_t read_timeout_ms = ZM_READ_POLL_MS;
        if (timeout_ms != 0) {
            const int64_t elapsed_us = esp_timer_get_time() - start_us;
            if (elapsed_us >= (int64_t)timeout_ms * 1000) {
                return ESP_ERR_TIMEOUT;
            }
            const int64_t remaining_us = (int64_t)timeout_ms * 1000 - elapsed_us;
            const uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
            if (remaining_ms < read_timeout_ms) {
                read_timeout_ms = remaining_ms == 0 ? 1 : remaining_ms;
            }
        }

        size_t read_len = 0;
        const esp_err_t err = solar_os_port_read(zm->port,
                                                zm->rx_buffer,
                                                zm->rx_capacity,
                                                read_timeout_ms,
                                                &read_len);
        if (err == ESP_OK && read_len > 0) {
            zm->rx_pos = 1;
            zm->rx_len = read_len;
            *byte = zm->rx_buffer[0];
            return ESP_OK;
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t zm_read_zdl(zm_session_t *zm,
                             uint32_t timeout_ms,
                             uint8_t *byte,
                             bool *terminator)
{
    uint8_t ch = 0;
    esp_err_t err = zm_read_byte(zm, timeout_ms, &ch);
    if (err != ESP_OK) {
        return err;
    }

    if (terminator != NULL) {
        *terminator = false;
    }
    if (ch != ZDLE) {
        *byte = ch;
        return ESP_OK;
    }

    err = zm_read_byte(zm, timeout_ms, &ch);
    if (err != ESP_OK) {
        return err;
    }

    if (ch == ZCRCE || ch == ZCRCG || ch == ZCRCQ || ch == ZCRCW) {
        *byte = ch;
        if (terminator != NULL) {
            *terminator = true;
        }
        return ESP_OK;
    }
    if (ch == ZRUB0) {
        *byte = 0x7f;
        return ESP_OK;
    }
    if (ch == ZRUB1) {
        *byte = 0xff;
        return ESP_OK;
    }

    *byte = (uint8_t)(ch ^ 0x40U);
    return ESP_OK;
}

static esp_err_t zm_read_hex_byte(zm_session_t *zm, uint8_t *byte)
{
    uint8_t hi = 0;
    uint8_t lo = 0;
    esp_err_t err = zm_read_byte(zm, ZM_HEADER_TIMEOUT_MS, &hi);
    if (err != ESP_OK) {
        return err;
    }
    err = zm_read_byte(zm, ZM_HEADER_TIMEOUT_MS, &lo);
    if (err != ESP_OK) {
        return err;
    }

    const int hi_value = zm_hex_digit(hi);
    const int lo_value = zm_hex_digit(lo);
    if (hi_value < 0 || lo_value < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *byte = (uint8_t)((hi_value << 4) | lo_value);
    return ESP_OK;
}

static esp_err_t zm_read_header_hex(zm_session_t *zm, zm_header_t *header)
{
    uint8_t raw[5];
    uint8_t crc_bytes[2];

    for (size_t i = 0; i < sizeof(raw); i++) {
        const esp_err_t err = zm_read_hex_byte(zm, &raw[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (size_t i = 0; i < sizeof(crc_bytes); i++) {
        const esp_err_t err = zm_read_hex_byte(zm, &crc_bytes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    const uint16_t got_crc = ((uint16_t)crc_bytes[0] << 8) | crc_bytes[1];
    if (zm_crc16(raw, sizeof(raw)) != got_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    header->type = raw[0];
    header->pos = (uint32_t)raw[1] |
        ((uint32_t)raw[2] << 8) |
        ((uint32_t)raw[3] << 16) |
        ((uint32_t)raw[4] << 24);
    header->crc_mode = ZM_CRC16;
    return ESP_OK;
}

static esp_err_t zm_read_header_bin16(zm_session_t *zm, zm_header_t *header)
{
    uint8_t raw[5];
    uint8_t crc_bytes[2];

    for (size_t i = 0; i < sizeof(raw); i++) {
        bool term = false;
        const esp_err_t err = zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &raw[i], &term);
        if (err != ESP_OK || term) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
    }
    for (size_t i = 0; i < sizeof(crc_bytes); i++) {
        bool term = false;
        const esp_err_t err = zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &crc_bytes[i], &term);
        if (err != ESP_OK || term) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
    }

    const uint16_t got_crc = ((uint16_t)crc_bytes[0] << 8) | crc_bytes[1];
    if (zm_crc16(raw, sizeof(raw)) != got_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    header->type = raw[0];
    header->pos = (uint32_t)raw[1] |
        ((uint32_t)raw[2] << 8) |
        ((uint32_t)raw[3] << 16) |
        ((uint32_t)raw[4] << 24);
    header->crc_mode = ZM_CRC16;
    return ESP_OK;
}

static esp_err_t zm_read_header_bin32(zm_session_t *zm, zm_header_t *header)
{
    uint8_t raw[5];
    uint8_t crc_bytes[4];

    for (size_t i = 0; i < sizeof(raw); i++) {
        bool term = false;
        const esp_err_t err = zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &raw[i], &term);
        if (err != ESP_OK || term) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
    }
    for (size_t i = 0; i < sizeof(crc_bytes); i++) {
        bool term = false;
        const esp_err_t err = zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &crc_bytes[i], &term);
        if (err != ESP_OK || term) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
    }

    const uint32_t got_crc = (uint32_t)crc_bytes[0] |
        ((uint32_t)crc_bytes[1] << 8) |
        ((uint32_t)crc_bytes[2] << 16) |
        ((uint32_t)crc_bytes[3] << 24);
    if (zm_crc32(raw, sizeof(raw)) != got_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    header->type = raw[0];
    header->pos = (uint32_t)raw[1] |
        ((uint32_t)raw[2] << 8) |
        ((uint32_t)raw[3] << 16) |
        ((uint32_t)raw[4] << 24);
    header->crc_mode = ZM_CRC32;
    return ESP_OK;
}

static esp_err_t zm_read_header(zm_session_t *zm, uint32_t timeout_ms, zm_header_t *header)
{
    const int64_t start_us = esp_timer_get_time();

    while (true) {
        uint8_t ch = 0;
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (timeout_ms != 0 && elapsed_us >= (int64_t)timeout_ms * 1000) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t step_timeout_ms =
            timeout_ms == 0 ? ZM_HEADER_TIMEOUT_MS :
            (uint32_t)(((int64_t)timeout_ms * 1000 - elapsed_us) / 1000);
        esp_err_t err = zm_read_byte(zm, step_timeout_ms, &ch);
        if (err != ESP_OK) {
            return err;
        }
        if (ch != ZPAD) {
            continue;
        }

        do {
            err = zm_read_byte(zm, ZM_HEADER_TIMEOUT_MS, &ch);
            if (err != ESP_OK) {
                return err;
            }
        } while (ch == ZPAD);

        if (ch != ZDLE) {
            continue;
        }

        err = zm_read_byte(zm, ZM_HEADER_TIMEOUT_MS, &ch);
        if (err != ESP_OK) {
            return err;
        }

        if (ch == ZHEX) {
            return zm_read_header_hex(zm, header);
        }
        if (ch == ZBIN) {
            return zm_read_header_bin16(zm, header);
        }
        if (ch == ZBIN32) {
            return zm_read_header_bin32(zm, header);
        }
    }
}

static esp_err_t zm_send_hex_header_bytes(const zm_session_t *zm, const uint8_t raw[5])
{
    static const char hex[] = "0123456789abcdef";
    char out[4 + 14 + 3];
    size_t pos = 0;
    const uint16_t crc = zm_crc16(raw, 5);

    out[pos++] = ZPAD;
    out[pos++] = ZPAD;
    out[pos++] = ZDLE;
    out[pos++] = ZHEX;
    for (size_t i = 0; i < 5; i++) {
        out[pos++] = hex[raw[i] >> 4];
        out[pos++] = hex[raw[i] & 0x0fU];
    }
    out[pos++] = hex[(crc >> 12) & 0x0fU];
    out[pos++] = hex[(crc >> 8) & 0x0fU];
    out[pos++] = hex[(crc >> 4) & 0x0fU];
    out[pos++] = hex[crc & 0x0fU];
    out[pos++] = '\r';
    out[pos++] = 0x8a;
    out[pos++] = 0x11;

    return zm_write_all(zm, (const uint8_t *)out, pos);
}

static esp_err_t zm_send_hex_header(const zm_session_t *zm, uint8_t type, uint32_t pos)
{
    const uint8_t raw[5] = {
        type,
        (uint8_t)pos,
        (uint8_t)(pos >> 8),
        (uint8_t)(pos >> 16),
        (uint8_t)(pos >> 24),
    };

    return zm_send_hex_header_bytes(zm, raw);
}

static esp_err_t zm_send_zrinit(const zm_session_t *zm)
{
    const uint8_t raw[5] = {ZRINIT, 0, 0, 0, CANFDX | CANOVIO | CANFC32};
    return zm_send_hex_header_bytes(zm, raw);
}

static esp_err_t zm_send_bin32_header(const zm_session_t *zm, uint8_t type, uint32_t pos)
{
    const uint8_t prefix[] = {ZPAD, ZDLE, ZBIN32};
    const uint8_t raw[5] = {
        type,
        (uint8_t)pos,
        (uint8_t)(pos >> 8),
        (uint8_t)(pos >> 16),
        (uint8_t)(pos >> 24),
    };
    const uint32_t crc = zm_crc32(raw, sizeof(raw));

    esp_err_t err = zm_write_all(zm, prefix, sizeof(prefix));
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < sizeof(raw); i++) {
        err = zm_write_escaped_byte(zm, raw[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return zm_write_escaped_le32(zm, crc);
}

static esp_err_t zm_send_data32(const zm_session_t *zm,
                                const uint8_t *data,
                                size_t len,
                                uint8_t terminator)
{
    uint32_t crc = 0xffffffffUL;

    for (size_t i = 0; i < len; i++) {
        crc = zm_crc32_update_raw(crc, data[i]);
        const esp_err_t err = zm_write_escaped_byte(zm, data[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    crc = zm_crc32_update_raw(crc, terminator);
    crc = ~crc;

    esp_err_t err = zm_write_byte(zm, ZDLE);
    if (err != ESP_OK) {
        return err;
    }
    err = zm_write_byte(zm, terminator);
    if (err != ESP_OK) {
        return err;
    }
    err = zm_write_escaped_le32(zm, crc);
    if (err != ESP_OK) {
        return err;
    }
    if (terminator == ZCRCW) {
        return zm_write_byte(zm, 0x11);
    }
    return ESP_OK;
}

static esp_err_t zm_read_data_packet(zm_session_t *zm,
                                     zm_crc_mode_t crc_mode,
                                     uint8_t *buffer,
                                     size_t buffer_len,
                                     size_t *data_len,
                                     uint8_t *terminator)
{
    size_t len = 0;
    uint16_t crc16 = 0;
    uint32_t crc32 = 0xffffffffUL;

    if (data_len != NULL) {
        *data_len = 0;
    }
    if (terminator != NULL) {
        *terminator = 0;
    }

    while (true) {
        uint8_t ch = 0;
        bool term = false;
        const esp_err_t err = zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &ch, &term);
        if (err != ESP_OK) {
            return err;
        }

        if (term) {
            if (crc_mode == ZM_CRC32) {
                crc32 = zm_crc32_update_raw(crc32, ch);
                crc32 = ~crc32;
                uint8_t crc_bytes[4];
                for (size_t i = 0; i < sizeof(crc_bytes); i++) {
                    bool crc_term = false;
                    const esp_err_t crc_err =
                        zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &crc_bytes[i], &crc_term);
                    if (crc_err != ESP_OK || crc_term) {
                        return crc_err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : crc_err;
                    }
                }
                const uint32_t got_crc = (uint32_t)crc_bytes[0] |
                    ((uint32_t)crc_bytes[1] << 8) |
                    ((uint32_t)crc_bytes[2] << 16) |
                    ((uint32_t)crc_bytes[3] << 24);
                if (got_crc != crc32) {
                    return ESP_ERR_INVALID_CRC;
                }
            } else {
                crc16 = zm_crc16_update(crc16, ch);
                uint8_t crc_bytes[2];
                for (size_t i = 0; i < sizeof(crc_bytes); i++) {
                    bool crc_term = false;
                    const esp_err_t crc_err =
                        zm_read_zdl(zm, ZM_HEADER_TIMEOUT_MS, &crc_bytes[i], &crc_term);
                    if (crc_err != ESP_OK || crc_term) {
                        return crc_err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : crc_err;
                    }
                }
                const uint16_t got_crc = ((uint16_t)crc_bytes[0] << 8) | crc_bytes[1];
                if (got_crc != crc16) {
                    return ESP_ERR_INVALID_CRC;
                }
            }

            if (data_len != NULL) {
                *data_len = len;
            }
            if (terminator != NULL) {
                *terminator = ch;
            }
            return ESP_OK;
        }

        if (len >= buffer_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        buffer[len++] = ch;
        if (crc_mode == ZM_CRC32) {
            crc32 = zm_crc32_update_raw(crc32, ch);
        } else {
            crc16 = zm_crc16_update(crc16, ch);
        }
    }
}

static const char *zm_basename(const char *path)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static esp_err_t zm_wait_for_header_type(zm_session_t *zm,
                                         uint8_t wanted,
                                         uint32_t timeout_ms,
                                         zm_header_t *out)
{
    const int64_t start_us = esp_timer_get_time();

    while (true) {
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (timeout_ms != 0 && elapsed_us >= (int64_t)timeout_ms * 1000) {
            return ESP_ERR_TIMEOUT;
        }
        zm_header_t header;
        const uint32_t remaining_ms =
            timeout_ms == 0 ? ZM_SESSION_TIMEOUT_MS :
            (uint32_t)(((int64_t)timeout_ms * 1000 - elapsed_us) / 1000);
        const esp_err_t err = zm_read_header(zm, remaining_ms, &header);
        if (err != ESP_OK) {
            return err;
        }
        if (header.type == wanted) {
            if (out != NULL) {
                *out = header;
            }
            return ESP_OK;
        }
        if (header.type == ZABORT || header.type == ZFERR) {
            return ESP_FAIL;
        }
    }
}

static void zm_read_oo(zm_session_t *zm)
{
    uint8_t byte = 0;

    if (zm_read_byte(zm, 1000, &byte) != ESP_OK || byte != 'O') {
        return;
    }
    (void)zm_read_byte(zm, 1000, &byte);
}

esp_err_t solar_os_zmodem_send(const solar_os_transfer_options_t *options,
                               const solar_os_port_handle_t *port,
                               FILE *file,
                               uint64_t file_size,
                               solar_os_transfer_result_t *result)
{
    zm_session_t zm;
    uint8_t buffer[ZM_SEND_CHUNK_SIZE];
    uint64_t transferred = 0;
    uint64_t next_progress = ZM_PROGRESS_STEP_BYTES;
    bool cancelled = false;
    esp_err_t err;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (options == NULL || port == NULL || file == NULL || options->path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = zm_session_init(&zm, options, port);
    if (err != ESP_OK) {
        return err;
    }

    zm_header_t header;
    err = zm_wait_for_header_type(&zm, ZRINIT, ZM_SESSION_TIMEOUT_MS, &header);
    if (err != ESP_OK) {
        goto cleanup;
    }

    const char *name = zm_basename(options->path);
    char fileinfo[ZM_FILEINFO_MAX];
    const int info_len = snprintf(fileinfo,
                                  sizeof(fileinfo),
                                  "%s%c%" PRIu64 " 0 100644 0 1 %" PRIu64 "%c",
                                  name != NULL ? name : "solaros.bin",
                                  '\0',
                                  file_size,
                                  file_size,
                                  '\0');
    if (info_len < 0 || (size_t)info_len >= sizeof(fileinfo)) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = zm_send_bin32_header(&zm, ZFILE, 0);
    if (err == ESP_OK) {
        err = zm_send_data32(&zm, (const uint8_t *)fileinfo, (size_t)info_len, ZCRCW);
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = zm_wait_for_header_type(&zm, ZRPOS, ZM_SESSION_TIMEOUT_MS, &header);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (header.pos > 0) {
        if (header.pos > file_size || fseek(file, (long)header.pos, SEEK_SET) != 0) {
            err = ESP_FAIL;
            goto cleanup;
        }
        transferred = header.pos;
    }

    err = zm_send_bin32_header(&zm, ZDATA, (uint32_t)transferred);
    if (err != ESP_OK) {
        goto cleanup;
    }

    while (transferred < file_size) {
        if (zm_should_cancel(&zm)) {
            cancelled = true;
            break;
        }

        size_t want = sizeof(buffer);
        if (file_size - transferred < want) {
            want = (size_t)(file_size - transferred);
        }
        const size_t read_len = fread(buffer, 1, want, file);
        if (read_len == 0) {
            err = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }

        const uint8_t term = (transferred + read_len) >= file_size ? ZCRCE : ZCRCG;
        err = zm_send_data32(&zm, buffer, read_len, term);
        if (err != ESP_OK) {
            break;
        }
        transferred += read_len;
        zm_report_progress(&zm, transferred, &next_progress);
    }

    if (err == ESP_OK && !cancelled) {
        err = zm_send_bin32_header(&zm, ZEOF, (uint32_t)transferred);
    }
    if (err == ESP_OK && !cancelled) {
        err = zm_wait_for_header_type(&zm, ZRINIT, ZM_SESSION_TIMEOUT_MS, NULL);
    }
    if (err == ESP_OK && !cancelled) {
        err = zm_send_hex_header(&zm, ZFIN, 0);
    }
    if (err == ESP_OK && !cancelled) {
        err = zm_wait_for_header_type(&zm, ZFIN, ZM_SESSION_TIMEOUT_MS, NULL);
    }
    if (err == ESP_OK && !cancelled) {
        const uint8_t oo[] = {'O', 'O'};
        err = zm_write_all(&zm, oo, sizeof(oo));
    }

    if (result != NULL) {
        result->bytes = transferred;
        result->cancelled = cancelled;
    }
cleanup:
    zm_session_deinit(&zm);
    return err;
}

static esp_err_t zm_recv_file_data(zm_session_t *zm,
                                   FILE *file,
                                   zm_crc_mode_t crc_mode,
                                   uint64_t *transferred,
                                   uint64_t *next_progress)
{
    uint8_t buffer[ZM_RECV_CHUNK_SIZE];

    while (true) {
        size_t len = 0;
        uint8_t term = 0;
        esp_err_t err = zm_read_data_packet(zm,
                                            crc_mode,
                                            buffer,
                                            sizeof(buffer),
                                            &len,
                                            &term);
        if (err != ESP_OK) {
            return err;
        }
        if (len > 0) {
            if (fwrite(buffer, 1, len, file) != len || fflush(file) != 0) {
                return ESP_FAIL;
            }
            *transferred += len;
            zm_report_progress(zm, *transferred, next_progress);
        }

        if (term == ZCRCE) {
            return ESP_OK;
        }
        if (term == ZCRCQ || term == ZCRCW) {
            (void)zm_send_hex_header(zm, ZACK, (uint32_t)*transferred);
            if (term == ZCRCW) {
                return ESP_OK;
            }
        }
    }
}

esp_err_t solar_os_zmodem_recv(const solar_os_transfer_options_t *options,
                               const solar_os_port_handle_t *port,
                               solar_os_transfer_result_t *result)
{
    zm_session_t zm;
    uint8_t fileinfo[ZM_FILEINFO_MAX];
    uint64_t transferred = 0;
    uint64_t next_progress = ZM_PROGRESS_STEP_BYTES;
    bool cancelled = false;
    bool got_file = false;
    FILE *file = NULL;
    esp_err_t err = ESP_OK;
    int64_t next_zrinit_us = 0;
    const int64_t start_us = esp_timer_get_time();

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (options == NULL || port == NULL || options->path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = zm_session_init(&zm, options, port);
    if (err != ESP_OK) {
        return err;
    }

    while (!got_file) {
        if (zm_should_cancel(&zm)) {
            cancelled = true;
            break;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_zrinit_us) {
            err = zm_send_zrinit(&zm);
            if (err != ESP_OK) {
                break;
            }
            next_zrinit_us = now_us + 3000000LL;
        }
        if (now_us - start_us >= (int64_t)ZM_SESSION_TIMEOUT_MS * 1000) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        zm_header_t header;
        err = zm_read_header(&zm, ZM_HEADER_TIMEOUT_MS, &header);
        if (err == ESP_ERR_TIMEOUT) {
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) {
            break;
        }

        if (header.type == ZRQINIT) {
            next_zrinit_us = 0;
            continue;
        }
        if (header.type == ZSINIT) {
            size_t ignored_len = 0;
            uint8_t ignored_term = 0;
            (void)zm_read_data_packet(&zm,
                                      header.crc_mode,
                                      fileinfo,
                                      sizeof(fileinfo),
                                      &ignored_len,
                                      &ignored_term);
            (void)zm_send_hex_header(&zm, ZACK, 0);
            continue;
        }
        if (header.type == ZFILE) {
            size_t info_len = 0;
            uint8_t term = 0;
            err = zm_read_data_packet(&zm,
                                      header.crc_mode,
                                      fileinfo,
                                      sizeof(fileinfo) - 1,
                                      &info_len,
                                      &term);
            if (err != ESP_OK) {
                break;
            }
            fileinfo[info_len] = '\0';

            file = fopen(options->path, options->append ? "ab" : "wb");
            if (file == NULL) {
                err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
                break;
            }

            err = zm_send_hex_header(&zm, ZRPOS, 0);
            got_file = true;
            break;
        }
        if (header.type == ZFIN) {
            (void)zm_send_hex_header(&zm, ZFIN, 0);
            zm_read_oo(&zm);
            break;
        }
        if (header.type == ZABORT || header.type == ZFERR) {
            err = ESP_FAIL;
            break;
        }
    }

    while (err == ESP_OK && got_file && !cancelled) {
        if (zm_should_cancel(&zm)) {
            cancelled = true;
            break;
        }

        zm_header_t header;
        err = zm_read_header(&zm, ZM_SESSION_TIMEOUT_MS, &header);
        if (err != ESP_OK) {
            break;
        }

        if (header.type == ZDATA) {
            err = zm_recv_file_data(&zm, file, header.crc_mode, &transferred, &next_progress);
            continue;
        }
        if (header.type == ZEOF) {
            if (file != NULL && fclose(file) != 0) {
                file = NULL;
                err = ESP_FAIL;
                break;
            }
            file = NULL;
            err = zm_send_zrinit(&zm);
            if (err != ESP_OK) {
                break;
            }

            err = zm_wait_for_header_type(&zm, ZFIN, ZM_SESSION_TIMEOUT_MS, NULL);
            if (err == ESP_OK) {
                (void)zm_send_hex_header(&zm, ZFIN, 0);
                zm_read_oo(&zm);
            }
            break;
        }
        if (header.type == ZFIN) {
            (void)zm_send_hex_header(&zm, ZFIN, 0);
            zm_read_oo(&zm);
            break;
        }
        if (header.type == ZABORT || header.type == ZFERR) {
            err = ESP_FAIL;
            break;
        }
    }

    if (file != NULL && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (result != NULL) {
        result->bytes = transferred;
        result->cancelled = cancelled;
    }
    zm_session_deinit(&zm);
    return err;
}
