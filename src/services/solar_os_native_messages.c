#include "solar_os_native_messages.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_messages_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_MESSAGING
#include "solar_os_memory.h"
#include "solar_os_messaging.h"

static int messages_status(solar_os_messaging_status_t *out)
{
    esp_err_t error = solar_os_messaging_get_status(out);
    if (error == ESP_OK) out->storage_error = solar_os_native_result(out->storage_error);
    return solar_os_native_result(error);
}

static bool messages_provider_next(solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *out)
{
    if (!solar_os_messaging_provider_next(cursor, out)) return false;
    out->last_error = solar_os_native_result(out->last_error);
    return true;
}

static int messages_provider_info(solar_os_messaging_provider_id_t provider,
    solar_os_messaging_provider_status_t *out)
{
    esp_err_t error = solar_os_messaging_provider_get_status(provider, out);
    if (error == ESP_OK) out->last_error = solar_os_native_result(out->last_error);
    return solar_os_native_result(error);
}

static int messages_event_next(uint32_t *cursor, solar_os_messaging_event_t *out)
{
    esp_err_t error = solar_os_messaging_read_event_after(cursor, out);
    if (error == ESP_OK) out->error = solar_os_native_result(out->error);
    return solar_os_native_result(error);
}

static int messages_provider_add(const char *type, const char *name, int argc,
    const char *const *argv, solar_os_messaging_provider_id_t *id)
{
    return solar_os_native_result(solar_os_messaging_instance_add(type, name, argc, argv, id));
}

static int messages_provider_configure(solar_os_messaging_provider_id_t id, int argc, const char *const *argv)
{
    return solar_os_native_result(solar_os_messaging_instance_configure(id, argc, argv));
}

static int messages_provider_remove(solar_os_messaging_provider_id_t provider)
{
    return solar_os_native_result(solar_os_messaging_instance_remove(provider));
}

static int messages_connect(solar_os_messaging_provider_id_t provider, bool enabled)
{
    return solar_os_native_result(solar_os_messaging_connect(provider, enabled));
}

static int messages_join(solar_os_messaging_provider_id_t provider, const char *target,
    solar_os_conversation_id_t *conversation)
{
    return solar_os_native_result(solar_os_messaging_join(provider, target, conversation));
}

static int messages_leave(solar_os_conversation_id_t conversation)
{
    return solar_os_native_result(solar_os_messaging_leave(conversation));
}

static int messages_open_direct(solar_os_messaging_provider_id_t provider, const char *target,
    solar_os_conversation_id_t *conversation)
{
    return solar_os_native_result(solar_os_messaging_open_direct(provider, target, conversation));
}

static int messages_open_contact(solar_os_contact_id_t contact, solar_os_conversation_id_t *conversation)
{
    return solar_os_native_result(solar_os_messaging_direct_open(contact, conversation));
}

static int messages_conversation_get(solar_os_conversation_id_t conversation, solar_os_messaging_conversation_t *out)
{
    return solar_os_native_result(solar_os_messaging_conversation_get(conversation, out));
}

static int messages_mark_read(solar_os_conversation_id_t conversation)
{
    return solar_os_native_result(solar_os_messaging_mark_read(conversation));
}

static int messages_page(solar_os_conversation_id_t conversation,
    solar_os_messaging_provider_id_t provider, bool unread_only, size_t offset,
    solar_os_messaging_message_t *out, size_t capacity, size_t *count, bool *more)
{
    return solar_os_native_result(solar_os_messaging_message_page_filtered(
        conversation, provider, unread_only, offset, out, capacity, count, more));
}

static int messages_message_get(solar_os_message_key_t key, solar_os_messaging_message_t *out)
{
    return solar_os_native_result(solar_os_messaging_message_get(key, out));
}

static int messages_message_delete(solar_os_message_key_t key)
{
    return solar_os_native_result(solar_os_messaging_message_delete(key));
}

static int messages_clear(solar_os_messaging_provider_id_t provider, size_t *removed)
{
    return solar_os_native_result(solar_os_messaging_clear(provider, removed));
}

static int messages_send(solar_os_conversation_id_t conversation, const char *body,
    bool untrusted, solar_os_message_key_t *key)
{
    return solar_os_native_result(solar_os_messaging_send(conversation, body, untrusted, key));
}

static int messages_cancel(solar_os_message_key_t key)
{
    return solar_os_native_result(solar_os_messaging_cancel(key));
}

