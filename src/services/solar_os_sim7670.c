#include "solar_os_sim7670.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_gnss.h"
#include "solar_os_modem.h"
#include "solar_os_task.h"

#if CONFIG_LWIP_PPP_SUPPORT
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "lwip/ip4_addr.h"
#endif

#define SIM7670_DEVICE_MAX 2U
#define SIM7670_PPP_RX_BUFFER_SIZE 768U
#define SIM7670_PPP_TASK_STACK 4096U
#define SIM7670_PPP_TASK_PRIORITY 8U
#define SIM7670_PPP_READ_TIMEOUT_MS 100U
#define SIM7670_PPP_CONNECT_TIMEOUT_MS 65000U
#define SIM7670_PPP_STOP_TIMEOUT_MS 5000U
#define SIM7670_DATA_ESCAPE_GUARD_MS 1100U

#define SIM7670_PPP_EVENT_GOT_IP BIT0
#define SIM7670_PPP_EVENT_LOST_IP BIT1
#define SIM7670_PPP_EVENT_ERROR BIT2
#define SIM7670_PPP_EVENT_PHASE_DEAD BIT3

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    bool gnss_powered;
    sim7670_t modem;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    sim7670_status_t cached_status;
    bool cached_status_valid;
#if CONFIG_LWIP_PPP_SUPPORT
    solar_os_modem_profile_t applied_profile;
    bool applied_profile_valid;
    esp_netif_t *ppp_netif;
    esp_netif_driver_ifconfig_t ppp_driver;
    esp_event_handler_instance_t ppp_got_ip_handler;
    esp_event_handler_instance_t ppp_lost_ip_handler;
    esp_event_handler_instance_t ppp_status_handler;
    EventGroupHandle_t ppp_events;
    StaticEventGroup_t ppp_events_storage;
    TaskHandle_t ppp_worker;
    volatile bool ppp_worker_stop_requested;
    volatile bool ppp_worker_done;
    bool ppp_mode;
    bool ppp_transitioning;
    solar_os_modem_network_state_t network_state;
    int32_t ppp_error;
    char ppp_if_key[24];
    char ppp_if_desc[24];
    char ppp_if_name[8];
    char ppp_ipv4[SOLAR_OS_MODEM_ADDRESS_MAX];
    char ppp_gateway[SOLAR_OS_MODEM_ADDRESS_MAX];
    char ppp_dns[SOLAR_OS_MODEM_ADDRESS_MAX];
#endif
} solar_os_sim7670_device_t;

