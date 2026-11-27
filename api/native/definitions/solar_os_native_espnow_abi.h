#pragma once

#include "solar_os_espnow_types_v1.h"
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_ESPNOW_SERVICE "network.espnow"
#define SOLAR_OS_NATIVE_ESPNOW_ABI 1U

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;

    /* Open the shared ESP-NOW radio. Channel zero follows Wi-Fi. The radio
     * has a single owner across built-in and native consumers. */
    int (*open)(const char *owner, uint8_t channel, solar_os_espnow_phy_t phy,
                solar_os_native_handle_t *out);
    /* A pending read or send holds a lease. Finish it before closing. */
    int (*close)(solar_os_native_handle_t radio);
    int (*status)(solar_os_espnow_status_t *out);

    /* Configured peers are shared with the shell and survive reboot where
     * settings storage is available. Learning updates the in-memory table. */
    size_t (*peer_count)(void);
    bool (*peer_at)(size_t index, solar_os_espnow_peer_t *out);
    int (*peer_set)(uint32_t link_id, const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN]);
    int (*peer_remove)(uint32_t link_id);
    int (*peer_learn)(solar_os_native_handle_t radio, uint32_t link_id,
                      const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN], int8_t rssi, uint32_t now_ms);

    /* At most one send is pending. Poll the result before sending again.
     * A timeout of zero polls without blocking. */
    int (*send)(solar_os_native_handle_t radio, uint32_t destination, const uint8_t *data, size_t len);
    int (*take_send_result)(solar_os_native_handle_t radio, int *out, uint32_t timeout_ms);
    int (*receive)(solar_os_native_handle_t radio, solar_os_espnow_packet_t *out, uint32_t timeout_ms);

    bool (*parse_mac)(const char *text, uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN]);
    void (*format_mac)(const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN], char *text, size_t capacity);
    bool (*parse_phy)(const char *text, solar_os_espnow_phy_t *out);
    const char *(*phy_name)(solar_os_espnow_phy_t phy);
} solar_os_native_espnow_api_v1_t;
