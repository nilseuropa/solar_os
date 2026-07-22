#include "solar_os_inbox_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "solar_os_inbox.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"

#define INBOX_APP_DETAIL_TEXT_MAX 640U
#define INBOX_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)
#define INBOX_APP_RECEPTION_TEXT_MAX 17U
#define INBOX_APP_SOURCE_TEXT_MAX \
    (SOLAR_OS_INBOX_SOURCE_MAX + SOLAR_OS_INBOX_TOPIC_MAX + 1U)
#define INBOX_APP_EPOCH_MIN_MS 1577836800000ULL

typedef enum {
    INBOX_APP_LIST,
    INBOX_APP_DETAIL,
} inbox_app_view_t;

typedef struct {
    solar_os_tui_t tui;
    solar_os_inbox_entry_t *entries;
    size_t count;
    size_t total;
    size_t cursor;
    size_t top;
    bool unread_only;
    inbox_app_view_t view;
    solar_os_inbox_entry_t detail;
    char detail_text[INBOX_APP_DETAIL_TEXT_MAX];
    size_t detail_scroll;
    solar_os_inbox_status_t status;
} inbox_app_state_t;

static inbox_app_state_t inbox_app;

static size_t inbox_app_utf8_char_len(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    const unsigned char ch = (unsigned char)text[0];
    if (ch < 0x80U) {
        return 1;
    }
    if ((ch & 0xe0U) == 0xc0U && text[1] != '\0' &&
        ((unsigned char)text[1] & 0xc0U) == 0x80U) {
        return 2;
    }
    if ((ch & 0xf0U) == 0xe0U && text[1] != '\0' && text[2] != '\0' &&
        ((unsigned char)text[1] & 0xc0U) == 0x80U &&
        ((unsigned char)text[2] & 0xc0U) == 0x80U) {
        return 3;
    }
    if ((ch & 0xf8U) == 0xf0U && text[1] != '\0' && text[2] != '\0' &&
        text[3] != '\0' &&
        ((unsigned char)text[1] & 0xc0U) == 0x80U &&
        ((unsigned char)text[2] & 0xc0U) == 0x80U &&
        ((unsigned char)text[3] & 0xc0U) == 0x80U) {
        return 4;
    }
    return 1;
}

static size_t inbox_app_clip_len(const char *text, size_t max_cols, size_t max_bytes)
{
    size_t bytes = 0;
    size_t cols = 0;

    if (text == NULL) {
        return 0;
    }
    while (text[bytes] != '\0' && cols < max_cols) {
        const size_t char_len = inbox_app_utf8_char_len(text + bytes);
        if (char_len == 0 || bytes + char_len > max_bytes) {
            break;
        }
        bytes += char_len;
        cols++;
    }
    return bytes;
}

static void inbox_app_write_cell(size_t row,
                                 size_t col,
                                 size_t width,
                                 const char *text,
                                 uint8_t attr)
{
    const size_t rows = solar_os_tui_rows(&inbox_app.tui);
    const size_t cols = solar_os_tui_cols(&inbox_app.tui);
    char clipped[INBOX_APP_LINE_MAX];

    if (row >= rows || col >= cols || width == 0) {
        return;
    }
    if (col + width > cols) {
        width = cols - col;
    }

    solar_os_tui_fill(&inbox_app.tui, row, col, 1, width, ' ', attr);
    const size_t copy_len = inbox_app_clip_len(text, width, sizeof(clipped) - 1U);
    memcpy(clipped, text, copy_len);
    clipped[copy_len] = '\0';
    if (copy_len > 0) {
        solar_os_tui_addstr(&inbox_app.tui, row, col, clipped, attr);
    }
}

static void inbox_app_flatten(char *text)
{
    if (text == NULL) {
        return;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\r' || text[i] == '\n' || text[i] == '\t') {
            text[i] = ' ';
        }
    }
}

static const char *inbox_app_summary(const solar_os_inbox_entry_t *entry)
{
    if (entry->body[0] != '\0') {
        return entry->body;
    }
    return entry->title;
}

static void inbox_app_format_reception(const solar_os_inbox_entry_t *entry,
                                       char *text,
                                       size_t text_len)
{
    if (text == NULL || text_len == 0) {
        return;
    }
    if (entry == NULL || entry->timestamp_ms < INBOX_APP_EPOCH_MIN_MS) {
        strlcpy(text, "---- -- -- --:--", text_len);
        return;
    }

    const time_t seconds = (time_t)(entry->timestamp_ms / 1000ULL);
    struct tm local;
    if (localtime_r(&seconds, &local) == NULL || local.tm_year < 120) {
        strlcpy(text, "---- -- -- --:--", text_len);
        return;
    }
    snprintf(text,
             text_len,
             "%04d-%02d-%02d %02d:%02d",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday,
             local.tm_hour,
             local.tm_min);
}

