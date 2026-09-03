#include "solar_os_wifi_repeater.h"

#include <limits.h>
#include <string.h>

#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "solar_os_log.h"

#define TAG "wifi_repeater"
#define ETHERNET_HEADER_LEN 14U
#define ARP_PACKET_LEN 28U
#define IPV4_HEADER_MIN_LEN 20U
#define UDP_HEADER_LEN 8U
#define DHCP_PACKET_MIN_LEN 240U
#define DHCP_CLIENT_PORT 68U
#define DHCP_SERVER_PORT 67U
#define DHCP_OPTION_PAD 0U
#define DHCP_OPTION_LEASE_TIME 51U
#define DHCP_OPTION_MESSAGE_TYPE 53U
#define DHCP_OPTION_END 255U
#define DHCP_MESSAGE_ACK 5U
#define DHCP_MAGIC_COOKIE 0x63825363UL
#define ETHER_TYPE_IPV4 0x0800U
#define ETHER_TYPE_ARP 0x0806U
#define REPEATER_CLIENT_TTL_SECONDS 600U
#define REPEATER_CLIENT_TTL_MAX_SECONDS 604800U

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    int64_t expires_us;
    bool valid;
} repeater_client_t;

typedef struct {
    esp_netif_t *ap_handle;
    esp_netif_t *sta_handle;
    struct netif *ap;
    struct netif *sta;
    netif_input_fn ap_input;
    netif_input_fn sta_input;
    netif_linkoutput_fn ap_linkoutput;
    netif_linkoutput_fn sta_linkoutput;
    esp_netif_ip_info_t saved_ap_ip;
    repeater_client_t clients[SOLAR_OS_WIFI_REPEATER_CLIENT_MAX];
    uint64_t upstream_frames;
    uint64_t downstream_frames;
    uint64_t dropped_frames;
    bool saved_ap_ip_valid;
    bool enabled;
} repeater_state_t;

typedef struct {
    struct netif *ap;
    struct netif *sta;
    err_t result;
} repeater_hook_request_t;

static repeater_state_t repeater;
static portMUX_TYPE repeater_lock = portMUX_INITIALIZER_UNLOCKED;

static err_t repeater_ap_input(struct pbuf *packet, struct netif *netif);
static err_t repeater_sta_input(struct pbuf *packet, struct netif *netif);

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8U) | data[1];
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
        ((uint32_t)data[1] << 16U) |
        ((uint32_t)data[2] << 8U) |
        data[3];
}

static bool mac_is_group(const uint8_t *mac)
{
    return (mac[0] & 0x01U) != 0U;
}

static bool mac_is_zero(const uint8_t *mac)
{
    uint8_t combined = 0U;
    for (size_t i = 0; i < 6U; i++) {
        combined |= mac[i];
    }
    return combined == 0U;
}

static bool mac_equal(const uint8_t *left, const uint8_t *right)
{
    return left != NULL && right != NULL && memcmp(left, right, 6U) == 0;
}

static bool ip_is_learnable(uint32_t ip)
{
    const uint32_t host = lwip_ntohl(ip);
    return ip != 0U && ip != UINT32_MAX && (host & 0xf0000000UL) != 0xe0000000UL;
}

static void repeater_note_drop(void)
{
    portENTER_CRITICAL(&repeater_lock);
    repeater.dropped_frames++;
    portEXIT_CRITICAL(&repeater_lock);
}

static void repeater_note_forward(bool upstream)
{
    portENTER_CRITICAL(&repeater_lock);
    if (upstream) {
        repeater.upstream_frames++;
    } else {
        repeater.downstream_frames++;
    }
    portEXIT_CRITICAL(&repeater_lock);
}

