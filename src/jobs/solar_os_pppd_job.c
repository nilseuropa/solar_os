#include "solar_os_pppd_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_network.h"
#include "solar_os_port.h"
#include "solar_os_ppp.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#if SOLAR_OS_PACKAGE_SERVICE_UART
#include "solar_os_uart.h"
#endif

#if !CONFIG_LWIP_PPP_SERVER_SUPPORT
#error "job.pppd requires CONFIG_LWIP_PPP_SERVER_SUPPORT=y"
#endif

#if CONFIG_LWIP_PPP_VJ_HEADER_COMPRESSION
#error "job.pppd downstream NAPT requires PPP VJ header compression to be disabled"
#endif

#define PPPD_TASK_STACK 3072U
#define PPPD_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define PPPD_READ_TIMEOUT_MS 100U
#define PPPD_RETRY_DELAY_MS 1000U
#define PPPD_STATUS_POLL_MS 100U
#define PPPD_STOP_WAIT_MS 8000U
#define PPPD_DEFAULT_LOCAL_IP "192.168.8.1"
#define PPPD_DEFAULT_PEER_IP "192.168.8.2"
#define PPPD_DEFAULT_UPLINK_PRIORITY 90
#define PPPD_DOWNSTREAM_ROUTE_PRIORITY 5

typedef enum {
    PPPD_ROLE_DOWNSTREAM = 0,
    PPPD_ROLE_UPLINK,
    PPPD_ROLE_PEER,
} pppd_role_t;

typedef enum {
    PPPD_DNS_AUTO = 0,
    PPPD_DNS_NONE,
    PPPD_DNS_ADDRESS,
} pppd_dns_mode_t;

typedef struct {
    char port_name[SOLAR_OS_PORT_NAME_MAX];
    uint32_t baud_rate;
    bool baud_explicit;
    pppd_role_t role;
    solar_os_ppp_mode_t mode;
    int priority;
    esp_ip4_addr_t local_address;
    esp_ip4_addr_t peer_address;
    pppd_dns_mode_t dns_mode;
    esp_ip4_addr_t dns_address;
} pppd_config_t;

typedef struct {
    bool running;
    volatile bool stop_requested;
    TaskHandle_t task;
    solar_os_port_handle_t port;
    solar_os_ppp_t *ppp;
    esp_netif_t *netif;
    pppd_config_t config;
    solar_os_ppp_profile_t profile;
    char path_name[SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U];
    bool nat_active;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_errors;
    uint32_t tx_errors;
    uint32_t reconnects;
    int32_t last_ppp_error;
    esp_err_t last_error;
} pppd_state_t;

static const char *TAG = "pppd";
static pppd_state_t pppd_job = {
    .port = SOLAR_OS_PORT_HANDLE_INIT,
    .last_error = ESP_OK,
};

static const char *pppd_role_name(pppd_role_t role)
{
    switch (role) {
    case PPPD_ROLE_DOWNSTREAM:
        return "downstream";
    case PPPD_ROLE_UPLINK:
        return "uplink";
    case PPPD_ROLE_PEER:
        return "peer";
    default:
        return "unknown";
    }
}

static const char *pppd_mode_name(solar_os_ppp_mode_t mode)
{
    return mode == SOLAR_OS_PPP_MODE_PASSIVE ? "passive" : "active";
}