static void inbox_app_source_label(const solar_os_inbox_entry_t *entry,
                                   char *text,
                                   size_t text_len)
{
    if (text == NULL || text_len == 0) {
        return;
    }
    if (entry == NULL) {
        text[0] = '\0';
        return;
    }
    if (strcmp(entry->source, "chat") == 0 && entry->topic[0] != '\0') {
        snprintf(text, text_len, "%s/%s", entry->source, entry->topic);
    } else {
        strlcpy(text, entry->source, text_len);
    }
}

static void inbox_app_refresh(void)
{
    uint32_t selected_id = 0;
    if (inbox_app.cursor < inbox_app.count) {
        selected_id = inbox_app.entries[inbox_app.cursor].id;
    }

    inbox_app.count = solar_os_inbox_snapshot(inbox_app.entries,
                                              SOLAR_OS_INBOX_CAPACITY,
                                              inbox_app.unread_only,
                                              &inbox_app.total);
    (void)solar_os_inbox_get_status(&inbox_app.status);

    if (inbox_app.count == 0) {
        inbox_app.cursor = 0;
        inbox_app.top = 0;
        return;
    }

    bool found = false;
    if (selected_id != 0) {
        for (size_t i = 0; i < inbox_app.count; i++) {
            if (inbox_app.entries[i].id == selected_id) {
                inbox_app.cursor = i;
                found = true;
                break;
            }
        }
    }
    if (!found && inbox_app.cursor >= inbox_app.count) {
        inbox_app.cursor = inbox_app.count - 1U;
    }
}

static size_t inbox_app_list_rows(void)
{
    const size_t rows = solar_os_tui_rows(&inbox_app.tui);
    return rows > 2U ? rows - 2U : 0;
}

static void inbox_app_ensure_visible(void)
{
    const size_t visible = inbox_app_list_rows();
    if (visible == 0 || inbox_app.count == 0) {
        inbox_app.top = 0;
        return;
    }
    if (inbox_app.cursor < inbox_app.top) {
        inbox_app.top = inbox_app.cursor;
    } else if (inbox_app.cursor >= inbox_app.top + visible) {
        inbox_app.top = inbox_app.cursor - visible + 1U;
    }
}

