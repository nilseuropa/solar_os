#include "solar_os_email_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_email.h"
#include "solar_os_keys.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"

#define EMAIL_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)
#define EMAIL_APP_DETAIL_MAX 1200U

typedef enum {
    EMAIL_APP_LIST,
    EMAIL_APP_DETAIL,
} email_app_view_t;

typedef struct {
    solar_os_tui_t tui;
    solar_os_email_message_t *messages;
    size_t count;
    size_t total;
    size_t cursor;
    size_t top;
    bool unread_only;
    email_app_view_t view;
    solar_os_email_message_t detail;
    char detail_text[EMAIL_APP_DETAIL_MAX];
    size_t detail_scroll;
    solar_os_email_status_t status;
} email_app_state_t;

static email_app_state_t email_app;

static size_t email_app_char_len(const char *text)
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

static size_t email_app_clip_len(const char *text, size_t cols, size_t bytes_max)
{
    size_t bytes = 0;
    size_t used_cols = 0;
    while (text != NULL && text[bytes] != '\0' && used_cols < cols) {
        const size_t length = email_app_char_len(text + bytes);
        if (length == 0 || bytes + length > bytes_max) {
            break;
        }
        bytes += length;
        used_cols++;
    }
    return bytes;
}

static void email_app_cell(size_t row,
                           size_t col,
                           size_t width,
                           const char *text,
                           uint8_t attr)
{
    const size_t rows = solar_os_tui_rows(&email_app.tui);
    const size_t cols = solar_os_tui_cols(&email_app.tui);
    char clipped[EMAIL_APP_LINE_MAX];
    if (row >= rows || col >= cols || width == 0) {
        return;
    }
    if (col + width > cols) {
        width = cols - col;
    }
    solar_os_tui_fill(&email_app.tui, row, col, 1, width, ' ', attr);
    const size_t length = email_app_clip_len(text, width, sizeof(clipped) - 1U);
    if (length > 0) {
        memcpy(clipped, text, length);
        clipped[length] = '\0';
        solar_os_tui_addstr(&email_app.tui, row, col, clipped, attr);
    }
}

static size_t email_app_content_rows(void)
{
    const size_t rows = solar_os_tui_rows(&email_app.tui);
    return rows > 2U ? rows - 2U : 0;
}

static void email_app_refresh(void)
{
    uint32_t selected = email_app.cursor < email_app.count ?
        email_app.messages[email_app.cursor].id : 0;
    email_app.count = solar_os_email_snapshot(email_app.messages,
                                              SOLAR_OS_EMAIL_CAPACITY,
                                              email_app.unread_only,
                                              &email_app.total);
    (void)solar_os_email_get_status(&email_app.status);
    if (email_app.count == 0) {
        email_app.cursor = 0;
        email_app.top = 0;
        return;
    }
    bool found = false;
    for (size_t i = 0; selected != 0 && i < email_app.count; i++) {
        if (email_app.messages[i].id == selected) {
            email_app.cursor = i;
            found = true;
            break;
        }
    }
    if (!found && email_app.cursor >= email_app.count) {
        email_app.cursor = email_app.count - 1U;
    }
}

static void email_app_ensure_visible(void)
{
    const size_t visible = email_app_content_rows();
    if (visible == 0 || email_app.count == 0) {
        email_app.top = 0;
    } else if (email_app.cursor < email_app.top) {
        email_app.top = email_app.cursor;
    } else if (email_app.cursor >= email_app.top + visible) {
        email_app.top = email_app.cursor - visible + 1U;
    }
}

static void email_app_flatten(char *text)
{
    for (size_t i = 0; text != NULL && text[i] != '\0'; i++) {
        if (text[i] == '\r' || text[i] == '\n' || text[i] == '\t') {
            text[i] = ' ';
        }
    }
}

