#include "solar_os_messages_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_chat_app.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_messaging.h"
#include "solar_os_terminal.h"
#include "solar_os_tui_widgets.h"

#define MESSAGES_PAGE_SIZE SOLAR_OS_MESSAGING_OUTBOX_CAPACITY
#define MESSAGES_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)

typedef enum { MESSAGES_INSTANCES, MESSAGES_CONNECTIONS, MESSAGES_BROWSER } messages_tab_t;
typedef enum { MESSAGES_CONVERSATIONS, MESSAGES_UNREAD, MESSAGES_OUTBOX, MESSAGES_HISTORY } messages_view_t;
typedef enum {
    MESSAGES_BROWSE, MESSAGES_FILTER, MESSAGES_CLASSES, MESSAGES_FORM,
    MESSAGES_EDIT_FIELD, MESSAGES_DETAIL, MESSAGES_CONNECTION_DETAIL,
    MESSAGES_CONFIRM_REMOVE, MESSAGES_CONFIRM_DELETE, MESSAGES_CONFIRM_CANCEL,
    MESSAGES_HELP, MESSAGES_ACTIONS,
} messages_mode_t;

typedef struct {
    solar_os_tui_t tui;
    messages_tab_t tab;
    messages_view_t view;
    messages_mode_t mode;
    solar_os_tui_viewport_t providers, browser, menu;
    solar_os_messaging_provider_id_t selected, form_instance, pending_instance;
    solar_os_conversation_id_t conversation;
    solar_os_message_key_t pending_message;
    solar_os_messaging_conversation_t conversations[SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY];
    union {
        solar_os_messaging_message_t messages[MESSAGES_PAGE_SIZE];
        solar_os_messaging_outbound_t outbox[MESSAGES_PAGE_SIZE];
    } page;
    solar_os_messaging_message_t detail;
    solar_os_messaging_settings_t settings;
    /* Job status is large. Keep diagnostics off the display task stack. */
    solar_os_job_status_t job;
    size_t count, offset;
    bool more, suspended, form_name_edit;
    uint32_t generation, refresh_at;
    char form_class[16], form_name[SOLAR_OS_MESSAGING_LABEL_MAX];
    unsigned form_index;
    size_t edit_field;
    solar_os_tui_input_state_t input;
    char edit[SOLAR_OS_MESSAGING_SETTING_MAX];
    char feedback[128];
} messages_app_state_t;

static void *messages_app_state;
#define messages_app (*(messages_app_state_t *)messages_app_state)

static const char *const messages_help[] = {
    "Tab: Instances / Connections / Messages", "Up/Down: select; Enter: open",
    "f: choose instance / All instances", "a: actions; Esc: back; q: quit",
    "Instances: n add; e edit; d remove", "Forms: Enter edit; s save; Esc discard",
    "Secret fields: unchanged keeps saved value", "Submit an empty secret to clear it",
    "Connections: c connect; x disconnect", "Enter shows worker status and errors",
    "Messages: v conversations/unread/outbox", "Enter conversation: browse history",
    "Enter message: read full text", "c: open existing Chat and return here",
    "d: delete one selected message", "x: cancel selected pending send",
    "r: mark selected conversation read", "Page Up: older; Page Down: newer",
    "Pending sends must be cancelled to delete", "Deletion affects local history only",
};

static size_t messages_rows(void)
{
    size_t rows = solar_os_tui_rows(&messages_app.tui);
    return rows > 5 ? rows - 5 : 1;
}

static void messages_result(const char *operation, esp_err_t error)
{
    snprintf(messages_app.feedback, sizeof(messages_app.feedback), "%s: %s",
        operation, error == ESP_OK ? "done" : esp_err_to_name(error));
}

static bool messages_provider_at(size_t index, solar_os_messaging_provider_status_t *status)
{
    solar_os_messaging_provider_id_t cursor = 0;
    while (solar_os_messaging_provider_next(&cursor, status))
        if (index-- == 0) return true;
    return false;
}

static size_t messages_provider_count(void)
{
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status;
    size_t count = 0;
    while (solar_os_messaging_provider_next(&cursor, &status)) count++;
    return count;
}

static void messages_select_provider(void)
{
    solar_os_messaging_provider_status_t status;
    if (messages_provider_at(messages_app.providers.cursor, &status)) messages_app.selected = status.id;
}

static void messages_align_provider(void)
{
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status;
    size_t index = 0;
    while (solar_os_messaging_provider_next(&cursor, &status)) {
        if (status.id == messages_app.selected) { messages_app.providers.cursor = index; return; }
        index++;
    }
    if (messages_app.selected) messages_app.selected = 0;
    solar_os_tui_viewport_reconcile(&messages_app.providers, index, messages_rows());
}

