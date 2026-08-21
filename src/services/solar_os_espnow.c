#include "solar_os_espnow.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "solar_os_link.h"
#include "solar_os_log.h"
#include "solar_os_wifi.h"

#define ESPNOW_RX_QUEUE_DEPTH 4U
#define ESPNOW_TX_QUEUE_DEPTH 2U
#define ESPNOW_NVS_NAMESPACE "espnow"
#define ESPNOW_NVS_PEERS_KEY "peers"
#define ESPNOW_PEER_STORE_MAGIC UINT32_C(0x45534e50)
#define ESPNOW_PEER_STORE_VERSION 1U

static const char *TAG = "solar_os_espnow";
static const uint8_t broadcast_mac[SOLAR_OS_ESPNOW_MAC_LEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

typedef struct {
    uint32_t link_id;
    uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN];
    uint8_t reserved[2];
} espnow_peer_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    espnow_peer_record_t peers[SOLAR_OS_ESPNOW_PEER_MAX];
} espnow_peer_store_t;

typedef struct {
    uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN];
    esp_now_send_status_t status;
} espnow_send_event_t;

typedef struct {
    bool loaded;
    bool running;
    bool suspended;
    bool channel_auto;
    bool send_inflight;
    bool protocol_changed;
    uint8_t channel;
    uint8_t previous_protocol;
    solar_os_espnow_phy_t phy;
    char owner[SOLAR_OS_ESPNOW_OWNER_MAX];
    solar_os_espnow_peer_t peers[SOLAR_OS_ESPNOW_PEER_MAX];
    QueueHandle_t rx_queue;
    QueueHandle_t tx_queue;
    uint32_t tx_packets;
    uint32_t tx_errors;
    uint32_t rx_packets;
    uint32_t rx_dropped;
    uint32_t peer_conflicts;
    esp_err_t last_error;
} espnow_state_t;

static EXT_RAM_BSS_ATTR espnow_state_t espnow_state;
static portMUX_TYPE espnow_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool espnow_callbacks_active;
static QueueHandle_t espnow_callback_rx_queue;
static QueueHandle_t espnow_callback_tx_queue;

static bool espnow_phy_valid(solar_os_espnow_phy_t phy)
{
    return phy == SOLAR_OS_ESPNOW_PHY_NORMAL ||
        phy == SOLAR_OS_ESPNOW_PHY_LR500 ||
        phy == SOLAR_OS_ESPNOW_PHY_LR250;
}

const char *solar_os_espnow_phy_name(solar_os_espnow_phy_t phy)
{
    switch (phy) {
    case SOLAR_OS_ESPNOW_PHY_NORMAL:
        return "normal";
    case SOLAR_OS_ESPNOW_PHY_LR500:
        return "lr500";
    case SOLAR_OS_ESPNOW_PHY_LR250:
        return "lr250";
    default:
        return "unknown";
    }
}

bool solar_os_espnow_parse_phy(const char *text, solar_os_espnow_phy_t *phy)
{
    if (text == NULL || phy == NULL) {
        return false;
    }
    if (strcmp(text, "normal") == 0) {
        *phy = SOLAR_OS_ESPNOW_PHY_NORMAL;
        return true;
    }
    if (strcmp(text, "lr500") == 0) {
        *phy = SOLAR_OS_ESPNOW_PHY_LR500;
        return true;
    }
    if (strcmp(text, "lr250") == 0) {
        *phy = SOLAR_OS_ESPNOW_PHY_LR250;
        return true;
    }
    return false;
}

static esp_err_t espnow_prepare_protocol(solar_os_espnow_phy_t phy,
                                         uint8_t *previous_protocol,
                                         bool *changed)
{
    if (previous_protocol == NULL || changed == NULL ||
        !espnow_phy_valid(phy)) {
        return ESP_ERR_INVALID_ARG;
    }
    *changed = false;
    *previous_protocol = 0U;
    if (phy == SOLAR_OS_ESPNOW_PHY_NORMAL) {
        return ESP_OK;
    }
    esp_err_t ret = esp_wifi_get_protocol(WIFI_IF_STA, previous_protocol);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t enabled_protocol = *previous_protocol | WIFI_PROTOCOL_LR;
    if (enabled_protocol == *previous_protocol) {
        return ESP_OK;
    }
    ret = esp_wifi_set_protocol(WIFI_IF_STA, enabled_protocol);
    if (ret == ESP_OK) {
        *changed = true;
    }
    return ret;
}

