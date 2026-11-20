#include "solar_os_qemu_eth.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_network.h"
#include "solar_os_wifi.h"

/* QEMU's OpenCores Ethernet card (-nic user,model=open_eth) as a network
 * path. Apps decide "online" from Wi-Fi status, so the link also reports
 * through solar_os_wifi_set_status_override. ESP-IDF's openeth driver runs
 * only in QEMU. Setup follows ESP-IDF's protocol_examples_common/eth_connect.c. */

#define QEMU_ETH_ROUTE_PRIORITY 100
#define QEMU_ETH_SSID "qemu-eth"

typedef struct {
    bool active;
    bool has_ip;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char ip[16];
    char gateway[16];
    char netmask[16];
    esp_netif_t *netif;
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    esp_eth_handle_t eth;
    esp_eth_netif_glue_handle_t glue;
} qemu_eth_device_t;

static const char *TAG = "qemu_eth";
static qemu_eth_device_t device;
static portMUX_TYPE device_lock = portMUX_INITIALIZER_UNLOCKED;

static bool fill_wifi_status(solar_os_wifi_status_t *status)
{
    bool has_ip;
    taskENTER_CRITICAL(&device_lock);
    has_ip = device.has_ip;
    if (has_ip) {
        strlcpy(status->ip, device.ip, sizeof(status->ip));
        strlcpy(status->gateway, device.gateway, sizeof(status->gateway));
        strlcpy(status->netmask, device.netmask, sizeof(status->netmask));
    }
    taskEXIT_CRITICAL(&device_lock);
    if (has_ip) {
        strlcpy(status->ssid, QEMU_ETH_SSID, sizeof(status->ssid));
    }
    return has_ip;
}

static void format_ip(const esp_ip4_addr_t *addr, char *out, size_t len)
{
    snprintf(out, len, IPSTR, IP2STR(addr));
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    if (event == NULL || event->esp_netif != device.netif) {
        return;
    }
    taskENTER_CRITICAL(&device_lock);
    format_ip(&event->ip_info.ip, device.ip, sizeof(device.ip));
    format_ip(&event->ip_info.gw, device.gateway, sizeof(device.gateway));
    format_ip(&event->ip_info.netmask, device.netmask, sizeof(device.netmask));
    device.has_ip = true;
    taskEXIT_CRITICAL(&device_lock);
    (void)solar_os_network_path_set_ready(device.netif, true);
    ESP_LOGI(TAG, "%s up: ip %s gw %s", device.name, device.ip, device.gateway);
}

static void on_link_down(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    taskENTER_CRITICAL(&device_lock);
    device.has_ip = false;
    device.ip[0] = '\0';
    device.gateway[0] = '\0';
    device.netmask[0] = '\0';
    taskEXIT_CRITICAL(&device_lock);
    (void)solar_os_network_path_set_ready(device.netif, false);
    ESP_LOGI(TAG, "%s link down", device.name);
}

/* Undo whatever attach got done, in reverse order; safe on a half-built device. */
static void teardown(void)
{
    solar_os_wifi_set_status_override(NULL);
    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip);
    (void)esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, on_link_down);
    if (device.eth != NULL) {
        (void)esp_eth_stop(device.eth);
    }
    if (device.netif != NULL) {
        (void)solar_os_network_path_unregister(device.netif);
    }
    if (device.glue != NULL) {
        (void)esp_eth_del_netif_glue(device.glue);
    }
    if (device.eth != NULL) {
        (void)esp_eth_driver_uninstall(device.eth);
    }
    if (device.phy != NULL) {
        (void)device.phy->del(device.phy);
    }
    if (device.mac != NULL) {
        (void)device.mac->del(device.mac);
    }
    if (device.netif != NULL) {
        esp_netif_destroy(device.netif);
    }
    taskENTER_CRITICAL(&device_lock);
    memset(&device, 0, sizeof(device));
    taskEXIT_CRITICAL(&device_lock);
}

esp_err_t solar_os_qemu_eth_attach(const char *name,
                                   const solar_os_expansion_binding_t *bindings,
                                   size_t binding_count)
{
    (void)bindings;
    if (name == NULL || name[0] == '\0' || binding_count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device.active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    device.active = true;
    strlcpy(device.name, name, sizeof(device.name));

    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    device.netif = esp_netif_new(&netif_config);
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    device.mac = esp_eth_mac_new_openeth(&mac_config);
    device.phy = esp_eth_phy_new_dp83848(&phy_config);
    if (device.netif == NULL || device.mac == NULL || device.phy == NULL) {
        teardown();
        return ESP_ERR_NO_MEM;
    }

    const esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(device.mac, device.phy);
    err = esp_eth_driver_install(&eth_config, &device.eth);
    if (err == ESP_OK) {
        device.glue = esp_eth_new_netif_glue(device.eth);
        err = device.glue != NULL ? esp_netif_attach(device.netif, device.glue) : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, on_link_down, NULL);
    }
    if (err == ESP_OK) {
        err = solar_os_network_path_register(QEMU_ETH_SSID, device.netif, QEMU_ETH_ROUTE_PRIORITY);
    }
    if (err == ESP_OK) {
        solar_os_wifi_set_status_override(fill_wifi_status);
        err = esp_eth_start(device.eth);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s attach failed: %s", name, esp_err_to_name(err));
        teardown();
        return err;
    }

    ESP_LOGI(TAG, "%s attached (QEMU open_eth, DHCP)", name);
    return ESP_OK;
}

esp_err_t solar_os_qemu_eth_detach(const char *name)
{
    if (!device.active || name == NULL || strcmp(device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    teardown();
    ESP_LOGI(TAG, "%s detached", name);
    return ESP_OK;
}
