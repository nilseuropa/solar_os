#include "solar_os_chat_app.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "services/solar_os_chat.h"
#include "solar_os_contacts.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define CHAT_APP_SYSTEM_CHANNEL "system"
#define CHAT_APP_CHANNEL_COUNT 33
#define CHAT_APP_PROVIDER_SECTION_COUNT 3
#define CHAT_APP_CHANNEL_VIEW_COUNT \
    (CHAT_APP_CHANNEL_COUNT + CHAT_APP_PROVIDER_SECTION_COUNT)
#define CHAT_APP_MESSAGE_COUNT 80
#define CHAT_APP_INPUT_MAX SOLAR_OS_CHAT_TEXT_MAX
#define CHAT_APP_STATUS_MAX 96
#define CHAT_APP_MIN_COLS 28
#define CHAT_APP_MIN_ROWS 8
#define CHAT_APP_TAB_ROWS 1
#define CHAT_APP_INPUT_ROWS 3
#define CHAT_APP_DRAIN_EVENTS_PER_TICK 12
#define CHAT_APP_HISTORY_COUNT 16
#define CHAT_APP_LINE_MAX 256
#define CHAT_APP_STATUS_TEXT_MAX 256

typedef enum {
    CHAT_APP_TAB_CHANNELS,
    CHAT_APP_TAB_CHAT,
} chat_app_tab_t;

typedef enum {
    CHAT_APP_EVENT_STATUS,
    CHAT_APP_EVENT_MESSAGE,
    CHAT_APP_EVENT_ERROR,
} chat_app_event_type_t;

typedef struct {
    chat_app_event_type_t type;
    uint64_t message_key;
    solar_os_conversation_id_t conversation_id;
    uint64_t timestamp;
    solar_os_messaging_provider_id_t provider;
    solar_os_message_direction_t direction;
    solar_os_delivery_state_t delivery;
    uint32_t security_flags;
    bool system;
    bool unread;
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
} chat_app_message_t;

typedef struct {
    char name[SOLAR_OS_CHAT_CHANNEL_MAX];
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    solar_os_conversation_id_t conversation_id;
    solar_os_messaging_provider_id_t provider;
    solar_os_conversation_kind_t kind;
    uint32_t security_flags;
    uint32_t unread_count;
    uint64_t last_message_ms;
    bool unread;
    bool system;
} chat_app_channel_t;

typedef struct {
    bool heading;
    uint8_t channel_index;
    char label[SOLAR_OS_MESSAGING_TITLE_MAX + 2U];
} chat_app_channel_row_t;

typedef struct {
    bool active;
    bool suspended;
    bool redraw;
    chat_app_tab_t tab;
    solar_os_tui_t tui;
    solar_os_messaging_provider_id_t filter_provider;
    solar_os_conversation_id_t initial_conversation_id;
    char *input;
    size_t input_len;
    size_t input_cursor;
    size_t input_view_offset;
    char *history_draft;
    char (*history)[CHAT_APP_INPUT_MAX];
    size_t history_count;
    int history_index;
    bool history_browsing;
    uint8_t selected_channel;
    uint8_t current_channel;
    uint8_t channel_count;
    uint8_t channel_scroll;
    size_t message_scroll;
    char status[CHAT_APP_STATUS_MAX];
    chat_app_channel_t channels[CHAT_APP_CHANNEL_COUNT];
    chat_app_channel_row_t channel_rows[CHAT_APP_CHANNEL_VIEW_COUNT];
    chat_app_message_t *messages;
    solar_os_messaging_event_t *messaging_event;
    uint32_t messaging_event_cursor;
    uint32_t messaging_generation;
    bool messaging_dirty;
    bool initial_selection_pending;
    bool gateway_join_pending;
    solar_os_conversation_id_t gateway_join_conversation_id;
    char gateway_join_provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    bool confirm_untrusted;
    char *pending_untrusted;
    size_t message_head;
    size_t message_count;
} chat_app_state_t;

static chat_app_state_t *chat_app_state;
#define chat_app (*chat_app_state)

static bool chat_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static const char *chat_current_channel_name(void)
{
    if (chat_app.channel_count == 0 || chat_app.current_channel >= chat_app.channel_count) {
        return CHAT_APP_SYSTEM_CHANNEL;
    }
    return chat_app.channels[chat_app.current_channel].name;
}

static bool chat_current_is_system(void)
{
    return chat_app.channel_count > 0 &&
        chat_app.current_channel < chat_app.channel_count &&
        chat_app.channels[chat_app.current_channel].system;
}

static solar_os_conversation_id_t chat_current_conversation_id(void)
{
    if (chat_app.channel_count == 0 ||
        chat_app.current_channel >= chat_app.channel_count) {
        return SOLAR_OS_CONVERSATION_ID_NONE;
    }
    return chat_app.channels[chat_app.current_channel].conversation_id;
}

static bool chat_current_is_discovered_direct(void)
{
    solar_os_messaging_conversation_t conversation;
    if (solar_os_messaging_conversation_get(
            chat_current_conversation_id(),
            &conversation) != ESP_OK ||
        conversation.kind != SOLAR_OS_CONVERSATION_DIRECT ||
        conversation.endpoint_id == 0) {
        return false;
    }
    solar_os_endpoint_t endpoint;
    return solar_os_contacts_get_endpoint(conversation.endpoint_id,
                                          &endpoint) == ESP_OK &&
        endpoint.trust == SOLAR_OS_CONTACT_TRUST_DISCOVERED;
}

static const char *chat_provider_state_name(
    solar_os_messaging_provider_id_t provider)
{
    solar_os_messaging_provider_status_t status;
    if (solar_os_messaging_provider_get_status(provider, &status) != ESP_OK) {
        return "unavailable";
    }
    if (provider == SOLAR_OS_MESSAGING_PROVIDER_GATEWAY) {
        return status.connected ? "connected" :
            (status.running ? "connecting" : "disconnected");
    }
    if (!status.running) {
        return "stopped";
    }
    if (status.last_error != ESP_OK) {
        return "error";
    }
    return status.connected ? "ready" : "starting";
}

static void chat_set_status(const char *status)
{
    strlcpy(chat_app.status, status != NULL ? status : "", sizeof(chat_app.status));
    chat_app.redraw = true;
}

static void chat_set_input(const char *text)
{
    if (chat_app.input == NULL) {
        return;
    }
    strlcpy(chat_app.input, text != NULL ? text : "", CHAT_APP_INPUT_MAX);
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
        if (chat_app.history_draft != NULL) {
            strlcpy(chat_app.history_draft, chat_app.input, CHAT_APP_INPUT_MAX);
        }
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
    chat_set_input(chat_app.history_draft != NULL ? chat_app.history_draft : "");
}

static size_t chat_utf8_char_len(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    const unsigned char ch = (unsigned char)text[0];
    if (ch < 0x80U) {
        return 1;
    }
    if ((ch & 0xe0U) == 0xc0U &&
        text[1] != '\0' &&
        (text[1] & 0xc0U) == 0x80U) {
        return 2;
    }
    if ((ch & 0xf0U) == 0xe0U &&
        text[1] != '\0' &&
        text[2] != '\0' &&
        (text[1] & 0xc0U) == 0x80U &&
        (text[2] & 0xc0U) == 0x80U) {
        return 3;
    }
    if ((ch & 0xf8U) == 0xf0U &&
        text[1] != '\0' &&
        text[2] != '\0' &&
        text[3] != '\0' &&
        (text[1] & 0xc0U) == 0x80U &&
        (text[2] & 0xc0U) == 0x80U &&
        (text[3] & 0xc0U) == 0x80U) {
        return 4;
    }
    return 1;
}