static esp_err_t espnow_reapply_protocol(void)
{
    portENTER_CRITICAL(&espnow_lock);
    const solar_os_espnow_phy_t phy = espnow_state.phy;
    const uint8_t previous_protocol = espnow_state.previous_protocol;
    portEXIT_CRITICAL(&espnow_lock);
    if (phy == SOLAR_OS_ESPNOW_PHY_NORMAL) {
        return ESP_OK;
    }
    return esp_wifi_set_protocol(WIFI_IF_STA,
                                 previous_protocol | WIFI_PROTOCOL_LR);
}

static esp_err_t espnow_restore_protocol(uint8_t previous_protocol,
                                         bool changed)
{
    return changed ? esp_wifi_set_protocol(WIFI_IF_STA, previous_protocol) :
        ESP_OK;
}

static esp_err_t espnow_driver_configure_peer_rate(
    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    portENTER_CRITICAL(&espnow_lock);
    const solar_os_espnow_phy_t phy = espnow_state.phy;
    portEXIT_CRITICAL(&espnow_lock);
    if (phy == SOLAR_OS_ESPNOW_PHY_NORMAL) {
        return ESP_OK;
    }
    esp_now_rate_config_t config = {
        .phymode = WIFI_PHY_MODE_LR,
        .rate = phy == SOLAR_OS_ESPNOW_PHY_LR250 ?
            WIFI_PHY_RATE_LORA_250K : WIFI_PHY_RATE_LORA_500K,
        .ersu = false,
        .dcm = false,
    };
    return esp_now_set_peer_rate_config(mac, &config);
}

static bool espnow_link_id_valid(uint32_t link_id)
{
    return link_id != 0U && link_id != SOLAR_OS_LINK_BROADCAST;
}

static bool espnow_mac_valid(const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    if (mac == NULL || (mac[0] & 0x01U) != 0U) {
        return false;
    }
    uint8_t combined = 0U;
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_MAC_LEN; i++) {
        combined |= mac[i];
    }
    return combined != 0U;
}

static int espnow_peer_index_by_id_locked(uint32_t link_id)
{
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (espnow_state.peers[i].link_id == link_id) {
            return (int)i;
        }
    }
    return -1;
}