static void repeater_client_learn(uint32_t ip, const uint8_t mac[6], uint32_t ttl_seconds)
{
    if (!ip_is_learnable(ip) || mac == NULL || mac_is_zero(mac) || mac_is_group(mac)) {
        return;
    }

    if (ttl_seconds > REPEATER_CLIENT_TTL_MAX_SECONDS) {
        ttl_seconds = REPEATER_CLIENT_TTL_MAX_SECONDS;
    }
    const int64_t now = esp_timer_get_time();
    const int64_t expires = now + (int64_t)ttl_seconds * 1000000LL;
    size_t selected = SOLAR_OS_WIFI_REPEATER_CLIENT_MAX;
    size_t oldest = 0U;
    int64_t oldest_expiry = INT64_MAX;

    portENTER_CRITICAL(&repeater_lock);
    for (size_t i = 0; i < SOLAR_OS_WIFI_REPEATER_CLIENT_MAX; i++) {
        repeater_client_t *entry = &repeater.clients[i];
        if (entry->valid &&
            (entry->ip == ip || memcmp(entry->mac, mac, sizeof(entry->mac)) == 0)) {
            selected = i;
            break;
        }
        if (selected == SOLAR_OS_WIFI_REPEATER_CLIENT_MAX &&
            (!entry->valid || entry->expires_us <= now)) {
            selected = i;
        }
        if (entry->expires_us < oldest_expiry) {
            oldest_expiry = entry->expires_us;
            oldest = i;
        }
    }
    if (selected == SOLAR_OS_WIFI_REPEATER_CLIENT_MAX) {
        selected = oldest;
    }
    repeater.clients[selected].ip = ip;
    memcpy(repeater.clients[selected].mac, mac, sizeof(repeater.clients[selected].mac));
    repeater.clients[selected].expires_us = expires;
    repeater.clients[selected].valid = true;
    portEXIT_CRITICAL(&repeater_lock);
}

static bool repeater_client_lookup(uint32_t ip, uint8_t mac[6])
{
    bool found = false;
    const int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&repeater_lock);
    for (size_t i = 0; i < SOLAR_OS_WIFI_REPEATER_CLIENT_MAX; i++) {
        repeater_client_t *entry = &repeater.clients[i];
        if (entry->valid && entry->expires_us <= now) {
            entry->valid = false;
        }
        if (entry->valid && entry->ip == ip) {
            if (mac != NULL) {
                memcpy(mac, entry->mac, sizeof(entry->mac));
            }
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&repeater_lock);
    return found;
}

static struct pbuf *repeater_clone(struct pbuf *packet)
{
    if (packet == NULL || packet->tot_len < ETHERNET_HEADER_LEN) {
        return NULL;
    }
    struct pbuf *copy = pbuf_clone(PBUF_RAW, PBUF_RAM, packet);
    if (copy == NULL || copy->len != copy->tot_len) {
        if (copy != NULL) {
            pbuf_free(copy);
        }
        return NULL;
    }
    return copy;
}

static bool repeater_emit(struct netif *netif,
                          netif_linkoutput_fn output,
                          struct pbuf *packet,
                          bool upstream)
{
    if (netif == NULL || output == NULL || packet == NULL) {
        repeater_note_drop();
        return false;
    }
    const err_t error = output(netif, packet);
    if (error == ERR_OK) {
        repeater_note_forward(upstream);
        return true;
    }
    repeater_note_drop();
    return false;
}

static bool repeater_ipv4_header(const uint8_t *frame,
                                 size_t length,
                                 size_t *ip_header_len)
{
    if (length < ETHERNET_HEADER_LEN + IPV4_HEADER_MIN_LEN ||
        read_be16(frame + 12U) != ETHER_TYPE_IPV4 ||
        (frame[ETHERNET_HEADER_LEN] >> 4U) != 4U) {
        return false;
    }
    const size_t header_len = (size_t)(frame[ETHERNET_HEADER_LEN] & 0x0fU) * 4U;
    if (header_len < IPV4_HEADER_MIN_LEN || length < ETHERNET_HEADER_LEN + header_len) {
        return false;
    }
    if (ip_header_len != NULL) {
        *ip_header_len = header_len;
    }
    return true;
}

