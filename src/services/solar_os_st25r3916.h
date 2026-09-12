#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

/* ST25R3916 default SPI CS GPIO on the LilyGO T-LoRa-Pager. */
#define SOLAR_OS_ST25R3916_CS_GPIO  39
#define SOLAR_OS_ST25R3916_IRQ_GPIO 5

/* Maximum UID length for ISO 14443A (single/double/triple UID = 4/7/10 bytes). */
#define SOLAR_OS_ST25R3916_UID_MAX 10U

typedef struct {
    uint8_t uid[SOLAR_OS_ST25R3916_UID_MAX];
    uint8_t uid_len;
    uint8_t sak;     /* Select Acknowledge — 0x20 = ISO 14443-4, 0x00 = Mifare Ultralight, etc. */
    uint8_t atqa[2]; /* Answer To Request type A */
} solar_os_st25r3916_tag_t;

esp_err_t solar_os_st25r3916_attach(const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count);
esp_err_t solar_os_st25r3916_detach(const char *name);

/* Scan for an ISO 14443A tag; fills *tag on success. Returns ESP_ERR_NOT_FOUND
 * when no tag is present within timeout_ms. */
esp_err_t solar_os_st25r3916_scan(uint32_t timeout_ms, solar_os_st25r3916_tag_t *tag);

/* True after a successful chip identity check (deferred until first scan if
 * the chip was powered off at attach time). */
bool solar_os_st25r3916_is_ready(void);

/* Mark chip as needing re-initialisation (call after cutting NFC power). */
void solar_os_st25r3916_reset_chip(void);
