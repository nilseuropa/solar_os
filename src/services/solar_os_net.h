#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_NET_ADDR_MAX 46
#define SOLAR_OS_NET_HOST_MAX 128
#define SOLAR_OS_NET_PING_FOREVER 0
#define SOLAR_OS_NET_PING_MAX_COUNT 999

typedef enum {
    SOLAR_OS_NET_PING_REPLY,
    SOLAR_OS_NET_PING_TIMEOUT,
} solar_os_net_ping_event_type_t;

typedef struct {
    solar_os_net_ping_event_type_t type;
    uint16_t seqno;
    uint32_t bytes;
    uint32_t elapsed_ms;
    uint8_t ttl;
    char from[SOLAR_OS_NET_ADDR_MAX];
} solar_os_net_ping_event_t;

typedef struct {
    uint32_t count;
    uint32_t timeout_ms;
    uint32_t interval_ms;
    uint32_t data_size;
} solar_os_net_ping_options_t;

typedef struct {
    char resolved_ip[SOLAR_OS_NET_ADDR_MAX];
    bool interrupted;
    uint32_t transmitted;
    uint32_t received;
    uint32_t loss_percent;
    uint32_t total_time_ms;
    uint32_t min_time_ms;
    uint32_t avg_time_ms;
    uint32_t max_time_ms;
} solar_os_net_ping_result_t;

typedef void (*solar_os_net_ping_event_fn_t)(const solar_os_net_ping_event_t *event, void *user);
typedef bool (*solar_os_net_ping_stop_fn_t)(void *user);
typedef bool (*solar_os_net_host_visitor_t)(const char *address,
                                           const char *alias,
                                           void *user);

esp_err_t solar_os_net_hosts_visit(solar_os_net_host_visitor_t visitor, void *user);
esp_err_t solar_os_net_resolve_host(const char *host, char *resolved_ip, size_t resolved_ip_len);
esp_err_t solar_os_net_ping(const char *host,
                            const solar_os_net_ping_options_t *options,
                            solar_os_net_ping_event_fn_t on_event,
                            void *event_user,
                            solar_os_net_ping_stop_fn_t should_stop,
                            void *stop_user,
                            solar_os_net_ping_result_t *result);
