#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_POCSAG_BATCH_BYTES 64
#define SOLAR_OS_POCSAG_TEXT_MAX 256

typedef enum {
    SOLAR_OS_POCSAG_FORMAT_ALPHA,
    SOLAR_OS_POCSAG_FORMAT_NUMERIC,
} solar_os_pocsag_format_t;

typedef struct {
    uint32_t ric;
    uint8_t function;
    solar_os_pocsag_format_t format;
    int16_t rssi_dbm;
    uint16_t corrected_codewords;
    uint16_t uncorrectable_codewords;
    bool truncated;
    char text[SOLAR_OS_POCSAG_TEXT_MAX];
} solar_os_pocsag_message_t;

typedef void (*solar_os_pocsag_message_cb_t)(const solar_os_pocsag_message_t *message,
                                             void *user);

typedef struct {
    uint32_t target_ric;
    solar_os_pocsag_format_t format;
    bool active;
    solar_os_pocsag_message_t message;
    size_t text_len;
    uint8_t character;
    uint8_t character_bits;
} solar_os_pocsag_decoder_t;

void solar_os_pocsag_decoder_init(solar_os_pocsag_decoder_t *decoder,
                                  uint32_t target_ric,
                                  solar_os_pocsag_format_t format);
size_t solar_os_pocsag_decode_batch(solar_os_pocsag_decoder_t *decoder,
                                    const uint8_t *batch,
                                    size_t len,
                                    int16_t rssi_dbm,
                                    solar_os_pocsag_message_cb_t callback,
                                    void *user);
bool solar_os_pocsag_decoder_flush(solar_os_pocsag_decoder_t *decoder,
                                   solar_os_pocsag_message_cb_t callback,
                                   void *user);
const char *solar_os_pocsag_format_name(solar_os_pocsag_format_t format);
