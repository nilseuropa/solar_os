#include "solar_os_meshcore_instances.h"
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_config.h"
#include "solar_os_contacts.h"
#include "solar_os_memory.h"

typedef struct meshcore_instance {
    struct meshcore_instance *next;
    solar_os_messaging_provider_id_t id;
    char name[SOLAR_OS_MESSAGING_LABEL_MAX];
    solar_os_meshcore_instance_config_t config;
    SemaphoreHandle_t config_lock;
    void *state[3];
    atomic_bool active;
    uint32_t retry_at, retry_delay;
} meshcore_instance_t;

static struct {
    SemaphoreHandle_t edit, workers[3];
    meshcore_instance_t *instances;
    const solar_os_meshcore_backend_t *backends[3];
} meshcore_instances;
static _Thread_local meshcore_instance_t *selected_instance;
static atomic_flag init_lock = ATOMIC_FLAG_INIT, state_lock = ATOMIC_FLAG_INIT;
static atomic_bool ready;

void solar_os_meshcore_backend_register(unsigned kind, const solar_os_meshcore_backend_t *backend)
{ if (kind > 0 && kind < 3) meshcore_instances.backends[kind] = backend; }

static void workers_lock(void)
{
    xSemaphoreTake(meshcore_instances.workers[1], portMAX_DELAY);
    xSemaphoreTake(meshcore_instances.workers[2], portMAX_DELAY);
}
static void workers_unlock(void)
{
    xSemaphoreGive(meshcore_instances.workers[2]);
    xSemaphoreGive(meshcore_instances.workers[1]);
}
static void wipe(void *data, size_t size)
{ volatile unsigned char *bytes = data; while (size--) *bytes++ = 0; }

solar_os_messaging_provider_id_t solar_os_meshcore_current_id(void)
{ return selected_instance ? selected_instance->id : SOLAR_OS_MESSAGING_PROVIDER_MESHCORE; }

void *solar_os_meshcore_current_state(unsigned kind, size_t size)
{
    if (!selected_instance || kind < 1 || kind > 2) return NULL;
    if (selected_instance->state[kind] || !size) return selected_instance->state[kind];
    while (atomic_flag_test_and_set(&state_lock)) vTaskDelay(1);
    if (!selected_instance->state[kind] && size)
        selected_instance->state[kind] = solar_os_memory_calloc(1, size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "meshcore.state");
    void *state = selected_instance->state[kind];
    atomic_flag_clear(&state_lock); return state;
}
void solar_os_meshcore_current_state_free(unsigned kind)
{
    if (!selected_instance || kind < 1 || kind > 2) return;
    solar_os_memory_free(selected_instance->state[kind]); selected_instance->state[kind] = NULL;
}

esp_err_t solar_os_meshcore_current_config(solar_os_meshcore_instance_config_t *config)
{
    if (!config || !selected_instance) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(selected_instance->config_lock, portMAX_DELAY);
    *config = selected_instance->config;
    xSemaphoreGive(selected_instance->config_lock); return ESP_OK;
}

static esp_err_t save_config(meshcore_instance_t *node, const solar_os_meshcore_instance_config_t *config)
{
    esp_err_t error = solar_os_messaging_profile_save(node->id, config, sizeof(*config));
    if (error == ESP_OK) node->config = *config;
    return error;
}

static esp_err_t save_local_setting(const char *name, int public_channel)
{
    meshcore_instance_t *node = selected_instance;
    if (!node) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(node->config_lock, portMAX_DELAY);
    solar_os_meshcore_instance_config_t config = node->config;
    esp_err_t error = ESP_OK;
    if (name && strlcpy(config.name, name, sizeof(config.name)) >= sizeof(config.name)) error = ESP_ERR_INVALID_SIZE;
    if (public_channel >= 0) config.public_channel = public_channel != 0;
    if (error == ESP_OK) error = save_config(node, &config);
    wipe(&config, sizeof(config)); xSemaphoreGive(node->config_lock); return error;
}
esp_err_t solar_os_meshcore_current_name_set(const char *name) { return save_local_setting(name, -1); }
esp_err_t solar_os_meshcore_current_public_set(bool enabled) { return save_local_setting(NULL, enabled); }

