#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ST25R3916_UID_MAX 10U

typedef struct {
    esp_err_t (*transfer)(void *ctx,
                          const uint8_t *tx,
                          uint8_t *rx,
                          size_t len);
    void (*delay_ms)(void *ctx, uint32_t delay_ms);
    int64_t (*time_ms)(void *ctx);
    bool (*irq_asserted)(void *ctx);
    void *ctx;
} st25r3916_io_t;

typedef struct {
    st25r3916_io_t io;
    uint8_t identity;
    bool initialized;
} st25r3916_t;

typedef struct {
    uint8_t uid[ST25R3916_UID_MAX];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
} st25r3916_nfca_tag_t;

esp_err_t st25r3916_init(st25r3916_t *device, const st25r3916_io_t *io);
esp_err_t st25r3916_deinit(st25r3916_t *device);
esp_err_t st25r3916_nfca_scan(st25r3916_t *device,
                              uint32_t timeout_ms,
                              st25r3916_nfca_tag_t *tag);
