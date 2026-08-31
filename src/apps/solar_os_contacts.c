#include "solar_os_contacts_app.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_contacts.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define CONTACTS_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)

typedef struct {
    solar_os_tui_t tui;
    solar_os_contact_t *contacts;
    solar_os_endpoint_t *endpoints;
    size_t visible[SOLAR_OS_CONTACT_CAPACITY];
    size_t contact_count;
    size_t visible_count;
    size_t cursor;
    size_t top;
    bool detail;
    bool searching;
    char search[SOLAR_OS_CONTACT_NAME_MAX + 1U];
    size_t search_len;
    solar_os_contact_t selected;
    size_t endpoint_count;
    size_t detail_scroll;
    uint32_t generation;
} contacts_app_state_t;

static void *contacts_app_state;
#define contacts_app (*(contacts_app_state_t *)contacts_app_state)

static int contacts_app_compare(const void *first_value,
                                const void *second_value)
{
    const solar_os_contact_t *first = first_value;
    const solar_os_contact_t *second = second_value;
    if (first->primary_trust != second->primary_trust) {
        return (int)first->primary_trust - (int)second->primary_trust;
    }
    if (first->primary_provider != second->primary_provider) {
        return (int)first->primary_provider - (int)second->primary_provider;
    }
    return strcasecmp(first->display_name, second->display_name);
}

static bool contacts_app_contains_folded(const char *text, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    const size_t needle_len = strlen(needle);
    for (size_t offset = 0U; text[offset] != '\0'; offset++) {
        size_t matched = 0U;
        while (matched < needle_len &&
               text[offset + matched] != '\0' &&
               tolower((unsigned char)text[offset + matched]) ==
                   tolower((unsigned char)needle[matched])) {
            matched++;
        }
        if (matched == needle_len) {
            return true;
        }
    }
    return false;
}

static void contacts_app_rebuild_visible(void)
{
    contacts_app.visible_count = 0U;
    for (size_t index = 0; index < contacts_app.contact_count; index++) {
        if (contacts_app_contains_folded(
                contacts_app.contacts[index].display_name,
                contacts_app.search)) {
            contacts_app.visible[contacts_app.visible_count++] = index;
        }
    }
    if (contacts_app.visible_count == 0U) {
        contacts_app.cursor = 0U;
        contacts_app.top = 0U;
    } else if (contacts_app.cursor >= contacts_app.visible_count) {
        contacts_app.cursor = contacts_app.visible_count - 1U;
    }
}

static void contacts_app_refresh(void)
{
    solar_os_contact_id_t selected_id = SOLAR_OS_CONTACT_ID_NONE;
    if (contacts_app.cursor < contacts_app.visible_count) {
        selected_id =
            contacts_app.contacts[contacts_app.visible[contacts_app.cursor]].id;
    }
    size_t total = 0U;
    contacts_app.contact_count =
        solar_os_contacts_snapshot(contacts_app.contacts,
                                   SOLAR_OS_CONTACT_CAPACITY,
                                   false,
                                   SOLAR_OS_CONTACT_TRUST_DISCOVERED,
                                   &total);
    qsort(contacts_app.contacts,
          contacts_app.contact_count,
          sizeof(*contacts_app.contacts),
          contacts_app_compare);
    contacts_app_rebuild_visible();
    if (selected_id != SOLAR_OS_CONTACT_ID_NONE) {
        for (size_t index = 0; index < contacts_app.visible_count; index++) {
            if (contacts_app.contacts[contacts_app.visible[index]].id ==
                selected_id) {
                contacts_app.cursor = index;
                break;
            }
        }
    }
    solar_os_contacts_status_t status;
    if (solar_os_contacts_get_status(&status) == ESP_OK) {
        contacts_app.generation = status.generation;
    }
}

static size_t contacts_app_visible_rows(void)
{
    const size_t rows = solar_os_tui_rows(&contacts_app.tui);
    return rows > 2U ? rows - 2U : 0U;
}

static void contacts_app_ensure_visible(void)
{
    solar_os_tui_viewport_t viewport = {
        .cursor = contacts_app.cursor, .top = contacts_app.top,
    };
    solar_os_tui_viewport_reconcile(&viewport, contacts_app.visible_count,
                                    contacts_app_visible_rows());
    contacts_app.cursor = viewport.cursor;
    contacts_app.top = viewport.top;
}

