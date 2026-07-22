#include "solar_os_chat.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_memory.h"

#define CHAT_NVS_NAMESPACE "chat"
#define CHAT_NVS_URL_KEY "url"
#define CHAT_NVS_TOKEN_KEY "token"
#define CHAT_NVS_USER_KEY "user"
#define CHAT_NVS_DEVICE_KEY "device"
#define CHAT_NVS_ENABLED_KEY "enabled"
#define CHAT_DEFAULT_USER "user"
#define CHAT_DEFAULT_DEVICE "sol"
#define CHAT_DEFAULT_CHANNEL "general"
#define CHAT_EVENT_WAIT_POLL_MS 20U

typedef struct {
    bool initialized;
    bool configured;
    bool enabled;
    bool sync_running;
    bool connected;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
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
    solar_os_chat_command_t *outbox;
    size_t outbox_head;
    size_t outbox_count;
    solar_os_chat_channel_t channels[SOLAR_OS_CHAT_CHANNEL_CAPACITY];
    size_t channel_count;
    SemaphoreHandle_t lock;
} solar_os_chat_store_t;

typedef struct {
    bool enabled;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
} solar_os_chat_saved_config_t;

static solar_os_chat_store_t chat_store;

static void chat_lock(void)
{
    if (chat_store.lock != NULL) {
        (void)xSemaphoreTake(chat_store.lock, portMAX_DELAY);
    }
}

static void chat_unlock(void)
{
    if (chat_store.lock != NULL) {
        xSemaphoreGive(chat_store.lock);
    }
}

static bool chat_string_is_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
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

static uint32_t chat_next_id(uint32_t *next)
{
    uint32_t id = (*next)++;
    if (id == 0) {
        id = (*next)++;
    }
    return id;
}

static void chat_clear_outbox_locked(void)
{
    chat_store.outbox_head = 0;
    chat_store.outbox_count = 0;
    if (chat_store.outbox != NULL) {
        memset(chat_store.outbox,
               0,
               SOLAR_OS_CHAT_OUTBOX_CAPACITY * sizeof(*chat_store.outbox));
    }
}