static bool text_valid(const char *text, size_t size)
{
    if (!text || !text[0] || strnlen(text, size) >= size) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) if (*p <= 32 || *p == 127) return false;
    return true;
}

static bool config_valid(const solar_os_meshcore_instance_config_t *config)
{
    if (config->version != 1 || config->backend > 2 || !memchr(config->name, 0, sizeof(config->name))) return false;
    if (config->backend == 1) return text_valid(config->radio, sizeof(config->radio)) && text_valid(config->profile, sizeof(config->profile));
    if (config->backend == 2) {
        unsigned any = 0; for (size_t i = 0; i < 6; i++) any |= config->bda[i];
        return any && config->addr_type <= 3 && (!config->pin_supplied || config->pin <= 999999);
    }
    return !config->enabled;
}

static esp_err_t parse_config(int argc, const char *const *argv, solar_os_meshcore_instance_config_t *config)
{
    if (argc < 3) return ESP_ERR_INVALID_ARG;
    if (!strcmp(argv[0], "radio") && argc == 3) {
        config->backend = 1;
        if (strlcpy(config->radio, argv[1], sizeof(config->radio)) >= sizeof(config->radio) ||
            strlcpy(config->profile, argv[2], sizeof(config->profile)) >= sizeof(config->profile)) return ESP_ERR_INVALID_SIZE;
    } else if (!strcmp(argv[0], "ble") && (argc == 3 || argc == 4)) {
        config->backend = 2;
        unsigned bytes[6]; int used = 0;
        if (sscanf(argv[1], "%2x:%2x:%2x:%2x:%2x:%2x%n", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5], &used) != 6 || argv[1][used]) return ESP_ERR_INVALID_ARG;
        for (size_t i = 0; i < 6; i++) config->bda[i] = bytes[i];
        static const char *const types[] = {"public", "random", "rpa_public", "rpa_random"};
        unsigned type; for (type = 0; type < 4 && strcmp(argv[2], types[type]); type++) {}
        if (type == 4) return ESP_ERR_INVALID_ARG;
        config->addr_type = type; config->pin_supplied = argc == 4; config->pin = 0;
        if (argc == 4) {
            if (strlen(argv[3]) != 6) return ESP_ERR_INVALID_ARG;
            for (size_t i = 0; i < 6; i++) {
                if (argv[3][i] < '0' || argv[3][i] > '9') return ESP_ERR_INVALID_ARG;
                config->pin = config->pin * 10 + argv[3][i] - '0';
            }
        }
    } else return ESP_ERR_INVALID_ARG;
    if (!meshcore_instances.backends[config->backend]) return ESP_ERR_NOT_SUPPORTED;
    return config_valid(config) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static bool same_binding(const solar_os_meshcore_instance_config_t *a, const solar_os_meshcore_instance_config_t *b)
{
    return a->backend && a->backend == b->backend && (a->backend == 1 ? !strcmp(a->radio, b->radio) :
        a->addr_type == b->addr_type && !memcmp(a->bda, b->bda, 6));
}
static bool binding_busy(meshcore_instance_t *except, const solar_os_meshcore_instance_config_t *config)
{
    for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next)
        if (node != except && same_binding(&node->config, config)) return true;
    return false;
}

static void stop_instance(meshcore_instance_t *node)
{
    const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[node->config.backend];
    meshcore_instance_t *previous = selected_instance; selected_instance = node;
    if (ops && node->state[node->config.backend]) ops->stop();
    node->active = false; node->retry_at = 0; node->retry_delay = 1000;
    selected_instance = previous;
}
void solar_os_meshcore_current_pause(void)
{ if (selected_instance) stop_instance(selected_instance); }