static void messages_refresh(void)
{
    messages_align_provider();
    if (messages_app.tab == MESSAGES_BROWSER) {
        solar_os_conversation_id_t selected_conversation = 0;
        solar_os_message_key_t selected_message = 0;
        if (messages_app.browser.cursor < messages_app.count) {
            if (messages_app.view == MESSAGES_CONVERSATIONS)
                selected_conversation = messages_app.conversations[messages_app.browser.cursor].id;
            else selected_message = messages_app.view == MESSAGES_OUTBOX ?
                messages_app.page.outbox[messages_app.browser.cursor].message_key :
                messages_app.page.messages[messages_app.browser.cursor].key;
        }
        messages_app.count = 0; messages_app.more = false;
        esp_err_t error = ESP_OK;
        if (messages_app.view == MESSAGES_CONVERSATIONS) {
            size_t count = solar_os_messaging_conversation_snapshot(messages_app.conversations,
                SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY);
            for (size_t i = 0; i < count; i++) {
                if (!messages_app.selected || messages_app.conversations[i].provider == messages_app.selected)
                    messages_app.conversations[messages_app.count++] = messages_app.conversations[i];
            }
        } else if (messages_app.view == MESSAGES_OUTBOX) {
            size_t count = solar_os_messaging_outbox_snapshot(messages_app.page.outbox, MESSAGES_PAGE_SIZE);
            for (size_t i = 0; i < count; i++) {
                if (!messages_app.selected || messages_app.page.outbox[i].provider == messages_app.selected)
                    messages_app.page.outbox[messages_app.count++] = messages_app.page.outbox[i];
            }
        } else {
            error = solar_os_messaging_message_page_filtered(
                messages_app.view == MESSAGES_HISTORY ? messages_app.conversation : 0,
                messages_app.selected, messages_app.view == MESSAGES_UNREAD, messages_app.offset,
                messages_app.page.messages, MESSAGES_PAGE_SIZE, &messages_app.count, &messages_app.more);
            if (error == ESP_OK && !messages_app.count && messages_app.offset) {
                messages_app.offset = messages_app.offset >= MESSAGES_PAGE_SIZE ? messages_app.offset - MESSAGES_PAGE_SIZE : 0;
                error = solar_os_messaging_message_page_filtered(
                    messages_app.view == MESSAGES_HISTORY ? messages_app.conversation : 0,
                    messages_app.selected, messages_app.view == MESSAGES_UNREAD, messages_app.offset,
                    messages_app.page.messages, MESSAGES_PAGE_SIZE, &messages_app.count, &messages_app.more);
            }
        }
        if (error != ESP_OK) messages_result("History", error);
        for (size_t i = 0; i < messages_app.count; i++) {
            if ((messages_app.view == MESSAGES_CONVERSATIONS && selected_conversation &&
                    messages_app.conversations[i].id == selected_conversation) ||
                (messages_app.view == MESSAGES_OUTBOX && selected_message && messages_app.page.outbox[i].message_key == selected_message) ||
                ((messages_app.view == MESSAGES_HISTORY || messages_app.view == MESSAGES_UNREAD) &&
                    selected_message && messages_app.page.messages[i].key == selected_message)) {
                messages_app.browser.cursor = i; break;
            }
        }
        solar_os_tui_viewport_reconcile(&messages_app.browser, messages_app.count, messages_rows());
    }
    solar_os_messaging_status_t status;
    if (solar_os_messaging_get_status(&status) == ESP_OK) messages_app.generation = status.generation;
}

/* Creation variants are supplied by providers; no protocol-specific UI switch. */
static size_t messages_class_choice(size_t wanted, char *label, size_t capacity, bool select)
{
    char cursor[16] = "";
    solar_os_messaging_class_status_t type;
    size_t count = 0;
    while (solar_os_messaging_class_next(cursor, &type)) {
        strlcpy(cursor, type.name, sizeof(cursor));
        if (!type.creatable) continue;
        for (unsigned form = 0; ; form++) {
            esp_err_t error = solar_os_messaging_settings_read(type.name, 0, form, &messages_app.settings);
            if (error != ESP_OK) break;
            if (count++ != wanted) continue;
            if (label) strlcpy(label, messages_app.settings.label, capacity);
            if (select) {
                strlcpy(messages_app.form_class, type.name, sizeof(messages_app.form_class));
                messages_app.form_index = form;
                return count;
            }
        }
    }
    return count;
}

static const char *messages_view_name(void)
{
    switch (messages_app.view) {
    case MESSAGES_CONVERSATIONS: return "Conversations";
    case MESSAGES_UNREAD: return "Unread";
    case MESSAGES_OUTBOX: return "Outbox";
    default: return "History";
    }
}

static bool messages_worker(const solar_os_messaging_settings_t *settings)
{
    if (!settings->worker[0]) return false;
    if (!settings->worker_is_resource)
        return solar_os_jobs_get_by_name(settings->worker, &messages_app.job);
    for (size_t i = 0; i < solar_os_jobs_count(); i++) {
        if (!solar_os_jobs_get(i, &messages_app.job)) continue;
        for (size_t r = 0; r < messages_app.job.resource_count; r++)
            if (!strcmp(messages_app.job.resources[r].name, settings->worker)) return true;
    }
    return false;
}

static const char *messages_connection_state(const solar_os_messaging_provider_status_t *status)
{
    if (status->last_error != ESP_OK) return "error";
    if (status->connected) return status->connection_oriented ? "connected" : "ready";
    return status->running ? (status->connection_oriented ? "connecting" : "starting") : "stopped";
}

static void messages_provider_line(size_t index, char *line, size_t capacity)
{
    solar_os_messaging_provider_status_t status;
    if (!messages_provider_at(index, &status)) { line[0] = 0; return; }
    if (messages_app.tab == MESSAGES_INSTANCES) {
        snprintf(line, capacity, "%s [%s]", status.name, status.class_name);
        return;
    }
    esp_err_t error = solar_os_messaging_settings_read(status.class_name, status.id, 0, &messages_app.settings);
    const char *state = messages_connection_state(&status);
    if (error == ESP_OK && !messages_app.settings.enabled) state = "disabled";
    else if (error == ESP_OK && !status.connected && !status.running &&
             (!messages_worker(&messages_app.settings) || messages_app.job.state != SOLAR_OS_JOB_RUNNING))
        state = "enabled, worker stopped";
    snprintf(line, capacity, "%s: %s%s%s", status.name, state,
        status.detail[0] ? " - " : "", status.detail);
}

