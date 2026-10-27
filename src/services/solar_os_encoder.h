#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_ENCODER_NAME_MAX 20
#define SOLAR_OS_ENCODER_DRIVER_MAX 32

typedef enum {
    SOLAR_OS_ENCODER_MODE_X1 = 1,
    SOLAR_OS_ENCODER_MODE_X2 = 2,
    SOLAR_OS_ENCODER_MODE_X4 = 4,
} solar_os_encoder_mode_t;

typedef struct {
    char name[SOLAR_OS_ENCODER_NAME_MAX];
    char driver[SOLAR_OS_ENCODER_DRIVER_MAX];
    solar_os_encoder_mode_t mode;
    bool can_reset;
} solar_os_encoder_info_t;

typedef struct {
    solar_os_encoder_info_t info;
    int32_t count;
} solar_os_encoder_status_t;

typedef struct {
    esp_err_t (*read)(void *ctx, int32_t *count);
    esp_err_t (*reset)(void *ctx); /* Optional; set the count to zero. */
    esp_err_t (*shutdown)(void *ctx); /* Optional; retain context on failure. */
} solar_os_encoder_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    solar_os_encoder_mode_t mode;
    const solar_os_encoder_ops_t *ops;
    void *ctx;
} solar_os_encoder_registration_t;

/* One named endpoint per encoder. Callbacks run outside the registry lock and
 * are serialized per endpoint. Do not re-enter operations on that endpoint.
 * The provider owns its context until unregister succeeds. */
esp_err_t solar_os_encoder_register(const solar_os_encoder_registration_t *registration);
esp_err_t solar_os_encoder_unregister(const char *name);
size_t solar_os_encoder_count(void);
bool solar_os_encoder_get(size_t index, solar_os_encoder_info_t *info);
esp_err_t solar_os_encoder_status(const char *name, solar_os_encoder_status_t *status);
esp_err_t solar_os_encoder_read(const char *name, int32_t *count);
esp_err_t solar_os_encoder_reset(const char *name);