static esp_err_t connect_instance(void *context, bool enabled)
{
    meshcore_instance_t *node = context;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    xSemaphoreTake(node->config_lock, portMAX_DELAY);
    solar_os_meshcore_instance_config_t config = node->config; config.enabled = enabled;
    esp_err_t error = config.backend ? save_config(node, &config) : ESP_ERR_INVALID_STATE;
    wipe(&config, sizeof(config)); xSemaphoreGive(node->config_lock);
    if (error == ESP_OK && !enabled) stop_instance(node);
    workers_unlock(); xSemaphoreGive(meshcore_instances.edit); return error;
}

static esp_err_t configure_instance(void *context, int argc, const char *const *argv)
{
    meshcore_instance_t *node = context;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    esp_err_t error;
    if (argc && (!strcmp(argv[0], "radio") || !strcmp(argv[0], "ble"))) {
        xSemaphoreTake(node->config_lock, portMAX_DELAY);
        solar_os_meshcore_instance_config_t config = node->config;
        error = parse_config(argc, argv, &config);
        if (error == ESP_OK && binding_busy(node, &config)) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK && node->config.backend && config.backend != node->config.backend) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) error = save_config(node, &config);
        wipe(&config, sizeof(config)); xSemaphoreGive(node->config_lock);
        if (error == ESP_OK) stop_instance(node);
    } else {
        const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[node->config.backend];
        meshcore_instance_t *previous = selected_instance; selected_instance = node;
        if (ops && ops->configure) { stop_instance(node); error = ops->configure(argc, argv); }
        else error = ESP_ERR_NOT_SUPPORTED;
        selected_instance = previous;
    }
    workers_unlock(); xSemaphoreGive(meshcore_instances.edit); return error;
}

static esp_err_t join_instance(void *context, const char *target, solar_os_conversation_id_t *conversation)
{
    meshcore_instance_t *node = context;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[node->config.backend];
    meshcore_instance_t *previous = selected_instance; selected_instance = node;
    esp_err_t error = ops && ops->join ? ops->join(target, conversation) : ESP_ERR_NOT_SUPPORTED;
    selected_instance = previous;
    workers_unlock(); xSemaphoreGive(meshcore_instances.edit); return error;
}
static esp_err_t leave_instance(void *context, const char *target)
{
    meshcore_instance_t *node = context;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[node->config.backend];
    meshcore_instance_t *previous = selected_instance; selected_instance = node;
    esp_err_t error = ESP_ERR_NOT_SUPPORTED;
    if (ops && ops->leave) { stop_instance(node); error = ops->leave(target); }
    selected_instance = previous;
    workers_unlock(); xSemaphoreGive(meshcore_instances.edit); return error;
}

static esp_err_t direct_instance(void *context, const char *target, solar_os_conversation_id_t *conversation)
{
    meshcore_instance_t *node = context;
    if (strlen(target) != 64) return ESP_ERR_INVALID_ARG;
    uint8_t key[32];
    for (size_t i = 0; i < sizeof(key); i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned char c = target[i * 2 + j];
            if (c >= 'A' && c <= 'F') c += 'a' - 'A';
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return ESP_ERR_INVALID_ARG;
            value = value * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
        }
        key[i] = value;
    }
    solar_os_contact_id_t contact; solar_os_endpoint_id_t endpoint_id;
    char title[SOLAR_OS_CONTACT_NAME_MAX + 1]; snprintf(title, sizeof(title), "MeshCore %.12s", target);
    esp_err_t error = solar_os_contacts_upsert_discovered(node->id, key, sizeof(key), title,
        SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_ACK, 0, NULL, 0, &contact, &endpoint_id);
    solar_os_endpoint_t endpoint;
    if (error == ESP_OK) error = solar_os_contacts_get_endpoint(endpoint_id, &endpoint);
    if (error != ESP_OK) return error;
    if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) return ESP_ERR_INVALID_STATE;
    char address[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    snprintf(address, sizeof(address), "direct:%" PRIu32, endpoint_id);
    solar_os_messaging_conversation_upsert_t request = {.provider = node->id, .provider_key = address,
        .kind = SOLAR_OS_CONVERSATION_DIRECT, .title = title, .contact_id = contact, .endpoint_id = endpoint_id,
        .security_flags = SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_PEER_KEY_KNOWN |
            (endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED ? SOLAR_OS_SECURITY_PEER_TRUSTED : 0)};
    return solar_os_messaging_conversation_upsert(&request, conversation);
}

