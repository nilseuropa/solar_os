#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_WEBRADIO_STATION_MAX 16U
#define SOLAR_OS_WEBRADIO_STATION_NAME_MAX 32U
#define SOLAR_OS_WEBRADIO_URL_MAX 160U

typedef struct {
    char name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    char url[SOLAR_OS_WEBRADIO_URL_MAX];
} solar_os_webradio_station_t;

esp_err_t solar_os_webradio_catalog_init(void);
size_t solar_os_webradio_catalog_snapshot(
    solar_os_webradio_station_t *stations,
    size_t capacity,
    uint32_t *generation);
esp_err_t solar_os_webradio_catalog_add(const char *name, const char *url);
esp_err_t solar_os_webradio_catalog_update(const char *old_name,
                                           const char *name,
                                           const char *url);
esp_err_t solar_os_webradio_catalog_remove(const char *name);
esp_err_t solar_os_webradio_catalog_reset(void);
esp_err_t solar_os_webradio_catalog_import(const char *path);
int32_t solar_os_webradio_catalog_duration(size_t index);
bool solar_os_webradio_url_valid(const char *url);