static const char *TAG = "sim7670";
static solar_os_sim7670_device_t devices[SIM7670_DEVICE_MAX];

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len)
{
    bool have_uart = false;
    if (bindings == NULL || uart_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_bus[0] = '\0';
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT && !have_uart) {
            strlcpy(uart_bus, binding->target, uart_bus_len);
            have_uart = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t modem_write(void *user,
                             const uint8_t *data,
                             size_t len,
                             size_t *written)
{
    solar_os_sim7670_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_uart_write(device->uart_bus, data, len, written);
}

static esp_err_t modem_read(void *user,
                            uint8_t *data,
                            size_t len,
                            uint32_t timeout_ms,
                            size_t *read_len)
{
    solar_os_sim7670_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_bus_uart_read(device->uart_bus,
                                  data,
                                  len,
                                  timeout_ms,
                                  read_len);
}

static solar_os_sim7670_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

#if CONFIG_LWIP_PPP_SUPPORT
static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static void format_ipv4(const esp_ip4_addr_t *address,
                        char *buffer,
                        size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    if (address == NULL || address->addr == 0U) {
        buffer[0] = '\0';
        return;
    }
    (void)snprintf(buffer, buffer_size, IPSTR, IP2STR(address));
}

static esp_err_t ppp_transmit(void *handle, void *buffer, size_t length)
{
    solar_os_sim7670_device_t *device = handle;
    if (device == NULL || buffer == NULL || length == 0U ||
        !device->active || !device->ppp_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t written = 0U;
    const esp_err_t ret = solar_os_bus_uart_write(device->uart_bus,
                                                   buffer,
                                                   length,
                                                   &written);
    return ret == ESP_OK && written == length ? ESP_OK :
        (ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret);
}

static void ppp_event_handler(void *arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    solar_os_sim7670_device_t *device = arg;
    if (device == NULL || !device->active || device->ppp_netif == NULL) {
        return;
    }

    EventBits_t bits = 0U;
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_PPP_GOT_IP) {
            const ip_event_got_ip_t *event = event_data;
            if (event == NULL || event->esp_netif != device->ppp_netif) {
                return;
            }
            esp_netif_dns_info_t dns = {0};
            (void)esp_netif_get_dns_info(device->ppp_netif,
                                         ESP_NETIF_DNS_MAIN,
                                         &dns);
            char dns_override[SOLAR_OS_MODEM_DNS_MAX + 1U] = {0};
            xSemaphoreTake(device->mutex, portMAX_DELAY);
            if (device->applied_profile_valid) {
                strlcpy(dns_override,
                        device->applied_profile.dns,
                        sizeof(dns_override));
            }
            xSemaphoreGive(device->mutex);
            /* Apply negotiated DNS after GOT_IP so lwIP's active resolver
             * follows the newly selected PPP interface. */
            if (dns.ip.type == ESP_IPADDR_TYPE_V4 &&
                dns.ip.u_addr.ip4.addr != 0U) {
                const esp_err_t dns_ret = esp_netif_set_dns_info(
                    device->ppp_netif,
                    ESP_NETIF_DNS_MAIN,
                    &dns);
                if (dns_ret != ESP_OK) {
                    ESP_LOGW(TAG,
                             "%s failed to activate PPP DNS: %s",
                             device->name,
                             esp_err_to_name(dns_ret));
                }
            }
            if (dns_override[0] != '\0') {
                esp_netif_dns_info_t override_dns = {
                    .ip.type = ESP_IPADDR_TYPE_V4,
                };
                if (esp_netif_str_to_ip4(
                        dns_override,
                        &override_dns.ip.u_addr.ip4) == ESP_OK) {
                    const esp_err_t override_ret = esp_netif_set_dns_info(
                        device->ppp_netif,
                        ESP_NETIF_DNS_MAIN,
                        &override_dns);
                    if (override_ret == ESP_OK) {
                        dns = override_dns;
                    } else {
                        ESP_LOGW(TAG,
                                 "%s failed to activate profile DNS: %s",
                                 device->name,
                                 esp_err_to_name(override_ret));
                    }
                } else {
                    ESP_LOGW(TAG,
                             "%s has invalid profile DNS",
                             device->name);
                }
            }
#if CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
            if (esp_netif_get_default_netif() == device->ppp_netif &&
                dns.ip.type == ESP_IPADDR_TYPE_V4 &&
                dns.ip.u_addr.ip4.addr != 0U) {
                const esp_err_t global_dns_ret = esp_netif_set_dns_info(
                    NULL,
                    ESP_NETIF_DNS_MAIN,
                    &dns);
                if (global_dns_ret != ESP_OK) {
                    ESP_LOGW(TAG,
                             "%s failed to synchronize active DNS: %s",
                             device->name,
                             esp_err_to_name(global_dns_ret));
                }
            }
#endif
            xSemaphoreTake(device->mutex, portMAX_DELAY);
            device->network_state = SOLAR_OS_MODEM_NETWORK_UP;
            device->ppp_error = 0;
            format_ipv4(&event->ip_info.ip,
                        device->ppp_ipv4,
                        sizeof(device->ppp_ipv4));
            format_ipv4(&event->ip_info.gw,
                        device->ppp_gateway,
                        sizeof(device->ppp_gateway));
            format_ipv4(&dns.ip.u_addr.ip4,
                        device->ppp_dns,
                        sizeof(device->ppp_dns));
            (void)esp_netif_get_netif_impl_name(device->ppp_netif,
                                                device->ppp_if_name);
            xSemaphoreGive(device->mutex);
            bits = SIM7670_PPP_EVENT_GOT_IP;
        } else if (event_id == IP_EVENT_PPP_LOST_IP) {
            const ip_event_got_ip_t *event = event_data;
            if (event == NULL || event->esp_netif != device->ppp_netif) {
                return;
            }
            xSemaphoreTake(device->mutex, portMAX_DELAY);
            if (device->ppp_mode && !device->ppp_transitioning) {
                device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
            }
            device->ppp_ipv4[0] = '\0';
            device->ppp_gateway[0] = '\0';
            device->ppp_dns[0] = '\0';
            xSemaphoreGive(device->mutex);
            bits = SIM7670_PPP_EVENT_LOST_IP;
        }
    } else if (event_base == NETIF_PPP_STATUS) {
        const esp_netif_t *netif = event_data != NULL
            ? *(esp_netif_t * const *)event_data
            : NULL;
        if (netif != device->ppp_netif) {
            return;
        }
        xSemaphoreTake(device->mutex, portMAX_DELAY);
        if (event_id == NETIF_PPP_PHASE_DEAD) {
            bits = SIM7670_PPP_EVENT_PHASE_DEAD;
        } else if (event_id > NETIF_PPP_ERRORNONE &&
                   event_id < NETIF_PP_PHASE_OFFSET &&
                   device->ppp_mode && !device->ppp_transitioning) {
            device->ppp_error = event_id;
            device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
            bits = SIM7670_PPP_EVENT_ERROR;
        }
        xSemaphoreGive(device->mutex);
    }
    if (bits != 0U && device->ppp_events != NULL) {
        xEventGroupSetBits(device->ppp_events, bits);
    }
}

static void ppp_worker(void *arg)
{
    solar_os_sim7670_device_t *device = arg;
    uint8_t buffer[SIM7670_PPP_RX_BUFFER_SIZE];
    while (!device->ppp_worker_stop_requested) {
        size_t received = 0U;
        const esp_err_t ret = solar_os_bus_uart_read(
            device->uart_bus,
            buffer,
            sizeof(buffer),
            SIM7670_PPP_READ_TIMEOUT_MS,
            &received);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            xSemaphoreTake(device->mutex, portMAX_DELAY);
            device->ppp_error = ret;
            device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
            xSemaphoreGive(device->mutex);
            xEventGroupSetBits(device->ppp_events,
                               SIM7670_PPP_EVENT_ERROR);
            break;
        }
        if (received > 0U &&
            esp_netif_receive(device->ppp_netif,
                              buffer,
                              received,
                              NULL) != ESP_OK) {
            xSemaphoreTake(device->mutex, portMAX_DELAY);
            device->ppp_error = ESP_FAIL;
            device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
            xSemaphoreGive(device->mutex);
            xEventGroupSetBits(device->ppp_events,
                               SIM7670_PPP_EVENT_ERROR);
            break;
        }
    }
    device->ppp_worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void ppp_runtime_destroy(solar_os_sim7670_device_t *device)
{
    if (device->ppp_status_handler != NULL) {
        (void)esp_event_handler_instance_unregister(
            NETIF_PPP_STATUS,
            ESP_EVENT_ANY_ID,
            device->ppp_status_handler);
        device->ppp_status_handler = NULL;
    }
    if (device->ppp_lost_ip_handler != NULL) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_PPP_LOST_IP,
            device->ppp_lost_ip_handler);
        device->ppp_lost_ip_handler = NULL;
    }
    if (device->ppp_got_ip_handler != NULL) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_PPP_GOT_IP,
            device->ppp_got_ip_handler);
        device->ppp_got_ip_handler = NULL;
    }
    if (device->ppp_netif != NULL) {
        esp_netif_destroy(device->ppp_netif);
        device->ppp_netif = NULL;
    }
}

