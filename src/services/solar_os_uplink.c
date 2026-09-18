#include "solar_os_uplink.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    solar_os_uplink_info_t info;
    bool registered;
    bool ready;
} uplink_entry_t;

ESP_EVENT_DEFINE_BASE(SOLAR_OS_UPLINK_EVENT);

static StaticSemaphore_t uplink_mutex_storage;
static SemaphoreHandle_t uplink_mutex;
static uplink_entry_t uplinks[SOLAR_OS_UPLINK_MAX];

static esp_err_t ensure_mutex(void)
{
    if (uplink_mutex == NULL) {
        uplink_mutex = xSemaphoreCreateMutexStatic(&uplink_mutex_storage);
    }
    return uplink_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
        strnlen(name, SOLAR_OS_UPLINK_NAME_MAX + 1U) <= SOLAR_OS_UPLINK_NAME_MAX;
}

static uplink_entry_t *find_netif_locked(esp_netif_t *netif)
{
    for (size_t i = 0; i < SOLAR_OS_UPLINK_MAX; i++) {
        if (uplinks[i].registered && uplinks[i].info.netif == netif) {
            return &uplinks[i];
        }
    }
    return NULL;
}

static uplink_entry_t *select_active_locked(void)
{
    esp_netif_t *default_netif = esp_netif_get_default_netif();
    uplink_entry_t *selected = NULL;
    for (size_t i = 0; i < SOLAR_OS_UPLINK_MAX; i++) {
        uplink_entry_t *candidate = &uplinks[i];
        if (!candidate->registered || !candidate->ready) {
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

static void post_changed(void)
{
    solar_os_uplink_info_t active = {0};
    xSemaphoreTake(uplink_mutex, portMAX_DELAY);
    const uplink_entry_t *selected = select_active_locked();
    if (selected != NULL) {
        active = selected->info;
    }
    xSemaphoreGive(uplink_mutex);
    (void)esp_event_post(SOLAR_OS_UPLINK_EVENT,
                         SOLAR_OS_UPLINK_EVENT_CHANGED,
                         &active,
                         sizeof(active),
                         0U);
}

esp_err_t solar_os_uplink_register(const char *name,
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

    xSemaphoreTake(uplink_mutex, portMAX_DELAY);
    uplink_entry_t *free_entry = NULL;
    for (size_t i = 0; i < SOLAR_OS_UPLINK_MAX; i++) {
        uplink_entry_t *entry = &uplinks[i];
        if (entry->registered &&
            (entry->info.netif == netif || strcmp(entry->info.name, name) == 0)) {
            xSemaphoreGive(uplink_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!entry->registered && free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (free_entry == NULL) {
        xSemaphoreGive(uplink_mutex);
        return ESP_ERR_NO_MEM;
    }
    *free_entry = (uplink_entry_t) {
        .info = {
            .netif = netif,
            .route_priority = route_priority,
        },
        .registered = true,
    };
    strlcpy(free_entry->info.name, name, sizeof(free_entry->info.name));
    xSemaphoreGive(uplink_mutex);
    post_changed();
    return ESP_OK;
}

esp_err_t solar_os_uplink_unregister(esp_netif_t *netif)
{
    if (netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(uplink_mutex, portMAX_DELAY);
    uplink_entry_t *entry = find_netif_locked(netif);
    if (entry == NULL) {
        xSemaphoreGive(uplink_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(entry, 0, sizeof(*entry));
    xSemaphoreGive(uplink_mutex);
    post_changed();
    return ESP_OK;
}

esp_err_t solar_os_uplink_set_ready(esp_netif_t *netif, bool ready)
{
    if (netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(uplink_mutex, portMAX_DELAY);
    uplink_entry_t *entry = find_netif_locked(netif);
    if (entry == NULL) {
        xSemaphoreGive(uplink_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    entry->ready = ready;
    xSemaphoreGive(uplink_mutex);
    post_changed();
    return ESP_OK;
}

bool solar_os_uplink_get_active(solar_os_uplink_info_t *info)
{
    if (ensure_mutex() != ESP_OK) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(uplink_mutex, portMAX_DELAY);
    const uplink_entry_t *selected = select_active_locked();
    if (selected != NULL) {
        if (info != NULL) {
            *info = selected->info;
        }
        found = true;
    } else if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    xSemaphoreGive(uplink_mutex);
    return found;
}
