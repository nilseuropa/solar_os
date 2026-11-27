#pragma once
#include "solar_os_native_service_abi.h"
#include <stdarg.h>
#define SOLAR_OS_NATIVE_CLIPBOARD_SERVICE "clipboard"
#define SOLAR_OS_NATIVE_CLIPBOARD_ABI 1U
#define SOLAR_OS_NATIVE_CLIPBOARD_MAX_BYTES 262144U
#define SOLAR_OS_NATIVE_LOG_SERVICE "log"
#define SOLAR_OS_NATIVE_LOG_ABI 1U
#define SOLAR_OS_NATIVE_LOG_ERROR 0U
#define SOLAR_OS_NATIVE_LOG_WARN 1U
#define SOLAR_OS_NATIVE_LOG_INFO 2U
#define SOLAR_OS_NATIVE_LOG_DEBUG 3U
typedef struct {
    uint32_t sequence, timestamp_ms, level, truncated;
    char tag[24], message[160];
} solar_os_native_log_entry_v1_t;
typedef struct {
    uint64_t capacity, count, bytes;
    uint32_t initialized, cdc_enabled, ring_in_psram, level, dropped;
} solar_os_native_log_status_v1_t;
/* Clipboard reads copy one atomic snapshot into caller storage. Offset beyond
 * the end returns an empty read. total_size describes that same snapshot;
 * independent calls may observe a subsequent clipboard value. Data may contain
 * embedded NUL bytes; size is authoritative. No clipboard pointer escapes. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*set)(const void *bytes, size_t size);
    int (*read)(uint64_t offset, void *bytes, size_t capacity, size_t *size, uint64_t *total_size);
    uint64_t (*size)(void);
    void (*clear)(void);
} solar_os_native_clipboard_api_v1_t;
/* Snapshots are copied. snapshot() selects the newest capacity entries in
 * chronological order. snapshot_since() selects the earliest matching entries
 * after the supplied sequence, with level <= threshold. total counts all
 * eligible entries. Text bindings can use text(); C callers can also format
 * through write/vwrite using the target's ordinary C calling convention. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*clear)(void);
    int (*set_level)(uint32_t level);
    int (*set_cdc_enabled)(bool enabled);
    int (*status)(solar_os_native_log_status_v1_t *status);
    int (*snapshot)(solar_os_native_log_entry_v1_t *entries, size_t capacity, size_t *copied, size_t *total);
    int (*snapshot_since)(uint32_t after_sequence, uint32_t threshold, solar_os_native_log_entry_v1_t *entries,
        size_t capacity, size_t *copied, size_t *total);
    int (*text)(uint32_t level, const char *tag, const char *text);
    int (*write)(uint32_t level, const char *tag, const char *format, ...);
    int (*vwrite)(uint32_t level, const char *tag, const char *format, va_list args);
    int (*buffer_hex)(uint32_t level, const char *tag, const void *bytes, size_t size);
    const char *(*level_name)(uint32_t level);
    int (*parse_level)(const char *text, uint32_t *level);
} solar_os_native_log_api_v1_t;
