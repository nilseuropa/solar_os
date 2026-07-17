#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_radio.h"

#define RFM69_VERSION 0x24
#define RFM69_MAX_PACKET_LEN 64

typedef struct {
    int cs_pin;
    uint32_t speed_hz;
    solar_os_radio_config_t config;
    solar_os_radio_state_t state;
    uint8_t rx_buffer[RFM69_MAX_PACKET_LEN + 1];
    size_t rx_len;
    int16_t rx_rssi_dbm;
    bool rx_has_rssi;
    SemaphoreHandle_t mutex;
} rfm69_t;

esp_err_t rfm69_init(rfm69_t *dev, int cs_pin, uint32_t speed_hz);
esp_err_t rfm69_probe(rfm69_t *dev, uint8_t *version);
esp_err_t rfm69_configure(rfm69_t *dev, const solar_os_radio_config_t *config);
esp_err_t rfm69_set_state(rfm69_t *dev, solar_os_radio_state_t state);
esp_err_t rfm69_get_status(rfm69_t *dev, solar_os_radio_status_t *status);
esp_err_t rfm69_send(rfm69_t *dev, const solar_os_radio_packet_t *packet, uint32_t timeout_ms);
esp_err_t rfm69_send_stream(rfm69_t *dev,
                            const uint8_t *data,
                            size_t len,
                            uint32_t timeout_ms);
esp_err_t rfm69_receive(rfm69_t *dev, solar_os_radio_packet_t *packet, uint32_t timeout_ms);
