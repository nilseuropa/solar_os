#include "solar_os_network.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NETWORK_NVS_NAMESPACE "network"
#define NETWORK_NVS_PRIORITIES_KEY "priorities"
#define NETWORK_PRIORITY_STORE_VERSION 1U
#define NETWORK_PRIORITY_STORE_MAX 16U
#define NETWORK_AUX_INTERFACE_MAX 8U

typedef struct {
    solar_os_network_path_info_t info;
    bool registered;
} network_path_entry_t;

typedef struct {
    solar_os_network_interface_info_t info;
    bool registered;
} network_interface_entry_t;

typedef struct {
    char name[SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U];
    int32_t priority;
} network_priority_record_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    network_priority_record_t records[NETWORK_PRIORITY_STORE_MAX];
} network_priority_store_t;

ESP_EVENT_DEFINE_BASE(SOLAR_OS_NETWORK_EVENT);

static StaticSemaphore_t network_mutex_storage;
static SemaphoreHandle_t network_mutex;
static EXT_RAM_BSS_ATTR network_path_entry_t
    network_paths[SOLAR_OS_NETWORK_PATH_MAX];
static EXT_RAM_BSS_ATTR network_interface_entry_t
    network_interfaces[NETWORK_AUX_INTERFACE_MAX];
static solar_os_network_router_provider_t router_provider;
static bool router_provider_registered;
static network_priority_store_t priority_store;
static bool priority_store_loaded;
static struct netif * volatile network_preferred_lwip;

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

static bool address_valid(const char *address)
{
    return address != NULL &&
        strnlen(address, SOLAR_OS_NETWORK_ADDRESS_MAX + 1U) <=
            SOLAR_OS_NETWORK_ADDRESS_MAX;
}

static bool interface_role_valid(solar_os_network_interface_role_t role)
{
    return role >= SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK &&
        role <= SOLAR_OS_NETWORK_INTERFACE_ROLE_TUNNEL;
}