static esp_err_t attach_instance(solar_os_messaging_provider_id_t id, const char *name,
    const solar_os_meshcore_instance_config_t *config)
{
    for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next) if (node->id == id) return ESP_OK;
    meshcore_instance_t *node = solar_os_memory_calloc(1, sizeof(*node), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "meshcore.instance");
    if (!node) return ESP_ERR_NO_MEM;
    node->id = id; node->config = *config; node->retry_delay = 1000;
    strlcpy(node->name, name, sizeof(node->name)); node->config_lock = xSemaphoreCreateMutex();
    esp_err_t error = node->config_lock ? solar_os_messaging_instance_register(id, "meshcore", name, node) : ESP_ERR_NO_MEM;
    if (error != ESP_OK) { if (node->config_lock) vSemaphoreDelete(node->config_lock); wipe(node, sizeof(*node)); solar_os_memory_free(node); return error; }
    workers_lock(); node->next = meshcore_instances.instances; meshcore_instances.instances = node; workers_unlock(); return ESP_OK;
}

static esp_err_t create_locked(const char *name, const solar_os_meshcore_instance_config_t *config, solar_os_messaging_provider_id_t *id)
{
    if (binding_busy(NULL, config)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = solar_os_messaging_instance_create("meshcore", name, config, sizeof(*config), id);
    if (error == ESP_OK) error = attach_instance(*id, name, config);
    if (error != ESP_OK) atomic_store(&ready, false);
    return error;
}
static esp_err_t create_instance(const char *name, int argc, const char *const *argv, solar_os_messaging_provider_id_t *id)
{
    solar_os_meshcore_instance_config_t config = {.version = 1, .public_channel = true};
    esp_err_t error = parse_config(argc, argv, &config);
    if (error == ESP_OK) {
        xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY);
        error = create_locked(name, &config, id);
        xSemaphoreGive(meshcore_instances.edit);
    }
    wipe(&config, sizeof(config)); return error;
}

static esp_err_t remove_prepare(void *context)
{
    meshcore_instance_t *node = context;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    xSemaphoreTake(node->config_lock, portMAX_DELAY);
    const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[node->config.backend];
    meshcore_instance_t *previous = selected_instance; selected_instance = node;
    esp_err_t error = ops && ops->remove_prepare ? ops->remove_prepare() : ESP_OK;
    selected_instance = previous;
    if (error != ESP_OK) { xSemaphoreGive(node->config_lock); workers_unlock(); xSemaphoreGive(meshcore_instances.edit); }
    return error;
}
static void remove_finish(void *context, bool removed)
{
    meshcore_instance_t *node = context;
    if (removed) {
        /* Backend stop can read its config. The worker/edit locks still exclude
         * all callers while its private config mutex is released. */
        xSemaphoreGive(node->config_lock);
        meshcore_instance_t *previous = selected_instance; selected_instance = node;
        stop_instance(node);
        for (unsigned kind = 1; kind <= 2; kind++)
            if (node->state[kind] && meshcore_instances.backends[kind]) meshcore_instances.backends[kind]->destroy();
        selected_instance = previous;
        meshcore_instance_t **next = &meshcore_instances.instances;
        while (*next && *next != node) next = &(*next)->next;
        if (*next) *next = node->next;
        vSemaphoreDelete(node->config_lock); wipe(node, sizeof(*node)); solar_os_memory_free(node);
    } else xSemaphoreGive(node->config_lock);
    workers_unlock(); xSemaphoreGive(meshcore_instances.edit);
}