static void inbox_app_render_list(void)
{
    const size_t rows = solar_os_tui_rows(&inbox_app.tui);
    const size_t cols = solar_os_tui_cols(&inbox_app.tui);
    char line[INBOX_APP_LINE_MAX];

    inbox_app_ensure_visible();
    snprintf(line,
             sizeof(line),
             "Inbox %u unread%s",
             (unsigned)inbox_app.status.unread,
             inbox_app.unread_only ? "  filtered" : "");
    inbox_app_write_cell(0,
                         0,
                         cols,
                         line,
                         SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    const size_t visible = inbox_app_list_rows();
    for (size_t row = 0; row < visible; row++) {
        const size_t index = inbox_app.top + row;
        if (index >= inbox_app.count) {
            const char *empty = row == 0 && inbox_app.count == 0 ?
                (inbox_app.unread_only ? "No unread messages" : "Inbox is empty") : "";
            inbox_app_write_cell(row + 1U, 0, cols, empty, SOLAR_OS_TUI_ATTR_NORMAL);
            continue;
        }

        const solar_os_inbox_entry_t *entry = &inbox_app.entries[index];
        char summary[SOLAR_OS_INBOX_TITLE_MAX > SOLAR_OS_INBOX_BODY_MAX ?
                     SOLAR_OS_INBOX_TITLE_MAX : SOLAR_OS_INBOX_BODY_MAX];
        char reception[INBOX_APP_RECEPTION_TEXT_MAX];
        char source[INBOX_APP_SOURCE_TEXT_MAX];
        strlcpy(summary, inbox_app_summary(entry), sizeof(summary));
        inbox_app_flatten(summary);
        inbox_app_format_reception(entry, reception, sizeof(reception));
        inbox_app_source_label(entry, source, sizeof(source));
        snprintf(line,
                 sizeof(line),
                 "%c%c %s %s%s%s",
                 entry->unread ? '*' : ' ',
                 entry->priority >= SOLAR_OS_INBOX_PRIORITY_HIGH ? '!' : ' ',
                 reception,
                 source,
                 summary[0] != '\0' ? ": " : "",
                 summary);
        inbox_app_write_cell(row + 1U,
                             0,
                             cols,
                             line,
                             index == inbox_app.cursor ? SOLAR_OS_TUI_ATTR_INVERSE
                                                       : SOLAR_OS_TUI_ATTR_NORMAL);
    }

    if (rows > 0) {
        inbox_app_write_cell(rows - 1U,
                             0,
                             cols,
                             "Enter open  u filter  m read  q quit",
                             SOLAR_OS_TUI_ATTR_INVERSE);
    }
}

static void inbox_app_build_detail_text(void)
{
    const solar_os_inbox_entry_t *entry = &inbox_app.detail;
    size_t used = 0;

#define INBOX_DETAIL_APPEND(...) do { \
        if (used + 1U < sizeof(inbox_app.detail_text)) { \
            const int written = snprintf(inbox_app.detail_text + used, \
                                         sizeof(inbox_app.detail_text) - used, \
                                         __VA_ARGS__); \
            if (written > 0) { \
                const size_t available = sizeof(inbox_app.detail_text) - used; \
                used += (size_t)written < available ? (size_t)written : available - 1U; \
            } \
        } \
    } while (0)

    inbox_app.detail_text[0] = '\0';
    if (entry->title[0] != '\0') {
        INBOX_DETAIL_APPEND("%s\n", entry->title);
    }
    INBOX_DETAIL_APPEND("%s%s%s\n",
                        entry->source,
                        entry->topic[0] != '\0' ? "/" : "",
                        entry->topic);
    if (entry->sender[0] != '\0') {
        INBOX_DETAIL_APPEND("From: %s\n", entry->sender);
    }
    INBOX_DETAIL_APPEND("Priority: %s\n\n", solar_os_inbox_priority_name(entry->priority));
    if (entry->body[0] != '\0') {
        INBOX_DETAIL_APPEND("%s", entry->body);
    }
    if (entry->truncated) {
        INBOX_DETAIL_APPEND("\n[message truncated]");
    }

#undef INBOX_DETAIL_APPEND
}

static size_t inbox_app_next_wrapped_line(const char *text,
                                          size_t offset,
                                          size_t width,
                                          char *line,
                                          size_t line_len)
{
    size_t bytes = 0;
    size_t cols = 0;
    size_t last_space_bytes = 0;
    size_t last_space_next = 0;

    if (line == NULL || line_len == 0 || text == NULL || text[offset] == '\0') {
        return SIZE_MAX;
    }
    if (text[offset] == '\n') {
        line[0] = '\0';
        return offset + 1U;
    }

    while (text[offset + bytes] != '\0' && text[offset + bytes] != '\n' && cols < width) {
        const size_t char_len = inbox_app_utf8_char_len(text + offset + bytes);
        if (char_len == 0) {
            break;
        }
        if (text[offset + bytes] == ' ') {
            last_space_bytes = bytes;
            last_space_next = bytes + char_len;
        }
        bytes += char_len;
        cols++;
    }

    size_t next = offset + bytes;
    size_t copy = bytes;
    if (text[next] == '\n') {
        next++;
    } else if (text[next] != '\0' && last_space_next > 0) {
        copy = last_space_bytes;
        next = offset + last_space_next;
    }
    while (text[next] == ' ') {
        next++;
    }
    if (copy >= line_len) {
        copy = line_len - 1U;
    }
    memcpy(line, text + offset, copy);
    line[copy] = '\0';
    return next;
}

static size_t inbox_app_detail_line_count(size_t width)
{
    size_t count = 0;
    size_t offset = 0;
    char line[INBOX_APP_LINE_MAX];

    while (inbox_app.detail_text[offset] != '\0') {
        const size_t next = inbox_app_next_wrapped_line(inbox_app.detail_text,
                                                        offset,
                                                        width,
                                                        line,
                                                        sizeof(line));
        if (next == SIZE_MAX || next <= offset) {
            break;
        }
        offset = next;
        count++;
    }
    return count;
}

