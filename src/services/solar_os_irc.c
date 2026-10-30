#include "solar_os_irc.h"
#include "solar_os_irc_protocol.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "solar_os_jobs.h"
#include "solar_os_storage.h"

#define IRC_CONTROL_CAPACITY 8U
#define IRC_TX_INTERVAL_MS 1000U
#define IRC_CONNECT_TIMEOUT_MS 15000U
#define IRC_IDLE_PING_MS 60000U
#define IRC_DEAD_TIMEOUT_MS 90000U

typedef struct irc_instance {
    struct irc_instance *next;
    solar_os_messaging_provider_id_t id;
    solar_os_irc_config_t config;       /* Protected by irc.lock. */
    uint32_t revision;
    solar_os_irc_config_t active;       /* Worker-owned from here down. */
    uint32_t active_revision;
    esp_tls_t *tls;
    esp_tls_cfg_t tls_config;
    char *ca;
    bool tls_ready, registered, sasl_ok, cap_sasl, sasl_started, sasl_payload_sent;
    bool dropping, ping_pending, tx_throttled;
    solar_os_irc_casemapping_t casemapping;
    unsigned registration_step;
    unsigned nick_retries;
    char nick[32];
    char chantypes[8];
    uint32_t retry_at, retry_delay, connect_at, last_rx, ping_at, next_tx, tx_at;
    char rx[SOLAR_OS_IRC_LINE_MAX];
    size_t rx_used;
    char controls[IRC_CONTROL_CAPACITY][SOLAR_OS_IRC_LINE_MAX];
    size_t control_head, control_count;
    char pong[SOLAR_OS_IRC_LINE_MAX];
    char tx[SOLAR_OS_IRC_LINE_MAX];
    size_t tx_used, tx_size;
    uint32_t outbound_id;
    uint64_t receive_sequence;
    solar_os_messaging_outbound_t outbound;
    char joined[SOLAR_OS_IRC_CHANNEL_CAPACITY][SOLAR_OS_IRC_TARGET_MAX];
    char attempted[SOLAR_OS_IRC_CHANNEL_CAPACITY][SOLAR_OS_IRC_TARGET_MAX];
} irc_instance_t;

static struct {
    bool initialized, running;
    volatile bool stop_requested, worker_done;
    SemaphoreHandle_t lock, edit_lock, worker_lock;
    TaskHandle_t worker;
    irc_instance_t *instances;
} irc;

static esp_err_t irc_join(void *context, const char *target);
static esp_err_t irc_leave(void *context, const char *target);
static esp_err_t irc_direct(void *context, const char *target, solar_os_conversation_id_t *conversation);
static esp_err_t irc_connect(void *context, bool enabled);
static esp_err_t irc_normalize(void *context, const char *target, char *out, size_t size);
static esp_err_t irc_remove_prepare(void *context);
static void irc_remove_finish(void *context, bool removed);
static esp_err_t irc_class_create(const char *name, int argc, const char *const *argv, solar_os_messaging_provider_id_t *id);
static esp_err_t irc_class_configure(void *context, int argc, const char *const *argv);
static esp_err_t irc_settings_read(void *context, unsigned form, solar_os_messaging_settings_t *settings);
static esp_err_t irc_settings_write(void *context, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings, solar_os_messaging_provider_id_t *id);
static const solar_os_messaging_provider_class_t irc_class = {
    .name = "irc", .connection_oriented = true, .capabilities = SOLAR_OS_ENDPOINT_CAP_ROOM | SOLAR_OS_ENDPOINT_CAP_DIRECT,
    .join = irc_join, .leave = irc_leave, .direct_open = irc_direct, .connect = irc_connect,
    .normalize_target = irc_normalize,
    .remove_prepare = irc_remove_prepare, .remove_finish = irc_remove_finish,
    .create = irc_class_create, .configure = irc_class_configure,
    .settings_read = irc_settings_read, .settings_write = irc_settings_write,
};

static uint32_t irc_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool irc_due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static void irc_wipe(void *memory, size_t size)
{ volatile unsigned char *p = memory; while (size--) *p++ = 0; }

/* Commands can run on the 8 KiB display task. Configuration copies contain
 * channel lists and credentials; keep them off that task's nested stack. */
static solar_os_irc_config_t *irc_config_alloc(void)
{
    return solar_os_memory_calloc(1, sizeof(solar_os_irc_config_t),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "irc.config");
}

static void irc_config_free(solar_os_irc_config_t *config)
{
    if (config) { irc_wipe(config, sizeof(*config)); solar_os_memory_free(config); }
}

static irc_instance_t *irc_find(solar_os_messaging_provider_id_t id)
{
    xSemaphoreTake(irc.lock, portMAX_DELAY);
    irc_instance_t *node = irc.instances;
    while (node && node->id != id) node = node->next;
    xSemaphoreGive(irc.lock); return node;
}

static bool irc_secret_valid(const char *text, size_t size)
{
    if (strnlen(text, size) >= size) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool irc_config_valid(const solar_os_irc_config_t *config)
{
    if (!config || config->version != 1 || !config->port ||
        !solar_os_irc_atom_valid(config->host, sizeof(config->host)) ||
        !solar_os_irc_nick_valid(config->nick, sizeof(config->nick)) ||
        !irc_secret_valid(config->account, sizeof(config->account)) ||
        !irc_secret_valid(config->password, sizeof(config->password)) ||
        !irc_secret_valid(config->server_password, sizeof(config->server_password)) ||
        !irc_secret_valid(config->ca_file, sizeof(config->ca_file)) ||
        (!!config->account[0] != !!config->password[0])) return false;
    for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++)
        if (config->channels[i][0] && !solar_os_irc_target_valid(config->channels[i], SOLAR_OS_IRC_TARGET_MAX)) return false;
    return true;
}

