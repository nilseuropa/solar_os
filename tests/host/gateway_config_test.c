/* Real Messaging, Contacts and Gateway services, with an in-memory legacy NVS. */
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#include "../../src/services/solar_os_chat.c"
#include <pthread.h>

static pthread_barrier_t scope_barrier;
static void *check_task_scope(void *store)
{
    void *previous = solar_os_gateway_scope_enter(store);
    pthread_barrier_wait(&scope_barrier);
    assert(solar_os_gateway_current_instance() == store);
    void *nested = solar_os_gateway_scope_enter(&default_chat);
    assert(solar_os_gateway_current_instance() == &default_chat);
    solar_os_gateway_scope_leave(nested);
    assert(solar_os_gateway_current_instance() == store);
    solar_os_gateway_scope_leave(previous);
    return NULL;
}

static bool legacy_url = true, legacy_token = true, legacy_enabled = true, legacy_user = true;
static unsigned nvs_writes;
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "chat") == 0);
    if (!legacy_url && !legacy_token && !legacy_enabled && !legacy_user) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) nvs_writes++;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    (void)handle;
    const char *text = NULL;
    if (strcmp(key, "url") == 0 && legacy_url) text = "chats://example.org:7443";
    if (strcmp(key, "token") == 0 && legacy_token) text = "retained-token";
    if (strcmp(key, "user") == 0 && legacy_user) text = "obsolete-user";
    if (!text) return ESP_ERR_NVS_NOT_FOUND;
    size_t length = strlen(text) + 1;
    if (value && *size < length) return ESP_ERR_INVALID_SIZE;
    if (value) memcpy(value, text, length);
    *size = length; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    if (strcmp(key, "enabled") || !legacy_enabled) return ESP_ERR_NVS_NOT_FOUND;
    *value = 1; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    if (!strcmp(key, "url")) legacy_url = false;
    if (!strcmp(key, "token")) legacy_token = false;
    if (!strcmp(key, "enabled")) legacy_enabled = false;
    if (!strcmp(key, "user")) legacy_user = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
void solar_os_identity_get_user(char *out, size_t size) { strlcpy(out, "user", size); }
void solar_os_identity_get_hostname(char *out, size_t size) { strlcpy(out, "device", size); }

static bool record_message_security(const solar_os_messaging_message_t *message, void *user)
{
    *(uint32_t *)user |= message->security_flags;
    return true;
}

static void check_gateway_v1_security(solar_os_conversation_id_t room)
{
    const uint32_t expected = SOLAR_OS_SECURITY_LEGACY_INSECURE | SOLAR_OS_SECURITY_TRANSPORT_SECURED;
    solar_os_messaging_conversation_t conversation;
    assert(solar_os_messaging_conversation_get(room, &conversation) == ESP_OK);
    assert(conversation.security_flags == expected);
    uint32_t message_flags = 0;
    assert(solar_os_messaging_message_visit(room, 0, record_message_security, &message_flags, NULL) > 0);
    assert(message_flags == expected);
}

static void reboot_gateway(void)
{
    while (gateways.instances) {
        solar_os_chat_store_state_t *next = gateways.instances->next;
        gateway_store_free(gateways.instances); gateways.instances = next;
    }
    vSemaphoreDelete(gateways.edit_lock); vSemaphoreDelete(gateways.worker_lock);
    memset(&gateways, 0, sizeof(gateways)); selected_chat = NULL; restart_messages();
    atomic_store(&gateway_ready, false);
    assert(solar_os_chat_init() == ESP_OK);
}

