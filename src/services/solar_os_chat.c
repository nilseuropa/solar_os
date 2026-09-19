#include "solar_os_chat.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_identity.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"

#define CHAT_NVS_NAMESPACE "chat"
#define CHAT_NVS_URL_KEY "url"
#define CHAT_NVS_TOKEN_KEY "token"
#define CHAT_NVS_LEGACY_USER_KEY "user"
#define CHAT_NVS_ENABLED_KEY "enabled"
#define CHAT_DEFAULT_CHANNEL "general"
#define CHAT_EVENT_WAIT_POLL_MS 20U

typedef struct {
    bool enabled;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
} solar_os_chat_saved_config_t;

typedef struct {
    bool initialized;
    bool configured;
    bool enabled;
    bool sync_running;
    bool connected;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char last_error[SOLAR_OS_CHAT_ERROR_MAX];
    esp_err_t last_esp_error;
    uint32_t config_revision;
    uint32_t next_event_id;
    uint32_t next_command_id;
    uint32_t legacy_cursor;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    solar_os_chat_event_t *events;
    size_t event_head;
    size_t event_count;
    solar_os_chat_command_t *commands;
    size_t command_head;
    size_t command_count;
    solar_os_chat_channel_t channels[SOLAR_OS_CHAT_CHANNEL_CAPACITY];
    size_t channel_count;
    SemaphoreHandle_t lock;
} solar_os_chat_store_state_t;

static EXT_RAM_BSS_ATTR solar_os_chat_store_state_t chat;

static void chat_lock(void)
{
    (void)xSemaphoreTake(chat.lock, portMAX_DELAY);
}

static void chat_unlock(void)
{
    xSemaphoreGive(chat.lock);
}

static uint32_t chat_next_id(uint32_t *next)
{
    uint32_t id = (*next)++;
    if (id == 0) {
        id = (*next)++;
    }
    return id;
}

static bool chat_string_is_valid(const char *text,
                                 size_t max_len,
                                 bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text;
         *p != '\0';
         p++) {
        if (*p < 0x20U || *p == 0x7fU) {
            return false;
        }
    }
    return true;
}

static bool chat_url_is_valid(const char *url)
{
    if (!chat_string_is_valid(url, SOLAR_OS_CHAT_URL_MAX, false)) {
        return false;
    }
    return strncmp(url, "chat://", 7) == 0 ||
        strncmp(url, "chats://", 8) == 0 ||
        strncmp(url, "tcp://", 6) == 0 ||
        strncmp(url, "tls://", 6) == 0;
}