static esp_err_t irc_attach(solar_os_messaging_provider_id_t id, const solar_os_irc_config_t *config)
{
    if (irc_find(id)) return ESP_OK;
    if (!irc_config_valid(config)) return ESP_ERR_INVALID_ARG;
    solar_os_messaging_provider_status_t status;
    esp_err_t error = solar_os_messaging_provider_get_status(id, &status);
    if (error != ESP_OK) return error;
    irc_instance_t *node = solar_os_memory_calloc(1, sizeof(*node), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "irc.instance");
    if (!node) return ESP_ERR_NO_MEM;
    node->id = id; node->config = *config; node->retry_delay = 1000;
    node->casemapping = SOLAR_OS_IRC_CASE_RFC1459;
    error = solar_os_messaging_instance_register(id, "irc", status.name, node);
    if (error != ESP_OK) { irc_wipe(node, sizeof(*node)); solar_os_memory_free(node); return error; }
    xSemaphoreTake(irc.lock, portMAX_DELAY);
    node->next = irc.instances; irc.instances = node;
    xSemaphoreGive(irc.lock);
    for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++) {
        if (!config->channels[i][0]) continue;
        const solar_os_messaging_conversation_upsert_t request = {.provider = id,
            .provider_key = config->channels[i], .title = config->channels[i], .kind = SOLAR_OS_CONVERSATION_ROOM,
            .security_flags = SOLAR_OS_SECURITY_TRANSPORT_SECURED | SOLAR_OS_SECURITY_SENDER_UNVERIFIED};
        (void)solar_os_messaging_conversation_upsert(&request, NULL);
        (void)solar_os_messaging_join_update(id, config->channels[i], SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
    }
    return ESP_OK;
}

esp_err_t solar_os_irc_init(void)
{
    if (irc.initialized) return ESP_OK;
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) return error;
    irc.lock = xSemaphoreCreateMutex(); irc.edit_lock = xSemaphoreCreateMutex();
    irc.worker_lock = xSemaphoreCreateMutex();
    if (!irc.lock || !irc.edit_lock || !irc.worker_lock) {
        if (irc.lock) vSemaphoreDelete(irc.lock);
        if (irc.edit_lock) vSemaphoreDelete(irc.edit_lock);
        if (irc.worker_lock) vSemaphoreDelete(irc.worker_lock);
        memset(&irc, 0, sizeof(irc)); return ESP_ERR_NO_MEM;
    }
    error = solar_os_messaging_class_register(&irc_class);
    if (error != ESP_OK) return error;
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status;
    solar_os_irc_config_t *config = irc_config_alloc();
    if (!config) {
        vSemaphoreDelete(irc.lock); vSemaphoreDelete(irc.edit_lock);
        vSemaphoreDelete(irc.worker_lock);
        irc.lock = irc.edit_lock = irc.worker_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    irc.initialized = true;
    while (solar_os_messaging_provider_next(&cursor, &status)) {
        if (strcmp(status.class_name, "irc") != 0) continue;
        error = solar_os_messaging_profile_load(cursor, config, sizeof(*config));
        if (error == ESP_OK) error = irc_attach(cursor, config);
        if (error != ESP_OK) (void)solar_os_messaging_provider_set_status(cursor, false, false, error, "IRC profile unavailable or invalid");
        irc_wipe(config, sizeof(*config));
    }
    irc_config_free(config);
    return ESP_OK;
}