static void email_app_render_list(void)
{
    const size_t rows = solar_os_tui_rows(&email_app.tui);
    const size_t cols = solar_os_tui_cols(&email_app.tui);
    char line[EMAIL_APP_LINE_MAX];
    email_app_ensure_visible();
    snprintf(line,
             sizeof(line),
             "Email %u unread%s%s",
             (unsigned)email_app.status.unread,
             email_app.status.syncing ? "  syncing" : "",
             email_app.unread_only ? "  filtered" : "");
    email_app_cell(0,
                   0,
                   cols,
                   line,
                   SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);

    const size_t visible = email_app_content_rows();
    for (size_t row = 0; row < visible; row++) {
        const size_t index = email_app.top + row;
        if (index >= email_app.count) {
            const char *empty = "";
            if (row == 0) {
                if (!email_app.status.configured) {
                    empty = "Not configured";
                } else if (email_app.unread_only) {
                    empty = "No unread email";
                } else {
                    empty = "No synchronized email";
                }
            }
            email_app_cell(row + 1U, 0, cols, empty, SOLAR_OS_TUI_ATTR_NORMAL);
            continue;
        }
        const solar_os_email_message_t *message = &email_app.messages[index];
        char subject[SOLAR_OS_EMAIL_SUBJECT_MAX];
        strlcpy(subject, message->subject, sizeof(subject));
        email_app_flatten(subject);
        snprintf(line,
                 sizeof(line),
                 "%c %s%s%s",
                 message->unread ? '*' : ' ',
                 cols >= 32U && message->from[0] != '\0' ? message->from : "",
                 cols >= 32U && message->from[0] != '\0' ? ": " : "",
                 subject);
        email_app_cell(row + 1U,
                       0,
                       cols,
                       line,
                       index == email_app.cursor ? SOLAR_OS_TUI_ATTR_INVERSE
                                                 : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 0) {
        email_app_cell(rows - 1U,
                       0,
                       cols,
                       "Enter open  u filter  m read  r refresh  q quit",
                       SOLAR_OS_TUI_ATTR_INVERSE);
    }
}

static void email_app_build_detail(void)
{
    snprintf(email_app.detail_text,
             sizeof(email_app.detail_text),
             "%s\nFrom: %s\nDate: %s\n\n%s%s",
             email_app.detail.subject,
             email_app.detail.from,
             email_app.detail.date,
             email_app.detail.preview,
             email_app.detail.truncated ? "\n[preview truncated]" : "");
}

static size_t email_app_next_line(const char *text,
                                  size_t offset,
                                  size_t width,
                                  char *line,
                                  size_t line_len)
{
    if (text[offset] == '\0') {
        return SIZE_MAX;
    }
    if (text[offset] == '\n') {
        line[0] = '\0';
        return offset + 1U;
    }
    size_t bytes = 0;
    size_t cols = 0;
    size_t space_bytes = 0;
    size_t space_next = 0;
    while (text[offset + bytes] != '\0' && text[offset + bytes] != '\n' && cols < width) {
        const size_t length = email_app_char_len(text + offset + bytes);
        if (text[offset + bytes] == ' ') {
            space_bytes = bytes;
            space_next = bytes + length;
        }
        bytes += length;
        cols++;
    }
    size_t copy = bytes;
    size_t next = offset + bytes;
    if (text[next] == '\n') {
        next++;
    } else if (text[next] != '\0' && space_next > 0) {
        copy = space_bytes;
        next = offset + space_next;
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

static size_t email_app_detail_lines(size_t width)
{
    size_t offset = 0;
    size_t count = 0;
    char line[EMAIL_APP_LINE_MAX];
    while (email_app.detail_text[offset] != '\0') {
        const size_t next = email_app_next_line(email_app.detail_text,
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

static void email_app_render_detail(void)
{
    const size_t rows = solar_os_tui_rows(&email_app.tui);
    const size_t cols = solar_os_tui_cols(&email_app.tui);
    const size_t visible = email_app_content_rows();
    char header[EMAIL_APP_LINE_MAX];
    char line[EMAIL_APP_LINE_MAX];
    snprintf(header, sizeof(header), "Email  UID %lu", (unsigned long)email_app.detail.uid);
    email_app_cell(0,
                   0,
                   cols,
                   header,
                   SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    for (size_t row = 0; row < visible; row++) {
        email_app_cell(row + 1U, 0, cols, "", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    size_t offset = 0;
    size_t logical = 0;
    while (email_app.detail_text[offset] != '\0' && logical < email_app.detail_scroll + visible) {
        const size_t next = email_app_next_line(email_app.detail_text,
                                                offset,
                                                cols,
                                                line,
                                                sizeof(line));
        if (next == SIZE_MAX || next <= offset) {
            break;
        }
        if (logical >= email_app.detail_scroll) {
            email_app_cell(1U + logical - email_app.detail_scroll,
                           0,
                           cols,
                           line,
                           SOLAR_OS_TUI_ATTR_NORMAL);
        }
        offset = next;
        logical++;
    }
    if (rows > 0) {
        email_app_cell(rows - 1U,
                       0,
                       cols,
                       "Left back  arrows scroll  m unread  q quit",
                       SOLAR_OS_TUI_ATTR_INVERSE);
    }
}

static void email_app_render(void)
{
    const size_t rows = solar_os_tui_rows(&email_app.tui);
    const size_t cols = solar_os_tui_cols(&email_app.tui);
    solar_os_tui_set_cursor_visible(&email_app.tui, false);
    solar_os_tui_clear(&email_app.tui);
    if (rows < 3U || cols < 12U) {
        email_app_cell(0, 0, cols, "email: terminal too small", SOLAR_OS_TUI_ATTR_NORMAL);
    } else if (email_app.view == EMAIL_APP_DETAIL) {
        email_app_render_detail();
    } else {
        email_app_render_list();
    }
    solar_os_tui_refresh(&email_app.tui);
}

static void email_app_open(void)
{
    if (email_app.cursor >= email_app.count) {
        return;
    }
    if (solar_os_email_get(email_app.messages[email_app.cursor].id,
                           &email_app.detail,
                           true) == ESP_OK) {
        email_app.view = EMAIL_APP_DETAIL;
        email_app.detail_scroll = 0;
        email_app_build_detail();
        (void)solar_os_email_get_status(&email_app.status);
    } else {
        email_app_refresh();
    }
}

static void email_app_toggle_read(void)
{
    solar_os_email_message_t *message = email_app.view == EMAIL_APP_DETAIL ?
        &email_app.detail :
        (email_app.cursor < email_app.count ? &email_app.messages[email_app.cursor] : NULL);
    if (message != NULL && solar_os_email_mark_read(message->id, message->unread) == ESP_OK) {
        message->unread = !message->unread;
        email_app_refresh();
    }
}

static void email_app_page(bool down)
{
    const size_t visible = email_app_content_rows();
    const size_t step = visible > 1U ? visible - 1U : 1U;
    if (email_app.view == EMAIL_APP_DETAIL) {
        const size_t total = email_app_detail_lines(solar_os_tui_cols(&email_app.tui));
        const size_t limit = total > visible ? total - visible : 0;
        if (down) {
            email_app.detail_scroll = email_app.detail_scroll + step < limit ?
                email_app.detail_scroll + step : limit;
        } else {
            email_app.detail_scroll = email_app.detail_scroll > step ?
                email_app.detail_scroll - step : 0;
        }
    } else if (email_app.count > 0) {
        if (down) {
            email_app.cursor = email_app.cursor + step < email_app.count ?
                email_app.cursor + step : email_app.count - 1U;
        } else {
            email_app.cursor = email_app.cursor > step ? email_app.cursor - step : 0;
        }
    }
}

static esp_err_t email_app_start(solar_os_context_t *ctx)
{
    memset(&email_app, 0, sizeof(email_app));
    email_app.messages = heap_caps_calloc(SOLAR_OS_EMAIL_CAPACITY,
                                          sizeof(email_app.messages[0]),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (email_app.messages == NULL) {
        email_app.messages = heap_caps_calloc(SOLAR_OS_EMAIL_CAPACITY,
                                              sizeof(email_app.messages[0]),
                                              MALLOC_CAP_8BIT);
    }
    if (email_app.messages == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = solar_os_tui_begin(&email_app.tui, ctx);
    if (err != ESP_OK) {
        heap_caps_free(email_app.messages);
        memset(&email_app, 0, sizeof(email_app));
        return err;
    }
    (void)solar_os_tui_enable_diff(&email_app.tui, true);
    email_app_refresh();
    email_app_render();
    return ESP_OK;
}

static void email_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&email_app.tui, true);
    solar_os_tui_refresh(&email_app.tui);
    solar_os_tui_end(&email_app.tui);
    heap_caps_free(email_app.messages);
    memset(&email_app, 0, sizeof(email_app));
}

static void email_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    email_app_refresh();
    email_app_render();
}

static bool email_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        solar_os_email_status_t status;
        if (solar_os_email_get_status(&status) == ESP_OK &&
            (status.count != email_app.status.count ||
             status.unread != email_app.status.unread ||
             status.syncing != email_app.status.syncing ||
             status.sync_count != email_app.status.sync_count)) {
            email_app_refresh();
            email_app_render();
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
    if (email_app.view == EMAIL_APP_DETAIL &&
        (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_LEFT || ch == '\b' || ch == 0x7fU)) {
        email_app.view = EMAIL_APP_LIST;
        email_app_refresh();
        email_app_render();
        return true;
    }
    if (email_app.view == EMAIL_APP_LIST && ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (email_app.view == EMAIL_APP_DETAIL) {
            if (email_app.detail_scroll > 0) {
                email_app.detail_scroll--;
            }
        } else if (email_app.cursor > 0) {
            email_app.cursor--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (email_app.view == EMAIL_APP_DETAIL) {
            const size_t total = email_app_detail_lines(solar_os_tui_cols(&email_app.tui));
            if (email_app.detail_scroll + email_app_content_rows() < total) {
                email_app.detail_scroll++;
            }
        } else if (email_app.cursor + 1U < email_app.count) {
            email_app.cursor++;
        }
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        email_app_page(false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        email_app_page(true);
        break;
    case SOLAR_OS_KEY_HOME:
        if (email_app.view == EMAIL_APP_DETAIL) {
            email_app.detail_scroll = 0;
        } else {
            email_app.cursor = 0;
        }
        break;
    case SOLAR_OS_KEY_END:
        if (email_app.view == EMAIL_APP_DETAIL) {
            const size_t total = email_app_detail_lines(solar_os_tui_cols(&email_app.tui));
            const size_t visible = email_app_content_rows();
            email_app.detail_scroll = total > visible ? total - visible : 0;
        } else if (email_app.count > 0) {
            email_app.cursor = email_app.count - 1U;
        }
        break;
    case '\r':
    case '\n':
    case SOLAR_OS_KEY_RIGHT:
        if (email_app.view == EMAIL_APP_LIST) {
            email_app_open();
        }
        break;
    case 'u':
    case 'U':
        if (email_app.view == EMAIL_APP_LIST) {
            email_app.unread_only = !email_app.unread_only;
            email_app.cursor = 0;
            email_app.top = 0;
            email_app_refresh();
        }
        break;
    case 'm':
    case 'M':
        email_app_toggle_read();
        break;
    case 'r':
    case 'R':
        email_app_refresh();
        break;
    default:
        return true;
    }
    email_app_render();
    return true;
}

static void email_app_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0) {
        strlcpy(buffer, email_app.view == EMAIL_APP_DETAIL ? "email: message" : "email", buffer_len);
    }
}

const solar_os_app_t solar_os_email_app = {
    .name = "email",
    .summary = "IMAP email client",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = email_app_start,
    .resume = email_app_resume,
    .stop = email_app_stop,
    .event = email_app_event,
    .title = email_app_title,
};
