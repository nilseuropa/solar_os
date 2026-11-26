#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_ROUTING_SERVICE "network.routing"
#define SOLAR_OS_NATIVE_ROUTING_ABI 1U
#define SOLAR_OS_NATIVE_ROUTING_UPLINK 0U
#define SOLAR_OS_NATIVE_ROUTING_DOWNSTREAM 1U
#define SOLAR_OS_NATIVE_ROUTING_PEER 2U
#define SOLAR_OS_NATIVE_ROUTING_TUNNEL 3U
#define SOLAR_OS_NATIVE_ROUTING_DOWN 0U
#define SOLAR_OS_NATIVE_ROUTING_CONNECTING 1U
#define SOLAR_OS_NATIVE_ROUTING_UP 2U
#define SOLAR_OS_NATIVE_ROUTING_ERROR 3U
typedef struct {
    char name[21];
    int32_t priority;
    uint32_t ready, connecting, dns_family, dns_zone;
    uint8_t dns_address[16]; /* Network-order bytes; family 0, 4 or 6. */
} solar_os_native_network_path_v1_t;
typedef struct {
    char name[21], address[47], peer[47];
    int32_t priority, error;
    uint32_t role, state, nat_enabled;
} solar_os_native_network_interface_v1_t;
typedef struct {
    uint32_t available, downstream_active, enabled, active, nat_enabled, client_count, client_limit;
    int32_t error;
    char downstream[21], address[47], label[33];
} solar_os_native_network_router_v1_t;
/* Lists return the total count and copy min(count, capacity) records. No
 * esp-netif or lwIP pointers cross this interface. Interface publication is
 * for logical peer/downstream/tunnel status; uplinks belong to OS drivers. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*paths)(solar_os_native_network_path_v1_t *paths, size_t capacity, size_t *count);
    int (*preferred)(solar_os_native_network_path_v1_t *path);
    int (*priority_set)(const char *name, int32_t priority);
    int (*interfaces)(solar_os_native_network_interface_v1_t *interfaces, size_t capacity, size_t *count);
    int (*interface_publish)(const solar_os_native_network_interface_v1_t *interface_info);
    int (*interface_remove)(const char *name);
    int (*router_status)(solar_os_native_network_router_v1_t *status);
    int (*router_start)(void);
    int (*router_stop)(void);
    const char *(*role_name)(uint32_t role);
    const char *(*state_name)(uint32_t state);
} solar_os_native_routing_api_v1_t;
