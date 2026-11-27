#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_nfc_abi.h"

typedef struct {
    esp_err_t (*scan)(void *ctx,
                      uint32_t timeout_ms,
                      solar_os_nfc_tag_t *tag);
    esp_err_t (*set_power)(void *ctx, bool enabled);
} solar_os_nfc_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const solar_os_nfc_ops_t *ops;
    void *ctx;
    bool powered;
} solar_os_nfc_registration_t;

esp_err_t solar_os_nfc_register(const solar_os_nfc_registration_t *registration);
esp_err_t solar_os_nfc_unregister(const char *name);
esp_err_t solar_os_nfc_unregister_owned(const char *name, const void *owner);
size_t solar_os_nfc_count(void);
bool solar_os_nfc_get(size_t index, solar_os_nfc_info_t *info);
esp_err_t solar_os_nfc_set_power(const char *name, bool enabled);
esp_err_t solar_os_nfc_scan(const char *name,
                            uint32_t timeout_ms,
                            solar_os_nfc_tag_t *tag);