esp_err_t solar_os_irc_create(const char *name, const char *host, uint16_t port,
    const char *nick, solar_os_messaging_provider_id_t *id)
{
    if (!host || !nick || !id) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_irc_init();
    if (error != ESP_OK) return error;
    solar_os_irc_config_t *config = irc_config_alloc();
    if (!config) return ESP_ERR_NO_MEM;
    config->version = 1; config->port = port;
    if (strlcpy(config->host, host, sizeof(config->host)) >= sizeof(config->host) ||
        strlcpy(config->nick, nick, sizeof(config->nick)) >= sizeof(config->nick) || !irc_config_valid(config)) {
        irc_config_free(config); return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    error = solar_os_messaging_instance_create("irc", name, config, sizeof(*config), id);
    if (error == ESP_OK) error = irc_attach(*id, config);
    xSemaphoreGive(irc.edit_lock); irc_config_free(config); return error;
}

esp_err_t solar_os_irc_get_config(solar_os_messaging_provider_id_t id, solar_os_irc_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_irc_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    irc_instance_t *node = irc_find(id);
    if (!node) error = solar_os_messaging_provider_is_class(id, "irc") ?
        solar_os_messaging_profile_load(id, config, sizeof(*config)) : ESP_ERR_NOT_FOUND;
    else { xSemaphoreTake(irc.lock, portMAX_DELAY); *config = node->config; xSemaphoreGive(irc.lock); }
    xSemaphoreGive(irc.edit_lock);
    return error;
}

/* Caller owns edit_lock, but never the runtime lock during filesystem I/O. */
static esp_err_t irc_save(irc_instance_t *node, const solar_os_irc_config_t *config)
{
    esp_err_t error = solar_os_messaging_profile_save(node->id, config, sizeof(*config));
    if (error == ESP_OK) {
        xSemaphoreTake(irc.lock, portMAX_DELAY); node->config = *config; node->revision++; xSemaphoreGive(irc.lock);
        if (irc.worker && !irc.worker_done) xTaskNotifyGive(irc.worker);
    }
    return error;
}

esp_err_t solar_os_irc_configure(solar_os_messaging_provider_id_t id, const solar_os_irc_config_t *config)
{
    if (!irc_config_valid(config)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_irc_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    irc_instance_t *node = irc_find(id);
    if (node) error = irc_save(node, config);
    else if (!solar_os_messaging_provider_is_class(id, "irc")) error = ESP_ERR_NOT_FOUND;
    else {
        error = solar_os_messaging_profile_save(id, config, sizeof(*config));
        if (error == ESP_OK) error = irc_attach(id, config);
    }
    xSemaphoreGive(irc.edit_lock);
    return error;
}

static bool irc_parse_port(const char *text, uint16_t *port)
{
    if (!text || !text[0]) return false;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + *p - '0'; if (value > 65535) return false;
    }
    *port = value; return value != 0;
}
static const solar_os_messaging_setting_t irc_settings_fields[] = {
    {"Host", "TLS server hostname", 128, false, true},
    {"Port", "TLS port, usually 6697", 6, false, true},
    {"Nickname", "IRC nickname", 32, false, true},
    {"SASL account", "Empty disables SASL", 64, false, false},
    {"SASL password", "Empty disables SASL", 128, true, false},
    {"Server password", "Empty clears password", 128, true, false},
    {"CA file", "Empty uses default CAs", 160, false, false},
};
static esp_err_t irc_settings_read(void *context, unsigned form, solar_os_messaging_settings_t *settings)
{
    if (form) return ESP_ERR_NOT_FOUND;
    settings->label = "IRC"; settings->fields = irc_settings_fields; settings->count = 7;
    strlcpy(settings->worker, "irc-sync", sizeof(settings->worker));
    if (!context) { strlcpy(settings->values[1], "6697", sizeof(settings->values[1])); return ESP_OK; }
    irc_instance_t *node = context;
    xSemaphoreTake(irc.lock, portMAX_DELAY);
    settings->enabled = node->config.enabled;
    solar_os_messaging_settings_value(settings, 0, node->config.host);
    snprintf(settings->values[1], sizeof(settings->values[1]), "%u", node->config.port);
    solar_os_messaging_settings_value(settings, 2, node->config.nick);
    solar_os_messaging_settings_value(settings, 3, node->config.account);
    solar_os_messaging_settings_value(settings, 4, node->config.password);
    solar_os_messaging_settings_value(settings, 5, node->config.server_password);
    solar_os_messaging_settings_value(settings, 6, node->config.ca_file);
    xSemaphoreGive(irc.lock); return ESP_OK;
}
static esp_err_t irc_settings_write(void *context, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings, solar_os_messaging_provider_id_t *id)
{
    if (form) return ESP_ERR_NOT_FOUND;
    solar_os_irc_config_t *config = irc_config_alloc();
    if (!config) return ESP_ERR_NO_MEM;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    irc_instance_t *node = context;
    if (node) { xSemaphoreTake(irc.lock, portMAX_DELAY); *config = node->config; xSemaphoreGive(irc.lock); }
    else config->version = 1;
    char *destinations[] = {config->host, NULL, config->nick, config->account,
        config->password, config->server_password, config->ca_file};
    bool valid = true;
    for (size_t i = 0; i < 7; i++) {
        if (node && !settings->changed[i]) continue;
        if (i == 1) valid = irc_parse_port(settings->values[i], &config->port) && valid;
        else strlcpy(destinations[i], settings->values[i], irc_settings_fields[i].capacity);
    }
    if (node && ((settings->changed[3] && !config->account[0]) ||
                 (settings->changed[4] && !config->password[0]))) {
        irc_wipe(config->account, sizeof(config->account));
        irc_wipe(config->password, sizeof(config->password));
    }
    esp_err_t error = valid && irc_config_valid(config) ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (error == ESP_OK && node) { *id = node->id; error = irc_save(node, config); }
    else if (error == ESP_OK) {
        error = solar_os_messaging_instance_create("irc", name, config, sizeof(*config), id);
        if (error == ESP_OK) error = irc_attach(*id, config);
    }
    xSemaphoreGive(irc.edit_lock); irc_config_free(config); return error;
}
static esp_err_t irc_class_create(const char *name, int argc, const char *const *argv, solar_os_messaging_provider_id_t *id)
{
    uint16_t port;
    if (argc != 3 || !irc_parse_port(argv[1], &port)) return ESP_ERR_INVALID_ARG;
    return solar_os_irc_create(name, argv[0], port, argv[2], id);
}
static esp_err_t irc_class_configure(void *context, int argc, const char *const *argv)
{
    irc_instance_t *node = context;
    if (!node || !argc) return ESP_ERR_INVALID_ARG;
    solar_os_irc_config_t *config = irc_config_alloc(); if (!config) return ESP_ERR_NO_MEM;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    xSemaphoreTake(irc.lock, portMAX_DELAY); *config = node->config; xSemaphoreGive(irc.lock);
    bool valid = false;
    if (argc == 3 && !strcmp(argv[0], "sasl")) {
        valid = strlcpy(config->account, argv[1], sizeof(config->account)) < sizeof(config->account) &&
            strlcpy(config->password, argv[2], sizeof(config->password)) < sizeof(config->password);
    } else if (argc == 2 && !strcmp(argv[0], "sasl") && !strcmp(argv[1], "off")) {
        irc_wipe(config->account, sizeof(config->account)); irc_wipe(config->password, sizeof(config->password)); valid = true;
    } else if (argc == 2 && !strcmp(argv[0], "pass")) {
        valid = strlcpy(config->server_password, !strcmp(argv[1], "off") ? "" : argv[1], sizeof(config->server_password)) < sizeof(config->server_password);
    } else if (argc == 2 && !strcmp(argv[0], "ca")) {
        valid = strlcpy(config->ca_file, !strcmp(argv[1], "default") ? "" : argv[1], sizeof(config->ca_file)) < sizeof(config->ca_file);
    } else if (argc == 3) {
        valid = irc_parse_port(argv[1], &config->port) && strlcpy(config->host, argv[0], sizeof(config->host)) < sizeof(config->host) &&
            strlcpy(config->nick, argv[2], sizeof(config->nick)) < sizeof(config->nick);
    }
    esp_err_t error = valid && irc_config_valid(config) ? irc_save(node, config) : ESP_ERR_INVALID_ARG;
    xSemaphoreGive(irc.edit_lock); irc_config_free(config); return error;
}

static esp_err_t irc_connect(void *context, bool enabled)
{
    irc_instance_t *node = context; if (!node) return ESP_ERR_INVALID_STATE;
    solar_os_irc_config_t *config = irc_config_alloc();
    if (!config) return ESP_ERR_NO_MEM;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    xSemaphoreTake(irc.lock, portMAX_DELAY); *config = node->config; xSemaphoreGive(irc.lock);
    config->enabled = enabled;
    esp_err_t error = irc_save(node, config);
    xSemaphoreGive(irc.edit_lock); irc_config_free(config); return error;
}

esp_err_t solar_os_irc_enable(solar_os_messaging_provider_id_t id, bool enabled)
{
    esp_err_t error = solar_os_irc_init(); if (error != ESP_OK) return error;
    return solar_os_messaging_connect(id, enabled);
}

static esp_err_t irc_channel_change(irc_instance_t *node, const char *target, bool join)
{
    if (!node || !solar_os_irc_target_valid(target, SOLAR_OS_IRC_TARGET_MAX) || !strchr("#&+!", target[0])) return ESP_ERR_INVALID_ARG;
    solar_os_irc_config_t *config = irc_config_alloc();
    if (!config) return ESP_ERR_NO_MEM;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    xSemaphoreTake(irc.lock, portMAX_DELAY); *config = node->config; xSemaphoreGive(irc.lock);
    char key[SOLAR_OS_IRC_TARGET_MAX];
    /* Default network mapping until ISUPPORT arrives; live worker also folds
     * received names using the server-advertised mapping. */
    (void)irc_normalize(node, target, key, sizeof(key));
    int found = -1, empty = -1;
    for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++) {
        if (!config->channels[i][0]) empty = (int)i;
        if (strcmp(config->channels[i], key) == 0) found = (int)i;
    }
    esp_err_t error = ESP_OK;
    if (join && found < 0) {
        if (empty < 0) error = ESP_ERR_NO_MEM;
        else strlcpy(config->channels[empty], key, sizeof(config->channels[empty]));
    } else if (!join && found >= 0) config->channels[found][0] = 0;
    if (error == ESP_OK) error = irc_save(node, config);
    irc_config_free(config); xSemaphoreGive(irc.edit_lock);
    return error;
}
static esp_err_t irc_join(void *context, const char *target) { return irc_channel_change(context, target, true); }
static esp_err_t irc_leave(void *context, const char *target) { return irc_channel_change(context, target, false); }

