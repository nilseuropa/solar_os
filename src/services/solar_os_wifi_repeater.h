#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#define SOLAR_OS_WIFI_REPEATER_CLIENT_MAX 16U

typedef struct {
    bool enabled;
    size_t learned_clients;
    uint64_t upstream_frames;
    uint64_t downstream_frames;
    uint64_t dropped_frames;
} solar_os_wifi_repeater_status_t;

esp_err_t solar_os_wifi_repeater_enable(esp_netif_t *ap, esp_netif_t *sta);
esp_err_t solar_os_wifi_repeater_disable(void);
bool solar_os_wifi_repeater_is_enabled(void);
void solar_os_wifi_repeater_on_ap_started(void);
void solar_os_wifi_repeater_on_upstream_ip(const esp_netif_ip_info_t *ip_info);
void solar_os_wifi_repeater_clear_clients(void);
void solar_os_wifi_repeater_get_status(solar_os_wifi_repeater_status_t *status);
struct netif *solar_os_wifi_repeater_route(const ip4_addr_t *destination);
struct netif *solar_os_wifi_repeater_upstream_route(void);
