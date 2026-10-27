#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SOLAR_OS_GNSS_OUTPUT_UNKNOWN,
    SOLAR_OS_GNSS_OUTPUT_UBX,
    SOLAR_OS_GNSS_OUTPUT_CASIC,
} solar_os_gnss_output_protocol_t;

typedef struct {
    solar_os_gnss_output_protocol_t protocol;
    bool enabled;
    uint8_t port_config[8];
} solar_os_gnss_output_t;

/* The caller owns the UART and suspends its ordinary receiver while using
 * these synchronous, bounded exchanges. No persistent settings are written. */
typedef struct {
    void *ctx;
    esp_err_t (*write)(void *ctx, const uint8_t *data, size_t len);
    esp_err_t (*read)(void *ctx, uint8_t *data, size_t len,
                      uint32_t timeout_ms, size_t *read_len);
    int64_t (*now_us)(void *ctx);
} solar_os_gnss_output_io_t;

esp_err_t solar_os_gnss_output_probe(const solar_os_gnss_output_io_t *io,
                                     uint32_t baud,
                                     solar_os_gnss_output_t *output);
esp_err_t solar_os_gnss_output_set(const solar_os_gnss_output_io_t *io,
                                   solar_os_gnss_output_t *output,
                                   bool enabled);
esp_err_t solar_os_gnss_output_check_quiet(const solar_os_gnss_output_io_t *io);

/* Bounded wire codec shared with the protocol simulator in host tests. */
size_t solar_os_gnss_output_encode(solar_os_gnss_output_protocol_t protocol,
                                   uint8_t cls, uint8_t id,
                                   const uint8_t *payload, size_t len,
                                   uint8_t frame[42]);
