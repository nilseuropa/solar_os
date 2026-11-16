#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_FILES_SERVICE "storage.files"
#define SOLAR_OS_NATIVE_JSON_SERVICE "json"
#define SOLAR_OS_NATIVE_JSON_OBJECT 1U
#define SOLAR_OS_NATIVE_JSON_ARRAY 2U
typedef struct solar_os_native_file solar_os_native_file_t;
/* Files are read-only leases. Close every lease before module unload. Reads
 * advance the position and are bounded to 64 KiB. sha256 reads the whole file
 * with bounded workspace, then rewinds it even on failure. Paths are absolute.
 * replace atomically replaces a small file in an existing directory; callers
 * own the contents and policy, and must serialize writes to the same path. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *path, solar_os_native_file_t **file, uint32_t *size);
    int (*read)(solar_os_native_file_t *file, void *data, size_t length, size_t *read);
    int (*seek)(solar_os_native_file_t *file, uint32_t position);
    void (*close)(solar_os_native_file_t *file);
    int (*sha256)(solar_os_native_file_t *file, char hex[65], solar_os_native_cancel_fn cancel, void *user);
    int (*replace)(const char *path, const void *data, size_t length);
} solar_os_native_files_api_v1_t;

typedef struct solar_os_native_json_value solar_os_native_json_value_t;
/* Strict bounded JSON objects/arrays. parse rejects duplicate keys, embedded
 * NUL, trailing data, and nesting beyond 32. Children and strings are borrowed
 * until free(root); only the root may be freed. uint_value rejects booleans,
 * fractions, negative values and values above 2^53-1. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*parse)(const char *text, size_t length, solar_os_native_json_value_t **root);
    const solar_os_native_json_value_t *(*get)(const solar_os_native_json_value_t *, const char *key);
    const solar_os_native_json_value_t *(*at)(const solar_os_native_json_value_t *, size_t index);
    const char *(*string)(const solar_os_native_json_value_t *);
    bool (*uint_value)(const solar_os_native_json_value_t *, uint64_t *value);
    size_t (*count)(const solar_os_native_json_value_t *);
    const char *(*key)(const solar_os_native_json_value_t *);
    void (*free)(solar_os_native_json_value_t *root);
    uint32_t (*kind)(const solar_os_native_json_value_t *);
} solar_os_native_json_api_v1_t;