static char contacts_app_trust_marker(solar_os_contact_trust_t trust)
{
    switch (trust) {
    case SOLAR_OS_CONTACT_TRUST_TRUSTED:
        return 'T';
    case SOLAR_OS_CONTACT_TRUST_BLOCKED:
        return 'B';
    case SOLAR_OS_CONTACT_TRUST_DISCOVERED:
    default:
        return 'D';
    }
}

static void contacts_app_render_list(void)
{
    const size_t rows = solar_os_tui_rows(&contacts_app.tui);
    const size_t cols = solar_os_tui_cols(&contacts_app.tui);
    char line[CONTACTS_APP_LINE_MAX];
    contacts_app_ensure_visible();

    snprintf(line,
             sizeof(line),
             "Contacts %u%s%s",
             (unsigned)contacts_app.visible_count,
             contacts_app.search[0] != '\0' ? "  /" : "",
             contacts_app.search);
    solar_os_tui_draw_title(&contacts_app.tui, line, NULL);

    const size_t visible_rows = contacts_app_visible_rows();
    for (size_t row = 0U; row < visible_rows; row++) {
        const size_t visible_index = contacts_app.top + row;
        if (visible_index >= contacts_app.visible_count) {
            solar_os_tui_write_cell(&contacts_app.tui,
                row + 1U,
                0U,
                cols,
                row == 0U && contacts_app.visible_count == 0U ?
                    (contacts_app.search[0] != '\0' ?
                         "No matching contacts" : "No contacts") : "",
                SOLAR_OS_TUI_ATTR_NORMAL);
            continue;
        }
        const solar_os_contact_t *contact =
            &contacts_app.contacts[contacts_app.visible[visible_index]];
        snprintf(line,
                 sizeof(line),
                 "%c %-8s %s  (%u)",
                 contacts_app_trust_marker(contact->primary_trust),
                 solar_os_messaging_provider_name(contact->primary_provider),
                 contact->display_name,
                 (unsigned)contact->endpoint_count);
        solar_os_tui_write_cell(&contacts_app.tui,
            row + 1U,
            0U,
            cols,
            line,
            visible_index == contacts_app.cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 0U) {
        snprintf(line,
                 sizeof(line),
                 contacts_app.searching ?
                    "Search: /%s  Enter accept  Esc clear" :
                    "Enter details  / search  q quit",
                 contacts_app.search);
        solar_os_tui_draw_help(&contacts_app.tui, line);
    }
}

static void contacts_app_address(const solar_os_endpoint_t *endpoint,
                                 char *text,
                                 size_t text_len)
{
    size_t used = 0U;
    if (text_len == 0U) {
        return;
    }
    text[0] = '\0';
    for (size_t index = 0U;
         index < endpoint->address.length && used + 2U < text_len;
         index++) {
        const int written =
            snprintf(text + used, text_len - used, "%02x",
                     endpoint->address.bytes[index]);
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }
}

static size_t contacts_app_detail_lines(void)
{
    return 5U + contacts_app.endpoint_count * 4U;
}

static void contacts_app_render_detail(void)
{
    const size_t rows = solar_os_tui_rows(&contacts_app.tui);
    const size_t cols = solar_os_tui_cols(&contacts_app.tui);
    char line_text[CONTACTS_APP_LINE_MAX];

    solar_os_tui_draw_title(&contacts_app.tui,
                            contacts_app.selected.display_name, NULL);
    const size_t visible = contacts_app_visible_rows();
    for (size_t row = 0U; row < visible; row++) {
        const size_t line = contacts_app.detail_scroll + row;
        line_text[0] = '\0';
        if (line == 0U) {
            snprintf(line_text,
                     sizeof(line_text),
                     "Contact %" PRIu32 ": %s",
                     contacts_app.selected.id,
                     contacts_app.selected.display_name);
        } else if (line == 1U) {
            snprintf(line_text,
                     sizeof(line_text),
                     "Endpoints: %u  Flags: 0x%08" PRIx32,
                     (unsigned)contacts_app.endpoint_count,
                     contacts_app.selected.flags);
        } else if (line == 2U) {
            snprintf(line_text,
                     sizeof(line_text),
                     "Created: %" PRIu64 " ms",
                     contacts_app.selected.created_ms);
        } else if (line == 3U) {
            snprintf(line_text,
                     sizeof(line_text),
                     "Updated: %" PRIu64 " ms",
                     contacts_app.selected.updated_ms);
        } else if (line >= 5U && line < contacts_app_detail_lines()) {
            const size_t endpoint_index = (line - 5U) / 4U;
            const size_t endpoint_line = (line - 5U) % 4U;
            const solar_os_endpoint_t *endpoint =
                &contacts_app.endpoints[endpoint_index];
            if (endpoint_line == 0U) {
                snprintf(line_text,
                         sizeof(line_text),
                         "Endpoint %" PRIu32 "  %s / %s",
                         endpoint->id,
                         solar_os_messaging_provider_name(endpoint->provider),
                         solar_os_contact_trust_name(endpoint->trust));
            } else if (endpoint_line == 1U) {
                char address[SOLAR_OS_MESSAGING_ADDRESS_MAX * 2U + 1U];
                contacts_app_address(endpoint, address, sizeof(address));
                snprintf(line_text,
                         sizeof(line_text),
                         "  Address: %s",
                         address);
            } else if (endpoint_line == 2U) {
                snprintf(line_text,
                         sizeof(line_text),
                         "  Security/caps: 0x%08" PRIx32,
                         endpoint->capabilities);
            } else {
                snprintf(line_text,
                         sizeof(line_text),
                         "  Seen: %" PRIu64 " ms  Metadata: %u bytes",
                         endpoint->last_seen_ms,
                         (unsigned)endpoint->provider_metadata_len);
            }
        }
        solar_os_tui_write_cell(&contacts_app.tui, row + 1U,
                                0U,
                                cols,
                                line_text,
                                SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 0U) {
        solar_os_tui_draw_help(&contacts_app.tui,
                               "Up/Down scroll  Esc back  q quit");
    }
}

static void contacts_app_render(void)
{
    solar_os_tui_clear(&contacts_app.tui);
    if (contacts_app.detail) {
        contacts_app_render_detail();
    } else {
        contacts_app_render_list();
    }
    solar_os_tui_set_cursor_visible(&contacts_app.tui, false);
    solar_os_tui_refresh(&contacts_app.tui);
}

static void contacts_app_open(void)
{
    if (contacts_app.cursor >= contacts_app.visible_count) {
        return;
    }
    contacts_app.selected =
        contacts_app.contacts[contacts_app.visible[contacts_app.cursor]];
    contacts_app.endpoint_count =
        solar_os_contacts_endpoint_snapshot(contacts_app.selected.id,
                                            contacts_app.endpoints,
                                            SOLAR_OS_ENDPOINT_CAPACITY);
    contacts_app.detail_scroll = 0U;
    contacts_app.detail = true;
}

static esp_err_t contacts_app_start(solar_os_context_t *ctx)
{
    memset(&contacts_app, 0, sizeof(contacts_app));
    esp_err_t error = solar_os_contacts_init();
    if (error != ESP_OK) {
        return error;
    }
    contacts_app.contacts =
        solar_os_memory_calloc(SOLAR_OS_CONTACT_CAPACITY,
                               sizeof(*contacts_app.contacts),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "app.contacts");
    contacts_app.endpoints =
        solar_os_memory_calloc(SOLAR_OS_ENDPOINT_CAPACITY,
                               sizeof(*contacts_app.endpoints),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "app.contacts.detail");
    if (contacts_app.contacts == NULL || contacts_app.endpoints == NULL) {
        solar_os_memory_free(contacts_app.contacts);
        solar_os_memory_free(contacts_app.endpoints);
        memset(&contacts_app, 0, sizeof(contacts_app));
        return ESP_ERR_NO_MEM;
    }
    error = solar_os_tui_screen_begin(&contacts_app.tui, ctx);
    if (error != ESP_OK) {
        solar_os_memory_free(contacts_app.contacts);
        solar_os_memory_free(contacts_app.endpoints);
        memset(&contacts_app, 0, sizeof(contacts_app));
        return error;
    }
    contacts_app_refresh();
    contacts_app_render();
    return ESP_OK;
}

static void contacts_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&contacts_app.tui, true);
    solar_os_tui_refresh(&contacts_app.tui);
    solar_os_tui_end(&contacts_app.tui);
    solar_os_memory_free(contacts_app.contacts);
    solar_os_memory_free(contacts_app.endpoints);
    memset(&contacts_app, 0, sizeof(contacts_app));
}