static bool repeater_dhcp_reply(const uint8_t *frame,
                                size_t length,
                                uint8_t client_mac[6],
                                uint32_t *assigned_ip,
                                uint32_t *lease_seconds)
{
    size_t ip_header_len = 0U;
    if (!repeater_ipv4_header(frame, length, &ip_header_len) || frame[23U] != 17U) {
        return false;
    }
    const size_t udp = ETHERNET_HEADER_LEN + ip_header_len;
    const size_t dhcp = udp + UDP_HEADER_LEN;
    if (length < dhcp + DHCP_PACKET_MIN_LEN ||
        read_be16(frame + udp) != DHCP_SERVER_PORT ||
        read_be16(frame + udp + 2U) != DHCP_CLIENT_PORT ||
        frame[dhcp] != 2U ||
        read_be32(frame + dhcp + 236U) != DHCP_MAGIC_COOKIE) {
        return false;
    }

    memcpy(client_mac, frame + dhcp + 28U, 6U);
    *assigned_ip = 0U;
    memcpy(assigned_ip, frame + dhcp + 16U, sizeof(*assigned_ip));
    *lease_seconds = REPEATER_CLIENT_TTL_SECONDS;

    uint8_t message_type = 0U;
    size_t option = dhcp + DHCP_PACKET_MIN_LEN;
    while (option < length) {
        const uint8_t code = frame[option++];
        if (code == DHCP_OPTION_END) {
            break;
        }
        if (code == DHCP_OPTION_PAD) {
            continue;
        }
        if (option >= length) {
            break;
        }
        const size_t option_len = frame[option++];
        if (option_len > length - option) {
            break;
        }
        if (code == DHCP_OPTION_MESSAGE_TYPE && option_len == 1U) {
            message_type = frame[option];
        } else if (code == DHCP_OPTION_LEASE_TIME && option_len == 4U) {
            const uint32_t lease = read_be32(frame + option);
            if (lease > 0U) {
                *lease_seconds = lease;
            }
        }
        option += option_len;
    }
    if (message_type != DHCP_MESSAGE_ACK) {
        *assigned_ip = 0U;
    }
    return !mac_is_zero(client_mac) && !mac_is_group(client_mac);
}

static void repeater_prepare_dhcp_request(uint8_t *frame, size_t length)
{
    size_t ip_header_len = 0U;
    if (!repeater_ipv4_header(frame, length, &ip_header_len) || frame[23U] != 17U) {
        return;
    }
    const size_t udp = ETHERNET_HEADER_LEN + ip_header_len;
    const size_t dhcp = udp + UDP_HEADER_LEN;
    if (length < dhcp + DHCP_PACKET_MIN_LEN ||
        read_be16(frame + udp) != DHCP_CLIENT_PORT ||
        read_be16(frame + udp + 2U) != DHCP_SERVER_PORT ||
        read_be32(frame + dhcp + 236U) != DHCP_MAGIC_COOKIE) {
        return;
    }
    frame[dhcp + 10U] |= 0x80U;
    frame[udp + 6U] = 0U;
    frame[udp + 7U] = 0U;
}

static bool repeater_forward_ap_to_sta(struct pbuf *packet)
{
    struct pbuf *copy = repeater_clone(packet);
    if (copy == NULL) {
        repeater_note_drop();
        return false;
    }

    uint8_t *frame = (uint8_t *)copy->payload;
    const size_t length = copy->tot_len;
    const uint16_t ether_type = read_be16(frame + 12U);
    const bool group = mac_is_group(frame);

    if (ether_type == ETHER_TYPE_ARP) {
        if (length < ETHERNET_HEADER_LEN + ARP_PACKET_LEN) {
            pbuf_free(copy);
            return false;
        }
        uint32_t sender_ip = 0U;
        uint32_t target_ip = 0U;
        memcpy(&sender_ip, frame + ETHERNET_HEADER_LEN + 14U, sizeof(sender_ip));
        memcpy(&target_ip, frame + ETHERNET_HEADER_LEN + 24U, sizeof(target_ip));
        if ((repeater.ap != NULL && target_ip == netif_ip4_addr(repeater.ap)->addr) ||
            (repeater.sta != NULL && target_ip == netif_ip4_addr(repeater.sta)->addr)) {
            pbuf_free(copy);
            return false;
        }
        repeater_client_learn(sender_ip, frame + 6U, REPEATER_CLIENT_TTL_SECONDS);
        memcpy(frame + ETHERNET_HEADER_LEN + 8U, repeater.sta->hwaddr, 6U);
    } else if (ether_type == ETHER_TYPE_IPV4) {
        if (!repeater_ipv4_header(frame, length, NULL)) {
            pbuf_free(copy);
            return false;
        }
        uint32_t source_ip = 0U;
        uint32_t destination_ip = 0U;
        memcpy(&source_ip, frame + 26U, sizeof(source_ip));
        memcpy(&destination_ip, frame + 30U, sizeof(destination_ip));
        if ((repeater.ap != NULL && destination_ip == netif_ip4_addr(repeater.ap)->addr) ||
            (repeater.sta != NULL && destination_ip == netif_ip4_addr(repeater.sta)->addr)) {
            pbuf_free(copy);
            return false;
        }
        repeater_client_learn(source_ip, frame + 6U, REPEATER_CLIENT_TTL_SECONDS);
        repeater_prepare_dhcp_request(frame, length);
    } else {
        pbuf_free(copy);
        return false;
    }

    memcpy(frame + 6U, repeater.sta->hwaddr, 6U);
    (void)repeater_emit(repeater.sta, repeater.sta_linkoutput, copy, true);
    pbuf_free(copy);

    if (group && ether_type == ETHER_TYPE_IPV4) {
        return false;
    }
    pbuf_free(packet);
    return true;
}

