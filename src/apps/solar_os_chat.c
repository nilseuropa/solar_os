#include "solar_os_chat_app.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "services/solar_os_chat.h"
#include "solar_os_keys.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"

#define CHAT_APP_DEFAULT_CHANNEL "general"
#define CHAT_APP_CHANNEL_COUNT 12
#define CHAT_APP_MESSAGE_COUNT 80
#define CHAT_APP_INPUT_MAX SOLAR_OS_CHAT_TEXT_MAX
#define CHAT_APP_STATUS_MAX 96
#define CHAT_APP_MIN_COLS 28
#define CHAT_APP_MIN_ROWS 8
#define CHAT_APP_INPUT_ROWS 3
#define CHAT_APP_DRAIN_EVENTS_PER_TICK 12
#define CHAT_APP_HISTORY_COUNT 16

typedef enum {
    CHAT_APP_FOCUS_MESSAGES,
    CHAT_APP_FOCUS_CHANNELS,
} chat_app_focus_t;

typedef struct {
    solar_os_chat_event_type_t type;
    bool system;
    bool unread;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
} chat_app_message_t;

typedef struct {
    char name[SOLAR_OS_CHAT_CHANNEL_MAX];
    bool joined;
    bool unread;
} chat_app_channel_t;

typedef struct {
    bool active;
    bool redraw;
    bool join_pending;
    chat_app_focus_t focus;
    solar_os_tui_t tui;
    char initial_url[SOLAR_OS_CHAT_URL_MAX];
    char initial_channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char initial_user[SOLAR_OS_CHAT_USER_MAX];
    char initial_token[SOLAR_OS_CHAT_TOKEN_MAX];
    char input[CHAT_APP_INPUT_MAX];
    size_t input_len;
    size_t input_cursor;
    size_t input_view_offset;
    char history_draft[CHAT_APP_INPUT_MAX];
    char (*history)[CHAT_APP_INPUT_MAX];
    size_t history_count;
    int history_index;
    bool history_browsing;
    uint8_t selected_channel;
    uint8_t current_channel;
    uint8_t channel_count;
    uint8_t channel_scroll;
    uint8_t message_scroll;
    char status[CHAT_APP_STATUS_MAX];
    chat_app_channel_t channels[CHAT_APP_CHANNEL_COUNT];
    chat_app_message_t *messages;
    size_t message_head;
    size_t message_count;
} chat_app_state_t;

static chat_app_state_t chat_app;

static bool chat_arg_is_url(const char *arg)
{
    return arg != NULL && strstr(arg, "://") != NULL;
}

static bool chat_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static const char *chat_current_channel_name(void)
{
    if (chat_app.channel_count == 0 || chat_app.current_channel >= chat_app.channel_count) {
        return CHAT_APP_DEFAULT_CHANNEL;
    }
    return chat_app.channels[chat_app.current_channel].name;
}

static void chat_set_status(const char *status)
{
    strlcpy(chat_app.status, status != NULL ? status : "", sizeof(chat_app.status));
    chat_app.redraw = true;
}

static void chat_set_input(const char *text)
{
    strlcpy(chat_app.input, text != NULL ? text : "", sizeof(chat_app.input));
    chat_app.input_len = strlen(chat_app.input);
    chat_app.input_cursor = chat_app.input_len;
    chat_app.input_view_offset = 0;
    chat_app.redraw = true;
}

static void chat_history_cancel(void)
{
    chat_app.history_browsing = false;
    chat_app.history_index = -1;
}

static void chat_history_add(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }
    if (chat_app.history == NULL) {
        return;
    }
    if (chat_app.history_count > 0 &&
        strcmp(chat_app.history[chat_app.history_count - 1U], line) == 0) {
        return;
    }

    if (chat_app.history_count < CHAT_APP_HISTORY_COUNT) {
        strlcpy(chat_app.history[chat_app.history_count++], line, sizeof(chat_app.history[0]));
    } else {
        memmove(chat_app.history[0],
                chat_app.history[1],
                sizeof(chat_app.history[0]) * (CHAT_APP_HISTORY_COUNT - 1U));
        strlcpy(chat_app.history[CHAT_APP_HISTORY_COUNT - 1U], line, sizeof(chat_app.history[0]));
    }
}

static void chat_history_previous(void)
{
    if (chat_app.history == NULL || chat_app.history_count == 0) {
        return;
    }

    if (!chat_app.history_browsing) {
        strlcpy(chat_app.history_draft, chat_app.input, sizeof(chat_app.history_draft));
        chat_app.history_index = (int)chat_app.history_count - 1;
        chat_app.history_browsing = true;
    } else if (chat_app.history_index > 0) {
        chat_app.history_index--;
    }

    chat_set_input(chat_app.history[chat_app.history_index]);
}

