#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_GNSS_NAME_MAX 20
#define SOLAR_OS_GNSS_DRIVER_MAX 24

typedef struct {
    bool valid;
    bool time_valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t fix_type;
    uint8_t satellites;
    int32_t latitude_deg_e7;
    int32_t longitude_deg_e7;
    int32_t height_msl_mm;
    uint32_t horizontal_accuracy_mm;
    uint32_t vertical_accuracy_mm;
    int32_t ground_speed_mm_s;
    int32_t heading_deg_e5;
    uint16_t position_dop_e2;
} solar_os_gnss_fix_t;

typedef struct {
    esp_err_t (*read_fix)(void *ctx,
                          uint32_t timeout_ms,
                          solar_os_gnss_fix_t *fix);
} solar_os_gnss_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const solar_os_gnss_ops_t *ops;
    void *ctx;
} solar_os_gnss_registration_t;

typedef struct {
    char name[SOLAR_OS_GNSS_NAME_MAX];
    char driver[SOLAR_OS_GNSS_DRIVER_MAX];
} solar_os_gnss_info_t;

esp_err_t solar_os_gnss_register(const solar_os_gnss_registration_t *registration);
esp_err_t solar_os_gnss_unregister(const char *name);
size_t solar_os_gnss_count(void);
bool solar_os_gnss_get(size_t index, solar_os_gnss_info_t *info);
esp_err_t solar_os_gnss_read_fix(const char *name,
                                 uint32_t timeout_ms,
                                 solar_os_gnss_fix_t *fix);
