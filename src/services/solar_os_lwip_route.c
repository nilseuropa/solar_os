#include "solar_os_lwip_route.h"

#include <string.h>

#include "lwip/ip4.h"
#include "solar_os_wifi_repeater.h"

typedef struct {
    struct netif *netif;
    solar_os_lwip_route_t routes[SOLAR_OS_LWIP_ROUTE_MAX];
    size_t route_count;
    bool fail_closed;
} solar_os_lwip_route_state_t;

static solar_os_lwip_route_state_t route_state;

void solar_os_lwip_route_configure(struct netif *netif,
                                   const solar_os_lwip_route_t *routes,
                                   size_t route_count,
                                   bool fail_closed)
{
    LWIP_ASSERT_CORE_LOCKED();

    memset(&route_state, 0, sizeof(route_state));
    if (netif == NULL || routes == NULL) {
        return;
    }
    if (route_count > SOLAR_OS_LWIP_ROUTE_MAX) {
        route_count = SOLAR_OS_LWIP_ROUTE_MAX;
    }
    route_state.netif = netif;
    route_state.route_count = route_count;
    route_state.fail_closed = fail_closed;
    memcpy(route_state.routes, routes, route_count * sizeof(route_state.routes[0]));
}

void solar_os_lwip_route_clear(struct netif *netif)
{
    LWIP_ASSERT_CORE_LOCKED();

    if (netif == NULL || route_state.netif == netif) {
        memset(&route_state, 0, sizeof(route_state));
    }
}

struct netif *solar_os_lwip_ip4_route_src_hook(const ip4_addr_t *src,
                                                const ip4_addr_t *dest)
{
    struct netif *candidate;

    LWIP_ASSERT_CORE_LOCKED();

    struct netif *repeater_netif = solar_os_wifi_repeater_route(dest);
    if (repeater_netif != NULL && netif_is_up(repeater_netif) &&
        netif_is_link_up(repeater_netif)) {
        return repeater_netif;
    }

    struct netif *repeater_upstream = solar_os_wifi_repeater_upstream_route();
    if (repeater_upstream != NULL && src != NULL && !ip4_addr_isany(src) &&
        ip4_addr_cmp(src, netif_ip4_addr(repeater_upstream)) &&
        netif_is_up(repeater_upstream) && netif_is_link_up(repeater_upstream)) {
        return repeater_upstream;
    }

    /* Preserve ESP-IDF's normal source-address routing before consulting the
     * destination table. This keeps a PCB bound to the Wi-Fi interface on
     * Wi-Fi even when 0.0.0.0/0 is routed through WireGuard. */
    if (src != NULL && !ip4_addr_isany(src)) {
        NETIF_FOREACH(candidate) {
            if (netif_is_up(candidate) && netif_is_link_up(candidate) &&
                !ip4_addr_isany_val(*netif_ip4_addr(candidate)) &&
                ip4_addr_cmp(src, netif_ip4_addr(candidate))) {
                return candidate;
            }
        }
    }

    if (dest != NULL && route_state.netif != NULL) {
        for (size_t i = 0; i < route_state.route_count; i++) {
            if (ip4_addr_netcmp(dest,
                               &route_state.routes[i].network,
                               &route_state.routes[i].netmask) &&
                (route_state.fail_closed ||
                 (netif_is_up(route_state.netif) &&
                  netif_is_link_up(route_state.netif)))) {
                return route_state.netif;
            }
        }
    }


    if (repeater_upstream != NULL && netif_is_up(repeater_upstream) &&
        netif_is_link_up(repeater_upstream)) {
        return repeater_upstream;
    }

    return NULL;
}
