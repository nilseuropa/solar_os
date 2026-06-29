#include "solar_os_chatd_job.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_chat.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define CHATD_DEFAULT_PORT 7777U
#define CHATD_MAX_CLIENTS 6U
#define CHATD_MAX_CHANNELS 32U
#define CHATD_LINE_MAX (SOLAR_OS_CHAT_TEXT_MAX * 6U + 768U)
#define CHATD_RX_CHUNK_SIZE 384U
#define CHATD_TASK_STACK 8192U
#define CHATD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define CHATD_SELECT_TIMEOUT_MS 250U
#define CHATD_STOP_WAIT_MS 2000U
#define CHATD_DEFAULT_CHANNEL "general"
#define CHATD_HISTORY_COUNT 64U
#define CHATD_HISTORY_TYPE_MAX 16U

static const char *TAG = "solar_os_chatd";

typedef struct {
    bool active;
    bool authed;
    int fd;
    uint32_t joined_mask;
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char peer[40];
    char *line;
    size_t line_len;
    uint32_t rx_count;
    uint32_t tx_count;
} chatd_client_t;

typedef struct {
    bool valid;
    char type[CHATD_HISTORY_TYPE_MAX];
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX];
    char from[SOLAR_OS_CHAT_USER_MAX];
    char text[SOLAR_OS_CHAT_TEXT_MAX];
    uint64_t timestamp;
} chatd_history_entry_t;

typedef struct {
    bool running;
    volatile bool stop_requested;
    TaskHandle_t task;
    int listen_fd;
    uint16_t port;
    bool token_set;
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char channels[CHATD_MAX_CHANNELS][SOLAR_OS_CHAT_CHANNEL_MAX];
    size_t channel_count;
    chatd_client_t clients[CHATD_MAX_CLIENTS];
    char *tx_line;
    char *text_arg;
    chatd_history_entry_t *history;
    size_t history_head;
    size_t history_count;
    FILE *history_file;
    char history_path[SOLAR_OS_STORAGE_PATH_MAX];
    uint32_t connection_count;
    uint32_t message_count;
    uint32_t dropped_count;
    esp_err_t last_error;
} chatd_job_state_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool ok;
} chatd_json_builder_t;

static chatd_job_state_t chatd_job = {
    .listen_fd = -1,
    .last_error = ESP_OK,
};

static uint64_t chatd_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void chatd_builder_init(chatd_json_builder_t *builder, char *data, size_t cap)
{
    builder->data = data;
    builder->len = 0;
    builder->cap = cap;
    builder->ok = data != NULL && cap > 0;
    if (builder->ok) {
        builder->data[0] = '\0';
    }
}

static void chatd_builder_putn(chatd_json_builder_t *builder, const char *text, size_t len)
{
    if (builder == NULL || !builder->ok || text == NULL) {
        return;
    }
    if (len >= builder->cap || builder->len >= builder->cap - len) {
        builder->ok = false;
        return;
    }
    memcpy(builder->data + builder->len, text, len);
    builder->len += len;
    builder->data[builder->len] = '\0';
}

static void chatd_builder_put(chatd_json_builder_t *builder, const char *text)
{
    chatd_builder_putn(builder, text, text != NULL ? strlen(text) : 0);
}

static void chatd_builder_putc(chatd_json_builder_t *builder, char ch)
{
    chatd_builder_putn(builder, &ch, 1);
}

static void chatd_builder_put_u64(chatd_json_builder_t *builder, uint64_t value)
{
    char tmp[24];
    const int written = snprintf(tmp, sizeof(tmp), "%" PRIu64, value);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        builder->ok = false;
        return;
    }
    chatd_builder_putn(builder, tmp, (size_t)written);
}

static void chatd_builder_put_json_string(chatd_json_builder_t *builder, const char *text)
{
    static const char hex[] = "0123456789abcdef";

    chatd_builder_putc(builder, '"');
    if (text != NULL) {
        for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
            switch (*p) {
            case '"':
                chatd_builder_put(builder, "\\\"");
                break;
            case '\\':
                chatd_builder_put(builder, "\\\\");
                break;
            case '\b':
                chatd_builder_put(builder, "\\b");
                break;
            case '\f':
                chatd_builder_put(builder, "\\f");
                break;
            case '\n':
                chatd_builder_put(builder, "\\n");
                break;
            case '\r':
                chatd_builder_put(builder, "\\r");
                break;
            case '\t':
                chatd_builder_put(builder, "\\t");
                break;
            default:
                if (*p < 0x20) {
                    char escaped[7] = {
                        '\\',
                        'u',
                        '0',
                        '0',
                        hex[*p >> 4],
                        hex[*p & 0x0f],
                        '\0',
                    };
                    chatd_builder_putn(builder, escaped, 6);
                } else {
                    chatd_builder_putc(builder, (char)*p);
                }
                break;
            }
        }
    }
    chatd_builder_putc(builder, '"');
}