static void inbox_app_render_detail(void)
{
    const size_t rows = solar_os_tui_rows(&inbox_app.tui);
    const size_t cols = solar_os_tui_cols(&inbox_app.tui);
    const size_t visible = rows > 2U ? rows - 2U : 0;
    char header[INBOX_APP_LINE_MAX];
    char line[INBOX_APP_LINE_MAX];
    size_t offset = 0;
    size_t logical_row = 0;

    snprintf(header,
             sizeof(header),
             "Message %lu  %s",
             (unsigned long)inbox_app.detail.id,
             inbox_app.detail.source);
    inbox_app_write_cell(0,
                         0,
                         cols,
                         header,
                         SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    for (size_t row = 0; row < visible; row++) {
        inbox_app_write_cell(row + 1U, 0, cols, "", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    while (inbox_app.detail_text[offset] != '\0' && logical_row < inbox_app.detail_scroll + visible) {
        const size_t next = inbox_app_next_wrapped_line(inbox_app.detail_text,
                                                        offset,
                                                        cols,
                                                        line,
                                                        sizeof(line));
        if (next == SIZE_MAX || next <= offset) {
            break;
        }
        if (logical_row >= inbox_app.detail_scroll) {
            inbox_app_write_cell(1U + logical_row - inbox_app.detail_scroll,
                                 0,
                                 cols,
                                 line,
                                 SOLAR_OS_TUI_ATTR_NORMAL);
        }
        offset = next;
        logical_row++;
    }

    if (rows > 0) {
        inbox_app_write_cell(rows - 1U,
                             0,
                             cols,
                             "Left back  arrows scroll  m unread  q quit",
                             SOLAR_OS_TUI_ATTR_INVERSE);
    }
}

static void inbox_app_render(void)
{
    const size_t rows = solar_os_tui_rows(&inbox_app.tui);
    const size_t cols = solar_os_tui_cols(&inbox_app.tui);
    solar_os_tui_set_cursor_visible(&inbox_app.tui, false);
    solar_os_tui_clear(&inbox_app.tui);
    if (rows < 3U || cols < 12U) {
        inbox_app_write_cell(0, 0, cols, "inbox: terminal too small", SOLAR_OS_TUI_ATTR_NORMAL);
    } else if (inbox_app.view == INBOX_APP_DETAIL) {
        inbox_app_render_detail();
    } else {
        inbox_app_render_list();
    }
    solar_os_tui_refresh(&inbox_app.tui);
}

static void inbox_app_open_selected(void)
{
    if (inbox_app.cursor >= inbox_app.count) {
        return;
    }
    const uint32_t id = inbox_app.entries[inbox_app.cursor].id;
    if (solar_os_inbox_get(id, &inbox_app.detail, true) != ESP_OK) {
        inbox_app_refresh();
        return;
    }
    inbox_app.detail.unread = false;
    inbox_app.view = INBOX_APP_DETAIL;
    inbox_app.detail_scroll = 0;
    inbox_app_build_detail_text();
    (void)solar_os_inbox_get_status(&inbox_app.status);
}

static void inbox_app_toggle_read(void)
{
    solar_os_inbox_entry_t *entry = NULL;
    if (inbox_app.view == INBOX_APP_DETAIL) {
        entry = &inbox_app.detail;
    } else if (inbox_app.cursor < inbox_app.count) {
        entry = &inbox_app.entries[inbox_app.cursor];
    }
    if (entry == NULL) {
        return;
    }
    if (solar_os_inbox_mark_read(entry->id, entry->unread) == ESP_OK) {
        entry->unread = !entry->unread;
        inbox_app_refresh();
    }
}

static void inbox_app_move(int delta)
{
    if (inbox_app.count == 0) {
        return;
    }
    if (delta < 0 && inbox_app.cursor > 0) {
        inbox_app.cursor--;
    } else if (delta > 0 && inbox_app.cursor + 1U < inbox_app.count) {
        inbox_app.cursor++;
    }
}

static void inbox_app_page(bool down)
{
    const size_t step = inbox_app_list_rows() > 1U ? inbox_app_list_rows() - 1U : 1U;
    if (inbox_app.view == INBOX_APP_DETAIL) {
        const size_t total = inbox_app_detail_line_count(solar_os_tui_cols(&inbox_app.tui));
        const size_t visible = inbox_app_list_rows();
        const size_t max_scroll = total > visible ? total - visible : 0;
        if (down) {
            inbox_app.detail_scroll = inbox_app.detail_scroll + step < max_scroll ?
                inbox_app.detail_scroll + step : max_scroll;
        } else {
            inbox_app.detail_scroll = inbox_app.detail_scroll > step ?
                inbox_app.detail_scroll - step : 0;
        }
        return;
    }

    if (inbox_app.count == 0) {
        return;
    }
    if (down) {
        inbox_app.cursor = inbox_app.cursor + step < inbox_app.count ?
            inbox_app.cursor + step : inbox_app.count - 1U;
    } else {
        inbox_app.cursor = inbox_app.cursor > step ? inbox_app.cursor - step : 0;
    }
}

static esp_err_t inbox_app_start(solar_os_context_t *ctx)
{
    memset(&inbox_app, 0, sizeof(inbox_app));
    inbox_app.entries = solar_os_memory_calloc(SOLAR_OS_INBOX_CAPACITY,
                                               sizeof(*inbox_app.entries),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "app.inbox");
    if (inbox_app.entries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = solar_os_tui_begin(&inbox_app.tui, ctx);
    if (err != ESP_OK) {
        solar_os_memory_free(inbox_app.entries);
        memset(&inbox_app, 0, sizeof(inbox_app));
        return err;
    }
    (void)solar_os_tui_enable_diff(&inbox_app.tui, true);
    inbox_app_refresh();
    inbox_app_render();
    return ESP_OK;
}

static void inbox_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&inbox_app.tui, true);
    solar_os_tui_refresh(&inbox_app.tui);
    solar_os_tui_end(&inbox_app.tui);
    solar_os_memory_free(inbox_app.entries);
    memset(&inbox_app, 0, sizeof(inbox_app));
}

static void inbox_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    inbox_app_refresh();
    inbox_app_render();
}

static void inbox_app_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (inbox_app.view == INBOX_APP_DETAIL && inbox_app.detail.source[0] != '\0') {
        snprintf(buffer, buffer_len, "inbox: %s", inbox_app.detail.source);
    } else {
        strlcpy(buffer, "inbox", buffer_len);
    }
}

