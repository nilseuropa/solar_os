#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_messaging_types_v1.h"

#define SOLAR_OS_NATIVE_CREDENTIALS_SERVICE "credentials"
#define SOLAR_OS_NATIVE_CREDENTIALS_ABI 1U

#define SOLAR_OS_CREDENTIAL_CAPACITY 12U
#define SOLAR_OS_CREDENTIAL_SECRET_MAX 128U
#define SOLAR_OS_CREDENTIAL_LABEL_MAX 32U

typedef enum {
    SOLAR_OS_CREDENTIAL_ASYMMETRIC_IDENTITY = 0,
    SOLAR_OS_CREDENTIAL_SHARED_KEY,
    SOLAR_OS_CREDENTIAL_TOKEN,
} solar_os_credential_kind_t;

typedef struct {
    solar_os_credential_id_t id;
    solar_os_messaging_provider_id_t provider;
    solar_os_credential_kind_t kind;
    char label[SOLAR_OS_CREDENTIAL_LABEL_MAX + 1U];
} solar_os_credential_info_t;

typedef struct {
    bool initialized;
    bool records_in_psram;
    bool persistent;
    size_t capacity;
    size_t count;
    uint32_t generation;
    int32_t storage_error;
} solar_os_credentials_status_t;

/* Enumeration copies metadata. Secrets are returned only by read_secret.
 * Store or read at most SOLAR_OS_CREDENTIAL_SECRET_MAX bytes. The caller owns
 * its buffers and can erase them with wipe after use. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*status)(solar_os_credentials_status_t *status);
    size_t (*snapshot)(solar_os_credential_info_t *records, size_t capacity);
    int (*find)(solar_os_messaging_provider_id_t provider, solar_os_credential_kind_t kind,
                const char *label, solar_os_credential_info_t *record);
    int (*put)(solar_os_messaging_provider_id_t provider, solar_os_credential_kind_t kind,
        const char *label, const void *secret, size_t size, bool replace, solar_os_credential_id_t *id);
    int (*read_secret)(solar_os_credential_id_t id, void *secret, size_t capacity, size_t *size);
    int (*remove)(solar_os_credential_id_t id);
    int (*remove_provider)(solar_os_messaging_provider_id_t provider);
    void (*wipe)(void *buffer, size_t size);
    const char *(*kind_name)(solar_os_credential_kind_t kind);
} solar_os_native_credentials_api_v1_t;
