#include "solar_os_net.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "solar_os_queue.h"
#include "solar_os_storage.h"
#include "solar_os_wifi.h"

#define SOLAR_OS_NET_PING_DEFAULT_TIMEOUT_MS 1000U
#define SOLAR_OS_NET_PING_DEFAULT_INTERVAL_MS 1000U
#define SOLAR_OS_NET_PING_DEFAULT_DATA_SIZE 32U
#define SOLAR_OS_NET_PING_EVENT_QUEUE_LEN 8
#define SOLAR_OS_NET_SSH_DIR ".ssh"
#define SOLAR_OS_NET_HOSTS "hosts"

static const char *TAG = "solar_os_net";

typedef struct {
    solar_os_net_ping_event_t packet;
    bool ended;
    uint32_t transmitted;
    uint32_t received;
    uint32_t total_time_ms;
} solar_os_net_ping_queue_event_t;

typedef struct {
    QueueHandle_t events;
} solar_os_net_ping_context_t;

static void ping_copy_ip(const ip_addr_t *addr, char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    if (addr == NULL || ipaddr_ntoa_r(addr, buffer, len) == NULL) {
        strlcpy(buffer, "0.0.0.0", len);
    }
}

static esp_err_t net_lookup_hosts_file(const char *host, char *resolved_host, size_t resolved_host_len)
{
    if (host == NULL || host[0] == '\0' || resolved_host == NULL || resolved_host_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(resolved_host, host, resolved_host_len);
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }

    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    char hosts_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(SOLAR_OS_NET_SSH_DIR, dir, sizeof(dir)) != ESP_OK ||
        solar_os_storage_join_path(dir, SOLAR_OS_NET_HOSTS, hosts_path, sizeof(hosts_path)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(hosts_path, "r");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[192];
    bool found = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }

        char *saveptr = NULL;
        const char *address = strtok_r(line, " \t\r\n", &saveptr);
        if (address == NULL) {
            continue;
        }

        const char *name = NULL;
        while ((name = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
            if (strcasecmp(name, host) == 0) {
                strlcpy(resolved_host, address, resolved_host_len);
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }

    fclose(file);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ping_resolve_host(const char *host,
                                   ip_addr_t *target_addr,
                                   char *resolved_ip,
                                   size_t resolved_ip_len)
{
    if (host == NULL || host[0] == '\0' || target_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;

    char resolved_host[SOLAR_OS_NET_HOST_MAX];
    esp_err_t hosts_ret = net_lookup_hosts_file(host, resolved_host, sizeof(resolved_host));
    if (hosts_ret == ESP_OK && strcmp(host, resolved_host) != 0) {
        SOLAR_OS_LOGI(TAG, "hosts: %s -> %s", host, resolved_host);
    } else if (hosts_ret != ESP_OK && hosts_ret != ESP_ERR_NOT_FOUND) {
        return hosts_ret;
    }

    const int gai = getaddrinfo(resolved_host, NULL, &hints, &res);
    if (gai != 0 || res == NULL) {
        SOLAR_OS_LOGW(TAG, "resolve failed: %s gai=%d", resolved_host, gai);
        return ESP_ERR_NOT_FOUND;
    }

    memset(target_addr, 0, sizeof(*target_addr));
    if (res->ai_family != AF_INET || res->ai_addr == NULL) {
        freeaddrinfo(res);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const struct in_addr addr4 = ((const struct sockaddr_in *)res->ai_addr)->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(target_addr), &addr4);
    IP_SET_TYPE(target_addr, IPADDR_TYPE_V4);
    ping_copy_ip(target_addr, resolved_ip, resolved_ip_len);
    freeaddrinfo(res);
    return ESP_OK;
}

esp_err_t solar_os_net_resolve_host(const char *host, char *resolved_ip, size_t resolved_ip_len)
{
    if (host == NULL || host[0] == '\0' || resolved_ip == NULL || resolved_ip_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;

    char resolved_host[SOLAR_OS_NET_HOST_MAX];
    esp_err_t hosts_ret = net_lookup_hosts_file(host, resolved_host, sizeof(resolved_host));
    if (hosts_ret == ESP_OK && strcmp(host, resolved_host) != 0) {
        SOLAR_OS_LOGI(TAG, "hosts: %s -> %s", host, resolved_host);
    } else if (hosts_ret != ESP_OK && hosts_ret != ESP_ERR_NOT_FOUND) {
        return hosts_ret;
    }

    const int gai = getaddrinfo(resolved_host, NULL, &hints, &res);
    if (gai != 0 || res == NULL) {
        SOLAR_OS_LOGW(TAG, "resolve failed: %s gai=%d", resolved_host, gai);
        return ESP_ERR_NOT_FOUND;
    }

    if (res->ai_family != AF_INET || res->ai_addr == NULL) {
        freeaddrinfo(res);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ip_addr_t target_addr = {0};
    const struct in_addr addr4 = ((const struct sockaddr_in *)res->ai_addr)->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    IP_SET_TYPE(&target_addr, IPADDR_TYPE_V4);
    ping_copy_ip(&target_addr, resolved_ip, resolved_ip_len);
    freeaddrinfo(res);
    return ESP_OK;
}

static void ping_store_event(solar_os_net_ping_context_t *ctx,
                             solar_os_net_ping_event_type_t type,
                             uint16_t seqno,
                             uint32_t bytes,
                             uint32_t elapsed_ms,
                             uint8_t ttl,
                             const ip_addr_t *addr)
{
    if (ctx == NULL || ctx->events == NULL) {
        return;
    }

    solar_os_net_ping_queue_event_t event = {
        .packet = {
            .type = type,
            .seqno = seqno,
            .bytes = bytes,
            .elapsed_ms = elapsed_ms,
            .ttl = ttl,
        },
    };
    ping_copy_ip(addr, event.packet.from, sizeof(event.packet.from));
    (void)xQueueSend(ctx->events, &event, portMAX_DELAY);
}

static void ping_on_success(esp_ping_handle_t handle, void *args)
{
    solar_os_net_ping_context_t *ctx = (solar_os_net_ping_context_t *)args;
    uint16_t seqno = 0;
    uint32_t bytes = 0;
    uint32_t elapsed_ms = 0;
    uint8_t ttl = 0;
    ip_addr_t addr = {0};

    (void)esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_SIZE, &bytes, sizeof(bytes));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
    ping_store_event(ctx, SOLAR_OS_NET_PING_REPLY, seqno, bytes, elapsed_ms, ttl, &addr);
}

static void ping_on_timeout(esp_ping_handle_t handle, void *args)
{
    solar_os_net_ping_context_t *ctx = (solar_os_net_ping_context_t *)args;
    uint16_t seqno = 0;
    ip_addr_t addr = {0};

    (void)esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
    ping_store_event(ctx, SOLAR_OS_NET_PING_TIMEOUT, seqno, 0, 0, 0, &addr);
}

static void ping_on_end(esp_ping_handle_t handle, void *args)
{
    solar_os_net_ping_context_t *ctx = (solar_os_net_ping_context_t *)args;
    solar_os_net_ping_queue_event_t event = {
        .ended = true,
    };

    if (ctx != NULL && ctx->events != NULL) {
        (void)esp_ping_get_profile(handle,
                                   ESP_PING_PROF_REQUEST,
                                   &event.transmitted,
                                   sizeof(event.transmitted));
        (void)esp_ping_get_profile(handle,
                                   ESP_PING_PROF_REPLY,
                                   &event.received,
                                   sizeof(event.received));
        (void)esp_ping_get_profile(handle,
                                   ESP_PING_PROF_DURATION,
                                   &event.total_time_ms,
                                   sizeof(event.total_time_ms));
        (void)xQueueSend(ctx->events, &event, portMAX_DELAY);
    }
}

static void ping_update_stats(solar_os_net_ping_result_t *result,
                              const solar_os_net_ping_event_t *event,
                              uint64_t *reply_time_total)
{
    if (result == NULL || event == NULL || event->type != SOLAR_OS_NET_PING_REPLY) {
        return;
    }

    if (result->received == 0 || event->elapsed_ms < result->min_time_ms) {
        result->min_time_ms = event->elapsed_ms;
    }
    if (event->elapsed_ms > result->max_time_ms) {
        result->max_time_ms = event->elapsed_ms;
    }
    if (reply_time_total != NULL) {
        *reply_time_total += event->elapsed_ms;
    }
    result->received++;
}

static void ping_finish_stats(solar_os_net_ping_result_t *result, uint64_t reply_time_total)
{
    if (result == NULL) {
        return;
    }
    if (result->transmitted > 0) {
        const uint32_t lost = result->transmitted > result->received ?
            result->transmitted - result->received :
            0U;
        result->loss_percent = (lost * 100U) / result->transmitted;
    }
    if (result->received > 0) {
        result->avg_time_ms = (uint32_t)(reply_time_total / result->received);
    }
}

esp_err_t solar_os_net_ping(const char *host,
                            const solar_os_net_ping_options_t *options,
                            solar_os_net_ping_event_fn_t on_event,
                            void *event_user,
                            solar_os_net_ping_stop_fn_t should_stop,
                            void *stop_user,
                            solar_os_net_ping_result_t *result)
{
    if (host == NULL || host[0] == '\0' || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.has_ip) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(result, 0, sizeof(*result));

    const uint32_t count = options != NULL ?
        options->count :
        SOLAR_OS_NET_PING_FOREVER;
    if (count > SOLAR_OS_NET_PING_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t timeout_ms = options != NULL && options->timeout_ms > 0 ?
        options->timeout_ms :
        SOLAR_OS_NET_PING_DEFAULT_TIMEOUT_MS;
    const uint32_t interval_ms = options != NULL && options->interval_ms > 0 ?
        options->interval_ms :
        SOLAR_OS_NET_PING_DEFAULT_INTERVAL_MS;
    const uint32_t data_size = options != NULL && options->data_size > 0 ?
        options->data_size :
        SOLAR_OS_NET_PING_DEFAULT_DATA_SIZE;

    ip_addr_t target_addr = {0};
    esp_err_t ret = ping_resolve_host(host,
                                      &target_addr,
                                      result->resolved_ip,
                                      sizeof(result->resolved_ip));
    if (ret != ESP_OK) {
        return ret;
    }

    QueueHandle_t events = solar_os_queue_create(
        SOLAR_OS_NET_PING_EVENT_QUEUE_LEN,
        sizeof(solar_os_net_ping_queue_event_t));
    if (events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_net_ping_context_t context = {
        .events = events,
    };
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = count;
    config.timeout_ms = timeout_ms;
    config.interval_ms = interval_ms;
    config.data_size = data_size;
    config.target_addr = target_addr;

    const esp_ping_callbacks_t callbacks = {
        .cb_args = &context,
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
    };

    esp_ping_handle_t ping = NULL;
    ret = esp_ping_new_session(&config, &callbacks, &ping);
    if (ret == ESP_OK) {
        ret = esp_ping_start(ping);
    }

    if (ret == ESP_OK) {
        bool ended = false;
        bool stop_requested = false;
        uint64_t reply_time_total = 0;

        while (!ended) {
            solar_os_net_ping_queue_event_t event;
            if (xQueueReceive(events, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (event.ended) {
                    result->transmitted = event.transmitted;
                    result->received = event.received;
                    result->total_time_ms = event.total_time_ms;
                    ended = true;
                } else {
                    ping_update_stats(result, &event.packet, &reply_time_total);
                    if (on_event != NULL) {
                        on_event(&event.packet, event_user);
                    }
                }
            }

            if (!stop_requested && should_stop != NULL && should_stop(stop_user)) {
                stop_requested = true;
                result->interrupted = true;
                if (ping != NULL) {
                    (void)esp_ping_stop(ping);
                }
            }
        }

        ping_finish_stats(result, reply_time_total);
    }

    if (ping != NULL) {
        (void)esp_ping_delete_session(ping);
    }
    solar_os_queue_delete(events);
    return ret;
}
