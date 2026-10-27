#include "solar_os_chat.h"
#include "solar_os_gateway_instances.h"

#include <string.h>
#include <stdatomic.h>

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
    uint32_t version;
    bool enabled;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char channels[SOLAR_OS_CHAT_CHANNEL_CAPACITY][SOLAR_OS_CHAT_CHANNEL_MAX];
} solar_os_chat_saved_config_t;

typedef struct gateway_store {
    struct gateway_store *next;
    solar_os_messaging_provider_id_t provider;
    char instance_name[SOLAR_OS_MESSAGING_LABEL_MAX];
    void *transport_state;
    void *worker_state;
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
    SemaphoreHandle_t edit_lock;
} solar_os_chat_store_state_t;

static EXT_RAM_BSS_ATTR solar_os_chat_store_state_t default_chat;
static _Thread_local solar_os_chat_store_state_t *selected_chat;
#define chat (*(selected_chat ? selected_chat : &default_chat))
static struct {
    bool initialized;
    SemaphoreHandle_t edit_lock, worker_lock;
    solar_os_chat_store_state_t *instances;
    const solar_os_gateway_runtime_hooks_t *runtime;
} gateways;
static atomic_flag gateway_init_lock = ATOMIC_FLAG_INIT;
static atomic_bool gateway_ready;

void *solar_os_gateway_scope_enter(void *instance)
{
    void *previous = selected_chat; selected_chat = instance; return previous;
}
void solar_os_gateway_scope_leave(void *previous) { selected_chat = previous; }
void *solar_os_gateway_current_instance(void) { return &chat; }
solar_os_messaging_provider_id_t solar_os_gateway_current_id(void)
{ return chat.provider ? chat.provider : SOLAR_OS_MESSAGING_PROVIDER_GATEWAY; }
const char *solar_os_gateway_current_name(void)
{ return chat.instance_name[0] ? chat.instance_name : "gateway"; }
void **solar_os_gateway_transport_slot(void) { return &chat.transport_state; }
void **solar_os_gateway_worker_slot(void) { return &chat.worker_state; }
void solar_os_gateway_workers_lock(void) { xSemaphoreTake(gateways.worker_lock, portMAX_DELAY); }
void solar_os_gateway_workers_unlock(void) { xSemaphoreGive(gateways.worker_lock); }
void solar_os_gateway_set_runtime_hooks(const solar_os_gateway_runtime_hooks_t *hooks) { gateways.runtime = hooks; }
void *solar_os_gateway_instance_next(solar_os_messaging_provider_id_t *cursor)
{
    solar_os_chat_store_state_t *found = NULL;
    for (solar_os_chat_store_state_t *node = gateways.instances; node; node = node->next)
        if (node->provider > *cursor && (!found || node->provider < found->provider)) found = node;
    if (found) *cursor = found->provider;
    return found;
}

static esp_err_t chat_state_init(void);
static esp_err_t gateway_remove_prepare(void *context);
static void gateway_remove_finish(void *context, bool removed);

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
    const size_t len = strnlen(text, max_len);
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
    /* Gateway v1 has no end-to-end encryption or sender signatures. */
    uint32_t flags = SOLAR_OS_SECURITY_LEGACY_INSECURE;
    chat_lock();
    if (strncmp(chat.url, "chats://", 8) == 0 ||
        strncmp(chat.url, "tls://", 6) == 0) {
        flags |= SOLAR_OS_SECURITY_TRANSPORT_SECURED;
    }
    chat_unlock();
    return flags;
}

static void chat_upsert_room(const char *channel)
{
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = solar_os_gateway_current_id(),
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
    memset(config, 0, sizeof(*config)); config->version = 1;
    size_t desired = 0;
    for (size_t i = 0; i < chat.channel_count; i++)
        if (chat.channels[i].desired) strlcpy(config->channels[desired++], chat.channels[i].name, sizeof(config->channels[0]));
    config->enabled = chat.enabled;
    strlcpy(config->url, chat.url, sizeof(config->url));
    strlcpy(config->token, chat.token, sizeof(config->token));
}