static solar_os_messaging_settings_t *messages_settings_allocate(void)
{
    return solar_os_memory_calloc(1, sizeof(solar_os_messaging_settings_t),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "messages.native.settings");
}

static int messages_settings_read(const char *class_name, solar_os_messaging_provider_id_t provider,
    uint32_t form, solar_os_message_settings_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    solar_os_messaging_settings_t *settings = messages_settings_allocate();
    if (!settings) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    esp_err_t error = solar_os_messaging_settings_read(class_name, provider, form, settings);
    if (error == ESP_OK) {
        if (settings->label) strlcpy(out->label, settings->label, sizeof(out->label));
        strlcpy(out->worker, settings->worker, sizeof(out->worker));
        out->enabled = settings->enabled;
        out->worker_is_resource = settings->worker_is_resource;
        out->data.count = settings->count;
        for (size_t i = 0; i < settings->count; i++) {
            const solar_os_messaging_setting_t *field = &settings->fields[i];
            solar_os_message_setting_v1_t *view = &out->fields[i];
            strlcpy(view->label, field->label, sizeof(view->label));
            if (field->hint) strlcpy(view->hint, field->hint, sizeof(view->hint));
            view->capacity = field->capacity;
            view->secret = field->secret;
            view->required = field->required;
            out->data.present[i] = settings->present[i];
            if (!field->secret)
                strlcpy(out->data.values[i], settings->values[i], sizeof(out->data.values[i]));
        }
    }
    solar_os_messaging_settings_clear(settings);
    solar_os_memory_free(settings);
    return solar_os_native_result(error);
}

static int messages_settings_write(const char *class_name, solar_os_messaging_provider_id_t provider,
    uint32_t form, const char *name, const solar_os_message_setting_values_v1_t *in,
    solar_os_messaging_provider_id_t *result)
{
    if (!result) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *result = 0;
    if (!in || !in->count || in->count > SOLAR_OS_MESSAGING_SETTINGS_FIELDS)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_messaging_settings_t *settings = messages_settings_allocate();
    if (!settings) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    settings->count = in->count;
    memcpy(settings->values, in->values, sizeof(settings->values));
    memcpy(settings->present, in->present, sizeof(settings->present));
    memcpy(settings->changed, in->changed, sizeof(settings->changed));
    esp_err_t error = solar_os_messaging_settings_write(class_name, provider, form, name, settings, result);
    solar_os_messaging_settings_clear(settings);
    solar_os_memory_free(settings);
    return solar_os_native_result(error);
}

static int messages_provider_claim(solar_os_messaging_provider_id_t provider, const char *detail)
{
    return solar_os_native_result(solar_os_messaging_provider_claim(provider, detail));
}

static int messages_provider_set_status(solar_os_messaging_provider_id_t provider,
    bool running, bool connected, int error, const char *detail)
{
    return solar_os_native_result(solar_os_messaging_provider_set_status(
        provider, running, connected, solar_os_native_callback_result(error), detail));
}

static int messages_publish_inbound(const solar_os_messaging_inbound_t *entry,
    bool *inserted, solar_os_message_key_t *key)
{
    return solar_os_native_result(solar_os_messaging_publish_inbound(entry, inserted, key));
}

static int messages_conversation_upsert(const solar_os_messaging_conversation_upsert_t *entry,
    solar_os_conversation_id_t *id)
{
    return solar_os_native_result(solar_os_messaging_conversation_upsert(entry, id));
}

static int messages_conversation_remove(solar_os_messaging_provider_id_t provider, const char *key)
{
    return solar_os_native_result(solar_os_messaging_conversation_remove(provider, key));
}

static int messages_groups_begin_sync(solar_os_messaging_provider_id_t provider, const char *prefix)
{
    return solar_os_native_result(solar_os_messaging_groups_begin_sync(provider, prefix));
}

static int messages_join_update(solar_os_messaging_provider_id_t provider, const char *target,
    solar_os_messaging_join_state_t state, const char *detail)
{
    return solar_os_native_result(solar_os_messaging_join_update(provider, target, state, detail));
}

static int messages_outbox_peek(solar_os_messaging_provider_id_t provider, solar_os_messaging_outbound_t *entry)
{
    return solar_os_native_result(solar_os_messaging_outbox_peek(provider, entry));
}