static esp_err_t irc_direct(void *context, const char *target, solar_os_conversation_id_t *conversation)
{
    irc_instance_t *node = context;
    if (!node || !solar_os_irc_target_valid(target, SOLAR_OS_IRC_TARGET_MAX) || strchr("#&+!", target[0])) return ESP_ERR_INVALID_ARG;
    char key[SOLAR_OS_IRC_TARGET_MAX];
    (void)irc_normalize(node, target, key, sizeof(key));
    const solar_os_messaging_conversation_upsert_t request = {.provider = node->id,
        .provider_key = key, .title = target, .kind = SOLAR_OS_CONVERSATION_DIRECT,
        .security_flags = SOLAR_OS_SECURITY_TRANSPORT_SECURED | SOLAR_OS_SECURITY_SENDER_UNVERIFIED};
    return solar_os_messaging_conversation_upsert(&request, conversation);
}

static esp_err_t irc_normalize(void *context, const char *target, char *out, size_t size)
{
    irc_instance_t *node = context;
    if (!node || !solar_os_irc_target_valid(target, size)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(irc.lock, portMAX_DELAY);
    solar_os_irc_casemapping_t mapping = node->casemapping;
    xSemaphoreGive(irc.lock);
    solar_os_irc_casefold(target, out, size, mapping);
    return ESP_OK;
}

static void irc_status(irc_instance_t *node, esp_err_t error, const char *detail)
{ (void)solar_os_messaging_provider_set_status(node->id,
    irc.running && node->active.enabled, node->registered, error, detail); }

static void irc_close(irc_instance_t *node, esp_err_t error, const char *reason, bool retry)
{
    if (node->outbound_id) {
        (void)solar_os_messaging_outbox_update(node->outbound_id, SOLAR_OS_DELIVERY_FAILED,
            "IRC connection lost; send outcome unknown, not retried");
        node->outbound_id = 0;
    }
    if (node->tls) esp_tls_conn_destroy(node->tls);
    node->tls = NULL; solar_os_memory_free(node->ca); node->ca = NULL;
    node->tls_ready = false; node->registered = false; node->sasl_ok = false;
    node->cap_sasl = false; node->ping_pending = false;
    node->sasl_started = node->sasl_payload_sent = false;
    node->rx_used = node->tx_size = node->tx_used = 0;
    node->tx_throttled = false;
    node->control_count = node->control_head = 0; node->pong[0] = 0; node->dropping = false;
    node->registration_step = node->nick_retries = 0;
    irc_wipe(node->controls, sizeof(node->controls)); irc_wipe(node->tx, sizeof(node->tx));
    memset(node->joined, 0, sizeof(node->joined)); memset(node->attempted, 0, sizeof(node->attempted));
    for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++)
        if (node->active.channels[i][0]) (void)solar_os_messaging_join_update(node->id,
            node->active.channels[i], SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
    node->retry_at = irc_now() + node->retry_delay;
    if (retry && node->retry_delay < 60000) node->retry_delay *= 2;
    if (node->retry_delay > 60000) node->retry_delay = 60000;
    irc_status(node, error, reason);
}

static esp_err_t irc_remove_prepare(void *context)
{
    if (!context) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(irc.edit_lock, portMAX_DELAY);
    xSemaphoreTake(irc.worker_lock, portMAX_DELAY);
    return ESP_OK;
}

static void irc_remove_finish(void *context, bool removed)
{
    irc_instance_t *node = context;
    if (removed) {
        irc_close(node, ESP_OK, "instance removed", false);
        xSemaphoreTake(irc.lock, portMAX_DELAY);
        irc_instance_t **link = &irc.instances;
        while (*link && *link != node) link = &(*link)->next;
        if (*link) *link = node->next;
        xSemaphoreGive(irc.lock);
        irc_wipe(node, sizeof(*node)); solar_os_memory_free(node);
    }
    xSemaphoreGive(irc.worker_lock);
    xSemaphoreGive(irc.edit_lock);
}

static bool irc_control(irc_instance_t *node, const char *command)
{
    if (node->control_count == IRC_CONTROL_CAPACITY || strlen(command) + 2 >= SOLAR_OS_IRC_LINE_MAX) {
        irc_close(node, ESP_ERR_NO_MEM, "IRC control queue overflow", true); return false;
    }
    size_t index = (node->control_head + node->control_count++) % IRC_CONTROL_CAPACITY;
    snprintf(node->controls[index], sizeof(node->controls[index]), "%s\r\n", command);
    return true;
}

static bool irc_equal(irc_instance_t *node, const char *a, const char *b)
{
    char x[SOLAR_OS_IRC_TARGET_MAX], y[SOLAR_OS_IRC_TARGET_MAX];
    solar_os_irc_casefold(a, x, sizeof(x), node->casemapping);
    solar_os_irc_casefold(b, y, sizeof(y), node->casemapping); return strcmp(x, y) == 0;
}

static void irc_received(irc_instance_t *node, char *line)
{
    solar_os_irc_message_t message;
    if (!solar_os_irc_parse(line, &message)) return;
    const char *command = message.command;
    char **p = message.params; size_t count = message.count;
    char sender[SOLAR_OS_IRC_TARGET_MAX] = "";
    if (message.prefix) {
        size_t length = strcspn(message.prefix, "!@");
        if (length >= sizeof(sender)) return;
        memcpy(sender, message.prefix, length); sender[length] = 0;
    }
    if (strcmp(command, "PING") == 0 && count) {
        if (snprintf(node->pong, sizeof(node->pong), "PONG :%s\r\n", p[count - 1]) >= (int)sizeof(node->pong))
            irc_close(node, ESP_ERR_INVALID_SIZE, "IRC PING too long", true);
        return;
    }
    if (strcmp(command, "PONG") == 0) { node->ping_pending = false; return; }
    if (strcmp(command, "ERROR") == 0) { irc_close(node, ESP_FAIL, "IRC server closed connection", true); return; }
    if (strcmp(command, "CAP") == 0 && count >= 3) {
        if (strcmp(p[1], "LS") == 0) {
            char *caps = p[count - 1], *save = NULL;
            for (char *cap = strtok_r(caps, " ", &save); cap; cap = strtok_r(NULL, " ", &save)) {
                if (strcmp(cap, "sasl") == 0) node->cap_sasl = true;
                else if (strncmp(cap, "sasl=", 5) == 0) {
                    char *mechanisms = cap + 5, *mechanism_save = NULL;
                    for (char *mechanism = strtok_r(mechanisms, ",", &mechanism_save); mechanism;
                         mechanism = strtok_r(NULL, ",", &mechanism_save))
                        if (strcmp(mechanism, "PLAIN") == 0) node->cap_sasl = true;
                }
            }
            if (count == 3) {
                if (node->cap_sasl) (void)irc_control(node, "CAP REQ :sasl");
                else irc_close(node, ESP_ERR_NOT_SUPPORTED, "SASL required but unavailable", true);
            }
        } else if (strcmp(p[1], "ACK") == 0 && node->active.account[0]) {
            node->sasl_started = true;
            (void)irc_control(node, "AUTHENTICATE PLAIN");
        } else if (strcmp(p[1], "NAK") == 0) irc_close(node, ESP_FAIL, "IRC SASL capability rejected", true);
        return;
    }
    if (strcmp(command, "AUTHENTICATE") == 0 && count == 1 && strcmp(p[0], "+") == 0 &&
        node->sasl_started && !node->sasl_payload_sent) {
        node->sasl_payload_sent = true;
        unsigned char plain[256], encoded[352]; size_t used = 0, encoded_size = 0;
        plain[used++] = 0;
        size_t account_size = strlen(node->active.account), password_size = strlen(node->active.password);
        memcpy(plain + used, node->active.account, account_size); used += account_size; plain[used++] = 0;
        memcpy(plain + used, node->active.password, password_size); used += password_size;
        char auth[400];
        if (mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_size, plain, used) == 0) {
            snprintf(auth, sizeof(auth), "AUTHENTICATE %.*s", (int)encoded_size, encoded);
            (void)irc_control(node, auth);
        } else irc_close(node, ESP_FAIL, "SASL encoding failed", true);
        irc_wipe(plain, sizeof(plain)); irc_wipe(encoded, sizeof(encoded)); irc_wipe(auth, sizeof(auth)); return;
    }
    if (strcmp(command, "903") == 0 && node->sasl_started && node->sasl_payload_sent) {
        node->sasl_ok = true; (void)irc_control(node, "CAP END"); return;
    }
    if (strcmp(command, "902") == 0 || strcmp(command, "904") == 0 ||
        strcmp(command, "905") == 0 || strcmp(command, "906") == 0) {
        char reason[SOLAR_OS_MESSAGING_ERROR_MAX];
        const char *detail = "authentication rejected";
        if (count > 1) {
            solar_os_irc_clean_text(p[count - 1]);
            if (p[count - 1][0]) detail = p[count - 1];
        }
        snprintf(reason, sizeof(reason), "IRC SASL %s: %s", command, detail);
        irc_close(node, ESP_FAIL, reason, true); return;
    }
    if (strcmp(command, "001") == 0 && count) {
        if (node->active.account[0] && !node->sasl_ok) { irc_close(node, ESP_FAIL, "IRC registered without required authentication", true); return; }
        strlcpy(node->nick, p[0], sizeof(node->nick)); node->registered = true; node->retry_delay = 1000;
        irc_status(node, ESP_OK, "connected; live messages only"); return;
    }
    if (strcmp(command, "005") == 0) {
        for (size_t i = 1; i < count; i++) {
            if (strncmp(p[i], "CASEMAPPING=", 12) == 0) {
                xSemaphoreTake(irc.lock, portMAX_DELAY);
                node->casemapping = strcmp(p[i] + 12, "ascii") == 0 ? SOLAR_OS_IRC_CASE_ASCII :
                    strcmp(p[i] + 12, "strict-rfc1459") == 0 ? SOLAR_OS_IRC_CASE_STRICT_RFC1459 : SOLAR_OS_IRC_CASE_RFC1459;
                xSemaphoreGive(irc.lock);
            }
            else if (strncmp(p[i], "CHANTYPES=", 10) == 0) strlcpy(node->chantypes, p[i] + 10, sizeof(node->chantypes));
        }
        return;
    }
    if (strcmp(command, "433") == 0 && !node->registered) {
        if (++node->nick_retries > 3) { irc_close(node, ESP_FAIL, "IRC nickname unavailable", true); return; }
        char nick[64]; snprintf(nick, sizeof(nick), "NICK %.*s_%u", 25, node->active.nick, node->nick_retries);
        (void)irc_control(node, nick); return;
    }
    if ((strcmp(command, "432") == 0 || strcmp(command, "464") == 0 || strcmp(command, "465") == 0)) {
        irc_close(node, ESP_FAIL, "IRC registration rejected", true); return;
    }
    if (strcmp(command, "NICK") == 0 && count && irc_equal(node, sender, node->nick)) {
        strlcpy(node->nick, p[0], sizeof(node->nick)); return;
    }
    if (!node->registered) return;
    char target[SOLAR_OS_IRC_TARGET_MAX];
    if ((strcmp(command, "JOIN") == 0 || strcmp(command, "PART") == 0) && count && irc_equal(node, sender, node->nick)) {
        if (!solar_os_irc_atom_valid(p[0], sizeof(target))) return;
        solar_os_irc_casefold(p[0], target, sizeof(target), node->casemapping);
        bool join = strcmp(command, "JOIN") == 0;
        if (join) {
            for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++)
                if (strcmp(node->joined[i], target) == 0) {
                    (void)solar_os_messaging_join_update(node->id, target, SOLAR_OS_MESSAGING_JOIN_JOINED, NULL); return;
                }
        }
        for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++) {
            if (!join && strcmp(node->joined[i], target) == 0) node->joined[i][0] = 0;
            if (join && !node->joined[i][0]) { strlcpy(node->joined[i], target, sizeof(node->joined[i])); break; }
        }
        (void)solar_os_messaging_join_update(node->id, target, join ? SOLAR_OS_MESSAGING_JOIN_JOINED : SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
        return;
    }
    if (strcmp(command, "KICK") == 0 && count >= 2 && irc_equal(node, p[1], node->nick)) {
        solar_os_irc_casefold(p[0], target, sizeof(target), node->casemapping);
        for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++)
            if (strcmp(node->joined[i], target) == 0) node->joined[i][0] = 0;
        (void)solar_os_messaging_join_update(node->id, target, SOLAR_OS_MESSAGING_JOIN_FAILED, "kicked from channel"); return;
    }
    if (count >= 2 && (strcmp(command, "403") == 0 || strcmp(command, "405") == 0 ||
        strcmp(command, "471") == 0 || strcmp(command, "473") == 0 || strcmp(command, "474") == 0 || strcmp(command, "475") == 0)) {
        solar_os_irc_casefold(p[1], target, sizeof(target), node->casemapping);
        (void)solar_os_messaging_join_update(node->id, target, SOLAR_OS_MESSAGING_JOIN_FAILED, "IRC server rejected channel join"); return;
    }
    if ((strcmp(command, "PRIVMSG") == 0 || strcmp(command, "NOTICE") == 0) && count >= 2 && sender[0]) {
        if (!solar_os_irc_atom_valid(sender, sizeof(sender)) || !solar_os_irc_atom_valid(p[0], sizeof(target))) return;
        bool room = strchr(node->chantypes, p[0][0]) != NULL;
        if (!room && !irc_equal(node, p[0], node->nick)) return;
        const char *name = room ? p[0] : sender;
        if (strlen(name) >= sizeof(target)) return;
        solar_os_irc_casefold(name, target, sizeof(target), node->casemapping);
        solar_os_irc_clean_text(p[1]);
        if (!p[1][0]) return;
        const solar_os_messaging_inbound_t inbound = {.provider = node->id,
            .provider_message_key = ++node->receive_sequence,
            .conversation_key = target, .conversation_title = name,
            .conversation_kind = room ? SOLAR_OS_CONVERSATION_ROOM : SOLAR_OS_CONVERSATION_DIRECT,
            .timestamp_ms = (uint64_t)time(NULL) * 1000,
            .security_flags = SOLAR_OS_SECURITY_TRANSPORT_SECURED | SOLAR_OS_SECURITY_SENDER_UNVERIFIED,
            .sender = sender, .body = p[1]};
        esp_err_t error = solar_os_messaging_publish_inbound(&inbound, NULL, NULL);
        if (error != ESP_OK) {
            irc_status(node, error, "message history storage failed");
            SOLAR_OS_LOGW("irc", "message history storage failed: %s", esp_err_to_name(error));
        }
    }
}