static int espnow_peer_index_by_mac_locked(
    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (espnow_state.peers[i].link_id != 0U &&
            memcmp(espnow_state.peers[i].mac, mac, SOLAR_OS_ESPNOW_MAC_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool espnow_peer_table_has_mac(
    const solar_os_espnow_peer_t peers[SOLAR_OS_ESPNOW_PEER_MAX],
    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (peers[i].link_id != 0U &&
            memcmp(peers[i].mac, mac, SOLAR_OS_ESPNOW_MAC_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t espnow_driver_add_peer(
    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    esp_now_peer_info_t driver_peer = {
        .channel = 0U,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(driver_peer.peer_addr, mac, SOLAR_OS_ESPNOW_MAC_LEN);
    esp_err_t ret = ESP_OK;
    if (esp_now_is_peer_exist(mac)) {
        ret = esp_now_mod_peer(&driver_peer);
    } else {
        ret = esp_now_add_peer(&driver_peer);
    }
    return ret == ESP_OK ? espnow_driver_configure_peer_rate(mac) : ret;
}

static esp_err_t espnow_driver_reconcile_peers(
    const solar_os_espnow_peer_t before[SOLAR_OS_ESPNOW_PEER_MAX],
    const solar_os_espnow_peer_t after[SOLAR_OS_ESPNOW_PEER_MAX])
{
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (before[i].link_id == 0U ||
            espnow_peer_table_has_mac(after, before[i].mac) ||
            !esp_now_is_peer_exist(before[i].mac)) {
            continue;
        }
        const esp_err_t ret = esp_now_del_peer(before[i].mac);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (after[i].link_id == 0U ||
            espnow_peer_table_has_mac(before, after[i].mac)) {
            continue;
        }
        const esp_err_t ret = espnow_driver_add_peer(after[i].mac);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t espnow_save_configured_peers(void)
{
    espnow_peer_store_t store = {
        .magic = ESPNOW_PEER_STORE_MAGIC,
        .version = ESPNOW_PEER_STORE_VERSION,
    };

    portENTER_CRITICAL(&espnow_lock);
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        const solar_os_espnow_peer_t *peer = &espnow_state.peers[i];
        if (peer->link_id == 0U || !peer->configured) {
            continue;
        }
        espnow_peer_record_t *record = &store.peers[store.count++];
        record->link_id = peer->link_id;
        memcpy(record->mac, peer->mac, sizeof(record->mac));
    }
    portEXIT_CRITICAL(&espnow_lock);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ESPNOW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(nvs, ESPNOW_NVS_PEERS_KEY, &store, sizeof(store));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t espnow_load_configured_peers(void)
{
    portENTER_CRITICAL(&espnow_lock);
    const bool loaded = espnow_state.loaded;
    portEXIT_CRITICAL(&espnow_lock);
    if (loaded) {
        return ESP_OK;
    }

    espnow_peer_store_t store = {0};
    size_t length = sizeof(store);
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ESPNOW_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = nvs_get_blob(nvs, ESPNOW_NVS_PEERS_KEY, &store, &length);
        nvs_close(nvs);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }

    if (length != sizeof(store) ||
        (store.magic != 0U &&
         (store.magic != ESPNOW_PEER_STORE_MAGIC ||
          store.version != ESPNOW_PEER_STORE_VERSION ||
          store.count > SOLAR_OS_ESPNOW_PEER_MAX))) {
        return ESP_ERR_INVALID_VERSION;
    }

    portENTER_CRITICAL(&espnow_lock);
    memset(espnow_state.peers, 0, sizeof(espnow_state.peers));
    for (size_t i = 0U; i < store.count; i++) {
        if (!espnow_link_id_valid(store.peers[i].link_id) ||
            !espnow_mac_valid(store.peers[i].mac)) {
            continue;
        }
        espnow_state.peers[i].link_id = store.peers[i].link_id;
        memcpy(espnow_state.peers[i].mac,
               store.peers[i].mac,
               SOLAR_OS_ESPNOW_MAC_LEN);
        espnow_state.peers[i].configured = true;
    }
    espnow_state.loaded = true;
    portEXIT_CRITICAL(&espnow_lock);
    return ESP_OK;
}

static void espnow_receive_callback(const esp_now_recv_info_t *info,
                                    const uint8_t *data,
                                    int data_len)
{
    if (!espnow_callbacks_active || espnow_callback_rx_queue == NULL ||
        info == NULL || info->src_addr == NULL || data == NULL ||
        data_len <= 0 || data_len > (int)SOLAR_OS_ESPNOW_FRAME_MTU) {
        return;
    }

    solar_os_espnow_packet_t packet = {
        .rssi = info->rx_ctrl != NULL ? info->rx_ctrl->rssi : 0,
        .len = (size_t)data_len,
    };
    memcpy(packet.source_mac, info->src_addr, sizeof(packet.source_mac));
    memcpy(packet.data, data, packet.len);
    const bool queued = xQueueSend(espnow_callback_rx_queue, &packet, 0) == pdTRUE;

    portENTER_CRITICAL(&espnow_lock);
    if (queued) {
        espnow_state.rx_packets++;
    } else {
        espnow_state.rx_dropped++;
        espnow_state.last_error = ESP_ERR_NO_MEM;
    }
    portEXIT_CRITICAL(&espnow_lock);
}

static void espnow_send_callback(const esp_now_send_info_t *info,
                                 esp_now_send_status_t status)
{
    if (!espnow_callbacks_active || espnow_callback_tx_queue == NULL ||
        info == NULL || info->des_addr == NULL) {
        return;
    }
    espnow_send_event_t event = {.status = status};
    memcpy(event.mac, info->des_addr, sizeof(event.mac));
    if (xQueueSend(espnow_callback_tx_queue, &event, 0) != pdTRUE) {
        portENTER_CRITICAL(&espnow_lock);
        espnow_state.tx_errors++;
        espnow_state.last_error = ESP_ERR_NO_MEM;
        espnow_state.send_inflight = false;
        portEXIT_CRITICAL(&espnow_lock);
    }
}

static esp_err_t espnow_driver_start(QueueHandle_t rx_queue,
                                     QueueHandle_t tx_queue)
{
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        return ret;
    }

    espnow_callback_rx_queue = rx_queue;
    espnow_callback_tx_queue = tx_queue;
    ret = esp_now_register_recv_cb(espnow_receive_callback);
    if (ret == ESP_OK) {
        ret = esp_now_register_send_cb(espnow_send_callback);
    }
    if (ret == ESP_OK) {
        ret = espnow_driver_add_peer(broadcast_mac);
    }
    for (size_t i = 0U; ret == ESP_OK && i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        portENTER_CRITICAL(&espnow_lock);
        const solar_os_espnow_peer_t peer = espnow_state.peers[i];
        portEXIT_CRITICAL(&espnow_lock);
        if (peer.link_id != 0U) {
            ret = espnow_driver_add_peer(peer.mac);
        }
    }
    if (ret == ESP_OK) {
        espnow_callbacks_active = true;
        return ESP_OK;
    }

    espnow_callbacks_active = false;
    espnow_callback_rx_queue = NULL;
    espnow_callback_tx_queue = NULL;
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_unregister_send_cb();
    (void)esp_now_deinit();
    return ret;
}

static esp_err_t espnow_driver_stop(void)
{
    espnow_callbacks_active = false;
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_unregister_send_cb();
    const esp_err_t ret = esp_now_deinit();
    espnow_callback_rx_queue = NULL;
    espnow_callback_tx_queue = NULL;
    return ret;
}

esp_err_t solar_os_espnow_start(const char *owner,
                                uint8_t requested_channel,
                                solar_os_espnow_phy_t phy)
{
    if (owner == NULL || owner[0] == '\0' ||
        strnlen(owner, SOLAR_OS_ESPNOW_OWNER_MAX) >= SOLAR_OS_ESPNOW_OWNER_MAX ||
        requested_channel > 13U || !espnow_phy_valid(phy)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = espnow_load_configured_peers();
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&espnow_lock);
    const bool already_running = espnow_state.running;
    portEXIT_CRITICAL(&espnow_lock);
    if (already_running) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t actual_channel = 0U;
    ret = solar_os_wifi_connectionless_acquire(owner,
                                               requested_channel,
                                               &actual_channel);
    if (ret != ESP_OK) {
        return ret;
    }

    QueueHandle_t rx_queue = xQueueCreate(ESPNOW_RX_QUEUE_DEPTH,
                                          sizeof(solar_os_espnow_packet_t));
    QueueHandle_t tx_queue = xQueueCreate(ESPNOW_TX_QUEUE_DEPTH,
                                          sizeof(espnow_send_event_t));
    if (rx_queue == NULL || tx_queue == NULL) {
        if (rx_queue != NULL) {
            vQueueDelete(rx_queue);
        }
        if (tx_queue != NULL) {
            vQueueDelete(tx_queue);
        }
        (void)solar_os_wifi_connectionless_release(owner);
        return ESP_ERR_NO_MEM;
    }

    uint8_t previous_protocol = 0U;
    bool protocol_changed = false;
    ret = espnow_prepare_protocol(phy,
                                  &previous_protocol,
                                  &protocol_changed);
    if (ret != ESP_OK) {
        vQueueDelete(rx_queue);
        vQueueDelete(tx_queue);
        (void)solar_os_wifi_connectionless_release(owner);
        return ret;
    }
    portENTER_CRITICAL(&espnow_lock);
    espnow_state.phy = phy;
    espnow_state.previous_protocol = previous_protocol;
    espnow_state.protocol_changed = protocol_changed;
    portEXIT_CRITICAL(&espnow_lock);

    ret = espnow_driver_start(rx_queue, tx_queue);
    if (ret != ESP_OK) {
        (void)espnow_restore_protocol(previous_protocol, protocol_changed);
        vQueueDelete(rx_queue);
        vQueueDelete(tx_queue);
        (void)solar_os_wifi_connectionless_release(owner);
        portENTER_CRITICAL(&espnow_lock);
        espnow_state.phy = SOLAR_OS_ESPNOW_PHY_NORMAL;
        espnow_state.previous_protocol = 0U;
        espnow_state.protocol_changed = false;
        portEXIT_CRITICAL(&espnow_lock);
        return ret;
    }

    portENTER_CRITICAL(&espnow_lock);
    espnow_state.running = true;
    espnow_state.suspended = false;
    espnow_state.channel_auto = requested_channel == 0U;
    espnow_state.channel = actual_channel;
    espnow_state.send_inflight = false;
    espnow_state.rx_queue = rx_queue;
    espnow_state.tx_queue = tx_queue;
    espnow_state.tx_packets = 0U;
    espnow_state.tx_errors = 0U;
    espnow_state.rx_packets = 0U;
    espnow_state.rx_dropped = 0U;
    espnow_state.peer_conflicts = 0U;
    espnow_state.last_error = ESP_OK;
    strlcpy(espnow_state.owner, owner, sizeof(espnow_state.owner));
    portEXIT_CRITICAL(&espnow_lock);

    SOLAR_OS_LOGI(TAG,
                  "started owner=%s channel=%u mode=%s phy=%s mtu=%u",
                  owner,
                  (unsigned)actual_channel,
                  requested_channel == 0U ? "auto" : "fixed",
                  solar_os_espnow_phy_name(phy),
                  (unsigned)SOLAR_OS_ESPNOW_FRAME_MTU);
    return ESP_OK;
}

esp_err_t solar_os_espnow_stop(const char *owner)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&espnow_lock);
    if (!espnow_state.running) {
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_OK;
    }
    if (strcmp(owner, espnow_state.owner) != 0) {
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_ERR_INVALID_STATE;
    }
    QueueHandle_t rx_queue = espnow_state.rx_queue;
    QueueHandle_t tx_queue = espnow_state.tx_queue;
    const bool suspended = espnow_state.suspended;
    const uint8_t previous_protocol = espnow_state.previous_protocol;
    const bool protocol_changed = espnow_state.protocol_changed;
    espnow_state.running = false;
    espnow_state.suspended = false;
    espnow_state.send_inflight = false;
    espnow_state.rx_queue = NULL;
    espnow_state.tx_queue = NULL;
    espnow_state.phy = SOLAR_OS_ESPNOW_PHY_NORMAL;
    espnow_state.previous_protocol = 0U;
    espnow_state.protocol_changed = false;
    espnow_state.owner[0] = '\0';
    portEXIT_CRITICAL(&espnow_lock);

    const esp_err_t deinit_ret = suspended ? ESP_OK : espnow_driver_stop();
    const esp_err_t protocol_ret =
        espnow_restore_protocol(previous_protocol, protocol_changed);
    if (rx_queue != NULL) {
        vQueueDelete(rx_queue);
    }
    if (tx_queue != NULL) {
        vQueueDelete(tx_queue);
    }
    const esp_err_t release_ret = solar_os_wifi_connectionless_release(owner);
    SOLAR_OS_LOGI(TAG, "stopped owner=%s", owner);
    return deinit_ret != ESP_OK ? deinit_ret :
        (protocol_ret != ESP_OK ? protocol_ret : release_ret);
}

esp_err_t solar_os_espnow_prepare_sleep(void)
{
    portENTER_CRITICAL(&espnow_lock);
    if (!espnow_state.running || espnow_state.suspended) {
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_OK;
    }
    const bool send_inflight = espnow_state.send_inflight;
    QueueHandle_t tx_queue = espnow_state.tx_queue;
    espnow_state.suspended = true;
    espnow_state.send_inflight = false;
    portEXIT_CRITICAL(&espnow_lock);

    const esp_err_t ret = espnow_driver_stop();
    if (send_inflight && tx_queue != NULL) {
        const espnow_send_event_t failed = {.status = ESP_NOW_SEND_FAIL};
        (void)xQueueSend(tx_queue, &failed, 0);
    }
    portENTER_CRITICAL(&espnow_lock);
    espnow_state.last_error = ret;
    portEXIT_CRITICAL(&espnow_lock);
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "sleep: ESP-NOW suspended");
    }
    return ret;
}

esp_err_t solar_os_espnow_resume(void)
{
    portENTER_CRITICAL(&espnow_lock);
    if (!espnow_state.running || !espnow_state.suspended) {
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_OK;
    }
    QueueHandle_t rx_queue = espnow_state.rx_queue;
    QueueHandle_t tx_queue = espnow_state.tx_queue;
    portEXIT_CRITICAL(&espnow_lock);

    esp_err_t ret = espnow_reapply_protocol();
    if (ret == ESP_OK) {
        ret = espnow_driver_start(rx_queue, tx_queue);
    }
    portENTER_CRITICAL(&espnow_lock);
    if (ret == ESP_OK) {
        espnow_state.suspended = false;
    }
    espnow_state.last_error = ret;
    portEXIT_CRITICAL(&espnow_lock);
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "wake: ESP-NOW resumed");
    }
    return ret;
}