static solar_os_message_key_t messages_selected_key(void)
{
    if (messages_app.mode == MESSAGES_DETAIL) return messages_app.detail.key;
    if (messages_app.tab != MESSAGES_BROWSER || messages_app.view == MESSAGES_CONVERSATIONS ||
        messages_app.browser.cursor >= messages_app.count) return 0;
    return messages_app.view == MESSAGES_OUTBOX ? messages_app.page.outbox[messages_app.browser.cursor].message_key :
        messages_app.page.messages[messages_app.browser.cursor].key;
}

static solar_os_conversation_id_t messages_selected_conversation(void)
{
    if (messages_app.mode == MESSAGES_DETAIL) return messages_app.detail.conversation_id;
    if (messages_app.browser.cursor >= messages_app.count) return 0;
    if (messages_app.view == MESSAGES_CONVERSATIONS) return messages_app.conversations[messages_app.browser.cursor].id;
    return messages_app.view == MESSAGES_OUTBOX ? messages_app.page.outbox[messages_app.browser.cursor].conversation_id :
        messages_app.page.messages[messages_app.browser.cursor].conversation_id;
}

static void messages_browser_line(size_t index, char *line, size_t capacity)
{
    if (messages_app.view == MESSAGES_CONVERSATIONS) {
        const solar_os_messaging_conversation_t *conversation = &messages_app.conversations[index];
        char label[SOLAR_OS_MESSAGING_TITLE_MAX];
        solar_os_messaging_conversation_label(conversation, label, sizeof(label));
        snprintf(line, capacity, "%s  %s  (%" PRIu32 " unread)", label,
            solar_os_messaging_provider_name(conversation->provider), conversation->unread_count);
    } else if (messages_app.view == MESSAGES_OUTBOX) {
        const solar_os_messaging_outbound_t *request = &messages_app.page.outbox[index];
        snprintf(line, capacity, "%s [%u attempts] %s", solar_os_messaging_provider_name(request->provider),
            request->attempts, request->body);
    } else {
        const solar_os_messaging_message_t *message = &messages_app.page.messages[index];
        snprintf(line, capacity, "%c %s %s: %s", message->unread ? '*' : ' ',
            message->direction == SOLAR_OS_MESSAGE_INBOUND ? "in" : solar_os_delivery_state_name(message->delivery),
            message->sender[0] ? message->sender : solar_os_messaging_provider_name(message->provider), message->body);
    }
    /* A row stays a single terminal line even when the message body is multiline. */
    for (char *p = line; *p; p++) if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
}

static const char *const messages_instance_actions[] = {"n Add instance", "e Edit settings", "d Remove instance"};
static const char *const messages_connection_actions[] = {"c Connect / enable", "x Disconnect / disable", "i Connection details"};
static const char *const messages_browser_actions[] = {"o Open selected item", "c Open Chat", "v Change message view",
    "d Delete selected message", "x Cancel pending send", "r Mark conversation read"};

static const char *const *messages_actions(size_t *count)
{
    if (messages_app.tab == MESSAGES_INSTANCES) { *count = 3; return messages_instance_actions; }
    if (messages_app.tab == MESSAGES_CONNECTIONS) { *count = 3; return messages_connection_actions; }
    *count = 6; return messages_browser_actions;
}

static size_t messages_menu_count(void)
{
    switch (messages_app.mode) {
    case MESSAGES_HELP: return sizeof(messages_help) / sizeof(messages_help[0]);
    case MESSAGES_FILTER: return messages_provider_count() + 1;
    case MESSAGES_CLASSES: return messages_class_choice(SIZE_MAX, NULL, 0, false);
    case MESSAGES_FORM:
    case MESSAGES_EDIT_FIELD: return messages_app.settings.count + (messages_app.form_instance ? 0 : 1) + 1;
    case MESSAGES_ACTIONS: { size_t count; (void)messages_actions(&count); return count; }
    default: return 0;
    }
}

static void messages_menu_line(size_t index, char *line, size_t capacity)
{
    line[0] = 0;
    if (messages_app.mode == MESSAGES_HELP) strlcpy(line, messages_help[index], capacity);
    else if (messages_app.mode == MESSAGES_ACTIONS) {
        size_t count; const char *const *actions = messages_actions(&count);
        if (index < count) strlcpy(line, actions[index], capacity);
    } else if (messages_app.mode == MESSAGES_FILTER) {
        solar_os_messaging_provider_status_t status;
        if (!index) strlcpy(line, "All instances", capacity);
        else if (messages_provider_at(index - 1, &status)) snprintf(line, capacity, "%s [%s]", status.name, status.class_name);
    } else if (messages_app.mode == MESSAGES_CLASSES) (void)messages_class_choice(index, line, capacity, false);
    else if (messages_app.mode == MESSAGES_FORM || messages_app.mode == MESSAGES_EDIT_FIELD) {
        if (index + 1 == messages_menu_count()) strlcpy(line, "[ Save ]", capacity);
        else if (!messages_app.form_instance && !index) snprintf(line, capacity, "Instance name: %s", messages_app.form_name);
        else {
            size_t field = index - (messages_app.form_instance ? 0 : 1);
            bool secret = messages_app.settings.fields[field].secret;
            const char *value = secret ?
                (messages_app.settings.changed[field] ? (messages_app.settings.values[field][0] ? "[changed]" : "[clear]") :
                messages_app.settings.present[field] ? "[saved]" : "[empty]") : messages_app.settings.values[field];
            snprintf(line, capacity, "%s: %s", messages_app.settings.fields[field].label, value);
        }
    }
}