static bool repeater_send_proxy_arp(struct pbuf *packet)
{
    struct pbuf *reply = repeater_clone(packet);
    if (reply == NULL || reply->tot_len < ETHERNET_HEADER_LEN + ARP_PACKET_LEN) {
        if (reply != NULL) {
            pbuf_free(reply);
        }
        repeater_note_drop();
        return false;
    }

    uint8_t *frame = (uint8_t *)reply->payload;
    uint8_t requester_mac[6];
    uint32_t requester_ip = 0U;
    uint32_t requested_ip = 0U;
    memcpy(requester_mac, frame + ETHERNET_HEADER_LEN + 8U, sizeof(requester_mac));
    memcpy(&requester_ip, frame + ETHERNET_HEADER_LEN + 14U, sizeof(requester_ip));
    memcpy(&requested_ip, frame + ETHERNET_HEADER_LEN + 24U, sizeof(requested_ip));

    memcpy(frame, requester_mac, 6U);
    memcpy(frame + 6U, repeater.sta->hwaddr, 6U);
    frame[ETHERNET_HEADER_LEN + 6U] = 0U;
    frame[ETHERNET_HEADER_LEN + 7U] = 2U;
    memcpy(frame + ETHERNET_HEADER_LEN + 8U, repeater.sta->hwaddr, 6U);
    memcpy(frame + ETHERNET_HEADER_LEN + 14U, &requested_ip, sizeof(requested_ip));
    memcpy(frame + ETHERNET_HEADER_LEN + 18U, requester_mac, 6U);
    memcpy(frame + ETHERNET_HEADER_LEN + 24U, &requester_ip, sizeof(requester_ip));
    const bool sent = repeater_emit(repeater.sta,
                                    repeater.sta_linkoutput,
                                    reply,
                                    true);
    pbuf_free(reply);
    return sent;
}

static bool repeater_forward_sta_to_ap(struct pbuf *packet)
{
    struct pbuf *copy = repeater_clone(packet);
    if (copy == NULL) {
        repeater_note_drop();
        return false;
    }

    uint8_t *frame = (uint8_t *)copy->payload;
    const size_t length = copy->tot_len;
    const uint16_t ether_type = read_be16(frame + 12U);
    const bool group = mac_is_group(frame);
    uint8_t client_mac[6] = {0};
    bool have_client = false;

    if (repeater.sta != NULL && mac_equal(frame + 6U, repeater.sta->hwaddr)) {
        pbuf_free(copy);
        return false;
    }

    if (ether_type == ETHER_TYPE_ARP) {
        if (length < ETHERNET_HEADER_LEN + ARP_PACKET_LEN) {
            pbuf_free(copy);
            return false;
        }
        const uint16_t operation = read_be16(frame + ETHERNET_HEADER_LEN + 6U);
        uint32_t target_ip = 0U;
        memcpy(&target_ip, frame + ETHERNET_HEADER_LEN + 24U, sizeof(target_ip));
        have_client = repeater_client_lookup(target_ip, client_mac);
        if (operation == 1U && have_client) {
            pbuf_free(copy);
            (void)repeater_send_proxy_arp(packet);
            pbuf_free(packet);
            return true;
        }
        if (have_client) {
            memcpy(frame + ETHERNET_HEADER_LEN + 18U, client_mac, 6U);
        }
    } else if (ether_type == ETHER_TYPE_IPV4) {
        if (!repeater_ipv4_header(frame, length, NULL)) {
            pbuf_free(copy);
            return false;
        }
        uint32_t destination_ip = 0U;
        memcpy(&destination_ip, frame + 30U, sizeof(destination_ip));
        uint32_t assigned_ip = 0U;
        uint32_t lease_seconds = REPEATER_CLIENT_TTL_SECONDS;
        if (repeater_dhcp_reply(frame,
                                length,
                                client_mac,
                                &assigned_ip,
                                &lease_seconds)) {
            have_client = true;
            if (assigned_ip != 0U) {
                repeater_client_learn(assigned_ip, client_mac, lease_seconds);
            }
        } else {
            have_client = repeater_client_lookup(destination_ip, client_mac);
        }
    } else {
        pbuf_free(copy);
        return false;
    }

    if (!have_client && !group) {
        pbuf_free(copy);
        return false;
    }
    if (have_client) {
        memcpy(frame, client_mac, 6U);
    }
    memcpy(frame + 6U, repeater.ap->hwaddr, 6U);
    (void)repeater_emit(repeater.ap, repeater.ap_linkoutput, copy, false);
    pbuf_free(copy);

    if (group && !have_client) {
        return false;
    }
    pbuf_free(packet);
    return true;
}