static bool inbox_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        solar_os_inbox_status_t status;
        if (solar_os_inbox_get_status(&status) == ESP_OK &&
            (status.count != inbox_app.status.count ||
             status.unread != inbox_app.status.unread ||
             status.dropped != inbox_app.status.dropped)) {
            inbox_app_refresh();
            inbox_app_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (inbox_app.view == INBOX_APP_DETAIL &&
        (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_LEFT || ch == '\b' || ch == 0x7fU)) {
        inbox_app.view = INBOX_APP_LIST;
        inbox_app_refresh();
        inbox_app_render();
        return true;
    }
    if (inbox_app.view == INBOX_APP_LIST && ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (inbox_app.view == INBOX_APP_DETAIL) {
            if (inbox_app.detail_scroll > 0) {
                inbox_app.detail_scroll--;
            }
        } else {
            inbox_app_move(-1);
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (inbox_app.view == INBOX_APP_DETAIL) {
            const size_t total = inbox_app_detail_line_count(solar_os_tui_cols(&inbox_app.tui));
            const size_t visible = inbox_app_list_rows();
            if (inbox_app.detail_scroll + visible < total) {
                inbox_app.detail_scroll++;
            }
        } else {
            inbox_app_move(1);
        }
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        inbox_app_page(false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        inbox_app_page(true);
        break;
    case SOLAR_OS_KEY_HOME:
        if (inbox_app.view == INBOX_APP_DETAIL) {
            inbox_app.detail_scroll = 0;
        } else {
            inbox_app.cursor = 0;
        }
        break;
    case SOLAR_OS_KEY_END:
        if (inbox_app.view == INBOX_APP_DETAIL) {
            const size_t total = inbox_app_detail_line_count(solar_os_tui_cols(&inbox_app.tui));
            const size_t visible = inbox_app_list_rows();
            inbox_app.detail_scroll = total > visible ? total - visible : 0;
        } else if (inbox_app.count > 0) {
            inbox_app.cursor = inbox_app.count - 1U;
        }
        break;
    case '\r':
    case '\n':
    case SOLAR_OS_KEY_RIGHT:
        if (inbox_app.view == INBOX_APP_LIST) {
            inbox_app_open_selected();
        }
        break;
    case 'u':
    case 'U':
        if (inbox_app.view == INBOX_APP_LIST) {
            inbox_app.unread_only = !inbox_app.unread_only;
            inbox_app.cursor = 0;
            inbox_app.top = 0;
            inbox_app_refresh();
        }
        break;
    case 'm':
    case 'M':
        inbox_app_toggle_read();
        break;
    case 'r':
    case 'R':
        inbox_app_refresh();
        break;
    default:
        return true;
    }

    inbox_app_render();
    return true;
}

const solar_os_app_t solar_os_inbox_app = {
    .name = "inbox",
    .summary = "universal incoming-message browser",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = inbox_app_start,
    .resume = inbox_app_resume,
    .stop = inbox_app_stop,
    .event = inbox_app_event,
    .title = inbox_app_title,
};
