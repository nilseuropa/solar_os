#include "solar_os_ppp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if !CONFIG_LWIP_PPP_SUPPORT
#error "service.ppp requires CONFIG_LWIP_PPP_SUPPORT=y"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "solar_os_task.h"

#define PPP_RX_BUFFER_SIZE 768U
#define PPP_TASK_STACK 4096U
#define PPP_TASK_PRIORITY 8U
#define PPP_DEFAULT_READ_TIMEOUT_MS 100U
#define PPP_STOP_TIMEOUT_MS 5000U

#define PPP_EVENT_GOT_IP BIT0
#define PPP_EVENT_LOST_IP BIT1
#define PPP_EVENT_ERROR BIT2
#define PPP_EVENT_PHASE_DEAD BIT3

struct solar_os_ppp {
    char name[SOLAR_OS_PPP_NAME_MAX + 1U];
    int route_priority;
    uint32_t read_timeout_ms;
    solar_os_ppp_transport_t transport;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    EventGroupHandle_t events;
    StaticEventGroup_t events_storage;
    esp_netif_t *netif;
    esp_netif_driver_ifconfig_t driver;
    esp_event_handler_instance_t got_ip_handler;
    esp_event_handler_instance_t lost_ip_handler;
    esp_event_handler_instance_t status_handler;
    TaskHandle_t worker;
    volatile bool worker_stop_requested;
    volatile bool worker_done;
    bool session_active;
    bool link_open;
    bool transitioning;
    char if_key[SOLAR_OS_PPP_NAME_MAX + 5U];
    char if_desc[SOLAR_OS_PPP_NAME_MAX + 5U];
    solar_os_ppp_profile_t profile;
    solar_os_ppp_status_t status;
};

static const char *TAG = "ppp";

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static bool string_terminated(const char *value, size_t size)
{
    return value != NULL && memchr(value, '\0', size) != NULL;
}

