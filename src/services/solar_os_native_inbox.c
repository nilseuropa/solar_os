#include "solar_os_native_inbox.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_inbox_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_INBOX
#include "solar_os_inbox.h"

static int inbox_status(solar_os_inbox_status_t *status)
{
    esp_err_t error = solar_os_inbox_get_status(status);
    if (error == ESP_OK) status->storage_error = solar_os_native_result(status->storage_error);
    return solar_os_native_result(error);
}

static int inbox_publish(const solar_os_inbox_publish_t *message, uint32_t *id)
{
    return solar_os_native_result(solar_os_inbox_publish(message, id));
}

static int inbox_get(uint32_t id, solar_os_inbox_entry_t *entry, bool mark_read)
{
    return solar_os_native_result(solar_os_inbox_get(id, entry, mark_read));
}

static int inbox_mark_read(uint32_t id, bool read)
{
    return solar_os_native_result(solar_os_inbox_mark_read(id, read));
}

static int inbox_delete(uint32_t id)
{
    return solar_os_native_result(solar_os_inbox_delete(id));
}

static int inbox_delete_many(const uint32_t *ids, size_t count, size_t *deleted)
{
    return solar_os_native_result(solar_os_inbox_delete_many(ids, count, deleted));
}

static int inbox_delete_sources(const char *const *sources, size_t count, size_t *deleted)
{
    return solar_os_native_result(solar_os_inbox_delete_sources(sources, count, deleted));
}

static int inbox_clear(void)
{
    return solar_os_native_result(solar_os_inbox_clear());
}

static int inbox_set_sound(bool enabled)
{
    return solar_os_native_result(solar_os_inbox_set_sound_enabled(enabled));
}

static int inbox_test_sound(uint32_t *request_id)
{
    return solar_os_native_result(solar_os_inbox_test_sound(request_id));
}

static const solar_os_native_inbox_api_v1_t inbox_api = {
    .abi_version = SOLAR_OS_NATIVE_INBOX_ABI,
    .struct_size = sizeof(inbox_api),
    .status = inbox_status,
    .publish = inbox_publish,
    .snapshot = solar_os_inbox_snapshot,
    .get = inbox_get,
    .mark_read = inbox_mark_read,
    .matches_source_id = solar_os_inbox_matches_source_id,
    .delete_entry = inbox_delete,
    .delete_many = inbox_delete_many,
    .delete_sources = inbox_delete_sources,
    .clear = inbox_clear,
    .set_sound = inbox_set_sound,
    .test_sound = inbox_test_sound,
    .priority_name = solar_os_inbox_priority_name,
};
#endif

const void *solar_os_native_inbox_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_inbox_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