static void chatd_builder_put_field(chatd_json_builder_t *builder,
                                    const char *name,
                                    const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return;
    }
    chatd_builder_put(builder, ",\"");
    chatd_builder_put(builder, name);
    chatd_builder_put(builder, "\":");
    chatd_builder_put_json_string(builder, value);
}

static bool chatd_build_event_at(chatd_job_state_t *state,
                                 const char *type,
                                 const char *channel,
                                 const char *from,
                                 const char *text,
                                 bool include_ts,
                                 uint64_t timestamp,
                                 int code,
                                 bool include_code)
{
    chatd_json_builder_t builder;
    chatd_builder_init(&builder, state->tx_line, CHATD_LINE_MAX);
    chatd_builder_put(&builder, "{\"type\":");
    chatd_builder_put_json_string(&builder, type);
    chatd_builder_put_field(&builder, "channel", channel);
    chatd_builder_put_field(&builder, "from", from);
    chatd_builder_put_field(&builder, "text", text);
    if (include_ts) {
        chatd_builder_put(&builder, ",\"ts\":");
        chatd_builder_put_u64(&builder, timestamp);
    }
    if (include_code) {
        chatd_builder_put(&builder, ",\"code\":");
        chatd_builder_put_u64(&builder, (uint64_t)code);
    }
    chatd_builder_put(&builder, "}\n");
    return builder.ok;
}

static bool chatd_build_event(chatd_job_state_t *state,
                              const char *type,
                              const char *channel,
                              const char *from,
                              const char *text,
                              bool include_ts,
                              int code,
                              bool include_code)
{
    return chatd_build_event_at(state,
                                type,
                                channel,
                                from,
                                text,
                                include_ts,
                                chatd_now_ms(),
                                code,
                                include_code);
}

static bool chatd_build_channel_event(chatd_job_state_t *state, const char *channel)
{
    chatd_json_builder_t builder;
    chatd_builder_init(&builder, state->tx_line, CHATD_LINE_MAX);
    chatd_builder_put(&builder, "{\"type\":\"channel\",\"name\":");
    chatd_builder_put_json_string(&builder, channel);
    chatd_builder_put(&builder, "}\n");
    return builder.ok;
}

static bool chatd_send_raw(chatd_client_t *client, const char *line)
{
    if (client == NULL || !client->active || line == NULL) {
        return false;
    }

    const char *p = line;
    size_t remaining = strlen(line);
    while (remaining > 0) {
        const ssize_t sent = send(client->fd, p, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }
        p += sent;
        remaining -= (size_t)sent;
    }
    client->tx_count++;
    return true;
}

static bool chatd_send_event(chatd_job_state_t *state,
                             chatd_client_t *client,
                             const char *type,
                             const char *channel,
                             const char *from,
                             const char *text,
                             bool include_ts,
                             int code,
                             bool include_code)
{
    return chatd_build_event(state, type, channel, from, text, include_ts, code, include_code) &&
        chatd_send_raw(client, state->tx_line);
}

static bool chatd_send_channel_event(chatd_job_state_t *state,
                                     chatd_client_t *client,
                                     const char *channel)
{
    return chatd_build_channel_event(state, channel) && chatd_send_raw(client, state->tx_line);
}

static void chatd_close_client(chatd_job_state_t *state, size_t index, bool notify);

static void chatd_store_history(chatd_job_state_t *state,
                                const char *type,
                                const char *channel,
                                const char *from,
                                const char *text,
                                uint64_t timestamp)
{
    if (state == NULL || state->history == NULL || type == NULL || channel == NULL ||
        channel[0] == '\0') {
        return;
    }

    const size_t index = state->history_head;
    chatd_history_entry_t *entry = &state->history[index];
    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    strlcpy(entry->type, type, sizeof(entry->type));
    strlcpy(entry->channel, channel, sizeof(entry->channel));
    if (from != NULL) {
        strlcpy(entry->from, from, sizeof(entry->from));
    }
    if (text != NULL) {
        strlcpy(entry->text, text, sizeof(entry->text));
    }
    entry->timestamp = timestamp;

    state->history_head = (state->history_head + 1U) % CHATD_HISTORY_COUNT;
    if (state->history_count < CHATD_HISTORY_COUNT) {
        state->history_count++;
    }
}

