#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#define SOLAR_OS_UPLINK_NAME_MAX 20U
#define SOLAR_OS_UPLINK_MAX 8U

typedef struct {
    esp_netif_t *netif;
    char name[SOLAR_OS_UPLINK_NAME_MAX + 1U];
    int route_priority;
} solar_os_uplink_info_t;

ESP_EVENT_DECLARE_BASE(SOLAR_OS_UPLINK_EVENT);

typedef enum {
    SOLAR_OS_UPLINK_EVENT_CHANGED = 1,
} solar_os_uplink_event_id_t;

esp_err_t solar_os_uplink_register(const char *name,
                                    esp_netif_t *netif,
                                    int route_priority);
esp_err_t solar_os_uplink_unregister(esp_netif_t *netif);
esp_err_t solar_os_uplink_set_ready(esp_netif_t *netif, bool ready);
bool solar_os_uplink_get_active(solar_os_uplink_info_t *info);
