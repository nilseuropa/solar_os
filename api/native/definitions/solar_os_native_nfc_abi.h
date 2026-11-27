#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_NFC_SERVICE "nfc"
#define SOLAR_OS_NATIVE_NFC_ABI 1U
#define SOLAR_OS_NFC_NAME_MAX 20U
#define SOLAR_OS_NFC_DRIVER_MAX 24U
#define SOLAR_OS_NFC_UID_MAX 10U

typedef enum {
    SOLAR_OS_NFC_TECHNOLOGY_NFCA = 0,
} solar_os_nfc_technology_t;

typedef struct {
    solar_os_nfc_technology_t technology;
    uint8_t uid[SOLAR_OS_NFC_UID_MAX];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
} solar_os_nfc_tag_t;

typedef struct {
    char name[SOLAR_OS_NFC_NAME_MAX];
    char driver[SOLAR_OS_NFC_DRIVER_MAX];
    bool power_control;
    bool powered;
} solar_os_nfc_info_t;

/* The descriptor and names are copied. Callbacks and user state remain alive
 * until provider_close succeeds. Pass zero for library_lease from a bound
 * driver callback, or pass a live library lease from a library provider. */
typedef struct {
    const char *name, *driver;
    uint32_t library_lease;
    void *user;
    bool powered;
    int (*scan)(void *user, uint32_t timeout_ms, solar_os_nfc_tag_t *tag);
    int (*set_power)(void *user, bool enabled);
} solar_os_native_nfc_provider_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    bool (*info)(size_t index, solar_os_nfc_info_t *out);
    int (*set_power)(const char *name, bool enabled);
    int (*scan)(const char *name, uint32_t timeout_ms, solar_os_nfc_tag_t *tag);
    int (*provider_register)(const solar_os_native_nfc_provider_v1_t *provider, solar_os_native_handle_t *handle);
    int (*provider_close)(solar_os_native_handle_t handle);
} solar_os_native_nfc_api_v1_t;