static void contacts_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    contacts_app_refresh();
    contacts_app_render();
}

static void contacts_app_title(solar_os_context_t *ctx,
                               char *buffer,
                               size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }
    if (contacts_app.detail && contacts_app.selected.display_name[0] != '\0') {
        snprintf(buffer,
                 buffer_len,
                 "contacts: %s",
                 contacts_app.selected.display_name);
    } else {
        strlcpy(buffer, "contacts", buffer_len);
    }
}

static bool contacts_app_search_event(uint8_t ch)
{
    if (ch == SOLAR_OS_KEY_ESCAPE) {
        contacts_app.searching = false;
        contacts_app.search_len = 0U;
        contacts_app.search[0] = '\0';
        contacts_app_rebuild_visible();
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        contacts_app.searching = false;
        return true;
    }
    if (ch == '\b' || ch == 0x7fU) {
        if (contacts_app.search_len > 0U) {
            contacts_app.search[--contacts_app.search_len] = '\0';
            contacts_app.cursor = 0U;
            contacts_app.top = 0U;
            contacts_app_rebuild_visible();
        }
        return true;
    }
    if (ch >= 0x20U && ch < 0x7fU &&
        contacts_app.search_len < SOLAR_OS_CONTACT_NAME_MAX) {
        contacts_app.search[contacts_app.search_len++] = (char)ch;
        contacts_app.search[contacts_app.search_len] = '\0';
        contacts_app.cursor = 0U;
        contacts_app.top = 0U;
        contacts_app_rebuild_visible();
    }
    return true;
}

