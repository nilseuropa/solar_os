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
#include "solar_os_config.h"
#include "solar_os_contacts.h"
#if SOLAR_OS_PACKAGE_APP_CHAT
#include "solar_os_chat_app.h"
#include "solar_os_messaging.h"
#endif
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define CONTACTS_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)

typedef enum {
    CONTACTS_BROWSE, CONTACTS_ACTIONS, CONTACTS_ENDPOINTS, CONTACTS_RENAME,
    CONTACTS_LINK, CONTACTS_CONFIRM_LINK, CONTACTS_CONFIRM_REMOVE,
    CONTACTS_HELP, CONTACTS_STATUS, CONTACTS_FILTER, CONTACTS_NOTICE,
} contacts_app_mode_t;

static const char * const contacts_actions[] = {
#if SOLAR_OS_PACKAGE_APP_CHAT
    "c Chat",
#endif
    "t Trust endpoints...", "b Block endpoints...", "r Rename...",
    "l Link contact here", "d Remove contact...",
};
static const char * const contacts_filters[] = {"All", "Discovered", "Trusted", "Blocked"};
static const char * const contacts_help[] = {
    "Up/Down: move", "Enter/Right: details",
#if SOLAR_OS_PACKAGE_APP_CHAT
    "c: chat with selected contact",
#endif
    "a: contact actions", "t: trust endpoints",
    "b: block endpoints", "r: rename contact",
    "l: link contact here", "d: remove contact",
    "/: search names", "f: trust filter", "s: store status",
    "D: discovered", "T: trusted", "B: blocked", "Esc/Left: back", "q: quit", "?: this help",
};

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
    contacts_app_mode_t mode;
    solar_os_tui_viewport_t menu;
    solar_os_tui_input_state_t input;
    solar_os_contact_trust_t pending_trust;
    solar_os_contact_id_t link_source;
    char link_name[SOLAR_OS_CONTACT_NAME_MAX + 1U];
    char edit[SOLAR_OS_CONTACT_NAME_MAX + 1U];
    char notice[96];
    uint8_t filter;
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
        bool trust_match = contacts_app.filter == 0U;
        for (size_t endpoint = 0; !trust_match && endpoint < contacts_app.endpoint_count; endpoint++) {
            trust_match = contacts_app.endpoints[endpoint].contact_id == contacts_app.contacts[index].id &&
                contacts_app.endpoints[endpoint].trust == (solar_os_contact_trust_t)(contacts_app.filter - 1U);
        }
        if (trust_match && contacts_app_contains_folded(
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
    /* The same endpoint buffer serves list filtering and the open detail view. */
    contacts_app.endpoint_count = solar_os_contacts_endpoint_snapshot(
        SOLAR_OS_CONTACT_ID_NONE, contacts_app.endpoints, SOLAR_OS_ENDPOINT_CAPACITY);
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

static void contacts_app_help_bar(const char *text)
{
    const size_t rows = solar_os_tui_rows(&contacts_app.tui);
    if (rows > 1U)
        solar_os_tui_write_cell(&contacts_app.tui, rows - 1U, 0U,
            solar_os_tui_cols(&contacts_app.tui), text, SOLAR_OS_TUI_ATTR_INVERSE);
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
             "Contacts %u %s%s%s",
             (unsigned)contacts_app.visible_count,
             contacts_filters[contacts_app.filter],
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
    if (rows > 1U && contacts_app.searching) {
        solar_os_tui_input_state_t input = {.cursor = contacts_app.search_len};
        solar_os_tui_draw_input(&contacts_app.tui, rows - 1U, 0U, cols, "Search: /",
            contacts_app.search, &input, SOLAR_OS_TUI_ATTR_INVERSE);
    } else {
        contacts_app_help_bar(
#if SOLAR_OS_PACKAGE_APP_CHAT
            cols < 25U ? "c Chat a Acts ? Help" :
            cols < 48U ? "c Chat  a Actions  ? Help" :
            "c Chat  a Actions  ? Help  Enter view  / Search"
#else
            cols < 38U ? "a Actions  ? Help" :
            "a Actions  ? Help  Enter view  / Search"
#endif
        );
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
        contacts_app_help_bar(
#if SOLAR_OS_PACKAGE_APP_CHAT
            cols < 25U ? "c Chat a Acts ? Help" :
            cols < 48U ? "c Chat  a Actions  ? Help" :
            "c Chat  a Actions  ? Help  Up/Down scroll  Esc back"
#else
            cols < 38U ? "a Actions  ? Help" :
            "a Actions  ? Help  Up/Down scroll  Esc back"
#endif
        );
    }
}

static const solar_os_contact_t *contacts_app_link_contact(size_t item)
{
    for (size_t i = 0; i < contacts_app.contact_count; i++) {
        if (contacts_app.contacts[i].id == contacts_app.selected.id) continue;
        if (item-- == 0U) return &contacts_app.contacts[i];
    }
    return NULL;
}

static size_t contacts_app_menu_count(void)
{
    switch (contacts_app.mode) {
    case CONTACTS_ACTIONS: return sizeof(contacts_actions) / sizeof(contacts_actions[0]);
    case CONTACTS_ENDPOINTS: return contacts_app.endpoint_count + 1U;
    case CONTACTS_LINK: {
        size_t count = 0;
        for (size_t i = 0; i < contacts_app.contact_count; i++)
            if (contacts_app.contacts[i].id != contacts_app.selected.id) count++;
        return count;
    }
    case CONTACTS_HELP: return sizeof(contacts_help) / sizeof(contacts_help[0]);
    case CONTACTS_STATUS: return 8U;
    case CONTACTS_FILTER: return sizeof(contacts_filters) / sizeof(contacts_filters[0]);
    case CONTACTS_CONFIRM_LINK: return 5U;
    case CONTACTS_CONFIRM_REMOVE: return 2U;
    case CONTACTS_NOTICE: return 1U;
    default: return 0U;
    }
}

static void contacts_app_menu_line(size_t item, char *text, size_t capacity)
{
    text[0] = '\0';
    switch (contacts_app.mode) {
    case CONTACTS_ACTIONS: strlcpy(text, contacts_actions[item], capacity); break;
    case CONTACTS_HELP: strlcpy(text, contacts_help[item], capacity); break;
    case CONTACTS_FILTER: strlcpy(text, contacts_filters[item], capacity); break;
    case CONTACTS_ENDPOINTS:
        if (item == 0U) strlcpy(text, "All endpoints", capacity);
        else {
            const solar_os_endpoint_t *endpoint = &contacts_app.endpoints[item - 1U];
            snprintf(text, capacity, "%" PRIu32 " %s %s", endpoint->id,
                solar_os_messaging_provider_name(endpoint->provider),
                solar_os_contact_trust_name(endpoint->trust));
        }
        break;
    case CONTACTS_LINK: {
        const solar_os_contact_t *source = contacts_app_link_contact(item);
        if (source) snprintf(text, capacity, "%" PRIu32 " %s", source->id, source->display_name);
        break;
    }
    case CONTACTS_CONFIRM_LINK:
        if (item == 0U) strlcpy(text, "Move endpoints from:", capacity);
        else if (item == 1U) snprintf(text, capacity, "%" PRIu32 " %s",
            contacts_app.link_source, contacts_app.link_name);
        else if (item == 2U) strlcpy(text, "Into:", capacity);
        else if (item == 3U) snprintf(text, capacity, "%" PRIu32 " %s",
            contacts_app.selected.id, contacts_app.selected.display_name);
        else strlcpy(text, "Source gets removed", capacity);
        break;
    case CONTACTS_CONFIRM_REMOVE:
        if (item == 0U) snprintf(text, capacity, "%" PRIu32 " %s",
            contacts_app.selected.id, contacts_app.selected.display_name);
        else strlcpy(text, "And all its endpoints", capacity);
        break;
    case CONTACTS_NOTICE: strlcpy(text, contacts_app.notice, capacity); break;
    case CONTACTS_STATUS: {
        solar_os_contacts_status_t status;
        const esp_err_t error = solar_os_contacts_get_status(&status);
        if (error != ESP_OK) {
            if (item == 0U) snprintf(text, capacity, "Unavailable: %s", esp_err_to_name(error));
            break;
        }
        switch (item) {
        case 0: snprintf(text, capacity, "Contacts: %u/%u", (unsigned)status.contact_count,
            (unsigned)status.contact_capacity); break;
        case 1: snprintf(text, capacity, "Endpoints: %u/%u", (unsigned)status.endpoint_count,
            (unsigned)status.endpoint_capacity); break;
        case 2: snprintf(text, capacity, "Storage: %s", status.persistent ? "persistent" : "volatile"); break;
        case 3: snprintf(text, capacity, "PSRAM: %s", status.records_in_psram ? "yes" : "no"); break;
        case 4: snprintf(text, capacity, "Store bytes: %u", (unsigned)status.storage_bytes); break;
        case 5: snprintf(text, capacity, "Generation: %" PRIu32, status.generation); break;
        case 6: snprintf(text, capacity, "Evicted: %" PRIu32, status.evicted); break;
        default: snprintf(text, capacity, "Storage error: %s", esp_err_to_name(status.storage_error)); break;
        }
        break;
    }
    default: break;
    }
}

static void contacts_app_render_menu(void)
{
    const size_t cols = solar_os_tui_cols(&contacts_app.tui);
    const size_t visible = contacts_app_visible_rows();
    const bool confirm = contacts_app.mode == CONTACTS_CONFIRM_REMOVE ||
                         contacts_app.mode == CONTACTS_CONFIRM_LINK;
    const bool select = contacts_app.mode == CONTACTS_ACTIONS || contacts_app.mode == CONTACTS_ENDPOINTS ||
                        contacts_app.mode == CONTACTS_LINK || contacts_app.mode == CONTACTS_FILTER;
    const char *title = "Contacts";
    switch (contacts_app.mode) {
    case CONTACTS_ACTIONS: title = "Actions"; break;
    case CONTACTS_ENDPOINTS:
        title = contacts_app.pending_trust == SOLAR_OS_CONTACT_TRUST_BLOCKED ? "Block" : "Trust";
        break;
    case CONTACTS_RENAME: title = "Rename"; break;
    case CONTACTS_LINK: title = "Link into"; break;
    case CONTACTS_CONFIRM_LINK: title = "Confirm link"; break;
    case CONTACTS_CONFIRM_REMOVE: title = "Confirm removal"; break;
    case CONTACTS_HELP: title = "Contacts help"; break;
    case CONTACTS_STATUS: title = "Contacts status"; break;
    case CONTACTS_FILTER: title = "Filter by trust"; break;
    case CONTACTS_NOTICE: title = "Contact result"; break;
    default: break;
    }
    char heading[CONTACTS_APP_LINE_MAX];
    if (contacts_app.mode == CONTACTS_ACTIONS || contacts_app.mode == CONTACTS_ENDPOINTS ||
        contacts_app.mode == CONTACTS_RENAME || contacts_app.mode == CONTACTS_LINK)
        snprintf(heading, sizeof(heading), "%s #%" PRIu32 " %s", title,
            contacts_app.selected.id, contacts_app.selected.display_name);
    else strlcpy(heading, title, sizeof(heading));
    solar_os_tui_draw_title(&contacts_app.tui, heading, NULL);
    if (contacts_app.mode == CONTACTS_RENAME) {
        contacts_app_help_bar(cols < 32U ? "Enter save Esc back" : "Enter save  Esc cancel");
        if (visible > 0U)
            solar_os_tui_draw_input(&contacts_app.tui, 1U, 0U, cols, "Name: ",
                contacts_app.edit, &contacts_app.input, SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }
    const size_t count = contacts_app_menu_count();
    solar_os_tui_viewport_reconcile(&contacts_app.menu, count, visible);
    for (size_t row = 0; row < visible; row++) {
        const size_t item = contacts_app.menu.top + row;
        char text[CONTACTS_APP_LINE_MAX] = "";
        if (item < count) contacts_app_menu_line(item, text, sizeof(text));
        else if (row == 0U && count == 0U) strlcpy(text, "No other contacts", sizeof(text));
        solar_os_tui_write_cell(&contacts_app.tui, row + 1U, 0U, cols, text,
            select && item < count && item == contacts_app.menu.cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    contacts_app_help_bar(contacts_app.mode == CONTACTS_NOTICE ? "Enter/Esc back" :
        confirm ? (cols < 32U ? "Y yes N/Esc back" : "Y confirm  N/Esc cancel") :
        select ? (cols < 32U ? "Arrows Enter Esc" : "Up/Down Enter select Esc back") :
        (cols < 32U ? "Arrows Esc back" : "Up/Down scroll  Esc back"));
}

static void contacts_app_render(void)
{
    solar_os_tui_clear(&contacts_app.tui);
    /* Active input widgets draw last and restore their cursor. */
    solar_os_tui_set_cursor_visible(&contacts_app.tui, false);
    if (contacts_app.mode != CONTACTS_BROWSE) {
        contacts_app_render_menu();
    } else if (contacts_app.detail) {
        contacts_app_render_detail();
    } else {
        contacts_app_render_list();
    }
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

static void contacts_app_sync_detail(void)
{
    if (!contacts_app.detail) return;
    if (solar_os_contacts_get(contacts_app.selected.id, &contacts_app.selected) != ESP_OK) {
        contacts_app.detail = false;
        return;
    }
    contacts_app.endpoint_count = solar_os_contacts_endpoint_snapshot(contacts_app.selected.id,
        contacts_app.endpoints, SOLAR_OS_ENDPOINT_CAPACITY);
    const size_t total = contacts_app_detail_lines();
    const size_t visible = contacts_app_visible_rows();
    const size_t maximum = total > visible ? total - visible : 0U;
    if (contacts_app.detail_scroll > maximum) contacts_app.detail_scroll = maximum;
}

static void contacts_app_mode(contacts_app_mode_t mode)
{
    contacts_app.mode = mode;
    contacts_app.menu = (solar_os_tui_viewport_t){0};
}

static void contacts_app_back(void)
{
    contacts_app_mode(CONTACTS_BROWSE);
    contacts_app_refresh();
    contacts_app_sync_detail();
}

static void contacts_app_result(const char *operation, esp_err_t error)
{
    contacts_app_back();
    snprintf(contacts_app.notice, sizeof(contacts_app.notice), "%s: %s", operation,
        error == ESP_OK ? "done" : esp_err_to_name(error));
    contacts_app_mode(CONTACTS_NOTICE);
}

static void contacts_app_action(solar_os_context_t *ctx, uint8_t ch)
{
    (void)ctx;
    /* Freeze the contact ID before sorting, filtering, or provider updates. */
    if (contacts_app.mode == CONTACTS_BROWSE && !contacts_app.detail) {
        if (contacts_app.cursor >= contacts_app.visible_count) return;
        contacts_app.selected = contacts_app.contacts[contacts_app.visible[contacts_app.cursor]];
    }
#if SOLAR_OS_PACKAGE_APP_CHAT
    if (ch == 'c') {
        solar_os_conversation_id_t conversation = SOLAR_OS_CONVERSATION_ID_NONE;
        esp_err_t error = solar_os_messaging_direct_open(contacts_app.selected.id,
                                                         &conversation);
        if (error == ESP_OK) {
            char app[] = "chat", selector[16];
            snprintf(selector, sizeof(selector), "%" PRIu32, conversation);
            char *args[] = {app, selector};
            error = solar_os_context_request_launch_ex(ctx, &solar_os_chat_app,
                2, args, SOLAR_OS_LAUNCH_CHILD_RETURN);
        }
        if (error == ESP_OK) contacts_app_back();
        else contacts_app_result("Chat", error);
        return;
    }
#endif
    if (ch == 'a') contacts_app_mode(CONTACTS_ACTIONS);
    else if (ch == 't' || ch == 'b') {
        contacts_app.pending_trust = ch == 'b' ? SOLAR_OS_CONTACT_TRUST_BLOCKED : SOLAR_OS_CONTACT_TRUST_TRUSTED;
        contacts_app.endpoint_count = solar_os_contacts_endpoint_snapshot(contacts_app.selected.id,
            contacts_app.endpoints, SOLAR_OS_ENDPOINT_CAPACITY);
        contacts_app_mode(CONTACTS_ENDPOINTS);
    } else if (ch == 'r') {
        strlcpy(contacts_app.edit, contacts_app.selected.display_name, sizeof(contacts_app.edit));
        contacts_app.input = (solar_os_tui_input_state_t){.cursor = strlen(contacts_app.edit)};
        contacts_app_mode(CONTACTS_RENAME);
    } else if (ch == 'l') contacts_app_mode(CONTACTS_LINK);
    else if (ch == 'd') contacts_app_mode(CONTACTS_CONFIRM_REMOVE);
}

static void contacts_app_menu_event(solar_os_context_t *ctx, uint8_t ch)
{
    if (contacts_app.mode == CONTACTS_RENAME) {
        const size_t cols = solar_os_tui_cols(&contacts_app.tui);
        const solar_os_tui_input_action_t action = solar_os_tui_input_key(contacts_app.edit,
            sizeof(contacts_app.edit), &contacts_app.input, ch == '\r' ? SOLAR_OS_KEY_ENTER : ch,
            cols > 6U ? cols - 6U : 1U);
        if (action == SOLAR_OS_TUI_INPUT_CANCEL) contacts_app_back();
        else if (action == SOLAR_OS_TUI_INPUT_SUBMIT)
            contacts_app_result("Rename", solar_os_contacts_rename(contacts_app.selected.id, contacts_app.edit));
        return;
    }
    const bool confirm = contacts_app.mode == CONTACTS_CONFIRM_REMOVE ||
                         contacts_app.mode == CONTACTS_CONFIRM_LINK;
    if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_LEFT ||
        (confirm && (ch == 'n' || ch == 'N'))) {
        contacts_app_back();
        return;
    }
    if (confirm && (ch == 'y' || ch == 'Y')) {
        if (contacts_app.mode == CONTACTS_CONFIRM_REMOVE) {
            const esp_err_t error = solar_os_contacts_remove(contacts_app.selected.id);
            if (error == ESP_OK) contacts_app.detail = false;
            contacts_app_result("Remove", error);
        } else contacts_app_result("Link", solar_os_contacts_link(contacts_app.selected.id, contacts_app.link_source));
        return;
    }
    if (contacts_app.mode == CONTACTS_ACTIONS && ch != 0 &&
        strchr(
#if SOLAR_OS_PACKAGE_APP_CHAT
            "c"
#endif
            "tbrld", ch)) {
        contacts_app_action(ctx, ch);
        return;
    }
    const bool enter = ch == '\r' || ch == '\n' || ch == SOLAR_OS_KEY_RIGHT;
    if (enter && contacts_app.mode == CONTACTS_NOTICE) {
        contacts_app_back();
        return;
    }
    const size_t count = contacts_app_menu_count();
    if (enter && contacts_app.menu.cursor < count) {
        const size_t item = contacts_app.menu.cursor;
        switch (contacts_app.mode) {
        case CONTACTS_ACTIONS: contacts_app_action(ctx, (uint8_t)contacts_actions[item][0]); break;
        case CONTACTS_ENDPOINTS:
            contacts_app_result(contacts_app.pending_trust == SOLAR_OS_CONTACT_TRUST_BLOCKED ? "Block" : "Trust",
                solar_os_contacts_set_trust(contacts_app.selected.id,
                item == 0U ? SOLAR_OS_ENDPOINT_ID_NONE : contacts_app.endpoints[item - 1U].id,
                contacts_app.pending_trust));
            break;
        case CONTACTS_LINK: {
            const solar_os_contact_t *source = contacts_app_link_contact(item);
            if (source) {
                contacts_app.link_source = source->id;
                strlcpy(contacts_app.link_name, source->display_name, sizeof(contacts_app.link_name));
                contacts_app_mode(CONTACTS_CONFIRM_LINK);
            }
            break;
        }
        case CONTACTS_FILTER:
            contacts_app.filter = (uint8_t)item;
            contacts_app.cursor = contacts_app.top = 0U;
            contacts_app_back();
            break;
        default: break;
        }
        return;
    }
    (void)solar_os_tui_viewport_key(&contacts_app.menu, ch, count,
        contacts_app_visible_rows(), false);
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
    if (contacts_app.mode == CONTACTS_BROWSE) {
        contacts_app_refresh();
        contacts_app_sync_detail();
    }
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
        if (contacts_app.mode == CONTACTS_BROWSE &&
            solar_os_contacts_get_status(&status) == ESP_OK &&
            status.generation != contacts_app.generation) {
            contacts_app_refresh();
            contacts_app_sync_detail();
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
    const solar_os_tui_screen_key_action_t screen_key = solar_os_tui_screen_key(&contacts_app.tui, ch);
    if (screen_key == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED) return true;
    if (screen_key == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED) {
        contacts_app_render();
        return true;
    }
    if (!contacts_app.searching && contacts_app.mode != CONTACTS_RENAME && (ch == 'q' || ch == 'Q')) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (contacts_app.mode != CONTACTS_BROWSE) {
        contacts_app_menu_event(ctx, ch);
        contacts_app_render();
        return true;
    }
    if (contacts_app.searching) {
        (void)contacts_app_search_event(ch);
        contacts_app_render();
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
    case '?': contacts_app_mode(CONTACTS_HELP); break;
    case 's': contacts_app_mode(CONTACTS_STATUS); break;
    case 'f':
        if (!contacts_app.detail) {
            contacts_app_mode(CONTACTS_FILTER);
            contacts_app.menu.cursor = contacts_app.filter;
        }
        break;
    case 'a': case 't': case 'b': case 'r': case 'l': case 'd':
#if SOLAR_OS_PACKAGE_APP_CHAT
    case 'c':
#endif
        contacts_app_action(ctx, ch);
        break;
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
    .summary = "provider-neutral address book",
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
