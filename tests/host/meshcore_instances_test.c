/* Real class registry/profiles/managers; deterministic backend resource owners. */
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#include "../../src/services/solar_os_meshcore_instances.c"

static bool legacy_name = true, legacy_public = true;
static unsigned nvs_writes, stops, destroyed, polls[256];
static solar_os_messaging_provider_id_t resource_busy;
esp_err_t nvs_open(const char *space, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(space, "meshcore"));
    if (!legacy_name && !legacy_public) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) nvs_writes++;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    (void)handle; assert(!strcmp(key, "name"));
    if (!legacy_name) return ESP_ERR_NVS_NOT_FOUND;
    if (value) { assert(*size >= 7); memcpy(value, "legacy", 7); }
    *size = 7; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ (void)handle; assert(!strcmp(key, "public")); if (!legacy_public) return ESP_ERR_NVS_NOT_FOUND; *value = 0; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ (void)handle; if (!strcmp(key, "name")) legacy_name = false; if (!strcmp(key, "public")) legacy_public = false; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

typedef struct { solar_os_messaging_provider_id_t owner; unsigned kind; bool running; } fake_state_t;
static esp_err_t fake_start(const solar_os_meshcore_instance_config_t *config, const char *owner)
{
    assert(owner && owner[0]);
    fake_state_t *state = solar_os_meshcore_current_state(config->backend, sizeof(*state)); assert(state);
    assert(!state->running); state->owner = solar_os_meshcore_current_id(); state->kind = config->backend; state->running = true;
    return solar_os_messaging_provider_set_status(state->owner, true, true, ESP_OK, "active");
}
static void fake_poll(void)
{
    solar_os_meshcore_instance_config_t config; assert(solar_os_meshcore_current_config(&config) == ESP_OK);
    fake_state_t *state = solar_os_meshcore_current_state(config.backend, 0);
    assert(state && state->running && state->owner == solar_os_meshcore_current_id()); polls[state->owner]++;
}
static void fake_stop(void)
{
    solar_os_meshcore_instance_config_t config; assert(solar_os_meshcore_current_config(&config) == ESP_OK);
    fake_state_t *state = solar_os_meshcore_current_state(config.backend, 0);
    if (state && state->running) { stops++; state->running = false; }
    (void)solar_os_messaging_provider_set_status(solar_os_meshcore_current_id(), false, false, ESP_OK, "stopped");
}
static void fake_destroy(void)
{
    solar_os_meshcore_instance_config_t config; assert(solar_os_meshcore_current_config(&config) == ESP_OK);
    fake_stop(); solar_os_meshcore_current_state_free(config.backend); destroyed++;
}
static esp_err_t fake_join(const char *target, solar_os_conversation_id_t *id)
{
    solar_os_messaging_conversation_upsert_t group = {.provider = solar_os_meshcore_current_id(),
        .provider_key = target, .title = target, .kind = SOLAR_OS_CONVERSATION_GROUP, .group_ref = 1};
    esp_err_t error = solar_os_messaging_conversation_upsert(&group, id);
    if (error == ESP_OK) (void)solar_os_messaging_join_update(group.provider, target, SOLAR_OS_MESSAGING_JOIN_JOINED, NULL);
    return error;
}
static esp_err_t fake_remove_prepare(void)
{ return resource_busy == solar_os_meshcore_current_id() ? ESP_ERR_INVALID_STATE : ESP_OK; }
static const solar_os_meshcore_backend_t fake_ops = {
    .start = fake_start, .poll = fake_poll, .stop = fake_stop, .destroy = fake_destroy, .join = fake_join,
    .remove_prepare = fake_remove_prepare,
};
static void install(void)
{
    solar_os_meshcore_backend_register(1, &fake_ops); solar_os_meshcore_backend_register(2, &fake_ops);
}
static void assert_legacy_instance(unsigned kind, solar_os_messaging_provider_id_t id)
{
    void *lease, *nested;
    assert(solar_os_meshcore_legacy_begin(kind, &lease) == ESP_OK && lease);
    assert(solar_os_meshcore_current_id() == id);
    assert(solar_os_meshcore_legacy_begin(kind, &nested) == ESP_OK && !nested);
    solar_os_meshcore_legacy_end(nested);
    assert(solar_os_meshcore_current_id() == id);
    solar_os_meshcore_legacy_end(lease);
}
static void reboot_instances(void)
{
    for (meshcore_instance_t *node = meshcore_instances.instances; node;) {
        selected_instance = node;
        for (unsigned kind = 1; kind < 3; kind++) free(node->state[kind]);
        vSemaphoreDelete(node->config_lock); meshcore_instance_t *next = node->next; free(node); node = next;
    }
    selected_instance = NULL;
    vSemaphoreDelete(meshcore_instances.edit);
    for (unsigned kind = 1; kind < 3; kind++) vSemaphoreDelete(meshcore_instances.workers[kind]);
    memset(&meshcore_instances, 0, sizeof(meshcore_instances)); atomic_store(&ready, false);
    restart_messages(); install(); assert(solar_os_meshcore_instances_init() == ESP_OK);
}