static int chat_channel_index_locked(const char *channel)
{
    for (size_t i = 0; i < chat_store.channel_count; i++) {
        if (strcmp(chat_store.channels[i].name, channel) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int chat_add_channel_locked(const char *channel, bool joined)
{
    int index = chat_channel_index_locked(channel);
    if (index >= 0) {
        if (joined) {
            chat_store.channels[index].joined = true;
        }
        return index;
    }
    if (chat_store.channel_count >= SOLAR_OS_CHAT_CHANNEL_CAPACITY) {
        return -1;
    }
    index = (int)chat_store.channel_count++;
    strlcpy(chat_store.channels[index].name,
            channel,
            sizeof(chat_store.channels[index].name));
    chat_store.channels[index].joined = joined;
    return index;
}

static void chat_snapshot_config_locked(solar_os_chat_saved_config_t *config)
{
    config->enabled = chat_store.enabled;
    strlcpy(config->url, chat_store.url, sizeof(config->url));
    strlcpy(config->token, chat_store.token, sizeof(config->token));
    strlcpy(config->user, chat_store.user, sizeof(config->user));
    strlcpy(config->device, chat_store.device, sizeof(config->device));
}

static esp_err_t chat_save_config(const solar_os_chat_saved_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CHAT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(nvs, CHAT_NVS_URL_KEY, config->url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_TOKEN_KEY, config->token);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_USER_KEY, config->user);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_DEVICE_KEY, config->device);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, CHAT_NVS_ENABLED_KEY, config->enabled ? 1U : 0U);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void chat_load_config(void)
{
    strlcpy(chat_store.user, CHAT_DEFAULT_USER, sizeof(chat_store.user));
    strlcpy(chat_store.device, CHAT_DEFAULT_DEVICE, sizeof(chat_store.device));

    nvs_handle_t nvs;
    if (nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    char url[SOLAR_OS_CHAT_URL_MAX] = {0};
    char token[SOLAR_OS_CHAT_TOKEN_MAX] = {0};
    char user[SOLAR_OS_CHAT_USER_MAX] = {0};
    char device[SOLAR_OS_CHAT_DEVICE_MAX] = {0};
    size_t len = sizeof(url);
    esp_err_t ret = nvs_get_str(nvs, CHAT_NVS_URL_KEY, url, &len);
    if (ret == ESP_OK && chat_url_is_valid(url)) {
        len = sizeof(token);
        if (nvs_get_str(nvs, CHAT_NVS_TOKEN_KEY, token, &len) == ESP_ERR_NVS_NOT_FOUND) {
            token[0] = '\0';
        }
        len = sizeof(user);
        if (nvs_get_str(nvs, CHAT_NVS_USER_KEY, user, &len) != ESP_OK) {
            strlcpy(user, CHAT_DEFAULT_USER, sizeof(user));
        }
        len = sizeof(device);
        if (nvs_get_str(nvs, CHAT_NVS_DEVICE_KEY, device, &len) != ESP_OK) {
            strlcpy(device, CHAT_DEFAULT_DEVICE, sizeof(device));
        }
        if (chat_string_is_valid(token, sizeof(token), true) &&
            chat_string_is_valid(user, sizeof(user), false) &&
            chat_string_is_valid(device, sizeof(device), false)) {
            strlcpy(chat_store.url, url, sizeof(chat_store.url));
            strlcpy(chat_store.token, token, sizeof(chat_store.token));
            strlcpy(chat_store.user, user, sizeof(chat_store.user));
            strlcpy(chat_store.device, device, sizeof(chat_store.device));
            chat_store.configured = true;
            uint8_t enabled = 1U;
            const esp_err_t enabled_ret =
                nvs_get_u8(nvs, CHAT_NVS_ENABLED_KEY, &enabled);
            chat_store.enabled = enabled_ret == ESP_ERR_NVS_NOT_FOUND || enabled != 0;
            chat_store.config_revision = 1U;
        }
    }
    nvs_close(nvs);
}

static esp_err_t chat_queue_command_locked(solar_os_chat_command_type_t type,
                                           const char *channel,
                                           const char *text)
{
    if (chat_store.outbox_count >= SOLAR_OS_CHAT_OUTBOX_CAPACITY) {
        chat_store.dropped_count++;
        return ESP_ERR_NO_MEM;
    }
    solar_os_chat_command_t *command = &chat_store.outbox[chat_store.outbox_head];
    memset(command, 0, sizeof(*command));
    command->id = chat_next_id(&chat_store.next_command_id);
    command->type = type;
    if (channel != NULL) {
        strlcpy(command->channel, channel, sizeof(command->channel));
    }
    if (text != NULL) {
        strlcpy(command->text, text, sizeof(command->text));
    }
    chat_store.outbox_head =
        (chat_store.outbox_head + 1U) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
    chat_store.outbox_count++;
    return ESP_OK;
}

esp_err_t solar_os_chat_init(void)
{
    if (chat_store.initialized) {
        return ESP_OK;
    }
    chat_store.lock = xSemaphoreCreateMutex();
    if (chat_store.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    chat_store.events = solar_os_memory_calloc(SOLAR_OS_CHAT_STORE_CAPACITY,
                                               sizeof(*chat_store.events),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "chat.store");
    chat_store.outbox = solar_os_memory_calloc(SOLAR_OS_CHAT_OUTBOX_CAPACITY,
                                               sizeof(*chat_store.outbox),
                                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                               "chat.outbox");
    if (chat_store.events == NULL || chat_store.outbox == NULL) {
        solar_os_memory_free(chat_store.events);
        solar_os_memory_free(chat_store.outbox);
        chat_store.events = NULL;
        chat_store.outbox = NULL;
        vSemaphoreDelete(chat_store.lock);
        chat_store.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    chat_store.next_event_id = 1U;
    chat_store.next_command_id = 1U;
    chat_load_config();
    (void)chat_add_channel_locked(CHAT_DEFAULT_CHANNEL, true);
    chat_store.initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token,
                                  const char *user,
                                  const char *device)
{
    if ((url != NULL && url[0] != '\0' && !chat_url_is_valid(url)) ||
        (token != NULL &&
         !chat_string_is_valid(token, SOLAR_OS_CHAT_TOKEN_MAX, true)) ||
        (user != NULL &&
         !chat_string_is_valid(user, SOLAR_OS_CHAT_USER_MAX, false)) ||
        (device != NULL &&
         !chat_string_is_valid(device, SOLAR_OS_CHAT_DEVICE_MAX, false))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    bool endpoint_changed = false;
    if (url != NULL && url[0] != '\0') {
        endpoint_changed = strcmp(chat_store.url, url) != 0;
        strlcpy(chat_store.url, url, sizeof(chat_store.url));
        chat_store.configured = true;
    }
    if (token != NULL) {
        strlcpy(chat_store.token, token, sizeof(chat_store.token));
    }
    if (user != NULL) {
        strlcpy(chat_store.user, user, sizeof(chat_store.user));
    }
    if (device != NULL) {
        strlcpy(chat_store.device, device, sizeof(chat_store.device));
    }
    if (!chat_store.configured || !chat_url_is_valid(chat_store.url)) {
        chat_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (endpoint_changed) {
        chat_clear_outbox_locked();
    }
    chat_store.config_revision++;
    if (chat_store.config_revision == 0) {
        chat_store.config_revision = 1U;
    }
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    ret = chat_save_config(&config);
    if (ret != ESP_OK) {
        chat_lock();
        chat_store.last_esp_error = ret;
        strlcpy(chat_store.last_error,
                esp_err_to_name(ret),
                sizeof(chat_store.last_error));
        chat_unlock();
    }
    return ret;
}

esp_err_t solar_os_chat_connect(const char *url,
                                const char *token,
                                const char *user,
                                const char *device)
{
    esp_err_t ret = solar_os_chat_configure(url, token, user, device);
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    chat_store.enabled = true;
    solar_os_chat_saved_config_t config;
    chat_snapshot_config_locked(&config);
    chat_unlock();
    return chat_save_config(&config);
}

esp_err_t solar_os_chat_disconnect(void)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    chat_store.enabled = false;
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
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    if (index < 0 && chat_store.channel_count >= SOLAR_OS_CHAT_CHANNEL_CAPACITY) {
        ret = ESP_ERR_NO_MEM;
    } else {
        ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_JOIN, channel, NULL);
        if (ret == ESP_OK) {
            (void)chat_add_channel_locked(channel, true);
        }
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_leave(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_LEAVE, channel, NULL);
    if (ret == ESP_OK && index >= 0) {
        chat_store.channels[index].joined = false;
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_delete_channel(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    const int index = chat_channel_index_locked(channel);
    ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL, channel, NULL);
    if (ret == ESP_OK && index >= 0) {
        chat_store.channels[index].joined = false;
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_send(const char *channel, const char *text)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false) ||
        text == NULL || text[0] == '\0' || strlen(text) >= SOLAR_OS_CHAT_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    ret = chat_queue_command_locked(SOLAR_OS_CHAT_COMMAND_MESSAGE, channel, text);
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_read_event_after(uint32_t *cursor,
                                         solar_os_chat_event_t *event)
{
    if (cursor == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ESP_ERR_NOT_FOUND;
    chat_lock();
    const size_t oldest = chat_store.event_count == SOLAR_OS_CHAT_STORE_CAPACITY ?
        chat_store.event_head : 0U;
    for (size_t logical = 0; logical < chat_store.event_count; logical++) {
        const size_t index = (oldest + logical) % SOLAR_OS_CHAT_STORE_CAPACITY;
        if (*cursor == 0 || chat_store.events[index].id > *cursor) {
            *event = chat_store.events[index];
            *cursor = event->id;
            ret = ESP_OK;
            break;
        }
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_read_event(solar_os_chat_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        esp_err_t ret = solar_os_chat_read_event_after(&chat_store.legacy_cursor, event);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (timeout_ms == 0 || xTaskGetTickCount() - start >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(CHAT_EVENT_WAIT_POLL_MS));
    }
}

esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = chat_store.initialized;
    status->configured = chat_store.configured;
    status->enabled = chat_store.enabled;
    status->running = chat_store.sync_running;
    status->connected = chat_store.connected;
    status->token_set = chat_store.token[0] != '\0';
    status->state = chat_store.connected ? SOLAR_OS_CHAT_STATE_CONNECTED :
        (chat_store.sync_running && chat_store.enabled && chat_store.configured ?
             SOLAR_OS_CHAT_STATE_CONNECTING : SOLAR_OS_CHAT_STATE_DISCONNECTED);
    strlcpy(status->url, chat_store.url, sizeof(status->url));
    strlcpy(status->user, chat_store.user, sizeof(status->user));
    strlcpy(status->device, chat_store.device, sizeof(status->device));
    strlcpy(status->last_error, chat_store.last_error, sizeof(status->last_error));
    status->last_esp_error = chat_store.last_esp_error;
    status->config_revision = chat_store.config_revision;
    status->rx_count = chat_store.rx_count;
    status->tx_count = chat_store.tx_count;
    status->dropped_count = chat_store.dropped_count;
    status->queued_events = chat_store.event_count;
    status->queued_outbox = chat_store.outbox_count;
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_get_config(solar_os_chat_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    memset(config, 0, sizeof(*config));
    config->configured = chat_store.configured;
    config->enabled = chat_store.enabled;
    config->revision = chat_store.config_revision;
    strlcpy(config->url, chat_store.url, sizeof(config->url));
    strlcpy(config->token, chat_store.token, sizeof(config->token));
    strlcpy(config->user, chat_store.user, sizeof(config->user));
    strlcpy(config->device, chat_store.device, sizeof(config->device));
    chat_unlock();
    return ESP_OK;
}

size_t solar_os_chat_channel_snapshot(solar_os_chat_channel_t *channels,
                                      size_t max_channels)
{
    if (solar_os_chat_init() != ESP_OK || channels == NULL || max_channels == 0) {
        return 0;
    }
    chat_lock();
    const size_t count = chat_store.channel_count < max_channels ?
        chat_store.channel_count : max_channels;
    memcpy(channels, chat_store.channels, count * sizeof(*channels));
    chat_unlock();
    return count;
}

esp_err_t solar_os_chat_sync_set_status(bool running,
                                        bool connected,
                                        esp_err_t error,
                                        const char *message)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    chat_store.sync_running = running;
    chat_store.connected = connected;
    chat_store.last_esp_error = error;
    if (error == ESP_OK) {
        chat_store.last_error[0] = '\0';
    } else {
        strlcpy(chat_store.last_error,
                message != NULL ? message : esp_err_to_name(error),
                sizeof(chat_store.last_error));
    }
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_sync_publish(const solar_os_chat_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    solar_os_chat_event_t *stored = &chat_store.events[chat_store.event_head];
    *stored = *event;
    stored->id = chat_next_id(&chat_store.next_event_id);
    chat_store.event_head =
        (chat_store.event_head + 1U) % SOLAR_OS_CHAT_STORE_CAPACITY;
    if (chat_store.event_count < SOLAR_OS_CHAT_STORE_CAPACITY) {
        chat_store.event_count++;
    } else {
        chat_store.dropped_count++;
    }
    chat_store.rx_count++;
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_outbox_peek(solar_os_chat_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    if (chat_store.outbox_count == 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        const size_t oldest =
            (chat_store.outbox_head + SOLAR_OS_CHAT_OUTBOX_CAPACITY -
             chat_store.outbox_count) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
        *command = chat_store.outbox[oldest];
        ret = ESP_OK;
    }
    chat_unlock();
    return ret;
}

esp_err_t solar_os_chat_outbox_ack(uint32_t id)
{
    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    chat_lock();
    if (chat_store.outbox_count == 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        const size_t oldest =
            (chat_store.outbox_head + SOLAR_OS_CHAT_OUTBOX_CAPACITY -
             chat_store.outbox_count) % SOLAR_OS_CHAT_OUTBOX_CAPACITY;
        if (chat_store.outbox[oldest].id != id) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            memset(&chat_store.outbox[oldest], 0, sizeof(chat_store.outbox[oldest]));
            chat_store.outbox_count--;
            chat_store.tx_count++;
            ret = ESP_OK;
        }
    }
    chat_unlock();
    return ret;
}

const char *solar_os_chat_state_name(solar_os_chat_state_t state)
{
    switch (state) {
    case SOLAR_OS_CHAT_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_CHAT_STATE_CONNECTED:
        return "connected";
    case SOLAR_OS_CHAT_STATE_DISCONNECTED:
    default:
        return "disconnected";
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
        return "deleted";
    case SOLAR_OS_CHAT_EVENT_JOINED:
        return "joined";
    case SOLAR_OS_CHAT_EVENT_LEFT:
        return "left";
    case SOLAR_OS_CHAT_EVENT_MESSAGE:
        return "message";
    case SOLAR_OS_CHAT_EVENT_PRESENCE:
        return "presence";
    case SOLAR_OS_CHAT_EVENT_COMMAND_SENT:
        return "sent";
    case SOLAR_OS_CHAT_EVENT_RAW:
    default:
        return "raw";
    }
}
