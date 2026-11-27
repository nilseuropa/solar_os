#include "solar_os_network.h"

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define ROUTER_PROVIDERS_MAX 4

typedef struct {
    char name[SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U];
    solar_os_network_router_provider_t provider;
    size_t refs;
} router_entry_t;

static EXT_RAM_BSS_ATTR router_entry_t routers[ROUTER_PROVIDERS_MAX];
static router_entry_t *selected;
static bool transition;
static portMUX_TYPE router_lock = portMUX_INITIALIZER_UNLOCKED;

static bool router_name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U) <=
            SOLAR_OS_NETWORK_PATH_NAME_MAX;
}

static router_entry_t *find_router(const char *name)
{
    for (size_t i = 0; i < ROUTER_PROVIDERS_MAX; ++i)
        if (routers[i].name[0] && strcmp(routers[i].name, name) == 0) return &routers[i];
    return NULL;
}

static router_entry_t *free_router(void)
{
    for (size_t i = 0; i < ROUTER_PROVIDERS_MAX; ++i)
        if (!routers[i].name[0]) return &routers[i];
    return NULL;
}

esp_err_t solar_os_network_router_register(const solar_os_network_router_provider_t *provider)
{
    if (!provider || !router_name_valid(provider->name) ||
        !provider->start || !provider->stop || !provider->get_status)
        return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&router_lock);
    router_entry_t *entry = find_router(provider->name);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!transition && entry) {
        const solar_os_network_router_provider_t *existing = &entry->provider;
        if (existing->context == provider->context && existing->start == provider->start &&
            existing->stop == provider->stop && existing->get_status == provider->get_status)
            result = ESP_OK;
    } else if (!transition) {
        entry = free_router();
        result = ESP_ERR_NO_MEM;
        if (entry) {
            strlcpy(entry->name, provider->name, sizeof(entry->name));
            entry->provider = *provider;
            entry->provider.name = entry->name;
            if (!selected) selected = entry;
            result = ESP_OK;
        }
    }
    portEXIT_CRITICAL(&router_lock);
    return result;
}

static esp_err_t acquire_router(router_entry_t **out)
{
    portENTER_CRITICAL(&router_lock);
    esp_err_t result = transition ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED;
    if (!transition && selected) {
        *out = selected;
        selected->refs++;
        result = ESP_OK;
    }
    portEXIT_CRITICAL(&router_lock);
    return result;
}

static void release_router(router_entry_t *entry)
{
    portENTER_CRITICAL(&router_lock);
    entry->refs--;
    portEXIT_CRITICAL(&router_lock);
}

static esp_err_t control_router(bool start)
{
    router_entry_t *entry;
    const esp_err_t error = acquire_router(&entry);
    if (error != ESP_OK) return error;
    const esp_err_t result = (start ? entry->provider.start : entry->provider.stop)(entry->provider.context);
    release_router(entry);
    return result;
}

esp_err_t solar_os_network_router_start(void)
{
    return control_router(true);
}

esp_err_t solar_os_network_router_stop(void)
{
    return control_router(false);
}

static void copy_status(router_entry_t *entry, solar_os_network_router_status_t *out)
{
    *out = (solar_os_network_router_status_t){.available = true};
    strlcpy(out->downstream, entry->name, sizeof(out->downstream));
    entry->provider.get_status(entry->provider.context, out);
    out->available = true;
    if (!out->downstream[0]) strlcpy(out->downstream, entry->name, sizeof(out->downstream));
}

void solar_os_network_router_get_status(solar_os_network_router_status_t *out)
{
    if (!out) return;
    *out = (solar_os_network_router_status_t){0};
    router_entry_t *entry;
    if (acquire_router(&entry) != ESP_OK) return;
    copy_status(entry, out);
    release_router(entry);
}

esp_err_t solar_os_network_router_select(const char *name, const void *owner)
{
    if (!name || !owner) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&router_lock);
    router_entry_t *entry = find_router(name);
    router_entry_t *previous = selected;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (entry && entry->provider.context == owner) {
        result = ESP_ERR_INVALID_STATE;
        if (!transition && (!previous || !previous->refs)) {
            transition = true;
            result = ESP_OK;
        }
    }
    portEXIT_CRITICAL(&router_lock);
    if (result != ESP_OK) return result;
    if (previous && previous != entry) {
        solar_os_network_router_status_t status;
        copy_status(previous, &status);
        if (status.enabled || status.active) result = ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&router_lock);
    if (result == ESP_OK) selected = entry;
    transition = false;
    portEXIT_CRITICAL(&router_lock);
    return result;
}

esp_err_t solar_os_network_router_unregister_owned(const char *name, const void *owner)
{
    if (!name || !owner) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&router_lock);
    router_entry_t *entry = find_router(name);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (entry && entry->provider.context == owner) {
        result = ESP_ERR_INVALID_STATE;
        if (!transition && !entry->refs) {
            transition = true;
            result = ESP_OK;
        }
    }
    portEXIT_CRITICAL(&router_lock);
    if (result != ESP_OK) return result;

    solar_os_network_router_status_t status;
    copy_status(entry, &status);
    if (status.enabled || status.active) result = entry->provider.stop(entry->provider.context);

    portENTER_CRITICAL(&router_lock);
    if (result == ESP_OK) {
        memset(entry, 0, sizeof(*entry));
        if (selected == entry) {
            selected = NULL;
            for (size_t i = 0; i < ROUTER_PROVIDERS_MAX; ++i) {
                if (routers[i].name[0]) {
                    selected = &routers[i];
                    break;
                }
            }
        }
    }
    transition = false;
    portEXIT_CRITICAL(&router_lock);
    return result;
}
