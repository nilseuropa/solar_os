#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_contacts.h"
#include "solar_os_contacts_app.h"
#include "solar_os_credentials.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const contacts_commands[] = {
    "status", "list", "show", "rename", "trust", "block", "remove", "link",
};

static void contacts_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  contacts");
    solar_os_shell_io_writeln(io, "  contacts status");
    solar_os_shell_io_writeln(io, "  contacts list [all|discovered|trusted|blocked]");
    solar_os_shell_io_writeln(io, "  contacts show <contact-id>");
    solar_os_shell_io_writeln(io, "  contacts rename <contact-id> <name>");
    solar_os_shell_io_writeln(io, "  contacts trust <contact-id> [endpoint-id]");
    solar_os_shell_io_writeln(io, "  contacts block <contact-id> [endpoint-id]");
    solar_os_shell_io_writeln(io, "  contacts remove <contact-id>");
    solar_os_shell_io_writeln(io, "  contacts link <target-contact-id> <source-contact-id>");
}

static bool contacts_parse_id(const char *text, uint32_t *id)
{
    if (text == NULL || id == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0UL || value > UINT32_MAX) {
        return false;
    }
    *id = (uint32_t)value;
    return true;
}

static void contacts_status(solar_os_shell_io_t *io)
{
    solar_os_contacts_status_t contacts;
    esp_err_t error = solar_os_contacts_get_status(&contacts);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "contacts: unavailable: %s\n",
                                 solar_os_shell_error_text(error));
        return;
    }
    solar_os_shell_io_printf(io,
                             "Contacts: %u/%u\n"
                             "Endpoints: %u/%u\n"
                             "Generation: %" PRIu32 "\n"
                             "Evicted: %" PRIu32 "\n"
                             "PSRAM: %s\n"
                             "Persistence: %s (%u bytes)\n",
                             (unsigned)contacts.contact_count,
                             (unsigned)contacts.contact_capacity,
                             (unsigned)contacts.endpoint_count,
                             (unsigned)contacts.endpoint_capacity,
                             contacts.generation,
                             contacts.evicted,
                             contacts.records_in_psram ? "yes" : "no",
                             contacts.persistent ? "active" : "volatile",
                             (unsigned)contacts.storage_bytes);
    if (contacts.storage_error != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "Storage error: %s\n",
                                 solar_os_shell_error_text(contacts.storage_error));
    }

    solar_os_credentials_status_t credentials;
    if (solar_os_credentials_get_status(&credentials) == ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "Opaque credentials: %u/%u, persistence %s\n",
                                 (unsigned)credentials.count,
                                 (unsigned)credentials.capacity,
                                 credentials.persistent ? "active" : "error");
        if (credentials.storage_error != ESP_OK) {
            solar_os_shell_io_printf(
                io,
                "Credential storage error: %s\n",
                solar_os_shell_error_text(credentials.storage_error));
        }
    }
}

static void contacts_list(solar_os_shell_io_t *io,
                          bool filter,
                          solar_os_contact_trust_t trust)
{
    solar_os_contact_t *contacts =
        solar_os_memory_calloc(SOLAR_OS_CONTACT_CAPACITY,
                               sizeof(*contacts),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "contacts.shell.list");
    if (contacts == NULL) {
        solar_os_shell_io_table_writeln(io, "contacts: no PSRAM for snapshot");
        return;
    }
    size_t total = 0U;
    const size_t count =
        solar_os_contacts_snapshot(contacts,
                                   SOLAR_OS_CONTACT_CAPACITY,
                                   filter,
                                   trust,
                                   &total);
    for (size_t index = 0U; index < count; index++) {
        solar_os_shell_io_table_printf(
            io,
            "%" PRIu32 "  %-10s %-8s  %s  (%u endpoint%s)\n",
            contacts[index].id,
            solar_os_contact_trust_name(contacts[index].primary_trust),
            solar_os_messaging_provider_name(contacts[index].primary_provider),
            contacts[index].display_name,
            (unsigned)contacts[index].endpoint_count,
            contacts[index].endpoint_count == 1U ? "" : "s");
    }
    if (total == 0U) {
        solar_os_shell_io_table_writeln(io, "No contacts");
    }
    solar_os_memory_free(contacts);
}

static void contacts_print_address(solar_os_shell_io_t *io,
                                   const solar_os_endpoint_t *endpoint)
{
    for (size_t index = 0U; index < endpoint->address.length; index++) {
        solar_os_shell_io_printf(io, "%02x", endpoint->address.bytes[index]);
    }
}