static void messages_form_row(size_t index, char *line, size_t capacity, void *user)
{
    (void)user;
    messages_menu_line(index, line, capacity);
}

/* Wrap at UTF-8 boundaries; control characters in remote content are inert. */
static size_t messages_text_line(const char *text, size_t offset, size_t width, char *line, size_t capacity)
{
    size_t cells = 0, used = 0;
    while (text[offset] && text[offset] != '\n' && cells < width) {
        unsigned char ch = (unsigned char)text[offset];
        size_t bytes = 1;
        if (ch >= 0xc2 && ch <= 0xf4) {
            size_t expected = ch < 0xe0 ? 2 : ch < 0xf0 ? 3 : 4;
            while (bytes < expected && text[offset + bytes] &&
                ((unsigned char)text[offset + bytes] & 0xc0) == 0x80) bytes++;
            if (bytes != expected) bytes = 1;
        }
        if (used + bytes >= capacity) break;
        if (ch < 32 || ch == 127) line[used++] = ' ';
        else { memcpy(line + used, text + offset, bytes); used += bytes; }
        offset += bytes; cells++;
    }
    line[used] = 0;
    return text[offset] == '\n' ? offset + 1 : offset;
}

static size_t messages_detail_count(void)
{
    size_t count = 5, offset = 0;
    char line[MESSAGES_LINE_MAX];
    size_t width = solar_os_tui_cols(&messages_app.tui);
    if (!width) return count;
    while (messages_app.detail.body[offset]) {
        offset = messages_text_line(messages_app.detail.body, offset, width, line, sizeof(line));
        count++;
    }
    return count;
}

static void messages_detail_line(size_t index, char *line, size_t capacity, size_t width)
{
    const solar_os_messaging_message_t *message = &messages_app.detail;
    line[0] = 0;
    if (index == 0) snprintf(line, capacity, "ID %016" PRIx64 "  %s", message->key, solar_os_messaging_provider_name(message->provider));
    else if (index == 1) snprintf(line, capacity, "%s / %s  %s", message->direction == SOLAR_OS_MESSAGE_INBOUND ? "inbound" : "outbound",
        solar_os_delivery_state_name(message->delivery), message->unread ? "unread" : "read");
    else if (index == 2) snprintf(line, capacity, "From: %s", message->sender);
    else if (index == 3) snprintf(line, capacity, "Error: %s", message->error[0] ? message->error : "none");
    else if (index == 4) snprintf(line, capacity, "Time: %" PRIu64 " ms%s", message->timestamp_ms,
        message->truncated ? " [truncated]" : "");
    else {
        size_t offset = 0;
        for (size_t row = 5; row <= index && message->body[offset]; row++)
            offset = messages_text_line(message->body, offset, width, line, capacity);
        if (index > 5 && !message->body[0]) line[0] = 0;
    }
    if (index < 5) for (char *p = line; *p; p++) if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
}

static void messages_connection_line(size_t index, char *line, size_t capacity)
{
    solar_os_messaging_provider_status_t status;
    line[0] = 0;
    if (solar_os_messaging_provider_get_status(messages_app.selected, &status) != ESP_OK) {
        if (!index) strlcpy(line, "Instance unavailable", capacity);
        return;
    }
    esp_err_t error = solar_os_messaging_settings_read(status.class_name, status.id, 0, &messages_app.settings);
    bool worker = error == ESP_OK && messages_worker(&messages_app.settings);
    switch (index) {
    case 0: snprintf(line, capacity, "%s [%s]", status.name, status.class_name); break;
    case 1: snprintf(line, capacity, "Setting: %s", error != ESP_OK ? "unavailable" : messages_app.settings.enabled ? "enabled" : "disabled"); break;
    case 2: snprintf(line, capacity, "Transport: %s", messages_connection_state(&status)); break;
    case 3: snprintf(line, capacity, "Worker: %s", worker ? messages_app.job.name :
        error == ESP_OK && messages_app.settings.worker[0] ? messages_app.settings.worker : "unavailable"); break;
    case 4: snprintf(line, capacity, "Worker state: %s", worker ? solar_os_job_state_name(messages_app.job.state) : "not registered / transport absent"); break;
    case 5: snprintf(line, capacity, "Provider error: %s", esp_err_to_name(status.last_error)); break;
    case 6: snprintf(line, capacity, "Worker error: %s", worker ? esp_err_to_name(messages_app.job.last_error) : "unavailable"); break;
    case 7: snprintf(line, capacity, "Detail: %s", status.detail); break;
    case 8: strlcpy(line, "Connect enables this instance.", capacity); break;
    case 9: strlcpy(line, "Start its worker with job start.", capacity); break;
    default: break;
    }
}