static void chatd_drop_channel_history(chatd_job_state_t *state, const char *channel)
{
    if (state == NULL || state->history == NULL || channel == NULL || channel[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < CHATD_HISTORY_COUNT; i++) {
        chatd_history_entry_t *entry = &state->history[i];
        if (entry->valid && strcmp(entry->channel, channel) == 0) {
            entry->valid = false;
        }
    }
}

static void chatd_append_history_dump(chatd_job_state_t *state, const char *line)
{
    if (state == NULL || state->history_file == NULL || line == NULL) {
        return;
    }

    fputs(line, state->history_file);
    fflush(state->history_file);
}

static void chatd_send_history(chatd_job_state_t *state,
                               chatd_client_t *client,
                               size_t channel_index)
{
    if (state == NULL || client == NULL || state->history == NULL ||
        channel_index >= state->channel_count || state->history_count == 0) {
        return;
    }

    const char *channel = state->channels[channel_index];
    const size_t oldest =
        state->history_count == CHATD_HISTORY_COUNT ? state->history_head : 0U;
    for (size_t logical = 0; logical < state->history_count; logical++) {
        const size_t index = (oldest + logical) % CHATD_HISTORY_COUNT;
        const chatd_history_entry_t *entry = &state->history[index];
        if (!entry->valid || strcmp(entry->channel, channel) != 0) {
            continue;
        }
        if (!chatd_build_event_at(state,
                                  entry->type,
                                  entry->channel,
                                  entry->from,
                                  entry->text,
                                  true,
                                  entry->timestamp,
                                  0,
                                  false) ||
            !chatd_send_raw(client, state->tx_line)) {
            state->dropped_count++;
            break;
        }
    }
}

static void chatd_broadcast_event(chatd_job_state_t *state,
                                  size_t channel_index,
                                  const char *type,
                                  const char *from,
                                  const char *text,
                                  bool include_ts,
                                  int exclude_index)
{
    if (state == NULL || channel_index >= state->channel_count) {
        return;
    }
    const uint64_t timestamp = include_ts ? chatd_now_ms() : 0;
    const char *channel = state->channels[channel_index];
    if (!chatd_build_event_at(state,
                              type,
                              channel,
                              from,
                              text,
                              include_ts,
                              timestamp,
                              0,
                              false)) {
        state->dropped_count++;
        return;
    }
    if (include_ts) {
        chatd_store_history(state, type, channel, from, text, timestamp);
    }
    chatd_append_history_dump(state, state->tx_line);

    const uint32_t mask = 1UL << channel_index;
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        chatd_client_t *client = &state->clients[i];
        if (!client->active || !client->authed || (client->joined_mask & mask) == 0 ||
            (exclude_index >= 0 && (size_t)exclude_index == i)) {
            continue;
        }
        if (!chatd_send_raw(client, state->tx_line)) {
            state->dropped_count++;
            chatd_close_client(state, i, false);
        }
    }
}

static void chatd_broadcast_global_event(chatd_job_state_t *state,
                                         const char *type,
                                         const char *channel,
                                         const char *from,
                                         const char *text,
                                         bool include_ts,
                                         int exclude_index)
{
    if (state == NULL) {
        return;
    }

    const uint64_t timestamp = include_ts ? chatd_now_ms() : 0;
    if (!chatd_build_event_at(state,
                              type,
                              channel,
                              from,
                              text,
                              include_ts,
                              timestamp,
                              0,
                              false)) {
        state->dropped_count++;
        return;
    }
    if (include_ts && channel != NULL && channel[0] != '\0') {
        chatd_store_history(state, type, channel, from, text, timestamp);
    }
    chatd_append_history_dump(state, state->tx_line);

    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        chatd_client_t *client = &state->clients[i];
        if (!client->active || !client->authed ||
            (exclude_index >= 0 && (size_t)exclude_index == i)) {
            continue;
        }
        if (!chatd_send_raw(client, state->tx_line)) {
            state->dropped_count++;
            chatd_close_client(state, i, false);
        }
    }
}

static void chatd_broadcast_channel(chatd_job_state_t *state, size_t channel_index)
{
    if (state == NULL || channel_index >= state->channel_count) {
        return;
    }
    if (!chatd_build_channel_event(state, state->channels[channel_index])) {
        state->dropped_count++;
        return;
    }

    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        chatd_client_t *client = &state->clients[i];
        if (!client->active || !client->authed) {
            continue;
        }
        if (!chatd_send_raw(client, state->tx_line)) {
            state->dropped_count++;
            chatd_close_client(state, i, false);
        }
    }
}

