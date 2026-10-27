#pragma once

#include <assert.h>
#include <string.h>
#include "gnss_output.h"

typedef struct {
    solar_os_gnss_output_protocol_t protocol;
    int64_t *clock;
    uint32_t baud;
    bool text_enabled;
    bool periodic_binary;
    bool ignore_set;
    bool nack;
    bool corrupt;
    bool wrong_key;
    bool write_fail;
    unsigned sets;
    uint8_t rx[128];
    size_t rx_len;
} gnss_output_fixture_t;

static inline void fixture_queue(gnss_output_fixture_t *f, uint8_t cls, uint8_t id,
                           const uint8_t *payload, size_t len)
{
    uint8_t frame[42];
    size_t size = solar_os_gnss_output_encode(f->protocol, cls, id, payload, len, frame);
    assert(size != 0 && f->rx_len + size <= sizeof(f->rx));
    if (f->corrupt) {
        frame[size - 1] ^= 1;
    }
    memcpy(f->rx + f->rx_len, frame, size);
    f->rx_len += size;
}

static inline esp_err_t fixture_write(void *ctx, const uint8_t *data, size_t len)
{
    gnss_output_fixture_t *f = ctx;
    if (f->write_fail) {
        return ESP_FAIL;
    }
    const bool casic = f->protocol == SOLAR_OS_GNSS_OUTPUT_CASIC;
    if (f->protocol == SOLAR_OS_GNSS_OUTPUT_UNKNOWN ||
        data[0] != (casic ? 0xba : 0xb5)) {
        return ESP_OK;
    }
    assert(len >= (casic ? 10U : 8U));
    const uint8_t cls = data[casic ? 4 : 2], id = data[casic ? 5 : 3];
    const size_t size = len - (casic ? 10 : 8);
    assert(cls == 6);
    if (f->nack) {
        const uint8_t payload[4] = {cls, id, 0, 0};
        fixture_queue(f, 5, 0, payload, casic ? 4 : 2);
        return ESP_OK;
    }
    if ((!casic && id == 0x8a) || (casic && id == 0 && size == 8)) {
        f->sets++;
        if (casic) {
            assert(data[6] == 0 && data[8] == 0xc0 && data[9] == 8);
            assert((data[7] & ~0x20) == (0xff & ~0x20));
            assert(data[10] == (uint8_t)f->baud && data[11] == (uint8_t)(f->baud >> 8));
        } else {
            assert(size == 9 && data[6] == 0 && data[7] == 1); /* RAM only. */
            assert(memcmp(data + 10, "\x02\x00\x74\x10", 4) == 0);
        }
        if (!f->ignore_set) {
            f->text_enabled = casic ? (data[7] & 0x20) != 0 : data[14] != 0;
        }
        const uint8_t ack[4] = {cls, id, 0, 0};
        fixture_queue(f, 5, 1, ack, casic ? 4 : 2);
        return ESP_OK;
    }
    if (casic) {
        assert(id == 0 && size == 0);
        uint8_t payload[8] = {0, f->text_enabled ? 0xff : 0xdf, 0xc0, 8};
        for (size_t i = 0; i < 4; i++) {
            payload[4 + i] = (uint8_t)(f->baud >> (8 * i));
        }
        fixture_queue(f, 6, 0, payload, 8);
    } else {
        assert(id == 0x8b && size == 8);
        assert(memcmp(data + 6, "\0\0\0\0\x02\0\x74\x10", 8) == 0);
        uint8_t payload[9] = {1, 0, 0, 0, f->wrong_key ? 3 : 2, 0, 0x74, 0x10,
                              f->text_enabled};
        fixture_queue(f, 6, 0x8b, payload, 9);
    }
    return ESP_OK;
}

static inline esp_err_t fixture_read(void *ctx, uint8_t *data, size_t len,
                               uint32_t timeout_ms, size_t *read_len)
{
    gnss_output_fixture_t *f = ctx;
    *f->clock += (int64_t)timeout_ms * 1000;
    *read_len = 0;
    if (f->rx_len != 0) {
        *read_len = f->rx_len < len ? f->rx_len : len;
        memcpy(data, f->rx, *read_len);
        f->rx_len -= *read_len;
        memmove(f->rx, f->rx + *read_len, f->rx_len);
    } else if (f->text_enabled || f->periodic_binary) {
        const char *line = "$GNRMC,120000.00,V,,,,,,,010126,,,N*64\r\n";
        assert(strlen(line) <= len);
        *read_len = strlen(line);
        memcpy(data, line, *read_len);
    }
    return ESP_OK;
}

static inline int64_t fixture_now(void *ctx)
{
    return * ((gnss_output_fixture_t *)ctx)->clock;
}

static inline solar_os_gnss_output_io_t fixture_io(gnss_output_fixture_t *f)
{
    return (solar_os_gnss_output_io_t) {
        .ctx = f, .write = fixture_write, .read = fixture_read, .now_us = fixture_now,
    };
}
