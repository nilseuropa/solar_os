#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_messaging_types.h"
#include "solar_os_native_contacts_abi.h"

#define SOLAR_OS_CONTACT_CAPACITY 512U
#define SOLAR_OS_ENDPOINT_CAPACITY 576U
#define SOLAR_OS_CONTACT_STORE_DIR ".contacts"
#define SOLAR_OS_CONTACT_STORE_FILE "contacts.bin"
#define SOLAR_OS_CONTACT_STORE_LIMIT_BYTES (192U * 1024U)

esp_err_t solar_os_contacts_init(void);
esp_err_t solar_os_contacts_get_status(solar_os_contacts_status_t *status);

size_t solar_os_contacts_snapshot(solar_os_contact_t *contacts,
                                  size_t max_contacts,
                                  bool filter_trust,
                                  solar_os_contact_trust_t trust,
                                  size_t *total_contacts);
esp_err_t solar_os_contacts_get(solar_os_contact_id_t id,
                                solar_os_contact_t *contact);
size_t solar_os_contacts_endpoint_snapshot(solar_os_contact_id_t contact_id,
                                           solar_os_endpoint_t *endpoints,
                                           size_t max_endpoints);
esp_err_t solar_os_contacts_get_endpoint(solar_os_endpoint_id_t id,
                                         solar_os_endpoint_t *endpoint);
esp_err_t solar_os_contacts_find_endpoint(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address,
    size_t address_len,
    solar_os_endpoint_t *endpoint);

esp_err_t solar_os_contacts_apply_discovery(const solar_os_contact_discovery_v1_t *entry,
    bool batch, solar_os_contact_id_t *contact, solar_os_endpoint_id_t *endpoint);
esp_err_t solar_os_contacts_upsert_discovered(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address,
    size_t address_len,
    const char *display_name,
    uint32_t capabilities,
    uint64_t last_seen_ms,
    const void *provider_metadata,
    size_t provider_metadata_len,
    solar_os_contact_id_t *contact_id,
    solar_os_endpoint_id_t *endpoint_id);
/* Import without evicting retained contacts or writing each record to disk.
 * Call flush after the batch, including after a partially completed import. */
esp_err_t solar_os_contacts_import_discovered(
    solar_os_messaging_provider_id_t provider,
    const uint8_t *address, size_t address_len, const char *display_name,
    uint32_t capabilities, uint64_t last_seen_ms,
    const void *provider_metadata, size_t provider_metadata_len,
    solar_os_contact_id_t *contact_id, solar_os_endpoint_id_t *endpoint_id);
esp_err_t solar_os_contacts_flush(void);
esp_err_t solar_os_contacts_rename(solar_os_contact_id_t id,
                                   const char *display_name);
esp_err_t solar_os_contacts_set_trust(solar_os_contact_id_t contact_id,
                                      solar_os_endpoint_id_t endpoint_id,
                                      solar_os_contact_trust_t trust);
esp_err_t solar_os_contacts_remove(solar_os_contact_id_t id);
esp_err_t solar_os_contacts_link(solar_os_contact_id_t target_id,
                                 solar_os_contact_id_t source_id);