static bool chatd_text_arg_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if (len >= max_len || (!allow_empty && len == 0)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool chatd_channel_valid(const char *channel)
{
    if (!chatd_text_arg_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)channel; *p != '\0'; p++) {
        if (isspace(*p) || *p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static const char *chatd_skip_ws(const char *p)
{
    while (p != NULL && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *chatd_parse_json_string(const char *p,
                                           char *out,
                                           size_t out_len,
                                           bool *truncated)
{
    if (p == NULL || *p != '"' || out == NULL || out_len == 0) {
        return NULL;
    }
    if (truncated != NULL) {
        *truncated = false;
    }

    p++;
    size_t out_pos = 0;
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[out_pos] = '\0';
            return p;
        }
        if (ch == '\\') {
            ch = (unsigned char)*p++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u':
                if (!isxdigit((unsigned char)p[0]) ||
                    !isxdigit((unsigned char)p[1]) ||
                    !isxdigit((unsigned char)p[2]) ||
                    !isxdigit((unsigned char)p[3])) {
                    return NULL;
                }
                p += 4;
                ch = '?';
                break;
            default:
                return NULL;
            }
        }
        if (out_pos + 1U < out_len) {
            out[out_pos++] = (char)ch;
        } else if (truncated != NULL) {
            *truncated = true;
        }
    }
    return NULL;
}

static const char *chatd_skip_json_value(const char *p)
{
    p = chatd_skip_ws(p);
    if (p == NULL) {
        return NULL;
    }
    if (*p == '"') {
        char scratch[2];
        return chatd_parse_json_string(p, scratch, sizeof(scratch), NULL);
    }
    if (*p == '{' || *p == '[') {
        const char open = *p++;
        const char close = open == '{' ? '}' : ']';
        int depth = 1;
        while (*p != '\0') {
            if (*p == '"') {
                char scratch[2];
                p = chatd_parse_json_string(p, scratch, sizeof(scratch), NULL);
                if (p == NULL) {
                    return NULL;
                }
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    while (*p != '\0' && *p != ',' && *p != '}') {
        p++;
    }
    return p;
}

static bool chatd_json_get_string(const char *json,
                                  const char *key,
                                  char *out,
                                  size_t out_len)
{
    if (json == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *p = chatd_skip_ws(json);
    if (p == NULL || *p != '{') {
        return false;
    }
    p++;

    while (*p != '\0') {
        p = chatd_skip_ws(p);
        if (*p == '}') {
            return false;
        }

        char member[32];
        p = chatd_parse_json_string(p, member, sizeof(member), NULL);
        if (p == NULL) {
            return false;
        }
        p = chatd_skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = chatd_skip_ws(p + 1);

        if (strcmp(member, key) == 0 && *p == '"') {
            return chatd_parse_json_string(p, out, out_len, NULL) != NULL;
        }

        p = chatd_skip_json_value(p);
        if (p == NULL) {
            return false;
        }
        p = chatd_skip_ws(p);
        if (*p == ',') {
            p++;
        } else if (*p == '}') {
            return false;
        }
    }
    return false;
}

static int chatd_find_channel(chatd_job_state_t *state, const char *channel)
{
    if (state == NULL || channel == NULL) {
        return -1;
    }
    for (size_t i = 0; i < state->channel_count; i++) {
        if (strcmp(state->channels[i], channel) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int chatd_add_channel(chatd_job_state_t *state, const char *channel, bool *created)
{
    if (created != NULL) {
        *created = false;
    }
    if (!chatd_channel_valid(channel)) {
        return -1;
    }

    const int existing = chatd_find_channel(state, channel);
    if (existing >= 0) {
        return existing;
    }
    if (state->channel_count >= CHATD_MAX_CHANNELS) {
        return -1;
    }

    const size_t index = state->channel_count++;
    strlcpy(state->channels[index], channel, sizeof(state->channels[index]));
    if (created != NULL) {
        *created = true;
    }
    return (int)index;
}

static void chatd_make_from(chatd_client_t *client)
{
    if (client == NULL) {
        return;
    }
    if (client->device[0] != '\0') {
        snprintf(client->from, sizeof(client->from), "%s@%s", client->user, client->device);
    } else {
        strlcpy(client->from, client->user, sizeof(client->from));
    }
}

static void chatd_client_join(chatd_job_state_t *state, size_t client_index, const char *channel)
{
    chatd_client_t *client = &state->clients[client_index];
    bool created = false;
    const int channel_index = chatd_add_channel(state, channel, &created);
    if (channel_index < 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "invalid or full channel",
                               false,
                               400,
                               true);
        return;
    }

    if (created) {
        chatd_broadcast_channel(state, (size_t)channel_index);
    } else {
        (void)chatd_send_channel_event(state, client, state->channels[channel_index]);
    }

    const uint32_t mask = 1UL << (uint32_t)channel_index;
    if ((client->joined_mask & mask) != 0) {
        (void)chatd_send_event(state,
                               client,
                               "joined",
                               state->channels[channel_index],
                               client->from,
                               "joined",
                               true,
                               0,
                               false);
        return;
    }

    client->joined_mask |= mask;
    (void)chatd_send_event(state,
                           client,
                           "joined",
                           state->channels[channel_index],
                           client->from,
                           "joined",
                           true,
                           0,
                           false);
    chatd_send_history(state, client, (size_t)channel_index);
    chatd_broadcast_event(state,
                          (size_t)channel_index,
                          "presence",
                          client->from,
                          "joined",
                          true,
                          (int)client_index);
}

static uint32_t chatd_mask_without_channel(uint32_t mask, size_t channel_index)
{
    const uint32_t lower_mask = channel_index == 0 ? 0U : ((1UL << channel_index) - 1U);
    const uint32_t lower = mask & lower_mask;
    const uint32_t upper = channel_index + 1U >= 32U ? 0U : mask >> (channel_index + 1U);
    return lower | (upper << channel_index);
}

static void chatd_client_leave(chatd_job_state_t *state, size_t client_index, const char *channel)
{
    chatd_client_t *client = &state->clients[client_index];
    const int channel_index = chatd_find_channel(state, channel);
    if (channel_index < 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "unknown channel",
                               false,
                               404,
                               true);
        return;
    }

    const uint32_t mask = 1UL << (uint32_t)channel_index;
    if ((client->joined_mask & mask) == 0) {
        (void)chatd_send_event(state,
                               client,
                               "left",
                               state->channels[channel_index],
                               client->from,
                               "left",
                               true,
                               0,
                               false);
        return;
    }

    client->joined_mask &= ~mask;
    (void)chatd_send_event(state,
                           client,
                           "left",
                           state->channels[channel_index],
                           client->from,
                           "left",
                           true,
                           0,
                           false);
    chatd_broadcast_event(state,
                          (size_t)channel_index,
                          "presence",
                          client->from,
                          "left",
                          true,
                          (int)client_index);
}

static void chatd_client_delete_channel(chatd_job_state_t *state,
                                        size_t client_index,
                                        const char *channel)
{
    chatd_client_t *client = &state->clients[client_index];
    const int channel_index = chatd_find_channel(state, channel);
    if (channel_index < 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "unknown channel",
                               false,
                               404,
                               true);
        return;
    }
    if (strcmp(state->channels[channel_index], CHATD_DEFAULT_CHANNEL) == 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               state->channels[channel_index],
                               NULL,
                               "default channel cannot be deleted",
                               false,
                               403,
                               true);
        return;
    }

    char removed[SOLAR_OS_CHAT_CHANNEL_MAX];
    strlcpy(removed, state->channels[channel_index], sizeof(removed));

    chatd_broadcast_global_event(state,
                                 "deleted",
                                 removed,
                                 client->from,
                                 "deleted",
                                 true,
                                 -1);

    const size_t remove_index = (size_t)channel_index;
    for (size_t i = remove_index; i + 1U < state->channel_count; i++) {
        strlcpy(state->channels[i], state->channels[i + 1U], sizeof(state->channels[i]));
    }
    if (state->channel_count > 0) {
        state->channel_count--;
    }

    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        state->clients[i].joined_mask =
            chatd_mask_without_channel(state->clients[i].joined_mask, remove_index);
    }
    chatd_drop_channel_history(state, removed);
}

static void chatd_client_message(chatd_job_state_t *state,
                                 size_t client_index,
                                 const char *channel,
                                 const char *text)
{
    chatd_client_t *client = &state->clients[client_index];
    const int channel_index = chatd_find_channel(state, channel);
    if (channel_index < 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "unknown channel",
                               false,
                               404,
                               true);
        return;
    }

    const uint32_t mask = 1UL << (uint32_t)channel_index;
    if ((client->joined_mask & mask) == 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               state->channels[channel_index],
                               NULL,
                               "not joined",
                               false,
                               403,
                               true);
        return;
    }

    chatd_broadcast_event(state,
                          (size_t)channel_index,
                          "msg",
                          client->from,
                          text,
                          true,
                          -1);
    state->message_count++;
}

static void chatd_close_client(chatd_job_state_t *state, size_t index, bool notify)
{
    if (state == NULL || index >= CHATD_MAX_CLIENTS) {
        return;
    }
    chatd_client_t *client = &state->clients[index];
    if (!client->active) {
        return;
    }

    const uint32_t joined = client->joined_mask;
    if (notify && client->authed) {
        for (size_t ch = 0; ch < state->channel_count; ch++) {
            if ((joined & (1UL << ch)) != 0) {
                chatd_broadcast_event(state,
                                      ch,
                                      "presence",
                                      client->from,
                                      "left",
                                      true,
                                      (int)index);
            }
        }
    }

    if (client->fd >= 0) {
        shutdown(client->fd, SHUT_RDWR);
        close(client->fd);
    }
    client->active = false;
    client->authed = false;
    client->fd = -1;
    client->joined_mask = 0;
    client->line_len = 0;
    client->user[0] = '\0';
    client->device[0] = '\0';
    client->from[0] = '\0';
    client->peer[0] = '\0';
}

static void chatd_handle_hello(chatd_job_state_t *state, size_t client_index, const char *line)
{
    chatd_client_t *client = &state->clients[client_index];
    char token[SOLAR_OS_CHAT_TOKEN_MAX] = {0};
    char user[SOLAR_OS_CHAT_USER_MAX] = {0};
    char device[SOLAR_OS_CHAT_DEVICE_MAX] = {0};

    (void)chatd_json_get_string(line, "token", token, sizeof(token));
    (void)chatd_json_get_string(line, "user", user, sizeof(user));
    (void)chatd_json_get_string(line, "device", device, sizeof(device));

    if (state->token_set && strcmp(token, state->token) != 0) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "auth failed",
                               false,
                               401,
                               true);
        chatd_close_client(state, client_index, false);
        return;
    }

    if (!chatd_text_arg_valid(user, sizeof(user), true)) {
        strlcpy(user, "user", sizeof(user));
    }
    if (!chatd_text_arg_valid(device, sizeof(device), true)) {
        strlcpy(device, "sol", sizeof(device));
    }
    if (user[0] == '\0') {
        strlcpy(user, "user", sizeof(user));
    }

    strlcpy(client->user, user, sizeof(client->user));
    strlcpy(client->device, device, sizeof(client->device));
    chatd_make_from(client);
    client->authed = true;

    (void)chatd_send_event(state,
                           client,
                           "connected",
                           NULL,
                           "chatd",
                           "connected",
                           true,
                           0,
                           false);
    for (size_t i = 0; i < state->channel_count; i++) {
        (void)chatd_send_channel_event(state, client, state->channels[i]);
    }
    chatd_client_join(state, client_index, CHATD_DEFAULT_CHANNEL);
    SOLAR_OS_LOGI(TAG, "client connected: %s %s", client->peer, client->from);
}

