#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_inbox_abi.h"

#define SOLAR_OS_INBOX_CAPACITY 64
#define SOLAR_OS_INBOX_STORE_DIR ".inbox"
#define SOLAR_OS_INBOX_STORE_FILE "messages.bin"

typedef void (*solar_os_inbox_clear_observer_t)(void *user);

esp_err_t solar_os_inbox_init(void);
/* Read a legacy saved Inbox without initializing, modifying or resetting it.
 * Used to migrate incoming message history across storage volumes. */
esp_err_t solar_os_inbox_saved_snapshot(const char *path,
    solar_os_inbox_entry_t *entries, size_t capacity, size_t *count);
esp_err_t solar_os_inbox_publish(const solar_os_inbox_publish_t *message, uint32_t *id);
esp_err_t solar_os_inbox_get(uint32_t id, solar_os_inbox_entry_t *entry, bool mark_read);
esp_err_t solar_os_inbox_mark_read(uint32_t id, bool read);
esp_err_t solar_os_inbox_delete(uint32_t id);
bool solar_os_inbox_matches_source_id(uint32_t id, uint64_t source_id);
esp_err_t solar_os_inbox_delete_many(const uint32_t *ids,
                                     size_t id_count,
                                     size_t *deleted);
esp_err_t solar_os_inbox_delete_sources(const char *const *sources,
                                        size_t source_count,
                                        size_t *deleted);
size_t solar_os_inbox_snapshot(solar_os_inbox_entry_t *entries,
                               size_t max_entries,
                               bool unread_only,
                               size_t *total_entries);
esp_err_t solar_os_inbox_get_status(solar_os_inbox_status_t *status);
esp_err_t solar_os_inbox_set_sound_enabled(bool enabled);
esp_err_t solar_os_inbox_test_sound(uint32_t *request_id);
esp_err_t solar_os_inbox_clear(void);
void solar_os_inbox_set_clear_observer(
    solar_os_inbox_clear_observer_t observer,
    void *user);
const char *solar_os_inbox_priority_name(solar_os_inbox_priority_t priority);