static bool contacts_app_event(solar_os_context_t *ctx,
                               const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        solar_os_contacts_status_t status;
        if (!contacts_app.detail &&
            solar_os_contacts_get_status(&status) == ESP_OK &&
            status.generation != contacts_app.generation) {
            contacts_app_refresh();
            contacts_app_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }
    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (contacts_app.searching) {
        (void)contacts_app_search_event(ch);
        contacts_app_render();
        return true;
    }
    if (ch == 'q' || ch == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (contacts_app.detail && ch == SOLAR_OS_KEY_ESCAPE) {
        contacts_app.detail = false;
        contacts_app_refresh();
        contacts_app_render();
        return true;
    }
    if (!contacts_app.detail && ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }

    switch (ch) {
    case '/':
        if (!contacts_app.detail) {
            contacts_app.searching = true;
        }
        break;
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (contacts_app.detail) {
            if (contacts_app.detail_scroll > 0U) {
                contacts_app.detail_scroll--;
            }
        } else if (contacts_app.cursor > 0U) {
            contacts_app.cursor--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (contacts_app.detail) {
            const size_t total = contacts_app_detail_lines();
            if (contacts_app.detail_scroll + contacts_app_visible_rows() <
                total) {
                contacts_app.detail_scroll++;
            }
        } else if (contacts_app.cursor + 1U < contacts_app.visible_count) {
            contacts_app.cursor++;
        }
        break;
    case '\r':
    case '\n':
    case SOLAR_OS_KEY_RIGHT:
        if (!contacts_app.detail) {
            contacts_app_open();
        }
        break;
    case SOLAR_OS_KEY_LEFT:
        if (contacts_app.detail) {
            contacts_app.detail = false;
            contacts_app_refresh();
        }
        break;
    default:
        return true;
    }
    contacts_app_render();
    return true;
}

const solar_os_app_t solar_os_contacts_app = {
    .name = "contacts",
    .summary = "provider-neutral contact browser",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = contacts_app_start,
    .resume = contacts_app_resume,
    .stop = contacts_app_stop,
    .event = contacts_app_event,
    .title = contacts_app_title,
    .state_slot = &contacts_app_state,
    .state_size = sizeof(contacts_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};
