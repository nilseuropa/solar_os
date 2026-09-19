#include "solar_os_network.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    solar_os_network_path_info_t info;
    bool registered;
} network_path_entry_t;

ESP_EVENT_DEFINE_BASE(SOLAR_OS_NETWORK_EVENT);

static StaticSemaphore_t network_mutex_storage;
static SemaphoreHandle_t network_mutex;
static network_path_entry_t network_paths[SOLAR_OS_NETWORK_PATH_MAX];
static solar_os_network_router_provider_t router_provider;
static bool router_provider_registered;

static esp_err_t ensure_mutex(void)
{
    if (network_mutex == NULL) {
        network_mutex = xSemaphoreCreateMutexStatic(&network_mutex_storage);
    }
    return network_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U) <=
            SOLAR_OS_NETWORK_PATH_NAME_MAX;
}

static network_path_entry_t *find_netif_locked(esp_netif_t *netif)
{
    for (size_t i = 0; i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        if (network_paths[i].registered && network_paths[i].info.netif == netif) {
            return &network_paths[i];
        }
    }
    return NULL;
}

static network_path_entry_t *select_preferred_locked(void)
{
    esp_netif_t *default_netif = esp_netif_get_default_netif();
    network_path_entry_t *selected = NULL;
    for (size_t i = 0; i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        network_path_entry_t *candidate = &network_paths[i];
        if (!candidate->registered || !candidate->info.ready) {
            continue;
        }
        if (selected == NULL ||
            candidate->info.route_priority > selected->info.route_priority ||
            (candidate->info.route_priority == selected->info.route_priority &&
             candidate->info.netif == default_netif)) {
            selected = candidate;
        }
    }
    return selected;
}

static void post_paths_changed(void)
{
    solar_os_network_path_info_t preferred = {0};
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    const network_path_entry_t *selected = select_preferred_locked();
    if (selected != NULL) {
        preferred = selected->info;
    }
    xSemaphoreGive(network_mutex);
    (void)esp_event_post(SOLAR_OS_NETWORK_EVENT,
                         SOLAR_OS_NETWORK_EVENT_PATHS_CHANGED,
                         &preferred,
                         sizeof(preferred),
                         0U);
}

esp_err_t solar_os_network_path_register(const char *name,
                                         esp_netif_t *netif,
                                         int route_priority)
{
    if (!name_valid(name) || netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    network_path_entry_t *free_entry = NULL;
    for (size_t i = 0; i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        network_path_entry_t *entry = &network_paths[i];
        if (entry->registered &&
            (entry->info.netif == netif || strcmp(entry->info.name, name) == 0)) {
            xSemaphoreGive(network_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!entry->registered && free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (free_entry == NULL) {
        xSemaphoreGive(network_mutex);
        return ESP_ERR_NO_MEM;
    }
    *free_entry = (network_path_entry_t) {
        .info = {
            .netif = netif,
            .route_priority = route_priority,
        },
        .registered = true,
    };
    strlcpy(free_entry->info.name, name, sizeof(free_entry->info.name));
    xSemaphoreGive(network_mutex);
    post_paths_changed();
    return ESP_OK;
}

esp_err_t solar_os_network_path_unregister(esp_netif_t *netif)
{
    if (netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    network_path_entry_t *entry = find_netif_locked(netif);
    if (entry == NULL) {
        xSemaphoreGive(network_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(entry, 0, sizeof(*entry));
    xSemaphoreGive(network_mutex);
    post_paths_changed();
    return ESP_OK;
}

esp_err_t solar_os_network_path_set_ready(esp_netif_t *netif, bool ready)
{
    if (netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_netif_dns_info_t dns = {0};
    if (ready) {
        (void)esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    network_path_entry_t *entry = find_netif_locked(netif);
    if (entry == NULL) {
        xSemaphoreGive(network_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    entry->info.ready = ready;
    entry->info.dns = dns;
    xSemaphoreGive(network_mutex);
    post_paths_changed();
    return ESP_OK;
}

bool solar_os_network_path_get_preferred(solar_os_network_path_info_t *info)
{
    if (ensure_mutex() != ESP_OK) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    const network_path_entry_t *selected = select_preferred_locked();
    if (selected != NULL) {
        if (info != NULL) {
            *info = selected->info;
        }
        found = true;
    } else if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    xSemaphoreGive(network_mutex);
    return found;
}

size_t solar_os_network_path_list(solar_os_network_path_info_t *paths,
                                  size_t max_paths)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        if (!network_paths[i].registered) {
            continue;
        }
        if (paths != NULL && count < max_paths) {
            paths[count] = network_paths[i].info;
        }
        count++;
    }
    xSemaphoreGive(network_mutex);
    return count;
}

esp_err_t solar_os_network_router_register(
    const solar_os_network_router_provider_t *provider)
{
    if (provider == NULL || !name_valid(provider->name) ||
        provider->start == NULL || provider->stop == NULL ||
        provider->get_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    if (router_provider_registered) {
        ret = strcmp(router_provider.name, provider->name) == 0 ?
            ESP_OK : ESP_ERR_INVALID_STATE;
    } else {
        router_provider = *provider;
        router_provider_registered = true;
        ret = ESP_OK;
    }
    xSemaphoreGive(network_mutex);
    return ret;
}

static bool router_provider_get(solar_os_network_router_provider_t *provider)
{
    if (provider == NULL || ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    const bool found = router_provider_registered;
    if (found) {
        *provider = router_provider;
    }
    xSemaphoreGive(network_mutex);
    return found;
}

esp_err_t solar_os_network_router_start(void)
{
    solar_os_network_router_provider_t provider = {0};
    return router_provider_get(&provider) ?
        provider.start(provider.context) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_network_router_stop(void)
{
    solar_os_network_router_provider_t provider = {0};
    return router_provider_get(&provider) ?
        provider.stop(provider.context) : ESP_ERR_NOT_SUPPORTED;
}

void solar_os_network_router_get_status(solar_os_network_router_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    solar_os_network_router_provider_t provider = {0};
    if (!router_provider_get(&provider)) {
        return;
    }
    status->available = true;
    strlcpy(status->downstream, provider.name, sizeof(status->downstream));
    provider.get_status(provider.context, status);
    status->available = true;
    if (status->downstream[0] == '\0') {
        strlcpy(status->downstream, provider.name, sizeof(status->downstream));
    }
}