static size_t chat_utf8_width(const char *text)
{
    size_t width = 0;
    if (text == NULL) {
        return 0;
    }

    for (size_t i = 0; text[i] != '\0';) {
        const size_t char_len = chat_utf8_char_len(text + i);
        if (char_len == 0) {
            break;
        }
        i += char_len;
        width++;
    }
    return width;
}

static size_t chat_take_columns(const char *text,
                                size_t max_cols,
                                size_t max_bytes,
                                size_t *cols_taken)
{
    size_t bytes = 0;
    size_t cols = 0;

    if (text == NULL || max_cols == 0 || max_bytes == 0) {
        if (cols_taken != NULL) {
            *cols_taken = 0;
        }
        return 0;
    }

    while (text[bytes] != '\0' && cols < max_cols) {
        const size_t char_len = chat_utf8_char_len(text + bytes);
        if (char_len == 0 || bytes + char_len > max_bytes) {
            break;
        }
        bytes += char_len;
        cols++;
    }

    if (cols_taken != NULL) {
        *cols_taken = cols;
    }
    return bytes;
}

static size_t chat_safe_clip_len(const char *text, size_t max_cols, size_t max_bytes)
{
    return chat_take_columns(text, max_cols, max_bytes, NULL);
}

static uint8_t chat_add_system_channel(void)
{
    const uint8_t index = chat_app.channel_count++;
    chat_app_channel_t *system = &chat_app.channels[index];
    memset(system, 0, sizeof(*system));
    system->system = true;
    strlcpy(system->name, CHAT_APP_SYSTEM_CHANNEL, sizeof(system->name));
    return index;
}

static void chat_refresh_conversations(void)
{
    solar_os_messaging_conversation_t *snapshot =
        solar_os_memory_calloc(SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
                               sizeof(*snapshot),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "chat.conversations");
    if (snapshot == NULL) {
        return;
    }
    const bool current_system = chat_current_is_system();
    const bool selected_system =
        chat_app.channel_count > 0 &&
        chat_app.selected_channel < chat_app.channel_count &&
        chat_app.channels[chat_app.selected_channel].system;
    const solar_os_conversation_id_t current_id =
        chat_current_conversation_id();
    const solar_os_conversation_id_t selected_id =
        chat_app.channel_count > 0 &&
                chat_app.selected_channel < chat_app.channel_count ?
            chat_app.channels[chat_app.selected_channel].conversation_id :
            SOLAR_OS_CONVERSATION_ID_NONE;
    const size_t count = solar_os_messaging_conversation_snapshot(
        snapshot,
        SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY);
    chat_app.channel_count = 0;
    const uint8_t system_index = chat_add_system_channel();
    uint8_t current = system_index;
    uint8_t selected = system_index;
    bool current_found = current_system &&
        !chat_app.initial_selection_pending;
    bool selected_found = selected_system &&
        !chat_app.initial_selection_pending;
    uint8_t preferred = system_index;
    bool preferred_found = false;
    const solar_os_messaging_provider_id_t provider_order[] = {
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        SOLAR_OS_MESSAGING_PROVIDER_LINK,
    };
    for (size_t provider = 0;
         provider < sizeof(provider_order) / sizeof(provider_order[0]);
         provider++) {
        for (size_t i = 0;
             i < count && chat_app.channel_count < CHAT_APP_CHANNEL_COUNT;
             i++) {
            if (snapshot[i].provider != provider_order[provider]) {
                continue;
            }
            if (chat_app.filter_provider != 0U &&
                snapshot[i].provider != chat_app.filter_provider) {
                continue;
            }
            if (chat_app.initial_conversation_id != 0U &&
                snapshot[i].id != chat_app.initial_conversation_id) {
                continue;
            }
            const uint8_t index = chat_app.channel_count++;
            chat_app_channel_t *conversation =
                &chat_app.channels[index];
            memset(conversation, 0, sizeof(*conversation));
            conversation->conversation_id = snapshot[i].id;
            conversation->provider = snapshot[i].provider;
            conversation->kind = snapshot[i].kind;
            conversation->security_flags = snapshot[i].security_flags;
            conversation->unread_count = snapshot[i].unread_count;
            conversation->last_message_ms = snapshot[i].last_message_ms;
            conversation->unread = snapshot[i].unread_count != 0;
            strlcpy(conversation->name,
                    snapshot[i].title,
                    sizeof(conversation->name));
            strlcpy(conversation->provider_key,
                    snapshot[i].provider_key,
                    sizeof(conversation->provider_key));
            if (!preferred_found ||
                (snapshot[i].unread_count != 0 &&
                 chat_app.channels[preferred].unread_count == 0) ||
                ((snapshot[i].unread_count != 0) ==
                     (chat_app.channels[preferred].unread_count != 0) &&
                 snapshot[i].last_message_ms >
                     chat_app.channels[preferred].last_message_ms)) {
                preferred = index;
                preferred_found = true;
            }
            if (!current_found && snapshot[i].id == current_id) {
                current = index;
                current_found = true;
            }
            if (!selected_found && snapshot[i].id == selected_id) {
                selected = index;
                selected_found = true;
            }
        }
    }
    if (chat_app.initial_selection_pending) {
        if (preferred_found) {
            current = preferred;
            selected = preferred;
            current_found = true;
            selected_found = true;
        }
        chat_app.initial_selection_pending = false;
    }
    if (!current_found) {
        for (uint8_t i = 0; i < chat_app.channel_count; i++) {
            if (!chat_app.channels[i].system) {
                current = i;
                current_found = true;
                break;
            }
        }
    }
    if (!selected_found && current_found) {
        selected = current;
        selected_found = true;
    }
    chat_app.current_channel = current;
    chat_app.selected_channel = selected_found ? selected : current;
    solar_os_memory_free(snapshot);
}

static void chat_append_event_full(chat_app_event_type_t type,
                                   const char *channel,
                                   const char *from,
                                   const char *text,
                                   bool system,
                                   uint64_t message_key,
                                   uint64_t timestamp)
{
    if (chat_app.messages == NULL) {
        return;
    }

    const char *message_channel =
        system ? CHAT_APP_SYSTEM_CHANNEL :
        (channel != NULL && channel[0] != '\0') ?
            channel : chat_current_channel_name();
    if (system && chat_app.message_count > 0) {
        const size_t newest =
            (chat_app.message_head + CHAT_APP_MESSAGE_COUNT - 1U) %
            CHAT_APP_MESSAGE_COUNT;
        const chat_app_message_t *previous = &chat_app.messages[newest];
        if (previous->system &&
            previous->type == type &&
            strcmp(previous->from, from != NULL ? from : "") == 0 &&
            strcmp(previous->text, text != NULL ? text : "") == 0) {
            return;
        }
    }
    const size_t index = chat_app.message_head;
    chat_app_message_t *message = &chat_app.messages[index];
    memset(message, 0, sizeof(*message));
    message->type = type;
    message->message_key = message_key;
    message->timestamp = timestamp;
    message->system = system;
    strlcpy(message->channel, message_channel, sizeof(message->channel));
    if (from != NULL) {
        strlcpy(message->from, from, sizeof(message->from));
    }
    if (text != NULL) {
        strlcpy(message->text, text, sizeof(message->text));
    }

    if (system) {
        for (uint8_t i = 0; i < chat_app.channel_count; i++) {
            if (chat_app.channels[i].system &&
                i != chat_app.current_channel) {
                chat_app.channels[i].unread = true;
                message->unread = true;
                break;
            }
        }
    }

    chat_app.message_head = (chat_app.message_head + 1U) % CHAT_APP_MESSAGE_COUNT;
    if (chat_app.message_count < CHAT_APP_MESSAGE_COUNT) {
        chat_app.message_count++;
    }
    chat_app.redraw = true;
}

