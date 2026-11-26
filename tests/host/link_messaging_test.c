/* Real Messaging/Contacts/profile persistence with two independent transports. */
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#define SOLAR_OS_PACKAGE_SERVICE_MESSAGING 1
#define TAG link_test_tag
#include "../../src/services/solar_os_link_messaging.c"
#undef TAG

#include "solar_os_native_link_messaging.h"
#include "solar_os_native_link_messaging_abi.h"

static unsigned sends_a, sends_b;
uint32_t esp_random(void) { static uint32_t next; return ++next; }
esp_err_t solar_os_link_get_status(const char *name, solar_os_link_status_t *status)
{
    if (strcmp(name, "a") && strcmp(name, "b")) return ESP_ERR_NOT_FOUND;
    memset(status, 0, sizeof(*status)); status->frame_mtu = 256; status->local_id = 1;
    return ESP_OK;
}
esp_err_t solar_os_link_send(const char *name, solar_os_link_message_type_t type, uint32_t destination,
    const void *payload, size_t length, uint16_t *sequence)
{
    assert(type == SOLAR_OS_LINK_MESSAGE_TEXT && destination == 0x1234 && length == 1);
    if (!strcmp(name, "a")) { assert(*(const char *)payload == 'A'); sends_a++; }
    else { assert(!strcmp(name, "b") && *(const char *)payload == 'B'); sends_b++; }
    *sequence = 1; return ESP_OK;
}
esp_err_t solar_os_link_decode(const uint8_t *frame, size_t length, solar_os_link_message_t *message)
{
    (void)frame; (void)length;
    memset(message, 0, sizeof(*message)); message->type = SOLAR_OS_LINK_MESSAGE_TEXT;
    message->sequence = 1; message->destination = 0x1234; return ESP_OK;
}

int main(void)
{
    const solar_os_native_link_messaging_api_v1_t *api = solar_os_native_link_messaging_get_service(
        SOLAR_OS_NATIVE_LINK_MESSAGING_SERVICE, SOLAR_OS_NATIVE_LINK_MESSAGING_ABI, sizeof(*api));
    assert(api != NULL && api->struct_size == sizeof(*api));
    assert(!solar_os_native_link_messaging_get_service(
        SOLAR_OS_NATIVE_LINK_MESSAGING_SERVICE, SOLAR_OS_NATIVE_LINK_MESSAGING_ABI + 1, sizeof(*api)));
    assert(mkdtemp(test_root));
    assert(solar_os_link_messaging_init() == ESP_OK);
    assert(api->start("a") == SOLAR_OS_NATIVE_OK);
    assert(api->start("b") == SOLAR_OS_NATIVE_OK);
    assert(api->start("a") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_messaging_provider_id_t a = find_link("a")->provider, b = find_link("b")->provider;
    assert(a == SOLAR_OS_MESSAGING_PROVIDER_LINK && a != b);
    solar_os_messaging_settings_t form;
    assert(solar_os_messaging_settings_read("link", a, 0, &form) == ESP_OK);
    assert(form.enabled && form.data.count == 1 && !strcmp(form.data.values[0], "a") && form.worker_is_resource);
    solar_os_messaging_provider_id_t saved;
    assert(solar_os_messaging_settings_write("link", a, 0, NULL, &form, &saved) == ESP_OK && saved == a);
    form.data.changed[0] = true; strlcpy(form.data.values[0], "b", sizeof(form.data.values[0]));
    assert(solar_os_messaging_settings_write("link", a, 0, NULL, &form, &saved) == ESP_ERR_INVALID_STATE);
    solar_os_messaging_provider_id_t duplicate;
    const char *binding[] = {"b"};
    assert(solar_os_messaging_instance_add("link", "duplicate", 1, binding, &duplicate) == ESP_ERR_INVALID_STATE);
    assert(solar_os_messaging_instance_configure(a, 1, binding) == ESP_ERR_INVALID_STATE);
    solar_os_conversation_id_t room_a, room_b;
    assert(solar_os_messaging_join(a, "broadcast", &room_a) == ESP_OK);
    assert(solar_os_messaging_join(b, "broadcast", &room_b) == ESP_OK && room_a != room_b);
    solar_os_messaging_conversation_t broadcast;
    assert(solar_os_messaging_conversation_get(room_a, &broadcast) == ESP_OK);
    assert(broadcast.kind == SOLAR_OS_CONVERSATION_BROADCAST &&
        broadcast.group_ref == SOLAR_OS_LINK_BROADCAST &&
        broadcast.join_state == SOLAR_OS_MESSAGING_JOIN_JOINED &&
        !strcmp(broadcast.title, "a broadcast"));
    solar_os_conversation_id_t direct_a, direct_b;
    assert(solar_os_messaging_open_direct(a, "1234", &direct_a) == ESP_OK);
    assert(solar_os_messaging_open_direct(b, "1234", &direct_b) == ESP_OK && direct_a != direct_b);
    solar_os_message_key_t key_a, key_b;
    assert(solar_os_messaging_send(direct_a, "A", true, &key_a) == ESP_OK);
    assert(solar_os_messaging_send(direct_b, "B", true, &key_b) == ESP_OK);
    api->process("a", 100);
    api->process("b", 100);
    assert(sends_a == 1 && sends_b == 1);
    solar_os_link_frame_t frame = {0};
    api->note_transmit("a", &frame, SOLAR_OS_NATIVE_OK, 101);
    api->note_transmit("b", &frame, SOLAR_OS_NATIVE_OK, 101);
    solar_os_link_ingest_result_t ack = {.acknowledgement = true,
        .message = {.source = 0x1234, .sequence = 1}};
    assert(api->note_ingest("b", &ack, 102) == SOLAR_OS_NATIVE_OK);
    solar_os_messaging_message_t message;
    assert(solar_os_messaging_message_get(key_b, &message) == ESP_OK && message.delivery == SOLAR_OS_DELIVERY_DELIVERED);
    assert(solar_os_messaging_message_get(key_a, &message) == ESP_OK && message.delivery == SOLAR_OS_DELIVERY_SENDING);
    fail_migration_promotion = true;
    assert(solar_os_messaging_instance_remove(a) != ESP_OK && find_link("a")->inflight);
    assert(solar_os_messaging_instance_remove(a) == ESP_OK && !find_link("a"));
    assert(solar_os_messaging_message_get(key_a, &message) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(key_b, &message) == ESP_OK);
    /* Reboot restores B's binding and desired state, and never revives A. */
    while (instances) { link_messaging_state_t *next = instances->next; free(instances); instances = next; }
    vSemaphoreDelete(instances_lock); instances_lock = NULL; initialized = false;
    restart_messages();
    assert(solar_os_link_messaging_init() == ESP_OK);
    assert(!find_link("a") && find_link("b")->provider == b && find_link("b")->config.enabled);
    assert(api->start("b") == SOLAR_OS_NATIVE_OK);
    api->stop("b");
    assert(!find_link("b")->active);
    assert(solar_os_messaging_instance_remove(b) == ESP_OK);
    const char *files[] = {".chat/profiles/003.bin", ".chat/profiles/004.bin", ".contacts/contacts.bin", ".messages/history.bin", ".messages/location"};
    char path[160];
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        assert(solar_os_storage_default_path(files[i], path, sizeof(path)) == ESP_OK); assert(unlink(path) == 0);
    }
    const char *dirs[] = {".chat/profiles", ".chat", ".contacts", ".messages"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        assert(solar_os_storage_default_path(dirs[i], path, sizeof(path)) == ESP_OK); assert(rmdir(path) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("Link messaging instance isolation tests: ok");
    return 0;
}