static const solar_os_messaging_setting_t meshcore_radio_fields[] = {
    {"Radio", "Radio device name", 24, false, true},
    {"Profile", "Radio profile name", 32, false, true},
    {"Node name", "Advertised node name", 33, false, false},
    {"Public channel", "on or off", 4, false, true},
};
static const solar_os_messaging_setting_t meshcore_ble_fields[] = {
    {"Address", "AA:BB:CC:DD:EE:FF", 18, false, true},
    {"Address type", "public/random/rpa_public/rpa_random", 12, false, true},
    {"PIN", "Six digits; empty clears PIN", 7, true, false},
    {"Node name", "Advertised node name", 33, false, false},
    {"Public channel", "on or off", 4, false, true},
};
static unsigned meshcore_settings_backend(meshcore_instance_t *node, unsigned form)
{
    if (node && node->config.backend) return form ? 0 : node->config.backend;
    for (unsigned backend = 1; backend <= 2; backend++)
        if (meshcore_instances.backends[backend] && form-- == 0) return backend;
    return 0;
}
static esp_err_t meshcore_settings_read(void *context, unsigned form, solar_os_messaging_settings_t *settings)
{
    meshcore_instance_t *node = context;
    if (node) xSemaphoreTake(node->config_lock, portMAX_DELAY);
    unsigned backend = meshcore_settings_backend(node, form);
    if (!backend) { if (node) xSemaphoreGive(node->config_lock); return ESP_ERR_NOT_FOUND; }
    settings->label = backend == 1 ? "MeshCore radio" : "MeshCore BLE";
    settings->fields = backend == 1 ? meshcore_radio_fields : meshcore_ble_fields;
    settings->count = backend == 1 ? 4 : 5;
    strlcpy(settings->worker, backend == 1 ? "meshcore" : "meshcore-ble", sizeof(settings->worker));
    if (backend == 2) strlcpy(settings->values[1], "public", sizeof(settings->values[1]));
    strlcpy(settings->values[settings->count - 1], "on", sizeof(settings->values[0]));
    if (node) {
        const solar_os_meshcore_instance_config_t *config = &node->config;
        settings->enabled = config->enabled;
        if (backend == 1) {
            solar_os_messaging_settings_value(settings, 0, config->radio);
            solar_os_messaging_settings_value(settings, 1, config->profile);
        } else if (config->backend == 2) {
            snprintf(settings->values[0], sizeof(settings->values[0]), "%02X:%02X:%02X:%02X:%02X:%02X",
                config->bda[0], config->bda[1], config->bda[2], config->bda[3], config->bda[4], config->bda[5]);
            static const char *const types[] = {"public", "random", "rpa_public", "rpa_random"};
            strlcpy(settings->values[1], types[config->addr_type], sizeof(settings->values[1]));
            settings->present[2] = config->pin_supplied;
        }
        solar_os_messaging_settings_value(settings, settings->count - 2, config->name);
        strlcpy(settings->values[settings->count - 1], config->public_channel ? "on" : "off", sizeof(settings->values[0]));
        xSemaphoreGive(node->config_lock);
    }
    return ESP_OK;
}
static esp_err_t meshcore_settings_write(void *context, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings, solar_os_messaging_provider_id_t *id)
{
    meshcore_instance_t *node = context;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    if (node) xSemaphoreTake(node->config_lock, portMAX_DELAY);
    unsigned backend = meshcore_settings_backend(node, form);
    solar_os_meshcore_instance_config_t config = {.version = 1, .public_channel = true};
    if (node) config = node->config;
    char address[18], pin[7], radio[24], profile[32];
    static const char *const types[] = {"public", "random", "rpa_public", "rpa_random"};
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
        config.bda[0], config.bda[1], config.bda[2], config.bda[3], config.bda[4], config.bda[5]);
    snprintf(pin, sizeof(pin), "%06" PRIu32, config.pin);
    strlcpy(radio, config.radio, sizeof(radio)); strlcpy(profile, config.profile, sizeof(profile));
    const char *args[4] = {backend == 1 ? "radio" : "ble", NULL, NULL, NULL};
    int argc = 3;
    if (backend == 1) {
        args[1] = !node || settings->changed[0] ? settings->values[0] : radio;
        args[2] = !node || settings->changed[1] ? settings->values[1] : profile;
    } else {
        args[1] = !node || settings->changed[0] ? settings->values[0] : address;
        args[2] = !node || settings->changed[1] ? settings->values[1] : types[config.addr_type];
        args[3] = !node || settings->changed[2] ? settings->values[2] : config.pin_supplied ? pin : "";
        if (args[3][0]) argc = 4;
    }
    size_t count = backend == 1 ? 4 : 5;
    esp_err_t error = backend ? parse_config(argc, args, &config) : ESP_ERR_NOT_FOUND;
    if (error == ESP_OK && (!node || settings->changed[count - 2]))
        strlcpy(config.name, settings->values[count - 2], sizeof(config.name));
    if (error == ESP_OK && (!node || settings->changed[count - 1])) {
        if (!strcmp(settings->values[count - 1], "on")) config.public_channel = true;
        else if (!strcmp(settings->values[count - 1], "off")) config.public_channel = false;
        else error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK && binding_busy(node, &config)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && node) { *id = node->id; error = save_config(node, &config); }
    else if (error == ESP_OK) error = create_locked(name, &config, id);
    wipe(pin, sizeof(pin)); wipe(&config, sizeof(config));
    if (node) { xSemaphoreGive(node->config_lock); if (error == ESP_OK) stop_instance(node); }
    workers_unlock(); xSemaphoreGive(meshcore_instances.edit); return error;
}
static const solar_os_messaging_provider_class_t meshcore_class = {
    .name = "meshcore", .capabilities = SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_GROUP | SOLAR_OS_ENDPOINT_CAP_ACK,
    .create = create_instance, .configure = configure_instance, .connect = connect_instance,
    .join_open = join_instance, .leave = leave_instance, .direct_open = direct_instance,
    .remove_prepare = remove_prepare, .remove_finish = remove_finish,
    .settings_read = meshcore_settings_read, .settings_write = meshcore_settings_write,
};