static void chat_append_event(chat_app_event_type_t type,
                              const char *channel,
                              const char *from,
                              const char *text,
                              bool system)
{
    chat_append_event_full(type, channel, from, text, system, 0, 0);
}

static bool chat_restore_message(
    const solar_os_messaging_message_t *message,
    void *user)
{
    (void)user;
    if (message == NULL) {
        return false;
    }
    solar_os_messaging_conversation_t conversation;
    if (solar_os_messaging_conversation_get(message->conversation_id,
                                            &conversation) != ESP_OK) {
        return true;
    }
    const size_t index = chat_app.message_head;
    chat_append_event_full(CHAT_APP_EVENT_MESSAGE,
                           conversation.title,
                           message->direction == SOLAR_OS_MESSAGE_OUTBOUND ?
                               "me" : message->sender,
                           message->body,
                           false,
                           message->key,
                           message->timestamp_ms);
    chat_app_message_t *stored = &chat_app.messages[index];
    stored->conversation_id = message->conversation_id;
    stored->provider = message->provider;
    stored->direction = message->direction;
    stored->delivery = message->delivery;
    stored->security_flags = message->security_flags;
    return true;
}

static void chat_reconcile_messaging(void)
{
    if (chat_app.messages == NULL) {
        return;
    }
    const size_t oldest =
        (chat_app.message_head + CHAT_APP_MESSAGE_COUNT -
         chat_app.message_count) % CHAT_APP_MESSAGE_COUNT;
    size_t system_count = 0;
    for (size_t i = 0; i < chat_app.message_count; i++) {
        const size_t read = (oldest + i) % CHAT_APP_MESSAGE_COUNT;
        if (!chat_app.messages[read].system) {
            continue;
        }
        const size_t write = (oldest + system_count) % CHAT_APP_MESSAGE_COUNT;
        if (write != read) {
            chat_app.messages[write] = chat_app.messages[read];
        }
        system_count++;
    }
    chat_app.message_count = system_count;
    chat_app.message_head = (oldest + system_count) % CHAT_APP_MESSAGE_COUNT;
    chat_refresh_conversations();
    (void)solar_os_messaging_message_visit_consistent(
        chat_app.initial_conversation_id,
        chat_app.filter_provider,
        chat_restore_message,
        NULL,
        &chat_app.messaging_event_cursor,
        &chat_app.messaging_generation);
    if (!chat_current_is_system()) {
        (void)solar_os_messaging_mark_read(chat_current_conversation_id());
    }
    chat_app.messaging_dirty = false;
    chat_app.redraw = true;
}