static void chatd_process_line(chatd_job_state_t *state, size_t client_index, const char *line)
{
    chatd_client_t *client = &state->clients[client_index];
    char type[24] = {0};
    char channel[SOLAR_OS_CHAT_CHANNEL_MAX] = {0};

    if (!chatd_json_get_string(line, "type", type, sizeof(type))) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "missing type",
                               false,
                               400,
                               true);
        return;
    }

    if (strcmp(type, "hello") == 0) {
        chatd_handle_hello(state, client_index, line);
        return;
    }

    if (!client->authed) {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "hello required",
                               false,
                               401,
                               true);
        chatd_close_client(state, client_index, false);
        return;
    }

    if (strcmp(type, "join") == 0) {
        if (!chatd_json_get_string(line, "channel", channel, sizeof(channel))) {
            strlcpy(channel, CHATD_DEFAULT_CHANNEL, sizeof(channel));
        }
        chatd_client_join(state, client_index, channel);
    } else if (strcmp(type, "leave") == 0) {
        if (!chatd_json_get_string(line, "channel", channel, sizeof(channel))) {
            strlcpy(channel, CHATD_DEFAULT_CHANNEL, sizeof(channel));
        }
        chatd_client_leave(state, client_index, channel);
    } else if (strcmp(type, "delete") == 0) {
        if (!chatd_json_get_string(line, "channel", channel, sizeof(channel))) {
            (void)chatd_send_event(state,
                                   client,
                                   "error",
                                   NULL,
                                   NULL,
                                   "missing channel",
                                   false,
                                   400,
                                   true);
            return;
        }
        chatd_client_delete_channel(state, client_index, channel);
    } else if (strcmp(type, "msg") == 0) {
        if (!chatd_json_get_string(line, "channel", channel, sizeof(channel))) {
            strlcpy(channel, CHATD_DEFAULT_CHANNEL, sizeof(channel));
        }
        if (!chatd_json_get_string(line, "text", state->text_arg, SOLAR_OS_CHAT_TEXT_MAX)) {
            (void)chatd_send_event(state,
                                   client,
                                   "error",
                                   NULL,
                                   NULL,
                                   "missing text",
                                   false,
                                   400,
                                   true);
            return;
        }
        chatd_client_message(state, client_index, channel, state->text_arg);
    } else {
        (void)chatd_send_event(state,
                               client,
                               "error",
                               NULL,
                               NULL,
                               "unknown type",
                               false,
                               400,
                               true);
    }
}