static void irc_read(irc_instance_t *node)
{
    char bytes[256];
    /* Per-instance work budget prevents a busy server starving other sessions. */
    for (unsigned pass = 0; pass < 4 && node->tls; pass++) {
        int count = esp_tls_conn_read(node->tls, bytes, sizeof(bytes));
        if (count == ESP_TLS_ERR_SSL_WANT_READ || count == ESP_TLS_ERR_SSL_WANT_WRITE) return;
        if (count <= 0) { irc_close(node, ESP_FAIL, "IRC connection lost", true); return; }
        node->last_rx = irc_now();
        for (int i = 0; i < count && node->tls; i++) {
            char c = bytes[i];
            if (c == '\n') {
                if (!node->dropping) {
                    if (node->rx_used && node->rx[node->rx_used - 1] == '\r') node->rx_used--;
                    node->rx[node->rx_used] = 0; irc_received(node, node->rx);
                }
                node->rx_used = 0; node->dropping = false;
            } else if (c == 0 || node->rx_used + 1 >= sizeof(node->rx)) node->dropping = true;
            else if (!node->dropping) node->rx[node->rx_used++] = c;
        }
    }
}

static bool irc_contains(char values[][SOLAR_OS_IRC_TARGET_MAX], const char *target)
{
    for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++) if (strcmp(values[i], target) == 0) return true;
    return false;
}

