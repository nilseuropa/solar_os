#include "solar_os_native_contacts.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_contacts_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_CONTACTS
#include "solar_os_contacts.h"

static int contacts_status(solar_os_contacts_status_t *status)
{
    esp_err_t error = solar_os_contacts_get_status(status);
    if (error == ESP_OK) status->storage_error = solar_os_native_result(status->storage_error);
    return solar_os_native_result(error);
}

static int contacts_get(solar_os_contact_id_t id, solar_os_contact_t *entry)
{
    return solar_os_native_result(solar_os_contacts_get(id, entry));
}

static int contacts_endpoint_get(solar_os_endpoint_id_t id, solar_os_endpoint_t *entry)
{
    return solar_os_native_result(solar_os_contacts_get_endpoint(id, entry));
}

static int contacts_endpoint_find(solar_os_messaging_provider_id_t provider,
    const solar_os_messaging_address_t *address, solar_os_endpoint_t *entry)
{
    if (!address) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(
        solar_os_contacts_find_endpoint(provider, address->bytes, address->length, entry));
}

static int contacts_upsert_discovered(const solar_os_contact_discovery_v1_t *entry, bool batch,
    solar_os_contact_id_t *contact, solar_os_endpoint_id_t *endpoint)
{
    return solar_os_native_result(solar_os_contacts_apply_discovery(entry, batch, contact, endpoint));
}

static int contacts_flush(void)
{
    return solar_os_native_result(solar_os_contacts_flush());
}

static int contacts_rename(solar_os_contact_id_t id, const char *name)
{
    return solar_os_native_result(solar_os_contacts_rename(id, name));
}

static int contacts_set_trust(solar_os_contact_id_t contact, solar_os_endpoint_id_t endpoint,
    solar_os_contact_trust_t trust)
{
    return solar_os_native_result(solar_os_contacts_set_trust(contact, endpoint, trust));
}

static int contacts_remove(solar_os_contact_id_t id)
{
    return solar_os_native_result(solar_os_contacts_remove(id));
}

static int contacts_link(solar_os_contact_id_t target, solar_os_contact_id_t source)
{
    return solar_os_native_result(solar_os_contacts_link(target, source));
}

static const solar_os_native_contacts_api_v1_t contacts_api = {
    .abi_version = SOLAR_OS_NATIVE_CONTACTS_ABI,
    .struct_size = sizeof(contacts_api),
    .status = contacts_status,
    .snapshot = solar_os_contacts_snapshot,
    .get = contacts_get,
    .endpoints = solar_os_contacts_endpoint_snapshot,
    .endpoint_get = contacts_endpoint_get,
    .endpoint_find = contacts_endpoint_find,
    .upsert_discovered = contacts_upsert_discovered,
    .flush = contacts_flush,
    .rename = contacts_rename,
    .set_trust = contacts_set_trust,
    .remove = contacts_remove,
    .link = contacts_link,
};
#endif

const void *solar_os_native_contacts_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_contacts_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