static void chatd_client_rx(chatd_job_state_t *state, size_t client_index)
{
    chatd_client_t *client = &state->clients[client_index];
    char chunk[CHATD_RX_CHUNK_SIZE];
    const ssize_t got = recv(client->fd, chunk, sizeof(chunk), 0);
    if (got <= 0) {
        chatd_close_client(state, client_index, true);
        return;
    }

    client->rx_count++;
    for (ssize_t i = 0; i < got; i++) {
        const char ch = chunk[i];
        if (ch == '\n') {
            if (client->line_len > 0 && client->line[client->line_len - 1U] == '\r') {
                client->line_len--;
            }
            client->line[client->line_len] = '\0';
            if (client->line_len > 0) {
                chatd_process_line(state, client_index, client->line);
            }
            client->line_len = 0;
            continue;
        }

        if (client->line_len + 1U >= CHATD_LINE_MAX) {
            (void)chatd_send_event(state,
                                   client,
                                   "error",
                                   NULL,
                                   NULL,
                                   "line too long",
                                   false,
                                   413,
                                   true);
            chatd_close_client(state, client_index, false);
            return;
        }
        client->line[client->line_len++] = ch;
    }
}

static int chatd_find_free_client(chatd_job_state_t *state)
{
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        if (!state->clients[i].active) {
            return (int)i;
        }
    }
    return -1;
}

static void chatd_accept_client(chatd_job_state_t *state)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    const int fd = accept(state->listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        return;
    }

    const int slot = chatd_find_free_client(state);
    if (slot < 0) {
        static const char busy[] =
            "{\"type\":\"error\",\"text\":\"server full\",\"code\":503}\n";
        (void)send(fd, busy, sizeof(busy) - 1U, 0);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        state->dropped_count++;
        return;
    }

    chatd_client_t *client = &state->clients[slot];
    client->active = true;
    client->authed = false;
    client->fd = fd;
    client->joined_mask = 0;
    client->line_len = 0;
    client->rx_count = 0;
    client->tx_count = 0;
    client->user[0] = '\0';
    client->device[0] = '\0';
    client->from[0] = '\0';
    snprintf(client->peer,
             sizeof(client->peer),
             "%s:%u",
             inet_ntoa(addr.sin_addr),
             (unsigned)ntohs(addr.sin_port));

    state->connection_count++;
}