static esp_err_t ppp_runtime_init_locked(solar_os_sim7670_device_t *device)
{
    if (device->ppp_netif != NULL) {
        return ESP_OK;
    }
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    (void)snprintf(device->ppp_if_key,
                   sizeof(device->ppp_if_key),
                   "PPP_%s",
                   device->name);
    (void)snprintf(device->ppp_if_desc,
                   sizeof(device->ppp_if_desc),
                   "ppp-%s",
                   device->name);
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base.if_key = device->ppp_if_key;
    base.if_desc = device->ppp_if_desc;
    /* An explicit modem connection becomes the preferred default route while
     * source-address routing keeps established Wi-Fi sessions on Wi-Fi. */
    base.route_prio = 110;
    device->ppp_driver = (esp_netif_driver_ifconfig_t) {
        .handle = device,
        .transmit = ppp_transmit,
    };
    const esp_netif_config_t config = {
        .base = &base,
        .driver = &device->ppp_driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };
    device->ppp_netif = esp_netif_new(&config);
    if (device->ppp_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_netif_ppp_config_t ppp_config = {
        .ppp_phase_event_enabled = true,
        .ppp_error_event_enabled = true,
    };
    ret = esp_netif_ppp_set_params(device->ppp_netif, &ppp_config);
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_PPP_GOT_IP,
            ppp_event_handler,
            device,
            &device->ppp_got_ip_handler);
    }
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_PPP_LOST_IP,
            ppp_event_handler,
            device,
            &device->ppp_lost_ip_handler);
    }
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(
            NETIF_PPP_STATUS,
            ESP_EVENT_ANY_ID,
            ppp_event_handler,
            device,
            &device->ppp_status_handler);
    }
    if (ret != ESP_OK) {
        ppp_runtime_destroy(device);
    }
    return ret;
}