static esp_err_t validate_profile(const solar_os_ppp_profile_t *profile)
{
    if (profile == NULL || profile->auth > SOLAR_OS_PPP_AUTH_CHAP ||
        !string_terminated(profile->username, sizeof(profile->username)) ||
        !string_terminated(profile->password, sizeof(profile->password)) ||
        !string_terminated(profile->dns, sizeof(profile->dns))) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool have_username = profile->username[0] != '\0';
    const bool have_password = profile->password[0] != '\0';
    if (profile->auth == SOLAR_OS_PPP_AUTH_NONE) {
        if (have_username || have_password) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (!have_username || !have_password) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile->dns[0] != '\0') {
        esp_ip4_addr_t dns;
        if (esp_netif_str_to_ip4(profile->dns, &dns) != ESP_OK ||
            dns.addr == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void clear_addresses_locked(solar_os_ppp_t *ppp)
{
    ppp->status.interface_name[0] = '\0';
    ppp->status.ipv4_address[0] = '\0';
    ppp->status.ipv4_gateway[0] = '\0';
    ppp->status.dns_address[0] = '\0';
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
    solar_os_ppp_t *ppp = handle;
    if (ppp == NULL || buffer == NULL || length == 0U ||
        !ppp->session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t written = 0U;
    const esp_err_t ret = ppp->transport.write(ppp->transport.ctx,
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
    solar_os_ppp_t *ppp = arg;
    if (ppp == NULL || ppp->netif == NULL) {
        return;
    }

    EventBits_t bits = 0U;
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_PPP_GOT_IP) {
            const ip_event_got_ip_t *event = event_data;
            if (event == NULL || event->esp_netif != ppp->netif) {
                return;
            }
            esp_netif_dns_info_t dns = {0};
            (void)esp_netif_get_dns_info(ppp->netif,
                                         ESP_NETIF_DNS_MAIN,
                                         &dns);
            char dns_override[SOLAR_OS_PPP_ADDRESS_MAX] = {0};
            xSemaphoreTake(ppp->mutex, portMAX_DELAY);
            strlcpy(dns_override, ppp->profile.dns, sizeof(dns_override));
            xSemaphoreGive(ppp->mutex);
            if (dns.ip.type == ESP_IPADDR_TYPE_V4 &&
                dns.ip.u_addr.ip4.addr != 0U) {
                const esp_err_t ret = esp_netif_set_dns_info(
                    ppp->netif,
                    ESP_NETIF_DNS_MAIN,
                    &dns);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG,
                             "%s failed to activate negotiated DNS: %s",
                             ppp->name,
                             esp_err_to_name(ret));
                }
            }
            if (dns_override[0] != '\0') {
                esp_netif_dns_info_t override_dns = {
                    .ip.type = ESP_IPADDR_TYPE_V4,
                };
                if (esp_netif_str_to_ip4(
                        dns_override,
                        &override_dns.ip.u_addr.ip4) == ESP_OK) {
                    const esp_err_t ret = esp_netif_set_dns_info(
                        ppp->netif,
                        ESP_NETIF_DNS_MAIN,
                        &override_dns);
                    if (ret == ESP_OK) {
                        dns = override_dns;
                    } else {
                        ESP_LOGW(TAG,
                                 "%s failed to activate profile DNS: %s",
                                 ppp->name,
                                 esp_err_to_name(ret));
                    }
                }
            }
#if CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
            if (esp_netif_get_default_netif() == ppp->netif &&
                dns.ip.type == ESP_IPADDR_TYPE_V4 &&
                dns.ip.u_addr.ip4.addr != 0U) {
                const esp_err_t ret = esp_netif_set_dns_info(
                    NULL,
                    ESP_NETIF_DNS_MAIN,
                    &dns);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG,
                             "%s failed to synchronize active DNS: %s",
                             ppp->name,
                             esp_err_to_name(ret));
                }
            }
#endif
            xSemaphoreTake(ppp->mutex, portMAX_DELAY);
            ppp->status.state = SOLAR_OS_PPP_STATE_UP;
            ppp->status.error = 0;
            format_ipv4(&event->ip_info.ip,
                        ppp->status.ipv4_address,
                        sizeof(ppp->status.ipv4_address));
            format_ipv4(&event->ip_info.gw,
                        ppp->status.ipv4_gateway,
                        sizeof(ppp->status.ipv4_gateway));
            format_ipv4(dns.ip.type == ESP_IPADDR_TYPE_V4
                            ? &dns.ip.u_addr.ip4
                            : NULL,
                        ppp->status.dns_address,
                        sizeof(ppp->status.dns_address));
            (void)esp_netif_get_netif_impl_name(
                ppp->netif,
                ppp->status.interface_name);
            xSemaphoreGive(ppp->mutex);
            bits = PPP_EVENT_GOT_IP;
        } else if (event_id == IP_EVENT_PPP_LOST_IP) {
            const ip_event_got_ip_t *event = event_data;
            if (event == NULL || event->esp_netif != ppp->netif) {
                return;
            }
            xSemaphoreTake(ppp->mutex, portMAX_DELAY);
            if (ppp->session_active && !ppp->transitioning) {
                ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
            }
            clear_addresses_locked(ppp);
            xSemaphoreGive(ppp->mutex);
            bits = PPP_EVENT_LOST_IP;
        }
    } else if (event_base == NETIF_PPP_STATUS) {
        const esp_netif_t *netif = event_data != NULL
            ? *(esp_netif_t * const *)event_data
            : NULL;
        if (netif != ppp->netif) {
            return;
        }
        xSemaphoreTake(ppp->mutex, portMAX_DELAY);
        if (event_id == NETIF_PPP_PHASE_DEAD) {
            bits = PPP_EVENT_PHASE_DEAD;
        } else if (event_id > NETIF_PPP_ERRORNONE &&
                   event_id < NETIF_PP_PHASE_OFFSET &&
                   ppp->session_active && !ppp->transitioning) {
            ppp->status.error = event_id;
            ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
            bits = PPP_EVENT_ERROR;
        }
        xSemaphoreGive(ppp->mutex);
    }
    if (bits != 0U && ppp->events != NULL) {
        xEventGroupSetBits(ppp->events, bits);
    }
}