static void irc_prepare_tx(irc_instance_t *node, uint32_t now)
{
    if (node->tx_size) return;
    node->tx_throttled = false;
    if (node->pong[0]) { strlcpy(node->tx, node->pong, sizeof(node->tx)); node->pong[0] = 0; }
    else if (node->control_count) {
        strlcpy(node->tx, node->controls[node->control_head], sizeof(node->tx));
        irc_wipe(node->controls[node->control_head], sizeof(node->controls[0]));
        node->control_head = (node->control_head + 1) % IRC_CONTROL_CAPACITY; node->control_count--;
    } else if (node->registered && irc_due(now, node->next_tx)) {
        for (size_t i = 0; i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++) {
            const char *channel = node->joined[i];
            if (channel[0] && !irc_contains(node->active.channels, channel)) {
                snprintf(node->tx, sizeof(node->tx), "PART %s\r\n", channel);
                for (size_t j = 0; j < SOLAR_OS_IRC_CHANNEL_CAPACITY; j++)
                    if (strcmp(node->attempted[j], channel) == 0) node->attempted[j][0] = 0;
                node->joined[i][0] = 0;
                break;
            }
        }
        for (size_t i = 0; !node->tx[0] && i < SOLAR_OS_IRC_CHANNEL_CAPACITY; i++) {
            const char *channel = node->active.channels[i];
            if (channel[0] && !irc_contains(node->attempted, channel) && !irc_contains(node->joined, channel)) {
                for (size_t j = 0; j < SOLAR_OS_IRC_CHANNEL_CAPACITY; j++) {
                    if (node->attempted[j][0]) continue;
                    strlcpy(node->attempted[j], channel, sizeof(node->attempted[j]));
                    snprintf(node->tx, sizeof(node->tx), "JOIN %s\r\n", channel);
                    (void)solar_os_messaging_join_update(node->id, channel, SOLAR_OS_MESSAGING_JOIN_PENDING, NULL); break;
                }
            }
        }
        if (!node->tx[0] && solar_os_messaging_outbox_peek(node->id, &node->outbound) == ESP_OK) {
            bool room = strchr(node->chantypes, node->outbound.provider_key[0]) != NULL;
            if (room && !irc_contains(node->joined, node->outbound.provider_key)) {
                solar_os_messaging_conversation_t conversation;
                if (!irc_contains(node->active.channels, node->outbound.provider_key) ||
                    (solar_os_messaging_conversation_get(node->outbound.conversation_id, &conversation) == ESP_OK &&
                     conversation.join_state == SOLAR_OS_MESSAGING_JOIN_FAILED))
                    (void)solar_os_messaging_outbox_update(node->outbound.id, SOLAR_OS_DELIVERY_FAILED, "IRC channel is not joined");
                return;
            }
            size_t length = strlen(node->outbound.body);
            if (!solar_os_irc_target_valid(node->outbound.provider_key, SOLAR_OS_IRC_TARGET_MAX) ||
                strpbrk(node->outbound.body, "\r\n") || !length ||
                length + strlen(node->outbound.provider_key) + 12 >= sizeof(node->tx)) {
                (void)solar_os_messaging_outbox_update(node->outbound.id, SOLAR_OS_DELIVERY_FAILED, "message exceeds IRC line limit or contains a line break"); return;
            }
            snprintf(node->tx, sizeof(node->tx), "PRIVMSG %s :%s\r\n", node->outbound.provider_key, node->outbound.body);
            node->outbound_id = node->outbound.id;
            if (solar_os_messaging_outbox_update(node->outbound_id, SOLAR_OS_DELIVERY_SENDING, NULL) != ESP_OK) {
                /* Cancellation can win between taking the snapshot and
                 * claiming this request. Never transmit that stale copy. */
                node->outbound_id = 0; irc_wipe(node->tx, sizeof(node->tx)); return;
            }
        }
        node->tx_throttled = node->tx[0] != 0;
    }
    if (node->tx[0]) { node->tx_size = strlen(node->tx); node->tx_used = 0; node->tx_at = now; }
}

