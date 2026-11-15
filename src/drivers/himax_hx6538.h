#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_HIMAX_MODE_MAX 32U
#define SOLAR_OS_HIMAX_REPLY_MAX (512U * 1024U)
#define SOLAR_OS_HIMAX_SPI_READ_MAX 4095U

typedef struct {
    void *ctx;
    esp_err_t (*write)(void *ctx, const uint8_t *data, size_t length);
    esp_err_t (*read)(void *ctx, uint8_t *data, size_t capacity, size_t *length);
    uint64_t (*now_ms)(void *ctx);
    void (*wait_ms)(void *ctx, uint32_t ms);
    void *(*resize)(void *ctx, void *data, size_t size);
} solar_os_himax_io_t;

typedef struct { uint16_t width, height; uint8_t option; } solar_os_himax_mode_t;
typedef struct {
    solar_os_himax_io_t io;
    uint8_t input[SOLAR_OS_HIMAX_SPI_READ_MAX];
    size_t input_used, input_position;
    char *reply;
    size_t reply_used, reply_capacity;
    solar_os_himax_mode_t modes[SOLAR_OS_HIMAX_MODE_MAX];
    size_t mode_count;
    unsigned sensor_id;
    char version[32];
} solar_os_himax_t;

esp_err_t solar_os_himax_init(solar_os_himax_t *camera, const solar_os_himax_io_t *io);
void solar_os_himax_clear(solar_os_himax_t *camera);
esp_err_t solar_os_himax_probe(solar_os_himax_t *camera, uint32_t timeout_ms);
esp_err_t solar_os_himax_command(solar_os_himax_t *camera, const char *command,
                                  const char *response_name, uint32_t timeout_ms);
esp_err_t solar_os_himax_sample(solar_os_himax_t *camera, uint32_t timeout_ms,
                                 const uint8_t **jpeg, size_t *length,
                                 uint16_t *width, uint16_t *height);