static void chat_history_next(void)
{
    if (!chat_app.history_browsing) {
        return;
    }

    if (chat_app.history_index + 1 < (int)chat_app.history_count) {
        chat_app.history_index++;
        chat_set_input(chat_app.history[chat_app.history_index]);
        return;
    }

    chat_history_cancel();
    chat_set_input(chat_app.history_draft);
}

static size_t chat_safe_clip_len(const char *text, size_t max_bytes)
{
    if (text == NULL) {
        return 0;
    }

    size_t len = 0;
    size_t last_safe = 0;
    while (text[len] != '\0' && len < max_bytes) {
        const unsigned char ch = (unsigned char)text[len];
        if ((ch & 0xc0U) != 0x80U) {
            last_safe = len;
        }
        len++;
    }
    if (text[len] == '\0') {
        return len;
    }
    return last_safe;
}

static void chat_write_cell(size_t row,
                            size_t col,
                            size_t width,
                            const char *text,
                            uint8_t attr)
{
    if (width == 0 || row >= solar_os_tui_rows(&chat_app.tui) ||
        col >= solar_os_tui_cols(&chat_app.tui)) {
        return;
    }

    solar_os_tui_fill(&chat_app.tui, row, col, 1, width, ' ', attr);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    char clipped[128];
    const size_t max_copy = width < sizeof(clipped) - 1U ? width : sizeof(clipped) - 1U;
    const size_t copy_len = chat_safe_clip_len(text, max_copy);
    memcpy(clipped, text, copy_len);
    clipped[copy_len] = '\0';
    solar_os_tui_addstr(&chat_app.tui, row, col, clipped, attr);
}