void solar_os_espnow_get_status(solar_os_espnow_status_t *status)
{
    if (status == NULL) {
        return;
    }
    (void)espnow_load_configured_peers();
    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);

    portENTER_CRITICAL(&espnow_lock);
    *status = (solar_os_espnow_status_t){
        .running = espnow_state.running,
        .channel_auto = espnow_state.channel_auto,
        .send_inflight = espnow_state.send_inflight,
        .phy = espnow_state.phy,
        .channel = espnow_state.running && wifi_status.connectionless_active ?
            wifi_status.connectionless_channel : espnow_state.channel,
        .tx_packets = espnow_state.tx_packets,
        .tx_errors = espnow_state.tx_errors,
        .rx_packets = espnow_state.rx_packets,
        .rx_dropped = espnow_state.rx_dropped,
        .peer_conflicts = espnow_state.peer_conflicts,
        .last_error = espnow_state.last_error,
    };
    strlcpy(status->owner, espnow_state.owner, sizeof(status->owner));
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (espnow_state.peers[i].link_id != 0U) {
            status->peer_count++;
            if (espnow_state.peers[i].configured) {
                status->configured_peer_count++;
            }
        }
    }
    portEXIT_CRITICAL(&espnow_lock);
}

size_t solar_os_espnow_peer_count(void)
{
    (void)espnow_load_configured_peers();
    size_t count = 0U;
    portENTER_CRITICAL(&espnow_lock);
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        count += espnow_state.peers[i].link_id != 0U ? 1U : 0U;
    }
    portEXIT_CRITICAL(&espnow_lock);
    return count;
}

