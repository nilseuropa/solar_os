#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t id;
    uint8_t length;
    uint8_t data[8];
    bool extended, remote;
} solar_os_can_frame_t;
typedef enum {
    SOLAR_OS_CAN_STOPPED, SOLAR_OS_CAN_RUNNING, SOLAR_OS_CAN_BUS_OFF, SOLAR_OS_CAN_RECOVERING,
} solar_os_can_state_t;
typedef struct {
    solar_os_can_state_t state;
    uint32_t pending_tx, pending_rx, tx_errors, rx_errors, failed_tx, missed_rx, bus_errors;
} solar_os_can_status_t;
typedef struct { void *handle; bool listen_only; } solar_os_can_twai_t;

esp_err_t solar_os_can_twai_open(solar_os_can_twai_t *, int tx, int rx, unsigned bitrate_kbit, bool listen_only);
esp_err_t solar_os_can_twai_close(solar_os_can_twai_t *);
esp_err_t solar_os_can_twai_send(solar_os_can_twai_t *, const solar_os_can_frame_t *, uint32_t timeout_ms);
esp_err_t solar_os_can_twai_receive(solar_os_can_twai_t *, solar_os_can_frame_t *, uint32_t timeout_ms);
esp_err_t solar_os_can_twai_status(solar_os_can_twai_t *, solar_os_can_status_t *);
esp_err_t solar_os_can_twai_recover(solar_os_can_twai_t *);
esp_err_t solar_os_can_twai_start(solar_os_can_twai_t *);
