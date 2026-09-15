#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_HAPTIC_NAME_MAX 20
#define SOLAR_OS_HAPTIC_DRIVER_MAX 24

typedef struct {
    esp_err_t (*play_effect)(void *ctx, uint16_t effect);
    esp_err_t (*stop)(void *ctx);
} solar_os_haptic_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    uint16_t effect_count;
    const solar_os_haptic_ops_t *ops;
    void *ctx;
} solar_os_haptic_registration_t;

typedef struct {
    char name[SOLAR_OS_HAPTIC_NAME_MAX];
    char driver[SOLAR_OS_HAPTIC_DRIVER_MAX];
    uint16_t effect_count;
} solar_os_haptic_info_t;

esp_err_t solar_os_haptic_register(
    const solar_os_haptic_registration_t *registration);
esp_err_t solar_os_haptic_unregister(const char *name);
size_t solar_os_haptic_count(void);
bool solar_os_haptic_get(size_t index, solar_os_haptic_info_t *info);
esp_err_t solar_os_haptic_play_effect(const char *name, uint16_t effect);
esp_err_t solar_os_haptic_stop(const char *name);