static void chatd_close_all_sockets(chatd_job_state_t *state)
{
    if (state->listen_fd >= 0) {
        shutdown(state->listen_fd, SHUT_RDWR);
        close(state->listen_fd);
        state->listen_fd = -1;
    }
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        chatd_close_client(state, i, false);
    }
}

static void chatd_free_buffers(chatd_job_state_t *state)
{
    if (state->history_file != NULL) {
        fclose(state->history_file);
        state->history_file = NULL;
    }
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        if (state->clients[i].line != NULL) {
            heap_caps_free(state->clients[i].line);
            state->clients[i].line = NULL;
        }
    }
    if (state->tx_line != NULL) {
        heap_caps_free(state->tx_line);
        state->tx_line = NULL;
    }
    if (state->text_arg != NULL) {
        heap_caps_free(state->text_arg);
        state->text_arg = NULL;
    }
    if (state->history != NULL) {
        heap_caps_free(state->history);
        state->history = NULL;
    }
}

static void chatd_job_task(void *arg)
{
    chatd_job_state_t *state = (chatd_job_state_t *)arg;

    SOLAR_OS_LOGI(TAG,
                  "started on port %u%s",
                  (unsigned)state->port,
                  state->token_set ? " token=yes" : "");

    while (!state->stop_requested) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = -1;

        if (state->listen_fd >= 0) {
            FD_SET(state->listen_fd, &readfds);
            max_fd = state->listen_fd;
        }
        for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
            const int fd = state->clients[i].fd;
            if (state->clients[i].active && fd >= 0) {
                FD_SET(fd, &readfds);
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }
        }

        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(CHATD_SELECT_TIMEOUT_MS));
            continue;
        }

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = CHATD_SELECT_TIMEOUT_MS * 1000,
        };
        const int ready = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno != EINTR && !state->stop_requested) {
                state->last_error = ESP_FAIL;
                SOLAR_OS_LOGW(TAG, "select failed errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        if (ready == 0) {
            continue;
        }

        if (state->listen_fd >= 0 && FD_ISSET(state->listen_fd, &readfds)) {
            chatd_accept_client(state);
        }
        for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
            if (state->clients[i].active &&
                state->clients[i].fd >= 0 &&
                FD_ISSET(state->clients[i].fd, &readfds)) {
                chatd_client_rx(state, i);
            }
        }
    }

    SOLAR_OS_LOGI(TAG,
                  "stopped: connections=%" PRIu32 " messages=%" PRIu32 " dropped=%" PRIu32,
                  state->connection_count,
                  state->message_count,
                  state->dropped_count);

    chatd_close_all_sockets(state);
    chatd_free_buffers(state);
    state->running = false;
    state->stop_requested = false;
    state->task = NULL;
    vTaskDelete(NULL);
}

