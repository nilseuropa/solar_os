#pragma once
#include <stdint.h>
typedef struct esp_netif_obj esp_netif_t;
#define ESP_IPADDR_TYPE_V4 0U
#define ESP_IPADDR_TYPE_V6 6U
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { uint32_t addr[4]; uint8_t zone; } esp_ip6_addr_t;
typedef struct { union { esp_ip4_addr_t ip4; esp_ip6_addr_t ip6; } u_addr; uint8_t type; } esp_ip_addr_t;
typedef struct { esp_ip_addr_t ip; } esp_netif_dns_info_t;
