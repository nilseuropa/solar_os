/* Exercise the real services and on-disk formats with a host filesystem. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Model an embedded filesystem that cannot transfer a whole snapshot at
 * once. Apply the restriction to Contacts, including legacy-file reads. */
static size_t contacts_test_read(void *data, size_t size, size_t count, FILE *file)
{
    if (size * count > 4096U) { errno = ENOMEM; return 0; }
    return fread(data, size, count, file);
}
static size_t contacts_test_write(const void *data, size_t size, size_t count, FILE *file)
{
    if (size * count > 4096U) { errno = ENOMEM; return 0; }
    return fwrite(data, size, count, file);
}
#define fread contacts_test_read
#define fwrite contacts_test_write
#include "../../src/services/solar_os_contacts.c"
#undef fread
#undef fwrite
static bool fail_history_write;
static bool check_history_io_lock;
static size_t history_disk_reads;
static size_t history_test_read(void *data, size_t size, size_t count, FILE *file)
{
    history_disk_reads++;
    return fread(data, size, count, file);
}
static void assert_history_io_unlocked(void);
static size_t history_test_write(const void *data, size_t size, size_t count, FILE *file)
{
    if (check_history_io_lock && size > 256U) assert_history_io_unlocked();
    if (fail_history_write && size > 256U) { fail_history_write = false; errno = ENOSPC; return 0; }
    return fwrite(data, size, count, file);
}
#define fwrite history_test_write
#define fread history_test_read
#include "../../src/services/solar_os_messaging.c"
#undef fwrite
#undef fread

