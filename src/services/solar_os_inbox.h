#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_INBOX_CAPACITY 64
#define SOLAR_OS_INBOX_SOURCE_MAX 16
#define SOLAR_OS_INBOX_TOPIC_MAX 24
#define SOLAR_OS_INBOX_SENDER_MAX 40
#define SOLAR_OS_INBOX_TITLE_MAX 64
#define SOLAR_OS_INBOX_BODY_MAX 256
#define SOLAR_OS_INBOX_STORE_DIR ".inbox"
#define SOLAR_OS_INBOX_STORE_FILE "messages.bin"

typedef enum {
    SOLAR_OS_INBOX_PRIORITY_LOW = 0,
    SOLAR_OS_INBOX_PRIORITY_NORMAL = 1,
    SOLAR_OS_INBOX_PRIORITY_HIGH = 2,
    SOLAR_OS_INBOX_PRIORITY_URGENT = 3,
} solar_os_inbox_priority_t;

typedef struct {
    const char *source;
    const char *topic;
    const char *sender;
    const char *title;
    const char *body;
    /* Stable producer identity used to suppress replayed notifications. */
    const char *dedupe_key;
    uint64_t timestamp_ms;
    solar_os_inbox_priority_t priority;
} solar_os_inbox_publish_t;

typedef struct {
    uint32_t id;
    uint64_t timestamp_ms;
    uint32_t received_ms;
    solar_os_inbox_priority_t priority;
    bool unread;
    bool truncated;
    char source[SOLAR_OS_INBOX_SOURCE_MAX];
    char topic[SOLAR_OS_INBOX_TOPIC_MAX];
    char sender[SOLAR_OS_INBOX_SENDER_MAX];
    char title[SOLAR_OS_INBOX_TITLE_MAX];
    char body[SOLAR_OS_INBOX_BODY_MAX];
} solar_os_inbox_entry_t;

typedef struct {
    bool initialized;
    bool ring_in_psram;
    size_t capacity;
    size_t count;
    size_t unread;
    size_t bytes;
    uint32_t dropped;
    bool persistent;
    size_t storage_limit_bytes;
    esp_err_t storage_error;
} solar_os_inbox_status_t;

esp_err_t solar_os_inbox_init(void);
esp_err_t solar_os_inbox_publish(const solar_os_inbox_publish_t *message, uint32_t *id);
esp_err_t solar_os_inbox_get(uint32_t id, solar_os_inbox_entry_t *entry, bool mark_read);
esp_err_t solar_os_inbox_mark_read(uint32_t id, bool read);
size_t solar_os_inbox_snapshot(solar_os_inbox_entry_t *entries,
                               size_t max_entries,
                               bool unread_only,
                               size_t *total_entries);
esp_err_t solar_os_inbox_get_status(solar_os_inbox_status_t *status);
esp_err_t solar_os_inbox_clear(void);
const char *solar_os_inbox_priority_name(solar_os_inbox_priority_t priority);
