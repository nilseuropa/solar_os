#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_diagnostics.h"
#include "solar_os_clipboard.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
static atomic_uint allocations;
static bool fail_allocation;
static const solar_os_native_clipboard_api_v1_t *clipboard;
static solar_os_log_level_t selected_level = SOLAR_OS_LOG_LEVEL_INFO;
static char last_message[160];
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); if (fail_allocation) return NULL; void *p = malloc(size); if (p) ++allocations; return p; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ void *p = solar_os_memory_alloc(n * size, cls, tag); if (p) memset(p, 0, n * size); return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
esp_err_t solar_os_log_clear(void) { return ESP_OK; }
esp_err_t solar_os_log_set_level(solar_os_log_level_t level) { selected_level = level; return ESP_OK; }
esp_err_t solar_os_log_set_cdc_enabled(bool enabled) { assert(enabled); return ESP_OK; }
esp_err_t solar_os_log_get_status(solar_os_log_status_t *out)
{ *out = (solar_os_log_status_t){.initialized = true, .cdc_enabled = true, .ring_in_psram = true, .level = selected_level, .capacity = 256, .count = 2, .bytes = 999, .dropped = 123}; return ESP_OK; }
static const solar_os_log_entry_t entries[] = {
    {.sequence = 2, .timestamp_ms = UINT32_MAX, .level = SOLAR_OS_LOG_LEVEL_ERROR, .truncated = true, .tag = "test", .message = "error"},
    {.sequence = 3, .timestamp_ms = 5, .level = SOLAR_OS_LOG_LEVEL_DEBUG, .message = "debug"}};
size_t solar_os_log_snapshot(solar_os_log_entry_t *out, size_t capacity, size_t *total)
{ *total = 2; if (!capacity) return 0; if (capacity == 1) { *out = entries[1]; return 1; } memcpy(out, entries, sizeof(entries)); return 2; }
size_t solar_os_log_snapshot_since(uint32_t after, solar_os_log_level_t level, solar_os_log_entry_t *out, size_t capacity, size_t *total)
{
    *total = 0; size_t copied = 0;
    for (unsigned i = 0; i < 2; ++i) if (entries[i].sequence > after && entries[i].level <= level) {
        ++*total; if (copied < capacity) out[copied++] = entries[i];
    }
    return copied;
}
esp_err_t solar_os_log_vwrite(solar_os_log_level_t level, const char *tag, const char *format, va_list args)
{ assert(level == SOLAR_OS_LOG_LEVEL_INFO && !strcmp(tag, "test")); vsnprintf(last_message, sizeof(last_message), format, args); return ESP_OK; }
esp_err_t solar_os_log_buffer_hex(solar_os_log_level_t level, const char *tag, const void *bytes, size_t size)
{ assert(level == SOLAR_OS_LOG_LEVEL_INFO && !strcmp(tag, "test") && size == 3 && !memcmp(bytes, "abc", 3)); return ESP_OK; }
const char *solar_os_log_level_name(solar_os_log_level_t level) { return level == SOLAR_OS_LOG_LEVEL_WARN ? "warn" : "other"; }
bool solar_os_log_parse_level(const char *text, solar_os_log_level_t *out) { if (strcmp(text, "warn")) return false; *out = SOLAR_OS_LOG_LEVEL_WARN; return true; }
static void *writer(void *arg)
{
    char data[4096]; bool large = (uintptr_t)arg != 0; memset(data, large ? 'A' : 'B', sizeof(data));
    for (unsigned i = 0; i < 2000; ++i) assert(!clipboard->set(data, large ? 4096 : 2048));
    return NULL;
}
static void *reader(void *unused)
{
    (void)unused; char data[128]; size_t size; uint64_t total;
    for (unsigned i = 0; i < 5000; ++i) {
        assert(!clipboard->read(2040, data, sizeof(data), &size, &total));
        assert((total == 4096 && size == 128) || (total == 2048 && size == 8));
        for (size_t j = 0; j < size; ++j) assert(data[j] == (total == 4096 ? 'A' : 'B'));
    }
    return NULL;
}
static int vwrite_call(const solar_os_native_log_api_v1_t *api, const char *format, ...)
{ va_list args; va_start(args, format); int result = api->vwrite(SOLAR_OS_NATIVE_LOG_INFO, "test", format, args); va_end(args); return result; }
int main(void)
{
    clipboard = solar_os_native_diagnostics_get_service("clipboard", 1, sizeof(*clipboard)); assert(clipboard);
    const solar_os_native_log_api_v1_t *log = solar_os_native_diagnostics_get_service("log", 1, sizeof(*log)); assert(log);
    char initial[4096]; memset(initial, 'A', sizeof(initial)); assert(!clipboard->set(initial, sizeof(initial)));
    pthread_t threads[3]; assert(!pthread_create(&threads[0], NULL, writer, (void *)1)); assert(!pthread_create(&threads[1], NULL, writer, NULL));
    assert(!pthread_create(&threads[2], NULL, reader, NULL)); for (unsigned i = 0; i < 3; ++i) pthread_join(threads[i], NULL);
    assert(!clipboard->set("a\0b", 3)); char bytes[5]; size_t size; uint64_t total;
    assert(!clipboard->read(0, bytes, sizeof(bytes), &size, &total) && size == 3 && total == 3 && !memcmp(bytes, "a\0b", 3));
    assert(!clipboard->read(UINT64_MAX, bytes, sizeof(bytes), &size, &total) && !size && total == 3);
    fail_allocation = true; assert(clipboard->set("new", 3) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY); fail_allocation = false;
    assert(clipboard->size() == 3); clipboard->clear(); assert(!clipboard->size() && !allocations);
    solar_os_native_log_entry_v1_t entry; size_t copied, count;
    assert(!log->snapshot(&entry, 1, &copied, &count) && copied == 1 && count == 2 && entry.sequence == 3 && entry.level == SOLAR_OS_NATIVE_LOG_DEBUG);
    assert(!log->snapshot_since(1, SOLAR_OS_NATIVE_LOG_WARN, &entry, 1, &copied, &count) && copied == 1 && count == 1 && entry.sequence == 2 && entry.truncated && entry.timestamp_ms == UINT32_MAX);
    assert(!log->set_level(SOLAR_OS_NATIVE_LOG_WARN)); solar_os_native_log_status_v1_t status; assert(!log->status(&status));
    assert(status.level == SOLAR_OS_NATIVE_LOG_WARN && status.ring_in_psram && status.bytes == 999 && status.dropped == 123);
    assert(log->set_level(99) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!log->text(SOLAR_OS_NATIVE_LOG_INFO, "test", "100% %n") && !strcmp(last_message, "100% %n"));
    assert(!log->write(SOLAR_OS_NATIVE_LOG_INFO, "test", "%s %d", "value", 42) && !strcmp(last_message, "value 42"));
    assert(!vwrite_call(log, "%u", 17U) && !strcmp(last_message, "17"));
    assert(!log->buffer_hex(SOLAR_OS_NATIVE_LOG_INFO, "test", "abc", 3));
    uint32_t level; assert(!log->parse_level("warn", &level) && level == SOLAR_OS_NATIVE_LOG_WARN && !strcmp(log->level_name(level), "warn"));
    fail_allocation = true; assert(log->snapshot(&entry, 1, &copied, &count) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !copied && !count); fail_allocation = false;
    assert(!allocations); puts("native clipboard atomic copies and log snapshots/formatting passed"); return 0;
}