static int chat_channel_index_locked(const char *channel)
{
    for (size_t i = 0; i < chat.channel_count; i++) {
        if (strcmp(chat.channels[i].name, channel) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int chat_add_channel_locked(const char *channel,
                                   bool desired,
                                   bool joined)
{
    int index = chat_channel_index_locked(channel);
    if (index >= 0) {
        if (desired) {
            chat.channels[index].desired = true;
        }
        if (joined) {
            chat.channels[index].joined = true;
        }
        return index;
    }
    if (chat.channel_count >= SOLAR_OS_CHAT_CHANNEL_CAPACITY) {
        return -1;
    }
    index = (int)chat.channel_count++;
    strlcpy(chat.channels[index].name,
            channel,
            sizeof(chat.channels[index].name));
    chat.channels[index].desired = desired;
    chat.channels[index].joined = joined;
    return index;
}

static uint32_t chat_security_flags(void)
{
    uint32_t flags = 0;
    chat_lock();
    if (strncmp(chat.url, "chats://", 8) == 0 ||
        strncmp(chat.url, "tls://", 6) == 0) {
        flags = SOLAR_OS_SECURITY_TRANSPORT_SECURED;
    }
    chat_unlock();
    return flags;
}

static void chat_upsert_room(const char *channel)
{
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        .provider_key = channel,
        .kind = SOLAR_OS_CONVERSATION_ROOM,
        .title = channel,
        .security_flags = chat_security_flags(),
    };
    (void)solar_os_messaging_conversation_upsert(&request, NULL);
}

static void chat_publish_event_locked(const solar_os_chat_event_t *event,
                                      uint64_t message_key)
{
    solar_os_chat_event_t *stored = &chat.events[chat.event_head];
    *stored = *event;
    stored->id = chat_next_id(&chat.next_event_id);
    if (message_key != 0) {
        stored->message_key = message_key;
    }
    chat.event_head =
        (chat.event_head + 1U) % SOLAR_OS_CHAT_STORE_CAPACITY;
    if (chat.event_count < SOLAR_OS_CHAT_STORE_CAPACITY) {
        chat.event_count++;
    } else {
        chat.dropped_count++;
    }
    chat.rx_count++;
}

static esp_err_t chat_queue_command_locked(solar_os_chat_command_type_t type,
                                           const char *channel,
                                           uint64_t cursor)
{
    if (chat.command_count >= SOLAR_OS_CHAT_OUTBOX_CAPACITY) {
        chat.dropped_count++;
        return ESP_ERR_NO_MEM;
    }
    solar_os_chat_command_t *command = &chat.commands[chat.command_head];
    memset(command, 0, sizeof(*command));
    command->id = chat_next_id(&chat.next_command_id);
    command->type = type;
    command->cursor = cursor;
    strlcpy(command->channel, channel, sizeof(command->channel));
    chat.command_head =
        (chat.command_head + 1U) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
    chat.command_count++;
    return ESP_OK;
}

static void chat_snapshot_config_locked(solar_os_chat_saved_config_t *config)
{
    config->enabled = chat.enabled;
    strlcpy(config->url, chat.url, sizeof(config->url));
    strlcpy(config->token, chat.token, sizeof(config->token));
}

static esp_err_t chat_save_config(const solar_os_chat_saved_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t error =
        nvs_open(CHAT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(nvs, CHAT_NVS_URL_KEY, config->url);
    if (error == ESP_OK) {
        error = nvs_set_str(nvs, CHAT_NVS_TOKEN_KEY, config->token);
    }
    if (error == ESP_OK) {
        error = nvs_set_u8(nvs,
                           CHAT_NVS_ENABLED_KEY,
                           config->enabled ? 1U : 0U);
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return error;
}

static void chat_remove_legacy_user_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CHAT_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(nvs, CHAT_NVS_LEGACY_USER_KEY) == ESP_OK) {
        (void)nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static void chat_load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    char url[SOLAR_OS_CHAT_URL_MAX] = {0};
    char token[SOLAR_OS_CHAT_TOKEN_MAX] = {0};
    size_t length = sizeof(url);
    esp_err_t error = nvs_get_str(nvs, CHAT_NVS_URL_KEY, url, &length);
    if (error == ESP_OK && chat_url_is_valid(url)) {
        length = sizeof(token);
        if (nvs_get_str(nvs,
                        CHAT_NVS_TOKEN_KEY,
                        token,
                        &length) == ESP_ERR_NVS_NOT_FOUND) {
            token[0] = '\0';
        }
        if (chat_string_is_valid(token, sizeof(token), true)) {
            strlcpy(chat.url, url, sizeof(chat.url));
            strlcpy(chat.token, token, sizeof(chat.token));
            chat.configured = true;
            uint8_t enabled = 1U;
            const esp_err_t enabled_error =
                nvs_get_u8(nvs, CHAT_NVS_ENABLED_KEY, &enabled);
            chat.enabled =
                enabled_error == ESP_ERR_NVS_NOT_FOUND || enabled != 0;
            chat.config_revision = 1U;
        }
    }
    nvs_close(nvs);
    chat_remove_legacy_user_config();
}

esp_err_t solar_os_chat_init(void)
{
    if (chat.initialized) {
        return ESP_OK;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    chat.lock = xSemaphoreCreateMutex();
    if (chat.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    chat.events = solar_os_memory_calloc(
        SOLAR_OS_CHAT_STORE_CAPACITY,
        sizeof(*chat.events),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "chat.events");
    chat.commands = solar_os_memory_calloc(
        SOLAR_OS_CHAT_OUTBOX_CAPACITY,
        sizeof(*chat.commands),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "chat.commands");
    if (chat.events == NULL || chat.commands == NULL) {
        solar_os_memory_free(chat.events);
        solar_os_memory_free(chat.commands);
        vSemaphoreDelete(chat.lock);
        memset(&chat, 0, sizeof(chat));
        return ESP_ERR_NO_MEM;
    }
    chat.next_event_id = 1U;
    chat.next_command_id = 1U;
    chat_load_config();
    (void)chat_add_channel_locked(CHAT_DEFAULT_CHANNEL, true, false);
    chat.initialized = true;
    (void)solar_os_messaging_provider_register(
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        "gateway");
    chat_upsert_room(CHAT_DEFAULT_CHANNEL);
    return ESP_OK;
}

esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token)
{
    if ((url != NULL && url[0] != '\0' && !chat_url_is_valid(url)) ||
        (token != NULL &&
         !chat_string_is_valid(token, SOLAR_OS_CHAT_TOKEN_MAX, true))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    if (url != NULL && url[0] != '\0') {
        strlcpy(chat.url, url, sizeof(chat.url));
        chat.configured = true;
    }
    if (token != NULL) {
        strlcpy(chat.token, token, sizeof(chat.token));
    }
    if (!chat.configured || !chat_url_is_valid(chat.url)) {
        chat_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    chat.config_revision++;
    if (chat.config_revision == 0) {
        chat.config_revision = 1U;
    }
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    error = chat_save_config(&config);
    if (error != ESP_OK) {
        chat_lock();
        chat.last_esp_error = error;
        strlcpy(chat.last_error,
                esp_err_to_name(error),
                sizeof(chat.last_error));
        chat_unlock();
    }
    return error;
}

esp_err_t solar_os_chat_connect(const char *url,
                                const char *token)
{
    esp_err_t error = solar_os_chat_configure(url, token);
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    chat.enabled = true;
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    return chat_save_config(&config);
}

esp_err_t solar_os_chat_disconnect(void)
{
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    chat.enabled = false;
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    return chat_save_config(&config);
}

esp_err_t solar_os_chat_join(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    const uint64_t cursor = solar_os_chat_channel_cursor(channel);
    chat_lock();
    const int existing_index = chat_channel_index_locked(channel);
    if (existing_index >= 0 && chat.channels[existing_index].desired) {
        error = ESP_OK;
    } else if (existing_index < 0 &&
        chat.channel_count >= SOLAR_OS_CHAT_CHANNEL_CAPACITY) {
        error = ESP_ERR_NO_MEM;
    } else {
        error = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_JOIN,
                                          channel,
                                          cursor);
        if (error == ESP_OK) {
            const int index = chat_add_channel_locked(channel, true, false);
            if (index >= 0) {
                chat.channels[index].desired = true;
            }
        }
    }
    chat_unlock();
    if (error == ESP_OK) {
        chat_upsert_room(channel);
    }
    return error;
}

esp_err_t solar_os_chat_leave(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    error = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_LEAVE, channel, 0);
    if (error == ESP_OK && index >= 0) {
        chat.channels[index].desired = false;
    }
    chat_unlock();
    return error;
}

esp_err_t solar_os_chat_delete_channel(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    error =
        chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL,
                                  channel,
                                  0);
    if (error == ESP_OK && index >= 0) {
        chat.channels[index].desired = false;
    }
    chat_unlock();
    if (error == ESP_OK) {
        (void)solar_os_messaging_conversation_remove(
            SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
            channel);
    }
    return error;
}

esp_err_t solar_os_chat_send(const char *channel, const char *text)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false) ||
        text == NULL || text[0] == '\0' ||
        strlen(text) >= SOLAR_OS_CHAT_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        .provider_key = channel,
        .kind = SOLAR_OS_CONVERSATION_ROOM,
        .title = channel,
        .security_flags = chat_security_flags(),
    };
    solar_os_conversation_id_t conversation_id = 0;
    error = solar_os_messaging_conversation_upsert(&request,
                                                   &conversation_id);
    if (error != ESP_OK) {
        return error;
    }
    return solar_os_messaging_send(conversation_id, text, true, NULL);
}

