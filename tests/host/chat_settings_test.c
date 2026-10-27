/* Real credential migration, checked files and removal recovery. */
#define SOLAR_OS_PACKAGE_SERVICE_CREDENTIALS 1
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#include "../../src/services/solar_os_credentials.c"

static credentials_store_blob_t legacy;
static bool legacy_present;
static unsigned write_opens;
esp_err_t nvs_open(const char *space, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(space, "credentials"));
    if (!legacy_present) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) write_opens++;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle; assert(!strcmp(key, "records"));
    if (!legacy_present) return ESP_ERR_NVS_NOT_FOUND;
    if (value) { assert(*length >= sizeof(legacy)); memcpy(value, &legacy, sizeof(legacy)); }
    *length = sizeof(legacy); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ (void)handle; assert(!strcmp(key, "records")); legacy_present = false; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
/* No nvs_set_blob implementation: new writes must never use it. */

static void reboot_credentials(void)
{
    free(credentials_store.records); free(credentials_store.scratch);
    vSemaphoreDelete(credentials_store.lock); vSemaphoreDelete(credentials_store.io_lock);
    memset(&credentials_store, 0, sizeof(credentials_store));
    memset(&credentials_file, 0, sizeof(credentials_file));
    assert(solar_os_credentials_init() == ESP_OK);
}

int main(void)
{
    assert(mkdtemp(test_root));
    legacy.magic = CREDENTIALS_STORE_MAGIC; legacy.version = 1;
    legacy.record_size = sizeof(credentials_disk_record_t); legacy.generation = 1; legacy.next_id = 2; legacy.count = 1;
    legacy.records[0].active = 1; legacy.records[0].id = 1; legacy.records[0].provider = 2;
    legacy.records[0].kind = SOLAR_OS_CREDENTIAL_SHARED_KEY; legacy.records[0].secret_len = 16;
    strlcpy(legacy.records[0].label, "group:example", sizeof(legacy.records[0].label));
    memset(legacy.records[0].secret, 0xa5, 16);
    legacy.crc32 = credentials_crc32(&legacy, sizeof(legacy) - sizeof(legacy.crc32)); legacy_present = true;
    fail_migration_promotion = true;
    assert(solar_os_credentials_init() == ESP_OK && legacy_present && write_opens == 0);
    solar_os_credential_info_t record;
    assert(solar_os_credentials_find(2, SOLAR_OS_CREDENTIAL_SHARED_KEY, "group:example", &record) == ESP_OK);
    reboot_credentials(); assert(!legacy_present && write_opens == 1);
    uint8_t secret[128]; size_t length;
    assert(solar_os_credentials_read_secret(record.id, secret, sizeof(secret), &length) == ESP_OK && length == 16 && secret[0] == 0xa5);
    reboot_credentials(); assert(write_opens == 1);
    legacy_present = true; /* Power loss between file commit and NVS cleanup. */
    reboot_credentials(); assert(!legacy_present && write_opens == 2);
    char backup[SOLAR_OS_STORAGE_PATH_MAX + 5];
    snprintf(backup, sizeof(backup), "%s.bak", credentials_file.path);
    assert(rename(credentials_file.path, backup) == 0);
    FILE *bad = fopen(credentials_file.path, "wb"); assert(bad); assert(fputs("bad", bad) >= 0); fclose(bad);
    reboot_credentials();
    assert(solar_os_credentials_find(2, SOLAR_OS_CREDENTIAL_SHARED_KEY, "group:example", &record) == ESP_OK);
    assert(access(backup, F_OK) != 0); /* Valid backup was restored to the primary. */
    assert(solar_os_messaging_init() == ESP_OK);
    solar_os_messaging_provider_id_t id;
    assert(solar_os_messaging_instance_create("detached", "remove-secrets", "profile", 8, &id) == ESP_OK);
    solar_os_credential_id_t private_id;
    assert(solar_os_credentials_put(id, SOLAR_OS_CREDENTIAL_TOKEN, "account", "private", 7, false, &private_id) == ESP_OK);
    assert(solar_os_messaging_instance_remove(id) == ESP_OK);
    reboot_credentials();
    assert(solar_os_credentials_read_secret(private_id, secret, sizeof(secret), &length) == ESP_ERR_NOT_FOUND);
    assert(solar_os_credentials_find(2, SOLAR_OS_CREDENTIAL_SHARED_KEY, "group:example", &record) == ESP_OK);
    uint32_t generation = credentials_store.generation;
    fail_migration_promotion = true;
    assert(solar_os_credentials_remove_provider(id) == ESP_OK && credentials_store.generation == generation);
    assert(fail_migration_promotion); fail_migration_promotion = false;
    FILE *stale = fopen(backup, "wb"); assert(stale); assert(fputs("stale secret", stale) >= 0); fclose(stale);
    assert(solar_os_credentials_remove_provider(id) == ESP_OK && access(backup, F_OK) != 0);
    /* Simulate a reset after the removal marker but before credential purge. */
    assert(solar_os_credentials_put(id, SOLAR_OS_CREDENTIAL_TOKEN, "interrupted", "leftover", 8, false, &private_id) == ESP_OK);
    restart_messages(); reboot_credentials();
    assert(solar_os_credentials_read_secret(private_id, secret, sizeof(secret), &length) == ESP_ERR_NOT_FOUND);
    /* With no valid backup, corruption must not revive stale NVS settings. */
    bad = fopen(credentials_file.path, "wb"); assert(bad); assert(fputs("bad", bad) >= 0); fclose(bad);
    legacy_present = true; reboot_credentials();
    solar_os_credentials_status_t status;
    assert(solar_os_credentials_get_status(&status) == ESP_OK && status.storage_error != ESP_OK && legacy_present);
    assert(solar_os_credentials_put(id, SOLAR_OS_CREDENTIAL_TOKEN, "unsafe", "secret", 6, false, NULL) == status.storage_error);
    assert(solar_os_credentials_remove_provider(id) == status.storage_error);
    const char *files[] = {".settings/credentials.bin", ".chat/profiles/004.bin", ".messages/history.bin", ".messages/location"};
    char path[160];
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        assert(solar_os_storage_default_path(files[i], path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    }
    const char *dirs[] = {".settings", ".chat/profiles", ".chat", ".messages"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        assert(solar_os_storage_default_path(dirs[i], path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("Chat settings migration and credential removal tests: ok");
    return 0;
}
