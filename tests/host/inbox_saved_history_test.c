/* Read the actual legacy Inbox format without changing its file or live state. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "solar_os_native_inbox.h"
static int xPortInIsrContext(void) { return 0; }
#include "../../src/services/solar_os_inbox.c"

static const solar_os_native_inbox_api_v1_t *api;
static bool check_init_reentry;
static bool fail_allocation;
static unsigned ring_allocations;

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls;
    assert(!strcmp(tag, "inbox.ring"));
    if (check_init_reentry) {
        solar_os_inbox_status_t status;
        assert(api->status(&status) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    }
    if (fail_allocation) return NULL;
    ring_allocations++;
    return calloc(count, size);
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return malloc(size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
bool solar_os_memory_is_external(const void *ptr) { return ptr != NULL; }
int64_t esp_timer_get_time(void) { return 321000; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch) { *epoch = 1700000000000ULL; return ESP_OK; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "fixture"; }
bool solar_os_storage_is_mounted(void) { return false; }
esp_err_t solar_os_storage_default_path(const char *relative, char *path, size_t capacity)
{ (void)relative; (void)path; (void)capacity; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_storage_mkdir(const char *path) { (void)path; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_storage_remove(const char *path) { (void)path; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_storage_sync_file(FILE *file) { (void)file; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_storage_sibling_path(const char *path, const char *suffix, char *out, size_t size)
{ (void)path; (void)suffix; (void)out; (void)size; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{ (void)staged; (void)active; (void)backup; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ (void)name; (void)mode; (void)handle; return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ (void)handle; (void)key; (void)value; return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ (void)handle; (void)key; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_ERR_NOT_SUPPORTED; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

static void test_native_inbox(void)
{
    api = solar_os_native_inbox_get_service("inbox", 1, sizeof(*api));
    assert(api && !solar_os_native_inbox_get_service("inbox", 2, 8));
    assert(!solar_os_native_inbox_get_service("inbox", 1, sizeof(*api) + 1));
    solar_os_inbox_status_t status;
    fail_allocation = true;
    check_init_reentry = true;
    assert(api->status(&status) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    fail_allocation = false;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && status.initialized && status.count == 0);
    check_init_reentry = false;
    assert(!status.persistent && !status.sound_available && ring_allocations == 1);
    char body[] = "report";
    solar_os_inbox_publish_t message = {
        .source = "provider", .body = body, .dedupe_key = "event-a",
        .source_id = 42, .priority = SOLAR_OS_INBOX_PRIORITY_HIGH,
    };
    uint32_t id, repeated;
    assert(api->publish(&message, &id) == SOLAR_OS_NATIVE_OK && id != 0);
    body[0] = 'X';
    assert(api->publish(&message, &repeated) == SOLAR_OS_NATIVE_OK && repeated == id);
    assert(api->matches_source_id(id, 42) && !api->matches_source_id(id, 43));
    solar_os_inbox_entry_t entry;
    size_t total;
    assert(api->snapshot(&entry, 1, true, &total) == 1 && total == 1);
    assert(entry.id == id && !strcmp(entry.body, "report"));
    assert(api->get(id, &entry, true) == SOLAR_OS_NATIVE_OK);
    assert(api->snapshot(&entry, 1, true, &total) == 0 && total == 0);
    assert(api->mark_read(id, false) == SOLAR_OS_NATIVE_OK);
    const char *sources[] = {"provider"};
    assert(api->delete_sources(sources, 1, &total) == SOLAR_OS_NATIVE_OK && total == 1);
    assert(api->delete_entry(id) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->test_sound(&id) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !id);
    assert(api->clear() == SOLAR_OS_NATIVE_OK);
    free(inbox_ring);
    vSemaphoreDelete(inbox_mutex);
}

int main(void)
{
    char path[] = "/tmp/solaros-inbox-history-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *file = fdopen(fd, "w+b"); assert(file);
    inbox_capacity = SOLAR_OS_INBOX_CAPACITY;
    inbox_ring = calloc(inbox_capacity, sizeof(*inbox_ring)); assert(inbox_ring);
    inbox_head = 1; inbox_count = 2; inbox_next_id = 3;
    inbox_ring[63].entry.id = 1; inbox_ring[0].entry.id = 2;
    strcpy(inbox_ring[63].entry.body, "oldest"); strcpy(inbox_ring[0].entry.body, "newest");
    inbox_store_header_t header;
    inbox_make_header(&header, 7);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    for (size_t i = 0; i < inbox_capacity; i++) {
        inbox_store_record_t record; inbox_make_record(&record, i);
        assert(fwrite(&record, sizeof(record), 1, file) == 1);
    }
    assert(fclose(file) == 0);
    solar_os_inbox_entry_t entries[2]; size_t count;
    assert(solar_os_inbox_saved_snapshot(path, entries, 2, &count) == ESP_OK && count == 2);
    assert(entries[0].id == 1 && !strcmp(entries[0].body, "oldest"));
    assert(entries[1].id == 2 && !strcmp(entries[1].body, "newest"));
    assert(solar_os_inbox_saved_snapshot(path, entries, 1, &count) == ESP_ERR_INVALID_SIZE);
    struct stat before, after; assert(stat(path, &before) == 0);
    file = fopen(path, "r+b"); assert(file);
    assert(fseek(file, INBOX_STORE_RECORDS_OFFSET, SEEK_SET) == 0 && fputc(0, file) != EOF);
    assert(fclose(file) == 0);
    assert(solar_os_inbox_saved_snapshot(path, entries, 2, &count) == ESP_FAIL);
    assert(stat(path, &after) == 0 && before.st_size == after.st_size);
    assert(inbox_count == 2 && inbox_head == 1 && inbox_ring[0].entry.id == 2);
    assert(unlink(path) == 0); free(inbox_ring);
    inbox_ring = NULL;
    inbox_head = inbox_count = 0;
    inbox_next_id = 1;
    test_native_inbox();
    puts("Inbox history, publication, deduplication, read state and initialization passed");
    return 0;
}
