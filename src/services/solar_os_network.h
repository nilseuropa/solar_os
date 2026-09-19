#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#define SOLAR_OS_NETWORK_PATH_NAME_MAX 20U
#define SOLAR_OS_NETWORK_PATH_MAX 8U
#define SOLAR_OS_NETWORK_ADDRESS_MAX 46U
#define SOLAR_OS_NETWORK_DOWNSTREAM_LABEL_MAX 32U

typedef struct {
    esp_netif_t *netif;
    char name[SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U];
    int route_priority;
    bool ready;
    esp_netif_dns_info_t dns;
} solar_os_network_path_info_t;

ESP_EVENT_DECLARE_BASE(SOLAR_OS_NETWORK_EVENT);

typedef enum {
    SOLAR_OS_NETWORK_EVENT_PATHS_CHANGED = 1,
} solar_os_network_event_id_t;

typedef struct {
    bool available;
    bool downstream_active;
    bool enabled;
    bool active;
    bool nat_enabled;
    uint16_t client_count;
    uint16_t client_limit;
    esp_err_t last_error;
    char downstream[SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U];
    char address[SOLAR_OS_NETWORK_ADDRESS_MAX + 1U];
    char label[SOLAR_OS_NETWORK_DOWNSTREAM_LABEL_MAX + 1U];
} solar_os_network_router_status_t;

typedef esp_err_t (*solar_os_network_router_control_fn_t)(void *context);
typedef void (*solar_os_network_router_status_fn_t)(
    void *context,
    solar_os_network_router_status_t *status);

typedef struct {
    const char *name;
    void *context;
    solar_os_network_router_control_fn_t start;
    solar_os_network_router_control_fn_t stop;
    solar_os_network_router_status_fn_t get_status;
} solar_os_network_router_provider_t;

esp_err_t solar_os_network_path_register(const char *name,
                                         esp_netif_t *netif,
                                         int route_priority);
esp_err_t solar_os_network_path_unregister(esp_netif_t *netif);
esp_err_t solar_os_network_path_set_ready(esp_netif_t *netif, bool ready);
bool solar_os_network_path_get_preferred(solar_os_network_path_info_t *info);
size_t solar_os_network_path_list(solar_os_network_path_info_t *paths,
                                  size_t max_paths);

esp_err_t solar_os_network_router_register(
    const solar_os_network_router_provider_t *provider);
esp_err_t solar_os_network_router_start(void);
esp_err_t solar_os_network_router_stop(void);
void solar_os_network_router_get_status(solar_os_network_router_status_t *status);