static void chat_append_statusf(const char *fmt, ...)
{
    char text[CHAT_APP_STATUS_TEXT_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    chat_append_event(CHAT_APP_EVENT_STATUS, chat_current_channel_name(), "", text, true);
}

static const chat_app_message_t *chat_message_at(size_t logical_index)
{
    if (chat_app.messages == NULL || logical_index >= chat_app.message_count) {
        return NULL;
    }
    const size_t oldest =
        (chat_app.message_head + CHAT_APP_MESSAGE_COUNT -
         chat_app.message_count) % CHAT_APP_MESSAGE_COUNT;
    const size_t index = (oldest + logical_index) % CHAT_APP_MESSAGE_COUNT;
    return &chat_app.messages[index];
}

static bool chat_message_matches_current(const chat_app_message_t *message)
{
    if (message == NULL) {
        return false;
    }
    if (message->system) {
        return chat_current_is_system();
    }
    if (chat_current_is_system()) {
        return false;
    }
    if (message->conversation_id != 0) {
        return message->conversation_id == chat_current_conversation_id();
    }
    return strcmp(message->channel, chat_current_channel_name()) == 0;
}

typedef void (*chat_visual_line_fn)(const char *line,
                                    uint8_t attr,
                                    size_t emphasis_bytes,
                                    void *user);

static void chat_make_user_prefix(const char *from,
                                  size_t width,
                                  char *prefix,
                                  size_t prefix_len,
                                  size_t *emphasis_bytes)
{
    if (prefix == NULL || prefix_len == 0) {
        return;
    }
    prefix[0] = '\0';
    if (emphasis_bytes != NULL) {
        *emphasis_bytes = 0;
    }

    if (from == NULL || from[0] == '\0' || width == 0) {
        return;
    }

    const size_t from_cols = chat_utf8_width(from);
    if (from_cols + 2U <= width) {
        snprintf(prefix, prefix_len, "%s: ", from);
        if (emphasis_bytes != NULL) {
            *emphasis_bytes = strlen(from);
        }
    } else {
        char name[SOLAR_OS_CHAT_USER_MAX];
        const size_t name_cols = width > 3U ? width - 3U : 1U;
        const size_t name_bytes = chat_take_columns(from,
                                                    name_cols,
                                                    sizeof(name) - 1U,
                                                    NULL);
        memcpy(name, from, name_bytes);
        name[name_bytes] = '\0';
        snprintf(prefix, prefix_len, "%s~: ", name);
        if (emphasis_bytes != NULL) {
            *emphasis_bytes = name_bytes + 1U;
        }
    }
}

static void chat_message_prefixes(const chat_app_message_t *message,
                                  size_t width,
                                  char *first,
                                  size_t first_len,
                                  size_t *emphasis_bytes)
{
    if (first == NULL || first_len == 0) {
        return;
    }
    first[0] = '\0';
    if (emphasis_bytes != NULL) {
        *emphasis_bytes = 0;
    }

    if (message == NULL) {
        return;
    }

    if (message->system) {
        snprintf(first,
                 first_len,
                 "%c ",
                 message->type == CHAT_APP_EVENT_ERROR ? '!' : '*');
        return;
    }

    char actor[SOLAR_OS_CHAT_USER_MAX + 4U];
    size_t actor_emphasis = 0;
    chat_make_user_prefix(message->from,
                          width,
                          actor,
                          sizeof(actor),
                          &actor_emphasis);
    if (message->direction == SOLAR_OS_MESSAGE_OUTBOUND) {
        const char delivery =
            message->delivery == SOLAR_OS_DELIVERY_QUEUED ? 'q' :
            message->delivery == SOLAR_OS_DELIVERY_SENDING ? '>' :
            message->delivery == SOLAR_OS_DELIVERY_SENT ? 's' :
            message->delivery == SOLAR_OS_DELIVERY_DELIVERED ? 'd' :
            message->delivery == SOLAR_OS_DELIVERY_FAILED ? '!' :
            message->delivery == SOLAR_OS_DELIVERY_CANCELLED ? 'x' : '-';
        snprintf(first,
                 first_len,
                 "[%c%c] %s",
                 delivery,
                 (message->security_flags & SOLAR_OS_SECURITY_ENCRYPTED) != 0 ?
                     '+' : '-',
                 actor);
        if (emphasis_bytes != NULL) {
            *emphasis_bytes = 0;
        }
    } else {
        strlcpy(first, actor, first_len);
        if (emphasis_bytes != NULL) {
            *emphasis_bytes = actor_emphasis;
        }
    }
}

static void chat_line_init(char *line,
                           size_t line_size,
                           const char *prefix,
                           size_t width,
                           size_t *line_bytes,
                           size_t *line_cols,
                           bool *has_text)
{
    if (line == NULL || line_size == 0) {
        return;
    }

    size_t prefix_cols = 0;
    const size_t prefix_bytes = chat_take_columns(prefix,
                                                 width,
                                                 line_size - 1U,
                                                 &prefix_cols);
    if (prefix_bytes > 0) {
        memcpy(line, prefix, prefix_bytes);
    }
    line[prefix_bytes] = '\0';

    if (line_bytes != NULL) {
        *line_bytes = prefix_bytes;
    }
    if (line_cols != NULL) {
        *line_cols = prefix_cols;
    }
    if (has_text != NULL) {
        *has_text = false;
    }
}

static void chat_line_append(char *line,
                             size_t line_size,
                             size_t *line_bytes,
                             size_t *line_cols,
                             const char *text,
                             size_t text_bytes,
                             size_t text_cols)
{
    if (line == NULL || line_size == 0 || line_bytes == NULL || line_cols == NULL ||
        text == NULL || text_bytes == 0) {
        return;
    }
    if (*line_bytes + text_bytes >= line_size) {
        return;
    }

    memcpy(line + *line_bytes, text, text_bytes);
    *line_bytes += text_bytes;
    *line_cols += text_cols;
    line[*line_bytes] = '\0';
}

static void chat_scan_word(const char *text, size_t *word_bytes, size_t *word_cols)
{
    size_t bytes = 0;
    size_t cols = 0;

    if (text != NULL) {
        while (text[bytes] != '\0' &&
               text[bytes] != '\n' &&
               text[bytes] != '\r' &&
               text[bytes] != ' ' &&
               text[bytes] != '\t') {
            const size_t char_len = chat_utf8_char_len(text + bytes);
            if (char_len == 0) {
                break;
            }
            bytes += char_len;
            cols++;
        }
    }

    if (word_bytes != NULL) {
        *word_bytes = bytes;
    }
    if (word_cols != NULL) {
        *word_cols = cols;
    }
}

static size_t chat_emit_wrapped_message(const chat_app_message_t *message,
                                        size_t width,
                                        chat_visual_line_fn emit,
                                        void *user)
{
    if (message == NULL || width == 0) {
        return 0;
    }

    char first_prefix[80];
    char line[CHAT_APP_LINE_MAX];
    const uint8_t attr = message->system ? SOLAR_OS_TUI_ATTR_BOLD : SOLAR_OS_TUI_ATTR_NORMAL;
    const char *text = message->text;
    size_t line_bytes = 0;
    size_t line_cols = 0;
    size_t emitted = 0;
    size_t line_emphasis_bytes = 0;
    bool has_text = false;

    chat_message_prefixes(message,
                          width,
                          first_prefix,
                          sizeof(first_prefix),
                          &line_emphasis_bytes);
    chat_line_init(line,
                   sizeof(line),
                   first_prefix,
                   width,
                   &line_bytes,
                   &line_cols,
                   &has_text);

    while (text != NULL && *text != '\0') {
        if (*text == '\n' || *text == '\r') {
            if (emit != NULL) {
                emit(line, attr, line_emphasis_bytes, user);
            }
            emitted++;
            line_emphasis_bytes = 0;
            if (*text == '\r' && text[1] == '\n') {
                text += 2;
            } else {
                text++;
            }
            chat_line_init(line,
                           sizeof(line),
                           "",
                           width,
                           &line_bytes,
                           &line_cols,
                           &has_text);
            continue;
        }

        if (*text == ' ' || *text == '\t') {
            text++;
            continue;
        }

        size_t word_bytes = 0;
        size_t word_cols = 0;
        chat_scan_word(text, &word_bytes, &word_cols);
        if (word_bytes == 0 || word_cols == 0) {
            text++;
            continue;
        }

        const char *word = text;
        size_t remaining_bytes = word_bytes;
        size_t remaining_cols = word_cols;
        while (remaining_cols > 0) {
            const size_t room_cols = line_cols < width ? width - line_cols : 0;
            const size_t space_cols = has_text ? 1U : 0U;

            if (remaining_cols + space_cols <= room_cols) {
                if (has_text) {
                    chat_line_append(line,
                                     sizeof(line),
                                     &line_bytes,
                                     &line_cols,
                                     " ",
                                     1,
                                     1);
                }
                chat_line_append(line,
                                 sizeof(line),
                                 &line_bytes,
                                 &line_cols,
                                 word,
                                 remaining_bytes,
                                 remaining_cols);
                has_text = true;
                word += remaining_bytes;
                remaining_bytes = 0;
                remaining_cols = 0;
                break;
            }

            if (!has_text && room_cols > 0) {
                size_t taken_cols = 0;
                const size_t taken_bytes = chat_take_columns(word,
                                                            room_cols,
                                                            sizeof(line) - line_bytes - 1U,
                                                            &taken_cols);
                if (taken_bytes == 0 || taken_cols == 0) {
                    break;
                }
                chat_line_append(line,
                                 sizeof(line),
                                 &line_bytes,
                                 &line_cols,
                                 word,
                                 taken_bytes,
                                 taken_cols);
                has_text = true;
                word += taken_bytes;
                remaining_bytes -= taken_bytes;
                remaining_cols -= taken_cols;
                if (remaining_cols == 0) {
                    break;
                }
            }

            if (emit != NULL) {
                emit(line, attr, line_emphasis_bytes, user);
            }
            emitted++;
            line_emphasis_bytes = 0;
            chat_line_init(line,
                           sizeof(line),
                           "",
                           width,
                           &line_bytes,
                           &line_cols,
                           &has_text);
        }

        text = word;
    }

    if (emit != NULL) {
        emit(line, attr, line_emphasis_bytes, user);
    }
    emitted++;
    return emitted;
}

static size_t chat_emit_visual_rows(size_t width,
                                    chat_visual_line_fn emit,
                                    void *user)
{
    size_t count = 0;
    char previous_from[SOLAR_OS_CHAT_USER_MAX] = {0};
    bool have_previous_sender = false;

    if (width == 0) {
        return 0;
    }

    for (size_t logical = 0; logical < chat_app.message_count; logical++) {
        const chat_app_message_t *message = chat_message_at(logical);
        if (!chat_message_matches_current(message)) {
            continue;
        }

        if (!message->system) {
            if (have_previous_sender && strcmp(previous_from, message->from) != 0) {
                if (emit != NULL) {
                    emit("", SOLAR_OS_TUI_ATTR_NORMAL, 0, user);
                }
                count++;
            }
            strlcpy(previous_from, message->from, sizeof(previous_from));
            have_previous_sender = true;
        }

        count += chat_emit_wrapped_message(message, width, emit, user);
    }
    return count;
}

static size_t chat_count_visual_rows(size_t width)
{
    return chat_emit_visual_rows(width, NULL, NULL);
}

typedef struct {
    size_t start_col;
    size_t width;
    size_t row;
    size_t first_visible;
    size_t visual_index;
    size_t max_rows;
    size_t drawn;
} chat_draw_visual_ctx_t;

static void chat_draw_visual_line(const char *line,
                                  uint8_t attr,
                                  size_t emphasis_bytes,
                                  void *user)
{
    chat_draw_visual_ctx_t *ctx = (chat_draw_visual_ctx_t *)user;
    if (ctx == NULL) {
        return;
    }

    if (ctx->visual_index >= ctx->first_visible && ctx->drawn < ctx->max_rows) {
        solar_os_tui_write_cell(&chat_app.tui, ctx->row + ctx->drawn, ctx->start_col, ctx->width, line, attr);
        if (emphasis_bytes > 0 && line != NULL) {
            char emphasis[SOLAR_OS_CHAT_USER_MAX];
            const size_t copy_len = emphasis_bytes < sizeof(emphasis) ?
                emphasis_bytes : sizeof(emphasis) - 1U;
            memcpy(emphasis, line, copy_len);
            emphasis[copy_len] = '\0';
            solar_os_tui_addstr(&chat_app.tui,
                                ctx->row + ctx->drawn,
                                ctx->start_col,
                                emphasis,
                                SOLAR_OS_TUI_ATTR_BOLD);
        }
        ctx->drawn++;
    }
    ctx->visual_index++;
}

static void chat_format_channel_label(const chat_app_channel_t *channel,
                                      char *label,
                                      size_t label_len)
{
    if (label == NULL || label_len == 0) {
        return;
    }
    label[0] = '\0';
    if (channel == NULL) {
        return;
    }
    if (!channel->system &&
        (channel->kind == SOLAR_OS_CONVERSATION_GROUP ||
         channel->kind == SOLAR_OS_CONVERSATION_ROOM) &&
        channel->name[0] != '#') {
        snprintf(label, label_len, "#%s", channel->name);
        return;
    }
    strlcpy(label, channel->name, label_len);
}

static bool chat_provider_has_conversations(
    solar_os_messaging_provider_id_t provider)
{
    for (uint8_t i = 0; i < chat_app.channel_count; i++) {
        if (!chat_app.channels[i].system &&
            chat_app.channels[i].provider == provider) {
            return true;
        }
    }
    return false;
}

static size_t chat_build_channel_rows(chat_app_channel_row_t *rows,
                                      size_t max_rows)
{
    if (rows == NULL || max_rows == 0) {
        return 0;
    }
    size_t count = 0;
    for (uint8_t i = 0;
         i < chat_app.channel_count && count < max_rows;
         i++) {
        if (!chat_app.channels[i].system) {
            continue;
        }
        rows[count].heading = false;
        rows[count].channel_index = i;
        chat_format_channel_label(&chat_app.channels[i],
                                  rows[count].label,
                                  sizeof(rows[count].label));
        count++;
    }

    const solar_os_messaging_provider_id_t provider_order[] = {
        SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        SOLAR_OS_MESSAGING_PROVIDER_LINK,
    };
    for (size_t provider = 0;
         provider < sizeof(provider_order) / sizeof(provider_order[0]) &&
         count < max_rows;
         provider++) {
        const solar_os_messaging_provider_id_t provider_id =
            provider_order[provider];
        if (!chat_provider_has_conversations(provider_id)) {
            continue;
        }
        rows[count].heading = true;
        rows[count].channel_index = 0;
        strlcpy(rows[count].label,
                solar_os_messaging_provider_name(provider_id),
                sizeof(rows[count].label));
        count++;
        for (uint8_t i = 0;
             i < chat_app.channel_count && count < max_rows;
             i++) {
            if (chat_app.channels[i].system ||
                chat_app.channels[i].provider != provider_id) {
                continue;
            }
            rows[count].heading = false;
            rows[count].channel_index = i;
            chat_format_channel_label(&chat_app.channels[i],
                                      rows[count].label,
                                      sizeof(rows[count].label));
            count++;
        }
    }
    return count;
}

static size_t chat_find_channel_row(const chat_app_channel_row_t *rows,
                                    size_t row_count,
                                    uint8_t channel_index)
{
    for (size_t i = 0; i < row_count; i++) {
        if (!rows[i].heading && rows[i].channel_index == channel_index) {
            return i;
        }
    }
    return 0;
}

static void chat_draw_tabs(size_t cols)
{
    static const char channels_label[] = " Channels ";
    static const char chat_label[] = " Chat ";

    solar_os_tui_write_cell(&chat_app.tui, 0, 0, cols, "", SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_draw_tab(&chat_app.tui, 0, 0,
                          sizeof(channels_label) - 1U, channels_label,
                          chat_app.tab == CHAT_APP_TAB_CHANNELS);
    solar_os_tui_draw_tab(&chat_app.tui, 0,
                          sizeof(channels_label) - 1U,
                          sizeof(chat_label) - 1U, chat_label,
                          chat_app.tab == CHAT_APP_TAB_CHAT);
}

static void chat_draw_channels(size_t start_row,
                               size_t width,
                               size_t body_rows)
{
    const uint8_t header_attr = SOLAR_OS_TUI_ATTR_BOLD;

    solar_os_tui_write_cell(&chat_app.tui, start_row, 0, width, "conversations", header_attr);

    const size_t row_count =
        chat_build_channel_rows(chat_app.channel_rows,
                                CHAT_APP_CHANNEL_VIEW_COUNT);
    const size_t list_rows = body_rows > 1 ? body_rows - 1 : 0;
    const size_t selected_row =
        chat_find_channel_row(chat_app.channel_rows,
                              row_count,
                              chat_app.selected_channel);
    if (selected_row < chat_app.channel_scroll) {
        chat_app.channel_scroll = (uint8_t)selected_row;
    }
    if (list_rows > 0 &&
        selected_row >= chat_app.channel_scroll + list_rows) {
        chat_app.channel_scroll =
            (uint8_t)(selected_row - list_rows + 1U);
    }

    for (size_t row = 0; row < list_rows; row++) {
        const size_t view_index = chat_app.channel_scroll + row;
        char line[80];
        uint8_t attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (view_index < row_count &&
            chat_app.channel_rows[view_index].heading) {
            snprintf(line,
                     sizeof(line),
                     "  %s",
                     chat_app.channel_rows[view_index].label);
            attr = SOLAR_OS_TUI_ATTR_BOLD;
        } else if (view_index < row_count) {
            const uint8_t channel_index =
                chat_app.channel_rows[view_index].channel_index;
            const chat_app_channel_t *channel =
                &chat_app.channels[channel_index];
            snprintf(line,
                     sizeof(line),
                     channel->system ? "%c%c %s" : "%c%c  %s",
                     channel_index == chat_app.current_channel ? '>' : ' ',
                     channel->unread ? '*' : ' ',
                     chat_app.channel_rows[view_index].label);
            if (channel_index == chat_app.selected_channel) {
                attr = SOLAR_OS_TUI_ATTR_INVERSE;
            } else if (channel_index == chat_app.current_channel || channel->unread) {
                attr = SOLAR_OS_TUI_ATTR_BOLD;
            }
        } else {
            line[0] = '\0';
        }

        solar_os_tui_write_cell(&chat_app.tui, start_row + row + 1U, 0, width, line, attr);
    }
}

static void chat_draw_messages(size_t start_row,
                               size_t start_col,
                               size_t width,
                               size_t body_rows)
{
    char header[128];
    const uint8_t header_attr = SOLAR_OS_TUI_ATTR_BOLD;

    if (chat_current_is_system()) {
        strlcpy(header, "system  messaging", sizeof(header));
    } else {
        char label[SOLAR_OS_MESSAGING_TITLE_MAX + 2U];
        const chat_app_channel_t *channel =
            &chat_app.channels[chat_app.current_channel];
        chat_format_channel_label(channel, label, sizeof(label));
        snprintf(header,
                 sizeof(header),
                 "%s:%s  %s",
                 solar_os_messaging_provider_name(channel->provider),
                 label,
                 chat_provider_state_name(channel->provider));
    }
    solar_os_tui_write_cell(&chat_app.tui, start_row, start_col, width, header, header_attr);

    const size_t text_rows = body_rows > 1 ? body_rows - 1 : 0;

    for (size_t i = 0; i < text_rows; i++) {
        solar_os_tui_write_cell(&chat_app.tui, start_row + 1U + i,
                        start_col,
                        width,
                        "",
                        SOLAR_OS_TUI_ATTR_NORMAL);
    }

    if (text_rows == 0 || chat_app.message_count == 0) {
        return;
    }

    const size_t total_rows = chat_count_visual_rows(width);
    if (total_rows == 0) {
        return;
    }

    const size_t max_scroll = total_rows > text_rows ? total_rows - text_rows : 0;
    if (chat_app.message_scroll > max_scroll) {
        chat_app.message_scroll = max_scroll;
    }

    const size_t first_visible = total_rows > text_rows + chat_app.message_scroll ?
        total_rows - text_rows - chat_app.message_scroll : 0;
    const size_t visible_rows = total_rows > first_visible ?
        total_rows - first_visible < text_rows ? total_rows - first_visible : text_rows : 0;
    if (visible_rows == 0) {
        return;
    }

    chat_draw_visual_ctx_t draw_ctx = {
        .start_col = start_col,
        .width = width,
        .row = start_row + 1U + text_rows - visible_rows,
        .first_visible = first_visible,
        .max_rows = visible_rows,
    };

    (void)chat_emit_visual_rows(width, chat_draw_visual_line, &draw_ctx);
}

static void chat_draw_input(size_t rows, size_t cols)
{
    if (rows < CHAT_APP_INPUT_ROWS) {
        return;
    }

    const size_t sep_row = rows - CHAT_APP_INPUT_ROWS;
    const size_t input_row = rows - 2U;

    solar_os_tui_set_cursor_visible(&chat_app.tui, false);
    solar_os_tui_hline(&chat_app.tui, sep_row, 0, cols, 0, SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_draw_help(
        &chat_app.tui,
        chat_app.status[0] != '\0' ? chat_app.status :
            "TAB channels  ENTER send  /help commands  ESC exits");

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
    if (cols > 2U && chat_app.input != NULL && chat_app.input[chat_app.input_view_offset] != '\0') {
        char visible[CHAT_APP_LINE_MAX];
        const size_t copy_len = chat_safe_clip_len(chat_app.input + chat_app.input_view_offset,
                                                   cols - 2U,
                                                   sizeof(visible) - 1U);
        memcpy(visible, chat_app.input + chat_app.input_view_offset, copy_len);
        visible[copy_len] = '\0';
        solar_os_tui_addstr(&chat_app.tui, input_row, 2, visible, SOLAR_OS_TUI_ATTR_NORMAL);
    }

    const size_t cursor_col = 2U + chat_app.input_cursor - chat_app.input_view_offset;
    solar_os_tui_move(&chat_app.tui,
                      input_row,
                      cursor_col < cols ? cursor_col : cols - 1U);
    solar_os_tui_set_cursor_visible(&chat_app.tui, true);
}

static void chat_draw_channel_footer(size_t rows, size_t cols)
{
    if (rows == 0) {
        return;
    }
    solar_os_tui_set_cursor_visible(&chat_app.tui, false);
    solar_os_tui_draw_help(
        &chat_app.tui,
        chat_app.status[0] != '\0' ? chat_app.status :
            "UP/DOWN select  ENTER join/open  TAB chat  ESC exits");
}

static void chat_render(void)
{
    const size_t rows = solar_os_tui_rows(&chat_app.tui);
    const size_t cols = solar_os_tui_cols(&chat_app.tui);

    if (rows < CHAT_APP_MIN_ROWS || cols < CHAT_APP_MIN_COLS) {
        solar_os_tui_clear(&chat_app.tui);
        solar_os_tui_draw_too_small(&chat_app.tui, "chat");
        solar_os_tui_refresh(&chat_app.tui);
        return;
    }

    solar_os_tui_clear(&chat_app.tui);

    chat_draw_tabs(cols);
    if (chat_app.tab == CHAT_APP_TAB_CHANNELS) {
        const size_t footer_rows = 1U;
        const size_t body_rows = rows > CHAT_APP_TAB_ROWS + footer_rows ?
            rows - CHAT_APP_TAB_ROWS - footer_rows : 0U;
        chat_draw_channels(CHAT_APP_TAB_ROWS, cols, body_rows);
        chat_draw_channel_footer(rows, cols);
    } else {
        const size_t body_rows = rows > CHAT_APP_TAB_ROWS + CHAT_APP_INPUT_ROWS ?
            rows - CHAT_APP_TAB_ROWS - CHAT_APP_INPUT_ROWS : 0U;
        chat_draw_messages(CHAT_APP_TAB_ROWS, 0, cols, body_rows);
        chat_draw_input(rows, cols);
    }
    solar_os_tui_refresh(&chat_app.tui);
    chat_app.redraw = false;
}

static bool chat_gateway_channel_state(const char *provider_key,
                                       bool *desired,
                                       bool *joined)
{
    if (provider_key == NULL || provider_key[0] == '\0') {
        return false;
    }
    solar_os_chat_channel_t *channels = solar_os_memory_calloc(
        SOLAR_OS_CHAT_CHANNEL_CAPACITY,
        sizeof(*channels),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "chat.gateway_channels");
    if (channels == NULL) {
        return false;
    }
    const size_t count = solar_os_chat_channel_snapshot(
        channels,
        SOLAR_OS_CHAT_CHANNEL_CAPACITY);
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(channels[i].name, provider_key) == 0) {
            *desired = channels[i].desired;
            *joined = channels[i].joined;
            found = true;
            break;
        }
    }
    solar_os_memory_free(channels);
    return found;
}

static bool chat_join_channel(const chat_app_channel_t *channel)
{
    if (channel == NULL || channel->system ||
        channel->provider != SOLAR_OS_MESSAGING_PROVIDER_GATEWAY ||
        (channel->kind != SOLAR_OS_CONVERSATION_ROOM &&
         channel->kind != SOLAR_OS_CONVERSATION_GROUP)) {
        return true;
    }
    const char *provider_key = channel->provider_key[0] != '\0' ?
        channel->provider_key : channel->name;
    bool desired = false;
    bool joined = false;
    (void)chat_gateway_channel_state(provider_key, &desired, &joined);
    if (joined) {
        chat_app.gateway_join_pending = false;
        return true;
    }
    chat_app.gateway_join_pending = true;
    chat_app.gateway_join_conversation_id = channel->conversation_id;
    strlcpy(chat_app.gateway_join_provider_key,
            provider_key,
            sizeof(chat_app.gateway_join_provider_key));
    if (desired) {
        chat_set_status("waiting for join confirmation");
        return false;
    }
    const esp_err_t error = solar_os_chat_join(provider_key);
    if (error != ESP_OK) {
        char status[CHAT_APP_STATUS_MAX];
        snprintf(status,
                 sizeof(status),
                 "join failed: %s",
                 esp_err_to_name(error));
        chat_set_status(status);
        chat_app.gateway_join_pending = false;
        return false;
    }
    char label[SOLAR_OS_MESSAGING_TITLE_MAX + 2U];
    char status[CHAT_APP_STATUS_MAX];
    chat_format_channel_label(channel, label, sizeof(label));
    snprintf(status, sizeof(status), "joining %s", label);
    chat_set_status(status);
    return false;
}

static void chat_select_channel(uint8_t index, bool join)
{
    if (index >= chat_app.channel_count) {
        return;
    }
    if (join) {
        chat_set_status("");
        if (!chat_join_channel(&chat_app.channels[index])) {
            return;
        }
    }
    chat_app.selected_channel = index;
    chat_app.current_channel = index;
    chat_app.channels[index].unread = false;
    chat_app.channels[index].unread_count = 0;
    if (!chat_app.channels[index].system) {
        (void)solar_os_messaging_mark_read(
            chat_app.channels[index].conversation_id);
    }
    chat_app.message_scroll = 0;
    if (join) {
        chat_app.tab = CHAT_APP_TAB_CHAT;
    }
    chat_app.redraw = true;

    if (chat_app.channels[index].system) {
        chat_set_status("system events");
    }
}

static void chat_check_pending_gateway_join(void)
{
    if (!chat_app.gateway_join_pending) {
        return;
    }
    bool desired = false;
    bool joined = false;
    if (!chat_gateway_channel_state(chat_app.gateway_join_provider_key,
                                    &desired,
                                    &joined) ||
        !joined) {
        if (!desired) {
            chat_app.gateway_join_pending = false;
            chat_set_status("join was not confirmed");
        }
        return;
    }

    chat_app.gateway_join_pending = false;
    for (uint8_t i = 0; i < chat_app.channel_count; i++) {
        if (chat_app.channels[i].conversation_id ==
                chat_app.gateway_join_conversation_id ||
            strcmp(chat_app.channels[i].provider_key,
                   chat_app.gateway_join_provider_key) == 0) {
            chat_select_channel(i, false);
            chat_app.tab = CHAT_APP_TAB_CHAT;
            chat_set_status("joined");
            return;
        }
    }
    chat_set_status("joined room is unavailable");
}

static void chat_handle_messaging_event(
    const solar_os_messaging_event_t *event)
{
    if (event == NULL) {
        return;
    }
    chat_app.messaging_dirty = true;
}

static void chat_drain_messaging_events(void)
{
    if (chat_app.messaging_event == NULL) {
        return;
    }
    for (size_t i = 0; i < CHAT_APP_DRAIN_EVENTS_PER_TICK; i++) {
        const esp_err_t error = solar_os_messaging_read_event_after(
            &chat_app.messaging_event_cursor,
            chat_app.messaging_event);
        if (error != ESP_OK) {
            break;
        }
        chat_handle_messaging_event(chat_app.messaging_event);
    }
}

static void chat_check_messaging_generation(void)
{
    solar_os_messaging_status_t status;
    if (solar_os_messaging_get_status(&status) == ESP_OK &&
        status.generation != chat_app.messaging_generation) {
        chat_app.messaging_dirty = true;
    }
    if (chat_app.messaging_dirty) {
        chat_reconcile_messaging();
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

static void chat_show_status(void)
{
    solar_os_messaging_status_t status;
    if (solar_os_messaging_get_status(&status) != ESP_OK) {
        chat_append_statusf("messaging service unavailable");
        return;
    }
    chat_append_statusf("current: %s", chat_current_channel_name());
    chat_append_statusf("conversations=%u messages=%u unread=%u outbox=%u",
                        (unsigned)status.conversations,
                        (unsigned)status.messages,
                        (unsigned)status.unread,
                        (unsigned)status.queued_outbox);
    chat_append_statusf("history: %s cap=%u",
                        status.persistent ?
                            (status.inbox_backed ? "compact internal" : "full") :
                            "volatile",
                        (unsigned)status.persistent_capacity);
    for (solar_os_messaging_provider_id_t provider =
             SOLAR_OS_MESSAGING_PROVIDER_GATEWAY;
         provider <= SOLAR_OS_MESSAGING_PROVIDER_LINK;
         provider++) {
        solar_os_messaging_provider_status_t provider_status;
        if (solar_os_messaging_provider_get_status(
                provider, &provider_status) == ESP_OK) {
            chat_append_statusf("%s: %s",
                                provider_status.name,
                                chat_provider_state_name(provider));
        }
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
        chat_append_statusf("/new contact-id");
        chat_append_statusf("/status");
        chat_append_statusf("/quit");
    } else if (strcasecmp(argv[0], "/new") == 0) {
        if (argc != 2) {
            chat_set_status("usage: /new contact-id");
            return;
        }
        char *end = NULL;
        const unsigned long contact_id = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || contact_id == 0 ||
            contact_id > UINT32_MAX) {
            chat_set_status("invalid contact id");
            return;
        }
        solar_os_conversation_id_t conversation_id = 0;
        const esp_err_t error = solar_os_messaging_direct_open(
            (solar_os_contact_id_t)contact_id,
            &conversation_id);
        if (error != ESP_OK) {
            chat_set_status(esp_err_to_name(error));
            return;
        }
        chat_refresh_conversations();
        for (uint8_t i = 0; i < chat_app.channel_count; i++) {
            if (chat_app.channels[i].conversation_id == conversation_id) {
                chat_select_channel(i, false);
                break;
            }
        }
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

    if (chat_app.input == NULL || chat_app.input_len == 0) {
        return;
    }

    char *line = solar_os_memory_alloc(CHAT_APP_INPUT_MAX,
                                        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                        "chat.input");
    if (line == NULL) {
        chat_set_status("input alloc failed");
        return;
    }

    strlcpy(line, chat_app.input, CHAT_APP_INPUT_MAX);
    chat_history_add(line);
    chat_history_cancel();
    chat_set_input("");
    chat_app.message_scroll = 0;
    chat_app.redraw = true;

    if (line[0] == '/') {
        chat_execute_command(line);
        solar_os_memory_free(line);
        return;
    }
    if (chat_current_is_system()) {
        chat_set_status("system is read-only");
        solar_os_memory_free(line);
        return;
    }

    const bool confirmed =
        chat_app.confirm_untrusted &&
        chat_app.pending_untrusted != NULL &&
        strcmp(chat_app.pending_untrusted, line) == 0;
    const esp_err_t err =
        solar_os_messaging_send(chat_current_conversation_id(),
                                line,
                                confirmed,
                                NULL);
    if (err == ESP_ERR_INVALID_STATE && !confirmed &&
        chat_current_is_discovered_direct() &&
        chat_app.pending_untrusted != NULL) {
        strlcpy(chat_app.pending_untrusted, line, CHAT_APP_INPUT_MAX);
        chat_app.confirm_untrusted = true;
        chat_set_input(line);
        chat_set_status(
            "discovered endpoint; press Enter again to send once");
        solar_os_memory_free(line);
        return;
    }
    chat_app.confirm_untrusted = false;
    if (chat_app.pending_untrusted != NULL) {
        chat_app.pending_untrusted[0] = '\0';
    }
    solar_os_memory_free(line);
    if (err == ESP_OK) {
        chat_set_status("queued");
    } else {
        chat_set_status(esp_err_to_name(err));
        chat_append_statusf("send failed: %s", esp_err_to_name(err));
    }
}

static void chat_insert_char(char ch)
{
    if (chat_app.input == NULL) {
        return;
    }
    if (chat_app.input_len + 1U >= CHAT_APP_INPUT_MAX) {
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
    if (chat_app.input == NULL) {
        return;
    }
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
    if (chat_app.input == NULL) {
        return;
    }
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

static size_t chat_message_scroll_step(void)
{
    const size_t rows = solar_os_tui_rows(&chat_app.tui);
    const size_t reserved_rows = CHAT_APP_TAB_ROWS + CHAT_APP_INPUT_ROWS;
    const size_t body_rows = rows > reserved_rows ? rows - reserved_rows : rows;
    const size_t text_rows = body_rows > 1U ? body_rows - 1U : 1U;

    return text_rows > 1U ? text_rows - 1U : 1U;
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
        {
            const size_t step = chat_message_scroll_step();
            chat_app.message_scroll = SIZE_MAX - chat_app.message_scroll >= step ?
                chat_app.message_scroll + step : SIZE_MAX;
            chat_app.redraw = true;
        }
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        {
            const size_t step = chat_message_scroll_step();
            chat_app.message_scroll = chat_app.message_scroll > step ?
                chat_app.message_scroll - step : 0;
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
    const size_t row_count =
        chat_build_channel_rows(chat_app.channel_rows,
                                CHAT_APP_CHANNEL_VIEW_COUNT);
    const size_t selected_row =
        chat_find_channel_row(chat_app.channel_rows,
                              row_count,
                              chat_app.selected_channel);

    switch (ch) {
    case SOLAR_OS_KEY_UP:
        for (size_t row = selected_row; row > 0; row--) {
            if (!chat_app.channel_rows[row - 1U].heading) {
                chat_app.selected_channel =
                    chat_app.channel_rows[row - 1U].channel_index;
                chat_app.redraw = true;
                break;
            }
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        for (size_t row = selected_row + 1U; row < row_count; row++) {
            if (!chat_app.channel_rows[row].heading) {
                chat_app.selected_channel =
                    chat_app.channel_rows[row].channel_index;
                chat_app.redraw = true;
                break;
            }
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

static void *chat_app_calloc(size_t count, size_t size)
{
    return solar_os_memory_calloc(count,
                                  size,
                                  SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                  "chat.app");
}

static chat_app_state_t *chat_app_alloc_state(void)
{
    return chat_app_calloc(1, sizeof(chat_app_state_t));
}

static void chat_app_free_state(void)
{
    solar_os_memory_free(chat_app_state);
    chat_app_state = NULL;
}

static void chat_free_buffers(void)
{
    if (chat_app.messages != NULL) {
        solar_os_memory_free(chat_app.messages);
        chat_app.messages = NULL;
    }
    if (chat_app.history != NULL) {
        solar_os_memory_free(chat_app.history);
        chat_app.history = NULL;
    }
    if (chat_app.input != NULL) {
        solar_os_memory_free(chat_app.input);
        chat_app.input = NULL;
    }
    if (chat_app.history_draft != NULL) {
        solar_os_memory_free(chat_app.history_draft);
        chat_app.history_draft = NULL;
    }
    if (chat_app.messaging_event != NULL) {
        solar_os_memory_free(chat_app.messaging_event);
        chat_app.messaging_event = NULL;
    }
    if (chat_app.pending_untrusted != NULL) {
        solar_os_memory_free(chat_app.pending_untrusted);
        chat_app.pending_untrusted = NULL;
    }
}

static bool chat_parse_selector(const char *selector)
{
    if (selector == NULL || selector[0] == '\0') {
        return false;
    }
    if (strcasecmp(selector, "gateway") == 0) {
        chat_app.filter_provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY;
        return true;
    }
    if (strcasecmp(selector, "meshcore") == 0) {
        chat_app.filter_provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE;
        return true;
    }
    if (strcasecmp(selector, "link") == 0) {
        chat_app.filter_provider = SOLAR_OS_MESSAGING_PROVIDER_LINK;
        return true;
    }
    char *end = NULL;
    const unsigned long id = strtoul(selector, &end, 10);
    if (end == selector || *end != '\0' || id == 0U || id > UINT32_MAX) {
        return false;
    }
    solar_os_messaging_conversation_t conversation;
    if (solar_os_messaging_conversation_get(
            (solar_os_conversation_id_t)id, &conversation) != ESP_OK) {
        return false;
    }
    chat_app.initial_conversation_id = (solar_os_conversation_id_t)id;
    return true;
}

static esp_err_t chat_start(solar_os_context_t *ctx)
{
    chat_app_state = chat_app_alloc_state();
    if (chat_app_state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    chat_app.tab = CHAT_APP_TAB_CHANNELS;
    chat_app.history_index = -1;
    chat_app.redraw = true;
    const esp_err_t messaging_error = solar_os_messaging_init();
    if (messaging_error != ESP_OK) {
        chat_app_free_state();
        return messaging_error;
    }
    const int argc = solar_os_context_argc(ctx);
    if (argc > 2 ||
        (argc == 2 && !chat_parse_selector(solar_os_context_argv(ctx, 1)))) {
        chat_app_free_state();
        return ESP_ERR_INVALID_ARG;
    }
    if (chat_app.initial_conversation_id != 0U) {
        chat_app.tab = CHAT_APP_TAB_CHAT;
    }

    const esp_err_t tui_err = solar_os_tui_screen_begin(&chat_app.tui, ctx);
    if (tui_err != ESP_OK) {
        chat_app_free_state();
        return tui_err;
    }

    chat_app.messages = chat_app_calloc(CHAT_APP_MESSAGE_COUNT, sizeof(chat_app_message_t));
    chat_app.history = chat_app_calloc(CHAT_APP_HISTORY_COUNT, sizeof(chat_app.history[0]));
    chat_app.input = chat_app_calloc(CHAT_APP_INPUT_MAX, 1);
    chat_app.history_draft = chat_app_calloc(CHAT_APP_INPUT_MAX, 1);
    chat_app.pending_untrusted = chat_app_calloc(CHAT_APP_INPUT_MAX, 1);
    chat_app.messaging_event =
        chat_app_calloc(1, sizeof(*chat_app.messaging_event));
    if (chat_app.messages == NULL ||
        chat_app.history == NULL ||
        chat_app.input == NULL ||
        chat_app.history_draft == NULL ||
        chat_app.pending_untrusted == NULL ||
        chat_app.messaging_event == NULL) {
        chat_free_buffers();
        solar_os_tui_end(&chat_app.tui);
        chat_app_free_state();
        return ESP_ERR_NO_MEM;
    }

    (void)chat_add_system_channel();
    chat_app.initial_selection_pending = true;
    chat_refresh_conversations();
    chat_app.active = true;
    chat_reconcile_messaging();
    chat_app.channels[chat_app.current_channel].unread = false;
    chat_set_status(chat_app.initial_conversation_id != 0U ?
        "conversation view" :
        chat_app.filter_provider != 0U ? "provider view" : "unified view");
    chat_render();
    return ESP_OK;
}

static void chat_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    solar_os_tui_set_cursor_visible(&chat_app.tui, true);
    solar_os_tui_refresh(&chat_app.tui);
    solar_os_tui_end(&chat_app.tui);
    chat_free_buffers();
    chat_app_free_state();
}

static void chat_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    if (chat_app_state != NULL) {
        chat_app.suspended = true;
    }
}

static void chat_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    if (chat_app_state == NULL) {
        return;
    }
    chat_app.suspended = false;
    chat_app.redraw = true;
    chat_render();
}

static void chat_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (chat_app_state != NULL) {
        snprintf(buffer, buffer_len, "chat %s", chat_current_channel_name());
        return;
    }
    strlcpy(buffer, "chat", buffer_len);
}

static bool chat_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        chat_drain_messaging_events();
        chat_check_messaging_generation();
        chat_check_pending_gateway_join();
        if (!chat_app.active) {
            solar_os_context_request_exit(ctx);
            return true;
        }
        if (!chat_app.suspended && chat_app.redraw) {
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
        chat_app.tab = chat_app.tab == CHAT_APP_TAB_CHANNELS ?
            CHAT_APP_TAB_CHAT : CHAT_APP_TAB_CHANNELS;
        chat_app.redraw = true;
        return true;
    }

    if (chat_app.tab == CHAT_APP_TAB_CHANNELS) {
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
    .summary = "provider-neutral conversation client",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = chat_start,
    .suspend = chat_suspend,
    .resume = chat_resume,
    .stop = chat_stop,
    .event = chat_event,
    .title = chat_title,
};