static const char *pppd_state_name(solar_os_ppp_state_t state)
{
    switch (state) {
    case SOLAR_OS_PPP_STATE_DOWN:
        return "down";
    case SOLAR_OS_PPP_STATE_CONNECTING:
        return "negotiating";
    case SOLAR_OS_PPP_STATE_UP:
        return "up";
    case SOLAR_OS_PPP_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static bool pppd_has_published_interface(void)
{
    return pppd_job.config.role == PPPD_ROLE_DOWNSTREAM ||
        pppd_job.config.role == PPPD_ROLE_PEER;
}

static esp_err_t pppd_publish_interface(
    solar_os_network_interface_state_t state,
    bool nat_enabled,
    esp_err_t error)
{
    if (!pppd_has_published_interface()) {
        return ESP_OK;
    }
    solar_os_network_interface_info_t info = {
        .role = pppd_job.config.role == PPPD_ROLE_DOWNSTREAM ?
            SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM :
            SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER,
        .state = state,
        .route_priority = PPPD_DOWNSTREAM_ROUTE_PRIORITY,
        .nat_enabled = nat_enabled,
        .last_error = error,
    };
    strlcpy(info.name, pppd_job.path_name, sizeof(info.name));

    solar_os_ppp_status_t status = {0};
    if (state == SOLAR_OS_NETWORK_INTERFACE_STATE_UP &&
        pppd_job.ppp != NULL &&
        solar_os_ppp_get_status(pppd_job.ppp, &status) == ESP_OK) {
        strlcpy(info.address, status.ipv4_address, sizeof(info.address));
        strlcpy(info.peer, status.ipv4_gateway, sizeof(info.peer));
    }
    if (info.address[0] == '\0' &&
        pppd_job.config.local_address.addr != 0U) {
        esp_ip4addr_ntoa(&pppd_job.config.local_address,
                         info.address,
                         sizeof(info.address));
    }
    if (info.peer[0] == '\0' && pppd_job.config.peer_address.addr != 0U) {
        esp_ip4addr_ntoa(&pppd_job.config.peer_address,
                         info.peer,
                         sizeof(info.peer));
    }
    return solar_os_network_interface_publish(&info);
}

static bool pppd_parse_u32(const char *text,
                           uint32_t minimum,
                           uint32_t maximum,
                           uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool pppd_parse_ip(const char *text, esp_ip4_addr_t *address)
{
    return text != NULL && address != NULL &&
        esp_netif_str_to_ip4(text, address) == ESP_OK && address->addr != 0U;
}

static bool pppd_parse_role(const char *text, pppd_role_t *role)
{
    if (strcmp(text, "downstream") == 0) {
        *role = PPPD_ROLE_DOWNSTREAM;
    } else if (strcmp(text, "uplink") == 0) {
        *role = PPPD_ROLE_UPLINK;
    } else if (strcmp(text, "peer") == 0) {
        *role = PPPD_ROLE_PEER;
    } else {
        return false;
    }
    return true;
}

static bool pppd_parse_mode(const char *text, solar_os_ppp_mode_t *mode)
{
    if (strcmp(text, "active") == 0) {
        *mode = SOLAR_OS_PPP_MODE_ACTIVE;
    } else if (strcmp(text, "passive") == 0) {
        *mode = SOLAR_OS_PPP_MODE_PASSIVE;
    } else {
        return false;
    }
    return true;
}

static bool pppd_parse_args(int argc, char **argv, pppd_config_t *config)
{
    if (config == NULL || argc < 0) {
        return false;
    }
    memset(config, 0, sizeof(*config));
    config->role = PPPD_ROLE_DOWNSTREAM;
    config->priority = PPPD_DEFAULT_UPLINK_PRIORITY;
    config->dns_mode = PPPD_DNS_AUTO;

    int first_arg = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_pppd_job.name) == 0) {
        first_arg = 1;
    }
    if (argc - first_arg < 1 || argv == NULL || argv[first_arg] == NULL ||
        argv[first_arg][0] == '\0' ||
        strlen(argv[first_arg]) >= sizeof(config->port_name)) {
        return false;
    }
    strlcpy(config->port_name,
            argv[first_arg],
            sizeof(config->port_name));

    bool mode_explicit = false;
    bool local_explicit = false;
    bool peer_explicit = false;
    bool priority_explicit = false;
    for (int i = first_arg + 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL || arg[0] == '\0') {
            return false;
        }
        uint32_t parsed = 0U;
        if (strchr(arg, '=') == NULL && !config->baud_explicit &&
            pppd_parse_u32(arg, 300U, 5000000U, &parsed)) {
            config->baud_rate = parsed;
            config->baud_explicit = true;
        } else if (strncmp(arg, "baud=", 5) == 0 && !config->baud_explicit &&
                   pppd_parse_u32(arg + 5, 300U, 5000000U, &parsed)) {
            config->baud_rate = parsed;
            config->baud_explicit = true;
        } else if (strncmp(arg, "role=", 5) == 0) {
            if (!pppd_parse_role(arg + 5, &config->role)) {
                return false;
            }
        } else if (strncmp(arg, "mode=", 5) == 0) {
            if (!pppd_parse_mode(arg + 5, &config->mode)) {
                return false;
            }
            mode_explicit = true;
        } else if (strncmp(arg, "local=", 6) == 0) {
            if (!pppd_parse_ip(arg + 6, &config->local_address)) {
                return false;
            }
            local_explicit = true;
        } else if (strncmp(arg, "peer=", 5) == 0) {
            if (!pppd_parse_ip(arg + 5, &config->peer_address)) {
                return false;
            }
            peer_explicit = true;
        } else if (strncmp(arg, "dns=", 4) == 0) {
            if (strcmp(arg + 4, "auto") == 0) {
                config->dns_mode = PPPD_DNS_AUTO;
                config->dns_address.addr = 0U;
            } else if (strcmp(arg + 4, "none") == 0) {
                config->dns_mode = PPPD_DNS_NONE;
                config->dns_address.addr = 0U;
            } else if (pppd_parse_ip(arg + 4, &config->dns_address)) {
                config->dns_mode = PPPD_DNS_ADDRESS;
            } else {
                return false;
            }
        } else if (strncmp(arg, "priority=", 9) == 0 &&
                   pppd_parse_u32(arg + 9,
                                   SOLAR_OS_NETWORK_PRIORITY_MIN,
                                   SOLAR_OS_NETWORK_PRIORITY_MAX,
                                   &parsed)) {
            config->priority = (int)parsed;
            priority_explicit = true;
        } else {
            return false;
        }
    }

    if (!mode_explicit) {
        config->mode = config->role == PPPD_ROLE_DOWNSTREAM
            ? SOLAR_OS_PPP_MODE_PASSIVE
            : SOLAR_OS_PPP_MODE_ACTIVE;
    }
    if (config->role == PPPD_ROLE_DOWNSTREAM) {
        if (!local_explicit &&
            !pppd_parse_ip(PPPD_DEFAULT_LOCAL_IP, &config->local_address)) {
            return false;
        }
        if (!peer_explicit &&
            !pppd_parse_ip(PPPD_DEFAULT_PEER_IP, &config->peer_address)) {
            return false;
        }
    }
    if (priority_explicit && config->role != PPPD_ROLE_UPLINK) {
        return false;
    }
    if (config->mode == SOLAR_OS_PPP_MODE_PASSIVE &&
        (config->local_address.addr == 0U || config->peer_address.addr == 0U)) {
        return false;
    }
    return config->local_address.addr == 0U || config->peer_address.addr == 0U ||
        config->local_address.addr != config->peer_address.addr;
}

