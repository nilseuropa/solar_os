#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_native_motion_abi.h"

typedef solar_os_native_motor_mode_v1_t solar_os_motor_mode_t;
typedef solar_os_native_motor_info_v1_t solar_os_motor_info_t;
typedef solar_os_native_motor_status_v1_t solar_os_motor_status_t;

typedef struct {
    esp_err_t (*status)(void *ctx, solar_os_motor_status_t *status);
    esp_err_t (*drive)(void *ctx, float value); /* Optional, normalized -1..1. */
    esp_err_t (*coast)(void *ctx);
    esp_err_t (*brake)(void *ctx); /* Optional. */
    esp_err_t (*clear)(void *ctx); /* Optional; leave the motor coasting. */
    /* Optional teardown, called during unregister instead of coast. On
     * failure the provider and its context remain registered for retry. */
    esp_err_t (*shutdown)(void *ctx);
} solar_os_motor_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const solar_os_motor_ops_t *ops;
    void *ctx;
} solar_os_motor_registration_t;

/* Register one named motor endpoint per channel. Callbacks are serialized per
 * endpoint and run outside the registry lock. Do not re-enter operations on
 * the same endpoint. The service does not own the provider context. */
esp_err_t solar_os_motor_register(const solar_os_motor_registration_t *registration);
/* Rejects unregister while operations are in flight; successful unregister
 * stops the motor before releasing the context back to its driver. */
esp_err_t solar_os_motor_unregister(const char *name);
size_t solar_os_motor_count(void);
bool solar_os_motor_get(size_t index, solar_os_motor_info_t *info);
esp_err_t solar_os_motor_status(const char *name, solar_os_motor_status_t *status);
/* Finite -1..1 command: sign selects direction; zero always means coast.
 * Providers map demand to hardware and enforce fault/recovery policy. */
esp_err_t solar_os_motor_drive(const char *name, float value);
esp_err_t solar_os_motor_coast(const char *name);
esp_err_t solar_os_motor_brake(const char *name);
esp_err_t solar_os_motor_clear(const char *name);
typedef solar_os_native_motor_config_v1_t solar_os_motor_config_t;
bool solar_os_motor_config_valid(const solar_os_motor_config_t *config);
esp_err_t solar_os_motor_config_get(const char *name, solar_os_motor_config_t *config);
esp_err_t solar_os_motor_configure(const char *name, const char *key, float value);
esp_err_t solar_os_motor_stream_configure(uint32_t claim, const solar_os_motor_config_t *config);
/* Exclusive output ownership. Claims pin the endpoint against detach and
 * exclude ordinary drive/coast/brake/clear calls. Tokens are not reusable.
 * Release applies coast or brake before dropping the claim; failure retains it. */
esp_err_t solar_os_motor_claim(const char *name, uint32_t *claim);
esp_err_t solar_os_motor_drive_owned(uint32_t claim, float value);
esp_err_t solar_os_motor_stop_owned(uint32_t claim, bool brake);
esp_err_t solar_os_motor_release(uint32_t claim, bool brake);
const char *solar_os_motor_mode_name(solar_os_motor_mode_t mode);