static esp_err_t chat_save_config(const solar_os_chat_saved_config_t *config)
{
    return solar_os_messaging_profile_save(solar_os_gateway_current_id(), config, sizeof(*config));
}

static bool chat_saved_config_valid(const solar_os_chat_saved_config_t *config)
{
    if (config->version != 1 || !chat_string_is_valid(config->url, sizeof(config->url), false) || !chat_url_is_valid(config->url) ||
        !chat_string_is_valid(config->token, sizeof(config->token), true)) return false;
    for (size_t i = 0; i < SOLAR_OS_CHAT_CHANNEL_CAPACITY; i++)
        if (!chat_string_is_valid(config->channels[i], sizeof(config->channels[i]), true)) return false;
    return true;
}

static void chat_apply_saved_config(const solar_os_chat_saved_config_t *config)
{
    strlcpy(chat.url, config->url, sizeof(chat.url));
    strlcpy(chat.token, config->token, sizeof(chat.token));
    chat.configured = true; chat.enabled = config->enabled; chat.config_revision = 1;
    for (size_t i = 0; i < SOLAR_OS_CHAT_CHANNEL_CAPACITY; i++)
        if (config->channels[i][0]) (void)chat_add_channel_locked(config->channels[i], true, false);
}

static void chat_erase_legacy_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    bool found = false;
    const char *keys[] = {CHAT_NVS_URL_KEY, CHAT_NVS_TOKEN_KEY, CHAT_NVS_LEGACY_USER_KEY};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        size_t size = 0;
        if (nvs_get_str(nvs, keys[i], NULL, &size) == ESP_OK) found = true;
    }
    uint8_t enabled;
    if (nvs_get_u8(nvs, CHAT_NVS_ENABLED_KEY, &enabled) == ESP_OK) found = true;
    nvs_close(nvs);
    if (!found || nvs_open(CHAT_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    (void)nvs_erase_key(nvs, CHAT_NVS_URL_KEY); (void)nvs_erase_key(nvs, CHAT_NVS_TOKEN_KEY);
    (void)nvs_erase_key(nvs, CHAT_NVS_ENABLED_KEY); (void)nvs_erase_key(nvs, CHAT_NVS_LEGACY_USER_KEY);
    (void)nvs_commit(nvs); nvs_close(nvs);
}

static void chat_load_config(void)
{
    solar_os_chat_saved_config_t *config = solar_os_memory_calloc(1, sizeof(*config),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gateway.config");
    if (!config) return;
    esp_err_t file_error = solar_os_messaging_profile_load(solar_os_gateway_current_id(), config, sizeof(*config));
    if (file_error == ESP_OK && chat_saved_config_valid(config)) {
        chat_apply_saved_config(config);
        /* Also finish a migration interrupted after file promotion. */
        if (solar_os_gateway_current_id() == SOLAR_OS_MESSAGING_PROVIDER_GATEWAY) chat_erase_legacy_config();
        goto done;
    }
    if (solar_os_gateway_current_id() != SOLAR_OS_MESSAGING_PROVIDER_GATEWAY) goto done;
    /* Read-only legacy fallback. Delete old keys only after a verified file
     * round trip, so an absent/full volume never destroys configuration. */
    nvs_handle_t nvs;
    if (nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) goto done;
    memset(config, 0, sizeof(*config)); config->version = 1;
    size_t size = sizeof(config->url);
    esp_err_t error = nvs_get_str(nvs, CHAT_NVS_URL_KEY, config->url, &size);
    size = sizeof(config->token);
    esp_err_t token_error = nvs_get_str(nvs, CHAT_NVS_TOKEN_KEY, config->token, &size);
    uint8_t enabled = 1;
    (void)nvs_get_u8(nvs, CHAT_NVS_ENABLED_KEY, &enabled); config->enabled = enabled != 0;
    strlcpy(config->channels[0], CHAT_DEFAULT_CHANNEL, sizeof(config->channels[0]));
    nvs_close(nvs);
    if (error == ESP_OK && (token_error == ESP_OK || token_error == ESP_ERR_NVS_NOT_FOUND) && chat_saved_config_valid(config)) {
        chat_apply_saved_config(config);
        if (chat_save_config(config) == ESP_OK &&
            solar_os_messaging_profile_load(solar_os_gateway_current_id(), config, sizeof(*config)) == ESP_OK &&
            chat_saved_config_valid(config)) chat_erase_legacy_config();
    }
done:
    volatile unsigned char *wipe = (volatile unsigned char *)config;
    for (size_t i = 0; i < sizeof(*config); i++) wipe[i] = 0;
    solar_os_memory_free(config);
}

static esp_err_t gateway_join(void *context, const char *target)
{
    if (!context) return ESP_ERR_INVALID_STATE;
    void *previous = solar_os_gateway_scope_enter(context);
    esp_err_t error = solar_os_chat_join(target);
    solar_os_gateway_scope_leave(previous); return error;
}
static esp_err_t gateway_leave(void *context, const char *target)
{
    if (!context) return ESP_ERR_INVALID_STATE;
    void *previous = solar_os_gateway_scope_enter(context);
    esp_err_t error = solar_os_chat_leave(target);
    solar_os_gateway_scope_leave(previous); return error;
}
static esp_err_t gateway_connect(void *context, bool enabled)
{
    if (!context) return ESP_ERR_INVALID_STATE;
    void *previous = solar_os_gateway_scope_enter(context);
    esp_err_t error = enabled ? solar_os_chat_connect(NULL, NULL) : solar_os_chat_disconnect();
    solar_os_gateway_scope_leave(previous); return error;
}
static esp_err_t gateway_create(const char *name, int argc, const char *const *argv,
    solar_os_messaging_provider_id_t *id)
{
    return argc == 1 || argc == 2 ? solar_os_gateway_instance_create(name, argv[0], argc == 2 ? argv[1] : "", id) : ESP_ERR_INVALID_ARG;
}
static esp_err_t gateway_configure(void *context, int argc, const char *const *argv)
{
    if (!context || (argc != 1 && argc != 2)) return ESP_ERR_INVALID_ARG;
    void *previous = solar_os_gateway_scope_enter(context);
    esp_err_t error = solar_os_chat_configure(argv[0], argc == 2 ? argv[1] : NULL);
    solar_os_gateway_scope_leave(previous); return error;
}
static const solar_os_messaging_setting_t gateway_settings_fields[] = {
    {"URL", "Gateway URL", SOLAR_OS_CHAT_URL_MAX, false, true},
    {"Token", "Empty clears token", SOLAR_OS_CHAT_TOKEN_MAX, true, false},
};
static esp_err_t gateway_settings_read(void *context, unsigned form, solar_os_messaging_settings_t *settings)
{
    if (form) return ESP_ERR_NOT_FOUND;
    settings->label = "Gateway"; settings->fields = gateway_settings_fields; settings->count = 2;
    strlcpy(settings->worker, "gateway-sync", sizeof(settings->worker));
    if (context) {
        void *previous = solar_os_gateway_scope_enter(context);
        chat_lock();
        settings->enabled = chat.enabled;
        solar_os_messaging_settings_value(settings, 0, chat.url);
        solar_os_messaging_settings_value(settings, 1, chat.token);
        chat_unlock(); solar_os_gateway_scope_leave(previous);
    }
    return ESP_OK;
}
static esp_err_t gateway_settings_write(void *context, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings, solar_os_messaging_provider_id_t *id)
{
    if (form) return ESP_ERR_NOT_FOUND;
    if (!context) return solar_os_gateway_instance_create(name, settings->values[0], settings->values[1], id);
    void *previous = solar_os_gateway_scope_enter(context);
    esp_err_t error = solar_os_chat_configure(settings->changed[0] ? settings->values[0] : NULL,
        settings->changed[1] ? settings->values[1] : NULL);
    *id = solar_os_gateway_current_id();
    solar_os_gateway_scope_leave(previous); return error;
}
static const solar_os_messaging_provider_class_t gateway_class = {
    .name = "gateway", .connection_oriented = true, .capabilities = SOLAR_OS_ENDPOINT_CAP_ROOM,
    .join = gateway_join, .leave = gateway_leave, .connect = gateway_connect,
    .create = gateway_create, .configure = gateway_configure,
    .settings_read = gateway_settings_read, .settings_write = gateway_settings_write,
    .remove_prepare = gateway_remove_prepare, .remove_finish = gateway_remove_finish,
};

static esp_err_t chat_state_init(void)
{
    if (!selected_chat) {
        esp_err_t error = solar_os_gateway_instances_init();
        return error != ESP_OK ? error : (default_chat.initialized ? ESP_OK : ESP_ERR_NOT_FOUND);
    }
    if (chat.initialized) {
        return ESP_OK;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    chat.lock = xSemaphoreCreateMutex();
    chat.edit_lock = xSemaphoreCreateMutex();
    if (chat.lock == NULL || chat.edit_lock == NULL) {
        if (chat.lock) vSemaphoreDelete(chat.lock);
        if (chat.edit_lock) vSemaphoreDelete(chat.edit_lock);
        memset(&chat, 0, sizeof(chat)); return ESP_ERR_NO_MEM;
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
        vSemaphoreDelete(chat.edit_lock);
        memset(&chat, 0, sizeof(chat));
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_messaging_class_register(&gateway_class);
    chat.next_event_id = 1U;
    chat.next_command_id = 1U;
    chat_load_config();
    (void)chat_add_channel_locked(CHAT_DEFAULT_CHANNEL, !chat.configured, false);
    chat.initialized = true;
    (void)solar_os_messaging_instance_register(
        solar_os_gateway_current_id(), "gateway", solar_os_gateway_current_name(), &chat);
    chat_upsert_room(CHAT_DEFAULT_CHANNEL);
    for (size_t i = 0; i < chat.channel_count; i++) chat_upsert_room(chat.channels[i].name);
    return ESP_OK;
}

static void chat_wipe_config(solar_os_chat_saved_config_t *config)
{
    volatile unsigned char *wipe = (volatile unsigned char *)config;
    for (size_t i = 0; i < sizeof(*config); i++) wipe[i] = 0;
}

static esp_err_t chat_change_connection(const char *url, const char *token, int enabled)
{
    if ((url && url[0] && !chat_url_is_valid(url)) ||
        (token && !chat_string_is_valid(token, SOLAR_OS_CHAT_TOKEN_MAX, true))) return ESP_ERR_INVALID_ARG;
    esp_err_t error = chat_state_init();
    if (error != ESP_OK) return error;
    solar_os_chat_saved_config_t *config = solar_os_memory_calloc(1, sizeof(*config), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gateway.config");
    if (!config) return ESP_ERR_NO_MEM;
    xSemaphoreTake(chat.edit_lock, portMAX_DELAY);
    chat_lock(); chat_snapshot_config_locked(config); chat_unlock();
    if (url && url[0]) strlcpy(config->url, url, sizeof(config->url));
    if (token) strlcpy(config->token, token, sizeof(config->token));
    if (enabled >= 0) config->enabled = enabled != 0;
    error = chat_saved_config_valid(config) ? chat_save_config(config) : ESP_ERR_INVALID_STATE;
    chat_lock();
    if (error == ESP_OK) {
        strlcpy(chat.url, config->url, sizeof(chat.url)); strlcpy(chat.token, config->token, sizeof(chat.token));
        chat.configured = true; chat.enabled = config->enabled;
        if (++chat.config_revision == 0) chat.config_revision = 1;
    } else {
        chat.last_esp_error = error; strlcpy(chat.last_error, esp_err_to_name(error), sizeof(chat.last_error));
    }
    chat_unlock(); xSemaphoreGive(chat.edit_lock);
    chat_wipe_config(config); solar_os_memory_free(config); return error;
}

esp_err_t solar_os_chat_configure(const char *url, const char *token)
{ return chat_change_connection(url, token, -1); }
esp_err_t solar_os_chat_connect(const char *url, const char *token)
{ return chat_change_connection(url, token, 1); }
esp_err_t solar_os_chat_disconnect(void)
{ return chat_change_connection(NULL, NULL, 0); }

static esp_err_t chat_change_membership(const char *channel, solar_os_chat_command_type_t type)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = chat_state_init(); if (error != ESP_OK) return error;
    solar_os_chat_saved_config_t *config = solar_os_memory_calloc(1, sizeof(*config), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gateway.membership");
    if (!config) return ESP_ERR_NO_MEM;
    xSemaphoreTake(chat.edit_lock, portMAX_DELAY);
    chat_lock(); chat_snapshot_config_locked(config);
    int index = chat_channel_index_locked(channel);
    bool room_available = index >= 0 || chat.channel_count < SOLAR_OS_CHAT_CHANNEL_CAPACITY;
    bool queue_available = chat.command_count < SOLAR_OS_CHAT_OUTBOX_CAPACITY;
    chat_unlock();
    bool joining = type == SOLAR_OS_CHAT_COMMAND_JOIN;
    int saved = -1, empty = -1;
    for (size_t i = 0; i < SOLAR_OS_CHAT_CHANNEL_CAPACITY; i++) {
        if (!config->channels[i][0]) empty = (int)i;
        if (strcmp(config->channels[i], channel) == 0) saved = (int)i;
    }
    if (!queue_available || (joining && (!room_available || (saved < 0 && empty < 0)))) error = ESP_ERR_NO_MEM;
    else {
        if (joining && saved < 0) strlcpy(config->channels[empty], channel, sizeof(config->channels[empty]));
        if (!joining && saved >= 0) config->channels[saved][0] = 0;
        error = chat_save_config(config);
    }
    if (error == ESP_OK) {
        uint64_t cursor = joining ? solar_os_chat_channel_cursor(channel) : 0;
        chat_lock();
        error = chat_queue_command_locked(type, channel, cursor);
        if (joining) index = chat_add_channel_locked(channel, true, false);
        else index = chat_channel_index_locked(channel);
        if (index >= 0) chat.channels[index].desired = joining;
        chat_unlock();
    }
    xSemaphoreGive(chat.edit_lock); chat_wipe_config(config); solar_os_memory_free(config);
    if (error == ESP_OK) {
        if (joining) {
            chat_upsert_room(channel);
            (void)solar_os_messaging_join_update(solar_os_gateway_current_id(),
                channel, SOLAR_OS_MESSAGING_JOIN_PENDING, NULL);
        } else if (type == SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL)
            (void)solar_os_messaging_conversation_remove(solar_os_gateway_current_id(), channel);
    }
    return error;
}

esp_err_t solar_os_chat_join(const char *channel)
{ return chat_change_membership(channel, SOLAR_OS_CHAT_COMMAND_JOIN); }
esp_err_t solar_os_chat_leave(const char *channel)
{ return chat_change_membership(channel, SOLAR_OS_CHAT_COMMAND_LEAVE); }
esp_err_t solar_os_chat_delete_channel(const char *channel)
{ return chat_change_membership(channel, SOLAR_OS_CHAT_COMMAND_DELETE_CHANNEL); }

esp_err_t solar_os_chat_send(const char *channel, const char *text)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false) ||
        text == NULL || text[0] == '\0' ||
        strlen(text) >= SOLAR_OS_CHAT_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = chat_state_init();
    if (error != ESP_OK) {
        return error;
    }
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = solar_os_gateway_current_id(),
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
    esp_err_t error = chat_state_init();
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
    esp_err_t error = chat_state_init();
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
        solar_os_gateway_current_id(),
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
                solar_os_gateway_current_id() &&
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
    esp_err_t error = chat_state_init();
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
    esp_err_t error = chat_state_init();
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
        chat_state_init() != ESP_OK) {
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
        solar_os_gateway_current_id(),
        channel);
}

esp_err_t solar_os_gateway_sync_set_status(bool running,
                                        bool connected,
                                        esp_err_t error,
                                        const char *message)
{
    esp_err_t init_error = chat_state_init();
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
        solar_os_gateway_current_id(),
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
    esp_err_t error = chat_state_init();
    if (error != ESP_OK) {
        return error;
    }
    uint64_t key = event->message_key;
    if (event->type == SOLAR_OS_CHAT_EVENT_MESSAGE) {
        const uint32_t security_flags = chat_security_flags();
        const solar_os_messaging_inbound_t request = {
            .provider = solar_os_gateway_current_id(),
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
        if (event->type == SOLAR_OS_CHAT_EVENT_JOINED)
            (void)solar_os_messaging_join_update(solar_os_gateway_current_id(),
                event->channel, SOLAR_OS_MESSAGING_JOIN_JOINED, NULL);
    } else if (event->type == SOLAR_OS_CHAT_EVENT_LEFT) {
        (void)solar_os_messaging_join_update(solar_os_gateway_current_id(),
            event->channel, SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
    } else if (event->type == SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED) {
        (void)solar_os_messaging_conversation_remove(
            solar_os_gateway_current_id(),
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
        solar_os_gateway_current_id());
}

esp_err_t solar_os_chat_outbox_peek(solar_os_chat_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = chat_state_init();
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
    esp_err_t error = chat_state_init();
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


static void gateway_store_free(solar_os_chat_store_state_t *store)
{
    solar_os_memory_free(store->events); solar_os_memory_free(store->commands);
    if (store->lock) vSemaphoreDelete(store->lock);
    if (store->edit_lock) vSemaphoreDelete(store->edit_lock);
    volatile unsigned char *bytes = (volatile unsigned char *)store;
    for (size_t i = 0; i < sizeof(*store); i++) bytes[i] = 0;
    if (store != &default_chat) solar_os_memory_free(store);
}

static esp_err_t gateway_attach(solar_os_messaging_provider_id_t id, const char *name)
{
    for (solar_os_chat_store_state_t *node = gateways.instances; node; node = node->next)
        if (node->provider == id) return ESP_OK;
    solar_os_chat_store_state_t *store = id == SOLAR_OS_MESSAGING_PROVIDER_GATEWAY ? &default_chat :
        solar_os_memory_calloc(1, sizeof(*store), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gateway.instance");
    if (!store) return ESP_ERR_NO_MEM;
    store->provider = id; strlcpy(store->instance_name, name, sizeof(store->instance_name));
    void *previous = solar_os_gateway_scope_enter(store);
    esp_err_t error = chat_state_init();
    solar_os_gateway_scope_leave(previous);
    if (error != ESP_OK) { gateway_store_free(store); return error; }
    solar_os_gateway_workers_lock(); store->next = gateways.instances; gateways.instances = store; solar_os_gateway_workers_unlock();
    return ESP_OK;
}

esp_err_t solar_os_gateway_instances_init(void)
{
    if (atomic_load(&gateway_ready)) return ESP_OK;
    while (atomic_flag_test_and_set(&gateway_init_lock)) vTaskDelay(1);
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) goto done;
    if (!gateways.edit_lock) gateways.edit_lock = xSemaphoreCreateMutex();
    if (!gateways.worker_lock) gateways.worker_lock = xSemaphoreCreateMutex();
    if (!gateways.edit_lock || !gateways.worker_lock) { error = ESP_ERR_NO_MEM; goto done; }
    error = solar_os_messaging_class_register(&gateway_class);
    if (error != ESP_OK) goto done;
    xSemaphoreTake(gateways.edit_lock, portMAX_DELAY);
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status;
    while (solar_os_messaging_provider_next(&cursor, &status)) {
        if (strcmp(status.class_name, "gateway") != 0) continue;
        error = gateway_attach(cursor, status.name);
        if (error != ESP_OK) break;
    }
    gateways.initialized = error == ESP_OK;
    atomic_store(&gateway_ready, gateways.initialized);
    xSemaphoreGive(gateways.edit_lock);
done:
    atomic_flag_clear(&gateway_init_lock);
    return error;
}
esp_err_t solar_os_chat_init(void) { return solar_os_gateway_instances_init(); }

esp_err_t solar_os_gateway_instance_create(const char *name, const char *url,
    const char *token, solar_os_messaging_provider_id_t *id)
{
    if (!id || !chat_url_is_valid(url) || !chat_string_is_valid(token, SOLAR_OS_CHAT_TOKEN_MAX, true)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_gateway_instances_init(); if (error != ESP_OK) return error;
    solar_os_chat_saved_config_t *config = solar_os_memory_calloc(1, sizeof(*config), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gateway.config");
    if (!config) return ESP_ERR_NO_MEM;
    config->version = 1; strlcpy(config->url, url, sizeof(config->url));
    strlcpy(config->token, token ? token : "", sizeof(config->token));
    xSemaphoreTake(gateways.edit_lock, portMAX_DELAY);
    error = solar_os_messaging_instance_create("gateway", name, config, sizeof(*config), id);
    if (error == ESP_OK) error = gateway_attach(*id, name);
    if (error != ESP_OK) atomic_store(&gateway_ready, false);
    xSemaphoreGive(gateways.edit_lock); chat_wipe_config(config); solar_os_memory_free(config); return error;
}

static solar_os_chat_store_state_t *gateway_find(solar_os_messaging_provider_id_t id)
{
    for (solar_os_chat_store_state_t *node = gateways.instances; node; node = node->next)
        if (node->provider == id) return node;
    return NULL;
}

esp_err_t solar_os_gateway_instance_configure(solar_os_messaging_provider_id_t id, const char *url, const char *token)
{
    esp_err_t error = solar_os_gateway_instances_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(gateways.edit_lock, portMAX_DELAY);
    solar_os_chat_store_state_t *store = gateway_find(id);
    if (!store) error = ESP_ERR_NOT_FOUND;
    else {
        void *previous = solar_os_gateway_scope_enter(store);
        error = solar_os_chat_configure(url, token);
        solar_os_gateway_scope_leave(previous);
    }
    xSemaphoreGive(gateways.edit_lock); return error;
}
esp_err_t solar_os_gateway_instance_get_status(solar_os_messaging_provider_id_t id, solar_os_chat_status_t *status)
{
    esp_err_t error = solar_os_gateway_instances_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(gateways.edit_lock, portMAX_DELAY);
    solar_os_chat_store_state_t *store = gateway_find(id);
    if (!store) error = ESP_ERR_NOT_FOUND;
    else {
        void *previous = solar_os_gateway_scope_enter(store);
        error = solar_os_chat_get_status(status);
        solar_os_gateway_scope_leave(previous);
    }
    xSemaphoreGive(gateways.edit_lock); return error;
}
size_t solar_os_gateway_instance_channels(solar_os_messaging_provider_id_t id, solar_os_chat_channel_t *channels, size_t capacity)
{
    if (solar_os_gateway_instances_init() != ESP_OK) return 0;
    xSemaphoreTake(gateways.edit_lock, portMAX_DELAY);
    solar_os_chat_store_state_t *store = gateway_find(id); size_t count = 0;
    if (store) {
        void *previous = solar_os_gateway_scope_enter(store);
        count = solar_os_chat_channel_snapshot(channels, capacity);
        solar_os_gateway_scope_leave(previous);
    }
    xSemaphoreGive(gateways.edit_lock); return count;
}

static esp_err_t gateway_remove_prepare(void *context)
{
    xSemaphoreTake(gateways.edit_lock, portMAX_DELAY); solar_os_gateway_workers_lock();
    solar_os_chat_store_state_t *store = context;
    xSemaphoreTake(store->edit_lock, portMAX_DELAY);
    void *previous = solar_os_gateway_scope_enter(store);
    esp_err_t error = gateways.runtime ? gateways.runtime->prepare() : ESP_OK;
    solar_os_gateway_scope_leave(previous);
    if (error != ESP_OK) {
        xSemaphoreGive(store->edit_lock); solar_os_gateway_workers_unlock(); xSemaphoreGive(gateways.edit_lock);
    }
    return error;
}
static void gateway_remove_finish(void *context, bool removed)
{
    solar_os_chat_store_state_t *store = context;
    if (removed) {
        void *previous = solar_os_gateway_scope_enter(store);
        if (gateways.runtime) gateways.runtime->destroy();
        solar_os_gateway_scope_leave(previous);
        solar_os_chat_store_state_t **link = &gateways.instances;
        while (*link && *link != store) link = &(*link)->next;
        if (*link) *link = store->next;
    }
    xSemaphoreGive(store->edit_lock);
    if (removed) gateway_store_free(store);
    solar_os_gateway_workers_unlock(); xSemaphoreGive(gateways.edit_lock);
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