esp_err_t solar_os_ppp_receive(solar_os_ppp_t *ppp,
                               const uint8_t *data,
                               size_t length)
{
    if (ppp == NULL || data == NULL || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ppp->session_active || ppp->netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_receive(ppp->netif, (void *)data, length, NULL);
}

static void ppp_worker(void *arg)
{
    solar_os_ppp_t *ppp = arg;
    uint8_t buffer[PPP_RX_BUFFER_SIZE];
    while (!ppp->worker_stop_requested) {
        size_t received = 0U;
        const esp_err_t ret = ppp->transport.read(
            ppp->transport.ctx,
            buffer,
            sizeof(buffer),
            ppp->read_timeout_ms,
            &received);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            xSemaphoreTake(ppp->mutex, portMAX_DELAY);
            ppp->status.error = ret;
            ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
            xSemaphoreGive(ppp->mutex);
            xEventGroupSetBits(ppp->events, PPP_EVENT_ERROR);
            break;
        }
        if (received > 0U &&
            solar_os_ppp_receive(ppp, buffer, received) != ESP_OK) {
            xSemaphoreTake(ppp->mutex, portMAX_DELAY);
            ppp->status.error = ESP_FAIL;
            ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
            xSemaphoreGive(ppp->mutex);
            xEventGroupSetBits(ppp->events, PPP_EVENT_ERROR);
            break;
        }
    }
    ppp->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void runtime_destroy(solar_os_ppp_t *ppp)
{
    if (ppp->status_handler != NULL) {
        (void)esp_event_handler_instance_unregister(
            NETIF_PPP_STATUS,
            ESP_EVENT_ANY_ID,
            ppp->status_handler);
        ppp->status_handler = NULL;
    }
    if (ppp->lost_ip_handler != NULL) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_PPP_LOST_IP,
            ppp->lost_ip_handler);
        ppp->lost_ip_handler = NULL;
    }
    if (ppp->got_ip_handler != NULL) {
        (void)esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_PPP_GOT_IP,
            ppp->got_ip_handler);
        ppp->got_ip_handler = NULL;
    }
    if (ppp->netif != NULL) {
        esp_netif_destroy(ppp->netif);
        ppp->netif = NULL;
    }
}

static esp_err_t runtime_init_locked(solar_os_ppp_t *ppp)
{
    if (ppp->netif != NULL) {
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

    (void)snprintf(ppp->if_key, sizeof(ppp->if_key), "PPP_%s", ppp->name);
    (void)snprintf(ppp->if_desc, sizeof(ppp->if_desc), "ppp-%s", ppp->name);
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base.if_key = ppp->if_key;
    base.if_desc = ppp->if_desc;
    base.route_prio = ppp->route_priority;
    ppp->driver = (esp_netif_driver_ifconfig_t) {
        .handle = ppp,
        .transmit = ppp_transmit,
    };
    const esp_netif_config_t config = {
        .base = &base,
        .driver = &ppp->driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };
    ppp->netif = esp_netif_new(&config);
    if (ppp->netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_netif_ppp_config_t ppp_config = {
        .ppp_phase_event_enabled = true,
        .ppp_error_event_enabled = true,
    };
    ret = esp_netif_ppp_set_params(ppp->netif, &ppp_config);
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_PPP_GOT_IP,
            ppp_event_handler,
            ppp,
            &ppp->got_ip_handler);
    }
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_PPP_LOST_IP,
            ppp_event_handler,
            ppp,
            &ppp->lost_ip_handler);
    }
    if (ret == ESP_OK) {
        ret = esp_event_handler_instance_register(
            NETIF_PPP_STATUS,
            ESP_EVENT_ANY_ID,
            ppp_event_handler,
            ppp,
            &ppp->status_handler);
    }
    if (ret != ESP_OK) {
        runtime_destroy(ppp);
    }
    return ret;
}

static esp_err_t configure_auth_locked(solar_os_ppp_t *ppp)
{
    esp_netif_auth_type_t auth = NETIF_PPP_AUTHTYPE_NONE;
    if (ppp->profile.auth == SOLAR_OS_PPP_AUTH_PAP) {
        auth = NETIF_PPP_AUTHTYPE_PAP;
    } else if (ppp->profile.auth == SOLAR_OS_PPP_AUTH_CHAP) {
        auth = NETIF_PPP_AUTHTYPE_CHAP;
    }
    return esp_netif_ppp_set_auth(ppp->netif,
                                  auth,
                                  ppp->profile.username,
                                  ppp->profile.password);
}