bool solar_os_espnow_peer_get(size_t index, solar_os_espnow_peer_t *peer)
{
    if (peer == NULL || espnow_load_configured_peers() != ESP_OK) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&espnow_lock);
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
        if (espnow_state.peers[i].link_id == 0U) {
            continue;
        }
        if (index == 0U) {
            *peer = espnow_state.peers[i];
            found = true;
            break;
        }
        index--;
    }
    portEXIT_CRITICAL(&espnow_lock);
    return found;
}

esp_err_t solar_os_espnow_peer_set(
    uint32_t link_id,
    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    if (!espnow_link_id_valid(link_id) || !espnow_mac_valid(mac)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = espnow_load_configured_peers();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_espnow_peer_t before[SOLAR_OS_ESPNOW_PEER_MAX];
    int selected = -1;
    portENTER_CRITICAL(&espnow_lock);
    memcpy(before, espnow_state.peers, sizeof(before));
    const int by_id = espnow_peer_index_by_id_locked(link_id);
    const int by_mac = espnow_peer_index_by_mac_locked(mac);
    if (by_mac >= 0 && by_mac != by_id && espnow_state.peers[by_mac].configured) {
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (by_mac >= 0 && by_mac != by_id) {
        memset(&espnow_state.peers[by_mac], 0, sizeof(espnow_state.peers[by_mac]));
    }
    selected = by_id;
    if (selected < 0) {
        for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
            if (espnow_state.peers[i].link_id == 0U) {
                selected = (int)i;
                break;
            }
        }
    }
    if (selected >= 0) {
        espnow_state.peers[selected] = (solar_os_espnow_peer_t){
            .link_id = link_id,
            .configured = true,
        };
        memcpy(espnow_state.peers[selected].mac, mac, SOLAR_OS_ESPNOW_MAC_LEN);
    }
    const bool running = espnow_state.running;
    portEXIT_CRITICAL(&espnow_lock);
    if (selected < 0) {
        return ESP_ERR_NO_MEM;
    }

    if (running) {
        ret = espnow_driver_reconcile_peers(before, espnow_state.peers);
        if (ret != ESP_OK) {
            solar_os_espnow_peer_t failed[SOLAR_OS_ESPNOW_PEER_MAX];
            portENTER_CRITICAL(&espnow_lock);
            memcpy(failed, espnow_state.peers, sizeof(failed));
            memcpy(espnow_state.peers, before, sizeof(before));
            espnow_state.last_error = ret;
            portEXIT_CRITICAL(&espnow_lock);
            (void)espnow_driver_reconcile_peers(failed, before);
            return ret;
        }
    }
    ret = espnow_save_configured_peers();
    if (ret != ESP_OK) {
        solar_os_espnow_peer_t failed[SOLAR_OS_ESPNOW_PEER_MAX];
        portENTER_CRITICAL(&espnow_lock);
        memcpy(failed, espnow_state.peers, sizeof(failed));
        memcpy(espnow_state.peers, before, sizeof(before));
        espnow_state.last_error = ret;
        portEXIT_CRITICAL(&espnow_lock);
        if (running) {
            (void)espnow_driver_reconcile_peers(failed, before);
        }
    }
    return ret;
}

esp_err_t solar_os_espnow_peer_remove(uint32_t link_id)
{
    if (!espnow_link_id_valid(link_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = espnow_load_configured_peers();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_espnow_peer_t before[SOLAR_OS_ESPNOW_PEER_MAX];
    bool configured = false;
    bool running = false;
    portENTER_CRITICAL(&espnow_lock);
    memcpy(before, espnow_state.peers, sizeof(before));
    const int index = espnow_peer_index_by_id_locked(link_id);
    if (index >= 0) {
        configured = espnow_state.peers[index].configured;
        memset(&espnow_state.peers[index], 0, sizeof(espnow_state.peers[index]));
        running = espnow_state.running;
    }
    portEXIT_CRITICAL(&espnow_lock);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (running) {
        ret = espnow_driver_reconcile_peers(before, espnow_state.peers);
        if (ret != ESP_OK) {
            solar_os_espnow_peer_t failed[SOLAR_OS_ESPNOW_PEER_MAX];
            portENTER_CRITICAL(&espnow_lock);
            memcpy(failed, espnow_state.peers, sizeof(failed));
            memcpy(espnow_state.peers, before, sizeof(before));
            espnow_state.last_error = ret;
            portEXIT_CRITICAL(&espnow_lock);
            (void)espnow_driver_reconcile_peers(failed, before);
            return ret;
        }
    }
    ret = configured ? espnow_save_configured_peers() : ESP_OK;
    if (ret != ESP_OK) {
        solar_os_espnow_peer_t failed[SOLAR_OS_ESPNOW_PEER_MAX];
        portENTER_CRITICAL(&espnow_lock);
        memcpy(failed, espnow_state.peers, sizeof(failed));
        memcpy(espnow_state.peers, before, sizeof(before));
        espnow_state.last_error = ret;
        portEXIT_CRITICAL(&espnow_lock);
        if (running) {
            (void)espnow_driver_reconcile_peers(failed, before);
        }
    }
    return ret;
}

esp_err_t solar_os_espnow_peer_learn(
    uint32_t link_id,
    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN],
    int8_t rssi,
    uint32_t now_ms)
{
    if (!espnow_link_id_valid(link_id) || !espnow_mac_valid(mac)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_espnow_peer_t before[SOLAR_OS_ESPNOW_PEER_MAX];
    int selected = -1;
    portENTER_CRITICAL(&espnow_lock);
    memcpy(before, espnow_state.peers, sizeof(before));
    const int by_id = espnow_peer_index_by_id_locked(link_id);
    const int by_mac = espnow_peer_index_by_mac_locked(mac);
    if ((by_id >= 0 &&
         memcmp(espnow_state.peers[by_id].mac, mac, SOLAR_OS_ESPNOW_MAC_LEN) != 0) ||
        (by_mac >= 0 && espnow_state.peers[by_mac].link_id != link_id)) {
        espnow_state.peer_conflicts++;
        espnow_state.last_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_ERR_INVALID_STATE;
    }
    selected = by_id >= 0 ? by_id : by_mac;
    if (selected < 0) {
        uint32_t oldest = UINT32_MAX;
        int oldest_learned = -1;
        for (size_t i = 0U; i < SOLAR_OS_ESPNOW_PEER_MAX; i++) {
            if (espnow_state.peers[i].link_id == 0U) {
                selected = (int)i;
                break;
            }
            if (!espnow_state.peers[i].configured &&
                espnow_state.peers[i].last_seen_ms <= oldest) {
                oldest = espnow_state.peers[i].last_seen_ms;
                oldest_learned = (int)i;
            }
        }
        if (selected < 0) {
            selected = oldest_learned;
        }
    }
    if (selected >= 0) {
        const bool configured = espnow_state.peers[selected].configured;
        espnow_state.peers[selected] = (solar_os_espnow_peer_t){
            .link_id = link_id,
            .configured = configured,
            .rssi = rssi,
            .last_seen_ms = now_ms,
        };
        memcpy(espnow_state.peers[selected].mac, mac, SOLAR_OS_ESPNOW_MAC_LEN);
    }
    const bool running = espnow_state.running;
    portEXIT_CRITICAL(&espnow_lock);
    if (selected < 0) {
        return ESP_ERR_NO_MEM;
    }
    if (!running) {
        return ESP_OK;
    }
    const esp_err_t ret = espnow_driver_reconcile_peers(before,
                                                        espnow_state.peers);
    if (ret != ESP_OK) {
        solar_os_espnow_peer_t failed[SOLAR_OS_ESPNOW_PEER_MAX];
        portENTER_CRITICAL(&espnow_lock);
        memcpy(failed, espnow_state.peers, sizeof(failed));
        memcpy(espnow_state.peers, before, sizeof(before));
        espnow_state.last_error = ret;
        portEXIT_CRITICAL(&espnow_lock);
        (void)espnow_driver_reconcile_peers(failed, before);
    }
    return ret;
}

esp_err_t solar_os_espnow_send(uint32_t link_destination,
                              const uint8_t *data,
                              size_t len)
{
    if (data == NULL || len == 0U || len > SOLAR_OS_ESPNOW_FRAME_MTU ||
        link_destination == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t destination[SOLAR_OS_ESPNOW_MAC_LEN];
    if (link_destination == SOLAR_OS_LINK_BROADCAST) {
        memcpy(destination, broadcast_mac, sizeof(destination));
    } else {
        portENTER_CRITICAL(&espnow_lock);
        const int index = espnow_peer_index_by_id_locked(link_destination);
        if (index >= 0) {
            memcpy(destination, espnow_state.peers[index].mac, sizeof(destination));
        }
        portEXIT_CRITICAL(&espnow_lock);
        if (index < 0) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    portENTER_CRITICAL(&espnow_lock);
    if (!espnow_state.running || espnow_state.suspended ||
        espnow_state.send_inflight) {
        portEXIT_CRITICAL(&espnow_lock);
        return ESP_ERR_INVALID_STATE;
    }
    espnow_state.send_inflight = true;
    portEXIT_CRITICAL(&espnow_lock);

    const esp_err_t ret = esp_now_send(destination, data, len);
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&espnow_lock);
        espnow_state.send_inflight = false;
        espnow_state.tx_errors++;
        espnow_state.last_error = ret;
        portEXIT_CRITICAL(&espnow_lock);
    }
    return ret;
}

esp_err_t solar_os_espnow_take_send_result(esp_err_t *result,
                                           uint32_t timeout_ms)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&espnow_lock);
    QueueHandle_t queue = espnow_state.tx_queue;
    const bool running = espnow_state.running;
    portEXIT_CRITICAL(&espnow_lock);
    if (!running || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    espnow_send_event_t event;
    if (xQueueReceive(queue, &event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *result = event.status == ESP_NOW_SEND_SUCCESS ? ESP_OK : ESP_FAIL;
    portENTER_CRITICAL(&espnow_lock);
    espnow_state.send_inflight = false;
    espnow_state.last_error = *result;
    if (*result == ESP_OK) {
        espnow_state.tx_packets++;
    } else {
        espnow_state.tx_errors++;
    }
    portEXIT_CRITICAL(&espnow_lock);
    return ESP_OK;
}

esp_err_t solar_os_espnow_receive(solar_os_espnow_packet_t *packet,
                                 uint32_t timeout_ms)
{
    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&espnow_lock);
    QueueHandle_t queue = espnow_state.rx_queue;
    const bool running = espnow_state.running;
    portEXIT_CRITICAL(&espnow_lock);
    if (!running || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueReceive(queue, packet, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

bool solar_os_espnow_parse_mac(const char *text,
                              uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN])
{
    if (text == NULL || mac == NULL) {
        return false;
    }
    unsigned values[SOLAR_OS_ESPNOW_MAC_LEN];
    char trailing = '\0';
    if (sscanf(text,
               "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &values[0],
               &values[1],
               &values[2],
               &values[3],
               &values[4],
               &values[5],
               &trailing) != (int)SOLAR_OS_ESPNOW_MAC_LEN) {
        return false;
    }
    for (size_t i = 0U; i < SOLAR_OS_ESPNOW_MAC_LEN; i++) {
        mac[i] = (uint8_t)values[i];
    }
    return espnow_mac_valid(mac);
}

void solar_os_espnow_format_mac(const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN],
                               char *text,
                               size_t text_len)
{
    if (text == NULL || text_len == 0U) {
        return;
    }
    if (mac == NULL) {
        text[0] = '\0';
        return;
    }
    snprintf(text,
             text_len,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