static void messages_render(void)
{
    size_t rows = solar_os_tui_rows(&messages_app.tui), cols = solar_os_tui_cols(&messages_app.tui);
    solar_os_tui_clear(&messages_app.tui);
    solar_os_tui_set_cursor_visible(&messages_app.tui, false);
    if (rows < 7 || cols < 20) { solar_os_tui_draw_too_small(&messages_app.tui, "Messages"); solar_os_tui_refresh(&messages_app.tui); return; }
    char line[MESSAGES_LINE_MAX];
    solar_os_tui_draw_title(&messages_app.tui, "Messages", NULL);
    const char *tabs[] = {cols >= 38 ? "Instances" : "Inst", cols >= 38 ? "Connections" : "Conn", cols >= 38 ? "Messages" : "Msgs"};
    for (size_t i = 0; i < 3; i++) {
        size_t start = cols * i / 3, end = cols * (i + 1) / 3;
        solar_os_tui_draw_tab(&messages_app.tui, 1, start, end - start, tabs[i], (size_t)messages_app.tab == i);
    }
    const char *context = messages_app.selected ? solar_os_messaging_provider_name(messages_app.selected) : "All instances";
    snprintf(line, sizeof(line), "%s%s%s", context, messages_app.tab == MESSAGES_BROWSER ? " / " : "",
        messages_app.tab == MESSAGES_BROWSER ? messages_view_name() : "");
    solar_os_tui_write_cell(&messages_app.tui, 2, 0, cols, line, SOLAR_OS_TUI_ATTR_BOLD);
    if (messages_app.mode == MESSAGES_FORM || messages_app.mode == MESSAGES_EDIT_FIELD) {
        bool editing = messages_app.mode == MESSAGES_EDIT_FIELD;
        bool secret = editing && !messages_app.form_name_edit && messages_app.settings.fields[messages_app.edit_field].secret;
        char label[96] = "";
        const char *status = messages_app.feedback;
        if (editing) {
            snprintf(label, sizeof(label), "%s: ", messages_app.form_name_edit ? "Instance name" :
                messages_app.settings.fields[messages_app.edit_field].label);
            status = messages_app.form_name_edit ? "Instance name" : messages_app.settings.fields[messages_app.edit_field].hint;
        }
        solar_os_tui_write_cell(&messages_app.tui, rows - 2, 0, cols, status ? status : "", SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_write_cell(&messages_app.tui, rows - 1, 0, cols,
            editing ? "Enter apply field  Esc cancel" : "Enter edit  s Save  Esc discard", SOLAR_OS_TUI_ATTR_INVERSE);
        const solar_os_tui_form_t form = {
            .bounds = {.row = 3, .col = 0, .height = messages_rows(), .width = cols},
            .viewport = &messages_app.menu, .count = messages_menu_count(),
            .row = messages_form_row, .empty_text = "No fields",
            .edit_label = label, .edit_text = messages_app.edit,
            .edit_state = editing ? &messages_app.input : NULL, .masked = secret,
        };
        (void)solar_os_tui_draw_form(&messages_app.tui, &form);
        solar_os_tui_refresh(&messages_app.tui);
        return;
    }
    const char *help = "a Actions  ? Help  Tab tabs";
    bool confirm = messages_app.mode == MESSAGES_CONFIRM_DELETE || messages_app.mode == MESSAGES_CONFIRM_REMOVE || messages_app.mode == MESSAGES_CONFIRM_CANCEL;
    solar_os_tui_viewport_t *viewport = &messages_app.menu;
    size_t count = 0;
    if (messages_app.mode == MESSAGES_BROWSE) {
        viewport = messages_app.tab == MESSAGES_BROWSER ? &messages_app.browser : &messages_app.providers;
        count = messages_app.tab == MESSAGES_BROWSER ? messages_app.count : messages_provider_count();
    } else if (messages_app.mode == MESSAGES_CONNECTION_DETAIL) count = 10;
    else if (messages_app.mode == MESSAGES_DETAIL) count = messages_detail_count();
    else if (confirm) count = messages_app.mode == MESSAGES_CONFIRM_REMOVE ? 4 : 3;
    else count = messages_menu_count();
    solar_os_tui_viewport_reconcile(viewport, count, messages_rows());
    for (size_t row = 0; row < messages_rows(); row++) {
        size_t index = viewport->top + row;
        line[0] = 0;
        if (index < count) {
            if (messages_app.mode == MESSAGES_BROWSE) {
                if (messages_app.tab == MESSAGES_BROWSER) messages_browser_line(index, line, sizeof(line));
                else messages_provider_line(index, line, sizeof(line));
            } else if (messages_app.mode == MESSAGES_DETAIL) messages_detail_line(index, line, sizeof(line), cols);
            else if (messages_app.mode == MESSAGES_CONNECTION_DETAIL) messages_connection_line(index, line, sizeof(line));
            else if (confirm) {
                if (index == 0) strlcpy(line, messages_app.mode == MESSAGES_CONFIRM_REMOVE ? "Remove instance?" :
                    messages_app.mode == MESSAGES_CONFIRM_DELETE ? "Delete this local message?" : "Cancel this pending send?", sizeof(line));
                else if (index == 1 && messages_app.mode == MESSAGES_CONFIRM_REMOVE) snprintf(line, sizeof(line), "%s", solar_os_messaging_provider_name(messages_app.pending_instance));
                else if (index == 1) snprintf(line, sizeof(line), "%016" PRIx64, messages_app.pending_message);
                else if (index == 2 && messages_app.mode == MESSAGES_CONFIRM_REMOVE) strlcpy(line, "Deletes history, settings, credentials", sizeof(line));
                else strlcpy(line, messages_app.mode == MESSAGES_CONFIRM_REMOVE ? "and queued sends. y confirms; n cancels" : "y confirms; n cancels", sizeof(line));
            } else messages_menu_line(index, line, sizeof(line));
        } else if (!count && !row) strlcpy(line, "No items", sizeof(line));
        bool selected = index == viewport->cursor && index < count && !confirm &&
            messages_app.mode != MESSAGES_DETAIL && messages_app.mode != MESSAGES_CONNECTION_DETAIL && messages_app.mode != MESSAGES_HELP;
        solar_os_tui_write_cell(&messages_app.tui, row + 3, 0, cols, line,
            selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (messages_app.mode == MESSAGES_DETAIL) help = "d Delete  c Chat  Esc back";
    else if (confirm) help = "y Confirm  n/Esc cancel";
    else if (messages_app.mode != MESSAGES_BROWSE) help = "Up/Down  Enter select  Esc back";
    solar_os_tui_write_cell(&messages_app.tui, rows - 2, 0, cols, messages_app.feedback, SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_write_cell(&messages_app.tui, rows - 1, 0, cols, help, SOLAR_OS_TUI_ATTR_INVERSE);
    solar_os_tui_refresh(&messages_app.tui);
}

static void messages_mode(messages_mode_t mode)
{
    messages_app.mode = mode;
    messages_app.menu = (solar_os_tui_viewport_t){0};
}

static void messages_reset_browser(void)
{
    messages_app.browser = (solar_os_tui_viewport_t){0};
    messages_app.offset = messages_app.count = 0;
    messages_refresh();
}

static void messages_open_form(bool create)
{
    if (create) { messages_mode(MESSAGES_CLASSES); return; }
    messages_select_provider();
    solar_os_messaging_provider_status_t status;
    esp_err_t error = solar_os_messaging_provider_get_status(messages_app.selected, &status);
    if (error == ESP_OK) {
        messages_app.form_instance = status.id;
        messages_app.form_index = 0;
        strlcpy(messages_app.form_name, status.name, sizeof(messages_app.form_name));
        strlcpy(messages_app.form_class, status.class_name, sizeof(messages_app.form_class));
        error = solar_os_messaging_settings_read(status.class_name, status.id, 0, &messages_app.settings);
    }
    if (error == ESP_OK) messages_mode(MESSAGES_FORM);
    else messages_result("Settings", error);
}

static void messages_save_form(void)
{
    solar_os_messaging_provider_id_t id = messages_app.form_instance;
    esp_err_t error = solar_os_messaging_settings_write(messages_app.form_class, id, messages_app.form_index,
        messages_app.form_name, &messages_app.settings, &id);
    messages_result("Save", error);
    if (error == ESP_OK) {
        solar_os_messaging_settings_clear(&messages_app.settings);
        messages_app.selected = id; messages_mode(MESSAGES_BROWSE); messages_refresh();
    }
}

static void messages_open(solar_os_context_t *ctx)
{
    (void)ctx;
    if (messages_app.tab == MESSAGES_INSTANCES) { messages_open_form(false); return; }
    if (messages_app.tab == MESSAGES_CONNECTIONS) {
        messages_select_provider(); messages_mode(MESSAGES_CONNECTION_DETAIL); return;
    }
    if (messages_app.browser.cursor >= messages_app.count) return;
    if (messages_app.view == MESSAGES_CONVERSATIONS) {
        messages_app.conversation = messages_selected_conversation();
        messages_app.view = MESSAGES_HISTORY; messages_reset_browser();
    } else {
        esp_err_t error = solar_os_messaging_message_get(messages_selected_key(), &messages_app.detail);
        if (error == ESP_OK) messages_mode(MESSAGES_DETAIL);
        else messages_result("Open", error);
    }
}

static void messages_chat(solar_os_context_t *ctx)
{
    char app[] = "chat", selector[SOLAR_OS_MESSAGING_LABEL_MAX];
    char *args[] = {app, selector};
    solar_os_conversation_id_t conversation = messages_selected_conversation();
    if (conversation) snprintf(selector, sizeof(selector), "%" PRIu32, conversation);
    else if (messages_app.selected) strlcpy(selector, solar_os_messaging_provider_name(messages_app.selected), sizeof(selector));
    else selector[0] = 0;
    esp_err_t error = solar_os_context_request_launch_ex(ctx, &solar_os_chat_app, selector[0] ? 2 : 1,
        args, SOLAR_OS_LAUNCH_CHILD_RETURN);
    if (error != ESP_OK) messages_result("Chat", error);
}

static void messages_action(solar_os_context_t *ctx, uint8_t ch)
{
    if (ch == 'o' || ch == '\r' || ch == '\n') { messages_open(ctx); return; }
    if (messages_app.tab == MESSAGES_INSTANCES) {
        if (ch == 'n') messages_open_form(true);
        else if (ch == 'e') messages_open_form(false);
        else if (ch == 'd') {
            messages_select_provider();
            if (messages_app.selected) { messages_app.pending_instance = messages_app.selected; messages_mode(MESSAGES_CONFIRM_REMOVE); }
        }
    } else if (messages_app.tab == MESSAGES_CONNECTIONS) {
        if (ch == 'c' || ch == 'x') {
            messages_select_provider();
            messages_result(ch == 'c' ? "Enable" : "Disable", solar_os_messaging_connect(messages_app.selected, ch == 'c'));
        } else if (ch == 'i') messages_open(ctx);
    } else {
        if (ch == 'c') messages_chat(ctx);
        else if (ch == 'v') {
            messages_app.view = messages_app.view == MESSAGES_CONVERSATIONS ? MESSAGES_UNREAD :
                messages_app.view == MESSAGES_UNREAD ? MESSAGES_OUTBOX : MESSAGES_CONVERSATIONS;
            messages_reset_browser();
        } else if (ch == 'r') {
            solar_os_conversation_id_t conversation = messages_selected_conversation();
            if (conversation) messages_result("Read", solar_os_messaging_mark_read(conversation));
            messages_refresh();
        } else if (ch == 'd' || ch == 'x') {
            solar_os_message_key_t key = messages_selected_key();
            if (key) {
                messages_app.pending_message = key;
                messages_mode(ch == 'd' ? MESSAGES_CONFIRM_DELETE : MESSAGES_CONFIRM_CANCEL);
            }
        }
    }
}

static void messages_menu_event(solar_os_context_t *ctx, uint8_t ch)
{
    bool enter = ch == '\r' || ch == '\n';
    if (messages_app.mode == MESSAGES_EDIT_FIELD) {
        size_t capacity = messages_app.form_name_edit ? sizeof(messages_app.form_name) :
            messages_app.settings.fields[messages_app.edit_field].capacity;
        size_t cols = solar_os_tui_cols(&messages_app.tui);
        solar_os_tui_input_action_t action = solar_os_tui_input_key(messages_app.edit, capacity,
            &messages_app.input, enter ? SOLAR_OS_KEY_ENTER : ch, cols > 2 ? cols - 2 : 1);
        if (action == SOLAR_OS_TUI_INPUT_SUBMIT) {
            if (messages_app.form_name_edit) strlcpy(messages_app.form_name, messages_app.edit, sizeof(messages_app.form_name));
            else {
                strlcpy(messages_app.settings.values[messages_app.edit_field], messages_app.edit,
                    sizeof(messages_app.settings.values[0]));
                messages_app.settings.changed[messages_app.edit_field] = true;
            }
        }
        if (action == SOLAR_OS_TUI_INPUT_SUBMIT || action == SOLAR_OS_TUI_INPUT_CANCEL) {
            memset(messages_app.edit, 0, sizeof(messages_app.edit)); messages_app.mode = MESSAGES_FORM;
        }
        return;
    }
    bool confirm = messages_app.mode == MESSAGES_CONFIRM_REMOVE || messages_app.mode == MESSAGES_CONFIRM_DELETE || messages_app.mode == MESSAGES_CONFIRM_CANCEL;
    if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_LEFT || (confirm && (ch == 'n' || ch == 'N'))) {
        solar_os_messaging_settings_clear(&messages_app.settings);
        messages_mode(MESSAGES_BROWSE); messages_refresh(); return;
    }
    if (confirm && (ch == 'y' || ch == 'Y')) {
        esp_err_t error;
        if (messages_app.mode == MESSAGES_CONFIRM_REMOVE) error = solar_os_messaging_instance_remove(messages_app.pending_instance);
        else if (messages_app.mode == MESSAGES_CONFIRM_DELETE) error = solar_os_messaging_message_delete(messages_app.pending_message);
        else error = solar_os_messaging_cancel(messages_app.pending_message);
        if (error == ESP_ERR_INVALID_STATE && messages_app.mode == MESSAGES_CONFIRM_DELETE)
            strlcpy(messages_app.feedback, "Message pending; cancel its send first", sizeof(messages_app.feedback));
        else messages_result("Update", error);
        messages_mode(MESSAGES_BROWSE); messages_refresh(); return;
    }
    if (confirm) return;
    if (messages_app.mode == MESSAGES_DETAIL) {
        if (ch == 'd' || ch == 'x' || ch == 'c' || ch == 'r') {
            messages_action(ctx, ch);
            if (messages_app.mode == MESSAGES_DETAIL && ch == 'r')
                (void)solar_os_messaging_message_get(messages_app.detail.key, &messages_app.detail);
            return;
        }
    } else if (messages_app.mode == MESSAGES_CONNECTION_DETAIL) {
        if (ch == 'c' || ch == 'x') { messages_action(ctx, ch); return; }
    } else if (messages_app.mode == MESSAGES_FORM && ch == 's') { messages_save_form(); return; }
    else if (enter) {
        size_t index = messages_app.menu.cursor;
        if (index >= messages_menu_count()) return;
        if (messages_app.mode == MESSAGES_FILTER) {
            solar_os_messaging_provider_status_t status;
            if (!index) messages_app.selected = 0;
            else if (messages_provider_at(index - 1, &status)) messages_app.selected = status.id;
            messages_mode(MESSAGES_BROWSE);
            messages_app.view = MESSAGES_CONVERSATIONS; messages_reset_browser();
        } else if (messages_app.mode == MESSAGES_CLASSES) {
            messages_app.form_instance = 0; messages_app.form_name[0] = 0;
            (void)messages_class_choice(index, NULL, 0, true); messages_mode(MESSAGES_FORM);
        } else if (messages_app.mode == MESSAGES_FORM) {
            if (index + 1 == messages_menu_count()) { messages_save_form(); return; }
            messages_app.form_name_edit = !messages_app.form_instance && index == 0;
            messages_app.edit_field = index - (messages_app.form_instance ? 0 : 1);
            bool secret = !messages_app.form_name_edit && messages_app.settings.fields[messages_app.edit_field].secret;
            strlcpy(messages_app.edit, messages_app.form_name_edit ? messages_app.form_name :
                secret ? "" : messages_app.settings.values[messages_app.edit_field], sizeof(messages_app.edit));
            messages_app.input = (solar_os_tui_input_state_t){.cursor = strlen(messages_app.edit)};
            messages_app.mode = MESSAGES_EDIT_FIELD;
        } else if (messages_app.mode == MESSAGES_ACTIONS) {
            size_t count; const char *const *actions = messages_actions(&count);
            uint8_t action = (uint8_t)actions[index][0];
            messages_mode(MESSAGES_BROWSE); messages_action(ctx, action);
        }
        return;
    }
    size_t count = messages_app.mode == MESSAGES_DETAIL ? messages_detail_count() :
        messages_app.mode == MESSAGES_CONNECTION_DETAIL ? 10 : messages_menu_count();
    (void)solar_os_tui_viewport_key(&messages_app.menu, ch, count, messages_rows(), false);
}

static esp_err_t messages_start(solar_os_context_t *ctx)
{
    memset(&messages_app, 0, sizeof(messages_app));
    esp_err_t error = solar_os_messaging_init();
    if (error == ESP_OK) error = solar_os_tui_screen_begin(&messages_app.tui, ctx);
    if (error != ESP_OK) return error;
    messages_refresh(); messages_render(); return ESP_OK;
}

static void messages_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_messaging_settings_clear(&messages_app.settings);
    memset(messages_app.edit, 0, sizeof(messages_app.edit));
    solar_os_tui_set_cursor_visible(&messages_app.tui, true);
    solar_os_tui_end(&messages_app.tui);
}

static void messages_suspend(solar_os_context_t *ctx)
{ (void)ctx; messages_app.suspended = true; }

static void messages_resume(solar_os_context_t *ctx)
{
    (void)ctx; messages_app.suspended = false;
    if (messages_app.mode == MESSAGES_BROWSE) messages_refresh();
    else if (messages_app.mode == MESSAGES_DETAIL &&
        solar_os_messaging_message_get(messages_app.detail.key, &messages_app.detail) != ESP_OK) {
        messages_mode(MESSAGES_BROWSE); messages_refresh();
    }
    messages_render();
}

static bool messages_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (!event || messages_app.suspended) return false;
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if ((int32_t)(event->data.tick_ms - messages_app.refresh_at) >= 0 && messages_app.mode == MESSAGES_BROWSE) {
            messages_app.refresh_at = event->data.tick_ms + 1000;
            solar_os_messaging_status_t status;
            if (solar_os_messaging_get_status(&status) == ESP_OK && status.generation != messages_app.generation)
                messages_refresh();
            messages_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) return false;
    uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) { solar_os_context_finish(ctx, 0, NULL); return true; }
    solar_os_tui_screen_key_action_t screen = solar_os_tui_screen_key(&messages_app.tui, ch);
    if (screen == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED) return true;
    if (screen == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED) { messages_render(); return true; }
    if (messages_app.mode != MESSAGES_BROWSE) { messages_menu_event(ctx, ch); messages_render(); return true; }
    if (ch == 'q') { solar_os_context_finish(ctx, 0, NULL); return true; }
    if (ch == '\t') {
        messages_app.tab = (messages_tab_t)((messages_app.tab + 1) % 3);
        messages_refresh();
    } else if (ch == '?') messages_mode(MESSAGES_HELP);
    else if (ch == 'a') messages_mode(MESSAGES_ACTIONS);
    else if (ch == 'f') messages_mode(MESSAGES_FILTER);
    else if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_LEFT) {
        if (messages_app.tab == MESSAGES_BROWSER && messages_app.view == MESSAGES_HISTORY) {
            messages_app.view = MESSAGES_CONVERSATIONS; messages_reset_browser();
        } else solar_os_context_finish(ctx, 0, NULL);
    } else if (messages_app.tab == MESSAGES_BROWSER &&
        (messages_app.view == MESSAGES_HISTORY || messages_app.view == MESSAGES_UNREAD) &&
        (ch == SOLAR_OS_KEY_PAGE_UP || ch == SOLAR_OS_KEY_PAGE_DOWN)) {
        if (ch == SOLAR_OS_KEY_PAGE_UP && messages_app.more) messages_app.offset += MESSAGES_PAGE_SIZE;
        else if (ch == SOLAR_OS_KEY_PAGE_DOWN && messages_app.offset)
            messages_app.offset = messages_app.offset >= MESSAGES_PAGE_SIZE ? messages_app.offset - MESSAGES_PAGE_SIZE : 0;
        messages_app.count = 0; messages_app.browser = (solar_os_tui_viewport_t){0}; messages_refresh();
    } else {
        solar_os_tui_viewport_t *viewport = messages_app.tab == MESSAGES_BROWSER ? &messages_app.browser : &messages_app.providers;
        size_t count = messages_app.tab == MESSAGES_BROWSER ? messages_app.count : messages_provider_count();
        if (solar_os_tui_viewport_key(viewport, ch, count, messages_rows(), false)) {
            if (messages_app.tab != MESSAGES_BROWSER) messages_select_provider();
        } else messages_action(ctx, ch);
    }
    messages_render(); return true;
}

const solar_os_app_t solar_os_messages_app = {
    .name = "messages", .summary = "manage messaging instances, connections, and history",
    .app_class = SOLAR_OS_APP_CLASS_TUI, .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = messages_start, .stop = messages_stop, .suspend = messages_suspend,
    .resume = messages_resume, .event = messages_event,
    .state_slot = &messages_app_state, .state_size = sizeof(messages_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_REQUIRED,
};