static char test_root[] = "/tmp/solaros-messaging-XXXXXX";
static const char *test_default_root = test_root;
static const char *test_sd_root;
static bool inbox_migration_fixture;
static void assert_history_io_unlocked(void)
{
    assert(pthread_mutex_trylock(messaging.lock) == 0);
    assert(pthread_mutex_unlock(messaging.lock) == 0);
}
bool solar_os_disk_image_path_busy(const char *path) { (void)path; return false; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(count, size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
bool solar_os_memory_is_external(const void *ptr) { return ptr != NULL; }
int64_t esp_timer_get_time(void) { return 1000000; }
TickType_t xTaskGetTickCount(void) { return 1000; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
bool __wrap_solar_os_storage_is_mounted(void) { return true; }
bool __wrap_solar_os_storage_sd_is_mounted(void) { return true; }
const char *__wrap_solar_os_storage_sd_mount_point(void) { return test_sd_root ? test_sd_root : test_root; }
bool __wrap_solar_os_storage_flash_is_mounted(void) { return true; }
const char *__wrap_solar_os_storage_flash_mount_point(void) { return test_root; }
esp_err_t __wrap_solar_os_storage_default_path(const char *relative, char *path, size_t size)
{
    int n = snprintf(path, size, "%s/%s", test_default_root, relative);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t __wrap_solar_os_storage_mkdir(const char *path) { return mkdir(path, 0700) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t __wrap_solar_os_storage_remove(const char *path) { return unlink(path) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t __wrap_solar_os_storage_get_usage(solar_os_storage_usage_t *usage)
{ memset(usage, 0, sizeof(*usage)); usage->free_bytes = 1024 * 1024; return ESP_OK; }
int __real_rename(const char *source, const char *destination);
static bool fail_migration_promotion;
static bool fail_history_move;
int __wrap_rename(const char *source, const char *destination)
{
    if (fail_history_move && strstr(source, ".promote")) { fail_history_move = false; errno = EIO; return -1; }
    if (fail_migration_promotion && strstr(source, ".tmp") != NULL && strstr(source, "/.messages/") == NULL) {
        fail_migration_promotion = false;
        errno = EIO;
        return -1;
    }
    struct stat info;
    if (stat(destination, &info) == 0) { errno = EEXIST; return -1; }
    return __real_rename(source, destination);
}
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }
esp_err_t solar_os_inbox_init(void) { return ESP_OK; }
esp_err_t solar_os_inbox_saved_snapshot(const char *path, solar_os_inbox_entry_t *entries,
    size_t capacity, size_t *count)
{
    (void)path; assert(capacity >= 1); *count = inbox_migration_fixture ? 1 : 0;
    if (*count) {
        memset(entries, 0, sizeof(*entries)); entries->id = 10; entries->source_id = 9999;
        entries->timestamp_ms = 1700000000000ULL; entries->unread = true;
        strcpy(entries->source, "msg-004"); strcpy(entries->topic, "#legacy-irc");
        strcpy(entries->sender, "remote"); strcpy(entries->body, "old Inbox history");
    }
    return ESP_OK;
}
esp_err_t solar_os_inbox_get_status(solar_os_inbox_status_t *status)
{ memset(status, 0, sizeof(*status)); return ESP_OK; }
size_t solar_os_inbox_snapshot(solar_os_inbox_entry_t *entries, size_t max, bool unread, size_t *total)
{ (void)entries; (void)max; (void)unread; if (total) *total = 0; return 0; }
void solar_os_inbox_set_clear_observer(solar_os_inbox_clear_observer_t observer, void *user)
{ (void)observer; (void)user; }
bool solar_os_inbox_matches_source_id(uint32_t id, uint64_t source) { (void)id; (void)source; return false; }
esp_err_t solar_os_inbox_publish(const solar_os_inbox_publish_t *message, uint32_t *id)
{ (void)message; if (id) *id = 1; return ESP_OK; }
esp_err_t solar_os_inbox_delete(uint32_t id) { (void)id; return ESP_OK; }
esp_err_t solar_os_inbox_mark_read(uint32_t id, bool read) { (void)id; (void)read; return ESP_OK; }
esp_err_t solar_os_inbox_delete_sources(const char *const *sources, size_t count, size_t *deleted)
{ (void)sources; (void)count; if (deleted) *deleted = 0; return ESP_OK; }

static void restart_contacts(void)
{
    free(contacts_store.contacts); free(contacts_store.endpoints); free(contacts_store.scratch);
    vSemaphoreDelete(contacts_store.lock); vSemaphoreDelete(contacts_store.io_lock);
    memset(&contacts_store, 0, sizeof(contacts_store));
    assert(solar_os_contacts_init() == ESP_OK);
}
static void restart_messages(void)
{
    while (messaging.providers) {
        messaging_provider_node_t *next = messaging.providers->next;
        free(messaging.providers); messaging.providers = next;
    }
    while (messaging.classes) {
        messaging_class_node_t *next = messaging.classes->next;
        free(messaging.classes); messaging.classes = next;
    }
    free(messaging.conversations); free(messaging.messages); free(messaging.outbox);
    free(messaging.events); free(messaging.record_scratch);
    free(messaging.history_scratch); free(messaging.pending_slot);
    free(messaging.history_buckets);
    free(messaging.navigation);
    vSemaphoreDelete(messaging.lock); vSemaphoreDelete(messaging.io_lock);
    vSemaphoreDelete(messaging.history_lock);
    memset(&messaging, 0, sizeof(messaging));
    assert(solar_os_messaging_init() == ESP_OK);
}

static void legacy_contacts_fixture(void)
{
    typedef struct __attribute__((packed)) {
        uint32_t generation, next_contact_id, next_endpoint_id, evicted;
        uint16_t contact_count, endpoint_count;
        contacts_disk_contact_t contacts[64];
        contacts_disk_endpoint_t endpoints[80];
    } legacy_data_t;
    legacy_data_t *data = calloc(1, sizeof(*data));
    assert(data);
    data->generation = 31; data->next_contact_id = 9; data->next_endpoint_id = 15;
    data->contact_count = 1; data->endpoint_count = 1;
    data->contacts[63] = (contacts_disk_contact_t){ .active = 1, .id = 7,
        .flags = SOLAR_OS_CONTACT_FLAG_PINNED, .created_ms = 100, .updated_ms = 200 };
    strcpy(data->contacts[63].display_name, "Retained trusted contact");
    data->endpoints[79] = (contacts_disk_endpoint_t){ .active = 1, .id = 11, .contact_id = 7,
        .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, .trust = SOLAR_OS_CONTACT_TRUST_TRUSTED,
        .address_len = 32, .provider_metadata_len = 3 };
    memset(data->endpoints[79].address, 0xa5, 32);
    memcpy(data->endpoints[79].provider_metadata, "old", 3);
    contacts_legacy_header_t header = { .magic = CONTACTS_STORE_MAGIC, .version = 1,
        .data_size = sizeof(*data), .generation = 17, .data_slot = 0,
        .data_crc32 = contacts_crc32(data, sizeof(*data)) };
    header.header_crc32 = contacts_crc32(&header, offsetof(contacts_legacy_header_t, header_crc32));
    contacts_legacy_header_t newer = header;
    newer.generation = 18; newer.data_slot = 1;
    newer.header_crc32 = contacts_crc32(&newer, offsetof(contacts_legacy_header_t, header_crc32));
    char path[160];
    assert(solar_os_storage_default_path(".contacts", path, sizeof(path)) == ESP_OK);
    assert(mkdir(path, 0700) == 0);
    assert(solar_os_storage_default_path(".contacts/contacts.bin", path, sizeof(path)) == ESP_OK);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&newer, sizeof(newer), 1, file) == 1);
    assert(fwrite(data, sizeof(*data), 1, file) == 1);
    data->generation++; /* Deliberately corrupt the newer copy: fall back to the valid older one. */
    assert(fwrite(data, sizeof(*data), 1, file) == 1);
    assert(fclose(file) == 0); free(data);
}

static void assert_legacy_contact(void)
{
    solar_os_contact_t contact; solar_os_endpoint_t endpoint;
    assert(solar_os_contacts_get(7, &contact) == ESP_OK);
    assert(contact.flags == SOLAR_OS_CONTACT_FLAG_PINNED);
    assert(strcmp(contact.display_name, "Retained trusted contact") == 0);
    assert(solar_os_contacts_get_endpoint(11, &endpoint) == ESP_OK);
    assert(endpoint.contact_id == 7 && endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED);
    assert(endpoint.address.bytes[0] == 0xa5 && endpoint.provider_metadata_len == 3);
    assert(memcmp(endpoint.provider_metadata, "old", 3) == 0);
}

static void test_contacts_migration_and_import(void)
{
    legacy_contacts_fixture();
    assert(solar_os_contacts_init() == ESP_OK);
    assert(contacts_store.legacy_format);
    assert_legacy_contact();
    /* A failed migration must leave the original file readable on restart. */
    char temporary[160];
    assert(solar_os_storage_default_path(".contacts/contacts.bin.tmp", temporary,
                                         sizeof(temporary)) == ESP_OK);
    assert(mkdir(temporary, 0700) == 0);
    assert(solar_os_contacts_flush() != ESP_OK);
    restart_contacts();
    assert(contacts_store.legacy_format);
    assert_legacy_contact();
    assert(rmdir(temporary) == 0);
    fail_migration_promotion = true;
    assert(solar_os_contacts_flush() != ESP_OK);
    assert(!fail_migration_promotion);
    restart_contacts();
    assert(contacts_store.legacy_format);
    assert_legacy_contact();
    uint32_t disk_generation = contacts_store.disk_generation;
    for (unsigned i = 0; i < SOLAR_OS_CONTACT_CAPACITY - 1; i++) {
        uint8_t key[32] = {0}; memcpy(key, &i, sizeof(i));
        assert(solar_os_contacts_import_discovered(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            key, sizeof(key), "Companion", SOLAR_OS_ENDPOINT_CAP_DIRECT, i + 300,
            NULL, 0, NULL, NULL) == ESP_OK);
    }
    assert(contacts_store.disk_generation == disk_generation); /* One batch, no per-contact writes. */
    uint8_t overflow_key[32]; memset(overflow_key, 0xff, sizeof(overflow_key));
    assert(solar_os_contacts_import_discovered(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        overflow_key, sizeof(overflow_key), "Overflow", SOLAR_OS_ENDPOINT_CAP_DIRECT,
        9000, NULL, 0, NULL, NULL) == ESP_ERR_NO_MEM);
    assert(contacts_store.evicted == 0 && contacts_store.contact_count == SOLAR_OS_CONTACT_CAPACITY);
    assert_legacy_contact();
    assert(solar_os_contacts_flush() == ESP_OK);
    assert(!contacts_store.legacy_format && contacts_store.disk_generation == disk_generation + 1);
    restart_contacts(); assert_legacy_contact();
    assert(contacts_store.contact_count == SOLAR_OS_CONTACT_CAPACITY);
    assert(contacts_store.evicted == 0);
    /* Recover the retained file if reset interrupts backup-and-replace. */
    assert(rename(contacts_store.store_path, contacts_store.backup_path) == 0);
    restart_contacts(); assert_legacy_contact();
    assert(contacts_store.contact_count == SOLAR_OS_CONTACT_CAPACITY);
    /* Updating an existing imported contact must preserve its endpoint ID at capacity. */
    uint8_t existing_key[32] = {0}; solar_os_endpoint_t before, after;
    assert(solar_os_contacts_find_endpoint(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        existing_key, 32, &before) == ESP_OK);
    assert(solar_os_contacts_import_discovered(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        existing_key, 32, "Updated", SOLAR_OS_ENDPOINT_CAP_DIRECT, 10000,
        NULL, 0, NULL, NULL) == ESP_OK);
    assert(solar_os_contacts_find_endpoint(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        existing_key, 32, &after) == ESP_OK);
    assert(before.id == after.id && after.last_seen_ms == 10000);
}

static solar_os_conversation_id_t group(const char *key, uint32_t ref)
{
    solar_os_messaging_conversation_upsert_t request = {
        .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, .provider_key = key,
        .kind = SOLAR_OS_CONVERSATION_GROUP, .title = "Public", .group_ref = ref };
    solar_os_conversation_id_t id;
    assert(solar_os_messaging_conversation_upsert(&request, &id) == ESP_OK);
    return id;
}
static void test_channel_ownership_and_restart(void)
{
    assert(solar_os_messaging_init() == ESP_OK);
    assert(solar_os_messaging_provider_register(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "meshcore") == ESP_OK);
    assert(solar_os_messaging_groups_begin_sync(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "group:") == ESP_OK);
    solar_os_conversation_id_t radio = group("group:2147483649", 2147483649U);
    solar_os_conversation_id_t ble = group("ble-group:0", 0x424c4500U);
    solar_os_message_key_t queued, sending, sent;
    assert(solar_os_messaging_send(ble, "Wrong transport", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_messaging_send(radio, "Queued before reboot", false, &queued) == ESP_OK);
    assert(solar_os_messaging_send(radio, "Sending before reboot", false, &sending) == ESP_OK);
    assert(solar_os_messaging_send(radio, "Already sent", false, &sent) == ESP_OK);
    solar_os_messaging_outbound_t requests[3];
    assert(solar_os_messaging_outbox_snapshot(requests, 3) == 3);
    assert(solar_os_messaging_outbox_update(requests[1].id, SOLAR_OS_DELIVERY_SENDING, NULL) == ESP_OK);
    assert(solar_os_messaging_outbox_update(requests[2].id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    restart_messages();
    solar_os_messaging_message_t *message = malloc(sizeof(*message)); assert(message);
    assert(solar_os_messaging_message_get(queued, message) == ESP_OK);
    assert(message->delivery == SOLAR_OS_DELIVERY_FAILED && strstr(message->error, "restart"));
    assert(solar_os_messaging_message_get(sending, message) == ESP_OK);
    assert(message->delivery == SOLAR_OS_DELIVERY_FAILED);
    assert(solar_os_messaging_message_get(sent, message) == ESP_OK);
    assert(message->delivery == SOLAR_OS_DELIVERY_SENT);
    assert(solar_os_messaging_outbox_snapshot(requests, 3) == 0);
    solar_os_messaging_conversation_t conversation;
    assert(solar_os_messaging_conversation_get(message->conversation_id, &conversation) == ESP_OK);
    char label[64]; solar_os_messaging_conversation_label(&conversation, label, sizeof(label));
    assert(strcmp(label, "Public [radio] (history)") == 0);
    assert(solar_os_messaging_send(conversation.id, "Inactive", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_messaging_groups_begin_sync(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE, "ble-group:") == ESP_OK);
    ble = group("ble-group:0", 0x424c4500U);
    /* Inspecting/upserting the unused local transport must not reactivate it. */
    radio = group("group:2147483649", 2147483649U);
    assert(solar_os_messaging_send(radio, "Still inactive", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_messaging_conversation_get(ble, &conversation) == ESP_OK);
    solar_os_messaging_conversation_label(&conversation, label, sizeof(label));
    assert(strcmp(label, "Public [companion]") == 0);
    assert(solar_os_messaging_send(ble, "Active companion", false, NULL) == ESP_OK);
    /* A same-transport refresh also retires channels no longer configured. */
    assert(solar_os_messaging_groups_begin_sync(SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
                                                "ble-group:") == ESP_OK);
    assert(solar_os_messaging_send(ble, "Removed channel", false, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(group("ble-group:0", 0x424c4500U) == ble);
    assert(solar_os_messaging_conversation_get(ble, &conversation) == ESP_OK);
    assert(!conversation.history_only);
    free(message);
}

static unsigned test_joins;
static bool reject_join;
static unsigned remove_prepares, remove_commits, remove_aborts;
static bool reject_remove_prepare;
static solar_os_messaging_provider_id_t remove_inside_join;
static esp_err_t test_normalize(void *context, const char *target, char *out, size_t size)
{
    assert(context == &test_joins);
    if (strncmp(target, "ROOM:", 5) == 0) target += 5;
    return strlcpy(out, target, size) < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static esp_err_t test_join(void *context, const char *target)
{
    assert(context == &test_joins); assert(strcmp(target, "#same") == 0);
    solar_os_messaging_status_t status;
    assert(solar_os_messaging_get_status(&status) == ESP_OK);
    if (remove_inside_join) assert(solar_os_messaging_instance_remove(remove_inside_join) == ESP_ERR_INVALID_STATE);
    test_joins++; return reject_join ? ESP_FAIL : ESP_OK;
}
static esp_err_t test_remove_prepare(void *context)
{
    assert(context == &test_joins); remove_prepares++;
    return reject_remove_prepare ? ESP_FAIL : ESP_OK;
}
static void test_remove_finish(void *context, bool removed)
{
    assert(context == &test_joins);
    if (removed) remove_commits++; else remove_aborts++;
}
static const solar_os_messaging_provider_class_t test_class = {
    .name = "test-net", .capabilities = SOLAR_OS_ENDPOINT_CAP_ROOM, .join = test_join,
    .normalize_target = test_normalize,
    .remove_prepare = test_remove_prepare, .remove_finish = test_remove_finish,
};
static void test_provider_instances(void)
{
    assert(solar_os_messaging_class_register(&test_class) == ESP_OK);
    unsigned config = 0x12345678;
    solar_os_messaging_provider_id_t a, b;
    assert(solar_os_messaging_instance_create("test-net", "network-a", &config, sizeof(config), &a) == ESP_OK);
    assert(solar_os_messaging_instance_create("test-net", "network-b", &config, sizeof(config), &b) == ESP_OK);
    assert(a > 3 && b > a);
    assert(solar_os_messaging_instance_register(a, "test-net", "network-a", &test_joins) == ESP_OK);
    assert(solar_os_messaging_instance_register(b, "test-net", "network-b", &test_joins) == ESP_OK);
    solar_os_conversation_id_t room_a, room_b;
    assert(solar_os_messaging_join(a, "ROOM:#same", &room_a) == ESP_OK);
    assert(solar_os_messaging_join(b, "#same", &room_b) == ESP_OK);
    assert(room_a != room_b && test_joins == 2);
    assert(solar_os_messaging_join_update(a, "#same", SOLAR_OS_MESSAGING_JOIN_JOINED, NULL) == ESP_OK);
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_OK && test_joins == 2);
    solar_os_messaging_conversation_t room;
    assert(solar_os_messaging_conversation_get(room_b, &room) == ESP_OK);
    assert(room.join_state == SOLAR_OS_MESSAGING_JOIN_PENDING);
    assert(solar_os_messaging_join_update(a, "#same", SOLAR_OS_MESSAGING_JOIN_LEFT, NULL) == ESP_OK);
    reject_join = true;
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_FAIL);
    assert(solar_os_messaging_conversation_get(room_a, &room) == ESP_OK && room.join_state == SOLAR_OS_MESSAGING_JOIN_FAILED);
    reject_join = false;
    solar_os_message_key_t key_a, key_b;
    assert(solar_os_messaging_send(room_a, "only A", false, &key_a) == ESP_OK);
    assert(solar_os_messaging_send(room_b, "only B", false, &key_b) == ESP_OK);
    solar_os_messaging_outbound_t *out = calloc(1, sizeof(*out)); assert(out);
    assert(solar_os_messaging_outbox_peek(a, out) == ESP_OK && out->message_key == key_a);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    assert(solar_os_messaging_outbox_peek(b, out) == ESP_OK && out->message_key == key_b);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_FAILED, "test") == ESP_OK);
    free(out);
    unsigned restored = 0;
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);
    fail_migration_promotion = true; unsigned updated = 0x77777777;
    assert(solar_os_messaging_profile_save(a, &updated, sizeof(updated)) != ESP_OK);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);
    restart_messages();
    assert(solar_os_messaging_provider_find("network-a") == a);
    assert(solar_os_messaging_provider_find("network-b") == b);
    assert(solar_os_messaging_provider_is_class(a, "test-net"));
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);
    assert(solar_os_messaging_class_register(&test_class) == ESP_OK);
    assert(solar_os_messaging_instance_register(a, "test-net", "network-a", &test_joins) == ESP_OK);
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_OK);
    solar_os_messaging_provider_id_t cursor = 0; solar_os_messaging_provider_status_t status;
    unsigned count = 0;
    while (solar_os_messaging_provider_next(&cursor, &status)) count++;
    assert(count == 5);

    /* A removal cannot destroy a callback's context while it is in use. */
    assert(solar_os_messaging_join_update(a, "#same", SOLAR_OS_MESSAGING_JOIN_LEFT, NULL) == ESP_OK);
    remove_inside_join = a;
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_OK);
    remove_inside_join = 0;
    assert(solar_os_messaging_instance_remove(1) == ESP_ERR_NOT_SUPPORTED);
    reject_remove_prepare = true;
    assert(solar_os_messaging_instance_remove(a) == ESP_FAIL && remove_commits == 0 && remove_aborts == 0);
    reject_remove_prepare = false;
    fail_migration_promotion = true;
    assert(solar_os_messaging_instance_remove(a) == ESP_FAIL && remove_aborts == 1);
    assert(solar_os_messaging_provider_find("network-a") == a);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_OK && restored == config);

    solar_os_conversation_id_t live_b = 0;
    solar_os_messaging_conversation_t conversations[32];
    size_t conversation_count = solar_os_messaging_conversation_snapshot(conversations, 32);
    for (size_t i = 0; i < conversation_count; i++) {
        if (conversations[i].provider == a) room_a = conversations[i].id;
        if (conversations[i].provider == b) live_b = conversations[i].id;
    }
    assert(live_b);
    solar_os_message_key_t pending_a, pending_b, pending_b2, sending_a;
    assert(solar_os_messaging_send(live_b, "B before", false, &pending_b) == ESP_OK);
    assert(solar_os_messaging_send(room_a, "A queued", false, &pending_a) == ESP_OK);
    assert(solar_os_messaging_send(live_b, "B after", false, &pending_b2) == ESP_OK);
    out = calloc(1, sizeof(*out)); assert(out);
    assert(solar_os_messaging_outbox_peek(a, out) == ESP_OK);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENDING, NULL) == ESP_OK);
    sending_a = pending_a;
    assert(solar_os_messaging_send(room_a, "A still queued", false, &pending_a) == ESP_OK);

    assert(solar_os_messaging_instance_remove(a) == ESP_OK && remove_commits == 1);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK && remove_commits == 1);
    assert(solar_os_messaging_provider_find("network-a") == 0);
    assert(solar_os_messaging_provider_get_status(a, &status) == ESP_OK && status.removed && !status.connected);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_profile_save(a, &config, sizeof(config)) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_instance_register(a, "test-net", "network-a", &test_joins) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_join(a, "#same", NULL) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_connect(a, true) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_send(room_a, "cannot send", false, NULL) != ESP_OK);
    solar_os_messaging_message_t message;
    assert(solar_os_messaging_message_get(pending_a, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(sending_a, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_conversation_get(room_a, &room) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_outbox_peek(a, out) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_outbox_peek(b, out) == ESP_OK && out->message_key == pending_b);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    assert(solar_os_messaging_outbox_peek(b, out) == ESP_OK && out->message_key == pending_b2);
    assert(solar_os_messaging_outbox_update(out->id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    free(out);

    char archived_path[160];
    assert(solar_os_storage_default_path(".chat/profiles/004.bin", archived_path, sizeof(archived_path)) == ESP_OK);
    struct stat info; assert(stat(archived_path, &info) == 0 && info.st_size == sizeof(messaging_profile_header_t));
    /* Simulate an old backup surviving an interrupted cleanup: the committed
     * removal record must take precedence, both before and after restart. */
    char backup_path[160]; assert(snprintf(backup_path, sizeof(backup_path), "%s.bak", archived_path) < (int)sizeof(backup_path));
    FILE *backup = fopen(backup_path, "wb"); assert(backup);
    messaging_profile_header_t old = {.magic = MESSAGING_PROFILE_MAGIC, .version = 1, .id = a, .size = sizeof(config)};
    strcpy(old.name, "network-a"); strcpy(old.class_name, "test-net");
    old.crc = (uint32_t)messaging_hash_update(messaging_hash_update(2166136261U, &old, sizeof(old)), &config, sizeof(config));
    assert(fwrite(&old, sizeof(old), 1, backup) == 1 && fwrite(&config, sizeof(config), 1, backup) == 1);
    assert(fclose(backup) == 0);
    restart_messages();
    assert(solar_os_messaging_provider_get_status(a, &status) == ESP_OK && status.removed);
    assert(solar_os_messaging_profile_load(a, &restored, sizeof(restored)) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(pending_a, &message) == ESP_ERR_NOT_FOUND);
    assert(strcmp(solar_os_messaging_provider_name(a), "network-a") == 0);
    solar_os_messaging_provider_id_t replacement;
    assert(solar_os_messaging_instance_create("test-net", "network-a", &config, sizeof(config), &replacement) == ESP_OK);
    assert(replacement > b && solar_os_messaging_provider_find("network-a") == replacement);
    restart_messages();
    assert(solar_os_messaging_provider_find("network-a") == replacement);
    assert(solar_os_messaging_provider_get_status(a, &status) == ESP_OK && status.removed);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK);
    assert(access(backup_path, F_OK) != 0);
    char path[160];
    assert(solar_os_storage_default_path(".chat/profiles/006.bin", path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    assert(solar_os_storage_default_path(".chat/profiles/004.bin", path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    assert(solar_os_storage_default_path(".chat/profiles/005.bin", path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    assert(solar_os_storage_default_path(".chat/profiles", path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
    assert(solar_os_storage_default_path(".chat", path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
}

static void test_unbound_and_evicted_removal(void)
{
    unsigned config = 0x98765432;
    solar_os_messaging_provider_id_t id, unsupported;
    assert(solar_os_messaging_instance_create("absent-class", "unbound", &config, sizeof(config), &id) == ESP_OK);
    solar_os_conversation_id_t room;
    solar_os_messaging_conversation_upsert_t request = {.provider = id, .provider_key = "#room",
        .title = "#room", .kind = SOLAR_OS_CONVERSATION_ROOM};
    assert(solar_os_messaging_conversation_upsert(&request, &room) == ESP_OK);
    solar_os_message_key_t queued;
    assert(solar_os_messaging_send(room, "will be evicted", false, &queued) == ESP_OK);
    for (unsigned i = 0; i < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY + 1; i++) {
        solar_os_messaging_inbound_t inbound = {.provider = id, .provider_message_key = i + 1,
            .conversation_key = "#room", .conversation_title = "#room", .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
            .sender = "remote", .body = "new inbound"};
        assert(solar_os_messaging_publish_inbound(&inbound, NULL, NULL) == ESP_OK);
    }
    solar_os_messaging_message_t message;
    assert(solar_os_messaging_message_get(queued, &message) == ESP_OK);
    solar_os_messaging_outbound_t *out = calloc(1, sizeof(*out)); assert(out);
    assert(solar_os_messaging_outbox_peek(id, out) == ESP_OK);
    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
    assert(solar_os_messaging_outbox_peek(id, out) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_outbox_snapshot(out, 1) == 0);
    free(out);
    static const solar_os_messaging_provider_class_t no_remove = {.name = "no-remove"};
    assert(solar_os_messaging_class_register(&no_remove) == ESP_OK);
    assert(solar_os_messaging_instance_create("no-remove", "unsupported", &config, sizeof(config), &unsupported) == ESP_OK);
    assert(solar_os_messaging_instance_register(unsupported, "no-remove", "unsupported", &test_joins) == ESP_OK);
    assert(solar_os_messaging_instance_remove(unsupported) == ESP_ERR_NOT_SUPPORTED);
    unsigned restored = 0;
    assert(solar_os_messaging_profile_load(unsupported, &restored, sizeof(restored)) == ESP_OK && restored == config);
    /* Reset after committing the removal marker but before purging history. */
    solar_os_messaging_provider_id_t interrupted;
    assert(solar_os_messaging_instance_create("absent-class", "interrupted", &config, sizeof(config), &interrupted) == ESP_OK);
    solar_os_messaging_inbound_t inbound = {.provider = interrupted, .provider_message_key = 1,
        .conversation_key = "#reset", .conversation_title = "#reset", .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
        .sender = "remote", .body = "must disappear after reset"};
    solar_os_message_key_t removed_key;
    assert(solar_os_messaging_publish_inbound(&inbound, NULL, &removed_key) == ESP_OK);
    solar_os_messaging_provider_status_t status;
    assert(solar_os_messaging_provider_get_status(interrupted, &status) == ESP_OK);
    bool committed = false;
    assert(messaging_archive_profile(&status, &committed) == ESP_OK && committed);
    restart_messages();
    assert(solar_os_messaging_message_get(removed_key, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_provider_find("interrupted") == 0);
    char path[160], relative[64];
    const solar_os_messaging_provider_id_t ids[] = {id, unsupported, interrupted};
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), ".chat/profiles/%03u.bin", ids[i]);
        assert(solar_os_storage_default_path(relative, path, sizeof(path)) == ESP_OK && unlink(path) == 0);
    }
    assert(solar_os_storage_default_path(".chat/profiles", path, sizeof(path)) == ESP_OK && rmdir(path) == 0);
    assert(solar_os_storage_default_path(".chat", path, sizeof(path)) == ESP_OK && rmdir(path) == 0);
}

static bool count_history(const solar_os_messaging_message_t *message, void *user)
{
    assert(message->key && message->conversation_id);
    (*(size_t *)user)++;
    solar_os_messaging_status_t status;
    assert(solar_os_messaging_get_status(&status) == ESP_OK);
    return true;
}

static void test_history_migration(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/.messages", test_root);
    assert(mkdir(path, 0700) == 0);
    strcat(path, "/messages.bin");
    FILE *file = fopen(path, "wb"); assert(file);
    messaging_store_header_t h = {.magic = MESSAGING_STORE_MAGIC,
        .version = 1, .capacity = 2, .generation = 1, .count = 2, .next_message_key = 400};
    h.crc32 = messaging_crc32(&h, offsetof(messaging_store_header_t, crc32));
    assert(fwrite(&h, sizeof(h), 1, file) == 1 && fwrite(&h, sizeof(h), 1, file) == 1);
    for (unsigned i = 0; i < 2; i++) {
        messaging_store_record_t r = {0};
        r.message.key = 398 + i; r.message.provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY;
        r.message.direction = i ? SOLAR_OS_MESSAGE_OUTBOUND : SOLAR_OS_MESSAGE_INBOUND;
        r.message.delivery = i ? SOLAR_OS_DELIVERY_SENT : SOLAR_OS_DELIVERY_RECEIVED;
        r.message.provider_message_key = i ? 0 : 72;
        strcpy(r.message.body, "legacy full history"); strcpy(r.conversation_key, "legacy");
        strcpy(r.conversation_title, "Legacy"); r.conversation_kind = SOLAR_OS_CONVERSATION_ROOM;
        r.crc32 = messaging_crc32(&r, offsetof(messaging_store_record_t, crc32));
        assert(fwrite(&r, sizeof(r), 1, file) == 1);
    }
    assert(fclose(file) == 0);
    struct stat before, after; assert(stat(path, &before) == 0);
    inbox_migration_fixture = true;
    assert(solar_os_messaging_init() == ESP_OK);
    inbox_migration_fixture = false;
    solar_os_messaging_status_t status;
    assert(solar_os_messaging_get_status(&status) == ESP_OK && status.persistent && status.messages == 3);
    solar_os_messaging_message_t m;
    assert(solar_os_messaging_message_get(399, &m) == ESP_OK && m.direction == SOLAR_OS_MESSAGE_OUTBOUND && m.delivery == SOLAR_OS_DELIVERY_SENT);
    assert(solar_os_messaging_message_get(9999, &m) == ESP_OK && m.timestamp_ms == 1700000000000ULL);
    assert(solar_os_messaging_provider_cursor(1, "legacy") == 72);
    assert(stat(path, &after) == 0 && before.st_size == after.st_size && before.st_mtime == after.st_mtime);
    restart_messages();
    assert(solar_os_messaging_get_status(&status) == ESP_OK && status.messages == 3);
    assert(solar_os_messaging_clear(0, NULL) == ESP_OK);
    assert(unlink(path) == 0);
}

static void test_disk_history_beyond_cache(void)
{
    check_history_io_lock = true;
    solar_os_messaging_inbound_t in = {.provider = 4, .conversation_key = "#quiet-irc",
        .conversation_title = "#quiet-irc", .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
        .provider_message_key = 1, .sender = "remote", .body = "keep IRC history"};
    solar_os_message_key_t irc, first = 0, last;
    assert(solar_os_messaging_publish_inbound(&in, NULL, &irc) == ESP_OK);
    in.provider = 1; in.conversation_key = "busy-gateway"; in.conversation_title = "Busy gateway";
    in.body = "gateway replay";
    for (unsigned i = 1; i <= 220; i++) {
        in.provider_message_key = i;
        assert(solar_os_messaging_publish_inbound(&in, NULL, &last) == ESP_OK);
        if (i == 1) first = last;
    }
    solar_os_messaging_status_t status;
    assert(solar_os_messaging_get_status(&status) == ESP_OK && status.messages == 221 &&
        status.cached_messages == 64 && status.dropped_messages == 0 && status.unread == 221);
    solar_os_messaging_message_t m;
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK && !strcmp(m.body, "keep IRC history"));
    assert(solar_os_messaging_message_get(first, &m) == ESP_OK && m.provider_message_key == 1);
    solar_os_conversation_id_t busy = m.conversation_id;
    size_t count = 0;
    assert(solar_os_messaging_message_visit(0, 0, count_history, &count, NULL) == 221 && count == 221);
    solar_os_messaging_message_t page[17]; bool more;
    assert(solar_os_messaging_message_page(busy, 1, 0, page, 17, &count, &more) == ESP_OK && count == 17 && more);
    assert(page[0].provider_message_key == 204 && page[16].provider_message_key == 220);
    assert(solar_os_messaging_message_page(busy, 1, 204, page, 17, &count, &more) == ESP_OK && count == 16 && !more);
    assert(page[0].provider_message_key == 1 && page[15].provider_message_key == 16);
    assert(solar_os_messaging_mark_read(busy) == ESP_OK);
    assert(solar_os_messaging_get_status(&status) == ESP_OK && status.unread == 1);
    solar_os_message_key_t sent;
    assert(solar_os_messaging_send(busy, "outbound history", false, &sent) == ESP_OK);
    solar_os_messaging_outbound_t outbound;
    assert(solar_os_messaging_outbox_peek(1, &outbound) == ESP_OK);
    for (unsigned i = 221; i <= 300; i++) {
        in.provider_message_key = i;
        assert(solar_os_messaging_publish_inbound(&in, NULL, &last) == ESP_OK);
    }
    assert(solar_os_messaging_outbox_update(outbound.id, SOLAR_OS_DELIVERY_SENT, NULL) == ESP_OK);
    restart_messages();
    assert(solar_os_messaging_message_get(sent, &m) == ESP_OK && m.delivery == SOLAR_OS_DELIVERY_SENT && m.direction == SOLAR_OS_MESSAGE_OUTBOUND);
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK);
    assert(solar_os_messaging_provider_cursor(1, "busy-gateway") == 300);
    in.provider_message_key = 1;
    bool inserted = true;
    assert(solar_os_messaging_publish_inbound(&in, &inserted, NULL) == ESP_OK && !inserted);
    assert(solar_os_messaging_message_get(first, &m) == ESP_OK && !m.unread);
    /* Deleting one archive record is durable and isolated. A failed tombstone
     * leaves the original message readable, including after reopening. */
    fail_history_write = true;
    assert(solar_os_messaging_message_delete(first) != ESP_OK);
    assert(solar_os_messaging_message_get(first, &m) == ESP_OK);
    restart_messages();
    assert(solar_os_messaging_message_get(first, &m) == ESP_OK);
    assert(solar_os_messaging_message_delete(first) == ESP_OK);
    restart_messages();
    assert(solar_os_messaging_message_get(first, &m) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(sent, &m) == ESP_OK);
    solar_os_messaging_message_t filtered[3]; size_t filtered_count; bool filtered_more;
    assert(solar_os_messaging_message_page_filtered(0, 1, true, 0, filtered, 3, &filtered_count, &filtered_more) == ESP_OK);
    assert(filtered_count == 3 && filtered_more);
    solar_os_message_key_t newest_unread = filtered[2].key;
    for (size_t i = 0; i < filtered_count; i++) assert(filtered[i].unread && filtered[i].provider == 1);
    assert(solar_os_messaging_message_page_filtered(0, 1, true, 3, filtered, 3, &filtered_count, &filtered_more) == ESP_OK);
    assert(filtered_count == 3 && filtered[2].key != newest_unread);
    /* A failed durable write cannot queue a send or admit a received message. */
    assert(solar_os_messaging_message_get(sent, &m) == ESP_OK); busy = m.conversation_id;
    assert(solar_os_messaging_get_status(&status) == ESP_OK); size_t saved = status.messages;
    fail_history_write = true;
    assert(solar_os_messaging_send(busy, "disk is full", false, NULL) != ESP_OK);
    assert(solar_os_messaging_outbox_peek(1, &outbound) == ESP_ERR_NOT_FOUND);
    in.provider_message_key = 301; fail_history_write = true; inserted = true;
    assert(solar_os_messaging_publish_inbound(&in, &inserted, NULL) != ESP_OK && !inserted);
    assert(solar_os_messaging_get_status(&status) == ESP_OK && status.messages == saved);
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK);
    /* A later-mounted default volume cannot hide the selected archive. */
    char sd[256]; snprintf(sd, sizeof(sd), "%s/sd", test_root); assert(mkdir(sd, 0700) == 0);
    test_default_root = sd;
    restart_messages();
    assert(solar_os_messaging_message_get(sent, &m) == ESP_OK);
    test_default_root = test_root; assert(rmdir(sd) == 0);
    size_t removed;
    assert(solar_os_messaging_clear(1, &removed) == ESP_OK && removed == 300);
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK);
    restart_messages();
    assert(solar_os_messaging_message_get(sent, &m) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK);
    /* A torn update must leave the previous checked copy readable. */
    assert(solar_os_messaging_mark_read(m.conversation_id) == ESP_OK);
    messaging_history_lock();
    int32_t index;
    assert(messaging_history_find_locked(irc, messaging.record_scratch, &index) == ESP_OK);
    FILE *file = fopen(messaging.store_path, "r+b"); assert(file);
    int selected = messaging_history_read_locked(file, (size_t)index, NULL); assert(selected >= 0);
    long crc_offset = (long)((size_t)index * MESSAGING_HISTORY_RECORD_BYTES +
        (size_t)selected * sizeof(messaging_history_copy_t) + offsetof(messaging_history_copy_t, crc32));
    assert(fseek(file, crc_offset, SEEK_SET) == 0);
    uint32_t corrupt = 0; assert(fwrite(&corrupt, sizeof(corrupt), 1, file) == 1);
    assert(fclose(file) == 0);
    messaging_history_unlock();
    restart_messages();
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK && !strcmp(m.body, "keep IRC history"));
    /* Recover the rename window of an interrupted archive replacement. */
    char backup[SOLAR_OS_STORAGE_PATH_MAX + 16];
    snprintf(backup, sizeof(backup), "%s.bak", messaging.store_path);
    assert(rename(messaging.store_path, backup) == 0);
    restart_messages();
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK);
    char offline[SOLAR_OS_STORAGE_PATH_MAX + 16];
    snprintf(offline, sizeof(offline), "%s.offline", messaging.store_path);
    assert(rename(messaging.store_path, offline) == 0);
    restart_messages();
    assert(solar_os_messaging_get_status(&status) == ESP_OK && !status.persistent && status.storage_error != ESP_OK);
    assert(access(messaging.store_path, F_OK) != 0 && access(offline, F_OK) == 0);
    assert(rename(offline, messaging.store_path) == 0);
    restart_messages();
    assert(solar_os_messaging_message_get(irc, &m) == ESP_OK);
    assert(solar_os_messaging_conversation_remove(4, "#quiet-irc") == ESP_OK);
    restart_messages();
    assert(solar_os_messaging_message_get(irc, &m) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_clear(0, NULL) == ESP_OK);
    check_history_io_lock = false;
}

static void *history_producer(void *argument)
{
    unsigned provider = (unsigned)(uintptr_t)argument;
    solar_os_messaging_inbound_t in = {.provider = provider,
        .conversation_key = "concurrent", .conversation_title = "Concurrent",
        .conversation_kind = SOLAR_OS_CONVERSATION_ROOM, .sender = "peer", .body = "parallel arrival"};
    for (unsigned i = 1; i <= 70; i++) {
        in.provider_message_key = i; bool inserted = false;
        assert(solar_os_messaging_publish_inbound(&in, &inserted, NULL) == ESP_OK && inserted);
    }
    return NULL;
}

static void test_concurrent_history(void)
{
    pthread_t a, b;
    assert(pthread_create(&a, NULL, history_producer, (void *)(uintptr_t)1) == 0);
    assert(pthread_create(&b, NULL, history_producer, (void *)(uintptr_t)4) == 0);
    assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    check_history_io_lock = false;
    restart_messages();
    size_t count = 0;
    assert(solar_os_messaging_message_visit(0, 0, count_history, &count, NULL) == 140 && count == 140);
    assert(solar_os_messaging_provider_cursor(1, "concurrent") == 70);
    assert(solar_os_messaging_provider_cursor(4, "concurrent") == 70);
    assert(solar_os_messaging_clear(0, NULL) == ESP_OK);
}

static void test_late_sd_promotion(void)
{
    solar_os_messaging_inbound_t in = {.provider=1, .conversation_key="late-sd", .conversation_title="Late SD",
        .conversation_kind=SOLAR_OS_CONVERSATION_ROOM, .provider_message_key=991, .sender="peer", .body="preserved during promotion"};
    solar_os_message_key_t key;
    assert(solar_os_messaging_publish_inbound(&in,NULL,&key)==ESP_OK);
    char sd[256], target[256], original[256];
    snprintf(sd,sizeof(sd),"%s/late-sd",test_root); assert(mkdir(sd,0700)==0);
    snprintf(target,sizeof(target),"%s/late-sd/.messages",test_root); assert(mkdir(target,0700)==0);
    snprintf(target,sizeof(target),"%s/late-sd/.messages/history.bin",test_root);
    strlcpy(original,messaging.store_path,sizeof(original));
    fail_history_move=true;
    messaging_history_lock();
    assert(messaging_history_relocate_locked(target)!=ESP_OK);
    messaging_history_unlock();
    assert(!strcmp(original,messaging.store_path) && access(original,F_OK)==0 && access(target,F_OK)!=0);
    test_sd_root=sd; messaging.sd_promotion_checked=false;
    solar_os_messaging_status_t status;
    assert(solar_os_messaging_get_status(&status)==ESP_OK && status.persistent && status.storage_error==ESP_OK);
    assert(!strcmp(messaging.store_path,target) && access(original,F_OK)!=0);
    restart_messages();
    solar_os_messaging_message_t message;
    assert(solar_os_messaging_message_get(key,&message)==ESP_OK && !strcmp(message.body,in.body));
    /* Conversation listing and cursors must attach a returned volume even
     * when no status query or incoming message has run since boot. */
    char offline[272]; snprintf(offline,sizeof(offline),"%s.offline",target);
    assert(rename(target,offline)==0);
    restart_messages(); assert(!messaging.persistent);
    assert(rename(offline,target)==0);
    solar_os_messaging_conversation_t conversations[SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY];
    assert(solar_os_messaging_conversation_snapshot(conversations,SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY)>0);
    assert(messaging.persistent);
    assert(solar_os_messaging_message_get(key,&message)==ESP_OK);
    assert(rename(target,offline)==0);
    restart_messages(); assert(!messaging.persistent);
    assert(rename(offline,target)==0);
    assert(solar_os_messaging_provider_cursor(1,"late-sd")==991);
    /* An interrupted location replacement recovers the old location record. */
    char pin_backup[256]; snprintf(pin_backup,sizeof(pin_backup),"%s.bak",messaging.location_path);
    assert(rename(messaging.location_path,pin_backup)==0);
    restart_messages(); assert(solar_os_messaging_message_get(key,&message)==ESP_OK);
    messaging_history_lock();
    assert(messaging_history_relocate_locked(original)==ESP_OK);
    messaging_history_unlock();
    test_sd_root=NULL;
    snprintf(target,sizeof(target),"%s/late-sd/.messages",test_root); assert(rmdir(target)==0);
    assert(rmdir(sd)==0);
    assert(solar_os_messaging_clear(0,NULL)==ESP_OK);
}

static void test_history_navigation_io(void)
{
    assert(messaging_crc32("123456789",9)==0xcbf43926U);
    solar_os_messaging_inbound_t in={.provider=4,.conversation_key="performance-quiet",.conversation_title="Quiet",
        .conversation_kind=SOLAR_OS_CONVERSATION_ROOM,.sender="peer",.body="quiet history"};
    solar_os_message_key_t quiet_key;
    for(unsigned i=1;i<=3;i++){in.provider_message_key=i;assert(solar_os_messaging_publish_inbound(&in,NULL,&quiet_key)==ESP_OK);}
    solar_os_messaging_message_t m;
    assert(solar_os_messaging_message_get(quiet_key,&m)==ESP_OK);
    solar_os_conversation_id_t quiet=m.conversation_id;
    in.provider=1;in.conversation_key="performance-busy";in.conversation_title="Busy";
    for(unsigned i=1;i<=300;i++){in.provider_message_key=i;assert(solar_os_messaging_publish_inbound(&in,NULL,NULL)==ESP_OK);}
    solar_os_messaging_message_t page[8];size_t count;bool more;
    history_disk_reads=0;
    assert(solar_os_messaging_message_page(quiet,0,0,page,8,&count,&more)==ESP_OK && count==3 && !more);
    assert(history_disk_reads==3); /* No reads of the busy conversation. */
    history_disk_reads=0;
    assert(solar_os_messaging_message_page(quiet,0,0,page,8,&count,&more)==ESP_OK && count==3);
    assert(history_disk_reads==0); /* Reuse the existing body cache. */
    history_disk_reads=0;
    assert(solar_os_messaging_mark_read(quiet)==ESP_OK && history_disk_reads<=6);
    history_disk_reads=0;
    assert(solar_os_messaging_mark_read(quiet)==ESP_OK && history_disk_reads==0);
    solar_os_messaging_conversation_upsert_t request={.provider=1,.provider_key="performance-empty",.title="Empty",.kind=SOLAR_OS_CONVERSATION_ROOM};
    solar_os_conversation_id_t empty;
    assert(solar_os_messaging_conversation_upsert(&request,&empty)==ESP_OK);
    history_disk_reads=0;
    assert(solar_os_messaging_message_page(empty,0,0,page,8,&count,&more)==ESP_OK && count==0 && !more);
    assert(solar_os_messaging_mark_read(empty)==ESP_OK && history_disk_reads==0);
    assert(solar_os_messaging_provider_cursor(4,"performance-quiet")==3 && history_disk_reads==0);
    in.provider=4;in.conversation_key="performance-quiet";in.conversation_title="Quiet";in.provider_message_key=4;
    assert(solar_os_messaging_publish_inbound(&in,NULL,&quiet_key)==ESP_OK);
    history_disk_reads=0;
    assert(solar_os_messaging_mark_read(quiet)==ESP_OK && history_disk_reads<=2);
    restart_messages();
    assert(solar_os_messaging_message_get(quiet_key,&m)==ESP_OK);
    history_disk_reads=0;
    assert(solar_os_messaging_provider_cursor(4,"performance-quiet")==4 && history_disk_reads==0);
    assert(solar_os_messaging_message_page(m.conversation_id,0,0,page,8,&count,&more)==ESP_OK && count==4);
    for(size_t i=0;i<count;i++)assert(!page[i].unread && page[i].provider_message_key==i+1);
    assert(solar_os_messaging_clear(0,NULL)==ESP_OK);
}

static void test_masked_provider_cursor(void)
{
    const uint64_t mask = UINT64_C(0xf) << 60;
    const uint64_t first = UINT64_C(1) << 60;
    const uint64_t second = UINT64_C(2) << 60;
    solar_os_messaging_inbound_t in = {.provider = 4,
        .conversation_key = "relay-cursors", .conversation_title = "Relays",
        .conversation_kind = SOLAR_OS_CONVERSATION_ROOM, .sender = "peer", .body = "history"};
    in.provider_message_key = first | 999;
    assert(solar_os_messaging_publish_inbound(&in, NULL, NULL) == ESP_OK);
    for (unsigned i = 1; i <= SOLAR_OS_MESSAGING_MESSAGE_CAPACITY + 10; i++) {
        in.provider_message_key = second | i;
        assert(solar_os_messaging_publish_inbound(&in, NULL, NULL) == ESP_OK);
    }
    for (unsigned restart = 0; restart < 2; restart++) {
        assert(solar_os_messaging_provider_cursor_masked(4, "relay-cursors", mask, first) == 999);
        assert(solar_os_messaging_provider_cursor_masked(4, "relay-cursors", mask, second) ==
            SOLAR_OS_MESSAGING_MESSAGE_CAPACITY + 10);
        assert(solar_os_messaging_provider_cursor_masked(4, "relay-cursors", mask, 0) == 0);
        assert(solar_os_messaging_provider_cursor_masked(1, "relay-cursors", mask, first) == 0);
        restart_messages();
    }
    assert(solar_os_messaging_clear(0, NULL) == ESP_OK);
}

int main(void)
{
    assert(mkdtemp(test_root));
    test_contacts_migration_and_import();
    test_history_migration();
    test_disk_history_beyond_cache();
    test_concurrent_history();
    test_late_sd_promotion();
    test_history_navigation_io();
    test_masked_provider_cursor();
    test_channel_ownership_and_restart();
    test_provider_instances();
    test_unbound_and_evicted_removal();
    char path[160];
    const char *paths[] = { ".contacts/contacts.bin", ".messages/history.bin", ".messages/location", ".contacts", ".messages" };
    for (size_t i = 0; i < 5; i++) {
        assert(solar_os_storage_default_path(paths[i], path, sizeof(path)) == ESP_OK);
        assert((i < 3 ? unlink(path) : rmdir(path)) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("messaging service tests: ok");
    return 0;
}