static esp_err_t pppd_validate_and_configure_port(const pppd_config_t *config)
{
    solar_os_port_info_t info;
    esp_err_t ret = solar_os_port_get_info(config->port_name, &info);
    if (ret != ESP_OK) {
        return ret;
    }
    if (info.claimed) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((info.capabilities & (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) !=
        (SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((info.capabilities & SOLAR_OS_PORT_CAP_CONFIG) == 0U) {
        return config->baud_explicit ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    }
#if SOLAR_OS_PACKAGE_SERVICE_UART
    if (config->baud_explicit) {
        ret = solar_os_uart_bus_set_baud_rate(config->port_name,
                                              config->baud_rate);
    }
    if (ret == ESP_OK) {
        ret = solar_os_uart_bus_set_mode(config->port_name,
                                         SOLAR_OS_UART_MODE_RAW);
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t pppd_transport_read(void *ctx,
                                     uint8_t *data,
                                     size_t length,
                                     uint32_t timeout_ms,
                                     size_t *read_length)
{
    pppd_state_t *state = ctx;
    const esp_err_t ret = solar_os_port_read(&state->port,
                                             data,
                                             length,
                                             timeout_ms,
                                             read_length);
    if (ret == ESP_OK && read_length != NULL) {
        state->rx_bytes += (uint32_t)*read_length;
    } else if (ret != ESP_ERR_TIMEOUT) {
        state->rx_errors++;
        state->last_error = ret;
    }
    return ret;
}

static esp_err_t pppd_transport_write(void *ctx,
                                      const uint8_t *data,
                                      size_t length,
                                      size_t *written)
{
    pppd_state_t *state = ctx;
    size_t offset = 0U;
    if (written != NULL) {
        *written = 0U;
    }
    while (!state->stop_requested && offset < length) {
        size_t chunk = 0U;
        const esp_err_t ret = solar_os_port_write(&state->port,
                                                  data + offset,
                                                  length - offset,
                                                  &chunk);
        if (ret != ESP_OK || chunk == 0U) {
            state->tx_errors++;
            state->last_error = ret == ESP_OK ? ESP_FAIL : ret;
            return state->last_error;
        }
        offset += chunk;
    }
    state->tx_bytes += (uint32_t)offset;
    if (written != NULL) {
        *written = offset;
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t pppd_netif_attach(void *ctx, esp_netif_t *netif)
{
    pppd_state_t *state = ctx;
    state->netif = netif;
    if (state->config.role == PPPD_ROLE_UPLINK) {
        esp_err_t ret = solar_os_network_path_register(state->path_name,
                                                       netif,
                                                       state->config.priority);
        if (ret == ESP_OK) {
            ret = solar_os_network_path_set_connecting(netif, true);
        }
        return ret;
    }
    if (esp_netif_set_route_prio(netif, PPPD_DOWNSTREAM_ROUTE_PRIORITY) !=
        PPPD_DOWNSTREAM_ROUTE_PRIORITY) {
        state->netif = NULL;
        return ESP_FAIL;
    }
    return pppd_publish_interface(SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING,
                                  false,
                                  ESP_OK);
}

static void pppd_netif_set_ready(void *ctx,
                                 esp_netif_t *netif,
                                 bool ready)
{
    pppd_state_t *state = ctx;
    if (state->config.role == PPPD_ROLE_DOWNSTREAM) {
        esp_err_t ret = ESP_OK;
        if (ready && !state->nat_active) {
            ret = esp_netif_napt_enable(netif);
            if (ret == ESP_OK) {
                state->nat_active = true;
            }
        } else if (!ready && state->nat_active) {
            ret = esp_netif_napt_disable(netif);
            if (ret == ESP_OK) {
                state->nat_active = false;
            }
        }
        if (ret != ESP_OK) {
            state->last_error = ret;
            SOLAR_OS_LOGW(TAG,
                          "%s NAPT %s failed: %s",
                          state->path_name,
                          ready ? "enable" : "disable",
                          esp_err_to_name(ret));
        }
        const esp_err_t publish_ret = pppd_publish_interface(
            ready ? SOLAR_OS_NETWORK_INTERFACE_STATE_UP :
                    SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING,
            state->nat_active,
            ret);
        if (publish_ret != ESP_OK) {
            state->last_error = publish_ret;
        }
        return;
    }
    if (state->config.role == PPPD_ROLE_PEER) {
        const esp_err_t ret = pppd_publish_interface(
            ready ? SOLAR_OS_NETWORK_INTERFACE_STATE_UP :
                    SOLAR_OS_NETWORK_INTERFACE_STATE_CONNECTING,
            false,
            ESP_OK);
        if (ret != ESP_OK) {
            state->last_error = ret;
        }
        return;
    }
    const esp_err_t ret = solar_os_network_path_set_ready(netif, ready);
    if (ret != ESP_OK) {
        state->last_error = ret;
        SOLAR_OS_LOGW(TAG,
                      "%s path ready=%s failed: %s",
                      state->path_name,
                      ready ? "yes" : "no",
                      esp_err_to_name(ret));
    } else if (!ready && !state->stop_requested) {
        const esp_err_t connecting_ret =
            solar_os_network_path_set_connecting(netif, true);
        if (connecting_ret != ESP_OK) {
            state->last_error = connecting_ret;
        }
    }
}

static void pppd_netif_detach(void *ctx, esp_netif_t *netif)
{
    pppd_state_t *state = ctx;
    if (state->config.role == PPPD_ROLE_UPLINK) {
        const esp_err_t ret = solar_os_network_path_unregister(netif);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            state->last_error = ret;
        }
    } else if (state->nat_active) {
        const esp_err_t ret = esp_netif_napt_disable(netif);
        if (ret != ESP_OK) {
            state->last_error = ret;
        }
        state->nat_active = false;
    }
    if (state->config.role != PPPD_ROLE_UPLINK) {
        (void)solar_os_network_interface_remove(state->path_name);
    }
    state->netif = NULL;
}

static esp_ip4_addr_t pppd_resolve_dns(const pppd_config_t *config)
{
    if (config->dns_mode == PPPD_DNS_ADDRESS) {
        return config->dns_address;
    }
    esp_ip4_addr_t dns = {0};
    if (config->dns_mode != PPPD_DNS_AUTO) {
        return dns;
    }
    solar_os_network_path_info_t path;
    if (solar_os_network_path_get_preferred(&path) &&
        path.dns.ip.type == ESP_IPADDR_TYPE_V4) {
        dns = path.dns.ip.u_addr.ip4;
    }
    return dns;
}

static void pppd_cleanup(void)
{
    if (pppd_job.ppp != NULL) {
        const esp_err_t disconnect_ret = solar_os_ppp_disconnect(pppd_job.ppp);
        if (disconnect_ret != ESP_OK) {
            pppd_job.last_error = disconnect_ret;
        }
        const esp_err_t destroy_ret = solar_os_ppp_destroy(pppd_job.ppp);
        if (destroy_ret == ESP_OK) {
            pppd_job.ppp = NULL;
        } else {
            pppd_job.last_error = destroy_ret;
        }
    }
    if (pppd_has_published_interface()) {
        (void)solar_os_network_interface_remove(pppd_job.path_name);
    }
    if (solar_os_port_handle_valid(&pppd_job.port)) {
        (void)solar_os_port_release(&pppd_job.port);
    }
    pppd_job.running = false;
    pppd_job.stop_requested = false;
    pppd_job.task = NULL;
}

static void pppd_task(void *arg)
{
    (void)arg;
    bool was_up = false;
    while (!pppd_job.stop_requested) {
        solar_os_ppp_status_t status = {0};
        esp_err_t ret = solar_os_ppp_get_status(pppd_job.ppp, &status);
        if (ret != ESP_OK) {
            pppd_job.last_error = ret;
            break;
        }
        if (status.state == SOLAR_OS_PPP_STATE_UP) {
            if (!was_up) {
                SOLAR_OS_LOGI(TAG,
                              "%s up: interface=%s address=%s",
                              pppd_job.path_name,
                              status.interface_name,
                              status.ipv4_address);
                was_up = true;
            }
        } else if (status.state == SOLAR_OS_PPP_STATE_FAILED ||
                   status.state == SOLAR_OS_PPP_STATE_DOWN) {
            pppd_job.last_ppp_error = status.error;
            (void)solar_os_ppp_disconnect(pppd_job.ppp);
            if (pppd_job.stop_requested) {
                break;
            }
            pppd_job.reconnects++;
            was_up = false;
            vTaskDelay(pdMS_TO_TICKS(PPPD_RETRY_DELAY_MS));
            ret = solar_os_ppp_start(pppd_job.ppp, &pppd_job.profile);
            if (ret != ESP_OK) {
                pppd_job.last_error = ret;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PPPD_STATUS_POLL_MS));
    }
    pppd_cleanup();
    solar_os_task_delete_external(NULL);
}

static esp_err_t pppd_job_start(solar_os_context_t *ctx,
                                int argc,
                                char **argv)
{
    (void)ctx;
    pppd_config_t config;
    if (!pppd_parse_args(argc, argv, &config)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pppd_job.running || pppd_job.task != NULL || pppd_job.ppp != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = pppd_validate_and_configure_port(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_jobs_claim_port(solar_os_pppd_job.name,
                                   config.port_name,
                                   &pppd_job.port);
    if (ret != ESP_OK) {
        return ret;
    }

    pppd_job.running = true;
    pppd_job.stop_requested = false;
    pppd_job.config = config;
    pppd_job.rx_bytes = 0U;
    pppd_job.tx_bytes = 0U;
    pppd_job.rx_errors = 0U;
    pppd_job.tx_errors = 0U;
    pppd_job.reconnects = 0U;
    pppd_job.last_ppp_error = 0;
    pppd_job.last_error = ESP_OK;
    pppd_job.nat_active = false;
    pppd_job.netif = NULL;
    (void)snprintf(pppd_job.path_name,
                   sizeof(pppd_job.path_name),
                   "ppp-%s",
                   config.port_name);

    const esp_ip4_addr_t dns = pppd_resolve_dns(&config);
    solar_os_ppp_profile_t profile = {
        .auth = SOLAR_OS_PPP_AUTH_NONE,
    };
    solar_os_ppp_link_t link = {
        .mode = config.mode,
        .local_address = config.local_address,
        .peer_address = config.peer_address,
    };
    if (config.role == PPPD_ROLE_UPLINK) {
        if (config.dns_mode == PPPD_DNS_ADDRESS) {
            (void)snprintf(profile.dns,
                           sizeof(profile.dns),
                           IPSTR,
                           IP2STR(&config.dns_address));
        }
    } else {
        link.primary_dns = dns;
    }
    pppd_job.profile = profile;

    const solar_os_ppp_config_t ppp_config = {
        .name = pppd_job.path_name,
        .read_timeout_ms = PPPD_READ_TIMEOUT_MS,
        .transport = {
            .read = pppd_transport_read,
            .write = pppd_transport_write,
            .ctx = &pppd_job,
        },
        .netif = {
            .attach = pppd_netif_attach,
            .set_ready = pppd_netif_set_ready,
            .detach = pppd_netif_detach,
            .ctx = &pppd_job,
        },
        .link = link,
    };
    ret = solar_os_ppp_create(&ppp_config, &pppd_job.ppp);
    if (ret == ESP_OK) {
        ret = solar_os_ppp_start(pppd_job.ppp, &profile);
    }
    if (ret != ESP_OK) {
        pppd_cleanup();
        return ret;
    }

    char resource[64];
    (void)snprintf(resource,
                   sizeof(resource),
                   "%s:%s",
                   pppd_job.path_name,
                   pppd_role_name(config.role));
    (void)solar_os_jobs_note_resource(solar_os_pppd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      resource,
                                      pppd_mode_name(config.mode));

    if (solar_os_task_create_pinned_external(pppd_task,
                                             "pppd_job",
                                             PPPD_TASK_STACK,
                                             NULL,
                                             PPPD_TASK_PRIORITY,
                                             &pppd_job.task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        pppd_cleanup();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void pppd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!pppd_job.running && pppd_job.task == NULL) {
        return;
    }
    pppd_job.stop_requested = true;
    if (pppd_job.task != NULL && pppd_job.task != xTaskGetCurrentTaskHandle()) {
        const uint32_t iterations = PPPD_STOP_WAIT_MS / 25U;
        for (uint32_t i = 0U; i < iterations && pppd_job.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(25U));
        }
    }
}

static void pppd_job_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (!pppd_job.running || pppd_job.ppp == NULL) {
        solar_os_shell_io_writeln(io, "  pppd: inactive");
        return;
    }
    solar_os_ppp_status_t status = {0};
    (void)solar_os_ppp_get_status(pppd_job.ppp, &status);
    if (pppd_job.config.baud_explicit) {
        solar_os_shell_io_printf(io,
                                 "  pppd: port=%s baud=%" PRIu32
                                 " mode=%s role=%s state=%s\n",
                                 pppd_job.config.port_name,
                                 pppd_job.config.baud_rate,
                                 pppd_mode_name(pppd_job.config.mode),
                                 pppd_role_name(pppd_job.config.role),
                                 pppd_state_name(status.state));
    } else {
        solar_os_shell_io_printf(io,
                                 "  pppd: port=%s baud=current"
                                 " mode=%s role=%s state=%s\n",
                                 pppd_job.config.port_name,
                                 pppd_mode_name(pppd_job.config.mode),
                                 pppd_role_name(pppd_job.config.role),
                                 pppd_state_name(status.state));
    }
    solar_os_shell_io_printf(io,
                             "  link: interface=%s address=%s peer=%s dns=%s"
                             " nat=%s priority=%d\n",
                             status.interface_name[0] != '\0'
                                 ? status.interface_name : "-",
                             status.ipv4_address[0] != '\0'
                                 ? status.ipv4_address : "-",
                             status.ipv4_gateway[0] != '\0'
                                 ? status.ipv4_gateway : "-",
                             status.dns_address[0] != '\0'
                                 ? status.dns_address : "-",
                             pppd_job.nat_active ? "on" : "off",
                             pppd_job.config.role == PPPD_ROLE_UPLINK
                                 ? pppd_job.config.priority
                                 : PPPD_DOWNSTREAM_ROUTE_PRIORITY);
    solar_os_shell_io_printf(io,
                             "  traffic: rx=%" PRIu32 " tx=%" PRIu32
                             " rx-errors=%" PRIu32 " tx-errors=%" PRIu32
                             " reconnects=%" PRIu32 " ppp-error=%" PRId32
                             " error=%s\n",
                             pppd_job.rx_bytes,
                             pppd_job.tx_bytes,
                             pppd_job.rx_errors,
                             pppd_job.tx_errors,
                             pppd_job.reconnects,
                             pppd_job.last_ppp_error,
                             esp_err_to_name(pppd_job.last_error));
}

const solar_os_job_t solar_os_pppd_job = {
    .name = "pppd",
    .summary = "PPP link on a byte-stream port",
    .start = pppd_job_start,
    .stop = pppd_job_stop,
    .worker_stack_bytes = PPPD_TASK_STACK,
    .worker_stack_external = true,
    .detail = pppd_job_detail,
};