static int chat_find_channel(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (uint8_t i = 0; i < chat_app.channel_count; i++) {
        if (strcmp(chat_app.channels[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int chat_add_channel(const char *name, bool select)
{
    const char *channel = (name != NULL && name[0] != '\0') ? name : CHAT_APP_DEFAULT_CHANNEL;
    int index = chat_find_channel(channel);
    if (index < 0) {
        if (chat_app.channel_count >= CHAT_APP_CHANNEL_COUNT) {
            return -1;
        }
        index = chat_app.channel_count++;
        strlcpy(chat_app.channels[index].name, channel, sizeof(chat_app.channels[index].name));
    }

    if (select) {
        chat_app.selected_channel = (uint8_t)index;
        chat_app.current_channel = (uint8_t)index;
        chat_app.channels[index].unread = false;
    }
    return index;
}

static void chat_remove_channel(const char *name)
{
    const int index = chat_find_channel(name);
    if (index < 0 || chat_app.channel_count == 0) {
        return;
    }

    const uint8_t remove_index = (uint8_t)index;
    for (uint8_t i = remove_index; i + 1U < chat_app.channel_count; i++) {
        chat_app.channels[i] = chat_app.channels[i + 1U];
    }
    chat_app.channel_count--;
    if (chat_app.channel_count == 0) {
        (void)chat_add_channel(CHAT_APP_DEFAULT_CHANNEL, true);
        return;
    }

    if (chat_app.current_channel >= chat_app.channel_count) {
        chat_app.current_channel = chat_app.channel_count - 1U;
    } else if (chat_app.current_channel > remove_index) {
        chat_app.current_channel--;
    } else if (chat_app.current_channel == remove_index) {
        chat_app.current_channel = remove_index < chat_app.channel_count ?
            remove_index : chat_app.channel_count - 1U;
    }

    if (chat_app.selected_channel >= chat_app.channel_count) {
        chat_app.selected_channel = chat_app.channel_count - 1U;
    } else if (chat_app.selected_channel > remove_index) {
        chat_app.selected_channel--;
    }
    chat_app.message_scroll = 0;
    chat_app.redraw = true;
}

static void chat_append_event(solar_os_chat_event_type_t type,
                              const char *channel,
                              const char *from,
                              const char *text,
                              bool system)
{
    if (chat_app.messages == NULL) {
        return;
    }

    const char *message_channel =
        (channel != NULL && channel[0] != '\0') ? channel : chat_current_channel_name();
    const size_t index = chat_app.message_head;
    chat_app_message_t *message = &chat_app.messages[index];
    memset(message, 0, sizeof(*message));
    message->type = type;
    message->system = system;
    strlcpy(message->channel, message_channel, sizeof(message->channel));
    if (from != NULL) {
        strlcpy(message->from, from, sizeof(message->from));
    }
    if (text != NULL) {
        strlcpy(message->text, text, sizeof(message->text));
    }

    const int channel_index = chat_add_channel(message_channel, false);
    if (channel_index >= 0 &&
        (uint8_t)channel_index != chat_app.current_channel &&
        type == SOLAR_OS_CHAT_EVENT_MESSAGE) {
        chat_app.channels[channel_index].unread = true;
        message->unread = true;
    }

    chat_app.message_head = (chat_app.message_head + 1U) % CHAT_APP_MESSAGE_COUNT;
    if (chat_app.message_count < CHAT_APP_MESSAGE_COUNT) {
        chat_app.message_count++;
    }
    chat_app.redraw = true;
}

static void chat_append_statusf(const char *fmt, ...)
{
    char text[SOLAR_OS_CHAT_TEXT_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    chat_append_event(SOLAR_OS_CHAT_EVENT_RAW, chat_current_channel_name(), "", text, true);
}

static const chat_app_message_t *chat_message_at(size_t logical_index)
{
    if (chat_app.messages == NULL || logical_index >= chat_app.message_count) {
        return NULL;
    }
    const size_t oldest =
        chat_app.message_count == CHAT_APP_MESSAGE_COUNT ? chat_app.message_head : 0U;
    const size_t index = (oldest + logical_index) % CHAT_APP_MESSAGE_COUNT;
    return &chat_app.messages[index];
}

static bool chat_message_matches_current(const chat_app_message_t *message)
{
    if (message == NULL) {
        return false;
    }
    if (message->system) {
        return true;
    }
    return strcmp(message->channel, chat_current_channel_name()) == 0;
}

static void chat_format_message(const chat_app_message_t *message, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (message == NULL) {
        return;
    }

    if (message->system) {
        snprintf(out,
                 out_len,
                 "%c %s",
                 message->type == SOLAR_OS_CHAT_EVENT_ERROR ? '!' : '*',
                 message->text);
        return;
    }

    if (message->from[0] != '\0') {
        snprintf(out, out_len, "%s: %s", message->from, message->text);
    } else {
        strlcpy(out, message->text, out_len);
    }
}

static void chat_draw_channels(size_t left_width, size_t body_rows)
{
    const uint8_t header_attr =
        chat_app.focus == CHAT_APP_FOCUS_CHANNELS ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_BOLD;

    chat_write_cell(0, 0, left_width, "channels", header_attr);

    const size_t list_rows = body_rows > 1 ? body_rows - 1 : 0;
    if (chat_app.selected_channel < chat_app.channel_scroll) {
        chat_app.channel_scroll = chat_app.selected_channel;
    }
    if (list_rows > 0 &&
        chat_app.selected_channel >= chat_app.channel_scroll + list_rows) {
        chat_app.channel_scroll = chat_app.selected_channel - (uint8_t)list_rows + 1U;
    }

    for (size_t row = 0; row < list_rows; row++) {
        const size_t channel_index = chat_app.channel_scroll + row;
        char line[80];
        uint8_t attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (channel_index < chat_app.channel_count) {
            const chat_app_channel_t *channel = &chat_app.channels[channel_index];
            snprintf(line,
                     sizeof(line),
                     "%c%c %s",
                     channel_index == chat_app.current_channel ? '>' : ' ',
                     channel->unread ? '*' : ' ',
                     channel->name);
            if (channel_index == chat_app.selected_channel) {
                attr = SOLAR_OS_TUI_ATTR_INVERSE;
            } else if (channel_index == chat_app.current_channel || channel->unread) {
                attr = SOLAR_OS_TUI_ATTR_BOLD;
            }
        } else {
            line[0] = '\0';
        }

        chat_write_cell(row + 1, 0, left_width, line, attr);
    }
}

static void chat_draw_messages(size_t start_col,
                               size_t width,
                               size_t body_rows,
                               const solar_os_chat_status_t *status)
{
    char header[128];
    const char *state = status != NULL ? solar_os_chat_state_name(status->state) : "?";
    const uint8_t header_attr =
        chat_app.focus == CHAT_APP_FOCUS_MESSAGES ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_BOLD;

    snprintf(header,
             sizeof(header),
             "#%s  %s",
             chat_current_channel_name(),
             state);
    chat_write_cell(0, start_col, width, header, header_attr);

    const size_t text_rows = body_rows > 1 ? body_rows - 1 : 0;
    size_t drawn = 0;
    size_t skipped = 0;

    for (size_t i = 0; i < text_rows; i++) {
        chat_write_cell(1 + i, start_col, width, "", SOLAR_OS_TUI_ATTR_NORMAL);
    }

    if (text_rows == 0 || chat_app.message_count == 0) {
        return;
    }

    for (size_t reverse = 0; reverse < chat_app.message_count && drawn < text_rows; reverse++) {
        const size_t logical = chat_app.message_count - 1U - reverse;
        const chat_app_message_t *message = chat_message_at(logical);
        if (!chat_message_matches_current(message)) {
            continue;
        }
        if (skipped < chat_app.message_scroll) {
            skipped++;
            continue;
        }
        drawn++;
    }

    if (drawn == 0) {
        return;
    }

    size_t row = text_rows - drawn + 1U;
    size_t emitted = 0;
    skipped = 0;
    for (size_t reverse = 0; reverse < chat_app.message_count && emitted < drawn; reverse++) {
        const size_t logical = chat_app.message_count - 1U - reverse;
        const chat_app_message_t *message = chat_message_at(logical);
        if (!chat_message_matches_current(message)) {
            continue;
        }
        if (skipped < chat_app.message_scroll) {
            skipped++;
            continue;
        }

        char line[256];
        chat_format_message(message, line, sizeof(line));
        const size_t draw_row = row + (drawn - emitted - 1U);
        const uint8_t attr = message->system ? SOLAR_OS_TUI_ATTR_BOLD : SOLAR_OS_TUI_ATTR_NORMAL;
        chat_write_cell(draw_row, start_col, width, line, attr);
        emitted++;
    }
}

static void chat_draw_input(size_t rows, size_t cols)
{
    if (rows < CHAT_APP_INPUT_ROWS) {
        return;
    }

    const size_t sep_row = rows - CHAT_APP_INPUT_ROWS;
    const size_t help_row = rows - 2U;
    const size_t input_row = rows - 1U;
    const uint8_t help_attr = SOLAR_OS_TUI_ATTR_INVERSE;

    solar_os_terminal_set_cursor_visible(chat_app.tui.terminal, false);
    solar_os_tui_hline(&chat_app.tui, sep_row, 0, cols, 0, SOLAR_OS_TUI_ATTR_NORMAL);
    chat_write_cell(help_row,
                    0,
                    cols,
                    chat_app.status[0] != '\0' ? chat_app.status :
                    "TAB pane  ENTER send/join  /help commands  ESC exits",
                    help_attr);

    const size_t input_width = cols > 2U ? cols - 2U : 0U;
    if (chat_app.input_cursor < chat_app.input_view_offset) {
        chat_app.input_view_offset = chat_app.input_cursor;
    }
    if (input_width > 0U &&
        chat_app.input_cursor >= chat_app.input_view_offset + input_width) {
        chat_app.input_view_offset = chat_app.input_cursor - input_width + 1U;
    }

    solar_os_tui_fill(&chat_app.tui, input_row, 0, 1, cols, ' ', SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_addstr(&chat_app.tui, input_row, 0, "> ", SOLAR_OS_TUI_ATTR_NORMAL);
    if (cols > 2U && chat_app.input[chat_app.input_view_offset] != '\0') {
        char visible[CHAT_APP_INPUT_MAX];
        const size_t copy_len = chat_safe_clip_len(chat_app.input + chat_app.input_view_offset,
                                                   cols - 2U);
        memcpy(visible, chat_app.input + chat_app.input_view_offset, copy_len);
        visible[copy_len] = '\0';
        solar_os_tui_addstr(&chat_app.tui, input_row, 2, visible, SOLAR_OS_TUI_ATTR_NORMAL);
    }

    const size_t cursor_col = 2U + chat_app.input_cursor - chat_app.input_view_offset;
    solar_os_tui_move(&chat_app.tui,
                      input_row,
                      cursor_col < cols ? cursor_col : cols - 1U);
    solar_os_terminal_set_cursor_visible(chat_app.tui.terminal,
                                         chat_app.focus == CHAT_APP_FOCUS_MESSAGES);
}

static void chat_render(void)
{
    const size_t rows = solar_os_tui_rows(&chat_app.tui);
    const size_t cols = solar_os_tui_cols(&chat_app.tui);

    if (rows < CHAT_APP_MIN_ROWS || cols < CHAT_APP_MIN_COLS) {
        solar_os_tui_clear(&chat_app.tui);
        chat_write_cell(0, 0, cols, "chat: terminal too small", SOLAR_OS_TUI_ATTR_BOLD);
        solar_os_tui_refresh(&chat_app.tui);
        return;
    }

    solar_os_chat_status_t status;
    memset(&status, 0, sizeof(status));
    (void)solar_os_chat_get_status(&status);

    solar_os_tui_clear(&chat_app.tui);

    const size_t input_rows = CHAT_APP_INPUT_ROWS;
    const size_t body_rows = rows > input_rows ? rows - input_rows : rows;
    size_t left_width = cols / 4U;
    if (left_width < 12U) {
        left_width = 12U;
    }
    if (left_width > 22U) {
        left_width = 22U;
    }
    if (left_width + 4U >= cols) {
        left_width = cols / 3U;
    }

    const size_t split_col = left_width;
    const size_t message_col = split_col + 1U;
    const size_t message_width = cols > message_col ? cols - message_col : 0;

    chat_draw_channels(left_width, body_rows);
    if (body_rows > 0 && split_col < cols) {
        solar_os_tui_vrule(&chat_app.tui, 0, split_col, body_rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    chat_draw_messages(message_col, message_width, body_rows, &status);
    chat_draw_input(rows, cols);
    solar_os_tui_refresh(&chat_app.tui);
    chat_app.redraw = false;
}

static void chat_select_channel(uint8_t index, bool join)
{
    if (index >= chat_app.channel_count) {
        return;
    }
    chat_app.selected_channel = index;
    chat_app.current_channel = index;
    chat_app.channels[index].unread = false;
    chat_app.message_scroll = 0;
    chat_app.redraw = true;

    if (join) {
        const esp_err_t err = solar_os_chat_join(chat_app.channels[index].name);
        if (err == ESP_OK) {
            chat_app.channels[index].joined = true;
            chat_set_status("joining");
        } else if (err == ESP_ERR_INVALID_STATE) {
            chat_set_status("not connected");
            chat_app.join_pending = true;
        } else {
            chat_set_status(esp_err_to_name(err));
        }
    }
}

static void chat_handle_service_event(const solar_os_chat_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case SOLAR_OS_CHAT_EVENT_CONNECTED:
        chat_append_event(event->type, chat_current_channel_name(), "", "connected", true);
        chat_set_status("connected");
        chat_app.join_pending = true;
        break;
    case SOLAR_OS_CHAT_EVENT_DISCONNECTED:
        chat_append_event(event->type, chat_current_channel_name(), "", "disconnected", true);
        chat_set_status("disconnected");
        break;
    case SOLAR_OS_CHAT_EVENT_ERROR:
        chat_append_event(event->type, chat_current_channel_name(), "", event->text, true);
        chat_set_status(event->text[0] != '\0' ? event->text : "chat error");
        break;
    case SOLAR_OS_CHAT_EVENT_CHANNEL: {
        const char *name = event->channel[0] != '\0' ? event->channel : event->text;
        if (chat_add_channel(name, false) >= 0) {
            chat_set_status("channel list updated");
        }
        break;
    }
    case SOLAR_OS_CHAT_EVENT_JOINED: {
        const char *name = event->channel[0] != '\0' ? event->channel : event->text;
        const int index = chat_add_channel(name, false);
        if (index >= 0) {
            chat_app.channels[index].joined = true;
        }
        chat_append_event(event->type, name, "", "joined", true);
        chat_set_status("joined");
        break;
    }
    case SOLAR_OS_CHAT_EVENT_LEFT: {
        const char *name = event->channel[0] != '\0' ? event->channel : event->text;
        chat_append_event(event->type, name, "", "left", true);
        chat_remove_channel(name);
        chat_set_status("left");
        break;
    }
    case SOLAR_OS_CHAT_EVENT_MESSAGE:
        chat_append_event(event->type, event->channel, event->from, event->text, false);
        break;
    case SOLAR_OS_CHAT_EVENT_PRESENCE:
        chat_append_event(event->type,
                          event->channel,
                          "",
                          event->text[0] != '\0' ? event->text : event->from,
                          true);
        break;
    case SOLAR_OS_CHAT_EVENT_RAW:
    default:
        chat_append_event(event->type, event->channel, event->from, event->text, true);
        break;
    }
}

static void chat_drain_events(void)
{
    for (size_t i = 0; i < CHAT_APP_DRAIN_EVENTS_PER_TICK; i++) {
        solar_os_chat_event_t event;
        const esp_err_t err = solar_os_chat_read_event(&event, 0);
        if (err != ESP_OK) {
            break;
        }
        chat_handle_service_event(&event);
    }

    if (chat_app.join_pending) {
        solar_os_chat_status_t status;
        if (solar_os_chat_get_status(&status) == ESP_OK && status.connected) {
            const esp_err_t err = solar_os_chat_join(chat_current_channel_name());
            if (err == ESP_OK) {
                chat_app.channels[chat_app.current_channel].joined = true;
                chat_app.join_pending = false;
                chat_set_status("joined");
            }
        }
    }
}

static int chat_tokenize(char *line, char **argv, int argv_max)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < argv_max) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return argc;
}

static void chat_connect_with_args(const char *url,
                                   const char *user,
                                   const char *token,
                                   bool announce)
{
    const esp_err_t err = solar_os_chat_connect(url, token, user, NULL);
    if (err == ESP_OK) {
        chat_set_status("connecting");
        if (announce) {
            chat_append_statusf("connecting %s", url != NULL && url[0] != '\0' ? url : "saved gateway");
        }
        chat_app.join_pending = true;
    } else {
        chat_set_status(esp_err_to_name(err));
        chat_append_statusf("connect failed: %s", esp_err_to_name(err));
    }
}

static void chat_show_status(void)
{
    solar_os_chat_status_t status;
    if (solar_os_chat_get_status(&status) != ESP_OK) {
        chat_append_statusf("chat service unavailable");
        return;
    }

    chat_append_statusf("state: %s", solar_os_chat_state_name(status.state));
    chat_append_statusf("url: %s", status.url[0] != '\0' ? status.url : "(not configured)");
    chat_append_statusf("identity: %s@%s", status.user, status.device);
    chat_append_statusf("token: %s", status.token_set ? "set" : "not set");
    chat_append_statusf("current: #%s", chat_current_channel_name());
    chat_append_statusf("channels: %u", (unsigned)chat_app.channel_count);
    chat_append_statusf("rx=%" PRIu32 " tx=%" PRIu32 " drop=%" PRIu32,
                        status.rx_count,
                        status.tx_count,
                        status.dropped_count);
    chat_append_statusf("cache: RAM session only; no SD cache");
}

static void chat_leave_channel(const char *name)
{
    const char *channel = (name != NULL && name[0] != '\0') ? name : chat_current_channel_name();
    const esp_err_t err = solar_os_chat_leave(channel);
    if (err == ESP_OK) {
        chat_append_statusf("left #%s", channel);
        chat_remove_channel(channel);
        chat_set_status("left");
    } else if (err == ESP_ERR_INVALID_STATE) {
        chat_set_status("not connected");
    } else {
        chat_set_status(esp_err_to_name(err));
    }
}

static void chat_execute_command(char *line)
{
    char *argv[5];
    const int argc = chat_tokenize(line, argv, 5);
    if (argc == 0) {
        return;
    }

    if (strcasecmp(argv[0], "/help") == 0) {
        chat_append_statusf("/join channel");
        chat_append_statusf("/leave [channel]");
        chat_append_statusf("/connect [url]");
        chat_append_statusf("/disconnect");
        chat_append_statusf("/status");
        chat_append_statusf("/quit");
    } else if (strcasecmp(argv[0], "/join") == 0 || strcasecmp(argv[0], "/j") == 0) {
        if (argc < 2) {
            chat_set_status("usage: /join channel");
            return;
        }
        const int index = chat_add_channel(argv[1], true);
        if (index >= 0) {
            chat_select_channel((uint8_t)index, true);
        } else {
            chat_set_status("channel list full");
        }
    } else if (strcasecmp(argv[0], "/leave") == 0 || strcasecmp(argv[0], "/part") == 0) {
        chat_leave_channel(argc >= 2 ? argv[1] : NULL);
    } else if (strcasecmp(argv[0], "/connect") == 0) {
        chat_connect_with_args(argc >= 2 ? argv[1] : NULL, NULL, NULL, true);
    } else if (strcasecmp(argv[0], "/disconnect") == 0) {
        const esp_err_t err = solar_os_chat_disconnect();
        chat_set_status(err == ESP_OK || err == ESP_ERR_INVALID_STATE ? "disconnected" : esp_err_to_name(err));
    } else if (strcasecmp(argv[0], "/status") == 0) {
        chat_show_status();
    } else if (strcasecmp(argv[0], "/quit") == 0 || strcasecmp(argv[0], "/exit") == 0) {
        chat_app.active = false;
    } else {
        chat_set_status("unknown command");
    }
}

static void chat_submit_input(solar_os_context_t *ctx)
{
    (void)ctx;

    if (chat_app.input_len == 0) {
        return;
    }

    char line[CHAT_APP_INPUT_MAX];
    strlcpy(line, chat_app.input, sizeof(line));
    chat_history_add(line);
    chat_history_cancel();
    chat_set_input("");
    chat_app.message_scroll = 0;
    chat_app.redraw = true;

    if (line[0] == '/') {
        chat_execute_command(line);
        return;
    }

    const esp_err_t err = solar_os_chat_send(chat_current_channel_name(), line);
    if (err == ESP_OK) {
        chat_set_status("sent");
    } else {
        chat_set_status(esp_err_to_name(err));
        chat_append_statusf("send failed: %s", esp_err_to_name(err));
    }
}

static void chat_insert_char(char ch)
{
    if (chat_app.input_len + 1U >= sizeof(chat_app.input)) {
        chat_set_status("input full");
        return;
    }

    if (chat_app.input_cursor < chat_app.input_len) {
        memmove(chat_app.input + chat_app.input_cursor + 1U,
                chat_app.input + chat_app.input_cursor,
                chat_app.input_len - chat_app.input_cursor + 1U);
    }
    chat_history_cancel();
    chat_app.input[chat_app.input_cursor++] = ch;
    chat_app.input_len++;
    chat_app.input[chat_app.input_len] = '\0';
    chat_app.redraw = true;
}

static void chat_backspace(void)
{
    if (chat_app.input_cursor == 0) {
        return;
    }
    memmove(chat_app.input + chat_app.input_cursor - 1U,
            chat_app.input + chat_app.input_cursor,
            chat_app.input_len - chat_app.input_cursor + 1U);
    chat_history_cancel();
    chat_app.input_cursor--;
    chat_app.input_len--;
    chat_app.input[chat_app.input_len] = '\0';
    chat_app.redraw = true;
}

static void chat_delete(void)
{
    if (chat_app.input_cursor >= chat_app.input_len) {
        return;
    }
    memmove(chat_app.input + chat_app.input_cursor,
            chat_app.input + chat_app.input_cursor + 1U,
            chat_app.input_len - chat_app.input_cursor);
    chat_history_cancel();
    chat_app.input_len--;
    chat_app.input[chat_app.input_len] = '\0';
    chat_app.redraw = true;
}

static void chat_handle_message_key(solar_os_context_t *ctx, uint8_t ch)
{
    switch (ch) {
    case '\r':
    case '\n':
        chat_submit_input(ctx);
        break;
    case '\b':
        chat_backspace();
        break;
    case SOLAR_OS_KEY_DELETE:
        chat_delete();
        break;
    case SOLAR_OS_KEY_UP:
        chat_history_previous();
        break;
    case SOLAR_OS_KEY_DOWN:
        chat_history_next();
        break;
    case SOLAR_OS_KEY_LEFT:
        if (chat_app.input_cursor > 0) {
            chat_history_cancel();
            chat_app.input_cursor--;
            chat_app.redraw = true;
        }
        break;
    case SOLAR_OS_KEY_RIGHT:
        if (chat_app.input_cursor < chat_app.input_len) {
            chat_history_cancel();
            chat_app.input_cursor++;
            chat_app.redraw = true;
        }
        break;
    case SOLAR_OS_KEY_HOME:
        chat_history_cancel();
        chat_app.input_cursor = 0;
        chat_app.redraw = true;
        break;
    case SOLAR_OS_KEY_END:
        chat_history_cancel();
        chat_app.input_cursor = chat_app.input_len;
        chat_app.redraw = true;
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        if (chat_app.message_scroll + 1U < CHAT_APP_MESSAGE_COUNT) {
            chat_app.message_scroll++;
            chat_app.redraw = true;
        }
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        if (chat_app.message_scroll > 0) {
            chat_app.message_scroll--;
            chat_app.redraw = true;
        }
        break;
    default:
        if (chat_printable(ch)) {
            chat_insert_char((char)ch);
        }
        break;
    }
}

static void chat_handle_channel_key(uint8_t ch)
{
    switch (ch) {
    case SOLAR_OS_KEY_UP:
        if (chat_app.selected_channel > 0) {
            chat_app.selected_channel--;
            chat_app.redraw = true;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (chat_app.selected_channel + 1U < chat_app.channel_count) {
            chat_app.selected_channel++;
            chat_app.redraw = true;
        }
        break;
    case '\r':
    case '\n':
        chat_select_channel(chat_app.selected_channel, true);
        break;
    default:
        break;
    }
}

static esp_err_t chat_start(solar_os_context_t *ctx)
{
    memset(&chat_app, 0, sizeof(chat_app));
    chat_app.focus = CHAT_APP_FOCUS_MESSAGES;
    chat_app.history_index = -1;
    chat_app.redraw = true;
    strlcpy(chat_app.initial_channel, CHAT_APP_DEFAULT_CHANNEL, sizeof(chat_app.initial_channel));

    const int argc = solar_os_context_argc(ctx);
    int argi = 1;
    if (argc > argi && chat_arg_is_url(solar_os_context_argv(ctx, argi))) {
        strlcpy(chat_app.initial_url, solar_os_context_argv(ctx, argi), sizeof(chat_app.initial_url));
        argi++;
    }
    if (argc > argi) {
        strlcpy(chat_app.initial_channel,
                solar_os_context_argv(ctx, argi),
                sizeof(chat_app.initial_channel));
        argi++;
    }
    if (argc > argi) {
        strlcpy(chat_app.initial_user, solar_os_context_argv(ctx, argi), sizeof(chat_app.initial_user));
        argi++;
    }
    if (argc > argi) {
        strlcpy(chat_app.initial_token, solar_os_context_argv(ctx, argi), sizeof(chat_app.initial_token));
    }

    const esp_err_t tui_err = solar_os_tui_begin(&chat_app.tui, ctx);
    if (tui_err != ESP_OK) {
        return tui_err;
    }

    chat_app.messages = heap_caps_calloc(CHAT_APP_MESSAGE_COUNT,
                                         sizeof(chat_app_message_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chat_app.messages == NULL) {
        chat_app.messages = heap_caps_calloc(CHAT_APP_MESSAGE_COUNT,
                                             sizeof(chat_app_message_t),
                                             MALLOC_CAP_8BIT);
    }
    if (chat_app.messages == NULL) {
        return ESP_ERR_NO_MEM;
    }

    chat_app.history = heap_caps_calloc(CHAT_APP_HISTORY_COUNT,
                                        sizeof(chat_app.history[0]),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chat_app.history == NULL) {
        chat_app.history = heap_caps_calloc(CHAT_APP_HISTORY_COUNT,
                                            sizeof(chat_app.history[0]),
                                            MALLOC_CAP_8BIT);
    }
    if (chat_app.history == NULL) {
        heap_caps_free(chat_app.messages);
        chat_app.messages = NULL;
        return ESP_ERR_NO_MEM;
    }

    (void)solar_os_chat_init();
    (void)chat_add_channel(chat_app.initial_channel, true);
    chat_app.active = true;
    chat_append_statusf("chat starting");

    chat_connect_with_args(chat_app.initial_url[0] != '\0' ? chat_app.initial_url : NULL,
                           chat_app.initial_user[0] != '\0' ? chat_app.initial_user : NULL,
                           chat_app.initial_token[0] != '\0' ? chat_app.initial_token : NULL,
                           false);
    chat_render();
    return ESP_OK;
}

static void chat_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    (void)solar_os_chat_disconnect();
    if (chat_app.messages != NULL) {
        heap_caps_free(chat_app.messages);
    }
    if (chat_app.history != NULL) {
        heap_caps_free(chat_app.history);
    }
    if (chat_app.tui.terminal != NULL) {
        solar_os_terminal_set_cursor_visible(chat_app.tui.terminal, true);
    }
    memset(&chat_app, 0, sizeof(chat_app));
}

static bool chat_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        chat_drain_events();
        if (!chat_app.active) {
            solar_os_context_request_exit(ctx);
            return true;
        }
        if (chat_app.redraw) {
            chat_render();
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (ch == '\t') {
        chat_app.focus = chat_app.focus == CHAT_APP_FOCUS_MESSAGES ?
            CHAT_APP_FOCUS_CHANNELS : CHAT_APP_FOCUS_MESSAGES;
        chat_app.redraw = true;
        return true;
    }

    if (chat_app.focus == CHAT_APP_FOCUS_CHANNELS) {
        chat_handle_channel_key(ch);
    } else {
        chat_handle_message_key(ctx, ch);
    }

    if (!chat_app.active) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (chat_app.redraw) {
        chat_render();
    }
    return true;
}

const solar_os_app_t solar_os_chat_app = {
    .name = "chat",
    .summary = "gateway chat client",
    .start = chat_start,
    .stop = chat_stop,
    .event = chat_event,
};