static bool interface_state_valid(solar_os_network_interface_state_t state)
{
    return state >= SOLAR_OS_NETWORK_INTERFACE_STATE_DOWN &&
        state <= SOLAR_OS_NETWORK_INTERFACE_STATE_ERROR;
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

static network_path_entry_t *find_name_locked(const char *name)
{
    for (size_t i = 0; i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        if (network_paths[i].registered &&
            strcmp(network_paths[i].info.name, name) == 0) {
            return &network_paths[i];
        }
    }
    return NULL;
}

static bool priority_valid(int priority)
{
    return priority >= SOLAR_OS_NETWORK_PRIORITY_MIN &&
        priority <= SOLAR_OS_NETWORK_PRIORITY_MAX;
}

static void priority_store_load_locked(void)
{
    if (priority_store_loaded) {
        return;
    }
    priority_store_loaded = true;
    memset(&priority_store, 0, sizeof(priority_store));

    nvs_handle_t nvs;
    if (nvs_open(NETWORK_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    network_priority_store_t stored = {0};
    size_t size = sizeof(stored);
    const esp_err_t ret = nvs_get_blob(nvs,
                                       NETWORK_NVS_PRIORITIES_KEY,
                                       &stored,
                                       &size);
    nvs_close(nvs);
    if (ret != ESP_OK || size != sizeof(stored) ||
        stored.version != NETWORK_PRIORITY_STORE_VERSION ||
        stored.count > NETWORK_PRIORITY_STORE_MAX) {
        return;
    }
    for (size_t i = 0; i < stored.count; i++) {
        if (!name_valid(stored.records[i].name) ||
            !priority_valid(stored.records[i].priority)) {
            return;
        }
    }
    priority_store = stored;
}

static bool priority_store_find_locked(const char *name, int *priority)
{
    priority_store_load_locked();
    for (size_t i = 0; i < priority_store.count; i++) {
        if (strcmp(priority_store.records[i].name, name) == 0) {
            if (priority != NULL) {
                *priority = priority_store.records[i].priority;
            }
            return true;
        }
    }
    return false;
}

static esp_err_t priority_store_save(const network_priority_store_t *store)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(NETWORK_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(nvs,
                           NETWORK_NVS_PRIORITIES_KEY,
                           store,
                           sizeof(*store));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    return ret;
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
        network_preferred_lwip = esp_netif_get_netif_impl(selected->info.netif);
    } else {
        network_preferred_lwip = NULL;
    }
    xSemaphoreGive(network_mutex);
    (void)esp_event_post(SOLAR_OS_NETWORK_EVENT,
                         SOLAR_OS_NETWORK_EVENT_PATHS_CHANGED,
                         &preferred,
                         sizeof(preferred),
                         0U);
}

static void post_interfaces_changed(void)
{
    (void)esp_event_post(SOLAR_OS_NETWORK_EVENT,
                         SOLAR_OS_NETWORK_EVENT_INTERFACES_CHANGED,
                         NULL,
                         0U,
                         0U);
}

esp_err_t solar_os_network_path_register(const char *name,
                                         esp_netif_t *netif,
                                         int route_priority)
{
    if (!name_valid(name) || netif == NULL || !priority_valid(route_priority)) {
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
    int effective_priority = route_priority;
    (void)priority_store_find_locked(name, &effective_priority);
    if (esp_netif_set_route_prio(netif, effective_priority) !=
        effective_priority) {
        xSemaphoreGive(network_mutex);
        return ESP_FAIL;
    }
    *free_entry = (network_path_entry_t) {
        .info = {
            .netif = netif,
            .route_priority = effective_priority,
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
    entry->info.connecting = false;
    entry->info.dns = dns;
    xSemaphoreGive(network_mutex);
    post_paths_changed();
    return ESP_OK;
}

esp_err_t solar_os_network_path_set_connecting(esp_netif_t *netif,
                                               bool connecting)
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
    entry->info.connecting = connecting && !entry->info.ready;
    xSemaphoreGive(network_mutex);
    post_paths_changed();
    return ESP_OK;
}

esp_err_t solar_os_network_path_set_priority(const char *name, int priority)
{
    if (!name_valid(name) || !priority_valid(priority)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    network_path_entry_t *path = find_name_locked(name);
    if (path == NULL) {
        xSemaphoreGive(network_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    priority_store_load_locked();
    network_priority_store_t updated = priority_store;
    size_t index = updated.count;
    for (size_t i = 0; i < updated.count; i++) {
        if (strcmp(updated.records[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index == updated.count) {
        if (updated.count >= NETWORK_PRIORITY_STORE_MAX) {
            xSemaphoreGive(network_mutex);
            return ESP_ERR_NO_MEM;
        }
        updated.count++;
    }
    updated.version = NETWORK_PRIORITY_STORE_VERSION;
    strlcpy(updated.records[index].name,
            name,
            sizeof(updated.records[index].name));
    updated.records[index].priority = priority;

    ret = priority_store_save(&updated);
    if (ret == ESP_OK &&
        esp_netif_set_route_prio(path->info.netif, priority) != priority) {
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK) {
        priority_store = updated;
        path->info.route_priority = priority;
    }
    xSemaphoreGive(network_mutex);
    if (ret == ESP_OK) {
        post_paths_changed();
    }
    return ret;
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

struct netif *solar_os_network_lwip_preferred(void)
{
    /* Registration removes a path before its esp-netif is destroyed. An
     * aligned pointer load is atomic on supported ESP targets, so the lwIP
     * route hook can stay non-blocking on the TCP/IP thread. */
    return network_preferred_lwip;
}

esp_err_t solar_os_network_interface_publish(
    const solar_os_network_interface_info_t *info)
{
    if (info == NULL || !name_valid(info->name) ||
        !interface_role_valid(info->role) ||
        info->role == SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK ||
        !interface_state_valid(info->state) ||
        !priority_valid(info->route_priority) ||
        !address_valid(info->address) || !address_valid(info->peer)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    if (find_name_locked(info->name) != NULL) {
        xSemaphoreGive(network_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    network_interface_entry_t *selected = NULL;
    for (size_t i = 0U; i < NETWORK_AUX_INTERFACE_MAX; i++) {
        network_interface_entry_t *entry = &network_interfaces[i];
        if (entry->registered && strcmp(entry->info.name, info->name) == 0) {
            selected = entry;
            break;
        }
        if (!entry->registered && selected == NULL) {
            selected = entry;
        }
    }
    if (selected == NULL) {
        xSemaphoreGive(network_mutex);
        return ESP_ERR_NO_MEM;
    }
    selected->info = *info;
    selected->info.netif = NULL;
    selected->registered = true;
    xSemaphoreGive(network_mutex);
    post_interfaces_changed();
    return ESP_OK;
}

esp_err_t solar_os_network_interface_remove(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(network_mutex, portMAX_DELAY);
    for (size_t i = 0U; i < NETWORK_AUX_INTERFACE_MAX; i++) {
        network_interface_entry_t *entry = &network_interfaces[i];
        if (entry->registered && strcmp(entry->info.name, name) == 0) {
            memset(entry, 0, sizeof(*entry));
            xSemaphoreGive(network_mutex);
            post_interfaces_changed();
            return ESP_OK;
        }
    }
    xSemaphoreGive(network_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_network_interface_list(
    solar_os_network_interface_info_t *interfaces,
    size_t max_interfaces)
{
    if (ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    for (size_t i = 0U; i < SOLAR_OS_NETWORK_PATH_MAX; i++) {
        const network_path_entry_t *path = &network_paths[i];
        if (!path->registered) {
            continue;
        }
        if (interfaces != NULL && count < max_interfaces) {
            solar_os_network_interface_info_t *info = &interfaces[count];
            memset(info, 0, sizeof(*info));
            info->netif = path->info.netif;
            info->role = SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK;
            info->state = path->info.ready ?
                SOLAR_OS_NETWORK_INTERFACE_STATE_UP :
                (path->info.connecting ?
                    SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING :
                    SOLAR_OS_NETWORK_INTERFACE_STATE_DOWN);
            info->route_priority = path->info.route_priority;
            strlcpy(info->name, path->info.name, sizeof(info->name));
        }
        count++;
    }
    for (size_t i = 0U; i < NETWORK_AUX_INTERFACE_MAX; i++) {
        if (!network_interfaces[i].registered) {
            continue;
        }
        if (interfaces != NULL && count < max_interfaces) {
            interfaces[count] = network_interfaces[i].info;
        }
        count++;
    }
    xSemaphoreGive(network_mutex);

    const size_t copied = count < max_interfaces ? count : max_interfaces;
    for (size_t i = 0U; interfaces != NULL && i < copied; i++) {
        solar_os_network_interface_info_t *info = &interfaces[i];
        if (info->role != SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK ||
            info->state != SOLAR_OS_NETWORK_INTERFACE_STATE_UP ||
            info->netif == NULL) {
            continue;
        }
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(info->netif, &ip) == ESP_OK &&
            ip.ip.addr != 0U) {
            esp_ip4addr_ntoa(&ip.ip, info->address, sizeof(info->address));
        }
    }
    return count;
}

const char *solar_os_network_interface_role_name(
    solar_os_network_interface_role_t role)
{
    switch (role) {
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK:
        return "uplink";
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM:
        return "downstream";
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER:
        return "peer";
    case SOLAR_OS_NETWORK_INTERFACE_ROLE_TUNNEL:
        return "tunnel";
    default:
        return "unknown";
    }
}

const char *solar_os_network_interface_state_name(
    solar_os_network_interface_state_t state)
{
    switch (state) {
    case SOLAR_OS_NETWORK_INTERFACE_STATE_DOWN:
        return "down";
    case SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_NETWORK_INTERFACE_STATE_UP:
        return "up";
    case SOLAR_OS_NETWORK_INTERFACE_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
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