int main(void)
{
    assert(mkdtemp(test_root));
    /* A failed staged promotion must retain every legacy key. */
    fail_migration_promotion = true;
    assert(solar_os_chat_init() == ESP_OK);
    assert(legacy_url && legacy_token && legacy_enabled && legacy_user && nvs_writes == 0);
    reboot_gateway();
    assert(!legacy_url && !legacy_token && !legacy_enabled && !legacy_user && nvs_writes == 1);
    solar_os_chat_config_t config;
    assert(solar_os_chat_get_config(&config) == ESP_OK);
    assert(config.enabled && strcmp(config.url, "chats://example.org:7443") == 0);
    assert(strcmp(config.token, "retained-token") == 0);
    solar_os_chat_channel_t channels[32];
    assert(solar_os_chat_channel_snapshot(channels, 32) == 1 && channels[0].desired);
    /* Reopening a valid file must not allocate an empty NVS namespace. */
    reboot_gateway(); assert(nvs_writes == 1);
    /* Recover a power interruption between file commit and NVS cleanup. */
    legacy_url = legacy_token = legacy_enabled = legacy_user = true;
    reboot_gateway();
    assert(!legacy_url && !legacy_token && !legacy_enabled && !legacy_user && nvs_writes == 2);
    /* Failed saves leave the active connection unchanged. */
    fail_migration_promotion = true;
    assert(solar_os_chat_configure("chats://other.example:7443", "new-token") != ESP_OK);
    assert(solar_os_chat_get_config(&config) == ESP_OK && strcmp(config.token, "retained-token") == 0);
    assert(solar_os_chat_leave("general") == ESP_OK);
    reboot_gateway();
    assert(solar_os_chat_channel_snapshot(channels, 32) == 1 && !channels[0].desired);
    /* Identical room names must belong to independent stores and queues. */
    solar_os_messaging_provider_id_t a, b;
    const char *settings_a[] = {"chats://a.example:7443", "token-a"};
    const char *settings_b[] = {"chats://b.example:7443", "token-b"};
    assert(solar_os_messaging_instance_add("gateway", "alpha", 2, settings_a, &a) == ESP_OK);
    assert(solar_os_messaging_instance_add("gateway", "beta", 2, settings_b, &b) == ESP_OK);
    assert(a != b);
    pthread_t task_a, task_b;
    assert(pthread_barrier_init(&scope_barrier, NULL, 2) == 0);
    assert(pthread_create(&task_a, NULL, check_task_scope, gateway_find(a)) == 0);
    assert(pthread_create(&task_b, NULL, check_task_scope, gateway_find(b)) == 0);
    assert(pthread_join(task_a, NULL) == 0 && pthread_join(task_b, NULL) == 0);
    assert(pthread_barrier_destroy(&scope_barrier) == 0);
    solar_os_conversation_id_t room_a, room_b;
    assert(solar_os_messaging_join(a, "shared", &room_a) == ESP_OK);
    assert(solar_os_messaging_join(b, "shared", &room_b) == ESP_OK && room_a != room_b);
    assert(solar_os_messaging_connect(a, true) == ESP_OK);
    assert(solar_os_messaging_connect(b, true) == ESP_OK);
    solar_os_chat_status_t status;
    assert(solar_os_gateway_instance_get_status(a, &status) == ESP_OK && status.enabled && !strcmp(status.url, settings_a[0]));
    assert(solar_os_gateway_instance_get_status(b, &status) == ESP_OK && status.enabled && !strcmp(status.url, settings_b[0]));
    void *scope = solar_os_gateway_scope_enter(gateway_find(a));
    assert(solar_os_chat_send("shared", "from-a") == ESP_OK);
    solar_os_gateway_scope_leave(scope);
    check_gateway_v1_security(room_a);
    scope = solar_os_gateway_scope_enter(gateway_find(b));
    solar_os_chat_command_t command;
    while (solar_os_chat_outbox_peek(&command) == ESP_OK) {
        assert(command.type != SOLAR_OS_CHAT_COMMAND_MESSAGE);
        assert(solar_os_chat_outbox_ack(command.id) == ESP_OK);
    }
    assert(solar_os_chat_get_config(&config) == ESP_OK && !strcmp(config.token, "token-b"));
    solar_os_gateway_scope_leave(scope);
    fail_migration_promotion = true;
    assert(solar_os_messaging_instance_remove(a) != ESP_OK);
    assert(solar_os_gateway_instance_get_status(a, &status) == ESP_OK);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK);
    assert(solar_os_messaging_conversation_get(room_a, &(solar_os_messaging_conversation_t){0}) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_conversation_get(room_b, &(solar_os_messaging_conversation_t){0}) == ESP_OK);
    reboot_gateway();
    assert(solar_os_gateway_instance_get_status(a, &status) == ESP_ERR_NOT_FOUND);
    assert(solar_os_gateway_instance_get_status(b, &status) == ESP_OK && status.enabled);
    scope = solar_os_gateway_scope_enter(gateway_find(b));
    assert(solar_os_chat_get_config(&config) == ESP_OK && !strcmp(config.token, "token-b"));
    solar_os_gateway_scope_leave(scope);
    /* Form readback never returns secrets. Editing another field preserves the
     * token, failed writes preserve it too, and an explicit empty edit clears. */
    solar_os_messaging_settings_t form;
    assert(solar_os_messaging_settings_read("gateway", b, 0, &form) == ESP_OK);
    assert(form.count == 2 && form.enabled && form.present[1] && !form.values[1][0]);
    assert(!strcmp(form.values[0], settings_b[0]) && !strcmp(form.worker, "gateway-sync"));
    assert(solar_os_messaging_settings_read("link", b, 0, &form) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_settings_read("gateway", b, 0, &form) == ESP_OK);
    form.changed[0] = true; strlcpy(form.values[0], "chats://edited.example:7443", sizeof(form.values[0]));
    solar_os_messaging_provider_id_t result = 0;
    assert(solar_os_messaging_settings_write("gateway", b, 0, NULL, &form, &result) == ESP_OK && result == b);
    scope = solar_os_gateway_scope_enter(gateway_find(b));
    assert(solar_os_chat_get_config(&config) == ESP_OK && !strcmp(config.token, "token-b") && config.enabled);
    solar_os_gateway_scope_leave(scope);
    assert(solar_os_messaging_settings_read("gateway", b, 0, &form) == ESP_OK);
    form.changed[1] = true; strlcpy(form.values[1], "replacement", sizeof(form.values[1]));
    fail_migration_promotion = true;
    assert(solar_os_messaging_settings_write("gateway", b, 0, NULL, &form, &result) != ESP_OK);
    scope = solar_os_gateway_scope_enter(gateway_find(b));
    assert(solar_os_chat_get_config(&config) == ESP_OK && !strcmp(config.token, "token-b"));
    solar_os_gateway_scope_leave(scope);
    memset(form.values[1], 0, sizeof(form.values[1]));
    assert(solar_os_messaging_settings_write("gateway", b, 0, NULL, &form, &result) == ESP_OK);
    assert(solar_os_messaging_settings_read("gateway", b, 0, &form) == ESP_OK && !form.present[1]);
    memset(form.values[0], 'x', sizeof(form.values[0]));
    assert(solar_os_messaging_settings_write("gateway", b, 0, NULL, &form, &result) == ESP_ERR_INVALID_SIZE);
    solar_os_messaging_settings_clear(&form);
    assert(solar_os_messaging_instance_remove(b) == ESP_OK);
    assert(solar_os_messaging_instance_remove(SOLAR_OS_MESSAGING_PROVIDER_GATEWAY) == ESP_OK);
    reboot_gateway();
    assert(solar_os_chat_get_config(&config) == ESP_ERR_NOT_FOUND);
    assert(!solar_os_messaging_provider_find("gateway"));
    char path[160];
    const char *files[] = {".chat/profiles/001.bin", ".chat/profiles/004.bin", ".chat/profiles/005.bin", ".messages/history.bin", ".messages/location"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        assert(solar_os_storage_default_path(files[i], path, sizeof(path)) == ESP_OK);
        assert(unlink(path) == 0);
    }
    const char *directories[] = {".chat/profiles", ".chat", ".messages"};
    for (size_t i = 0; i < 3; i++) {
        assert(solar_os_storage_default_path(directories[i], path, sizeof(path)) == ESP_OK);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("gateway config migration tests: ok");
    return 0;
}