int main(void)
{
    assert(mkdtemp(test_root)); install();
    fail_migration_promotion = true;
    assert(solar_os_meshcore_instances_init() == ESP_OK && legacy_name && legacy_public && !nvs_writes);
    reboot_instances(); assert(!legacy_name && !legacy_public && nvs_writes == 1);
    void *lease; assert(solar_os_meshcore_legacy_begin(1, &lease) == ESP_OK);
    solar_os_meshcore_instance_config_t config;
    assert(solar_os_meshcore_current_config(&config) == ESP_OK && !strcmp(config.name, "legacy") && !config.public_channel);
    void *nested; assert(solar_os_meshcore_legacy_begin(1, &nested) == ESP_OK && !nested);
    solar_os_meshcore_legacy_end(nested); solar_os_meshcore_legacy_end(lease);
    assert_legacy_instance(1, SOLAR_OS_MESSAGING_PROVIDER_MESHCORE);
    assert_legacy_instance(2, SOLAR_OS_MESSAGING_PROVIDER_MESHCORE);
    const char *legacy_args[] = {"radio0", "profile-a"};
    assert(solar_os_meshcore_instances_bind(1, 2, legacy_args) == ESP_OK);
    solar_os_messaging_provider_id_t a = 2, b, companion;
    const char *radio_args[] = {"radio", "radio1", "profile-b"};
    const char *ble_args[] = {"ble", "01:02:03:04:05:06", "random", "123456"};
    assert(solar_os_messaging_instance_add("meshcore", "radio-b", 3, radio_args, &b) == ESP_OK);
    assert(solar_os_messaging_instance_add("meshcore", "companion", 4, ble_args, &companion) == ESP_OK);
    solar_os_messaging_settings_t form;
    assert(solar_os_messaging_settings_read("meshcore", 0, 0, &form) == ESP_OK && form.count == 4);
    assert(!strcmp(form.label, "MeshCore radio"));
    assert(solar_os_messaging_settings_read("meshcore", 0, 1, &form) == ESP_OK && form.count == 5);
    assert(!strcmp(form.label, "MeshCore BLE"));
    assert(solar_os_messaging_settings_read("meshcore", companion, 0, &form) == ESP_OK);
    assert(form.present[2] && !form.values[2][0] && !strcmp(form.values[0], ble_args[1]));
    assert(!strcmp(form.worker, "meshcore-ble"));
    form.changed[3] = true; strlcpy(form.values[3], "edited", sizeof(form.values[3]));
    solar_os_messaging_provider_id_t saved;
    assert(solar_os_messaging_settings_write("meshcore", companion, 0, NULL, &form, &saved) == ESP_OK && saved == companion);
    assert(solar_os_messaging_profile_load(companion, &config, sizeof(config)) == ESP_OK);
    assert(config.pin_supplied && config.pin == 123456 && !strcmp(config.name, "edited") && !config.enabled);
    form.changed[2] = true; /* Explicit empty edit clears a saved PIN. */
    assert(solar_os_messaging_settings_write("meshcore", companion, 0, NULL, &form, &saved) == ESP_OK);
    assert(solar_os_messaging_profile_load(companion, &config, sizeof(config)) == ESP_OK && !config.pin_supplied);
    form.changed[2] = false; form.changed[4] = true;
    strlcpy(form.values[4], "invalid", sizeof(form.values[4]));
    assert(solar_os_messaging_settings_write("meshcore", companion, 0, NULL, &form, &saved) == ESP_ERR_INVALID_SIZE);
    solar_os_messaging_settings_clear(&form);
    assert_legacy_instance(1, a);
    assert_legacy_instance(2, companion);
    solar_os_messaging_provider_id_t duplicate;
    assert(solar_os_messaging_instance_add("meshcore", "duplicate", 3, radio_args, &duplicate) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_instance_add("meshcore", "duplicate", 4, ble_args, &duplicate) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_instance_configure(a, 3, radio_args) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_connect(b, true) == ESP_OK && solar_os_messaging_connect(companion, true) == ESP_OK);
    solar_os_meshcore_instances_poll(1); solar_os_meshcore_instances_poll(2);
    assert(polls[a] == 1 && polls[b] == 1 && polls[companion] == 1);
    solar_os_conversation_id_t room_a, room_b;
    unsigned previous_stops = stops;
    assert(solar_os_messaging_join(a, "same-group", &room_a) == ESP_OK);
    assert(solar_os_messaging_join(b, "same-group", &room_b) == ESP_OK && room_a != room_b);
    assert(solar_os_messaging_join(a, "same-group", &room_a) == ESP_OK && stops == previous_stops);
    const char *peer_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    solar_os_conversation_id_t direct_a, direct_b;
    assert(solar_os_messaging_open_direct(a, peer_key, &direct_a) == ESP_OK);
    assert(solar_os_messaging_open_direct(b, peer_key, &direct_b) == ESP_OK && direct_a != direct_b);
    solar_os_messaging_conversation_t peer_a, peer_b;
    assert(solar_os_messaging_conversation_get(direct_a, &peer_a) == ESP_OK);
    assert(solar_os_messaging_conversation_get(direct_b, &peer_b) == ESP_OK);
    assert(solar_os_contacts_link(peer_a.contact_id, peer_b.contact_id) == ESP_OK);
    solar_os_messaging_inbound_t retained = {.provider = b, .conversation_key = "group:99", .conversation_title = "field",
        .conversation_kind = SOLAR_OS_CONVERSATION_GROUP, .group_ref = 99, .provider_message_key = 99, .sender = "peer", .body = "retained"};
    solar_os_message_key_t retained_key;
    assert(solar_os_messaging_publish_inbound(&retained, NULL, &retained_key) == ESP_OK);
    solar_os_messaging_message_t retained_message;
    assert(solar_os_messaging_message_get(retained_key, &retained_message) == ESP_OK);
    solar_os_conversation_id_t retained_room = retained_message.conversation_id;
    solar_os_messaging_conversation_t retained_conversation;
    assert(solar_os_messaging_conversation_get(retained_room, &retained_conversation) == ESP_OK);
    char label[96]; solar_os_messaging_conversation_label(&retained_conversation, label, sizeof(label));
    assert(strstr(label, "[radio]"));
    solar_os_meshcore_instances_poll(1);
    void *pin;
    assert(solar_os_messaging_instance_retain(a, &pin) == ESP_OK);
    assert(solar_os_messaging_instance_remove(a) == ESP_ERR_INVALID_STATE);
    solar_os_messaging_instance_release(pin);
    resource_busy = a;
    assert(solar_os_messaging_instance_remove(a) == ESP_ERR_INVALID_STATE && destroyed == 0);
    resource_busy = 0;
    fail_migration_promotion = true;
    assert(solar_os_messaging_instance_remove(a) != ESP_OK && destroyed == 0);
    solar_os_meshcore_instances_poll(1); assert(polls[a] == 3 && polls[b] == 3);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK && destroyed == 1);
    assert_legacy_instance(1, b);
    assert_legacy_instance(2, companion);
    solar_os_conversation_id_t selected_peer;
    assert(solar_os_messaging_direct_open(peer_a.contact_id, &selected_peer) == ESP_OK);
    assert(solar_os_messaging_conversation_get(selected_peer, &peer_b) == ESP_OK && peer_b.provider == b);
    assert(solar_os_messaging_conversation_get(room_a, &(solar_os_messaging_conversation_t){0}) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_conversation_get(room_b, &(solar_os_messaging_conversation_t){0}) == ESP_OK);
    solar_os_meshcore_instances_poll(1); solar_os_meshcore_instances_poll(2);
    assert(polls[a] == 3 && polls[b] == 4 && polls[companion] == 2);
    reboot_instances(); assert(nvs_writes == 1);
    assert(solar_os_messaging_message_get(retained_key, &retained_message) == ESP_OK);
    assert(solar_os_messaging_conversation_get(retained_message.conversation_id, &retained_conversation) == ESP_OK && retained_conversation.history_only);
    assert_legacy_instance(1, b);
    assert_legacy_instance(2, companion);
    assert(solar_os_messaging_provider_find("radio-b") == b && solar_os_messaging_provider_find("companion") == companion);
    solar_os_meshcore_instances_poll(1); solar_os_meshcore_instances_poll(2);
    assert(polls[b] == 5 && polls[companion] == 3);
    /* A stopped binding still counts: start/stop must not redirect legacy
     * identity/channel writes between two configured radios. */
    solar_os_messaging_provider_id_t extra;
    const char *extra_args[] = {"radio", "radio2", "profile-c"};
    assert(solar_os_messaging_instance_add("meshcore", "extra", 3, extra_args, &extra) == ESP_OK);
    assert(solar_os_meshcore_legacy_begin(1, &lease) == ESP_ERR_INVALID_STATE && !lease);
    assert_legacy_instance(2, companion);
    assert(solar_os_messaging_instance_remove(extra) == ESP_OK);
    assert_legacy_instance(1, b);
    assert(solar_os_messaging_instance_remove(b) == ESP_OK);
    assert(solar_os_meshcore_legacy_begin(1, &lease) == ESP_ERR_NOT_FOUND && !lease);
    assert_legacy_instance(2, companion);
    assert(solar_os_messaging_instance_remove(companion) == ESP_OK);
    assert(solar_os_meshcore_legacy_begin(2, &lease) == ESP_ERR_NOT_FOUND && !lease);
    assert(solar_os_meshcore_legacy_begin(0, &lease) == ESP_ERR_INVALID_ARG && !lease);
    assert(solar_os_meshcore_legacy_begin(1, NULL) == ESP_ERR_INVALID_ARG);
    char extra_file[64]; snprintf(extra_file, sizeof(extra_file), ".chat/profiles/%03" PRIu32 ".bin", extra);
    const char *files[] = {".chat/profiles/002.bin", ".chat/profiles/004.bin", ".chat/profiles/005.bin", extra_file, ".messages/history.bin", ".messages/location", ".contacts/contacts.bin"};
    char path[160];
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        assert(solar_os_storage_default_path(files[i], path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    }
    const char *dirs[] = {".chat/profiles", ".chat", ".messages", ".contacts"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        assert(solar_os_storage_default_path(dirs[i], path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("MeshCore instance lifecycle and binding tests: ok");
    return 0;
}