static esp_err_t migrate_default(solar_os_meshcore_instance_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("meshcore", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    size_t length = sizeof(config->name); uint8_t public_channel = 1;
    error = nvs_get_str(handle, "name", config->name, &length);
    esp_err_t public_error = nvs_get_u8(handle, "public", &public_channel);
    config->public_channel = public_channel != 0; nvs_close(handle);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) return error;
    return public_error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : public_error;
}
static void erase_default_legacy(void)
{
    nvs_handle_t handle;
    if (nvs_open("meshcore", NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = 0; uint8_t value;
    bool present = nvs_get_str(handle, "name", NULL, &length) == ESP_OK;
    present |= nvs_get_u8(handle, "public", &value) == ESP_OK;
    nvs_close(handle);
    if (!present) return;
    if (nvs_open("meshcore", NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_erase_key(handle, "name"); (void)nvs_erase_key(handle, "public");
    (void)nvs_commit(handle); nvs_close(handle);
}

esp_err_t solar_os_meshcore_instances_init(void)
{
    if (atomic_load(&ready)) return ESP_OK;
    while (atomic_flag_test_and_set(&init_lock)) vTaskDelay(1);
    esp_err_t error = solar_os_messaging_init();
#if SOLAR_OS_PACKAGE_SERVICE_MESHCORE
    extern void solar_os_meshcore_install_backend(void); solar_os_meshcore_install_backend();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_MESHCORE_BLE
    extern void solar_os_meshcore_ble_install_backend(void); solar_os_meshcore_ble_install_backend();
#endif
    if (!meshcore_instances.edit) meshcore_instances.edit = xSemaphoreCreateMutex();
    for (unsigned kind = 1; kind <= 2; kind++) if (!meshcore_instances.workers[kind]) meshcore_instances.workers[kind] = xSemaphoreCreateMutex();
    if (!meshcore_instances.edit || !meshcore_instances.workers[1] || !meshcore_instances.workers[2]) error = ESP_ERR_NO_MEM;
    if (error == ESP_OK) error = solar_os_messaging_class_register(&meshcore_class);
    if (error == ESP_OK) {
        xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY);
        solar_os_messaging_provider_id_t cursor = 0; solar_os_messaging_provider_status_t status;
        while (solar_os_messaging_provider_next(&cursor, &status)) {
            if (strcmp(status.class_name, "meshcore")) continue;
            solar_os_meshcore_instance_config_t config = {.version = 1, .public_channel = true};
            error = solar_os_messaging_profile_load(cursor, &config, sizeof(config));
            if (error == ESP_ERR_NOT_FOUND && cursor == SOLAR_OS_MESSAGING_PROVIDER_MESHCORE) {
                error = migrate_default(&config);
                if (error == ESP_OK && solar_os_messaging_profile_save(cursor, &config, sizeof(config)) == ESP_OK) erase_default_legacy();
            }
            if (error == ESP_OK && !config_valid(&config)) error = ESP_ERR_INVALID_ARG;
            if (error == ESP_OK && cursor == SOLAR_OS_MESSAGING_PROVIDER_MESHCORE) {
                solar_os_meshcore_instance_config_t checked;
                if (solar_os_messaging_profile_load(cursor, &checked, sizeof(checked)) == ESP_OK) erase_default_legacy();
                wipe(&checked, sizeof(checked));
            }
            if (error == ESP_OK) error = attach_instance(cursor, status.name, &config);
            wipe(&config, sizeof(config)); if (error != ESP_OK) break;
        }
        atomic_store(&ready, error == ESP_OK); xSemaphoreGive(meshcore_instances.edit);
    }
    atomic_flag_clear(&init_lock); return error;
}

esp_err_t solar_os_meshcore_legacy_begin(unsigned kind, void **lease)
{
    if (!lease) return ESP_ERR_INVALID_ARG;
    *lease = NULL;
    if (kind < SOLAR_OS_MESHCORE_BACKEND_RADIO || kind > SOLAR_OS_MESHCORE_BACKEND_BLE) return ESP_ERR_INVALID_ARG;
    if (selected_instance) return ESP_OK;
    esp_err_t error = solar_os_meshcore_instances_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY); workers_lock();
    meshcore_instance_t *node = meshcore_instances.instances;
    while (node && node->id != SOLAR_OS_MESSAGING_PROVIDER_MESHCORE) node = node->next;
    if (!node || (node->config.backend && node->config.backend != kind)) {
        node = NULL;
        for (meshcore_instance_t *candidate = meshcore_instances.instances; candidate; candidate = candidate->next) {
            if (candidate->config.backend != kind) continue;
            if (node) {
                workers_unlock(); xSemaphoreGive(meshcore_instances.edit);
                return ESP_ERR_INVALID_STATE;
            }
            node = candidate;
        }
    }
    if (!node) { workers_unlock(); xSemaphoreGive(meshcore_instances.edit); return ESP_ERR_NOT_FOUND; }
    selected_instance = node; *lease = node; return ESP_OK;
}
void solar_os_meshcore_legacy_end(void *lease)
{
    if (!lease) return;
    selected_instance = NULL; workers_unlock(); xSemaphoreGive(meshcore_instances.edit);
}

esp_err_t solar_os_meshcore_instances_bind(unsigned kind, int argc, const char *const *argv)
{
    esp_err_t error = solar_os_meshcore_instances_init(); if (error != ESP_OK) return error;
    const char *args[5]; args[0] = kind == 1 ? "radio" : "ble";
    if (argc < 2 || argc > 3) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < argc; i++) args[i + 1] = argv[i];
    solar_os_meshcore_instance_config_t config = {.version = 1, .enabled = true, .public_channel = true};
    error = parse_config(argc + 1, args, &config); if (error != ESP_OK) { wipe(&config, sizeof(config)); return error; }
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY);
    meshcore_instance_t *found = NULL;
    for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next)
        if (same_binding(&node->config, &config)) { found = node; break; }
    if (!found) for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next)
        if (!node->config.backend) { found = node; break; }
    if (found) {
        workers_lock(); xSemaphoreTake(found->config_lock, portMAX_DELAY);
        config.public_channel = found->config.public_channel; strlcpy(config.name, found->config.name, sizeof(config.name));
        error = save_config(found, &config); xSemaphoreGive(found->config_lock);
        if (error == ESP_OK) stop_instance(found);
        workers_unlock();
    } else {
        char name[SOLAR_OS_MESSAGING_LABEL_MAX];
        if (kind == 1) snprintf(name, sizeof(name), "meshcore-%s", config.radio);
        else snprintf(name, sizeof(name), "meshcore-%02x%02x%02x%02x%02x%02x", config.bda[0], config.bda[1], config.bda[2], config.bda[3], config.bda[4], config.bda[5]);
        solar_os_messaging_provider_id_t id; error = create_locked(name, &config, &id);
    }
    xSemaphoreGive(meshcore_instances.edit); wipe(&config, sizeof(config)); return error;
}