static int messages_outbox_update(uint32_t request_id, solar_os_delivery_state_t state, const char *detail)
{
    return solar_os_native_result(solar_os_messaging_outbox_update(request_id, state, detail));
}

static const solar_os_native_messages_api_v1_t messages_api = {
    .abi_version = SOLAR_OS_NATIVE_MESSAGES_ABI,
    .struct_size = sizeof(messages_api),
    .status = messages_status,
    .provider_next = messages_provider_next,
    .provider_info = messages_provider_info,
    .provider_find = solar_os_messaging_provider_find,
    .provider_is_class = solar_os_messaging_provider_is_class,
    .class_next = solar_os_messaging_class_next,
    .provider_add = messages_provider_add,
    .provider_configure = messages_provider_configure,
    .provider_remove = messages_provider_remove,
    .connect = messages_connect,
    .join = messages_join,
    .leave = messages_leave,
    .room_left = solar_os_messaging_room_left,
    .open_direct = messages_open_direct,
    .open_contact = messages_open_contact,
    .conversation_snapshot = solar_os_messaging_conversation_snapshot,
    .conversation_get = messages_conversation_get,
    .conversation_label = solar_os_messaging_conversation_label,
    .mark_read = messages_mark_read,
    .message_page = messages_page,
    .message_visit = solar_os_messaging_message_visit_consistent,
    .message_get = messages_message_get,
    .message_delete = messages_message_delete,
    .clear = messages_clear,
    .send = messages_send,
    .cancel = messages_cancel,
    .event_next = messages_event_next,
    .outbox_snapshot = solar_os_messaging_outbox_snapshot,
    .provider_name = solar_os_messaging_provider_name,
    .contact_trust_name = solar_os_contact_trust_name,
    .conversation_kind_name = solar_os_conversation_kind_name,
    .delivery_name = solar_os_delivery_state_name,
    .settings_read = messages_settings_read,
    .settings_write = messages_settings_write,
    .provider_claim = messages_provider_claim,
    .provider_set_status = messages_provider_set_status,
    .provider_context = solar_os_messaging_provider_context,
    .provider_cursor = solar_os_messaging_provider_cursor_masked,
    .publish_inbound = messages_publish_inbound,
    .conversation_upsert = messages_conversation_upsert,
    .conversation_remove = messages_conversation_remove,
    .groups_begin_sync = messages_groups_begin_sync,
    .join_update = messages_join_update,
    .outbox_peek = messages_outbox_peek,
    .outbox_update = messages_outbox_update,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_CREDENTIALS
#include "solar_os_credentials.h"

static int credentials_status(solar_os_credentials_status_t *out)
{
    const int result = solar_os_native_result(solar_os_credentials_get_status(out));
    if (!result) out->storage_error = solar_os_native_result(out->storage_error);
    return result;
}

static int credentials_find(solar_os_messaging_provider_id_t provider, solar_os_credential_kind_t kind,
    const char *label, solar_os_credential_info_t *record)
{ return solar_os_native_result(solar_os_credentials_find(provider, kind, label, record)); }

static int credentials_put(solar_os_messaging_provider_id_t provider, solar_os_credential_kind_t kind,
    const char *label, const void *secret, size_t size, bool replace, solar_os_credential_id_t *id)
{ return solar_os_native_result(solar_os_credentials_put(provider, kind, label, secret, size, replace, id)); }

static int credentials_read(solar_os_credential_id_t id, void *secret, size_t capacity, size_t *size)
{ return solar_os_native_result(solar_os_credentials_read_secret(id, secret, capacity, size)); }
static int credentials_remove(solar_os_credential_id_t id)
{ return solar_os_native_result(solar_os_credentials_remove(id)); }
static int credentials_remove_provider(solar_os_messaging_provider_id_t id)
{ return solar_os_native_result(solar_os_credentials_remove_provider(id)); }

static const solar_os_native_credentials_api_v1_t credentials_api = {
    .abi_version = SOLAR_OS_NATIVE_CREDENTIALS_ABI, .struct_size = sizeof(credentials_api),
    .status = credentials_status, .snapshot = solar_os_credentials_snapshot, .find = credentials_find,
    .put = credentials_put, .read_secret = credentials_read, .remove = credentials_remove,
    .remove_provider = credentials_remove_provider, .wipe = solar_os_credentials_wipe,
    .kind_name = solar_os_credential_kind_name,
};
#endif

const void *solar_os_native_messages_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_messages_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