esp_err_t solar_os_ppp_create(const solar_os_ppp_config_t *config,
                              solar_os_ppp_t **out_ppp)
{
    if (config == NULL || out_ppp == NULL || config->name == NULL ||
        config->name[0] == '\0' ||
        strlen(config->name) > SOLAR_OS_PPP_NAME_MAX ||
        config->transport.write == NULL ||
        ((config->transport.start == NULL) !=
         (config->transport.stop == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ppp = NULL;
    solar_os_ppp_t *ppp = calloc(1U, sizeof(*ppp));
    if (ppp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(ppp->name, config->name, sizeof(ppp->name));
    ppp->route_priority = config->route_priority;
    ppp->read_timeout_ms = config->read_timeout_ms != 0U
        ? config->read_timeout_ms
        : PPP_DEFAULT_READ_TIMEOUT_MS;
    ppp->transport = config->transport;
    ppp->mutex = xSemaphoreCreateMutexStatic(&ppp->mutex_storage);
    ppp->events = xEventGroupCreateStatic(&ppp->events_storage);
    if (ppp->mutex == NULL || ppp->events == NULL) {
        if (ppp->events != NULL) {
            vEventGroupDelete(ppp->events);
        }
        if (ppp->mutex != NULL) {
            vSemaphoreDelete(ppp->mutex);
        }
        free(ppp);
        return ESP_ERR_NO_MEM;
    }
    ppp->status.state = SOLAR_OS_PPP_STATE_DOWN;
    *out_ppp = ppp;
    return ESP_OK;
}

esp_err_t solar_os_ppp_destroy(solar_os_ppp_t *ppp)
{
    if (ppp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    if (ppp->session_active || ppp->link_open || ppp->transitioning) {
        xSemaphoreGive(ppp->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ppp->transitioning = true;
    xSemaphoreGive(ppp->mutex);
    runtime_destroy(ppp);
    secure_zero(&ppp->profile, sizeof(ppp->profile));
    vEventGroupDelete(ppp->events);
    vSemaphoreDelete(ppp->mutex);
    free(ppp);
    return ESP_OK;
}

esp_err_t solar_os_ppp_connect(solar_os_ppp_t *ppp,
                               const solar_os_ppp_profile_t *profile,
                               uint32_t timeout_ms)
{
    if (ppp == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = validate_profile(profile);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    if (ppp->status.state == SOLAR_OS_PPP_STATE_UP &&
        ppp->session_active) {
        xSemaphoreGive(ppp->mutex);
        return ESP_OK;
    }
    if (ppp->session_active || ppp->link_open || ppp->transitioning) {
        xSemaphoreGive(ppp->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ret = runtime_init_locked(ppp);
    if (ret == ESP_OK) {
        ppp->profile = *profile;
        ret = configure_auth_locked(ppp);
    }
    if (ret != ESP_OK) {
        secure_zero(&ppp->profile, sizeof(ppp->profile));
        xSemaphoreGive(ppp->mutex);
        return ret;
    }
    xEventGroupClearBits(ppp->events,
                         PPP_EVENT_GOT_IP |
                         PPP_EVENT_LOST_IP |
                         PPP_EVENT_ERROR |
                         PPP_EVENT_PHASE_DEAD);
    ppp->status.state = SOLAR_OS_PPP_STATE_CONNECTING;
    ppp->status.error = 0;
    clear_addresses_locked(ppp);
    ppp->transitioning = true;
    xSemaphoreGive(ppp->mutex);

    if (ppp->transport.start != NULL) {
        ret = ppp->transport.start(ppp->transport.ctx);
    }
    if (ret != ESP_OK) {
        xSemaphoreTake(ppp->mutex, portMAX_DELAY);
        ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
        ppp->status.error = ret;
        ppp->transitioning = false;
        secure_zero(&ppp->profile, sizeof(ppp->profile));
        xSemaphoreGive(ppp->mutex);
        return ret;
    }

    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    ppp->link_open = ppp->transport.start != NULL;
    ppp->session_active = true;
    ppp->worker_stop_requested = false;
    ppp->worker_done = false;
    xSemaphoreGive(ppp->mutex);

    if (ppp->transport.read != NULL &&
        solar_os_task_create_pinned_internal(
            ppp_worker,
            "ppp_rx",
            PPP_TASK_STACK,
            ppp,
            PPP_TASK_PRIORITY,
            &ppp->worker,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        xSemaphoreTake(ppp->mutex, portMAX_DELAY);
        ppp->session_active = false;
        ppp->worker_done = true;
        ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
        ppp->status.error = ESP_ERR_NO_MEM;
        const bool link_open = ppp->link_open;
        xSemaphoreGive(ppp->mutex);
        esp_err_t stop_ret = ESP_OK;
        if (link_open) {
            stop_ret = ppp->transport.stop(ppp->transport.ctx);
        }
        xSemaphoreTake(ppp->mutex, portMAX_DELAY);
        if (stop_ret == ESP_OK) {
            ppp->link_open = false;
        }
        ppp->transitioning = false;
        secure_zero(&ppp->profile, sizeof(ppp->profile));
        xSemaphoreGive(ppp->mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_netif_action_start(ppp->netif, 0, 0, NULL);
    esp_netif_action_connected(ppp->netif, 0, 0, NULL);
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    ppp->transitioning = false;
    xSemaphoreGive(ppp->mutex);

    const EventBits_t bits = xEventGroupWaitBits(
        ppp->events,
        PPP_EVENT_GOT_IP | PPP_EVENT_ERROR,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if ((bits & PPP_EVENT_GOT_IP) != 0U) {
        return ESP_OK;
    }
    ret = (bits & PPP_EVENT_ERROR) != 0U ? ESP_FAIL : ESP_ERR_TIMEOUT;
    const esp_err_t stop_ret = solar_os_ppp_disconnect(ppp);
    return stop_ret == ESP_OK ? ret : stop_ret;
}

esp_err_t solar_os_ppp_disconnect(solar_os_ppp_t *ppp)
{
    if (ppp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    if (ppp->transitioning) {
        xSemaphoreGive(ppp->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!ppp->session_active && !ppp->link_open) {
        ppp->status.state = SOLAR_OS_PPP_STATE_DOWN;
        xSemaphoreGive(ppp->mutex);
        return ESP_OK;
    }
    ppp->transitioning = true;
    const bool session_active = ppp->session_active;
    TaskHandle_t worker = ppp->worker;
    if (session_active) {
        xEventGroupClearBits(ppp->events,
                             PPP_EVENT_LOST_IP | PPP_EVENT_PHASE_DEAD);
    }
    xSemaphoreGive(ppp->mutex);

    if (session_active) {
        esp_netif_action_disconnected(ppp->netif, 0, 0, NULL);
        esp_netif_action_stop(ppp->netif, 0, 0, NULL);
        (void)xEventGroupWaitBits(ppp->events,
                                  PPP_EVENT_LOST_IP | PPP_EVENT_PHASE_DEAD,
                                  pdFALSE,
                                  pdFALSE,
                                  pdMS_TO_TICKS(PPP_STOP_TIMEOUT_MS));
    }

    ppp->worker_stop_requested = true;
    if (worker != NULL &&
        !solar_os_task_wait_done(worker,
                                 &ppp->worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        xSemaphoreTake(ppp->mutex, portMAX_DELAY);
        ppp->status.state = SOLAR_OS_PPP_STATE_FAILED;
        ppp->status.error = ESP_ERR_TIMEOUT;
        ppp->transitioning = false;
        xSemaphoreGive(ppp->mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    ppp->worker = NULL;
    ppp->worker_stop_requested = false;
    ppp->worker_done = false;
    ppp->session_active = false;
    const bool link_open = ppp->link_open;
    xSemaphoreGive(ppp->mutex);

    esp_err_t ret = ESP_OK;
    if (link_open) {
        ret = ppp->transport.stop(ppp->transport.ctx);
    }
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    if (ret == ESP_OK) {
        ppp->link_open = false;
    }
    ppp->status.state = ret == ESP_OK
        ? SOLAR_OS_PPP_STATE_DOWN
        : SOLAR_OS_PPP_STATE_FAILED;
    ppp->status.error = ret == ESP_OK ? 0 : ret;
    clear_addresses_locked(ppp);
    ppp->transitioning = false;
    secure_zero(&ppp->profile, sizeof(ppp->profile));
    xSemaphoreGive(ppp->mutex);
    return ret;
}

esp_err_t solar_os_ppp_get_status(solar_os_ppp_t *ppp,
                                  solar_os_ppp_status_t *status)
{
    if (ppp == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    *status = ppp->status;
    xSemaphoreGive(ppp->mutex);
    return ESP_OK;
}

bool solar_os_ppp_is_busy(solar_os_ppp_t *ppp)
{
    if (ppp == NULL) {
        return false;
    }
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    const bool busy = ppp->session_active || ppp->link_open ||
        ppp->transitioning;
    xSemaphoreGive(ppp->mutex);
    return busy;
}

bool solar_os_ppp_is_connected(solar_os_ppp_t *ppp)
{
    if (ppp == NULL) {
        return false;
    }
    xSemaphoreTake(ppp->mutex, portMAX_DELAY);
    const bool connected = ppp->session_active &&
        ppp->status.state == SOLAR_OS_PPP_STATE_UP;
    xSemaphoreGive(ppp->mutex);
    return connected;
}