static bool chatd_parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }
    uint32_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!isdigit(*p)) {
            return false;
        }
        value = value * 10U + (uint32_t)(*p - '0');
        if (value > UINT16_MAX) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static esp_err_t chatd_parse_args(int argc,
                                  char **argv,
                                  uint16_t *port,
                                  const char **token,
                                  const char **history_path)
{
    if (argc < 1 || argc > 6 || argv == NULL || port == NULL || token == NULL ||
        history_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *port = CHATD_DEFAULT_PORT;
    *token = NULL;
    *history_path = NULL;
    bool port_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--history") == 0 || strcmp(argv[i], "--log") == 0) {
            if (i + 1 >= argc || *history_path != NULL) {
                return ESP_ERR_INVALID_ARG;
            }
            *history_path = argv[++i];
            continue;
        }

        uint16_t parsed_port = 0;
        if (!port_set && chatd_parse_port(argv[i], &parsed_port)) {
            *port = parsed_port;
            port_set = true;
            continue;
        }
        if (*token == NULL) {
            *token = argv[i];
            continue;
        }
        if (*history_path == NULL) {
            *history_path = argv[i];
            continue;
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (*token != NULL &&
        !chatd_text_arg_valid(*token, SOLAR_OS_CHAT_TOKEN_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*history_path != NULL && (*history_path)[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void chatd_mkdir_parent(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }

    char work[SOLAR_OS_STORAGE_PATH_MAX];
    if (strlcpy(work, path, sizeof(work)) >= sizeof(work)) {
        return;
    }

    char *slash = strrchr(work, '/');
    if (slash == NULL || slash == work) {
        return;
    }
    *slash = '\0';

    for (char *p = work + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (work[0] != '\0') {
            (void)mkdir(work, 0777);
        }
        *p = '/';
    }
    (void)mkdir(work, 0777);
}

static void chatd_open_history_dump(chatd_job_state_t *state, const char *path)
{
    if (state == NULL || path == NULL || path[0] == '\0') {
        return;
    }

    char resolved[SOLAR_OS_STORAGE_PATH_MAX];
    const char *open_path = path;
    if (solar_os_storage_resolve_path(path, resolved, sizeof(resolved)) == ESP_OK) {
        open_path = resolved;
    }

    chatd_mkdir_parent(open_path);
    state->history_file = fopen(open_path, "a");
    if (state->history_file == NULL) {
        SOLAR_OS_LOGW(TAG, "history dump unavailable: %s errno=%d", open_path, errno);
        state->history_path[0] = '\0';
        return;
    }

    strlcpy(state->history_path, open_path, sizeof(state->history_path));
    SOLAR_OS_LOGI(TAG, "history dump: %s", state->history_path);
}

static esp_err_t chatd_alloc_buffers(chatd_job_state_t *state)
{
    state->tx_line = solar_os_psram_malloc(CHATD_LINE_MAX);
    state->text_arg = solar_os_psram_malloc(SOLAR_OS_CHAT_TEXT_MAX);
    state->history = solar_os_psram_calloc(CHATD_HISTORY_COUNT, sizeof(chatd_history_entry_t));
    if (state->tx_line == NULL || state->text_arg == NULL || state->history == NULL) {
        chatd_free_buffers(state);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        state->clients[i].line = solar_os_psram_malloc(CHATD_LINE_MAX);
        if (state->clients[i].line == NULL) {
            chatd_free_buffers(state);
            return ESP_ERR_NO_MEM;
        }
        state->clients[i].fd = -1;
    }
    return ESP_OK;
}

static esp_err_t chatd_open_listener(chatd_job_state_t *state)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return ESP_FAIL;
    }

    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(state->port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    if (listen(fd, CHATD_MAX_CLIENTS) != 0) {
        close(fd);
        return ESP_FAIL;
    }

    state->listen_fd = fd;
    return ESP_OK;
}

static esp_err_t chatd_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (chatd_job.running || chatd_job.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t port = CHATD_DEFAULT_PORT;
    const char *token = NULL;
    const char *history_path = NULL;
    esp_err_t err = chatd_parse_args(argc, argv, &port, &token, &history_path);
    if (err != ESP_OK) {
        return err;
    }

    chatd_job.running = false;
    chatd_job.stop_requested = false;
    chatd_job.listen_fd = -1;
    chatd_job.port = port;
    chatd_job.token_set = token != NULL;
    chatd_job.token[0] = '\0';
    chatd_job.history_path[0] = '\0';
    chatd_job.history_head = 0;
    chatd_job.history_count = 0;
    if (token != NULL) {
        strlcpy(chatd_job.token, token, sizeof(chatd_job.token));
    }
    chatd_job.channel_count = 1;
    strlcpy(chatd_job.channels[0], CHATD_DEFAULT_CHANNEL, sizeof(chatd_job.channels[0]));
    chatd_job.connection_count = 0;
    chatd_job.message_count = 0;
    chatd_job.dropped_count = 0;
    chatd_job.last_error = ESP_OK;
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        chatd_job.clients[i].active = false;
        chatd_job.clients[i].authed = false;
        chatd_job.clients[i].fd = -1;
        chatd_job.clients[i].joined_mask = 0;
        chatd_job.clients[i].line_len = 0;
    }

    err = chatd_alloc_buffers(&chatd_job);
    if (err != ESP_OK) {
        chatd_job.last_error = err;
        return err;
    }
    chatd_open_history_dump(&chatd_job, history_path);

    err = chatd_open_listener(&chatd_job);
    if (err != ESP_OK) {
        chatd_free_buffers(&chatd_job);
        chatd_job.last_error = err;
        return err;
    }

    chatd_job.running = true;
    if (xTaskCreate(chatd_job_task,
                    "chatd_job",
                    CHATD_TASK_STACK,
                    &chatd_job,
                    CHATD_TASK_PRIORITY,
                    &chatd_job.task) != pdPASS) {
        chatd_close_all_sockets(&chatd_job);
        chatd_free_buffers(&chatd_job);
        chatd_job.running = false;
        chatd_job.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void chatd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (!chatd_job.running && chatd_job.task == NULL) {
        return;
    }

    chatd_job.stop_requested = true;
    if (chatd_job.listen_fd >= 0) {
        shutdown(chatd_job.listen_fd, SHUT_RDWR);
    }
    for (size_t i = 0; i < CHATD_MAX_CLIENTS; i++) {
        if (chatd_job.clients[i].active && chatd_job.clients[i].fd >= 0) {
            shutdown(chatd_job.clients[i].fd, SHUT_RDWR);
        }
    }

    const uint32_t waits = CHATD_STOP_WAIT_MS / 25U;
    for (uint32_t i = 0; i < waits && chatd_job.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

const solar_os_job_t solar_os_chatd_job = {
    .name = "chatd",
    .summary = "local chat gateway server",
    .start = chatd_job_start,
    .stop = chatd_job_stop,
    .event = NULL,
};