static void irc_write(irc_instance_t *node, uint32_t now)
{
    irc_prepare_tx(node, now);
    if (!node->tx_size) return;
    if ((uint32_t)(now - node->tx_at) > 5000) { irc_close(node, ESP_ERR_TIMEOUT, "IRC send stalled", true); return; }
    int written = esp_tls_conn_write(node->tls, node->tx + node->tx_used, node->tx_size - node->tx_used);
    if (written == ESP_TLS_ERR_SSL_WANT_READ || written == ESP_TLS_ERR_SSL_WANT_WRITE) return;
    if (written <= 0) { irc_close(node, ESP_FAIL, "IRC send interrupted", true); return; }
    node->tx_used += written;
    if (node->tx_used == node->tx_size) {
        /* Profile/outbox persistence and partial TLS writes can delay a send.
         * Pace from the completed write so that delay cannot shorten the next
         * interval. Keep registration and keepalive controls independent. */
        if (node->tx_throttled) node->next_tx = irc_now() + IRC_TX_INTERVAL_MS;
        node->tx_throttled = false;
        if (node->outbound_id) {
            (void)solar_os_messaging_outbox_update(node->outbound_id, SOLAR_OS_DELIVERY_SENT, NULL);
            node->outbound_id = 0;
        }
        irc_wipe(node->tx, sizeof(node->tx)); node->tx_size = node->tx_used = 0;
    }
}