static err_t repeater_ap_input(struct pbuf *packet, struct netif *netif)
{
    netif_input_fn original = NULL;
    bool enabled = false;
    portENTER_CRITICAL(&repeater_lock);
    enabled = repeater.enabled;
    original = repeater.ap_input;
    portEXIT_CRITICAL(&repeater_lock);

    if (enabled && packet != NULL && repeater_forward_ap_to_sta(packet)) {
        return ERR_OK;
    }
    return original != NULL ? original(packet, netif) : ERR_IF;
}

static err_t repeater_sta_input(struct pbuf *packet, struct netif *netif)
{
    netif_input_fn original = NULL;
    bool enabled = false;
    portENTER_CRITICAL(&repeater_lock);
    enabled = repeater.enabled;
    original = repeater.sta_input;
    portEXIT_CRITICAL(&repeater_lock);

    if (enabled && packet != NULL && repeater_forward_sta_to_ap(packet)) {
        return ERR_OK;
    }
    return original != NULL ? original(packet, netif) : ERR_IF;
}

static void repeater_install_hooks(void *argument)
{
    repeater_hook_request_t *request = (repeater_hook_request_t *)argument;
    LWIP_ASSERT_CORE_LOCKED();
    request->result = ERR_ARG;
    if (request->ap == NULL || request->sta == NULL ||
        request->ap->input == NULL || request->sta->input == NULL ||
        request->ap->linkoutput == NULL || request->sta->linkoutput == NULL) {
        return;
    }

    repeater.ap = request->ap;
    repeater.sta = request->sta;
    repeater.ap_input = request->ap->input;
    repeater.sta_input = request->sta->input;
    repeater.ap_linkoutput = request->ap->linkoutput;
    repeater.sta_linkoutput = request->sta->linkoutput;
    request->ap->input = repeater_ap_input;
    request->sta->input = repeater_sta_input;
    portENTER_CRITICAL(&repeater_lock);
    repeater.enabled = true;
    portEXIT_CRITICAL(&repeater_lock);
    request->result = ERR_OK;
}

static void repeater_restore_hooks(void *argument)
{
    repeater_hook_request_t *request = (repeater_hook_request_t *)argument;
    LWIP_ASSERT_CORE_LOCKED();
    request->result = ERR_OK;
    portENTER_CRITICAL(&repeater_lock);
    repeater.enabled = false;
    portEXIT_CRITICAL(&repeater_lock);

    if (repeater.ap != NULL) {
        if (repeater.ap->input == repeater_ap_input) {
            repeater.ap->input = repeater.ap_input;
        } else {
            request->result = ERR_USE;
        }
    }
    if (repeater.sta != NULL) {
        if (repeater.sta->input == repeater_sta_input) {
            repeater.sta->input = repeater.sta_input;
        } else {
            request->result = ERR_USE;
        }
    }
}