static esp_err_t ppp_configure_auth_locked(
    solar_os_sim7670_device_t *device)
{
    if (!device->applied_profile_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_auth_type_t auth = NETIF_PPP_AUTHTYPE_NONE;
    if (device->applied_profile.auth == SOLAR_OS_MODEM_AUTH_PAP) {
        auth = NETIF_PPP_AUTHTYPE_PAP;
    } else if (device->applied_profile.auth == SOLAR_OS_MODEM_AUTH_CHAP) {
        auth = NETIF_PPP_AUTHTYPE_CHAP;
    }
    return esp_netif_ppp_set_auth(device->ppp_netif,
                                  auth,
                                  device->applied_profile.username,
                                  device->applied_profile.password);
}

static esp_err_t ppp_start_locked(solar_os_sim7670_device_t *device)
{
    if (device->ppp_mode || device->ppp_transitioning) {
        return device->network_state == SOLAR_OS_MODEM_NETWORK_UP
            ? ESP_OK
            : ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ppp_runtime_init_locked(device);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ppp_configure_auth_locked(device);
    if (ret != ESP_OK) {
        return ret;
    }
    xEventGroupClearBits(device->ppp_events,
                         SIM7670_PPP_EVENT_GOT_IP |
                         SIM7670_PPP_EVENT_LOST_IP |
                         SIM7670_PPP_EVENT_ERROR |
                         SIM7670_PPP_EVENT_PHASE_DEAD);
    device->network_state = SOLAR_OS_MODEM_NETWORK_CONNECTING;
    device->ppp_error = 0;
    device->ppp_ipv4[0] = '\0';
    device->ppp_gateway[0] = '\0';
    device->ppp_dns[0] = '\0';
    device->ppp_transitioning = true;
    ret = sim7670_set_packet_attached(&device->modem, true);
    if (ret == ESP_OK) {
        ret = sim7670_read_status(&device->modem, &device->cached_status);
        device->cached_status_valid = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = sim7670_enter_data_mode(&device->modem);
    }
    if (ret != ESP_OK) {
        device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
        device->ppp_transitioning = false;
        return ret;
    }

    device->ppp_mode = true;
    if (device->cached_status_valid) {
        device->cached_status.data_status_valid = true;
        device->cached_status.data_active = true;
    }
    device->ppp_worker_stop_requested = false;
    device->ppp_worker_done = false;
    if (solar_os_task_create_pinned_internal(
            ppp_worker,
            "sim7670_ppp",
            SIM7670_PPP_TASK_STACK,
            device,
            SIM7670_PPP_TASK_PRIORITY,
            &device->ppp_worker,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        device->ppp_worker = NULL;
        device->ppp_worker_done = true;
        device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
        device->ppp_transitioning = false;
        return ESP_ERR_NO_MEM;
    }
    esp_netif_action_start(device->ppp_netif, 0, 0, NULL);
    esp_netif_action_connected(device->ppp_netif, 0, 0, NULL);
    device->ppp_transitioning = false;
    return ESP_OK;
}

static esp_err_t escape_data_mode(solar_os_sim7670_device_t *device)
{
    vTaskDelay(pdMS_TO_TICKS(SIM7670_DATA_ESCAPE_GUARD_MS));
    static const uint8_t escape[] = {'+', '+', '+'};
    size_t written = 0U;
    esp_err_t ret = solar_os_bus_uart_write(device->uart_bus,
                                            escape,
                                            sizeof(escape),
                                            &written);
    if (ret != ESP_OK || written != sizeof(escape)) {
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }
    vTaskDelay(pdMS_TO_TICKS(SIM7670_DATA_ESCAPE_GUARD_MS));
    char response[128];
    ret = sim7670_command(&device->modem,
                          "AT",
                          3000U,
                          response,
                          sizeof(response));
    if (ret != ESP_OK) {
        return ret;
    }
    return sim7670_command(&device->modem,
                           "ATH",
                           50000U,
                           response,
                           sizeof(response));
}

static esp_err_t ppp_stop(solar_os_sim7670_device_t *device)
{
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (device->ppp_transitioning) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!device->ppp_mode) {
        device->network_state = SOLAR_OS_MODEM_NETWORK_DOWN;
        xSemaphoreGive(device->mutex);
        return ESP_OK;
    }
    device->ppp_transitioning = true;
    xEventGroupClearBits(device->ppp_events,
                         SIM7670_PPP_EVENT_LOST_IP |
                         SIM7670_PPP_EVENT_PHASE_DEAD);
    esp_netif_t *netif = device->ppp_netif;
    TaskHandle_t worker = device->ppp_worker;
    xSemaphoreGive(device->mutex);

    esp_netif_action_disconnected(netif, 0, 0, NULL);
    esp_netif_action_stop(netif, 0, 0, NULL);
    (void)xEventGroupWaitBits(device->ppp_events,
                              SIM7670_PPP_EVENT_LOST_IP |
                              SIM7670_PPP_EVENT_PHASE_DEAD,
                              pdFALSE,
                              pdFALSE,
                              pdMS_TO_TICKS(SIM7670_PPP_STOP_TIMEOUT_MS));

    device->ppp_worker_stop_requested = true;
    if (worker != NULL &&
        !solar_os_task_wait_done(worker,
                                 &device->ppp_worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        xSemaphoreTake(device->mutex, portMAX_DELAY);
        device->network_state = SOLAR_OS_MODEM_NETWORK_FAILED;
        device->ppp_transitioning = false;
        xSemaphoreGive(device->mutex);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = escape_data_mode(device);
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    device->ppp_worker = NULL;
    device->ppp_worker_stop_requested = false;
    device->ppp_worker_done = false;
    if (ret == ESP_OK) {
        device->ppp_mode = false;
        sim7670_status_t status;
        if (sim7670_read_status(&device->modem, &status) == ESP_OK) {
            device->cached_status = status;
            device->cached_status_valid = true;
        } else if (device->cached_status_valid) {
            device->cached_status.data_status_valid = true;
            device->cached_status.data_active = false;
        }
    }
    device->network_state = ret == ESP_OK
        ? SOLAR_OS_MODEM_NETWORK_DOWN
        : SOLAR_OS_MODEM_NETWORK_FAILED;
    device->ppp_ipv4[0] = '\0';
    device->ppp_gateway[0] = '\0';
    device->ppp_dns[0] = '\0';
    device->ppp_transitioning = false;
    secure_zero(&device->applied_profile,
                sizeof(device->applied_profile));
    device->applied_profile_valid = false;
    xSemaphoreGive(device->mutex);
    return ret;
}
#endif

static solar_os_modem_network_registration_t map_registration(
    sim7670_registration_t registration)
{
    switch (registration) {
    case SIM7670_REGISTRATION_NOT_REGISTERED:
        return SOLAR_OS_MODEM_REGISTRATION_NOT_REGISTERED;
    case SIM7670_REGISTRATION_SEARCHING:
        return SOLAR_OS_MODEM_REGISTRATION_SEARCHING;
    case SIM7670_REGISTRATION_DENIED:
        return SOLAR_OS_MODEM_REGISTRATION_DENIED;
    case SIM7670_REGISTRATION_HOME:
        return SOLAR_OS_MODEM_REGISTRATION_HOME;
    case SIM7670_REGISTRATION_ROAMING:
        return SOLAR_OS_MODEM_REGISTRATION_ROAMING;
    case SIM7670_REGISTRATION_UNKNOWN:
    default:
        return SOLAR_OS_MODEM_REGISTRATION_UNKNOWN;
    }
}

static esp_err_t modem_get_status(void *ctx, solar_os_modem_status_t *status)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    sim7670_status_t modem_status = {0};
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        if (device->cached_status_valid) {
            modem_status = device->cached_status;
        } else {
            modem_status.online = true;
            modem_status.data_status_valid = true;
            modem_status.data_active = device->ppp_mode;
        }
    } else
#endif
    {
        ret = sim7670_read_status(&device->modem, &modem_status);
        if (ret == ESP_OK) {
            device->cached_status = modem_status;
            device->cached_status_valid = true;
        }
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (ret == ESP_OK) {
        modem_status.data_status_valid = true;
        modem_status.data_active = device->ppp_mode;
    }
#endif
    if (ret == ESP_OK) {
        *status = (solar_os_modem_status_t) {
            .online = modem_status.online,
            .sim_status_valid = modem_status.sim_status_valid,
            .sim_ready = modem_status.sim_ready,
            .registration_status_valid =
                modem_status.registration_status_valid,
            .registration = map_registration(modem_status.registration),
            .signal_status_valid = modem_status.signal_status_valid,
            .rssi_valid = modem_status.rssi_valid,
            .rssi_dbm = modem_status.rssi_dbm,
            .bit_error_rate = modem_status.bit_error_rate,
            .data_status_valid = modem_status.data_status_valid,
            .data_active = modem_status.data_active,
        };
#if CONFIG_LWIP_PPP_SUPPORT
        status->network_status_valid = true;
        status->network_state = device->network_state;
        strlcpy(status->network_interface,
                device->ppp_if_name,
                sizeof(status->network_interface));
        strlcpy(status->ipv4_address,
                device->ppp_ipv4,
                sizeof(status->ipv4_address));
        strlcpy(status->ipv4_gateway,
                device->ppp_gateway,
                sizeof(status->ipv4_gateway));
        strlcpy(status->dns_address,
                device->ppp_dns,
                sizeof(status->dns_address));
#endif
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_apply_profile(
    void *ctx,
    const solar_os_modem_profile_t *profile)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sim7670_pdp_type_t pdp_type;
    switch (profile->ip_type) {
    case SOLAR_OS_MODEM_IP_IPV4:
        pdp_type = SIM7670_PDP_IPV4;
        break;
    case SOLAR_OS_MODEM_IP_IPV6:
        pdp_type = SIM7670_PDP_IPV6;
        break;
    case SOLAR_OS_MODEM_IP_IPV4V6:
        pdp_type = SIM7670_PDP_IPV4V6;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    sim7670_auth_t auth;
    switch (profile->auth) {
    case SOLAR_OS_MODEM_AUTH_NONE:
        auth = SIM7670_AUTH_NONE;
        break;
    case SOLAR_OS_MODEM_AUTH_PAP:
        auth = SIM7670_AUTH_PAP;
        break;
    case SOLAR_OS_MODEM_AUTH_CHAP:
        auth = SIM7670_AUTH_CHAP;
        break;
    case SOLAR_OS_MODEM_AUTH_AUTO:
        auth = SIM7670_AUTH_AUTO;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_configure_pdp(&device->modem,
                                    profile->apn,
                                    pdp_type,
                                    auth,
                                    profile->username,
                                    profile->password);
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (ret == ESP_OK) {
        secure_zero(&device->applied_profile,
                    sizeof(device->applied_profile));
        device->applied_profile = *profile;
        device->applied_profile_valid = true;
    }
#endif
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_clear_profile(void *ctx)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_clear_pdp(&device->modem);
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (ret == ESP_OK) {
        secure_zero(&device->applied_profile,
                    sizeof(device->applied_profile));
        device->applied_profile_valid = false;
    }
#endif
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_set_data_active(void *ctx, bool active)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (!active) {
        return ppp_stop(device);
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ppp_start_locked(device);
    const bool needs_cleanup = ret != ESP_OK && device->ppp_mode;
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) {
        if (needs_cleanup) {
            (void)ppp_stop(device);
        }
        return ret;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        device->ppp_events,
        SIM7670_PPP_EVENT_GOT_IP | SIM7670_PPP_EVENT_ERROR,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(SIM7670_PPP_CONNECT_TIMEOUT_MS));
    if ((bits & SIM7670_PPP_EVENT_GOT_IP) != 0U) {
        return ESP_OK;
    }
    ret = (bits & SIM7670_PPP_EVENT_ERROR) != 0U
        ? ESP_FAIL
        : ESP_ERR_TIMEOUT;
    const esp_err_t stop_ret = ppp_stop(device);
    return stop_ret == ESP_OK ? ret : stop_ret;
#else
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = sim7670_set_pdp_active(&device->modem, active);
    xSemaphoreGive(device->mutex);
    return ret;
#endif
}

static esp_err_t modem_unlock_sim(void *ctx, const char *pin)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_unlock_sim(&device->modem, pin);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t modem_command(void *ctx,
                               const char *command,
                               uint32_t timeout_ms,
                               char *response,
                               size_t response_size)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_command(&device->modem,
                              command,
                              timeout_ms,
                              response,
                              response_size);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_modem_ops_t modem_ops = {
    .get_status = modem_get_status,
    .apply_profile = modem_apply_profile,
    .clear_profile = modem_clear_profile,
    .set_data_active = modem_set_data_active,
    .unlock_sim = modem_unlock_sim,
    .command = modem_command,
};

static esp_err_t gnss_read_fix(void *ctx,
                               uint32_t timeout_ms,
                               solar_os_gnss_fix_t *fix)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active || fix == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (!device->gnss_powered) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    sim7670_gnss_fix_t modem_fix;
    const esp_err_t ret = sim7670_read_gnss_fix(&device->modem,
                                                timeout_ms,
                                                &modem_fix);
    if (ret == ESP_OK) {
        *fix = (solar_os_gnss_fix_t) {
            .valid = modem_fix.valid,
            .time_valid = modem_fix.time_valid,
            .year = modem_fix.year,
            .month = modem_fix.month,
            .day = modem_fix.day,
            .hour = modem_fix.hour,
            .minute = modem_fix.minute,
            .second = modem_fix.second,
            .fix_type = modem_fix.valid ? 3U : 0U,
            .latitude_deg_e7 = modem_fix.latitude_deg_e7,
            .longitude_deg_e7 = modem_fix.longitude_deg_e7,
            .height_msl_mm = modem_fix.height_msl_mm,
        };
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t gnss_set_power(void *ctx, bool enabled)
{
    solar_os_sim7670_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    const esp_err_t ret = sim7670_set_gnss_power(&device->modem, enabled);
    if (ret == ESP_OK) {
        device->gnss_powered = enabled;
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_gnss_ops_t gnss_ops = {
    .read_fix = gnss_read_fix,
    .set_power = gnss_set_power,
};

esp_err_t solar_os_sim7670_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_sim7670_device_t *device = NULL;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) {
            device = &devices[i];
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       uart_bus,
                                       sizeof(uart_bus)),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    const sim7670_io_t io = {
        .write = modem_write,
        .read = modem_read,
        .user = device,
    };
    esp_err_t ret = sim7670_init(&device->modem, &io);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }

    const solar_os_modem_registration_t modem_registration = {
        .name = name,
        .driver = "sim7670",
        .transport = device->uart_bus,
        .ops = &modem_ops,
        .ctx = device,
    };
    ret = solar_os_modem_register(&modem_registration);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }

    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = "sim7670",
        .ops = &gnss_ops,
        .ctx = device,
        .powered = false,
    };
    ret = solar_os_gnss_register(&registration);
    if (ret != ESP_OK) {
        (void)solar_os_modem_unregister(name);
        memset(device, 0, sizeof(*device));
        return ret;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    device->ppp_events =
        xEventGroupCreateStatic(&device->ppp_events_storage);
    if (device->ppp_events == NULL) {
        (void)solar_os_gnss_unregister(name);
        (void)solar_os_modem_unregister(name);
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    device->network_state = SOLAR_OS_MODEM_NETWORK_DOWN;
#endif
    ESP_LOGI(TAG, "%s attached on %s", name, uart_bus);
    return ESP_OK;
}

esp_err_t solar_os_sim7670_detach(const char *name)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode) {
        ESP_RETURN_ON_ERROR(ppp_stop(device),
                            TAG,
                            "PPP stop failed");
    }
#endif
    ESP_RETURN_ON_ERROR(solar_os_gnss_unregister(name),
                        TAG,
                        "GNSS unregister failed");
    ESP_RETURN_ON_ERROR(solar_os_modem_unregister(name),
                        TAG,
                        "modem unregister failed");
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    device->active = false;
    xSemaphoreGive(device->mutex);
#if CONFIG_LWIP_PPP_SUPPORT
    ppp_runtime_destroy(device);
    if (device->ppp_events != NULL) {
        vEventGroupDelete(device->ppp_events);
        device->ppp_events = NULL;
    }
    secure_zero(&device->applied_profile,
                sizeof(device->applied_profile));
#endif
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}

size_t solar_os_sim7670_count(void)
{
    size_t count = 0U;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        count += devices[i].active ? 1U : 0U;
    }
    return count;
}

bool solar_os_sim7670_get(size_t index, solar_os_sim7670_info_t *info)
{
    if (info == NULL) {
        return false;
    }
    size_t current = 0U;
    for (size_t i = 0; i < SIM7670_DEVICE_MAX; i++) {
        if (!devices[i].active) {
            continue;
        }
        if (current++ == index) {
            xSemaphoreTake(devices[i].mutex, portMAX_DELAY);
            *info = (solar_os_sim7670_info_t) {
                .gnss_powered = devices[i].gnss_powered,
            };
            strlcpy(info->name, devices[i].name, sizeof(info->name));
            strlcpy(info->uart_bus,
                    devices[i].uart_bus,
                    sizeof(info->uart_bus));
            xSemaphoreGive(devices[i].mutex);
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_sim7670_read_status(const char *name,
                                       sim7670_status_t *status)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_read_status(&device->modem, status);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

esp_err_t solar_os_sim7670_command(const char *name,
                                   const char *command,
                                   uint32_t timeout_ms,
                                   char *response,
                                   size_t response_size)
{
    solar_os_sim7670_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
#if CONFIG_LWIP_PPP_SUPPORT
    if (device->ppp_mode || device->ppp_transitioning) {
        ret = ESP_ERR_INVALID_STATE;
    }
#endif
    if (ret == ESP_OK) {
        ret = sim7670_command(&device->modem,
                              command,
                              timeout_ms,
                              response,
                              response_size);
    }
    xSemaphoreGive(device->mutex);
    return ret;
}
