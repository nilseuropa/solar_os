#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_sensors_abi.h"

#define SOLAR_OS_GNSS_NAME_MAX 20
#define SOLAR_OS_GNSS_DRIVER_MAX 24

typedef solar_os_native_gnss_fix_v1_t solar_os_gnss_fix_t;
typedef solar_os_native_gnss_status_v1_t solar_os_gnss_status_t;

typedef struct {
    esp_err_t (*read_fix)(void *ctx,
                          uint32_t timeout_ms,
                          solar_os_gnss_fix_t *fix);
    esp_err_t (*set_power)(void *ctx, bool enabled);
    /* Receiver output control, separate from switching its supply. */
    esp_err_t (*set_enabled)(void *ctx, bool enabled);
} solar_os_gnss_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const solar_os_gnss_ops_t *ops;
    void *ctx;
    bool powered;
} solar_os_gnss_registration_t;

typedef struct {
    char name[SOLAR_OS_GNSS_NAME_MAX];
    char driver[SOLAR_OS_GNSS_DRIVER_MAX];
    bool power_control;
    bool powered;
    bool enable_control;
    bool enabled;
} solar_os_gnss_info_t;

esp_err_t solar_os_gnss_register(const solar_os_gnss_registration_t *registration);
esp_err_t solar_os_gnss_unregister(const char *name);
esp_err_t solar_os_gnss_unregister_owned(const char *name, const void *context);
size_t solar_os_gnss_count(void);
bool solar_os_gnss_get(size_t index, solar_os_gnss_info_t *info);
esp_err_t solar_os_gnss_set_power(const char *name, bool enabled);
esp_err_t solar_os_gnss_set_enabled(const char *name, bool enabled);
esp_err_t solar_os_gnss_notify_enabled_state(const char *name, bool enabled);
/* Driver notification for physical power changes, including error cleanup;
 * does not touch hardware. */
esp_err_t solar_os_gnss_notify_power_state(const char *name, bool powered);
esp_err_t solar_os_gnss_notify_state_owned(const char *name, const void *context, uint32_t property, bool enabled);
esp_err_t solar_os_gnss_get_status(const char *name,
                                   uint32_t timeout_ms,
                                   solar_os_gnss_status_t *status);
esp_err_t solar_os_gnss_read_fix(const char *name,
                                 uint32_t timeout_ms,
                                 solar_os_gnss_fix_t *fix);
