#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_NFC_NAME_MAX 20
#define SOLAR_OS_NFC_DRIVER_MAX 24
#define SOLAR_OS_NFC_UID_MAX 10U

typedef enum {
    SOLAR_OS_NFC_TECHNOLOGY_NFCA,
} solar_os_nfc_technology_t;

typedef struct {
    solar_os_nfc_technology_t technology;
    uint8_t uid[SOLAR_OS_NFC_UID_MAX];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
} solar_os_nfc_tag_t;

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

typedef struct {
    char name[SOLAR_OS_NFC_NAME_MAX];
    char driver[SOLAR_OS_NFC_DRIVER_MAX];
    bool power_control;
    bool powered;
} solar_os_nfc_info_t;

esp_err_t solar_os_nfc_register(const solar_os_nfc_registration_t *registration);
esp_err_t solar_os_nfc_unregister(const char *name);
size_t solar_os_nfc_count(void);
bool solar_os_nfc_get(size_t index, solar_os_nfc_info_t *info);
esp_err_t solar_os_nfc_set_power(const char *name, bool enabled);
esp_err_t solar_os_nfc_scan(const char *name,
                            uint32_t timeout_ms,
                            solar_os_nfc_tag_t *tag);
