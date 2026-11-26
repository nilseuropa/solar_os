#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_native_motion_abi.h"

typedef solar_os_native_encoder_mode_v1_t solar_os_encoder_mode_t;
typedef solar_os_native_encoder_info_v1_t solar_os_encoder_info_t;
typedef solar_os_native_encoder_status_v1_t solar_os_encoder_status_t;

typedef struct {
    /* Acquire a fresh count from the device; do not return a cached sample.
     * sample_owned timestamps completion of this synchronous acquisition. */
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

typedef solar_os_native_encoder_sample_v1_t solar_os_encoder_sample_t;

/* One named endpoint per encoder. Callbacks run outside the registry lock and
 * are serialized per endpoint. Do not re-enter operations on that endpoint.
 * The provider owns its context until unregister succeeds. */
esp_err_t solar_os_encoder_register(const solar_os_encoder_registration_t *registration);
esp_err_t solar_os_encoder_unregister(const char *name);
size_t solar_os_encoder_count(void);
bool solar_os_encoder_get(size_t index, solar_os_encoder_info_t *info);
/* Fresh calibrated feedback. Idle encoders use a 20 ms measurement window;
 * an active stream's estimator is previewed without changing its history.
 * Raw read remains an immediate count acquisition. */
esp_err_t solar_os_encoder_status(const char *name, solar_os_encoder_status_t *status);
esp_err_t solar_os_encoder_read(const char *name, int32_t *count);
esp_err_t solar_os_encoder_reset(const char *name);
typedef solar_os_native_encoder_config_v1_t solar_os_encoder_config_t;
bool solar_os_encoder_config_valid(const solar_os_encoder_config_t *config);
esp_err_t solar_os_encoder_config_get(const char *name, solar_os_encoder_config_t *config);
esp_err_t solar_os_encoder_configure(const char *name, const char *key, float value);
/* The stream owns a device/configuration claim until close. Owned calibration
 * is used for transactional model startup; ordinary setters reject claims. */
esp_err_t solar_os_encoder_stream_configure(uint32_t claim, const solar_os_encoder_config_t *config);
esp_err_t solar_os_encoder_feedback(uint32_t claim, solar_os_native_encoder_feedback_v1_t *feedback);
/* A reference claim permits shared reads but excludes reset and detach. */
esp_err_t solar_os_encoder_claim(const char *name, uint32_t *claim);
esp_err_t solar_os_encoder_sample_owned(uint32_t claim, solar_os_encoder_sample_t *sample);
esp_err_t solar_os_encoder_release(uint32_t claim);