static void irc_tick(irc_instance_t *node)
{
    uint32_t now = irc_now();
    solar_os_irc_config_t next;
    xSemaphoreTake(irc.lock, portMAX_DELAY); next = node->config;
    uint32_t revision = node->revision; xSemaphoreGive(irc.lock);
    if (revision != node->active_revision) {
        memset(node->attempted, 0, sizeof(node->attempted)); node->active_revision = revision;
    }
    bool connection_changed = memcmp(&next, &node->active, offsetof(solar_os_irc_config_t, channels)) != 0;
    if (connection_changed) {
        irc_close(node, ESP_OK, "configuration changed", false);
        node->retry_at = 0; node->retry_delay = 1000;
    }
    node->active = next; irc_wipe(&next, sizeof(next));
    if (!node->active.enabled) { irc_status(node, ESP_OK, "disabled"); return; }
    if (!node->tls) {
        if (node->retry_at && !irc_due(now, node->retry_at)) return;
        node->tls = esp_tls_init();
        if (!node->tls) { irc_close(node, ESP_ERR_NO_MEM, "TLS allocation failed", true); return; }
        memset(&node->tls_config, 0, sizeof(node->tls_config));
        node->tls_config.non_block = true; node->tls_config.timeout_ms = IRC_CONNECT_TIMEOUT_MS;
        node->tls_config.crt_bundle_attach = esp_crt_bundle_attach;
        if (node->active.ca_file[0]) {
            FILE *file = fopen(node->active.ca_file, "rb");
            node->ca = solar_os_memory_calloc(1, 8192, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "irc.ca");
            size_t count = file && node->ca ? fread(node->ca, 1, 8191, file) : 0;
            bool ok = file && node->ca && count > 0 && fgetc(file) == EOF;
            if (file) fclose(file);
            if (!ok) { irc_close(node, ESP_FAIL, "IRC CA file unavailable or too large", true); return; }
            node->tls_config.crt_bundle_attach = NULL;
            node->tls_config.cacert_buf = (const unsigned char *)node->ca;
            node->tls_config.cacert_bytes = count + 1;
        }
        node->connect_at = now;
        strlcpy(node->chantypes, "#&", sizeof(node->chantypes));
        strlcpy(node->nick, node->active.nick, sizeof(node->nick));
        irc_status(node, ESP_OK, "connecting with TLS verification");
    }
    if (!node->tls_ready) {
        if ((uint32_t)(now - node->connect_at) > IRC_CONNECT_TIMEOUT_MS) {
            irc_close(node, ESP_ERR_TIMEOUT, "IRC TLS connection timed out", true); return;
        }
        int result = esp_tls_conn_new_async(node->active.host, strlen(node->active.host),
            node->active.port, &node->tls_config, node->tls);
        if (result < 0) { irc_close(node, ESP_FAIL, "IRC TLS connection failed", true); return; }
        if (result == 0) return;
        node->tls_ready = true; node->last_rx = now;
        /* IRC has no replay IDs in this client. Give every live line a fresh
         * identity, including repeated text within the same clock tick. */
        node->receive_sequence = ((uint64_t)esp_random() << 32) | esp_random();
        char command[256];
        if (node->active.server_password[0]) { snprintf(command, sizeof(command), "PASS :%s", node->active.server_password); (void)irc_control(node, command); }
        if (node->active.account[0]) (void)irc_control(node, "CAP LS 302");
        snprintf(command, sizeof(command), "NICK %s", node->active.nick); (void)irc_control(node, command);
        snprintf(command, sizeof(command), "USER %s 0 * :SolarOS", node->active.nick); (void)irc_control(node, command);
        irc_wipe(command, sizeof(command));
    }
    irc_read(node);
    if (!node->tls) return;
    /* Receiving a line can persist messages and advance last_rx. Use a fresh
     * timestamp so unsigned elapsed-time checks cannot see it in the future. */
    now = irc_now();
    if (!node->registered && (uint32_t)(now - node->connect_at) > 45000) {
        irc_close(node, ESP_ERR_TIMEOUT, "IRC registration timed out", true); return;
    }
    if (node->ping_pending && (uint32_t)(now - node->ping_at) > IRC_DEAD_TIMEOUT_MS) {
        irc_close(node, ESP_ERR_TIMEOUT, "IRC keepalive timed out", true); return;
    }
    if (!node->ping_pending && (uint32_t)(now - node->last_rx) > IRC_IDLE_PING_MS) {
        if (!irc_control(node, "PING :solaros")) return;
        node->ping_pending = true; node->ping_at = now;
    }
    irc_write(node, now);
}

static void irc_worker(void *argument)
{
    (void)argument;
    while (!irc.stop_requested) {
        xSemaphoreTake(irc.worker_lock, portMAX_DELAY);
        xSemaphoreTake(irc.lock, portMAX_DELAY); irc_instance_t *node = irc.instances; xSemaphoreGive(irc.lock);
        for (; node && !irc.stop_requested; node = node->next) irc_tick(node);
        xSemaphoreGive(irc.worker_lock);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
    }
    xSemaphoreTake(irc.worker_lock, portMAX_DELAY);
    xSemaphoreTake(irc.lock, portMAX_DELAY); irc_instance_t *node = irc.instances; xSemaphoreGive(irc.lock);
    irc.running = false;
    for (; node; node = node->next) {
        irc_close(node, ESP_OK, "stopped", false);
        irc_wipe(&node->active, sizeof(node->active));
    }
    xSemaphoreGive(irc.worker_lock);
    irc.worker_done = true; solar_os_task_delete(NULL);
}

esp_err_t solar_os_irc_start(void)
{
    esp_err_t error = solar_os_irc_init(); if (error != ESP_OK) return error;
    /* Socket use must also be safe when Wi-Fi was disabled for this boot.
     * esp_netif_init is idempotent and does not enable a network interface. */
    error = esp_netif_init(); if (error != ESP_OK) return error;
    if (irc.running || (irc.worker && !irc.worker_done)) return ESP_ERR_INVALID_STATE;
    irc.stop_requested = false; irc.worker_done = false; irc.running = true;
    if (solar_os_task_create_pinned(irc_worker, "irc_sync", 8192, NULL,
        tskIDLE_PRIORITY + 1, &irc.worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        irc.running = false; irc.worker = NULL; return ESP_ERR_NO_MEM;
    }
    (void)solar_os_jobs_note_resource("irc-sync", SOLAR_OS_JOB_RESOURCE_NET, "IRC TLS", "configured messaging instances");
    return ESP_OK;
}

void solar_os_irc_stop(void)
{
    irc.stop_requested = true;
    if (irc.worker && !irc.worker_done) xTaskNotifyGive(irc.worker);
    if (solar_os_task_wait_done(irc.worker, &irc.worker_done, 5000)) irc.worker = NULL;
    else SOLAR_OS_LOGW("irc", "IRC worker is still stopping");
}