esp_err_t solar_os_wifi_repeater_enable(esp_netif_t *ap, esp_netif_t *sta)
{
    if (ap == NULL || sta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_wifi_repeater_is_enabled()) {
        return ESP_OK;
    }

    repeater_hook_request_t request = {
        .ap = esp_netif_get_netif_impl(ap),
        .sta = esp_netif_get_netif_impl(sta),
        .result = ERR_ARG,
    };
    if (request.ap == NULL || request.sta == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    repeater.ap_handle = ap;
    repeater.sta_handle = sta;
    repeater.saved_ap_ip_valid = esp_netif_get_ip_info(ap, &repeater.saved_ap_ip) == ESP_OK;
    solar_os_wifi_repeater_clear_clients();
    portENTER_CRITICAL(&repeater_lock);
    repeater.upstream_frames = 0U;
    repeater.downstream_frames = 0U;
    repeater.dropped_frames = 0U;
    portEXIT_CRITICAL(&repeater_lock);

    if (tcpip_callback_wait(repeater_install_hooks, &request) != ERR_OK ||
        request.result != ERR_OK) {
        return ESP_FAIL;
    }
    solar_os_wifi_repeater_on_ap_started();

    esp_netif_ip_info_t sta_ip = {0};
    if (esp_netif_get_ip_info(sta, &sta_ip) == ESP_OK && sta_ip.ip.addr != 0U) {
        solar_os_wifi_repeater_on_upstream_ip(&sta_ip);
    }
    SOLAR_OS_LOGI(TAG, "L2 forwarding enabled");
    return ESP_OK;
}

esp_err_t solar_os_wifi_repeater_disable(void)
{
    if (!solar_os_wifi_repeater_is_enabled()) {
        return ESP_OK;
    }

    repeater_hook_request_t request = {.result = ERR_OK};
    if (tcpip_callback_wait(repeater_restore_hooks, &request) != ERR_OK ||
        request.result != ERR_OK) {
        return ESP_FAIL;
    }

    if (repeater.ap_handle != NULL && repeater.saved_ap_ip_valid) {
        (void)esp_netif_dhcps_stop(repeater.ap_handle);
        (void)esp_netif_set_ip_info(repeater.ap_handle, &repeater.saved_ap_ip);
        const esp_err_t start_error = esp_netif_dhcps_start(repeater.ap_handle);
        if (start_error != ESP_OK &&
            start_error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            SOLAR_OS_LOGW(TAG, "AP DHCP restore failed: %s", esp_err_to_name(start_error));
        }
    }
    solar_os_wifi_repeater_clear_clients();
    SOLAR_OS_LOGI(TAG, "L2 forwarding disabled");
    return ESP_OK;
}

bool solar_os_wifi_repeater_is_enabled(void)
{
    bool enabled = false;
    portENTER_CRITICAL(&repeater_lock);
    enabled = repeater.enabled;
    portEXIT_CRITICAL(&repeater_lock);
    return enabled;
}

void solar_os_wifi_repeater_on_ap_started(void)
{
    if (!solar_os_wifi_repeater_is_enabled() || repeater.ap_handle == NULL) {
        return;
    }
    const esp_err_t error = esp_netif_dhcps_stop(repeater.ap_handle);
    if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        SOLAR_OS_LOGW(TAG, "AP DHCP stop failed: %s", esp_err_to_name(error));
    }
}

void solar_os_wifi_repeater_on_upstream_ip(const esp_netif_ip_info_t *ip_info)
{
    if (!solar_os_wifi_repeater_is_enabled() ||
        repeater.ap_handle == NULL || ip_info == NULL || ip_info->ip.addr == 0U) {
        return;
    }
    solar_os_wifi_repeater_on_ap_started();
    const esp_err_t error = esp_netif_set_ip_info(repeater.ap_handle, ip_info);
    if (error != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "AP management IP sync failed: %s", esp_err_to_name(error));
    }
}

void solar_os_wifi_repeater_clear_clients(void)
{
    portENTER_CRITICAL(&repeater_lock);
    memset(repeater.clients, 0, sizeof(repeater.clients));
    portEXIT_CRITICAL(&repeater_lock);
}

void solar_os_wifi_repeater_get_status(solar_os_wifi_repeater_status_t *status)
{
    if (status == NULL) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    *status = (solar_os_wifi_repeater_status_t){0};

    portENTER_CRITICAL(&repeater_lock);
    status->enabled = repeater.enabled;
    status->upstream_frames = repeater.upstream_frames;
    status->downstream_frames = repeater.downstream_frames;
    status->dropped_frames = repeater.dropped_frames;
    for (size_t i = 0; i < SOLAR_OS_WIFI_REPEATER_CLIENT_MAX; i++) {
        if (repeater.clients[i].valid && repeater.clients[i].expires_us > now) {
            status->learned_clients++;
        }
    }
    portEXIT_CRITICAL(&repeater_lock);
}

struct netif *solar_os_wifi_repeater_route(const ip4_addr_t *destination)
{
    if (destination == NULL || !solar_os_wifi_repeater_is_enabled()) {
        return NULL;
    }
    return repeater_client_lookup(destination->addr, NULL) ? repeater.ap : NULL;
}

struct netif *solar_os_wifi_repeater_upstream_route(void)
{
    return solar_os_wifi_repeater_is_enabled() ? repeater.sta : NULL;
}