void solar_os_meshcore_instances_poll(unsigned kind)
{
    if (kind < 1 || kind > 2 || solar_os_meshcore_instances_init() != ESP_OK) return;
    const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[kind]; if (!ops) return;
    xSemaphoreTake(meshcore_instances.workers[kind], portMAX_DELAY);
    for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next) {
        if (node->config.backend != kind || !node->config.enabled) continue;
        meshcore_instance_t *previous = selected_instance; selected_instance = node;
        uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
        if (!node->active && (!node->retry_at || (int32_t)(now - node->retry_at) >= 0)) {
            char owner[40]; snprintf(owner, sizeof(owner), "meshcore:%" PRIu32, node->id);
            esp_err_t error = ops->start(&node->config, owner);
            if (error == ESP_OK) { node->active = true; node->retry_delay = 1000; }
            else {
                node->retry_at = now + node->retry_delay;
                if (node->retry_delay < 30000) node->retry_delay *= 2;
                if (node->retry_delay > 30000) node->retry_delay = 30000;
                (void)solar_os_messaging_provider_set_status(node->id, false, false, error, "MeshCore binding unavailable; retrying");
            }
        }
        if (node->active) ops->poll();
        selected_instance = previous;
    }
    xSemaphoreGive(meshcore_instances.workers[kind]);
}
void solar_os_meshcore_instances_cancel(unsigned kind)
{
    if (!atomic_load(&ready) || kind < 1 || kind > 2) return;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY);
    const solar_os_meshcore_backend_t *ops = meshcore_instances.backends[kind];
    if (ops && ops->cancel) for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next) {
        if (node->config.backend != kind || !node->active) continue;
        meshcore_instance_t *previous = selected_instance; selected_instance = node;
        ops->cancel(); selected_instance = previous;
    }
    xSemaphoreGive(meshcore_instances.edit);
}
void solar_os_meshcore_instances_stop(unsigned kind)
{
    if (!atomic_load(&ready) || kind < 1 || kind > 2) return;
    xSemaphoreTake(meshcore_instances.workers[kind], portMAX_DELAY);
    for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next)
        if (node->config.backend == kind) stop_instance(node);
    xSemaphoreGive(meshcore_instances.workers[kind]);
}

bool solar_os_meshcore_instances_next(unsigned kind, solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status)
{
    if (kind < 1 || kind > 2 || !cursor || !status || solar_os_meshcore_instances_init() != ESP_OK) return false;
    xSemaphoreTake(meshcore_instances.edit, portMAX_DELAY);
    meshcore_instance_t *found = NULL;
    for (meshcore_instance_t *node = meshcore_instances.instances; node; node = node->next)
        if (node->config.backend == kind && node->id > *cursor && (!found || node->id < found->id)) found = node;
    bool ok = found && solar_os_messaging_provider_get_status(found->id, status) == ESP_OK;
    if (ok) *cursor = found->id;
    xSemaphoreGive(meshcore_instances.edit); return ok;
}