esp_err_t solar_os_chat_read_event_after(uint32_t *cursor,
                                         solar_os_chat_event_t *event)
{
    if (cursor == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    const size_t oldest =
        (chat.event_head + SOLAR_OS_CHAT_STORE_CAPACITY - chat.event_count) %
        SOLAR_OS_CHAT_STORE_CAPACITY;
    bool found = false;
    for (size_t i = 0; i < chat.event_count; i++) {
        const solar_os_chat_event_t *candidate =
            &chat.events[(oldest + i) % SOLAR_OS_CHAT_STORE_CAPACITY];
        if ((int32_t)(candidate->id - *cursor) > 0) {
            *event = *candidate;
            *cursor = candidate->id;
            found = true;
            break;
        }
    }
    chat_unlock();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_chat_read_event(solar_os_chat_event_t *event,
                                   uint32_t timeout_ms)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    const TickType_t started = xTaskGetTickCount();
    do {
        error = solar_os_chat_read_event_after(&chat.legacy_cursor, event);
        if (error == ESP_OK || timeout_ms == 0) {
            return error;
        }
        vTaskDelay(pdMS_TO_TICKS(CHAT_EVENT_WAIT_POLL_MS));
    } while (pdTICKS_TO_MS(xTaskGetTickCount() - started) < timeout_ms);
    return ESP_ERR_TIMEOUT;
}

typedef struct {
    solar_os_chat_message_visitor_t visitor;
    void *user;
} chat_visit_context_t;

static bool chat_visit_message(
    const solar_os_messaging_message_t *message,
    void *user)
{
    chat_visit_context_t *context = user;
    solar_os_messaging_conversation_t conversation;
    if (solar_os_messaging_conversation_get(message->conversation_id,
                                            &conversation) != ESP_OK) {
        return true;
    }
    solar_os_chat_message_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    legacy.id = (uint32_t)message->key;
    legacy.inbox_id = message->inbox_id;
    legacy.message_key = message->key;
    legacy.timestamp = message->timestamp_ms;
    legacy.unread = message->unread;
    legacy.truncated = message->truncated;
    strlcpy(legacy.channel,
            conversation.provider_key,
            sizeof(legacy.channel));
    strlcpy(legacy.from, message->sender, sizeof(legacy.from));
    strlcpy(legacy.text, message->body, sizeof(legacy.text));
    return context->visitor(&legacy, context->user);
}

size_t solar_os_chat_message_visit(solar_os_chat_message_visitor_t visitor,
                                   void *user,
                                   uint32_t *event_cursor)
{
    if (visitor == NULL) {
        return 0;
    }
    chat_visit_context_t context = {
        .visitor = visitor,
        .user = user,
    };
    return solar_os_messaging_message_visit(
        0,
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        chat_visit_message,
        &context,
        event_cursor);
}

esp_err_t solar_os_chat_mark_message_read(uint64_t message_key, bool read)
{
    if (!read) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_messaging_message_t message;
    esp_err_t error =
        solar_os_messaging_message_get(message_key, &message);
    if (error != ESP_OK) {
        return error;
    }
    return solar_os_messaging_mark_read(message.conversation_id);
}

esp_err_t solar_os_chat_mark_channel_read(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_messaging_conversation_t *conversations =
        solar_os_memory_calloc(SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
                               sizeof(*conversations),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "chat.read.snapshot");
    if (conversations == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const size_t count = solar_os_messaging_conversation_snapshot(
        conversations,
        SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY);
    for (size_t i = 0; i < count; i++) {
        if (conversations[i].provider ==
                SOLAR_OS_MESSAGING_PROVIDER_GATEWAY &&
            strcmp(conversations[i].provider_key, channel) == 0) {
            const esp_err_t error =
                solar_os_messaging_mark_read(conversations[i].id);
            solar_os_memory_free(conversations);
            return error;
        }
    }
    solar_os_memory_free(conversations);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    solar_os_messaging_status_t messaging_status;
    error = solar_os_messaging_get_status(&messaging_status);
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = chat.initialized;
    status->configured = chat.configured;
    status->enabled = chat.enabled;
    status->running = chat.sync_running;
    status->connected = chat.connected;
    status->token_set = chat.token[0] != '\0';
    status->state = chat.connected ?
        SOLAR_OS_CHAT_STATE_CONNECTED :
        (chat.sync_running && chat.enabled ?
             SOLAR_OS_CHAT_STATE_CONNECTING :
             SOLAR_OS_CHAT_STATE_DISCONNECTED);
    strlcpy(status->url, chat.url, sizeof(status->url));
    solar_os_identity_get_user(status->user,
                               sizeof(status->user));
    solar_os_identity_get_hostname(status->device,
                                   sizeof(status->device));
    strlcpy(status->last_error,
            chat.last_error,
            sizeof(status->last_error));
    status->last_esp_error = chat.last_esp_error;
    status->config_revision = chat.config_revision;
    status->rx_count = chat.rx_count;
    status->tx_count = chat.tx_count;
    status->dropped_count =
        chat.dropped_count + messaging_status.dropped_messages +
        messaging_status.dropped_outbox;
    status->queued_events = chat.event_count;
    status->queued_outbox =
        chat.command_count + messaging_status.queued_outbox;
    status->stored_messages = messaging_status.messages;
    status->unread_messages = messaging_status.unread;
    status->persistent_capacity = messaging_status.persistent_capacity;
    status->persistent_limit_bytes = messaging_status.persistent_limit_bytes;
    status->persistent = messaging_status.persistent;
    status->persistent_inbox_backed = messaging_status.inbox_backed;
    status->storage_error = messaging_status.storage_error;
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_get_config(solar_os_chat_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    memset(config, 0, sizeof(*config));
    config->configured = chat.configured;
    config->enabled = chat.enabled;
    config->revision = chat.config_revision;
    strlcpy(config->url, chat.url, sizeof(config->url));
    strlcpy(config->token, chat.token, sizeof(config->token));
    solar_os_identity_get_user(config->user,
                               sizeof(config->user));
    solar_os_identity_get_hostname(config->device,
                                   sizeof(config->device));
    chat_unlock();
    return ESP_OK;
}

size_t solar_os_chat_channel_snapshot(solar_os_chat_channel_t *channels,
                                      size_t max_channels)
{
    if (channels == NULL || max_channels == 0 ||
        solar_os_chat_init() != ESP_OK) {
        return 0;
    }
    chat_lock();
    const size_t count =
        chat.channel_count < max_channels ? chat.channel_count : max_channels;
    memcpy(channels, chat.channels, count * sizeof(*channels));
    chat_unlock();
    return count;
}

uint64_t solar_os_chat_channel_cursor(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return 0;
    }
    return solar_os_messaging_provider_cursor(
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        channel);
}

esp_err_t solar_os_gateway_sync_set_status(bool running,
                                        bool connected,
                                        esp_err_t error,
                                        const char *message)
{
    esp_err_t init_error = solar_os_chat_init();
    if (init_error != ESP_OK) {
        return init_error;
    }
    chat_lock();
    chat.sync_running = running;
    chat.connected = connected;
    if (!connected) {
        for (size_t i = 0; i < chat.channel_count; i++) {
            chat.channels[i].joined = false;
        }
    }
    chat.last_esp_error = error;
    strlcpy(chat.last_error,
            message != NULL ? message : "",
            sizeof(chat.last_error));
    chat_unlock();
    return solar_os_messaging_provider_set_status(
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
        running,
        connected,
        error,
        message);
}

esp_err_t solar_os_gateway_sync_publish(const solar_os_chat_event_t *event,
                                     bool *inserted,
                                     uint64_t *message_key)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (inserted != NULL) {
        *inserted = false;
    }
    if (message_key != NULL) {
        *message_key = 0;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    uint64_t key = event->message_key;
    if (event->type == SOLAR_OS_CHAT_EVENT_MESSAGE) {
        const uint32_t security_flags = chat_security_flags();
        const solar_os_messaging_inbound_t request = {
            .provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
            .conversation_key =
                event->channel[0] != '\0' ?
                    event->channel : CHAT_DEFAULT_CHANNEL,
            .conversation_kind = SOLAR_OS_CONVERSATION_ROOM,
            .conversation_title =
                event->channel[0] != '\0' ?
                    event->channel : CHAT_DEFAULT_CHANNEL,
            .provider_message_key = event->message_key,
            .timestamp_ms = event->timestamp,
            .security_flags = security_flags,
            .sender = event->from,
            .body = event->text,
            .truncated = event->truncated,
        };
        error =
            solar_os_messaging_publish_inbound(&request, inserted, &key);
        if (error != ESP_OK) {
            return error;
        }
    }
    chat_lock();
    if (event->type == SOLAR_OS_CHAT_EVENT_CONNECTED ||
        event->type == SOLAR_OS_CHAT_EVENT_DISCONNECTED) {
        for (size_t i = 0; i < chat.channel_count; i++) {
            chat.channels[i].joined = false;
        }
    }
    if (event->type == SOLAR_OS_CHAT_EVENT_CHANNEL &&
        event->channel[0] != '\0') {
        (void)chat_add_channel_locked(event->channel, false, false);
    } else if (event->type == SOLAR_OS_CHAT_EVENT_JOINED &&
               event->channel[0] != '\0') {
        (void)chat_add_channel_locked(event->channel, false, true);
    } else if (event->type == SOLAR_OS_CHAT_EVENT_LEFT &&
               event->channel[0] != '\0') {
        const int index = chat_channel_index_locked(event->channel);
        if (index >= 0) {
            chat.channels[index].desired = false;
            chat.channels[index].joined = false;
        }
    } else if (event->type == SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED &&
               event->channel[0] != '\0') {
        const int index = chat_channel_index_locked(event->channel);
        if (index >= 0) {
            memmove(&chat.channels[index],
                    &chat.channels[index + 1],
                    (chat.channel_count - (size_t)index - 1U) *
                        sizeof(*chat.channels));
            chat.channel_count--;
        }
    }
    chat_publish_event_locked(event, key);
    chat_unlock();
    if (event->channel[0] != '\0' &&
        (event->type == SOLAR_OS_CHAT_EVENT_CHANNEL ||
         event->type == SOLAR_OS_CHAT_EVENT_JOINED)) {
        chat_upsert_room(event->channel);
    } else if (event->type == SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED) {
        (void)solar_os_messaging_conversation_remove(
            SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
            event->channel);
    }
    if (message_key != NULL) {
        *message_key = key;
    }
    return ESP_OK;
}

esp_err_t solar_os_gateway_sync_set_inbox_id(uint64_t message_key,
                                          uint32_t inbox_id)
{
    (void)message_key;
    (void)inbox_id;
    return ESP_ERR_NOT_SUPPORTED;
}

uint64_t solar_os_chat_context_id(void)
{
    return solar_os_messaging_provider_context(
        SOLAR_OS_MESSAGING_PROVIDER_GATEWAY);
}

esp_err_t solar_os_chat_outbox_peek(solar_os_chat_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    if (chat.command_count == 0) {
        chat_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const size_t oldest =
        (chat.command_head + SOLAR_OS_CHAT_OUTBOX_CAPACITY -
         chat.command_count) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
    *command = chat.commands[oldest];
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_outbox_ack(uint32_t id)
{
    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_chat_init();
    if (error != ESP_OK) {
        return error;
    }
    chat_lock();
    if (chat.command_count == 0) {
        chat_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const size_t oldest =
        (chat.command_head + SOLAR_OS_CHAT_OUTBOX_CAPACITY -
         chat.command_count) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
    if (chat.commands[oldest].id != id) {
        chat_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&chat.commands[oldest], 0, sizeof(chat.commands[oldest]));
    chat.command_count--;
    chat.tx_count++;
    chat_unlock();
    return ESP_OK;
}

const char *solar_os_chat_state_name(solar_os_chat_state_t state)
{
    switch (state) {
    case SOLAR_OS_CHAT_STATE_DISCONNECTED:
        return "disconnected";
    case SOLAR_OS_CHAT_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_CHAT_STATE_CONNECTED:
        return "connected";
    default:
        return "unknown";
    }
}

const char *solar_os_chat_event_type_name(solar_os_chat_event_type_t type)
{
    switch (type) {
    case SOLAR_OS_CHAT_EVENT_CONNECTED:
        return "connected";
    case SOLAR_OS_CHAT_EVENT_DISCONNECTED:
        return "disconnected";
    case SOLAR_OS_CHAT_EVENT_ERROR:
        return "error";
    case SOLAR_OS_CHAT_EVENT_CHANNEL:
        return "channel";
    case SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED:
        return "channel-deleted";
    case SOLAR_OS_CHAT_EVENT_JOINED:
        return "joined";
    case SOLAR_OS_CHAT_EVENT_LEFT:
        return "left";
    case SOLAR_OS_CHAT_EVENT_MESSAGE:
        return "message";
    case SOLAR_OS_CHAT_EVENT_PRESENCE:
        return "presence";
    case SOLAR_OS_CHAT_EVENT_COMMAND_SENT:
        return "command-sent";
    case SOLAR_OS_CHAT_EVENT_RAW:
        return "raw";
    default:
        return "unknown";
    }
}