static void contacts_show(solar_os_shell_io_t *io,
                          solar_os_contact_id_t contact_id)
{
    solar_os_contact_t contact;
    esp_err_t error = solar_os_contacts_get(contact_id, &contact);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "contacts: %" PRIu32 ": %s\n",
                                 contact_id,
                                 solar_os_shell_error_text(error));
        return;
    }
    solar_os_shell_io_printf(io,
                             "Contact: %" PRIu32 "\n"
                             "Name: %s\n"
                             "Flags: 0x%08" PRIx32 "\n"
                             "Created: %" PRIu64 " ms\n"
                             "Updated: %" PRIu64 " ms\n",
                             contact.id,
                             contact.display_name,
                             contact.flags,
                             contact.created_ms,
                             contact.updated_ms);

    solar_os_endpoint_t *endpoints =
        solar_os_memory_calloc(SOLAR_OS_ENDPOINT_CAPACITY,
                               sizeof(*endpoints),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "contacts.shell.show");
    if (endpoints == NULL) {
        solar_os_shell_io_writeln(io, "contacts: no PSRAM for endpoint snapshot");
        return;
    }
    const size_t count =
        solar_os_contacts_endpoint_snapshot(contact_id,
                                            endpoints,
                                            SOLAR_OS_ENDPOINT_CAPACITY);
    for (size_t index = 0U; index < count; index++) {
        const solar_os_endpoint_t *endpoint = &endpoints[index];
        solar_os_shell_io_printf(
            io,
            "Endpoint %" PRIu32 ": %s, %s, caps 0x%08" PRIx32 "\n  Address: ",
            endpoint->id,
            solar_os_messaging_provider_name(endpoint->provider),
            solar_os_contact_trust_name(endpoint->trust),
            endpoint->capabilities);
        contacts_print_address(io, endpoint);
        solar_os_shell_io_printf(io,
                                 "\n  Last seen: %" PRIu64
                                 " ms, metadata: %u bytes\n",
                                 endpoint->last_seen_ms,
                                 (unsigned)endpoint->provider_metadata_len);
    }
    solar_os_memory_free(endpoints);
}

static void contacts_result(solar_os_shell_io_t *io,
                            const char *operation,
                            esp_err_t error)
{
    if (error == ESP_OK) {
        solar_os_shell_io_printf(io, "contacts: %s\n", operation);
    } else {
        solar_os_shell_io_printf(io,
                                 "contacts: %s failed: %s\n",
                                 operation,
                                 solar_os_shell_error_text(error));
    }
}

void solar_os_shell_cmd_contacts(solar_os_context_t *ctx,
                                 int argc,
                                 char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 1) {
        const esp_err_t error =
            solar_os_context_request_launch(ctx, &solar_os_contacts_app, 0, NULL);
        if (error == ESP_OK) {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        } else {
            solar_os_shell_io_printf(io,
                                     "contacts: launch failed: %s\n",
                                     solar_os_shell_error_text(error));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        contacts_status(io);
        return;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "list") == 0) {
        bool filter = false;
        solar_os_contact_trust_t trust = SOLAR_OS_CONTACT_TRUST_DISCOVERED;
        if (argc == 3 && strcmp(argv[2], "all") != 0) {
            filter = true;
            if (strcmp(argv[2], "discovered") == 0) {
                trust = SOLAR_OS_CONTACT_TRUST_DISCOVERED;
            } else if (strcmp(argv[2], "trusted") == 0) {
                trust = SOLAR_OS_CONTACT_TRUST_TRUSTED;
            } else if (strcmp(argv[2], "blocked") == 0) {
                trust = SOLAR_OS_CONTACT_TRUST_BLOCKED;
            } else {
                solar_os_shell_diag_invalid(io,
                                            "contacts list",
                                            "filter",
                                            argv[2],
                                            "all, discovered, trusted, or blocked",
                                            "contacts list [all|discovered|trusted|blocked]",
                                            false);
                return;
            }
        }
        contacts_list(io, filter, trust);
        return;
    }

    uint32_t first_id = 0U;
    if (argc >= 3 && !contacts_parse_id(argv[2], &first_id)) {
        solar_os_shell_diag_invalid(io,
                                    "contacts",
                                    "contact ID",
                                    argv[2],
                                    "a decimal contact ID",
                                    NULL,
                                    false);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "show") == 0) {
        contacts_show(io, first_id);
        return;
    }
    if (argc == 4 && strcmp(argv[1], "rename") == 0) {
        contacts_result(io,
                        "renamed",
                        solar_os_contacts_rename(first_id, argv[3]));
        return;
    }
    if ((argc == 3 || argc == 4) &&
        (strcmp(argv[1], "trust") == 0 ||
         strcmp(argv[1], "block") == 0)) {
        uint32_t endpoint_id = 0U;
        if (argc == 4 && !contacts_parse_id(argv[3], &endpoint_id)) {
            solar_os_shell_diag_invalid(io,
                                        "contacts",
                                        "endpoint ID",
                                        argv[3],
                                        "a decimal endpoint ID",
                                        NULL,
                                        false);
            return;
        }
        const bool block = strcmp(argv[1], "block") == 0;
        contacts_result(
            io,
            block ? "blocked" : "trusted",
            solar_os_contacts_set_trust(
                first_id,
                endpoint_id,
                block ? SOLAR_OS_CONTACT_TRUST_BLOCKED :
                        SOLAR_OS_CONTACT_TRUST_TRUSTED));
        return;
    }
    if (argc == 3 && strcmp(argv[1], "remove") == 0) {
        contacts_result(io,
                        "removed",
                        solar_os_contacts_remove(first_id));
        return;
    }
    if (argc == 4 && strcmp(argv[1], "link") == 0) {
        uint32_t source_id = 0U;
        if (!contacts_parse_id(argv[3], &source_id)) {
            solar_os_shell_diag_invalid(io,
                                        "contacts link",
                                        "source contact ID",
                                        argv[3],
                                        "a decimal contact ID",
                                        "contacts link <target-contact-id> <source-contact-id>",
                                        false);
            return;
        }
        contacts_result(io,
                        "linked",
                        solar_os_contacts_link(first_id, source_id));
        return;
    }
    solar_os_shell_diag_subcommand(io,
                                   "contacts",
                                   argc,
                                   argv,
                                   "contacts status|list|show|rename|trust|block|remove|link",
                                   contacts_commands,
                                   sizeof(contacts_commands) / sizeof(contacts_commands[0]));
}
